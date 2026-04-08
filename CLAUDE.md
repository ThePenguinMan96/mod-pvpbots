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

### Bot login (`OnBotLoginInternal` → `InitPvpBot`)

`OnBotLoginInternal` is now a thin wrapper: sets `PLAYER_FLAGS_NO_XP_GAIN`, calls `InitPvpBot(bot)`, logs zone.

`InitPvpBot` is a single-pass custom init that mirrors `Randomize(false)` but sets the PvP spec **before** the spec-sensitive functions run:

```
Prepare (manual)                           // ResurrectPlayer, CombatStop
factory.ClearEverything()                  // resetTalents, ClearSkills, ClearSpells, ClearInventory
bot->GiveLevel(level) + InitStatsForLevel  // restore level after ClearEverything reset it
InitInstanceQuests / InitAttunementQuests
LearnDefaultSkills / InitSkills
InitClassSpells / InitAvailableSpells      // pass 1 (pre-talent)

// ── PvP spec applied HERE — before gear/enchants/glyphs ──
InitTalentsByParsedSpecLink(parsedSpec)    // apply PvP spec from conf
FillRemainingTalents(bot, primaryTab)      // spend leftover points
botAI->ResetStrategies(false)             // fix strategy to match PvP talent tree

InitAvailableSpells                        // pass 2 (talents unlock new spells)
InitReputation / InitSpecialSpells / InitMounts
factory.InitEquipment(false)              // correct spec weights on first pass
InitBags / InitAmmo / InitFood / InitPotions / InitReagents / InitKeyring
factory.ApplyEnchantAndGemsNew(true)      // destroyOld=true — correct spec on first pass
InitConsumables
factory.InitGlyphs()                      // PvP auras active — aura-based tab detection works

if (level >= 66) EquipPvpGear(bot)        // resilience overlay (no weapons)
else if (_enableCcBreakTrinket) EquipCcBreakTrinket(bot)

Pet cleanup / InitPet / InitPetTalents
SaveToDB
```

**Key design decisions:**
- `ClearAllItems()` (private) is skipped — `InitEquipment(false)` replaces all equipment slots
- `CancelAuras()` (private) is skipped — not needed for fresh/re-logging bots
- `InitGuild()` / `InitArenaTeam()` (private) are skipped — pvpbots don't need these
- `ApplyEnchantAndGemsNew(true)` — `destroyOld=true` wipes and reapplies everything in one pass
- `InitGlyphs()` runs once with correct PvP spec (no typeflags warning from double-call)

---

## Known Remaining Gear Issues and Why

All three known gear issues (wrong-spec gear, wrong glyphs, wrong-spec enchants) are **resolved** by `InitPvpBot()`. The PvP spec is applied before `InitEquipment`, `InitGlyphs`, and `ApplyEnchantAndGemsNew(true)` — all run once with the correct spec.

---

## Key Technical Concepts

- **Strategy vs. Tab:** Two separate things. `GetPlayerSpecTab(player)` always reads the talent tree directly (returns 0/1/2). Strategy is set by `ResetStrategies()` → `AddDefaultCombatStrategies()` which calls `GetPlayerSpecTab()` at that moment. `StatsWeightCalculator` reads strategy for type detection, not the talent tree.
- **`InitEquipment(false)` incremental=false:** Gears all slots from scratch, not just empty ones.
- **`ApplyEnchantAndGemsNew(bool destroyOld)`:** `true` = wipe all existing enchants/gems and reapply. `false` = only fill empty slots, preserve existing.
- **Async vs DirectExecute:** `CharacterDatabase.Execute()` is queued. `DirectExecute()` bypasses the queue and runs immediately, BEFORE prior async writes. Always use async `Execute()` for the pet DELETE so it queues after any prior async INSERTs (e.g. from `InitPet()` or `CreateRandomBot()`).
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
