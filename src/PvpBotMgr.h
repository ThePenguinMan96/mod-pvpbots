/*
 * mod-pvpbots — PvpBotMgr.h
 *
 * PvpBotMgr inherits PlayerbotHolder but bypasses PlayerbotHolder::AddPlayerBot
 * entirely, replacing it with LoginBot(). This cuts RandomPlayerbotMgr out of
 * our bot lifecycle completely.
 *
 * Login flow:
 *   LoginBot(guid) creates a LoginQueryHolder (AzerothCore core), submits it
 *   via sWorld->AddQueryHolderCallback, and in the callback creates a
 *   WorldSession, calls HandlePlayerLoginFromDB, creates the PlayerbotAI via
 *   PlayerbotsMgr::AddPlayerbotData, then inserts directly into our playerBots
 *   map and calls OnBotLoginInternal.
 *
 *   OnBotLoginInternal calls PlayerbotFactory::Randomize to gear and spec
 *   each bot according to its level bracket.
 *
 * sRandomPlayerbotMgr is never called. No migration step needed.
 * UpdateSessions() runs every world tick for maximum bot responsiveness.
 */

#ifndef MOD_PVPBOTS_MGR_H
#define MOD_PVPBOTS_MGR_H

#include "Common.h"
#include "SharedDefines.h"
#include "PlayerbotMgr.h"           // PlayerbotHolder base class
#include "RandomPlayerbotFactory.h" // NameRaceAndGender, CreateRandomBot
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <string>

// Forward declaration for DBC type used in private static helper signature.
struct ScalingStatDistributionEntry;

enum PvpBotState : uint8
{
    PVPBOT_STATE_IDLE   = 0,
    PVPBOT_STATE_ACTIVE = 1,
    PVPBOT_STATE_DEAD   = 2,
    PVPBOT_STATE_IN_BG  = 3,
};


// Persistent record — loaded from pvpbots_registry and kept in memory.
// Separate from PlayerbotHolder::playerBots (the live Player* map).
struct PvpBotEntry
{
    uint32      accountId;
    ObjectGuid  charGuid;
    std::string charName;
    uint8       faction;    // 0 = Horde, 1 = Alliance
    uint8       classId;
    uint8       level;
    PvpBotState state;
};

// One of the ten configurable level brackets.
// percent and count are populated by LoadConfig().
struct PvpBotLevelBracket
{
    uint8       minLevel;
    uint8       maxLevel;
    const char* cfgKey;   // full config key, e.g. "PvpBots.Level10to19Percent"
    uint32      percent;  // configured percentage
    uint32      count;    // computed: totalCount * percent / 100 (remainder given to last bracket)
};

// A talent link string keyed to the minimum bot level at which it applies.
struct PvpSpecLevelLink
{
    uint8       level;
    std::string link;   // wowhead-style talent link, e.g. "0320332023335100...-3250001"
};

// One PvP spec definition for a class.
// Links are sorted ascending by level; the highest one that doesn't exceed
// the bot's actual level is chosen at login time.
struct PvpSpec
{
    std::string                   name;
    std::string                   glyphs;  // comma-separated glyph item IDs (future use)
    std::vector<PvpSpecLevelLink> links;
};

// A resilience-bearing item loaded from item_template at startup.
// Loaded at startup by BuildPvpItemCache(); used by InitPvpEquipment() for
// CC-break trinket lookup. Resilience item cache kept for future reference.
struct PvpItemEntry
{
    uint32 itemId;
    uint8  inventoryType;
    uint8  requiredLevel;
    uint16 itemLevel;
    int32  allowableClass;  // -1 = unrestricted
    uint8  quality;         // ITEM_QUALITY_* — used to gate heirloom (7) entries
};

class PvpBotMgr : public PlayerbotHolder
{
public:
    static PvpBotMgr* instance();

    void Initialize();
    void Update(uint32 diff);
    void Shutdown();

    // Called from the PlayerScript in mod_pvpbots_loader.cpp.
    // Only real (non-bot) players trigger these.
    void NotifyRealPlayerLogin();
    void NotifyRealPlayerLogout();

