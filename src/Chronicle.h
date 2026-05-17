/*
 * Copyright (C) 2024+ Chronicle <https://github.com/Emyrk/chronicle>
 * Released under GNU AGPL v3 license
 *
 * Server-side combat log generator for Chronicle.
 * Produces per-instance log files in WotLK combat log format with
 * Chronicle-specific extension events (CHRONICLE_*).
 *
 * Line format: <unix_millis>  EVENT,field,field,...
 *   - Standard WotLK events: SWING_DAMAGE, SPELL_DAMAGE, SPELL_HEAL, etc.
 *   - Chronicle extensions:  CHRONICLE_HEADER, CHRONICLE_ZONE_INFO,
 *                             CHRONICLE_COMBATANT_INFO, CHRONICLE_UNIT_INFO
 */

#ifndef MOD_CHRONICLE_H
#define MOD_CHRONICLE_H

#include "ObjectGuid.h"
#include "Player.h"        // EnviromentalDamage (unscoped enum, can't forward-declare)
#include "SharedDefines.h" // Powers, SpellMissInfo (unscoped enums)
#include "DBCEnums.h"      // Difficulty
#include <array>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <unordered_map>
#include <vector>

class Aura;
class AuraApplication;
class AuraEffect;
class Creature;
class DamageInfo;
class HealInfo;
class Item;
class Map;
class Spell;
class SpellInfo;
class WorldObject;
struct CalcDamageInfo;
struct SpellNonMeleeDamage;
struct SpellPeriodicAuraLogInfo;
enum AuraRemoveMode : uint8;
enum EncounterCreditType : uint8;
enum EncounterState : uint8;
struct DungeonEncounter;

// ---------------------------------------------------------------------------
// EventFormatter — produces WotLK-style comma-separated combat log lines
// with unix millisecond timestamps.
//
// Format:  <unix_millis>  EVENT_TYPE,sourceGUID,"sourceName",sourceFlags,
//                         destGUID,"destName",destFlags,[event params...]
// GUIDs:   0x%016llX of ObjectGuid::GetRawValue()
// ---------------------------------------------------------------------------
class EventFormatter
{
public:
    static std::string Guid(ObjectGuid guid);

    // Returns wall-clock milliseconds since Unix epoch.
    static uint64 Now();

    // Compute WotLK-style unit flags from Unit properties.
    // Flag reference (from WoW API):
    //   Type:        PLAYER=0x0400, NPC=0x0800, PET=0x1000, GUARDIAN=0x2000
    //   Control:     PLAYER=0x0100, NPC=0x0200
    //   Reaction:    FRIENDLY=0x0010, NEUTRAL=0x0020, HOSTILE=0x0040
    //   Affiliation: MINE=0x0001, PARTY=0x0002, RAID=0x0004, OUTSIDER=0x0008
    static uint32 UnitFlags(Unit* unit);

    // Base params: sourceGUID,"sourceName",sourceFlags,destGUID,"destName",destFlags
    static std::string BaseParams(Unit* source, Unit* dest);

    // --- Chronicle extension events (CHRONICLE_*) ---
    static std::string Header(std::string const& realmName);
    static std::string ZoneInfo(std::string const& zoneName, uint32 mapId,
                                uint32 instanceId, std::string const& instanceType,
                                std::string const& runType);
    static std::string CombatantInfo(Player* player);
    static std::string UnitInfo(Unit* unit);
    static std::string UnitEvade(Unit* unit, uint8 evadeReason);
    static std::string UnitCombat(Unit* unit, Unit* victim);
    static std::string UnitDespawn(Creature* creature);

    // --- Standard WotLK combat events ---

    // Melee auto-attack with full outcome data.
    // slot selects the damage sub-index (0 or 1).
    // overkill is the hook-provided value (combined damage vs HP).
    static std::string SwingDamage(CalcDamageInfo const* damageInfo,
                                   uint8 slot, int32 overkill);
    static std::string SwingMissed(CalcDamageInfo const* damageInfo);

    // Spell damage with absorb/resist/block
    static std::string SpellDamage(SpellNonMeleeDamage const* log, int32 overkill);

    // Spell miss/immune/resist/reflect
    static std::string SpellMissed(Unit* attacker, Unit* victim, uint32 spellId,
                                    SpellMissInfo missInfo);

    // Healing with overheal and crit
    static std::string SpellHeal(HealInfo const& healInfo, bool critical);

    // Mana/rage/energy restore
    static std::string SpellEnergize(Unit* caster, Unit* target, uint32 spellId,
                                      uint32 amount, Powers powerType);

