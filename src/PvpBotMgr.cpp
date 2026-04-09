/*
 * mod-pvpbots — PvpBotMgr.cpp
 *
 * LoginBot() bypasses PlayerbotHolder::AddPlayerBot entirely, cutting
 * RandomPlayerbotMgr out of our bot lifecycle. Bots are loaded via
 * AzerothCore's LoginQueryHolder + HandlePlayerLoginFromDB, the PlayerbotAI
 * is created via PlayerbotsMgr::AddPlayerbotData, and the bot is inserted
 * directly into our playerBots map. sRandomPlayerbotMgr is never called.
 */

#include "PvpBotMgr.h"

#include "AccountMgr.h"
#include "CharacterCache.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "Log.h"
#include "SpellMgr.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Pet.h"
#include "Player.h"
#include "World.h"
#include "WorldSession.h"

// mod-playerbots
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotFactory.h"
#include "PlayerbotMgr.h"
#include "Playerbots.h"             // GET_PLAYERBOT_AI
#include "RandomPlayerbotFactory.h"
#include "StatsWeightCalculator.h"

#include <algorithm>
#include <set>
#include <sstream>
#include <thread>
#include <chrono>

// ============================================================
// Singleton
// ============================================================
PvpBotMgr* PvpBotMgr::instance()
{
    static PvpBotMgr instance;
    return &instance;
}

// ============================================================
// Initialize
// ============================================================
void PvpBotMgr::Initialize()
{
    LoadConfig();

    if (!_enabled)
    {
        LOG_INFO("playerbots", "[mod-pvpbots] Disabled in config.");
        return;
    }

    if (_deleteBots)
    {
        DeleteAllPvpBots();
        return;
    }

    LOG_INFO("playerbots", "[mod-pvpbots] Starting. Total bots: {}", _totalCount);

    BuildPvpItemCache();
    EnsureSchema();
    LoadFromDB();
    CleanupInvalidPets();
    CleanupDkStartingArea();
    EnsureBotCount();

    if (_disabledWithoutRealPlayer)
    {
        LOG_INFO("playerbots",
            "[mod-pvpbots] DisabledWithoutRealPlayer = 1. "
            "Bots will log in {} seconds after the first real player connects.",
            _loginDelay);
    }
    else
    {
        LoginAllBots();
        _botsLoggedIn = true;
    }
}

// ============================================================
// Update — called every world tick
// ============================================================
void PvpBotMgr::Update(uint32 diff)
{
    if (!_enabled)
        return;

    if (_botsLoggedIn)
        UpdateSessions();

    if (!_disabledWithoutRealPlayer)
        return;

    if (!_botsLoggedIn && _loginDelayTimer > 0)
    {
        if (_loginDelayTimer <= diff)
        {
            _loginDelayTimer = 0;
            LoginAllBots();
            _botsLoggedIn = true;
            LOG_INFO("playerbots", "[mod-pvpbots] Login delay elapsed. {} bots now online.",
                GetOnlineBotCount());
        }
        else
        {
            _loginDelayTimer -= diff;
        }
    }
    else if (_botsLoggedIn && _logoutDelayTimer > 0)
    {
        if (_logoutDelayTimer <= diff)
        {
            _logoutDelayTimer = 0;
            LogoutAllBots();
            _botsLoggedIn = false;
            LOG_INFO("playerbots", "[mod-pvpbots] Logout delay elapsed. All pvpbots offline.");
        }
        else
        {
            _logoutDelayTimer -= diff;
        }
    }
}

// ============================================================
// Shutdown
// ============================================================
void PvpBotMgr::Shutdown()
{
    if (!_enabled)
        return;

    LOG_INFO("playerbots", "[mod-pvpbots] Shutting down — logging out {} bots.",
        GetOnlineBotCount());

    LogoutAllBots();
    _botsLoggedIn = false;
    _bots.clear();
}

// ============================================================
// NotifyRealPlayerLogin
// ============================================================
void PvpBotMgr::NotifyRealPlayerLogin()
{
    ++_realPlayerCount;

    if (!_disabledWithoutRealPlayer)
        return;

    if (_botsLoggedIn)
        return;

    _logoutDelayTimer = 0;

    if (_loginDelayTimer == 0)
    {
        _loginDelayTimer = _loginDelay * 1000u;
        DebugLog("Real player logged in. Login countdown started (" +
                 std::to_string(_loginDelay) + "s).");
    }
}

// ============================================================
// NotifyRealPlayerLogout
// ============================================================
void PvpBotMgr::NotifyRealPlayerLogout()
{
    if (_realPlayerCount > 0)
        --_realPlayerCount;

    if (!_disabledWithoutRealPlayer)
        return;

    if (!_botsLoggedIn)
        return;

    if (_realPlayerCount > 0)
        return;

    _loginDelayTimer = 0;

    if (_logoutDelayTimer == 0)
    {
        _logoutDelayTimer = _logoutDelay * 1000u;
        DebugLog("Last real player logged out. Logout countdown started (" +
                 std::to_string(_logoutDelay) + "s).");
    }
}

