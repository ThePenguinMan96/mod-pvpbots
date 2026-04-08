/*
 * mod-pvpbots — PvpStatsWeightCalculator.cpp
 */

#include "PvpStatsWeightCalculator.h"
#include "ItemTemplate.h"
#include "ObjectMgr.h"

PvpStatsWeightCalculator::PvpStatsWeightCalculator(Player* player)
    : player_(player)
{
    // Determine collector type the same way StatsWeightCalculator does
    if (PlayerbotAI::IsHeal(player))
        type_ = CollectorType::SPELL_HEAL;
    else if (PlayerbotAI::IsCaster(player))
        type_ = CollectorType::SPELL_DMG;
    else if (PlayerbotAI::IsTank(player))
        type_ = CollectorType::MELEE_TANK;
    else if (PlayerbotAI::IsMelee(player))
        type_ = CollectorType::MELEE_DMG;
    else
        type_ = CollectorType::RANGED;

    cls_ = player->getClass();
    tab_ = AiFactory::GetPlayerSpecTab(player);

    BuildWeights(player);
}

float PvpStatsWeightCalculator::CalculateItem(uint32 itemId) const
{
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
    if (!proto)
        return 0.0f;

    // Collect the item's stats into a temporary collector
    StatsCollector collector(type_, cls_);
    collector.CollectItemStats(proto);

    // Dot product: item_stat_value * our_weight for each stat type
    float score = 0.0f;
    for (uint32 i = 0; i < STATS_TYPE_MAX; ++i)
        score += weights_[i] * collector.stats[i];

    return score;
}

