/*
 * mod-pvpbots — Script loader
 * Wires PvpBotMgr into the AzerothCore script system.
 */

#include "ScriptMgr.h"
#include "Player.h"
#include "PvpBotMgr.h"
#include "PlayerbotAI.h"

// ---------------------------------------------------------------------------
// World hooks — startup / update / shutdown
// ---------------------------------------------------------------------------
class PvpBotsWorldScript : public WorldScript
{
public:
    PvpBotsWorldScript() : WorldScript("PvpBotsWorldScript") {}

    void OnStartup() override
    {
        LOG_INFO("playerbots", "[mod-pvpbots] Initializing...");
        sPvpBotMgr->Initialize();
    }

    void OnUpdate(uint32 diff) override
    {
        sPvpBotMgr->Update(diff);
    }

    void OnShutdown() override
    {
        sPvpBotMgr->Shutdown();
    }
};

// ---------------------------------------------------------------------------
// Player hooks — track real player presence for DisabledWithoutRealPlayer
// ---------------------------------------------------------------------------
class PvpBotsPlayerScript : public PlayerScript
{
public:
    PvpBotsPlayerScript() : PlayerScript("PvpBotsPlayerScript") {}

    void OnPlayerLogin(Player* player) override
    {
        if (PlayerbotsMgr::instance().GetPlayerbotAI(player))
            return;

        sPvpBotMgr->NotifyRealPlayerLogin();
    }

    void OnPlayerLogout(Player* player) override
    {
        if (PlayerbotsMgr::instance().GetPlayerbotAI(player))
            return;

        sPvpBotMgr->NotifyRealPlayerLogout();
    }
};

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------
void Addmod_pvpbotsScripts()
{
    new PvpBotsWorldScript();
    new PvpBotsPlayerScript();
}