// ============================================================
// OnBotLoginInternal (PlayerbotHolder pure virtual)
// ============================================================
void PvpBotMgr::OnBotLoginInternal(Player* bot)
{
    if (!bot)
        return;

    bot->SetPlayerFlag(PLAYER_FLAGS_NO_XP_GAIN);
    InitPvpBot(bot);

    std::string zoneName = "Unknown";
    if (AreaTableEntry const* area = sAreaTableStore.LookupEntry(bot->GetZoneId()))
        if (const char* name = area->area_name[sWorld->GetDefaultDbcLocale()])
            zoneName = name;

    LOG_INFO("playerbots",
        "[mod-pvpbots] pvpbot '{}' | level {} {} | {}",
        bot->GetName(), bot->GetLevel(),
        GetClassName(bot->getClass()),
        zoneName
    );
}


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
    factory.InitEquipment(false);
    factory.InitBags();
    factory.InitAmmo();
    factory.InitFood();
    factory.InitPotions();
    factory.InitReagents();
    factory.InitKeyring();
    factory.InitConsumables();

    if (level >= 66)
        EquipPvpGear(bot);
    else if (_enableCcBreakTrinket)
        EquipCcBreakTrinket(bot);

    if (level >= sPlayerbotAIConfig.minEnchantingBotLevel)
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
// LoadConfig
// ============================================================
void PvpBotMgr::LoadConfig()
{
    _enabled          = sConfigMgr->GetOption<bool>       ("PvpBots.Enable",                               false);
    _deleteBots       = sConfigMgr->GetOption<bool>       ("PvpBots.DeletePvpBotAccounts",                 false);
    _accountPrefix    = sConfigMgr->GetOption<std::string>("PvpBots.AccountPrefix",                        "pvpbot");
    _totalCount       = sConfigMgr->GetOption<uint32>     ("PvpBots.TotalCount",                           200);
    _factionMode      = sConfigMgr->GetOption<uint8>      ("PvpBots.FactionMode",                          2);
    _hordePercent     = sConfigMgr->GetOption<uint32>     ("PvpBots.HordePercent",                         50);
    _alliancePercent  = sConfigMgr->GetOption<uint32>     ("PvpBots.AlliancePercent",                      50);
    _disabledWithoutRealPlayer = sConfigMgr->GetOption<bool>("PvpBots.DisabledWithoutRealPlayer",          false);
    _loginDelay       = sConfigMgr->GetOption<uint32>     ("PvpBots.DisabledWithoutRealPlayerLoginDelay",  30);
    _logoutDelay      = sConfigMgr->GetOption<uint32>     ("PvpBots.DisabledWithoutRealPlayerLogoutDelay", 300);
    _debug            = sConfigMgr->GetOption<bool>       ("PvpBots.Debug",                                false);
    _gearQualityLimit     = sConfigMgr->GetOption<uint32>     ("PvpBots.GearQualityLimit",       4);
    _gearScoreLimit       = sConfigMgr->GetOption<uint32>     ("PvpBots.GearScoreLimit",         0);
    _preferResilienceGear     = sConfigMgr->GetOption<bool>("PvpBots.PreferResilienceGear",   true);
    _enableCcBreakTrinket     = sConfigMgr->GetOption<bool>("PvpBots.EnableCcBreakTrinket",   true);
    _enableHeirloomCcTrinket  = sConfigMgr->GetOption<bool>("PvpBots.EnableHeirloomCcTrinket", true);

    {
        static const uint8  classes[]     = {1, 2, 3, 4, 5, 6, 7, 8, 9, 11};
        static const uint8  checkLevels[] = {40, 60, 65, 70, 75, 80};
        static const uint32 kMaxSpecNo    = 3; 

        _pvpSpecs.clear();

        for (uint8 cls : classes)
        {
            for (uint32 specNo = 0; specNo < kMaxSpecNo; ++specNo)
            {
                std::string prefix = "PvpBots.Spec";
                std::string clsSpec = std::to_string(cls) + "." + std::to_string(specNo);

                std::string name = sConfigMgr->GetOption<std::string>(
                    prefix + "Name." + clsSpec, "");
                if (name.empty())
                    break; 

                PvpSpec spec;
                spec.name   = name;
                spec.glyphs = sConfigMgr->GetOption<std::string>(
                    prefix + "Glyph." + clsSpec, "");

                for (uint8 lvl : checkLevels)
                {
                    std::string link = sConfigMgr->GetOption<std::string>(
                        prefix + "Link." + clsSpec + "." + std::to_string(lvl), "", false);
                    if (!link.empty())
                        spec.links.push_back({lvl, link});
                }

                _pvpSpecs[cls].push_back(std::move(spec));
            }

            if (!_pvpSpecs[cls].empty())
                DebugLog("Class " + std::to_string(cls) + ": loaded " +
                         std::to_string(_pvpSpecs[cls].size()) + " PvP spec(s).");
        }
    }

    // Level brackets — 10 fixed ranges, each configured by percentage.
    struct BracketDef { uint8 minLevel; uint8 maxLevel; const char* cfgKey; };
    static const BracketDef kDefs[] = {
        {10, 19, "PvpBots.Level10to19Percent"},
        {20, 29, "PvpBots.Level20to29Percent"},
        {30, 39, "PvpBots.Level30to39Percent"},
        {40, 49, "PvpBots.Level40to49Percent"},
        {50, 59, "PvpBots.Level50to59Percent"},
        {60, 60, "PvpBots.Level60Percent"    },
        {61, 69, "PvpBots.Level61to69Percent"},
        {70, 70, "PvpBots.Level70Percent"    },
        {71, 79, "PvpBots.Level71to79Percent"},
        {80, 80, "PvpBots.Level80Percent"    },
    };

    _levelBrackets.clear();
    uint32 pctSum = 0;
    for (const auto& d : kDefs)
    {
        PvpBotLevelBracket b;
        b.minLevel = d.minLevel;
        b.maxLevel = d.maxLevel;
        b.cfgKey   = d.cfgKey;
        b.percent  = sConfigMgr->GetOption<uint32>(d.cfgKey, 10);
        b.count    = (_totalCount * b.percent) / 100;
        pctSum    += b.percent;
        _levelBrackets.push_back(b);
    }

    if (pctSum != 100)
    {
        LOG_ERROR("playerbots",
            "[mod-pvpbots] Level bracket percents sum to {} (must be 100). "
            "Module disabled.", pctSum);
        _enabled = false;
        return;
    }

    // Distribute integer-division remainder to the last bracket with percent > 0.
    uint32 allocated = 0;
    for (const auto& b : _levelBrackets)
        allocated += b.count;

    uint32 remainder = _totalCount - allocated;
    if (remainder > 0)
    {
        for (int i = static_cast<int>(_levelBrackets.size()) - 1; i >= 0; --i)
        {
            if (_levelBrackets[i].percent > 0)
            {
                _levelBrackets[i].count += remainder;
                break;
            }
        }
    }

    DebugLog("Config loaded.");
}

