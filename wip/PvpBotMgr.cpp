/*
 * mod-pvpbots — PvpBotMgr.cpp
 */

#include "PvpBotMgr.h"
#include "AccountMgr.h"
#include "CharacterCache.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "World.h"
#include "WorldSession.h"

// mod-playerbots
#include "PlayerbotAIConfig.h"
#include "PlayerbotMgr.h"       // PlayerbotHolder::AddPlayerBot
#include "RandomPlayerbotFactory.h"
#include "RandomPlayerbotMgr.h" // sRandomPlayerbotMgr

PvpBotMgr* PvpBotMgr::instance()
{
    static PvpBotMgr instance;
    return &instance;
}

void PvpBotMgr::Initialize()
{
    LoadConfig();

    if (!_enabled)
    {
        LOG_INFO("module", "[mod-pvpbots] Disabled in config.");
        return;
    }

    // --------------------------------------------------------
    // Delete mode — wipe everything and shut down cleanly
    // Mirrors AiPlayerbot.DeleteRandomBotAccounts behavior
    // --------------------------------------------------------
    if (_deleteBots)
    {
        DeleteAllPvpBots();
        return;
    }

    if (_gankerPercent + _duelistPercent + _objectivePercent != 100)
    {
        LOG_ERROR("module",
            "[mod-pvpbots] GankerPercent ({}) + DuelistPercent ({}) + ObjectivePercent ({}) must equal 100. "
            "Module disabled.",
            _gankerPercent, _duelistPercent, _objectivePercent);
        _enabled = false;
        return;
    }

    LOG_INFO("module",
        "[mod-pvpbots] Starting. Total: {}, Gankers: {}%, Duelists: {}%, Objectives: {}%",
        _totalCount, _gankerPercent, _duelistPercent, _objectivePercent);

    LoadFromDB();
    EnsureBotCount();

    // Login bots directly — does not depend on RandomBotAutologin
    for (const auto& entry : _bots)
        LoginBot(entry);

    LOG_INFO("module", "[mod-pvpbots] {} PvP bots logged in.", _bots.size());
}

void PvpBotMgr::Update(uint32 diff)
{
    if (!_enabled || _bots.empty())
        return;

    _updateTimer += diff;
    if (_updateTimer < UPDATE_INTERVAL_MS)
        return;

    _updateTimer = 0;

    // Phase 4 stub — role behavior implemented later
    for (auto& entry : _bots)
        (void)entry;
}

void PvpBotMgr::Shutdown()
{
    if (!_enabled)
        return;

    LOG_INFO("module", "[mod-pvpbots] Shutting down.");
    _bots.clear();
}

void PvpBotMgr::LoadConfig()
{
    _enabled           = sConfigMgr->GetOption<bool>       ("PvpBots.Enable",              false);
    _deleteBots        = sConfigMgr->GetOption<bool>       ("PvpBots.DeletePvpBotAccounts", false);
    _accountPrefix     = sConfigMgr->GetOption<std::string>("PvpBots.AccountPrefix",        "pvpbot");
    _totalCount        = sConfigMgr->GetOption<uint32>     ("PvpBots.TotalCount",           50);
    _gankerPercent     = sConfigMgr->GetOption<uint32>     ("PvpBots.GankerPercent",        40);
    _duelistPercent    = sConfigMgr->GetOption<uint32>     ("PvpBots.DuelistPercent",       30);
    _objectivePercent  = sConfigMgr->GetOption<uint32>     ("PvpBots.ObjectivePercent",     30);
    _factionMode       = sConfigMgr->GetOption<uint8>      ("PvpBots.FactionMode",          2);
    _hordePercent      = sConfigMgr->GetOption<uint32>     ("PvpBots.HordePercent",         50);
    _alliancePercent   = sConfigMgr->GetOption<uint32>     ("PvpBots.AlliancePercent",      50);
    _respawnDelay      = sConfigMgr->GetOption<uint32>     ("PvpBots.RespawnDelay",         30);
    _minGearIlvl       = sConfigMgr->GetOption<uint32>     ("PvpBots.MinGearItemLevel",     226);
    _forcePvpTrinket   = sConfigMgr->GetOption<bool>       ("PvpBots.ForcePvpTrinket",      true);
    _joinBattlegrounds = sConfigMgr->GetOption<bool>       ("PvpBots.JoinBattlegrounds",    true);
    _joinArenas        = sConfigMgr->GetOption<bool>       ("PvpBots.JoinArenas",           true);
    _debug             = sConfigMgr->GetOption<bool>       ("PvpBots.Debug",                false);

    DebugLog("Config loaded.");
}

