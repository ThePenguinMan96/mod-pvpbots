/*
 * mod-pvpbots — PvpBotGear.cpp
 *
 * Gear selection system for PvP bots.
 *
 *   BuildPvpItemCache        — startup: indexes resilience items + trinket caches
 *   BuildHeirloomCache       — startup: indexes scaling heirloom items
 *   GetHeirloomStatWeight    — per-stat weight for heirloom scoring
 *   GetHeirloomDpsWeight     — weapon DPS weight for heirloom scoring
 *   GetHeirloomDpsWeight     — 0.0f for caster weapons on physical specs / 2H on
 *                              Enhancement+Hunter, so hasPrimaryStats gate fires
 *   ScoreHeirloom            — scores a heirloom using SSD/SSV DBC data
 *   CanEquipItemTemp         — safe equip-check that avoids stale update-queue pointers
 *   GetWeaponSpeedMultiplier — governance boost for spec-ideal weapon speeds
 *   InitPvpEquipment         — main gear loop called from InitPvpBot
 */

#include "PvpBotMgr.h"

#include "AccountMgr.h"
#include "CharacterCache.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "Item.h"
#include "Log.h"
#include "SpellMgr.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Pet.h"
#include "Player.h"
#include "World.h"
#include "WorldSession.h"

// mod-playerbots
#include "AiFactory.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotFactory.h"
#include "PlayerbotMgr.h"
#include "Playerbots.h"             // GET_PLAYERBOT_AI
#include "RandomItemMgr.h"
#include "RandomPlayerbotFactory.h"
#include "StatsWeightCalculator.h"

#include <algorithm>
#include <set>
#include <sstream>
#include <thread>
#include <chrono>

// ============================================================
// BuildPvpItemCache
//
// Queries item_template for every item that has resilience rating
// (stat_type = 35) as one of its ten stat slots.  Stored in _pvpItemCache
// for future use and as reference data.  Also builds _ccBreakTrinketCache
// (used by InitPvpEquipment for TRINKET1) and _allTrinketCache.
//
// Called once during Initialize().
// ============================================================
void PvpBotMgr::BuildPvpItemCache()
{
    _pvpItemCache.clear();

    // Fetch every equippable item that has any resilience (stat_type=35).
    // Scoring is handled at gear-time by StatsWeightCalculator — we only need
    // the identity and equip-eligibility fields here.
    QueryResult result = WorldDatabase.Query(
        "SELECT entry, InventoryType, RequiredLevel, ItemLevel, AllowableClass, Quality "
        "FROM item_template "
        "WHERE Quality >= 2 "
        "  AND RequiredLevel > 0 "
        "  AND InventoryType BETWEEN 1 AND 27 "
        "  AND InventoryType NOT IN (18, 19) "
        "  AND (FlagsExtra & 8192) = 0 "
        "  AND (stat_type1=35 OR stat_type2=35 OR stat_type3=35 OR stat_type4=35 "
        "    OR stat_type5=35 OR stat_type6=35 OR stat_type7=35 OR stat_type8=35 "
        "    OR stat_type9=35 OR stat_type10=35)"
    );

    if (!result)
    {
        LOG_WARN("playerbots", "[mod-pvpbots] BuildPvpItemCache: no resilience items found.");
        return;
    }

    uint32 count = 0;
    do
    {
        Field* fields = result->Fetch();

        PvpItemEntry e;
        e.itemId         = fields[0].Get<uint32>();
        e.inventoryType  = fields[1].Get<uint8>();
        e.requiredLevel  = fields[2].Get<uint8>();
        e.itemLevel      = fields[3].Get<uint16>();
        e.allowableClass = fields[4].Get<int32>();
        e.quality        = fields[5].Get<uint8>();

        _pvpItemCache[e.inventoryType].push_back(e);
        ++count;
    }
    while (result->NextRow());

    // Sort each bucket: best item level first.
    for (auto& kv : _pvpItemCache)
    {
        std::sort(kv.second.begin(), kv.second.end(),
            [](const PvpItemEntry& a, const PvpItemEntry& b)
            { return a.itemLevel > b.itemLevel; });
    }

    LOG_INFO("playerbots",
        "[mod-pvpbots] PvP item cache: {} resilience items across {} inventory types.",
        count, _pvpItemCache.size());

    // ── CC-break trinket cache ─────────────────────────────────────────────
    // Spell 42292 is "Removes all movement impairing effects and all effects
    // which cause loss of control of your character" — the iconic PvP trinket.
    _ccBreakTrinketCache.clear();
    {
        QueryResult r = WorldDatabase.Query(
            "SELECT entry, InventoryType, RequiredLevel, ItemLevel, AllowableClass, Quality "
            "FROM item_template "
            "WHERE Quality >= 2 "
            "  AND InventoryType = 12 "
            "  AND (FlagsExtra & 8192) = 0 "
            "  AND (spellid_1 = 42292 OR spellid_2 = 42292 OR spellid_3 = 42292 "
            "    OR spellid_4 = 42292 OR spellid_5 = 42292)"
        );
        if (r) do {
            Field* f = r->Fetch();
            PvpItemEntry e;
            e.itemId         = f[0].Get<uint32>();
            e.inventoryType  = f[1].Get<uint8>();
            e.requiredLevel  = f[2].Get<uint8>();
            e.itemLevel      = f[3].Get<uint16>();
            e.allowableClass = f[4].Get<int32>();
            e.quality        = f[5].Get<uint8>();
            _ccBreakTrinketCache.push_back(e);
        } while (r->NextRow());

        std::sort(_ccBreakTrinketCache.begin(), _ccBreakTrinketCache.end(),
            [](const PvpItemEntry& a, const PvpItemEntry& b)
            { return a.itemLevel > b.itemLevel; });

        LOG_INFO("playerbots", "[mod-pvpbots] CC-break trinket cache: {} items.",
            _ccBreakTrinketCache.size());
    }

    // ── All-trinket cache (for TRINKET2 PvE scoring) ──────────────────────
    _allTrinketCache.clear();
    {
        QueryResult r = WorldDatabase.Query(
            "SELECT entry, InventoryType, RequiredLevel, ItemLevel, AllowableClass, Quality "
            "FROM item_template "
            "WHERE Quality >= 2 "
            "  AND InventoryType = 12 "
            "  AND RequiredLevel > 0 "
            "  AND (FlagsExtra & 8192) = 0"
        );
        if (r) do {
            Field* f = r->Fetch();
            PvpItemEntry e;
            e.itemId         = f[0].Get<uint32>();
            e.inventoryType  = f[1].Get<uint8>();
            e.requiredLevel  = f[2].Get<uint8>();
            e.itemLevel      = f[3].Get<uint16>();
            e.allowableClass = f[4].Get<int32>();
            e.quality        = f[5].Get<uint8>();
            _allTrinketCache.push_back(e);
        } while (r->NextRow());

        std::sort(_allTrinketCache.begin(), _allTrinketCache.end(),
            [](const PvpItemEntry& a, const PvpItemEntry& b)
            { return a.itemLevel > b.itemLevel; });

        LOG_INFO("playerbots", "[mod-pvpbots] All-trinket cache: {} items.",
            _allTrinketCache.size());
    }
}