// ============================================================
// EnsureSchema
// Creates pvpbots_registry if it doesn't already exist.
// Runs on every startup — safe to call repeatedly (IF NOT EXISTS).
// This means zero manual SQL steps for the server operator.
// ============================================================

void PvpBotMgr::EnsureSchema()
{
    WorldDatabase.DirectExecute(
        "CREATE TABLE IF NOT EXISTS `pvpbots_registry` ("
        "  `id`           INT UNSIGNED     NOT NULL AUTO_INCREMENT,"
        "  `account_id`   INT UNSIGNED     NOT NULL,"
        "  `char_guid`    INT UNSIGNED     NOT NULL,"
        "  `char_name`    VARCHAR(12)      NOT NULL,"
        "  `faction`      TINYINT UNSIGNED NOT NULL,"
        "  `class`        TINYINT UNSIGNED NOT NULL,"
        "  `level`        TINYINT UNSIGNED NOT NULL DEFAULT 80,"
        "  `state`        TINYINT UNSIGNED NOT NULL DEFAULT 0,"
        "  `last_updated` TIMESTAMP        NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,"
        "  PRIMARY KEY (`id`),"
        "  UNIQUE KEY `uq_char_guid` (`char_guid`),"
        "  KEY `idx_account_id` (`account_id`)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4"
    );

    // Migrate: drop obsolete 'role' column if present.
    QueryResult roleColCheck = WorldDatabase.Query(
        "SELECT 1 FROM information_schema.COLUMNS "
        "WHERE TABLE_SCHEMA = DATABASE() "
        "AND TABLE_NAME = 'pvpbots_registry' "
        "AND COLUMN_NAME = 'role'"
    );
    if (roleColCheck)
    {
        WorldDatabase.DirectExecute("ALTER TABLE `pvpbots_registry` DROP COLUMN `role`");
        LOG_INFO("playerbots", "[mod-pvpbots] Migrated pvpbots_registry: dropped 'role' column.");
    }

    // Migrate: add 'level' column to existing tables that predate this schema version.
    QueryResult colCheck = WorldDatabase.Query(
        "SELECT 1 FROM information_schema.COLUMNS "
        "WHERE TABLE_SCHEMA = DATABASE() "
        "AND TABLE_NAME = 'pvpbots_registry' "
        "AND COLUMN_NAME = 'level'"
    );
    if (!colCheck)
    {
        WorldDatabase.DirectExecute(
            "ALTER TABLE `pvpbots_registry` "
            "ADD COLUMN `level` TINYINT UNSIGNED NOT NULL DEFAULT 80 AFTER `class`"
        );
        LOG_INFO("playerbots", "[mod-pvpbots] Migrated pvpbots_registry: added 'level' column.");
    }

    DebugLog("Schema verified.");
}

// ============================================================
// LoadFromDB
// ============================================================
void PvpBotMgr::LoadFromDB()
{
    _bots.clear();

    QueryResult result = WorldDatabase.Query(
        "SELECT account_id, char_guid, char_name, faction, class, level, state "
        "FROM pvpbots_registry ORDER BY id ASC"
    );

    if (!result)
    {
        DebugLog("No existing pvpbots found in DB.");
        return;
    }

    do
    {
        Field* fields = result->Fetch();

        PvpBotEntry entry;
        entry.accountId = fields[0].Get<uint32>();
        entry.charGuid  = ObjectGuid::Create<HighGuid::Player>(fields[1].Get<uint32>());
        entry.charName  = fields[2].Get<std::string>();
        entry.faction   = fields[3].Get<uint8>();
        entry.classId   = fields[4].Get<uint8>();
        entry.level     = fields[5].Get<uint8>();
        entry.state     = static_cast<PvpBotState>(fields[6].Get<uint8>());

        _bots.push_back(entry);

    } while (result->NextRow());

    LOG_INFO("playerbots", "[mod-pvpbots] Loaded {} existing pvpbots from DB.", _bots.size());
}

// ============================================================
// EnsureBotCount
// ============================================================
void PvpBotMgr::EnsureBotCount()
{
    uint32 current = static_cast<uint32>(_bots.size());

    if (current >= _totalCount)
    {
        DebugLog("Bot count at or above target (" + std::to_string(current) + ").");
        return;
    }

    uint32 needed = _totalCount - current;
    LOG_INFO("playerbots", "[mod-pvpbots] Creating {} new pvpbot(s)...", needed);

    std::unordered_map<RandomPlayerbotFactory::NameRaceAndGender, std::vector<std::string>> nameCache;
    QueryResult nameResult = CharacterDatabase.Query("SELECT name, gender FROM playerbots_names");

    if (nameResult)
    {
        do
        {
            Field* fields = nameResult->Fetch();
            std::string name = fields[0].Get<std::string>();
            auto key = static_cast<RandomPlayerbotFactory::NameRaceAndGender>(fields[1].Get<uint8>());

            if (ObjectMgr::CheckPlayerName(name) == CHAR_NAME_SUCCESS)
            {
                CharacterDatabasePreparedStatement* stmt =
                    CharacterDatabase.GetPreparedStatement(CHAR_SEL_CHECK_NAME);
                stmt->SetData(0, name);
                if (!CharacterDatabase.Query(stmt))
                    nameCache[key].push_back(name);
            }
        } while (nameResult->NextRow());
    }

    for (uint32 i = 0; i < needed; ++i)
    {
        uint32 globalIndex = current + i;
        uint8 botLevel = GetLevelForIndex(globalIndex);
        CreatePvpBot(
            GetFactionForIndex(globalIndex),
            GetRandomPvpClass(botLevel),
            botLevel,
            globalIndex,
            nameCache
        );
    }

    if (needed > 0)
    {
        LOG_INFO("playerbots", "[mod-pvpbots] Waiting for character DB writes...");
        while (CharacterDatabase.QueueSize())
            std::this_thread::sleep_for(std::chrono::seconds(1));
        LOG_INFO("playerbots", "[mod-pvpbots] Character creation complete.");
    }
}