// ============================================================
// BuildWeights
//
// Step 1: Set class/spec PvE base weights (mirrors GenerateBasicWeights)
// Step 2: Add PvP bonuses on top for resilience, stamina, crit
//
// The PvP additive values are chosen so that:
//   - A piece with ONLY resilience still scores reasonably
//   - A piece with resilience + class stat scores best of all
//   - A piece with good class stats but NO resilience is still
//     competitive (we don't ignore PvE gear entirely at low levels)
// ============================================================
void PvpStatsWeightCalculator::BuildWeights(Player* player)
{
    // Zero all weights
    for (uint32 i = 0; i < STATS_TYPE_MAX; ++i)
        weights_[i] = 0.0f;

    // --------------------------------------------------------
    // Step 1: Base class/spec weights (PvE anchors)
    // These are the same values from StatsWeightCalculator
    // so that we don't break offensive stat selection.
    // --------------------------------------------------------
    weights_[STATS_TYPE_ARMOR]     += 0.001f;
    weights_[STATS_TYPE_BONUS]     += 1.0f;
    weights_[STATS_TYPE_MELEE_DPS] += 0.01f;
    weights_[STATS_TYPE_RANGED_DPS]+= 0.01f;

    if (cls_ == CLASS_HUNTER)
    {
        weights_[STATS_TYPE_AGILITY]          += 2.4f;
        weights_[STATS_TYPE_ATTACK_POWER]     += 1.0f;
        weights_[STATS_TYPE_ARMOR_PENETRATION]+= 1.8f;
        weights_[STATS_TYPE_HIT]              += 1.8f;
        weights_[STATS_TYPE_CRIT]             += 1.7f;
        weights_[STATS_TYPE_HASTE]            += 1.5f;
        weights_[STATS_TYPE_RANGED_DPS]       += 8.0f;
    }
    else if (cls_ == CLASS_ROGUE)
    {
        weights_[STATS_TYPE_AGILITY]          += 1.7f;
        weights_[STATS_TYPE_STRENGTH]         += 1.1f;
        weights_[STATS_TYPE_ATTACK_POWER]     += 1.0f;
        weights_[STATS_TYPE_ARMOR_PENETRATION]+= 1.5f;
        weights_[STATS_TYPE_HIT]              += 2.0f;
        weights_[STATS_TYPE_CRIT]             += 1.3f;
        weights_[STATS_TYPE_HASTE]            += 1.7f;
        weights_[STATS_TYPE_EXPERTISE]        += 2.0f;
        weights_[STATS_TYPE_MELEE_DPS]        += 6.0f;
    }
    else if (cls_ == CLASS_WARRIOR)
    {
        weights_[STATS_TYPE_AGILITY]          += 1.7f;
        weights_[STATS_TYPE_STRENGTH]         += 2.4f;
        weights_[STATS_TYPE_ATTACK_POWER]     += 1.0f;
        weights_[STATS_TYPE_ARMOR_PENETRATION]+= 1.9f;
        weights_[STATS_TYPE_HIT]              += 2.1f;
        weights_[STATS_TYPE_CRIT]             += 2.0f;
        weights_[STATS_TYPE_HASTE]            += 1.2f;
        weights_[STATS_TYPE_EXPERTISE]        += 1.8f;
        weights_[STATS_TYPE_MELEE_DPS]        += 7.0f;
    }
    else if (cls_ == CLASS_DEATH_KNIGHT)
    {
        weights_[STATS_TYPE_AGILITY]          += 1.3f;
        weights_[STATS_TYPE_STRENGTH]         += 2.6f;
        weights_[STATS_TYPE_ATTACK_POWER]     += 1.0f;
        weights_[STATS_TYPE_ARMOR_PENETRATION]+= 2.0f;
        weights_[STATS_TYPE_HIT]              += 2.2f;
        weights_[STATS_TYPE_CRIT]             += 1.9f;
        weights_[STATS_TYPE_HASTE]            += 1.8f;
        weights_[STATS_TYPE_EXPERTISE]        += 2.5f;
        weights_[STATS_TYPE_MELEE_DPS]        += 6.0f;
    }
    else if (cls_ == CLASS_PALADIN && tab_ == PALADIN_TAB_RETRIBUTION)
    {
        weights_[STATS_TYPE_STRENGTH]         += 2.5f;
        weights_[STATS_TYPE_AGILITY]          += 1.5f;
        weights_[STATS_TYPE_ATTACK_POWER]     += 1.0f;
        weights_[STATS_TYPE_SPELL_POWER]      += 0.3f;
        weights_[STATS_TYPE_HIT]              += 1.8f;
        weights_[STATS_TYPE_CRIT]             += 1.6f;
        weights_[STATS_TYPE_HASTE]            += 1.5f;
        weights_[STATS_TYPE_EXPERTISE]        += 1.8f;
        weights_[STATS_TYPE_MELEE_DPS]        += 9.0f;
    }
    else if (cls_ == CLASS_SHAMAN && tab_ == SHAMAN_TAB_ENHANCEMENT)
    {
        weights_[STATS_TYPE_AGILITY]          += 1.4f;
        weights_[STATS_TYPE_STRENGTH]         += 1.1f;
        weights_[STATS_TYPE_INTELLECT]        += 0.3f;
        weights_[STATS_TYPE_ATTACK_POWER]     += 1.0f;
        weights_[STATS_TYPE_SPELL_POWER]      += 0.95f;
        weights_[STATS_TYPE_HIT]              += 2.0f;
        weights_[STATS_TYPE_CRIT]             += 1.5f;
        weights_[STATS_TYPE_HASTE]            += 1.7f;
        weights_[STATS_TYPE_EXPERTISE]        += 2.0f;
        weights_[STATS_TYPE_MELEE_DPS]        += 8.0f;
    }
    else if (cls_ == CLASS_DRUID && tab_ == DRUID_TAB_FERAL)
    {
        weights_[STATS_TYPE_AGILITY]          += 2.2f;
        weights_[STATS_TYPE_STRENGTH]         += 2.3f;
        weights_[STATS_TYPE_ATTACK_POWER]     += 1.0f;
        weights_[STATS_TYPE_ARMOR_PENETRATION]+= 2.2f;
        weights_[STATS_TYPE_HIT]              += 1.8f;
        weights_[STATS_TYPE_CRIT]             += 1.5f;
        weights_[STATS_TYPE_HASTE]            += 1.8f;
        weights_[STATS_TYPE_EXPERTISE]        += 2.0f;
        weights_[STATS_TYPE_MELEE_DPS]        += 12.0f;
    }
    else if (cls_ == CLASS_WARLOCK ||
             cls_ == CLASS_MAGE ||
             (cls_ == CLASS_PRIEST  && tab_ == PRIEST_TAB_SHADOW) ||
             (cls_ == CLASS_DRUID   && tab_ == DRUID_TAB_BALANCE)  ||
             (cls_ == CLASS_SHAMAN  && tab_ == SHAMAN_TAB_ELEMENTAL))
    {
        // Caster DPS — spell power is the anchor
        weights_[STATS_TYPE_INTELLECT]        += 0.3f;
        weights_[STATS_TYPE_SPIRIT]           += 0.4f;
        weights_[STATS_TYPE_SPELL_POWER]      += 1.0f;
        weights_[STATS_TYPE_HIT]              += 1.1f;
        weights_[STATS_TYPE_CRIT]             += 0.8f;
        weights_[STATS_TYPE_HASTE]            += 0.9f;
    }
    else if ((cls_ == CLASS_PALADIN  && tab_ == PALADIN_TAB_HOLY) ||
             (cls_ == CLASS_SHAMAN   && tab_ == SHAMAN_TAB_RESTORATION) ||
             (cls_ == CLASS_PRIEST   && tab_ != PRIEST_TAB_SHADOW) ||
             (cls_ == CLASS_DRUID    && tab_ == DRUID_TAB_RESTORATION))
    {
        // Healers
        weights_[STATS_TYPE_INTELLECT]        += 0.9f;
        weights_[STATS_TYPE_SPIRIT]           += 0.4f;
        weights_[STATS_TYPE_HEAL_POWER]       += 1.0f;
        weights_[STATS_TYPE_MANA_REGENERATION]+= 0.7f;
        weights_[STATS_TYPE_CRIT]             += 0.6f;
        weights_[STATS_TYPE_HASTE]            += 0.7f;
    }
    else
    {
        // Fallback — treat as generic melee
        weights_[STATS_TYPE_STRENGTH]         += 2.0f;
        weights_[STATS_TYPE_AGILITY]          += 1.5f;
        weights_[STATS_TYPE_ATTACK_POWER]     += 1.0f;
        weights_[STATS_TYPE_HIT]              += 1.5f;
        weights_[STATS_TYPE_CRIT]             += 1.5f;
        weights_[STATS_TYPE_MELEE_DPS]        += 5.0f;
    }

    // --------------------------------------------------------
    // Step 2: PvP additive bonuses
    //
    // These are added ON TOP of whatever Step 1 set.
    // Tuning rationale:
    //
    //   RESILIENCE +2.0  — Was completely 0. Now scores meaningfully
    //                      but a caster piece with spell_power=1.0 and
    //                      resilience=2.0 beats a piece with only
    //                      resilience. PvP gear that has both wins.
    //
    //   STAMINA    +1.0  — Was 0.1. Now 1.1 total. Survivability
    //                      matters in PvP but not at the cost of
    //                      ignoring a caster's 1.0 spell_power anchor.
    //
    //   CRIT       +0.5  — Small bonus. Burst is valuable in PvP
    //                      but crit was already weighted per class.
    //                      This nudges toward crit where equal.
    // --------------------------------------------------------
    weights_[STATS_TYPE_RESILIENCE] += 2.0f;
    weights_[STATS_TYPE_STAMINA]    += 1.0f;
    weights_[STATS_TYPE_CRIT]       += 0.5f;
}