    bool   IsEnabled()             const { return _enabled; }
    size_t GetRegisteredBotCount() const { return _bots.size(); }
    size_t GetOnlineBotCount()           { return GetPlayerbotsCount(); }

protected:
    // Pure virtual override — called after each bot is inserted into our
    // playerBots map. Gears and specs the bot via PlayerbotFactory.
    void OnBotLoginInternal(Player* bot) override;

private:
    PvpBotMgr()  = default;
    ~PvpBotMgr() = default;
    PvpBotMgr(const PvpBotMgr&)            = delete;
    PvpBotMgr& operator=(const PvpBotMgr&) = delete;

    // ----------------------------------------------------------------
    // Config
    // ----------------------------------------------------------------
    bool        _enabled          = false;
    bool        _deleteBots       = false;
    std::string _accountPrefix    = "pvpbot";
    uint32      _totalCount       = 200;
    uint8       _factionMode      = 2;
    uint32      _hordePercent     = 50;
    uint32      _alliancePercent  = 50;
    bool        _disabledWithoutRealPlayer = false;
    uint32      _loginDelay       = 30;
    uint32      _logoutDelay      = 300;
    bool        _debug            = false;

    // Gear quality (1=Normal … 4=Epic) and optional gear score ceiling (0 = no limit).
    uint32      _gearQualityLimit     = ITEM_QUALITY_EPIC;
    uint32      _gearScoreLimit       = 0;

    // Multiplicative resilience score weight (set from PvpBots.PreferPvpGear).
    // 5.0 when enabled; 0.0 when disabled.
    float       _pvpResilienceWeight  = 5.0f;

    // When true, enchants and gems are always applied regardless of
    // AiPlayerbot.MinEnchantingBotLevel.
    bool        _applyEnchantsAndGems = true;

    // When true, TRINKET1 is always the highest-level CC-break trinket
    // the bot qualifies for. Applies to all bots regardless of level.
    bool        _enableCcBreakTrinket = true;

    // When true, heirloom items (quality 7) are added as candidates in
    // InitPvpEquipment and scored via ScoreHeirloom (SSD/SSV DBC scaling).
    // Also enables heirloom CC-break trinkets in the CC-break cache.
    bool        _allowHeirlooms = true;

    // Multiplicative bonus applied to weapons of the correct speed for the
    // bot's spec (e.g. slow 2H for Arms, slow 1H for Enhancement).
    // score *= (1.0 + _weaponSpeedWeight) when speed matches the spec ideal.
    // 0.0 = disabled; default 2.0 (3× boost for matching speed).
    float       _weaponSpeedWeight = 2.0f;

    // Ten level brackets populated by LoadConfig().
    std::vector<PvpBotLevelBracket> _levelBrackets;

    // PvP spec pool per class — classId → list of PvpSpec definitions.
    // Populated by LoadConfig(). One spec is chosen at random per bot at login.
    std::unordered_map<uint8, std::vector<PvpSpec>> _pvpSpecs;

    // Resilience items from item_template, indexed by InventoryType.
    // Built once at startup by BuildPvpItemCache(); vectors sorted by itemLevel desc.
    std::unordered_map<uint8, std::vector<PvpItemEntry>> _pvpItemCache;

    // Trinkets whose on-use spell is the CC-break (spell 42292).
    // Sorted by itemLevel desc. TRINKET1 always gets the highest-level one
    // the bot qualifies for, guaranteeing every bot can break out of CC.
    std::vector<PvpItemEntry> _ccBreakTrinketCache;

    // All Quality >= 2 trinkets, sorted by itemLevel desc.
    // Used to pick the best PvE trinket for TRINKET2 via StatsWeightCalculator.
    std::vector<PvpItemEntry> _allTrinketCache;

    // Heirloom items (quality 7, ScalingStatDistribution > 0), indexed by
    // InventoryType. Built by BuildHeirloomCache() when _allowHeirlooms = true.
    std::unordered_map<uint8, std::vector<PvpItemEntry>> _heirloomCache;

    // ----------------------------------------------------------------
    // Runtime state
    // ----------------------------------------------------------------
    uint32 _realPlayerCount  = 0;
    bool   _botsLoggedIn     = false;
    uint32 _loginDelayTimer  = 0;
    uint32 _logoutDelayTimer = 0;