// ============================================================
// LoginAllBots
// ============================================================
void PvpBotMgr::LoginAllBots()
{
    LOG_INFO("playerbots", "[mod-pvpbots] Logging in {} pvpbots...", _bots.size());

    for (const auto& entry : _bots)
        LoginBot(entry.charGuid);
}

// ============================================================
// LoginBot
//
// Bypasses PlayerbotHolder::AddPlayerBot entirely so that
// sRandomPlayerbotMgr is never involved in our bot lifecycle.
//
// Uses AzerothCore's LoginQueryHolder directly, submits the async
// character DB load, then in the callback creates the WorldSession,
// initializes the PlayerbotAI, and inserts the bot into our own
// playerBots map.
// ============================================================
void PvpBotMgr::LoginBot(ObjectGuid guid)
{
    // Already loading or already in world
    if (_botLoading.count(guid))
        return;
    if (ObjectAccessor::FindConnectedPlayer(guid))
        return;

    uint32 accountId = sCharacterCache->GetCharacterAccountIdByGuid(guid);
    if (!accountId)
    {
        LOG_ERROR("playerbots", "[mod-pvpbots] LoginBot: no account for guid {}", guid.GetCounter());
        return;
    }

    auto holder = std::make_shared<LoginQueryHolder>(accountId, guid);
    if (!holder->Initialize())
    {
        LOG_ERROR("playerbots", "[mod-pvpbots] LoginBot: LoginQueryHolder init failed for guid {}", guid.GetCounter());
        return;
    }

    _botLoading.insert(guid);

    sWorld->AddQueryHolderCallback(CharacterDatabase.DelayQueryHolder(holder))
        .AfterComplete([this](SQLQueryHolderBase const& base)
        {
            LoginQueryHolder const& h = static_cast<LoginQueryHolder const&>(base);
            ObjectGuid guid           = h.GetGuid();
            uint32     accountId      = h.GetAccountId();

            _botLoading.erase(guid);

            WorldSession* session = new WorldSession(
                accountId, "", 0x0, nullptr,
                SEC_PLAYER, EXPANSION_WRATH_OF_THE_LICH_KING,
                time_t(0), sWorld->GetDefaultDbcLocale(),
                0, false, false, 0, true
            );

            session->HandlePlayerLoginFromDB(h);

            Player* bot = session->GetPlayer();
            if (!bot)
            {
                LOG_ERROR("playerbots",
                    "[mod-pvpbots] LoginBot: player failed to load for account {}", accountId);
                session->LogoutPlayer(true);
                delete session;
                return;
            }

            PlayerbotsMgr::instance().AddPlayerbotData(bot, true);
            playerBots[guid] = bot;
            OnBotLoginInternal(bot);
        });
}

// ============================================================
// CreatePvpBot
// ============================================================
bool PvpBotMgr::CreatePvpBot(
    uint8 faction,
    uint8 classId,
    uint8 level,
    uint32 index,
    std::unordered_map<RandomPlayerbotFactory::NameRaceAndGender,
                       std::vector<std::string>>& nameCache)
{
    std::string accountName = _accountPrefix + std::to_string(index);
    std::string password    = accountName;

    uint32 accountId = AccountMgr::GetId(accountName);
    if (!accountId)
    {
        AccountMgr::CreateAccount(accountName, password);

        while (LoginDatabase.QueueSize())
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

        accountId = AccountMgr::GetId(accountName);
        if (!accountId)
        {
            LOG_ERROR("playerbots", "[mod-pvpbots] Failed to create account '{}'", accountName);
            return false;
        }
        DebugLog("Created account: " + accountName);
    }

    WorldSession* session = new WorldSession(
        accountId, "", 0x0, nullptr,
        SEC_PLAYER, EXPANSION_WRATH_OF_THE_LICH_KING,
        time_t(0), LOCALE_enUS, 0, false, false, 0, true
    );

    RandomPlayerbotFactory factory;
    Player* playerBot = factory.CreateRandomBot(session, classId, nameCache);

    if (!playerBot)
    {
        LOG_ERROR("playerbots",
            "[mod-pvpbots] Character creation failed for account {} class {}",
            accountId, classId);
        delete session;
        return false;
    }

    playerBot->GiveLevel(level);
    playerBot->SaveToDB(true, false);

    // SaveToDB uses async DB writes (CommitTransaction). Queue our DELETE
    // after it so the character_pet rows written by CreateRandomBot/GiveLevel
    // are removed before the bot ever logs in.  Non-pet classes only.
    if (classId != CLASS_HUNTER && classId != CLASS_WARLOCK && classId != CLASS_DEATH_KNIGHT)
        CharacterDatabase.Execute(
            "DELETE FROM character_pet WHERE owner = {}",
            playerBot->GetGUID().GetCounter()
        );

    uint32      charGuid      = playerBot->GetGUID().GetCounter();
    std::string charName      = playerBot->GetName();
    uint8       charRace      = playerBot->getRace();
    uint8       actualFaction = IsAlliance(charRace) ? 1 : 0;

    sCharacterCache->AddCharacterCacheEntry(
        playerBot->GetGUID(), accountId, charName,
        playerBot->getGender(), charRace,
        playerBot->getClass(), playerBot->GetLevel()
    );

    playerBot->CleanupsBeforeDelete();
    delete playerBot;
    delete session;

    WorldDatabase.Execute(
        "INSERT INTO pvpbots_registry "
        "(account_id, char_guid, char_name, faction, class, level, state) "
        "VALUES ({}, {}, '{}', {}, {}, {}, {})",
        accountId, charGuid, charName,
        static_cast<uint32>(actualFaction),
        static_cast<uint32>(classId),
        static_cast<uint32>(level),
        static_cast<uint32>(PVPBOT_STATE_IDLE)
    );

    PvpBotEntry entry;
    entry.accountId = accountId;
    entry.charGuid  = ObjectGuid::Create<HighGuid::Player>(charGuid);
    entry.charName  = charName;
    entry.faction   = actualFaction;
    entry.classId   = classId;
    entry.level     = level;
    entry.state     = PVPBOT_STATE_IDLE;
    _bots.push_back(entry);

    LOG_INFO("playerbots",
        "[mod-pvpbots] Created: {} | Level {} | {} | Faction: {}",
        charName, level, GetClassName(classId),
        actualFaction == 0 ? "Horde" : "Alliance"
    );

    return true;
}

