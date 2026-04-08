# mod-pvpbots — Claude Code Context

## Canonical Paths

- **Module root (always use this):** `Z:\azerothcore-wotlk\modules\mod-pvpbots`
- **Server build root:** `Z:\azerothcore-wotlk`
- **NEVER use:** `Z:\WoW WOTLK Server\Server\azerothcore-wotlk` — this is an old path, ignore it entirely.

---

## What This Module Is

`mod-pvpbots` is an AzerothCore WotLK (3.3.5a) module that creates dedicated PvP-only bots layered on top of `mod-playerbots`. It is **not a fork** — it depends on `mod-playerbots` as its engine and adds a behavioral layer on top.

**Key architectural decision:** `PvpBotMgr` inherits `PlayerbotHolder` directly. Bots live in our own `playerBots` map and `sRandomPlayerbotMgr` never touches them. `UpdateSessions()` runs every world tick.

---

## Source Files

```
src/PvpBotMgr.h                        — Manager singleton, structs, config members
src/PvpBotMgr.cpp                      — All logic (creation, login, gearing, equip)
src/mod_pvpbots_loader.cpp             — WorldScript + PlayerScript entry point
conf/mod_pvpbots.conf.dist             — Full config file with all options
data/sql/db-world/pvpbots_registry.sql — Bot account/char tracking table
```

---

## How Bots Are Created and Geared (Current State)

### Character creation (`CreatePvpBot`)
1. `RandomPlayerbotFactory::CreateRandomBot()` — creates the account and character
2. `GiveLevel(level)` + `SaveToDB()`
3. Async `CharacterDatabase.Execute("DELETE FROM character_pet ...")` for non-pet classes — queued AFTER `SaveToDB`'s async writes so it fires last, preventing phantom pet rows

### Bot login (`OnBotLoginInternal`)
```
factory.Randomize(false)                   // Full PvE init: gear, spells, enchants, gems, glyphs
Pet cleanup (async Execute DELETE)         // Non-pet classes only; must be async not DirectExecute
InitTalentsByParsedSpecLink(parsedSpec)    // Apply PvP spec from conf
FillRemainingTalents(bot, primaryTab)      // Spend leftover points in primary tree
factory.InitEquipment(false)              // Re-gear for PvP spec (Randomize used wrong spec)
if (level >= 66) EquipPvpGear(bot)        // Replace with best resilience items (no weapons)
else if (_enableCcBreakTrinket) EquipCcBreakTrinket(bot)
factory.ApplyEnchantAndGemsNew(false)     // Re-enchant/gem new items
```

---

## Known Remaining Gear Issues and Why

### 1. Feral Druid in caster gear / Enhancement Shaman in spellpower gear

**Root cause:** `StatsWeightCalculator` reads the active **strategy** for type detection (SPELL_DMG, MELEE_DMG, etc.), not the talent tree. `Randomize()` set the strategy for a random PvE spec. Even though we apply the PvP spec and call `InitEquipment(false)`, the strategy is still from Randomize's random spec — so the type bucket is wrong.

**Fix needed:** After `FillRemainingTalents`, call:
```cpp
if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
    botAI->ResetStrategies(false);
```
`ResetStrategies(false)` calls `AiFactory::AddDefaultCombatStrategies()` which reads `GetPlayerSpecTab(player)` (talent tree). After PvP spec is applied, this recalculates the strategy correctly. Then `InitEquipment(false)` uses the right type bucket.

`GET_PLAYERBOT_AI` is defined in `mod-playerbots/src/Script/Playerbots.h` (not `PlayerbotAI.h`). Add `#include "Playerbots.h"` when implementing.

### 2. Glyphs assigned for wrong spec

**Root cause:** `Randomize()` called `InitGlyphs()` for its random PvE spec. Our PvP spec is applied after, but `InitGlyphs()` is not re-called (currently blocked by a typeflags warning: "has glyph with typeflags 0 in slot with typeflags 1" if called twice).

**Background:** The user's PR to `mod-playerbots` added aura-based PvP spec detection inside `InitGlyphs()` (tabs 3/4/5 are PvP-spec-only paths). `GetPlayerSpecTab()` only returns 0/1/2 — tabs 3-5 only exist inside `InitGlyphs()`'s self-contained aura detection. This fix correctly applies PvP glyphs when called after the PvP spec auras are active.

**Fix needed:** Investigate the typeflags warning to determine if it's safe to call `InitGlyphs(false)` after `ResetStrategies`. If calling with `increment=false` clears existing glyphs first, the warning may not apply.

### 3. Wrong-spec enchants from Randomize not wiped

**Root cause:** `ApplyEnchantAndGemsNew(false)` (destroyOld=false) preserves existing enchants on all slots. Slots replaced by `EquipPvpGear` get fresh enchants; all other slots keep Randomize's wrong-spec enchants.

**Fix needed:** Change to `ApplyEnchantAndGemsNew(true)` and move the call to AFTER `EquipPvpGear` so it wipes everything and re-applies correctly for the PvP spec. This is the final step.