// ============================================================
// BuildHeirloomCache
//
// Queries item_template for all heirloom items (quality=7) that have a
// ScalingStatDistribution entry (i.e. they actually scale with level).
// Stored in _heirloomCache indexed by InventoryType.
//
// Called once during Initialize() when _allowHeirlooms = true.
// ============================================================
void PvpBotMgr::BuildHeirloomCache()
{
    _heirloomCache.clear();

    QueryResult r = WorldDatabase.Query(
        "SELECT entry, InventoryType, RequiredLevel, ItemLevel, AllowableClass, Quality "
        "FROM item_template "
        "WHERE Quality = 7 "
        "  AND InventoryType BETWEEN 1 AND 27 "
        "  AND InventoryType NOT IN (18, 19) "
        "  AND ScalingStatDistribution > 0"
    );

    if (!r)
    {
        LOG_INFO("playerbots", "[mod-pvpbots] Heirloom cache: no items found.");
        return;
    }

    uint32 count = 0;
    do
    {
        Field* f = r->Fetch();
        PvpItemEntry e;
        e.itemId         = f[0].Get<uint32>();
        e.inventoryType  = f[1].Get<uint8>();
        e.requiredLevel  = f[2].Get<uint8>();
        e.itemLevel      = f[3].Get<uint16>();
        e.allowableClass = f[4].Get<int32>();
        e.quality        = f[5].Get<uint8>();
        _heirloomCache[e.inventoryType].push_back(e);
        ++count;
    }
    while (r->NextRow());

    for (auto& kv : _heirloomCache)
        std::sort(kv.second.begin(), kv.second.end(),
            [](const PvpItemEntry& a, const PvpItemEntry& b)
            { return a.itemLevel > b.itemLevel; });

    LOG_INFO("playerbots",
        "[mod-pvpbots] Heirloom cache: {} items across {} inventory types.",
        count, _heirloomCache.size());
}

// ============================================================
// GetHeirloomStatWeight
//
// Returns a per-stat weight for heirloom scoring, keyed to the bot's class.
// Mirrors the ranges used by StatsWeightCalculator::GenerateBasicWeights so
// that ScoreHeirloom produces values on the same scale as CalculateItem.
//
// ITEM_MOD_RESILIENCE_RATING is intentionally 0.0 — it is handled as a
// multiplicative bonus in ScoreHeirloom, not as an additive stat weight.
// ============================================================
/*static*/ float PvpBotMgr::GetHeirloomStatWeight(Player* bot, uint32 statType)
{
    uint8 cls = bot->getClass();

    // Detect spec for hybrid classes. AiFactory::GetPlayerSpecTab reads the talent
    // tree directly (0/1/2) and is authoritative after InitPvpSpec has run.
    uint8 specTab = AiFactory::GetPlayerSpecTab(bot);

    // Broad role categories. Hybrids default to agi if their spec tab = 1
    // (Feral druid / Enhancement shaman), caster otherwise.
    bool strMelee  = (cls == CLASS_WARRIOR || cls == CLASS_PALADIN || cls == CLASS_DEATH_KNIGHT);
    bool agiMelee  = (cls == CLASS_ROGUE   || cls == CLASS_HUNTER);
    bool agiHybrid = ((cls == CLASS_DRUID  || cls == CLASS_SHAMAN) && specTab == 1);
    bool caster    = (cls == CLASS_MAGE    || cls == CLASS_WARLOCK || cls == CLASS_PRIEST)
                  || ((cls == CLASS_DRUID  || cls == CLASS_SHAMAN) && specTab != 1);

    // Primary stat weights. Weights of 0.0 for mismatched stats are intentional —
    // ScoreHeirloom uses a hasPrimaryStats gate to reject heirlooms whose primary
    // stats give 0 total weight (i.e. wrong item type for this spec).
    switch (statType)
    {
        case ITEM_MOD_STAMINA:
            return 0.1f;  // universal, but excluded from the primary-stat gate

        case ITEM_MOD_STRENGTH:
            return strMelee ? 2.5f : (agiMelee || agiHybrid ? 0.2f : 0.0f);

        case ITEM_MOD_AGILITY:
            return (agiMelee || agiHybrid) ? 2.2f : (strMelee ? 0.3f : 0.0f);

        case ITEM_MOD_INTELLECT:
            return (caster || cls == CLASS_PALADIN) ? 1.2f : 0.0f;

        case ITEM_MOD_SPIRIT:
            return caster ? 0.3f : 0.0f;

        case ITEM_MOD_HIT_RATING:           return 0.7f;
        case ITEM_MOD_CRIT_RATING:          return 0.8f;
        case ITEM_MOD_HASTE_RATING:         return 0.6f;

        case ITEM_MOD_EXPERTISE_RATING:
            return (strMelee || agiMelee || agiHybrid) ? 0.4f : 0.0f;

        case ITEM_MOD_ARMOR_PENETRATION_RATING:
            return (strMelee || agiMelee) ? 0.5f : 0.0f;

        case ITEM_MOD_ATTACK_POWER:
            return (strMelee || agiMelee || agiHybrid) ? 0.7f : 0.0f;

        case ITEM_MOD_RANGED_ATTACK_POWER:
            return (cls == CLASS_HUNTER) ? 0.7f : 0.0f;

        case ITEM_MOD_SPELL_POWER:          return caster ? 1.1f : 0.0f;
        case ITEM_MOD_MANA_REGENERATION:    return caster ? 0.2f : 0.0f;

        case ITEM_MOD_RESILIENCE_RATING:    return 0.0f;  // handled separately in ScoreHeirloom

        default:                            return 0.0f;
    }
}