// ============================================================
// DeleteAllPvpBots
// ============================================================
void PvpBotMgr::DeleteAllPvpBots()
{
    LOG_INFO("playerbots", "[mod-pvpbots] DeletePvpBotAccounts = 1. Starting deletion...");

    if (_botsLoggedIn)
    {
        LogoutAllBots();
        _botsLoggedIn = false;
    }

    std::string loginDB = LoginDatabase.GetConnectionInfo()->database;

    CharacterDatabase.Execute(
        "DELETE FROM characters WHERE account IN "
        "(SELECT id FROM {}.account WHERE username LIKE '{}%')",
        loginDB, _accountPrefix
    );

    while (CharacterDatabase.QueueSize())
        std::this_thread::sleep_for(std::chrono::seconds(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    CharacterDatabase.Execute("DELETE FROM character_inventory  WHERE guid NOT IN (SELECT guid FROM characters)");
    CharacterDatabase.Execute("DELETE FROM item_instance        WHERE owner_guid NOT IN (SELECT guid FROM characters) AND owner_guid > 0");
    CharacterDatabase.Execute("DELETE FROM character_spell      WHERE guid NOT IN (SELECT guid FROM characters)");
    CharacterDatabase.Execute("DELETE FROM character_talent     WHERE guid NOT IN (SELECT guid FROM characters)");
    CharacterDatabase.Execute("DELETE FROM character_skills     WHERE guid NOT IN (SELECT guid FROM characters)");
    CharacterDatabase.Execute("DELETE FROM character_aura       WHERE guid NOT IN (SELECT guid FROM characters)");
    CharacterDatabase.Execute("DELETE FROM character_action     WHERE guid NOT IN (SELECT guid FROM characters)");
    CharacterDatabase.Execute("DELETE FROM character_reputation WHERE guid NOT IN (SELECT guid FROM characters)");
    CharacterDatabase.Execute("DELETE FROM character_queststatus WHERE guid NOT IN (SELECT guid FROM characters)");
    CharacterDatabase.Execute("DELETE FROM character_homebind   WHERE guid NOT IN (SELECT guid FROM characters)");
    CharacterDatabase.Execute("DELETE FROM character_glyphs     WHERE guid NOT IN (SELECT guid FROM characters)");

    WorldDatabase.Execute("DELETE FROM pvpbots_registry");

    while (CharacterDatabase.QueueSize() || WorldDatabase.QueueSize())
        std::this_thread::sleep_for(std::chrono::seconds(1));

    QueryResult accounts = LoginDatabase.Query(
        "SELECT id FROM account WHERE username LIKE '{}%'", _accountPrefix
    );

    uint32 count = 0;
    if (accounts)
    {
        do
        {
            AccountMgr::DeleteAccount(accounts->Fetch()[0].Get<uint32>());
            ++count;
        } while (accounts->NextRow());
    }

    LoginDatabase.Execute("COMMIT");
    CharacterDatabase.Execute("COMMIT");

    while (LoginDatabase.QueueSize() || CharacterDatabase.QueueSize())
        std::this_thread::sleep_for(std::chrono::seconds(1));

    LOG_INFO("playerbots", "[mod-pvpbots] Deleted {} pvpbot accounts and all associated data.", count);
    LOG_INFO("playerbots", "[mod-pvpbots] Set PvpBots.DeletePvpBotAccounts = 0 and restart to recreate bots.");

    World::StopNow(SHUTDOWN_EXIT_CODE);
}

// ============================================================
// GetFactionForIndex
// ============================================================
uint8 PvpBotMgr::GetFactionForIndex(uint32 index) const
{
    switch (_factionMode)
    {
        case 0: return 0;
        case 1: return 1;
        case 2: return (index % 2 == 0) ? 0 : 1;
        case 3:
        {
            uint32 hordeCount = (_totalCount * _hordePercent) / 100;
            return (index < hordeCount) ? 0 : 1;
        }
        default:
            return (index % 2 == 0) ? 0 : 1;
    }
}

// ============================================================
// GetLevelForIndex
//
// Maps a global bot index to a level by walking the bracket list.
// Each bracket owns a contiguous slice of [0, totalCount).
// Within a bracket a random level in [minLevel, maxLevel] is returned.
// ============================================================
uint8 PvpBotMgr::GetLevelForIndex(uint32 index) const
{
    uint32 pos = index % _totalCount;
    uint32 offset = 0;

    for (const auto& bracket : _levelBrackets)
    {
        if (bracket.count == 0)
            continue;

        if (pos < offset + bracket.count)
        {
            if (bracket.minLevel == bracket.maxLevel)
                return bracket.minLevel;
            return static_cast<uint8>(urand(bracket.minLevel, bracket.maxLevel));
        }
        offset += bracket.count;
    }

    // Fallback — should never happen if percentages sum to 100.
    return 80;
}

// ============================================================
// GetRandomPvpClass
// ============================================================
uint8 PvpBotMgr::GetRandomPvpClass(uint8 level) const
{
    static const std::vector<uint8> pvpClassPool = {
        CLASS_MAGE,         CLASS_MAGE,
        CLASS_WARLOCK,      CLASS_WARLOCK,
        CLASS_HUNTER,       CLASS_HUNTER,
        CLASS_SHAMAN,       CLASS_SHAMAN,
        CLASS_ROGUE,        CLASS_ROGUE,
        CLASS_PALADIN,
        CLASS_DEATH_KNIGHT,
        CLASS_DRUID,
        CLASS_PRIEST,
        CLASS_WARRIOR,
    };
    static const std::vector<uint8> pvpClassPoolNoDK = {
        CLASS_MAGE,         CLASS_MAGE,
        CLASS_WARLOCK,      CLASS_WARLOCK,
        CLASS_HUNTER,       CLASS_HUNTER,
        CLASS_SHAMAN,       CLASS_SHAMAN,
        CLASS_ROGUE,        CLASS_ROGUE,
        CLASS_PALADIN,
        CLASS_DRUID,
        CLASS_PRIEST,
        CLASS_WARRIOR,
    };

    const auto& pool = (level >= 60) ? pvpClassPool : pvpClassPoolNoDK;
    return pool[urand(0, static_cast<uint32>(pool.size()) - 1)];
}

// ============================================================
// GetRaceName
// ============================================================
const char* PvpBotMgr::GetRaceName(uint8 race)
{
    switch (race)
    {
        case RACE_HUMAN:         return "Human";
        case RACE_ORC:           return "Orc";
        case RACE_DWARF:         return "Dwarf";
        case RACE_NIGHTELF:      return "Night Elf";
        case RACE_UNDEAD_PLAYER: return "Undead";
        case RACE_TAUREN:        return "Tauren";
        case RACE_GNOME:         return "Gnome";
        case RACE_TROLL:         return "Troll";
        case RACE_BLOODELF:      return "Blood Elf";
        case RACE_DRAENEI:       return "Draenei";
        default:                 return "Unknown";
    }
}

// ============================================================
// GetClassName
// ============================================================
const char* PvpBotMgr::GetClassName(uint8 classId)
{
    switch (classId)
    {
        case CLASS_WARRIOR:      return "Warrior";
        case CLASS_PALADIN:      return "Paladin";
        case CLASS_HUNTER:       return "Hunter";
        case CLASS_ROGUE:        return "Rogue";
        case CLASS_PRIEST:       return "Priest";
        case CLASS_DEATH_KNIGHT: return "Death Knight";
        case CLASS_SHAMAN:       return "Shaman";
        case CLASS_MAGE:         return "Mage";
        case CLASS_WARLOCK:      return "Warlock";
        case CLASS_DRUID:        return "Druid";
        default:                 return "Unknown";
    }
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

// ============================================================
// CleanupInvalidPets
//
// Deletes character_pet rows for every pvpbot whose class cannot have a
// persistent pet (i.e. not hunter / warlock / death knight).
//
// Root cause: PlayerbotFactory::Randomize() calls InitPet(), which can
// leave stale entries in character_pet (e.g. companion pets from gear
// initialization, or entries left over from a previous class assignment).
// When HandlePlayerLoginFromDB loads that entry, the core logs:
//   "Unknown type pet X is summoned by player class Y"
// because the stored PetType is neither SUMMON_PET nor HUNTER_PET.
//
// Called once at startup (before LoginAllBots) so the error never fires on
// login.  OnBotLoginInternal also calls Pet::Remove(PET_SAVE_AS_DELETED)
// after each Randomize to keep the table clean going forward.
// ============================================================
void PvpBotMgr::CleanupInvalidPets()
{
    uint32 count = 0;

    for (const auto& entry : _bots)
    {
        if (entry.classId == CLASS_HUNTER ||
            entry.classId == CLASS_WARLOCK ||
            entry.classId == CLASS_DEATH_KNIGHT)
            continue;

        CharacterDatabase.DirectExecute(
            "DELETE FROM character_pet WHERE owner = {}",
            entry.charGuid.GetCounter()
        );
        ++count;
    }

    if (count == 0)
        return;

    LOG_INFO("playerbots",
        "[mod-pvpbots] Cleaned up stale pet data for {} non-pet bots.", count);
}

// ============================================================
// CleanupDkStartingArea
//
// Death Knights stored in MAP_EBON_HOLD (map 609) receive a severely
// restricted talent point formula when CalculateTalentsPoints() fires
// during InitPvpSpec:
//
//   Ebon Hold:  (level - 55) + m_questRewardTalentCount
//   Everywhere: level - 9
//
// A level 60 DK in Ebon Hold with no completed talent-reward quests gets
// only 5 free points instead of 51 — causing the spec link to under-spend.
//
// Fix: before any bot logs in, move every DK pvpbot whose stored map is
// 609 to Light's Hope Chapel in Eastern Plaguelands (map 0). This is the
// natural exit destination after completing the DK starting zone, so it's
// a lore-appropriate position. The characters table is updated directly;
// the bot is not yet in the world so no TeleportTo is needed.
// ============================================================
void PvpBotMgr::CleanupDkStartingArea()
{
    uint32 count = 0;

    for (const auto& entry : _bots)
    {
        if (entry.classId != CLASS_DEATH_KNIGHT)
            continue;

        // Light's Hope Chapel, Eastern Plaguelands (map 0)
        // This is the standard DK exit point from the starting zone.
        CharacterDatabase.DirectExecute(
            "UPDATE characters SET map = 0, position_x = 2356.4, position_y = -5664.5, "
            "position_z = 423.5, orientation = 0.0 WHERE guid = {} AND map = 609",
            entry.charGuid.GetCounter()
        );
        ++count;
    }

    if (count == 0)
        return;

    LOG_INFO("playerbots",
        "[mod-pvpbots] Relocated {} Death Knight bot(s) out of Ebon Hold.", count);
}

// ============================================================
// BuildPvpItemCache
//
// Queries item_template for every item that has resilience rating
// (stat_type = 35) as one of its ten stat slots.  Results are stored
// keyed by InventoryType, sorted by ItemLevel descending so that
// EquipPvpGear() always tries the highest-level qualifying item first.
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
        "SELECT entry, InventoryType, RequiredLevel, ItemLevel, AllowableClass "
        "FROM item_template "
        "WHERE Quality >= 2 "
        "  AND RequiredLevel > 0 "
        "  AND InventoryType BETWEEN 1 AND 27 "
        "  AND InventoryType NOT IN (18, 19) "
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
            "SELECT entry, InventoryType, RequiredLevel, ItemLevel, AllowableClass "
            "FROM item_template "
            "WHERE Quality >= 2 "
            "  AND InventoryType = 12 "
            "  AND RequiredLevel > 0 "
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
            "SELECT entry, InventoryType, RequiredLevel, ItemLevel, AllowableClass "
            "FROM item_template "
            "WHERE Quality >= 2 "
            "  AND InventoryType = 12 "
            "  AND RequiredLevel > 0"
        );
        if (r) do {
            Field* f = r->Fetch();
            PvpItemEntry e;
            e.itemId         = f[0].Get<uint32>();
            e.inventoryType  = f[1].Get<uint8>();
            e.requiredLevel  = f[2].Get<uint8>();
            e.itemLevel      = f[3].Get<uint16>();
            e.allowableClass = f[4].Get<int32>();
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
// EquipPvpGear
//
// Called after InitEquipment() has already geared the bot correctly for its
// PvP spec.  This pass replaces armor and accessory slots with the best
// resilience-bearing items from _pvpItemCache.
//
// Weapons (MAINHAND, OFFHAND, RANGED) are intentionally left alone —
// InitEquipment() owns weapons and uses CanEquipWeapon() +
// CalculateItemTypePenalty() correctly.  Adding a resilience bonus distorts
// those penalties so we do not touch weapon slots here.
//
// When PvpBots.PreferResilienceGear = 1, a fixed bonus per resilience point
// is added so resilience items edge out otherwise equally scored PvE items.
//
// TRINKET1 → highest-level CC-break trinket (spell 42292) the bot qualifies for.
// TRINKET2 → highest StatsWeightCalculator score across all trinkets (best PvE).
// ============================================================
void PvpBotMgr::EquipPvpGear(Player* bot)
{
    if (_pvpItemCache.empty())
        return;

    // Same scorer playerbots uses — auto-detects role from class+spec.
    StatsWeightCalculator calculator(bot);
    calculator.SetItemSetBonus(false); // bots mix PvP sets; partial bonuses mislead scoring

    uint8 level    = bot->GetLevel();
    int32 classMask = static_cast<int32>(bot->getClassMask());

    // Items already placed this pass — prevents double-slots (FINGER1/2) from
    // receiving the same item.
    std::set<uint32> usedIds;

    // Scan all invTypes in the 0-terminated array, score every candidate with
    // StatsWeightCalculator + resilience bonus, equip the winner.
    auto tryEquip = [&](uint8 equipSlot, const uint8* invTypes) -> bool
    {
        float  bestScore = -1.0f;
        uint32 bestId    = 0;
        uint16 bestDest  = 0;

        for (int ti = 0; invTypes[ti] != 0; ++ti)
        {
            auto cacheIt = _pvpItemCache.find(invTypes[ti]);
            if (cacheIt == _pvpItemCache.end())
                continue;

            for (const PvpItemEntry& entry : cacheIt->second)
            {
                if (usedIds.count(entry.itemId))               continue;
                if (entry.requiredLevel > level)               continue;
                if (entry.allowableClass != -1 &&
                    !(entry.allowableClass & classMask))       continue;

                uint16 dest;
                if (bot->CanEquipNewItem(equipSlot, dest, entry.itemId, true) != EQUIP_ERR_OK)
                    continue;

                float score = calculator.CalculateItem(entry.itemId);

                // Add resilience bonus so items with more resilience rating are
                // preferred over items that are otherwise equally scored.
                if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry.itemId))
                {
                    for (uint8 j = 0; j < MAX_ITEM_PROTO_STATS; ++j)
                    {
                        if (proto->ItemStat[j].ItemStatType == ITEM_MOD_RESILIENCE_RATING &&
                            proto->ItemStat[j].ItemStatValue > 0)
                        {
                            if (_preferResilienceGear)
                                score += proto->ItemStat[j].ItemStatValue * 2.0f;
                            break;
                        }
                    }
                }

                if (score > bestScore)
                {
                    bestScore = score;
                    bestId    = entry.itemId;
                    bestDest  = dest;
                }
            }
        }

        if (bestId == 0)
            return false;

        if (Item* old = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, equipSlot))
            bot->DestroyItem(INVENTORY_SLOT_BAG_0, equipSlot, true);

        bot->EquipNewItem(bestDest, bestId, true);
        usedIds.insert(bestId);
        return true;
    };

    // ── Armor and accessory slots ────────────────────────────────────────
    // CanEquipNewItem enforces armor proficiency (cloth/leather/mail/plate).
    static const uint8 sHead[]      = { INVTYPE_HEAD,      0 };
    static const uint8 sNeck[]      = { INVTYPE_NECK,      0 };
    static const uint8 sShoulders[] = { INVTYPE_SHOULDERS, 0 };
    static const uint8 sChest[]     = { INVTYPE_CHEST, INVTYPE_ROBE, 0 };
    static const uint8 sWaist[]     = { INVTYPE_WAIST,     0 };
    static const uint8 sLegs[]      = { INVTYPE_LEGS,      0 };
    static const uint8 sFeet[]      = { INVTYPE_FEET,      0 };
    static const uint8 sWrists[]    = { INVTYPE_WRISTS,    0 };
    static const uint8 sHands[]     = { INVTYPE_HANDS,     0 };
    static const uint8 sFinger[]    = { INVTYPE_FINGER,    0 };
    static const uint8 sTrinket[]   = { INVTYPE_TRINKET,   0 };
    static const uint8 sBack[]      = { INVTYPE_CLOAK,     0 };

    tryEquip(EQUIPMENT_SLOT_HEAD,      sHead);
    tryEquip(EQUIPMENT_SLOT_NECK,      sNeck);
    tryEquip(EQUIPMENT_SLOT_SHOULDERS, sShoulders);
    tryEquip(EQUIPMENT_SLOT_CHEST,     sChest);
    tryEquip(EQUIPMENT_SLOT_WAIST,     sWaist);
    tryEquip(EQUIPMENT_SLOT_LEGS,      sLegs);
    tryEquip(EQUIPMENT_SLOT_FEET,      sFeet);
    tryEquip(EQUIPMENT_SLOT_WRISTS,    sWrists);
    tryEquip(EQUIPMENT_SLOT_HANDS,     sHands);
    tryEquip(EQUIPMENT_SLOT_FINGER1,   sFinger);
    tryEquip(EQUIPMENT_SLOT_FINGER2,   sFinger);

    // ── Trinket slots ─────────────────────────────────────────────────────
    // Clear both slots first to eliminate unique-equip category conflicts.
    for (uint8 s : { (uint8)EQUIPMENT_SLOT_TRINKET1, (uint8)EQUIPMENT_SLOT_TRINKET2 })
        if (Item* t = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, s))
            bot->DestroyItem(INVENTORY_SLOT_BAG_0, s, true);

    // TRINKET1: CC-break trinket if enabled, otherwise falls through to PvE scoring.
    bool trinket1Filled = false;
    if (_enableCcBreakTrinket)
    {
        for (const PvpItemEntry& entry : _ccBreakTrinketCache)
        {
            if (entry.requiredLevel > level)                           continue;
            if (entry.allowableClass != -1 &&
                !(entry.allowableClass & classMask))                   continue;

            uint16 dest;
            if (bot->CanEquipNewItem(EQUIPMENT_SLOT_TRINKET1, dest, entry.itemId, false) != EQUIP_ERR_OK)
                continue;

            bot->EquipNewItem(dest, entry.itemId, true);
            usedIds.insert(entry.itemId);
            trinket1Filled = true;
            break;
        }

        // Heirloom fallback: faction-appropriate CC-break insignia (no level req).
        if (!trinket1Filled && _enableHeirloomCcTrinket)
        {
            uint32 hId = (bot->GetTeamId() == TEAM_ALLIANCE)
                ? HEIRLOOM_CC_ALLIANCE : HEIRLOOM_CC_HORDE;
            uint16 dest;
            if (bot->CanEquipNewItem(EQUIPMENT_SLOT_TRINKET1, dest, hId, false) == EQUIP_ERR_OK)
            {
                bot->EquipNewItem(dest, hId, true);
                usedIds.insert(hId);
                trinket1Filled = true;
            }
        }
    }

    // Fill any unfilled trinket slot(s) with the best PvE trinket by spec.
    // No resilience bonus here — pure StatsWeightCalculator score.
    auto fillPveTrinket = [&](uint8 slot)
    {
        float  bestScore = -1.0f;
        uint32 bestId    = 0;
        uint16 bestDest  = 0;

        for (const PvpItemEntry& entry : _allTrinketCache)
        {
            if (usedIds.count(entry.itemId))                           continue;
            if (entry.requiredLevel > level)                           continue;
            if (entry.allowableClass != -1 &&
                !(entry.allowableClass & classMask))                   continue;

            uint16 dest;
            if (bot->CanEquipNewItem(slot, dest, entry.itemId, false) != EQUIP_ERR_OK)
                continue;

            float score = calculator.CalculateItem(entry.itemId);
            if (score > bestScore)
            {
                bestScore = score;
                bestId    = entry.itemId;
                bestDest  = dest;
            }
        }

        if (bestId != 0)
        {
            bot->EquipNewItem(bestDest, bestId, true);
            usedIds.insert(bestId);
        }
    };

    if (!trinket1Filled)
        fillPveTrinket(EQUIPMENT_SLOT_TRINKET1);
    fillPveTrinket(EQUIPMENT_SLOT_TRINKET2);

    tryEquip(EQUIPMENT_SLOT_BACK,      sBack);

    // Weapons (MAINHAND, OFFHAND, RANGED) are intentionally skipped.
    // InitEquipment() already chose spec-correct weapons using CanEquipWeapon()
    // and CalculateItemTypePenalty().  Adding a resilience bonus here distorts
    // those penalties (e.g. a high-resilience 1H can outscore a 2H for an Arms
    // Warrior), so we leave weapons alone.
}