    // Periodic ticks — damage, healing, energize
    static std::string SpellPeriodicDamage(Unit* victim, SpellPeriodicAuraLogInfo* pInfo);
    static std::string SpellPeriodicHeal(Unit* victim, SpellPeriodicAuraLogInfo* pInfo);
    static std::string SpellPeriodicDrain(Unit* victim, SpellPeriodicAuraLogInfo* pInfo);
    static std::string SpellPeriodicEnergize(Unit* victim, SpellPeriodicAuraLogInfo* pInfo);

    // Damage shield (thorns etc.)
    static std::string DamageShield(Unit* shieldOwner, Unit* attacker, SpellInfo const* spell, uint32 damage, uint32 absorb, uint32 overkill);

    // Absorb aura soaked damage (PW:S, Mana Shield, etc.)
    static std::string SpellAbsorbed(DamageInfo& dmgInfo,
        SpellInfo const* absorbSpell, Unit* absorbCaster, uint32 absorbAmount);

    // Aura apply/remove with caster info
    static std::string SpellAuraApplied(Unit* caster, Unit* target, SpellInfo const* spell);
    static std::string SpellAuraRemoved(Unit* caster, Unit* target, SpellInfo const* spell);

    // Spell cast
    static std::string SpellCastSuccess(Unit* caster, WorldObject* explicitTarget,
                                        SpellInfo const* spell);

    // Summon
    static std::string SpellSummon(Unit* caster, SpellInfo const* spell, WorldObject* summoned);

    // Death
    static std::string UnitDied(Unit* killer, Unit* victim);

    // Environmental damage
    static std::string EnvironmentalDamage(Player* victim, EnviromentalDamage type,
                                           uint32 damage);

    // Spell interrupt (Kick, Counterspell, Pummel, etc.)
    static std::string SpellInterrupt(Unit* interrupter, Unit* interrupted,
                                      uint32 interruptSpellId, uint32 interruptedSpellId);
    static std::string SpellDispel(Unit* dispeller, Unit* victim,
                                   uint32 dispelSpellId, uint32 removedSpellId,
                                   bool isSteal);
    static std::string SpellTargetResult(Unit* caster, Unit* target,
                                         SpellInfo const* spell,
                                         SpellMissInfo missInfo,
                                         SpellMissInfo reflectResult,
                                         uint8 effectMask);
    static std::string LootItem(Player* looter, ObjectGuid lootGuid,
                                Item* item, uint32 count);
    static std::string LootMoney(Player* looter, ObjectGuid lootGuid,
                                 uint32 amount);

    // --- Encounter events ---
    static std::string EncounterStart(uint32 bossId, Map* instance);
    static std::string EncounterEnd(uint32 bossId, Map* instance, bool success);
    static std::string EncounterCredit(
        Map* map, EncounterCreditType type, uint32 creditEntry,
        Unit* source, Difficulty difficulty,
        uint32 encounterDbcId, std::string const& encounterName,
        uint32 dungeonCompleted);
};

// ---------------------------------------------------------------------------
// CombatLogWriter — one per active instance.  Owns an std::ofstream.
// ---------------------------------------------------------------------------
class CombatLogWriter
{
public:
    CombatLogWriter(std::string const& dir, uint32 mapId, uint32 instanceId,
                    std::string const& mapName, std::string const& realmName);
    ~CombatLogWriter();

    void WriteLine(std::string const& line);
    void Flush();
    void Close();
    bool Reopen();
    bool IsOpen() const;
    bool IsIdleFor(std::chrono::seconds idleTimeout) const;
    std::string const& GetPath() const { return _path; }
    uint32 GetInstanceId() const { return _instanceId; }
    std::string const& GetMapName() const { return _mapName; }
    std::string const& GetRealmName() const { return _realmName; }

private:
    void OpenNewFile();

    std::ofstream _file;
    std::string   _dir;
    std::string   _path;
    uint32        _mapId;
    uint32        _instanceId;
    std::string   _mapName;
    std::string   _realmName;
    std::chrono::steady_clock::time_point _lastWriteTime;
};

// ---------------------------------------------------------------------------
// InstanceTracker — singleton.  Maps instanceId → CombatLogWriter.
// ---------------------------------------------------------------------------
class InstanceTracker
{
public:
    static InstanceTracker& Instance();

    void LoadConfig();
    void Shutdown();
    bool IsEnabled() const { return _enabled; }
    std::string const& GetUploadURL() const { return _uploadURL; }
    std::string const& GetUploadSecret() const { return _uploadSecret; }
    bool ShouldSnapshotOnEncounterCredit() const { return _uploadSnapshots && _snapshotOnEncounterCredit; }
    static void PingRemote(std::string url, std::string secret, bool requireTls, bool verifyTls);
    void QueuePingTask(std::string url, std::string secret);