// ============================================================
// GetHeirloomDpsWeight
//
// Returns the weight for weapon DPS for the given bot and item, mirroring
// StatsWeightCalculator::GenerateBasicWeights STATS_TYPE_MELEE_DPS /
// STATS_TYPE_RANGED_DPS values so ScoreHeirloom produces a DPS contribution
// on the same scale as CalculateItem.
//
// Uses proto->InventoryType to distinguish ranged vs melee weapons.
// ============================================================
/*static*/ float PvpBotMgr::GetHeirloomDpsWeight(Player* bot, ItemTemplate const* proto)
{
    uint8 cls = bot->getClass();
    uint8 tab = AiFactory::GetPlayerSpecTab(bot);

    bool isRanged = (proto->InventoryType == INVTYPE_RANGED ||
                     proto->InventoryType == INVTYPE_RANGEDRIGHT);

    if (isRanged)
    {
        if (cls == CLASS_HUNTER)
            return (tab == HUNTER_TAB_MARKSMANSHIP) ? 10.01f : 7.51f;

        // Wand/ranged for casters and healers.
        if (cls == CLASS_MAGE    || cls == CLASS_WARLOCK ||
            (cls == CLASS_PRIEST && tab != PRIEST_TAB_SHADOW) ||
            (cls == CLASS_DRUID  && tab == DRUID_TAB_BALANCE)  ||
            (cls == CLASS_DRUID  && tab == DRUID_TAB_RESTORATION))
            return 1.01f;

        if (cls == CLASS_PRIEST && tab == PRIEST_TAB_SHADOW)
            return 1.01f;

        return 0.01f;
    }

    bool   is2H          = (proto->InventoryType == INVTYPE_2HWEAPON);
    uint32 ssvMask       = proto->ScalingStatValue;
    bool   isCasterWeapon = (ssvMask & (0x800u | 0x1000u | 0x4000u)) != 0;

    // Caster weapons (staff/wand DPS ssvMask bits) score 0.0f for all melee
    // specs.  Combined with zero-weight INT/SP stats on physical specs, this
    // triggers the hasPrimaryStats gate in ScoreHeirloom and returns 0 — so
    // Grand Staff of Jordan will never compete on a warrior or enhancement shaman.
    if (isCasterWeapon)
        return 0.0f;

    // Hunters use melee slots as stat sticks (1H only). A 2H weapon would occupy
    // both weapon slots with no offhand benefit, so return 0.0f to ensure the
    // hasPrimaryStats gate fires and the item scores 0.
    if (cls == CLASS_HUNTER && is2H)
        return 0.0f;

    // Melee DPS weights.
    switch (cls)
    {
        case CLASS_DRUID:
            // Feral cat — speed normalised in form, raw DPS still determines
            // the normalised value used by Mangle/Shred.
            if (tab == DRUID_TAB_FERAL && !PlayerbotAI::IsTank(bot))
                return 15.01f;
            return 0.01f;

        case CLASS_PALADIN:
            if (tab == PALADIN_TAB_RETRIBUTION) return 9.01f;
            if (tab == PALADIN_TAB_PROTECTION)  return 2.01f;
            return 0.01f;

        case CLASS_SHAMAN:
            if (tab == SHAMAN_TAB_ENHANCEMENT)
            {
                // Once Dual Wield is learned, Enhancement never uses 2H.
                // Before it's learned, 2H is the correct weapon choice and
                // should score normally so the best 2H heirloom wins.
                if (is2H && bot->CanDualWield()) return 0.0f;
                return 8.51f;
            }
            return 0.01f;

        case CLASS_WARRIOR:
            if (tab == WARRIOR_TAB_ARMS) return 7.01f;
            if (tab == WARRIOR_TAB_FURY)
            {
                // TG Fury always wants 2H — no guard.
                // SMF Fury: once DW is learned, 1H only (same pattern as Enhancement).
                // Pre-DW: 2H is fine for both.
                if (is2H && bot->CanDualWield() && !bot->CanTitanGrip()) return 0.0f;
                return 7.01f;
            }
            if (tab == WARRIOR_TAB_PROTECTION) return 2.01f;
            return 0.01f;

        case CLASS_HUNTER:
            // Hunters use melee weapons as stat sticks (very low weight).
            // Once DW is learned, a 2H blocks the offhand stat stick — reject it
            // so both hands can carry 1H sticks post-DW.
            if (is2H && bot->CanDualWield()) return 0.0f;
            return 0.01f;

        case CLASS_DEATH_KNIGHT:
            if (tab == DEATH_KNIGHT_TAB_FROST)
            {
                // Frost DK has Dual Wield innately from creation — always 1H only.
                if (is2H) return 0.0f;
                return 7.01f;
            }
            if (tab == DEATH_KNIGHT_TAB_UNHOLY) return 5.01f;
            if (tab == DEATH_KNIGHT_TAB_BLOOD)  return 2.01f;
            return 0.01f;

        case CLASS_ROGUE:
            if (tab == ROGUE_TAB_COMBAT)        return 7.01f;
            return 5.01f;  // Assassination / Subtlety

        default:
            return 0.01f;
    }
}


