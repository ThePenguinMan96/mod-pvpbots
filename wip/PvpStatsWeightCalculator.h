/*
 * mod-pvpbots — PvpStatsWeightCalculator.h
 *
 * Standalone PvP stat scorer. Uses StatsCollector directly
 * (same as StatsWeightCalculator does internally) but applies
 * PvP-aware weights.
 *
 * Design principle: PvP stats (resilience, stamina, crit) get
 * additive bonuses ON TOP of each class's normal offensive weights.
 * This means:
 *   - A caster still needs spell power (anchor stat stays)
 *   - A melee still needs str/agi
 *   - BUT gear that ALSO has resilience/stamina scores higher
 *     than pure PvE stat-stick gear of the same slot
 *   - Below level 60 where resilience doesn't appear on gear,
 *     scoring gracefully falls back to normal class weights
 */

#ifndef MOD_PVPBOTS_STATS_WEIGHT_CALCULATOR_H
#define MOD_PVPBOTS_STATS_WEIGHT_CALCULATOR_H

#include "StatsCollector.h"
#include "Player.h"
#include "AiFactory.h"
#include "PlayerbotAI.h"

class PvpStatsWeightCalculator
{
public:
    explicit PvpStatsWeightCalculator(Player* player);

    // Score a single item. Higher = better for this bot's PvP role.
    float CalculateItem(uint32 itemId) const;

private:
    void BuildWeights(Player* player);

    Player*       player_;
    CollectorType type_;
    uint8         cls_;
    int           tab_;
    float         weights_[STATS_TYPE_MAX];
};

#endif // MOD_PVPBOTS_STATS_WEIGHT_CALCULATOR_H