    // Sweep the log directory on startup and upload any leftover .log files
    // from a previous crash or unclean shutdown.
    void UploadOrphanedLogs();

    // Called from AllMapScript hooks
    void OnPlayerEnterInstance(Map* map, Player* player);
    void OnPlayerLeaveInstance(Map* map, Player* player);
    void RemoveInstance(uint32 instanceId);
    void OnInstanceIdRemoved(uint32 instanceId);

    // Called from UnitScript / GlobalScript / PlayerScript hooks — resolves unit→map→writer.
    void WriteForUnit(Unit* unit, std::string const& line);
    void WriteForMap(Map* map, std::string const& line);

    // Write directly to a known instance (used by hooks that provide Map* but no Unit*).
    void WriteForInstance(uint32 instanceId, std::string const& line);

    // Flush the log file for a given instance to disk (e.g. after boss kill).
    void FlushInstance(uint32 instanceId);
    void FlushMap(Map* map);

    // Flush, copy the log to a temp file, and upload the snapshot in a
    // background thread.  The original file stays open for continued writing.
    void UploadInstanceSnapshot(uint32 instanceId);
    void UploadInstanceSnapshot(Map* map);

    // Emits CHRONICLE_UNIT_INFO for a unit the first time it's seen in an instance.
    // Called from combat hooks before writing damage/heal/death events.
    void EnsureUnitInfo(Unit* unit);

    // Returns the unique instance token for the given instanceId, or "" if none.
    std::string GetInstanceToken(uint32 instanceId) const;

private:
    enum class BackgroundTaskType
    {
        Upload,
        Ping,
    };

    struct BackgroundTask
    {
        BackgroundTaskType type = BackgroundTaskType::Upload;
        std::string path;
        std::string url;
        std::string secret;
        uint32 instanceId = 0;
        std::string mapName;
        std::string realmName;
        std::string instanceToken;
        bool requireTls = false;
        bool verifyTls = true;
    };

    InstanceTracker() = default;

    bool ShouldTrackMap(Map const* map) const;
    CombatLogWriter* GetOrCreateWriter(Map* map);
    void FinalizeWriterForIdleRotation(uint32 instanceId, std::unique_ptr<CombatLogWriter> writer,
                                       std::string instanceToken);
    void EmitWriterPreamble(CombatLogWriter& writer, Map* map);
    void EmitCombatantInfoIfNeeded(CombatLogWriter& writer, Player* player, uint32 instanceId);
    void QueueUploadTask(std::string path, uint32 instanceId, std::string mapName,
                         std::string realmName, std::string instanceToken);
    void EnqueueUploadTaskLocked(std::string path, std::string url, std::string secret,
                                 uint32 instanceId, std::string mapName,
                                 std::string realmName, std::string instanceToken,
                                 bool requireTls, bool verifyTls);
    void EnqueuePingTaskLocked(std::string url, std::string secret,
                               bool requireTls, bool verifyTls);
    void EnsureTaskWorkerStartedLocked();
    void RunTaskWorker();

    // Generate a 128-bit random hex token (32 chars) for unique instance identity.
    // Uses AzerothCore's rand32() + ByteArrayToHexStr().
    static std::string GenerateInstanceToken();

    std::mutex _mutex;
    std::unordered_map<uint32, std::unique_ptr<CombatLogWriter>> _writers;
    // Set of (instanceId, unitGuid) pairs to avoid duplicate UNIT_INFO
    std::unordered_map<uint32, std::unordered_set<uint64>> _seenUnits;
    std::unordered_map<uint32, std::unordered_set<uint64>> _seenCombatants;
    // Per-instance random token, immune to AzerothCore instance-ID reuse.
    std::unordered_map<uint32, std::string> _instanceTokens;

    std::mutex _taskMutex;
    std::condition_variable _taskCv;
    std::deque<BackgroundTask> _taskQueue;
    std::thread _taskWorker;
    uint32 _activeBackgroundTasks = 0;
    bool _shuttingDown = false;
    uint64 _nextSnapshotId = 0;

    static void UploadAndDelete(std::string path, std::string url, std::string secret,
                                uint32 instanceId, std::string mapName, std::string realmName,
                                std::string instanceToken, bool requireTls, bool verifyTls);

    bool        _enabled   = false;
    std::string _logDir    = "chronicle_logs";
    std::string _uploadURL;
    std::string _uploadSecret;
    bool        _requireTls = false;
    bool        _verifyTls = true;
    bool        _trackDungeonRuns = true;
    uint32      _idleCloseSeconds = 0;
    bool        _rotateOnIdle = false;
    bool        _uploadSnapshots = false;
    bool        _snapshotOnEncounterCredit = false;
};

#endif // MOD_CHRONICLE_H