// ============================================================
// ScoreHeirloom
//
// Scores a heirloom item for the given bot using SSD/SSV DBC data to recover
// the actual scaled stat values that item_template stores as zeros.
//
// Formula per stat:  val = (ssv->getssdMultiplier(mask) * ssd->Modifier[i]) / 10000
// This matches the client-side formula verified against known heirloom tooltips.
//
// The final score is scaled by CalcMixedGearScore(level * 2.5, EPIC) so it
// sits on the same magnitude as StatsWeightCalculator::CalculateItem output.
// At level 80 this places heirlooms below S8 gear; below 80 they win easily.
//
// Resilience is extracted separately and applied as a multiplicative bonus
// (identical to how the main gear loop treats resilience on regular items).
// ============================================================
float PvpBotMgr::ScoreHeirloom(Player* bot, uint32 itemId)
{
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
    if (!proto || proto->ScalingStatDistribution == 0)
        return 0.0f;

    uint8 level = bot->GetLevel();

    ScalingStatDistributionEntry const* ssd =
        sScalingStatDistributionStore.LookupEntry(proto->ScalingStatDistribution);
    if (!ssd)
        return 0.0f;

    // Clamp to the heirloom's MaxLevel (some scale only to 60 or 70).
    uint32 effectiveLevel = std::min(static_cast<uint32>(level), ssd->MaxLevel);

    // SSV rows are indexed by their Id field; in WotLK, Id == Level (1–100).
    ScalingStatValuesEntry const* ssv =
        sScalingStatValuesStore.LookupEntry(effectiveLevel);
    if (!ssv)
        return 0.0f;

    uint32 ssvMask = proto->ScalingStatValue;

    float  primaryScore   = 0.0f;  // non-stamina, non-resilience stats × class weights
    float  stamina        = 0.0f;
    float  resilience     = 0.0f;
    bool   hasPrimaryStats = false; // item has at least one non-stamina, non-resilience stat

    for (int i = 0; i < 10; ++i)
    {
        if (ssd->StatMod[i] < 0)   continue;
        if (ssd->Modifier[i] == 0) continue;

        uint32 mult    = ssv->getssdMultiplier(ssvMask);
        float  statVal = static_cast<float>(mult * ssd->Modifier[i]) / 10000.0f;
        uint32 statType = static_cast<uint32>(ssd->StatMod[i]);

        if (statType == ITEM_MOD_STAMINA)
            stamina = statVal;
        else if (statType == ITEM_MOD_RESILIENCE_RATING)
            resilience = statVal;
        else
        {
            hasPrimaryStats = true;
            primaryScore += statVal * GetHeirloomStatWeight(bot, statType);
        }
    }

    // Weapon DPS component — mirrors StatsCollector's STATS_TYPE_MELEE/RANGED_DPS
    // contribution to CalculateItem.  getDPSMod returns the weapon's DPS at
    // effectiveLevel for items whose ScalingStatValue has a DPS mask bit set.
    // This is what makes heirloom weapons competitive with non-heirloom weapons.
    if (proto->Class == ITEM_CLASS_WEAPON)
    {
        uint32 dpsMod = ssv->getDPSMod(ssvMask);
        if (dpsMod > 0)
        {
            float dpsWeight = GetHeirloomDpsWeight(bot, proto);
            primaryScore += static_cast<float>(dpsMod) * dpsWeight;
            hasPrimaryStats = true;
        }
    }

    // Gate: if the item has primary stats but none of them contribute for this
    // class/spec (e.g. a spell-power heirloom on a physical DPS, or an agi
    // heirloom on a caster), return 0 so it doesn't enter the candidate pool.
    // Items with NO primary stats (pure resilience/stamina, e.g. CC trinkets)
    // bypass this gate and are scored on resilience alone below.
    if (hasPrimaryStats && primaryScore == 0.0f)
        return 0.0f;

    // Full stat score — primary stats (including weapon DPS) + stamina.
    float statScore = primaryScore + stamina * 0.1f;

    // Scale to match StatsWeightCalculator::CalculateItem magnitude.
    // CalculateItem does:  sum(statVal × classWeight) × CalcMixedGearScore(ilvl, quality)
    // Using level × 2.5 as effective ilvl: heirlooms compete at sub-80 levels
    // and naturally lose to endgame S8 gear at 80.
    float effectiveIlvl = static_cast<float>(level) * 2.5f;
    statScore *= PlayerbotFactory::CalcMixedGearScore(effectiveIlvl, ITEM_QUALITY_EPIC);

    if (_pvpResilienceWeight > 0.0f && resilience > 0.0f)
    {
        if (statScore > 0.0f)
        {
            // Normal case: resilience as a multiplicative bonus (same as main gear loop).
            statScore *= (1.0f + (resilience / 100.0f) * _pvpResilienceWeight);
        }
        else
        {
            // Item has no primary stats (e.g. pure CC-break trinket with only
            // resilience in its SSD). Score from resilience alone so it can beat
            // a blue 0-stat CC trinket when PreferPvpGear is enabled.
            statScore = resilience * 0.5f *
                        PlayerbotFactory::CalcMixedGearScore(effectiveIlvl, ITEM_QUALITY_EPIC);
        }
    }

    return statScore;
}

// ============================================================
// CanEquipItemTemp
//
// Safe replacement for Player::CanEquipNewItem when called in a tight loop.
// Player::CanEquipNewItem allocates the temp Item with a real GUID and never
// calls RemoveFromUpdateQueueOf before deleting — leaving stale pointers in
// the bot's update queue when called hundreds of times per init.
//
// This version mirrors PlayerbotFactory::CanEquipUnseenItem (private):
//   temp=true  →  GUID = 0xFFFFFFFF, no object-manager registration
//   RemoveFromUpdateQueueOf  →  safe to delete immediately after
// ============================================================
bool PvpBotMgr::CanEquipItemTemp(Player* bot, uint8 slot, uint16& dest, uint32 itemId)
{
    dest = 0;
    Item* pItem = Item::CreateItem(itemId, 1, bot, false, 0, true);  // temp=true
    if (!pItem)
        return false;

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    InventoryResult result = botAI
        ? botAI->CanEquipItem(slot, dest, pItem, true, true)
        : bot->CanEquipItem(slot, dest, pItem, true, true);

    pItem->RemoveFromUpdateQueueOf(bot);
    delete pItem;
    return result == EQUIP_ERR_OK;
}