    // GUIDs currently mid-login (LoginQueryHolder submitted but callback
    // not yet fired). Prevents duplicate login requests.
    std::unordered_set<ObjectGuid> _botLoading;

    // ----------------------------------------------------------------
    // Bot registry — persistent record of all pvpbots.
    // PlayerbotHolder::playerBots is the separate live Player* map.
    // ----------------------------------------------------------------
    std::vector<PvpBotEntry> _bots;

    // ----------------------------------------------------------------
    // Internal methods
    // ----------------------------------------------------------------
    void LoadConfig();
    void EnsureSchema();
    void LoadFromDB();
    void EnsureBotCount();
    void DeleteAllPvpBots();
    void LoginAllBots();
    void LoginBot(ObjectGuid guid);

    bool CreatePvpBot(
        uint8 faction,
        uint8 classId,
        uint8 level,
        uint32 index,
        std::unordered_map<RandomPlayerbotFactory::NameRaceAndGender,
                           std::vector<std::string>>& nameCache
    );

    uint8      GetFactionForIndex(uint32 index) const;
    uint8      GetLevelForIndex(uint32 index)   const;
    uint8      GetRandomPvpClass(uint8 level)   const;
    void       DebugLog(const std::string& msg) const;

    void CleanupInvalidPets();
    void CleanupDkStartingArea();
    void BuildPvpItemCache();
    void BuildHeirloomCache();
    void InitPvpEquipment(Player* bot);

    // Score a heirloom item for the given bot using SSD/SSV DBC scaling.
    // Returns a value on the same scale as StatsWeightCalculator::CalculateItem
    // so heirlooms compete directly with normal candidates.
    float ScoreHeirloom(Player* bot, uint32 itemId);

    // Per-stat weight for heirloom scoring, keyed to the bot's class.
    // Mirrors StatsWeightCalculator's GenerateBasicWeights ranges without
    // needing a full StatsWeightCalculator instance.
    static float GetHeirloomStatWeight(Player* bot, uint32 statType);
    static float GetHeirloomDpsWeight(Player* bot, ItemTemplate const* proto);


    // Full single-pass initialization for a pvpbot.
    // Sets the PvP spec BEFORE calling InitEquipment/ApplyEnchantAndGemsNew/
    // InitGlyphs so every spec-sensitive function runs once, correctly.
    // Called from OnBotLoginInternal.
    void InitPvpBot(Player* bot);

    // Apply the glyphs defined in spec.glyphs to the bot.
    // Reads directly from our config (PvpBots.SpecGlyph.<class>.<tab>),
    // bypassing sPlayerbotAIConfig.parsedSpecGlyph entirely.
    void InitPvpGlyphs(Player* bot, const PvpSpec& spec);

    // Apply the PvP spec from config, including talents, glyphs, and strategy reset.
    // Mirrors the "talents spec <name>" command flow exactly.
    void InitPvpSpec(Player* bot, const PvpSpec* selectedSpec, uint8 primaryTab);

    // Weapon-speed governance multiplier for InitPvpEquipment's scoring loop.
    // Returns (1.0 + weight) when proto's Delay matches the spec-ideal speed for
    // the given slot; returns 1.0 otherwise (no boost). Only applies to
    // MAINHAND / OFFHAND weapon items; non-weapons always return 1.0.
    static float GetWeaponSpeedMultiplier(Player* bot, int32 slot,
                                          ItemTemplate const* proto, float weight);

    // Safe equip-check helper: mirrors PlayerbotFactory::CanEquipUnseenItem (private).
    // Uses temp=true so the throw-away Item gets GUID 0xFFFFFFFF and is cleaned up
    // properly with RemoveFromUpdateQueueOf before deletion — avoids the stale update
    // queue entries that Player::CanEquipNewItem leaves behind when called in a loop.
    static bool CanEquipItemTemp(Player* bot, uint8 slot, uint16& dest, uint32 itemId);

    static const char* GetRaceName(uint8 race);
    static const char* GetClassName(uint8 classId);
};

#define sPvpBotMgr PvpBotMgr::instance()

#endif // MOD_PVPBOTS_MGR_H
