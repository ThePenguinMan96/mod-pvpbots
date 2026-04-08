/*
 * mod-pvpbots — PvpBotMgr.h
 */

#ifndef MOD_PVPBOTS_MGR_H
#define MOD_PVPBOTS_MGR_H

#include "Common.h"
#include "SharedDefines.h"
#include "RandomPlayerbotFactory.h"
#include <unordered_map>
#include <vector>
#include <string>
#include <thread>
#include <chrono>

enum PvpBotRole : uint8
{
    PVPBOT_ROLE_GANKER    = 0,
    PVPBOT_ROLE_DUELIST   = 1,
    PVPBOT_ROLE_OBJECTIVE = 2,
};

enum PvpBotState : uint8
{
    PVPBOT_STATE_IDLE   = 0,
    PVPBOT_STATE_ACTIVE = 1,
    PVPBOT_STATE_DEAD   = 2,
    PVPBOT_STATE_IN_BG  = 3,
};

struct PvpBotEntry
{
    uint32      accountId;
    ObjectGuid  charGuid;
    std::string charName;
    uint8       faction;
    PvpBotRole  role;
    uint8       classId;
    PvpBotState state;
};

class PvpBotMgr
{
public:
    static PvpBotMgr* instance();

    void Initialize();
    void Update(uint32 diff);
    void Shutdown();

    bool   IsEnabled()   const { return _enabled; }
    size_t GetBotCount() const { return _bots.size(); }

private:
    PvpBotMgr()  = default;
    ~PvpBotMgr() = default;
    PvpBotMgr(const PvpBotMgr&)            = delete;
    PvpBotMgr& operator=(const PvpBotMgr&) = delete;

    // Config
    bool        _enabled           = false;
    bool        _deleteBots        = false;   // PvpBots.DeletePvpBotAccounts
    std::string _accountPrefix     = "pvpbot";
    uint32      _totalCount        = 50;
    uint32      _gankerPercent     = 40;
    uint32      _duelistPercent    = 30;
    uint32      _objectivePercent  = 30;
    uint8       _factionMode       = 2;
    uint32      _hordePercent      = 50;
    uint32      _alliancePercent   = 50;
    uint32      _respawnDelay      = 30;
    uint32      _minGearIlvl       = 226;
    bool        _forcePvpTrinket   = true;
    bool        _joinBattlegrounds = true;
    bool        _joinArenas        = true;
    bool        _debug             = false;

    // Methods
    void LoadConfig();
    void LoadFromDB();
    void EnsureBotCount();
    void DeleteAllPvpBots();    // Handles PvpBots.DeletePvpBotAccounts = 1

    bool CreatePvpBot(
        PvpBotRole role,
        uint8 faction,
        uint8 classId,
        uint32 index,
        std::unordered_map<RandomPlayerbotFactory::NameRaceAndGender,
                           std::vector<std::string>>& nameCache
    );

    // Direct login — does NOT depend on RandomBotAutologin being enabled
    void LoginBot(const PvpBotEntry& entry);

    uint8      GetFactionForIndex(uint32 index) const;
    PvpBotRole GetRoleForIndex(uint32 index)    const;
    uint8      GetRandomPvpClass()              const;
    void       DebugLog(const std::string& msg) const;

    std::vector<PvpBotEntry> _bots;
    uint32 _updateTimer = 0;
    static constexpr uint32 UPDATE_INTERVAL_MS = 5000;
};

#define sPvpBotMgr PvpBotMgr::instance()

#endif // MOD_PVPBOTS_MGR_H