// ============================================================
// GetWeaponSpeedMultiplier
//
// Returns (1.0 + weight) when proto->Delay matches the spec-ideal speed for
// the given slot; 1.0 otherwise.  Applied as a multiplicative factor after
// CalculateItem / ScoreHeirloom so the scorer still picks the best-statted
// weapon at the correct speed rather than any weapon at that speed.
//
// Speed thresholds (milliseconds, verified from WotLK reference data):
//   Slow 2H      : Delay >= 3400  (Arms/Ret/Blood/Unholy/Fury-TG)
//   Slow 1H      : Delay >= 2600  (Enhancement/Frost DK/Fury-SMF, both slots;
//                                   Rogue Combat MH)
//   Fast 1H      : Delay <= 1500  (Rogue Combat OH, Rogue Ass/Sub OH)
//   Slow dagger  : Delay >= 1700  (Rogue Ass/Sub MH — daggers only)
//   Slow ranged  : Delay >= 2800  (Hunter ranged — bows/guns/crossbows;
//                                   higher top-end damage amplifies Aimed/
//                                   Chimera/Explosive Shot scaling)
//
// Enhancement Shaman sync bonus: MH and OH with identical Delay values swing
// simultaneously, consuming only ONE Flurry charge per pair instead of two.
// This doubles the effective value of Flurry (6 boosted attacks vs 3).  The
// bonus is applied only when scoring the OH (MH is already equipped by then),
// stacking multiplicatively on top of the base slow bonus.
//
// Hunter melee: pure stat sticks; melee speed is irrelevant — returns 1.0.
// Feral Druid: attack speed is normalised in bear/cat form — returns 1.0.
// ============================================================
/*static*/ float PvpBotMgr::GetWeaponSpeedMultiplier(Player* bot, int32 slot,
                                                      ItemTemplate const* proto,
                                                      float weight)
{
    if (weight == 0.0f)
        return 1.0f;

    // Applies to mainhand, offhand, and ranged slots only.
    if (slot != EQUIPMENT_SLOT_MAINHAND &&
        slot != EQUIPMENT_SLOT_OFFHAND  &&
        slot != EQUIPMENT_SLOT_RANGED)
        return 1.0f;

    // Only weapon items carry a Delay value.
    if (proto->Class != ITEM_CLASS_WEAPON)
        return 1.0f;

    uint8  cls   = bot->getClass();
    uint8  tab   = AiFactory::GetPlayerSpecTab(bot);
    uint32 delay = proto->Delay;  // milliseconds

    // Hunter: melee weapons are stat sticks — speed irrelevant.
    // Ranged weapons scale Aimed/Chimera/Explosive Shot from top-end damage,
    // so a slow ranged weapon (>=2800 ms) is strongly preferred.
    if (cls == CLASS_HUNTER)
    {
        if (slot == EQUIPMENT_SLOT_RANGED && delay >= 2800)
            return 1.0f + weight;
        return 1.0f;
    }

    // Feral Druid: forms normalise attack speed; raw weapon Delay is irrelevant.
    if (cls == CLASS_DRUID && tab == DRUID_TAB_FERAL)
        return 1.0f;

    float boost = 1.0f + weight;  // applied on a match

    switch (cls)
    {
        case CLASS_WARRIOR:
            if (tab == WARRIOR_TAB_ARMS)
            {
                // Arms: slow 2H in mainhand only.
                if (slot == EQUIPMENT_SLOT_MAINHAND && delay >= 3400)
                    return boost;
            }
            else if (tab == WARRIOR_TAB_FURY)
            {
                if (!bot->CanDualWield())
                {
                    // Pre-DW: treat like Arms — slow 2H in mainhand only.
                    if (slot == EQUIPMENT_SLOT_MAINHAND && delay >= 3400)
                        return boost;
                }
                else if (bot->CanTitanGrip())
                {
                    // TG: slow 2H (>=3400) in both hands.
                    if (delay >= 3400)
                        return boost;
                }
                else
                {
                    // SMF: slow 1H (>=2600) in both hands.
                    // 2H must be excluded — delay >= 2600 would otherwise pass
                    // for a 2H heirloom (~3600ms) just as it did for Enhancement.
                    if (proto->InventoryType == INVTYPE_2HWEAPON)
                        break;
                    if (delay >= 2600)
                        return boost;
                }
            }
            break;

        case CLASS_PALADIN:
            if (tab == PALADIN_TAB_RETRIBUTION)
            {
                // Ret: slow 2H in mainhand only.
                if (slot == EQUIPMENT_SLOT_MAINHAND && delay >= 3400)
                    return boost;
            }
            break;

        case CLASS_DEATH_KNIGHT:
            if (tab == DEATH_KNIGHT_TAB_BLOOD || tab == DEATH_KNIGHT_TAB_UNHOLY)
            {
                // Blood / Unholy: slow 2H in mainhand only.
                if (slot == EQUIPMENT_SLOT_MAINHAND && delay >= 3400)
                    return boost;
            }
            else if (tab == DEATH_KNIGHT_TAB_FROST)
            {
                // Frost DK has Dual Wield innately — always dual-wields 1H.
                // 2H weapons must not receive the slow-1H bonus.
                if (proto->InventoryType == INVTYPE_2HWEAPON)
                    break;
                if (delay >= 2600)
                    return boost;
            }
            break;

        case CLASS_SHAMAN:
            if (tab == SHAMAN_TAB_ENHANCEMENT)
            {
                if (!bot->CanDualWield())
                {
                    // Pre-Dual Wield: Enhancement plays like a 2H spec.
                    // Reward a slow 2H in mainhand the same way Arms/Ret does.
                    if (slot == EQUIPMENT_SLOT_MAINHAND && delay >= 3400)
                        return boost;
                }
                else
                {
                    // Post-Dual Wield: slow 1H (>=2600) in both hands.
                    // 2H weapons must not receive this bonus.
                    if (proto->InventoryType == INVTYPE_2HWEAPON)
                        break;

                    // Matching speeds between MH and OH cause both auto-attacks
                    // to land simultaneously, consuming only ONE Flurry charge
                    // instead of two — giving 6 boosted attacks under Flurry
                    // rather than 3.  Sync bonus applied on OH only (MH is
                    // already equipped by then).
                    if (delay >= 2600)
                    {
                        float mult = boost;
                        if (slot == EQUIPMENT_SLOT_OFFHAND)
                        {
                            Item* mh = bot->GetItemByPos(INVENTORY_SLOT_BAG_0,
                                                         EQUIPMENT_SLOT_MAINHAND);
                            if (mh && mh->GetTemplate() && mh->GetTemplate()->Delay == delay)
                                mult *= boost;  // synchronized: ×(1+weight)² total
                        }
                        return mult;
                    }
                }
            }
            break;

        case CLASS_ROGUE:
            if (tab == ROGUE_TAB_COMBAT)
            {
                // Combat: slow MH (>=2600), fast OH (<=1500).
                if (slot == EQUIPMENT_SLOT_MAINHAND && delay >= 2600)
                    return boost;
                if (slot == EQUIPMENT_SLOT_OFFHAND && delay <= 1500)
                    return boost;
            }
            else  // Assassination or Subtlety: slow dagger MH, fast dagger OH.
            {
                bool isDagger = (proto->SubClass == ITEM_SUBCLASS_WEAPON_DAGGER);
                if (slot == EQUIPMENT_SLOT_MAINHAND && isDagger && delay >= 1700)
                    return boost;
                if (slot == EQUIPMENT_SLOT_OFFHAND && isDagger && delay <= 1500)
                    return boost;
            }
            break;

        default:
            break;
    }

    return 1.0f;
}

