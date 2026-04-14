/*
 * mod-pvpbots — PvpBotInit.cpp
 *
 * Bot initialisation pipeline.  Called once per bot login from
 * PvpBotMgr::OnBotLoginInternal → InitPvpBot.
 *
 *   InitPvpBot     — top-level single-pass init (level, talents, gear)
 *   InitPvpGlyphs  — applies configured glyph IDs to the correct slots
 *   InitPvpSpec    — applies talent links + resets AI strategies
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

void PvpBotMgr::InitPvpBot(Player* bot)
{
    uint8 level = bot->GetLevel();
    uint8 classId = bot->getClass();

    std::string specName = "Unknown";
    uint32 primaryTab = 0;
    const PvpSpec* selectedSpec = nullptr;

    auto it = _pvpSpecs.find(classId);
    if (it != _pvpSpecs.end() && !it->second.empty())
    {
        // Pick a random spec for this class
        selectedSpec =
            &it->second[urand(0, static_cast<uint32>(it->second.size()) - 1)];
        specName = selectedSpec->name;

        // Calculate which tree is primary by counting talent points
        std::vector<std::vector<uint32>> parsedSpec;
        std::string specLinkStr;
        for (const auto& ll : selectedSpec->links)
        {
            if (ll.level <= level)
                specLinkStr = ll.link;
            else
                break;
        }
        if (specLinkStr.empty() && !selectedSpec->links.empty())
            specLinkStr = selectedSpec->links[0].link;

        if (!specLinkStr.empty())
            parsedSpec = PlayerbotAIConfig::ParseTempTalentsOrder(classId, specLinkStr);

        if (!parsedSpec.empty())
        {
            uint32 tabPoints[3] = { 0, 0, 0 };
            for (const auto& entry : parsedSpec)
                if (entry.size() >= 4 && entry[0] < 3)
                    tabPoints[entry[0]] += entry[3];
            if (tabPoints[1] > tabPoints[0]) primaryTab = 1;
            if (tabPoints[2] > tabPoints[primaryTab]) primaryTab = 2;
        }
    }

    PlayerbotFactory factory(bot, level, _gearQualityLimit, _gearScoreLimit);

    if (bot->isDead())
        bot->ResurrectPlayer(1.0f, false);
    bot->CombatStop(true);

    factory.ClearEverything();
    CharacterDatabase.Execute(
        "DELETE FROM character_spell WHERE guid = {}",
        bot->GetGUID().GetCounter()
    );

    bot->GiveLevel(level);
    bot->InitStatsForLevel(true);
    bot->RemoveAllSpellCooldown();
    factory.UnbindInstance();
    factory.InitInstanceQuests();
    factory.InitAttunementQuests();

    // Death Knights earn talent points through quests in the Ebon Hold starting
    // zone (RewardTalents > 0). These quests have no reward spell so they are
    // excluded from PlayerbotFactory::classQuestIds and never processed by
    // InitInstanceQuests. Without rewarding them, m_questRewardTalentCount = 0,
    // which restricts talent points to (level - 55) inside MAP_EBON_HOLD.
    // Rewarding them here sets m_questRewardTalentCount = 46 (total across all
    // DK starting quests), ensuring full talent availability on any map.
    if (classId == CLASS_DEATH_KNIGHT)
    {
        static const std::vector<uint32> dkTalentQuestIds = {
            12678, 12679, 12680, 12687, 12698, 12701, 12706,
            12716, 12719, 12720, 12722, 12724, 12725, 12727,
            12733,
            12739, 12740, 12741, 12742, 12743, 12744,  // "A Special Surprise" —
            12745, 12746, 12747, 12748, 12749, 12750,  // racial variants; only the
                                                        // matching race will pass
                                                        // SatisfyQuestRace
            12751, 12754, 12755, 12756, 12757, 12779, 12801
        };

        uint32 savedXP = bot->GetUInt32Value(PLAYER_XP);

        for (uint32 questId : dkTalentQuestIds)
        {
            if (bot->IsQuestRewarded(questId))
                continue;

            Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
            if (!quest)
                continue;

            if (!bot->SatisfyQuestClass(quest, false) || !bot->SatisfyQuestRace(quest, false))
                continue;

            bot->SetQuestStatus(questId, QUEST_STATUS_COMPLETE);
            bot->RewardQuest(quest, 0, bot, false);
        }

        // RewardQuest can grant XP and level ups — restore the intended level.
        bot->GiveLevel(level);
        bot->SetUInt32Value(PLAYER_XP, savedXP);
    }

    bot->LearnDefaultSkills();
    factory.InitSkills();
    factory.InitClassSpells();
    factory.InitAvailableSpells();

    // Apply PvP talents (includes glyphs and strategy reset)
    if (selectedSpec)
        InitPvpSpec(bot, selectedSpec, primaryTab);

    factory.InitAvailableSpells();
    factory.InitReputation();
    factory.InitSpecialSpells();
    factory.InitMounts();
    InitPvpEquipment(bot);
    factory.InitBags();
    factory.InitAmmo();
    factory.InitFood();
    factory.InitPotions();
    factory.InitReagents();
    factory.InitKeyring();
    factory.InitConsumables();

    if (_applyEnchantsAndGems)
        factory.ApplyEnchantAndGemsNew(true);

    if (classId != CLASS_HUNTER && classId != CLASS_WARLOCK && classId != CLASS_DEATH_KNIGHT)
    {
        if (Pet* pet = bot->GetPet())
            pet->Remove(PET_SAVE_AS_DELETED);
        CharacterDatabase.Execute(
            "DELETE FROM character_pet WHERE owner = {}",
            bot->GetGUID().GetCounter()
        );
    }
    else if (level >= 10)
    {
        bot->RemovePet(nullptr, PET_SAVE_AS_CURRENT, true);
        bot->RemovePet(nullptr, PET_SAVE_NOT_IN_SLOT, true);
        factory.InitPet();
        factory.InitPetTalents();
    }

    bot->SetMoney(urand(level * 100000, level * 5 * 100000));
    bot->SetHealth(bot->GetMaxHealth());
    bot->SetPower(POWER_MANA, bot->GetMaxPower(POWER_MANA));
    bot->SaveToDB(false, false);

    DebugLog("InitPvpBot complete: " + bot->GetName() + " [" + specName + "]");
}

void PvpBotMgr::InitPvpGlyphs(Player* bot, const PvpSpec& spec)
{
    bot->InitGlyphsForLevel();

    for (uint32 slotIndex = 0; slotIndex < MAX_GLYPH_SLOT_INDEX; ++slotIndex)
    {
        uint32 glyph = bot->GetGlyph(slotIndex);
        if (GlyphPropertiesEntry const* glyphEntry = sGlyphPropertiesStore.LookupEntry(glyph))
        {
            bot->RemoveAurasDueToSpell(glyphEntry->SpellId);
            bot->SetGlyph(slotIndex, 0, true);
        }
    }

    if (spec.glyphs.empty())
        return;

    std::vector<uint32> glyphItemIds;
    {
        std::stringstream ss(spec.glyphs);
        std::string token;
        while (std::getline(ss, token, ','))
        {
            auto first = std::find_if(token.begin(), token.end(), [](unsigned char c){ return !std::isspace(c); });
            auto last  = std::find_if(token.rbegin(), token.rend(), [](unsigned char c){ return !std::isspace(c); }).base();
            if (first < last)
                glyphItemIds.push_back(static_cast<uint32>(std::stoul(std::string(first, last))));
        }
    }

    if (glyphItemIds.empty())
        return;

    uint32 level = bot->GetLevel();
    uint32 maxSlot = 0;
    if (level >= 15) maxSlot = 2;
    if (level >= 30) maxSlot = 3;
    if (level >= 50) maxSlot = 4;
    if (level >= 70) maxSlot = 5;
    if (level >= 80) maxSlot = 6;

    if (!maxSlot)
        return;

    static const uint8 glyphOrder[6] = {0, 1, 3, 2, 4, 5};

    for (uint32 slotIndex = 0; slotIndex < maxSlot; ++slotIndex)
    {
        if (slotIndex >= glyphItemIds.size())
            break;

        uint8 realSlot = glyphOrder[slotIndex];
        uint32 itemId  = glyphItemIds[slotIndex];

        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
        if (!proto)
            continue;

        if (proto->Class != ITEM_CLASS_GLYPH)
            continue;

        if ((proto->AllowableClass & bot->getClassMask()) == 0 || (proto->AllowableRace & bot->getRaceMask()) == 0)
            continue;

        if (proto->RequiredLevel > bot->GetLevel())
            continue;

        uint32 glyphId = 0;
        for (uint32 spell = 0; spell < MAX_ITEM_PROTO_SPELLS; ++spell)
        {
            uint32 spellId = proto->Spells[spell].SpellId;
            SpellInfo const* entry = sSpellMgr->GetSpellInfo(spellId);
            if (!entry)
                continue;

            for (uint32 effect = 0; effect <= EFFECT_2; ++effect)
            {
                if (entry->Effects[effect].Effect != SPELL_EFFECT_APPLY_GLYPH)
                    continue;

                glyphId = entry->Effects[effect].MiscValue;
            }
        }

        if (!glyphId)
            continue;

        GlyphPropertiesEntry const* glyphEntry = sGlyphPropertiesStore.LookupEntry(glyphId);
        if (!glyphEntry)
            continue;

        uint32 slot = bot->GetGlyphSlot(realSlot);
        GlyphSlotEntry const* gs = sGlyphSlotStore.LookupEntry(slot);
        if (!gs || glyphEntry->TypeFlags != gs->TypeFlags)
            continue;

        bot->CastSpell(bot, glyphEntry->SpellId,
                       TriggerCastFlags(TRIGGERED_FULL_MASK &
                                        ~(TRIGGERED_IGNORE_SHAPESHIFT | TRIGGERED_IGNORE_CASTER_AURASTATE)));
        bot->SetGlyph(realSlot, glyphId, true);
    }

    bot->SendTalentsInfoData(false);
}

// ============================================================
// InitPvpSpec
//
// Applies talents, glyphs, and resets strategies.
// Mirrors the "talents spec <name>" command flow exactly.
// ============================================================
void PvpBotMgr::InitPvpSpec(Player* bot, const PvpSpec* selectedSpec, uint8 primaryTab)
{
    if (!bot || !selectedSpec)
        return;

    uint8 level   = bot->GetLevel();
    uint8 classId = bot->getClass();

    if (selectedSpec->links.empty())
    {
        DebugLog("No talent links configured for spec " + selectedSpec->name);
        return;
    }

    // Reset once up front — all subsequent applications use reset=false so we
    // accumulate points across breakpoints, exactly like InitTalentsBySpecNo does.
    bot->resetTalents(true);

    // Walk every configured breakpoint in ascending level order and apply it.
    // GetFreeTalentPoints() naturally caps spending — a level 70 bot applies the
    // .60 link fully, then applies as many steps of the .80 link as points allow.
    // This mirrors the cumulative behaviour of playerbots' InitTalentsBySpecNo.
    // All links come from our pvpbots config (selectedSpec->links), never playerbots.
    bool anyApplied = false;
    for (const auto& ll : selectedSpec->links)
    {
        if (bot->GetFreeTalentPoints() == 0)
            break;

        std::vector<std::vector<uint32>> parsed =
            PlayerbotAIConfig::ParseTempTalentsOrder(classId, ll.link);

        if (parsed.empty())
        {
            DebugLog("Failed to parse talent link (level " + std::to_string(ll.level) +
                     ") for spec " + selectedSpec->name);
            continue;
        }

        PlayerbotFactory::InitTalentsByParsedSpecLink(bot, parsed, false);
        anyApplied = true;
    }

    if (!anyApplied)
    {
        DebugLog("No talent links applied for spec " + selectedSpec->name +
                 " at level " + std::to_string(level));
        return;
    }

    // Reset strategies so the AI's role/type reflects the new talent tree.
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (botAI)
        botAI->ResetStrategies(false);
}