// ============================================================
// EquipCcBreakTrinket
//
// Used for bots below level 66 when PvpBots.EnableCcBreakTrinket = 1.
// Places the highest-level CC-break trinket (spell 42292) into TRINKET1.
// TRINKET2 is left as Randomize() chose it.
// ============================================================
void PvpBotMgr::EquipCcBreakTrinket(Player* bot)
{
    uint8  level     = bot->GetLevel();
    int32  classMask = static_cast<int32>(bot->getClassMask());

    // Clear TRINKET1 only; TRINKET2 keeps Randomize()'s choice.
    if (Item* t = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_TRINKET1))
        bot->DestroyItem(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_TRINKET1, true);

    // Try the regular CC-break trinket cache first (sorted by ilvl desc).
    bool equipped = false;
    for (const PvpItemEntry& entry : _ccBreakTrinketCache)
    {
        if (entry.requiredLevel > level)                               continue;
        if (entry.allowableClass != -1 &&
            !(entry.allowableClass & classMask))                       continue;

        uint16 dest;
        if (bot->CanEquipNewItem(EQUIPMENT_SLOT_TRINKET1, dest, entry.itemId, false) != EQUIP_ERR_OK)
            continue;

        bot->EquipNewItem(dest, entry.itemId, true);
        equipped = true;
        break;
    }

    // Fallback: faction-appropriate heirloom (scales from level 1, no level req).
    if (!equipped && _enableHeirloomCcTrinket)
    {
        uint32 hId = (bot->GetTeamId() == TEAM_ALLIANCE)
            ? HEIRLOOM_CC_ALLIANCE : HEIRLOOM_CC_HORDE;
        uint16 dest;
        if (bot->CanEquipNewItem(EQUIPMENT_SLOT_TRINKET1, dest, hId, false) == EQUIP_ERR_OK)
            bot->EquipNewItem(dest, hId, true);
    }
}

// ============================================================
// DebugLog
// ============================================================
void PvpBotMgr::DebugLog(const std::string& msg) const
{
    if (_debug)
        LOG_INFO("playerbots", "[mod-pvpbots][DEBUG] {}", msg);
}