// ============================================================
// InitPvpEquipment
//
// Single-pass gear initialization for PvP bots.  Mirrors the structure of
// PlayerbotFactory::InitEquipment but adds a resilience rating bonus so that
// PvP gear edges out equivalent PvE gear when both are candidates.
//
// Item pool: sRandomItemMgr.GetCachedEquipments (all equippable items, same
// as InitEquipment).  Scoring: StatsWeightCalculator + _pvpResilienceWeight
// per resilience rating point.
//
// TRINKET1 → highest-level CC-break trinket (spell 42292) the bot qualifies
//            for, with a faction heirloom fallback.
// TRINKET2 → best scored trinket from the full pool (StatsWeightCalculator).
// All other slots → best scored item from sRandomItemMgr for that slot.
// ============================================================
void PvpBotMgr::InitPvpEquipment(Player* bot)
{
    uint8  level     = bot->GetLevel();
    int32  delta     = static_cast<int32>(std::min(static_cast<uint32>(level), 10u));
    int32  classMask = static_cast<int32>(bot->getClassMask());

    // Scorer: same one playerbots uses — detects role from class+spec.
    StatsWeightCalculator calculator(bot);
    calculator.SetItemSetBonus(false);

    // Maps an equipment slot to the InventoryTypes that fit it.
    // Mirrors PlayerbotFactory::GetPossibleInventoryTypeListBySlot (private).
    auto getInvTypes = [](int32 slot) -> std::vector<InventoryType>
    {
        switch (slot)
        {
            case EQUIPMENT_SLOT_HEAD:      return { INVTYPE_HEAD };
            case EQUIPMENT_SLOT_NECK:      return { INVTYPE_NECK };
            case EQUIPMENT_SLOT_SHOULDERS: return { INVTYPE_SHOULDERS };
            case EQUIPMENT_SLOT_BODY:      return { INVTYPE_BODY };
            case EQUIPMENT_SLOT_CHEST:     return { INVTYPE_CHEST, INVTYPE_ROBE };
            case EQUIPMENT_SLOT_WAIST:     return { INVTYPE_WAIST };
            case EQUIPMENT_SLOT_LEGS:      return { INVTYPE_LEGS };
            case EQUIPMENT_SLOT_FEET:      return { INVTYPE_FEET };
            case EQUIPMENT_SLOT_WRISTS:    return { INVTYPE_WRISTS };
            case EQUIPMENT_SLOT_HANDS:     return { INVTYPE_HANDS };
            case EQUIPMENT_SLOT_FINGER1:
            case EQUIPMENT_SLOT_FINGER2:   return { INVTYPE_FINGER };
            case EQUIPMENT_SLOT_TRINKET1:
            case EQUIPMENT_SLOT_TRINKET2:  return { INVTYPE_TRINKET };
            case EQUIPMENT_SLOT_BACK:      return { INVTYPE_CLOAK };
            case EQUIPMENT_SLOT_MAINHAND:  return { INVTYPE_WEAPON, INVTYPE_2HWEAPON, INVTYPE_WEAPONMAINHAND };
            case EQUIPMENT_SLOT_OFFHAND:   return { INVTYPE_WEAPON, INVTYPE_2HWEAPON, INVTYPE_WEAPONOFFHAND,
                                                    INVTYPE_SHIELD, INVTYPE_HOLDABLE };
            case EQUIPMENT_SLOT_RANGED:    return { INVTYPE_RANGED, INVTYPE_RANGEDRIGHT, INVTYPE_RELIC };
            default:                       return {};
        }
    };

    // Inline equivalent of PlayerbotFactory::CanEquipItem (private):
    // rejects timed items, quest-bound items, and over-level-requirement items.
    auto canEquipItem = [&](ItemTemplate const* proto) -> bool
    {
        if (!proto)                               return false;
        if (proto->Duration != 0)                 return false;
        if (proto->Bonding == BIND_QUEST_ITEM)    return false;
        if (proto->RequiredLevel > level)         return false;
        return true;
    };

    // Slot processing order mirrors InitEquipment (weapons and trinkets first).
    static const std::vector<int32> pvpSlotsOrder = {
        EQUIPMENT_SLOT_TRINKET1, EQUIPMENT_SLOT_TRINKET2,
        EQUIPMENT_SLOT_MAINHAND, EQUIPMENT_SLOT_OFFHAND,  EQUIPMENT_SLOT_RANGED,
        EQUIPMENT_SLOT_HEAD,     EQUIPMENT_SLOT_SHOULDERS, EQUIPMENT_SLOT_CHEST,
        EQUIPMENT_SLOT_LEGS,     EQUIPMENT_SLOT_HANDS,    EQUIPMENT_SLOT_NECK,
        EQUIPMENT_SLOT_BODY,     EQUIPMENT_SLOT_WAIST,    EQUIPMENT_SLOT_FEET,
        EQUIPMENT_SLOT_WRISTS,   EQUIPMENT_SLOT_FINGER1,  EQUIPMENT_SLOT_FINGER2,
        EQUIPMENT_SLOT_BACK
    };

    // Track items already placed so FINGER1/2 and similar paired slots don't
    // receive the same physical item twice.
    std::set<uint32> usedIds;

    // ── TRINKET1: CC-break trinket ────────────────────────────────────────
    // Handled before the main loop. If filled here, TRINKET1 is skipped below.
    bool trinket1Filled = false;
    if (_enableCcBreakTrinket && !_ccBreakTrinketCache.empty())
    {
        if (Item* t = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_TRINKET1))
            bot->DestroyItem(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_TRINKET1, true);

        // Score every qualifying CC trinket and equip the best one.
        // Heirlooms compete on score via ScoreHeirloom — their resilience stat
        // (scored even when it's the only stat, via the resilience-only path in
        // ScoreHeirloom) lets them beat a blue 0-stat trinket when PreferPvpGear=1.
        float  bestTrinketScore = -1.0f;
        uint32 bestTrinketId    = 0;
        uint16 bestTrinketDest  = 0;

        for (const PvpItemEntry& entry : _ccBreakTrinketCache)
        {
            if (entry.quality == ITEM_QUALITY_HEIRLOOM && !_allowHeirlooms) continue;
            if (entry.requiredLevel > level)                                 continue;
            if (entry.allowableClass != -1 &&
                !(entry.allowableClass & classMask))                         continue;

            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry.itemId);
            if (!proto) continue;

            float score;
            if (entry.quality == ITEM_QUALITY_HEIRLOOM)
            {
                score = ScoreHeirloom(bot, entry.itemId);
            }
            else
            {
                score = calculator.CalculateItem(entry.itemId);
                if (_pvpResilienceWeight > 0.0f && score > 0.0f)
                    for (uint8 j = 0; j < MAX_ITEM_PROTO_STATS; ++j)
                        if (proto->ItemStat[j].ItemStatType == ITEM_MOD_RESILIENCE_RATING &&
                            proto->ItemStat[j].ItemStatValue > 0)
                        {
                            score *= 1.0f + (proto->ItemStat[j].ItemStatValue / 100.0f) * _pvpResilienceWeight;
                            break;
                        }
            }

            if (score <= bestTrinketScore) continue;

            uint16 dest;
            if (!CanEquipItemTemp(bot, EQUIPMENT_SLOT_TRINKET1, dest, entry.itemId))
                continue;

            bestTrinketScore = score;
            bestTrinketId    = entry.itemId;
            bestTrinketDest  = dest;
        }

        if (bestTrinketId != 0)
        {
            bot->EquipNewItem(bestTrinketDest, bestTrinketId, true);
            usedIds.insert(bestTrinketId);
            trinket1Filled = true;
        }
    }

    // ── Main slot loop ────────────────────────────────────────────────────
    for (int32 slot : pvpSlotsOrder)
    {
        if (slot == EQUIPMENT_SLOT_TABARD || slot == EQUIPMENT_SLOT_BODY)
            continue;

        // TRINKET1 already filled above.
        if (slot == EQUIPMENT_SLOT_TRINKET1 && trinket1Filled)
            continue;

        // Clear current item so CanEquipNewItem doesn't reject due to
        // unique-equip conflicts with whatever InitEquipment left here.
        if (Item* oldItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            bot->DestroyItem(INVENTORY_SLOT_BAG_0, slot, true);

        // Gather candidates from sRandomItemMgr, stepping down quality if
        // fewer than 25 are found at the desired quality.
        std::vector<uint32> candidates;
        int32 desiredQuality = static_cast<int32>(_gearQualityLimit);

        do
        {
            for (uint32 reqLevel = level;
                 reqLevel > static_cast<uint32>(std::max(static_cast<int32>(level) - delta, 0));
                 --reqLevel)
            {
                for (InventoryType invType : getInvTypes(slot))
                {
                    for (uint32 itemId : sRandomItemMgr.GetCachedEquipments(reqLevel, invType))
                    {
                        if (itemId == 46978) continue; // shaman earth ring totem
                        if (usedIds.count(itemId)) continue;

                        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
                        if (!proto) continue;
                        if (proto->Class != ITEM_CLASS_WEAPON && proto->Class != ITEM_CLASS_ARMOR)
                            continue;
                        if (proto->Quality != static_cast<uint32>(desiredQuality))
                            continue;
                        if (_gearScoreLimit != 0 && desiredQuality > ITEM_QUALITY_NORMAL &&
                            PlayerbotFactory::CalcMixedGearScore(proto->ItemLevel, proto->Quality) > _gearScoreLimit)
                            continue;

                        // Proficiency checks via sRandomItemMgr (public).
                        if (proto->Class == ITEM_CLASS_ARMOR &&
                            (slot == EQUIPMENT_SLOT_HEAD      || slot == EQUIPMENT_SLOT_SHOULDERS ||
                             slot == EQUIPMENT_SLOT_CHEST     || slot == EQUIPMENT_SLOT_WAIST     ||
                             slot == EQUIPMENT_SLOT_LEGS      || slot == EQUIPMENT_SLOT_FEET      ||
                             slot == EQUIPMENT_SLOT_WRISTS    || slot == EQUIPMENT_SLOT_HANDS) &&
                            !sRandomItemMgr.CanEquipArmor(bot->getClass(), level, proto))
                            continue;
                        if (proto->Class == ITEM_CLASS_WEAPON &&
                            !sRandomItemMgr.CanEquipWeapon(bot->getClass(), proto))
                            continue;

                        candidates.push_back(itemId);
                    }
                }
            }
        } while (candidates.size() < 25 && desiredQuality-- > ITEM_QUALITY_POOR);

        // Add heirloom candidates from _heirloomCache (scored separately via ScoreHeirloom).
        // Heirlooms are excluded from sRandomItemMgr because quality=7 is above GearQualityLimit.
        if (_allowHeirlooms)
        {
            for (InventoryType invType : getInvTypes(slot))
            {
                auto it = _heirloomCache.find(static_cast<uint8>(invType));
                if (it == _heirloomCache.end()) continue;
                for (const PvpItemEntry& he : it->second)
                {
                    // Note: usedIds check intentionally omitted for heirlooms.
                    // Heirlooms are BOA and can be dual-wielded (same item ID in
                    // both MH and OH).  usedIds exists only to prevent the same
                    // non-unique drop from filling two slots (e.g. a ring twice).
                    if (he.allowableClass != -1 && !(he.allowableClass & classMask)) continue;
                    ItemTemplate const* hProto = sObjectMgr->GetItemTemplate(he.itemId);
                    if (!hProto || hProto->Duration != 0 || hProto->Bonding == BIND_QUEST_ITEM)
                        continue;
                    candidates.push_back(he.itemId);
                }
            }
        }

        if (candidates.empty())
            continue;

        // Score all candidates, pick the winner.
        // Heirlooms use ScoreHeirloom (SSD/SSV DBC); regular items use
        // StatsWeightCalculator::CalculateItem with a multiplicative resilience bonus.
        float  bestScore = -1.0f;
        uint32 bestId    = 0;
        uint16 bestDest  = 0;

        for (uint32 itemId : candidates)
        {
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
            if (!proto) continue;

            float score;
            if (proto->Quality == ITEM_QUALITY_HEIRLOOM)
            {
                // ScoreHeirloom uses SSD/SSV scaling and applies the resilience
                // multiplier internally — no extra step needed here.
                score = ScoreHeirloom(bot, itemId);
            }
            else
            {
                score = calculator.CalculateItem(itemId);

                // Multiplicative resilience boost: score *= (1 + resilience/100 * weight).
                if (_pvpResilienceWeight > 0.0f && score > 0.0f)
                {
                    for (uint8 j = 0; j < MAX_ITEM_PROTO_STATS; ++j)
                    {
                        if (proto->ItemStat[j].ItemStatType == ITEM_MOD_RESILIENCE_RATING &&
                            proto->ItemStat[j].ItemStatValue > 0)
                        {
                            score *= 1.0f + (proto->ItemStat[j].ItemStatValue / 100.0f) * _pvpResilienceWeight;
                            break;
                        }
                    }
                }
            }

            // Weapon-speed governance: multiply score by (1 + weight) when
            // this weapon's Delay matches the spec-ideal speed for this slot.
            // Applied after stat/resilience scoring so the scorer still picks
            // the best-statted weapon at the right speed, not just any slow weapon.
            if (_weaponSpeedWeight > 0.0f && score > 0.0f)
                score *= GetWeaponSpeedMultiplier(bot, slot, proto, _weaponSpeedWeight);

            if (score <= bestScore)
                continue;

            // Deferred heavy checks — same pattern as InitEquipment.
            if (!canEquipItem(proto))
                continue;

            uint16 dest;
            if (!CanEquipItemTemp(bot, slot, dest, itemId))
                continue;

            bestScore = score;
            bestId    = itemId;
            bestDest  = dest;
        }

        if (bestId == 0)
            continue;

        if (_debug)
        {
            ItemTemplate const* win = sObjectMgr->GetItemTemplate(bestId);
            bool isHeirloom = win && win->Quality == ITEM_QUALITY_HEIRLOOM;
            float resBonus   = 0.0f;
            float speedMult  = 1.0f;
            if (win)
            {
                speedMult = GetWeaponSpeedMultiplier(bot, slot, win, _weaponSpeedWeight);
                if (!isHeirloom)
                    for (uint8 j = 0; j < MAX_ITEM_PROTO_STATS; ++j)
                        if (win->ItemStat[j].ItemStatType == ITEM_MOD_RESILIENCE_RATING &&
                            win->ItemStat[j].ItemStatValue > 0)
                        {
                            resBonus = bestScore - bestScore / (1.0f + (win->ItemStat[j].ItemStatValue / 100.0f) * _pvpResilienceWeight);
                            break;
                        }
            }
            LOG_INFO("playerbots",
                "[mod-pvpbots][GEAR] slot={} item={} ({}) ilvl={} delay={}ms score={:.1f} resil_bonus={:.1f} speed_mult={:.2f} candidates={} {}",
                slot,
                bestId,
                win ? win->Name1 : "???",
                win ? win->ItemLevel : 0,
                win ? win->Delay : 0,
                bestScore,
                resBonus,
                speedMult,
                candidates.size(),
                isHeirloom ? "[heirloom]" : "");
        }

        bot->EquipNewItem(bestDest, bestId, true);
        bot->AutoUnequipOffhandIfNeed();
        usedIds.insert(bestId);
    }
}