// ============================================================
// DeleteAllPvpBots
// Mirrors RandomPlayerbotFactory::CreateRandomBots() delete path.
// Deletes all characters and accounts matching our prefix,
// clears pvpbots_registry, then stops the server.
// ============================================================
void PvpBotMgr::DeleteAllPvpBots()
{
    LOG_INFO("module", "[mod-pvpbots] DeletePvpBotAccounts = 1. Starting deletion...");

    // Get DB names for cross-DB queries
    std::string loginDB = LoginDatabase.GetConnectionInfo()->database;
    std::string charDB  = CharacterDatabase.GetConnectionInfo()->database;

    // Delete characters on pvpbot accounts
    CharacterDatabase.Execute(
        "DELETE FROM characters WHERE account IN "
        "(SELECT id FROM {}.account WHERE username LIKE '{}%')",
        loginDB, _accountPrefix
    );

    // Wait for character deletes to complete
    while (CharacterDatabase.QueueSize())
        std::this_thread::sleep_for(std::chrono::seconds(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Clean up orphaned character data (mirrors the randombot cleanup)
    CharacterDatabase.Execute("DELETE FROM character_inventory WHERE guid NOT IN (SELECT guid FROM characters)");
    CharacterDatabase.Execute("DELETE FROM item_instance WHERE owner_guid NOT IN (SELECT guid FROM characters) AND owner_guid > 0");
    CharacterDatabase.Execute("DELETE FROM character_spell WHERE guid NOT IN (SELECT guid FROM characters)");
    CharacterDatabase.Execute("DELETE FROM character_talent WHERE guid NOT IN (SELECT guid FROM characters)");
    CharacterDatabase.Execute("DELETE FROM character_skills WHERE guid NOT IN (SELECT guid FROM characters)");
    CharacterDatabase.Execute("DELETE FROM character_aura WHERE guid NOT IN (SELECT guid FROM characters)");
    CharacterDatabase.Execute("DELETE FROM character_action WHERE guid NOT IN (SELECT guid FROM characters)");
    CharacterDatabase.Execute("DELETE FROM character_reputation WHERE guid NOT IN (SELECT guid FROM characters)");
    CharacterDatabase.Execute("DELETE FROM character_queststatus WHERE guid NOT IN (SELECT guid FROM characters)");
    CharacterDatabase.Execute("DELETE FROM character_homebind WHERE guid NOT IN (SELECT guid FROM characters)");
    CharacterDatabase.Execute("DELETE FROM character_glyphs WHERE guid NOT IN (SELECT guid FROM characters)");

    // Clear our registry
    WorldDatabase.Execute("DELETE FROM pvpbots_registry");

    // Wait again
    while (CharacterDatabase.QueueSize() || WorldDatabase.QueueSize())
        std::this_thread::sleep_for(std::chrono::seconds(1));

    // Delete the pvpbot accounts themselves
    QueryResult accounts = LoginDatabase.Query(
        "SELECT id FROM account WHERE username LIKE '{}%'", _accountPrefix
    );

    uint32 count = 0;
    if (accounts)
    {
        do
        {
            uint32 accId = accounts->Fetch()[0].Get<uint32>();
            AccountMgr::DeleteAccount(accId);
            ++count;
        } while (accounts->NextRow());
    }

    // Final flush
    LoginDatabase.Execute("COMMIT");
    CharacterDatabase.Execute("COMMIT");

    while (LoginDatabase.QueueSize() || CharacterDatabase.QueueSize())
        std::this_thread::sleep_for(std::chrono::seconds(1));

    LOG_INFO("module",
        "[mod-pvpbots] Deleted {} pvpbot accounts and all associated characters.",
        count);
    LOG_INFO("module",
        "[mod-pvpbots] Set PvpBots.DeletePvpBotAccounts = 0 and restart to recreate bots.");

    World::StopNow(SHUTDOWN_EXIT_CODE);
}

void PvpBotMgr::LoadFromDB()
{
    _bots.clear();

    QueryResult result = WorldDatabase.Query(
        "SELECT account_id, char_guid, char_name, faction, role, class, state "
        "FROM pvpbots_registry ORDER BY id ASC"
    );

    if (!result)
    {
        DebugLog("No existing pvpbots found. Will create.");
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
        entry.role      = static_cast<PvpBotRole>(fields[4].Get<uint8>());
        entry.classId   = fields[5].Get<uint8>();
        entry.state     = static_cast<PvpBotState>(fields[6].Get<uint8>());

        _bots.push_back(entry);

    } while (result->NextRow());

    LOG_INFO("module", "[mod-pvpbots] Loaded {} existing pvpbots from DB.", _bots.size());
}

void PvpBotMgr::EnsureBotCount()
{
    uint32 current = static_cast<uint32>(_bots.size());
    if (current >= _totalCount)
    {
        DebugLog("Bot count at target.");
        return;
    }

    uint32 needed = _totalCount - current;
    LOG_INFO("module", "[mod-pvpbots] Creating {} new pvpbot(s).", needed);

    // Build name cache from playerbots_names table
    std::unordered_map<RandomPlayerbotFactory::NameRaceAndGender, std::vector<std::string>> nameCache;
    QueryResult nameResult = CharacterDatabase.Query("SELECT name, gender FROM playerbots_names");
    if (nameResult)
    {
        do
        {
            Field* fields = nameResult->Fetch();
            std::string name = fields[0].Get<std::string>();
            auto raceAndGender = static_cast<RandomPlayerbotFactory::NameRaceAndGender>(fields[1].Get<uint8>());

            if (ObjectMgr::CheckPlayerName(name) == CHAR_NAME_SUCCESS)
            {
                CharacterDatabasePreparedStatement* stmt =
                    CharacterDatabase.GetPreparedStatement(CHAR_SEL_CHECK_NAME);
                stmt->SetData(0, name);
                if (!CharacterDatabase.Query(stmt))
                    nameCache[raceAndGender].push_back(name);
            }
        } while (nameResult->NextRow());
    }

    for (uint32 i = 0; i < needed; ++i)
    {
        uint32 globalIndex = current + i;
        CreatePvpBot(
            GetRoleForIndex(globalIndex),
            GetFactionForIndex(globalIndex),
            GetRandomPvpClass(),
            globalIndex,
            nameCache
        );
    }

    if (needed > 0)
    {
        LOG_INFO("module", "[mod-pvpbots] Waiting for DB writes...");
        while (CharacterDatabase.QueueSize())
            std::this_thread::sleep_for(std::chrono::seconds(1));
        LOG_INFO("module", "[mod-pvpbots] Character creation complete.");
    }
}

bool PvpBotMgr::CreatePvpBot(
    PvpBotRole role,
    uint8 faction,
    uint8 classId,
    uint32 index,
    std::unordered_map<RandomPlayerbotFactory::NameRaceAndGender, std::vector<std::string>>& nameCache)
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
            LOG_ERROR("module", "[mod-pvpbots] Failed to create account '{}'", accountName);
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
        LOG_ERROR("module", "[mod-pvpbots] Character creation failed for account {} class {}",
            accountId, classId);
        delete session;
        return false;
    }

    playerBot->SaveToDB(true, false);

    uint32 charGuid     = playerBot->GetGUID().GetCounter();
    std::string charName = playerBot->GetName();
    uint8 charRace      = playerBot->getRace();
    uint8 actualFaction = IsAlliance(charRace) ? 1 : 0;

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
        "(account_id, char_guid, char_name, faction, role, class, state) "
        "VALUES ({}, {}, '{}', {}, {}, {}, {})",
        accountId, charGuid, charName,
        static_cast<uint32>(actualFaction),
        static_cast<uint32>(role),
        static_cast<uint32>(classId),
        static_cast<uint32>(PVPBOT_STATE_IDLE)
    );

    PvpBotEntry entry;
    entry.accountId = accountId;
    entry.charGuid  = ObjectGuid::Create<HighGuid::Player>(charGuid);
    entry.charName  = charName;
    entry.faction   = actualFaction;
    entry.role      = role;
    entry.classId   = classId;
    entry.state     = PVPBOT_STATE_IDLE;
    _bots.push_back(entry);

    LOG_INFO("module", "[mod-pvpbots] Created: {} | Class {} | Role: {} | Faction: {}",
        charName, classId,
        role == PVPBOT_ROLE_GANKER ? "Ganker" :
        role == PVPBOT_ROLE_DUELIST ? "Duelist" : "Objective",
        actualFaction == 0 ? "Horde" : "Alliance"
    );

    return true;
}