---

## Correct Initialization Order (Target State)

```cpp
factory.Randomize(false)
// Pet cleanup (async Execute) for non-pet classes
PlayerbotFactory::InitTalentsByParsedSpecLink(bot, parsedSpec, true)
FillRemainingTalents(bot, primaryTab)
// ADD: botAI->ResetStrategies(false)       — fixes strategy/type detection
// ADD: factory.InitEquipment(false)         — re-gear with correct strategy
// INVESTIGATE: factory.InitGlyphs(false)   — PvP glyphs (needs typeflags check)
if (level >= 66) EquipPvpGear(bot)
else if (_enableCcBreakTrinket) EquipCcBreakTrinket(bot)
// CHANGE: factory.ApplyEnchantAndGemsNew(true) — wipe all, reapply for PvP spec
```

---

## Key Technical Concepts

- **Strategy vs. Tab:** Two separate things. `GetPlayerSpecTab(player)` always reads the talent tree directly (returns 0/1/2). Strategy is set by `ResetStrategies()` → `AddDefaultCombatStrategies()` which calls `GetPlayerSpecTab()` at that moment. `StatsWeightCalculator` reads strategy for type detection, not the talent tree.
- **`InitEquipment(false)` incremental=false:** Gears all slots from scratch, not just empty ones.
- **`ApplyEnchantAndGemsNew(bool destroyOld)`:** `true` = wipe all existing enchants/gems and reapply. `false` = only fill empty slots, preserve existing.
- **Async vs DirectExecute:** `CharacterDatabase.Execute()` is queued. `DirectExecute()` bypasses the queue and runs immediately, BEFORE prior async writes. Always use async `Execute()` for the pet DELETE so it queues after Randomize's async INSERT.
- **`FillRemainingTalents` overflow risk:** A bot at level 79 using a 60-point link will have 7 leftover points. `FillRemainingTalents` distributes them in the primary tree row by row. Watch for edge cases where more points are available than a tree can absorb.

---

## EquipPvpGear Design

- **`_pvpItemCache`:** All items with resilience (stat_type=35) from `item_template`, indexed by `InventoryType`, sorted by `itemLevel` desc. Built once at startup by `BuildPvpItemCache()`.
- **`_ccBreakTrinketCache`:** Trinkets whose on-use spell is spell 42292 (CC-break / Every Man for Himself equivalent). Sorted by itemLevel desc.
- **Scoring:** `StatsWeightCalculator::CalculateItem(itemId)` + optional resilience bonus from `_pvpResilienceWeight`. Same scorer playerbots uses — no custom formulas.
- **Weapons excluded:** `EquipPvpGear` deliberately skips MAINHAND/OFFHAND/RANGED. `InitEquipment()` owns weapon selection via `CanEquipWeapon()` and `CalculateItemTypePenalty()`. Adding a resilience bonus there distorts penalty math (e.g. a high-resilience 1H outscoring a 2H for an Arms Warrior).
- **TRINKET1:** Always the best CC-break trinket the bot qualifies for (level check), or the heirloom fallback if none found.
- **TRINKET2:** Left as `Randomize()` chose it (scored by `_allTrinketCache` via `StatsWeightCalculator`).
- **Heirloom fallback:** Item 44098 (Alliance) / 44097 (Horde) — scale with level, no level req. Controlled by `PvpBots.EnableHeirloomCcTrinket`.

---

## Standing Rules

- **Do NOT touch `mod-playerbots`.** All our work stays in `mod-pvpbots`. Use only public interfaces from playerbots.
- **PvP specs stay in the pvpbots config**, not the playerbots config. Specs are parsed from talent link strings in `mod_pvpbots.conf.dist`.
- **Do not add resilience score bonuses to weapon slots.** Let `CalculateItemTypePenalty()` do its job.
- **Always use async `CharacterDatabase.Execute()`** for our DB writes that must queue after Randomize's writes. Never `DirectExecute` for these.
- Do not use crude statFlavor/GetRoleFlavor filtering — `StatsWeightCalculator` already handles all per-spec scoring correctly.

---

## What Is Not Yet Implemented

- PvP strategy files (all class/role strategies are stubs or empty)
- BG/Arena queue integration
- World behavior hooks (zone detection, ganker positioning, world PvP objective capture)
- CC diminishing returns tracking
- Bot-to-bot communication for cooldowns/CC chains
- Wand slot (INVTYPE_RANGED currently excluded from PvP cache — separate issue)

The user's stated priority: **get gear perfect first**, then move to strategies, duels, battlegrounds, arenas, and world PvP.

---

## User Profile

- **GitHub:** ThePenguinMan96
- **Experience:** C++ beginner; has contributed PRs to `mod-playerbots` (mage/warlock/hunter/shaman strategies, glyph PvP spec detection with aura-based tab 3/4/5 system)
- **Preference:** Explain reasoning, not just changes. Keep solutions scalable and clean. Don't make changes without being asked.