// ============================================================
// LoginBot
//
// Calls AddPlayerBot(guid, 0) DIRECTLY on sRandomPlayerbotMgr.
// masterAccountId=0 marks it as a randombot (isRndbot=true),
// which bypasses the permission checks in AddPlayerBot().
//
// This works regardless of:
//   - AiPlayerbot.RandomBotAutologin setting
//   - AiPlayerbot.MinRandomBots / MaxRandomBots values
//
// The only requirement is AiPlayerbot.Enabled = 1.
// ============================================================
void PvpBotMgr::LoginBot(const PvpBotEntry& entry)
{
    sRandomPlayerbotMgr.AddPlayerBot(entry.charGuid, 0);
    DebugLog("Logged in: " + entry.charName);
}

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
        default: return (index % 2 == 0) ? 0 : 1;
    }
}

PvpBotRole PvpBotMgr::GetRoleForIndex(uint32 index) const
{
    uint32 gankerCount  = (_totalCount * _gankerPercent)  / 100;
    uint32 duelistCount = (_totalCount * _duelistPercent) / 100;
    uint32 pos          = index % _totalCount;

    if (pos < gankerCount)
        return PVPBOT_ROLE_GANKER;
    else if (pos < gankerCount + duelistCount)
        return PVPBOT_ROLE_DUELIST;
    else
        return PVPBOT_ROLE_OBJECTIVE;
}

uint8 PvpBotMgr::GetRandomPvpClass() const
{
    static const std::vector<uint8> pvpClassPool = {
        CLASS_MAGE,    CLASS_MAGE,
        CLASS_WARLOCK, CLASS_WARLOCK,
        CLASS_HUNTER,  CLASS_HUNTER,
        CLASS_SHAMAN,  CLASS_SHAMAN,
        CLASS_ROGUE,   CLASS_ROGUE,
        CLASS_PALADIN,
        CLASS_DEATH_KNIGHT,
        CLASS_DRUID,
        CLASS_PRIEST,
        CLASS_WARRIOR,
    };
    return pvpClassPool[urand(0, static_cast<uint32>(pvpClassPool.size()) - 1)];
}

void PvpBotMgr::DebugLog(const std::string& msg) const
{
    if (_debug)
        LOG_INFO("module", "[mod-pvpbots][DEBUG] {}", msg);
}
