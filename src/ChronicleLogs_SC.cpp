/*
 * Copyright (C) 2024+ Chronicle <https://github.com/Emyrk/chronicle>
 * Released under GNU AGPL v3 license
 *
 * AzerothCore hook scripts that capture combat events and feed them
 * to Chronicle module classes for writing as WotLK-format combat log lines.
 */

#include "Chronicle.h"

#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "Unit.h"
#include "InstanceScript.h" // EncounterState
#include "LootScript.h"
#include "ObjectMgr.h"      // DungeonEncounter, DungeonEncounterList

// ===========================================================================
// UnitScript — captures combat events at the "send to client" point,
// providing complete damage/heal/miss data including absorb, resist,
// block, crit, overkill, overheal, etc.
// ===========================================================================
class ChronicleUnitScript : public UnitScript
{
public:
    ChronicleUnitScript() : UnitScript("ChronicleUnitScript", true, {
        UNITHOOK_ON_SEND_ATTACK_STATE_UPDATE,
        UNITHOOK_ON_SEND_SPELL_NON_MELEE_DAMAGE_LOG,
        UNITHOOK_ON_SEND_HEAL_SPELL_LOG,
        UNITHOOK_ON_SEND_SPELL_MISS,
        UNITHOOK_ON_SEND_SPELL_DAMAGE_IMMUNE,
        UNITHOOK_ON_SEND_SPELL_DAMAGE_RESIST,
        UNITHOOK_ON_SEND_SPELL_NON_MELEE_REFLECT_LOG,
        UNITHOOK_ON_SEND_ENERGIZE_SPELL_LOG,
        UNITHOOK_ON_SEND_PERIODIC_AURA_LOG,
        UNITHOOK_ON_DEAL_DAMAGE_SHIELD_DAMAGE,
        UNITHOOK_ON_SCHOOL_ABSORB_APPLIED,
        UNITHOOK_ON_UNIT_DEATH,
        UNITHOOK_ON_UNIT_ENTER_EVADE_MODE,
        UNITHOOK_ON_UNIT_ENTER_COMBAT,
    }) { }

    // -----------------------------------------------------------------------
    // OnSendAttackStateUpdate — melee hit/miss → SWING_DAMAGE or SWING_MISSED
    // -----------------------------------------------------------------------
    void OnSendAttackStateUpdate(CalcDamageInfo const* damageInfo, int32 overkill) override
    {
        if (!damageInfo || !damageInfo->attacker || !InstanceTracker::Instance().IsEnabled())
            return;

        InstanceTracker::Instance().EnsureUnitInfo(damageInfo->attacker);
        InstanceTracker::Instance().EnsureUnitInfo(damageInfo->target);

        if (damageInfo->hitOutCome == MELEE_HIT_MISS ||
            damageInfo->hitOutCome == MELEE_HIT_DODGE ||
            damageInfo->hitOutCome == MELEE_HIT_PARRY ||
            damageInfo->hitOutCome == MELEE_HIT_EVADE)
        {
            InstanceTracker::Instance().WriteForUnit(
                damageInfo->target, EventFormatter::SwingMissed(damageInfo));
            return;
        }

        // Emit one SWING_DAMAGE per non-zero damage slot (weapons can
        // have two damage types, e.g. physical + fire).
        //
        // Effective damage is taken off slot 0 first, then slot 1.
        //  d0 = 100
        //  d1 = 50
        //  overkill = 120
        //  → slot1ok = min(120, 50) = 50
        //  → slot0ok = 120 - 50 = 70
        bool hasSlot1 = damageInfo->damages[1].damage
                     || damageInfo->damages[1].absorb
                     || damageInfo->damages[1].resist;

        // Overkill peels off slot 1 (last applied) first, then slot 0.
        int32 d1 = static_cast<int32>(damageInfo->damages[1].damage);
        int32 slot1ok = hasSlot1 ? std::min(overkill, d1) : 0;
        int32 slot0ok = overkill - slot1ok;

        InstanceTracker::Instance().WriteForUnit(
            damageInfo->target,
            EventFormatter::SwingDamage(damageInfo, 0, slot0ok));

        if (hasSlot1)
        {
            InstanceTracker::Instance().WriteForUnit(
                damageInfo->target,
                EventFormatter::SwingDamage(damageInfo, 1, slot1ok));
        }
    }

    // -----------------------------------------------------------------------
    // OnSendSpellNonMeleeDamageLog — spell damage → SPELL_DAMAGE
    // -----------------------------------------------------------------------
    void OnSendSpellNonMeleeDamageLog(SpellNonMeleeDamage const* log,
                                       int32 overkill) override
    {
        if (!log || !log->attacker || !InstanceTracker::Instance().IsEnabled())
            return;

        InstanceTracker::Instance().EnsureUnitInfo(log->attacker);
        InstanceTracker::Instance().EnsureUnitInfo(log->target);

        InstanceTracker::Instance().WriteForUnit(
            log->target, EventFormatter::SpellDamage(log, overkill));
    }

    // -----------------------------------------------------------------------
    // OnSendHealSpellLog — healing → SPELL_HEAL
    // -----------------------------------------------------------------------
    void OnSendHealSpellLog(HealInfo const& healInfo, bool critical) override
    {
        if (!healInfo.GetHealer() || !InstanceTracker::Instance().IsEnabled())
            return;

        InstanceTracker::Instance().EnsureUnitInfo(healInfo.GetHealer());
        InstanceTracker::Instance().EnsureUnitInfo(healInfo.GetTarget());

        InstanceTracker::Instance().WriteForUnit(
            healInfo.GetTarget(), EventFormatter::SpellHeal(healInfo, critical));
    }

    // -----------------------------------------------------------------------
    // OnSendSpellMiss — spell miss → SPELL_MISSED
    // -----------------------------------------------------------------------
    void OnSendSpellMiss(Unit* attacker, Unit* victim, uint32 spellID,
                         SpellMissInfo missInfo) override
    {
        if (!attacker || !InstanceTracker::Instance().IsEnabled())
            return;

        InstanceTracker::Instance().EnsureUnitInfo(attacker);
        InstanceTracker::Instance().EnsureUnitInfo(victim);

        InstanceTracker::Instance().WriteForUnit(
            victim, EventFormatter::SpellMissed(attacker, victim, spellID, missInfo));
    }

    // -----------------------------------------------------------------------
    // OnSendSpellDamageImmune — spell immune → SPELL_MISSED (IMMUNE)
    // -----------------------------------------------------------------------
    void OnSendSpellDamageImmune(Unit* attacker, Unit* victim, uint32 spellId) override
    {
        if (!attacker || !InstanceTracker::Instance().IsEnabled())
            return;

        InstanceTracker::Instance().EnsureUnitInfo(attacker);
        InstanceTracker::Instance().EnsureUnitInfo(victim);

        InstanceTracker::Instance().WriteForUnit(
            victim, EventFormatter::SpellMissed(attacker, victim, spellId, SPELL_MISS_IMMUNE));
    }

    // -----------------------------------------------------------------------
    // OnSendSpellDamageResist — full resist → SPELL_MISSED (RESIST)
    // -----------------------------------------------------------------------
    void OnSendSpellDamageResist(Unit* attacker, Unit* victim, uint32 spellId) override
    {
        if (!attacker || !InstanceTracker::Instance().IsEnabled())
            return;

        InstanceTracker::Instance().EnsureUnitInfo(attacker);
        InstanceTracker::Instance().EnsureUnitInfo(victim);

        InstanceTracker::Instance().WriteForUnit(
            victim, EventFormatter::SpellMissed(attacker, victim, spellId, SPELL_MISS_RESIST));
    }

    // -----------------------------------------------------------------------
    // OnSendSpellNonMeleeReflectLog — reflected spell → SPELL_DAMAGE
    // -----------------------------------------------------------------------
    void OnSendSpellNonMeleeReflectLog(SpellNonMeleeDamage* log,
                                        Unit* /*attacker*/) override
    {
        if (!log || !log->attacker || !InstanceTracker::Instance().IsEnabled())
            return;

        InstanceTracker::Instance().EnsureUnitInfo(log->attacker);
        InstanceTracker::Instance().EnsureUnitInfo(log->target);

        // Overkill must be computed here — the reflect hook does not
        // receive it from the core (only OnSendSpellNonMeleeDamageLog does).
        // Mirror Unit::SendSpellNonMeleeReflectLog so Chronicle stays aligned
        // with the packet the client would have received.
        int32 reflectOverkill = static_cast<int32>(log->damage)
                              - static_cast<int32>(log->target->GetHealth());
        InstanceTracker::Instance().WriteForUnit(
            log->target, EventFormatter::SpellDamage(
                log, reflectOverkill < 0 ? 0 : reflectOverkill));
    }

    // -----------------------------------------------------------------------
    // OnSendEnergizeSpellLog — mana/rage/energy → SPELL_ENERGIZE
    // -----------------------------------------------------------------------
    void OnSendEnergizeSpellLog(Unit* attacker, Unit* victim, uint32 spellID,
                                 uint32 damage, Powers powerType) override
    {
        if (!attacker || !InstanceTracker::Instance().IsEnabled())
            return;

        InstanceTracker::Instance().EnsureUnitInfo(attacker);
        InstanceTracker::Instance().EnsureUnitInfo(victim);

        InstanceTracker::Instance().WriteForUnit(
            victim, EventFormatter::SpellEnergize(attacker, victim, spellID,
                                                   damage, powerType));
    }

    // -----------------------------------------------------------------------
    // OnSendPeriodicAuraLog — periodic tick dispatcher
    // -----------------------------------------------------------------------
    void OnSendPeriodicAuraLog(Unit* victim, SpellPeriodicAuraLogInfo* pInfo) override
    {
        if (!victim || !pInfo || !pInfo->auraEff || !InstanceTracker::Instance().IsEnabled())
            return;

        Unit* caster = pInfo->auraEff->GetCaster();
        InstanceTracker::Instance().EnsureUnitInfo(caster);
        InstanceTracker::Instance().EnsureUnitInfo(victim);

        switch (pInfo->auraEff->GetAuraType())
        {
            case SPELL_AURA_PERIODIC_DAMAGE:
            case SPELL_AURA_PERIODIC_DAMAGE_PERCENT:
            // case SPELL_AURA_PERIODIC_LEECH:  // unreachable — leech uses SendSpellNonMeleeDamageLog + HealBySpell
                InstanceTracker::Instance().WriteForUnit(
                    victim, EventFormatter::SpellPeriodicDamage(victim, pInfo));
                break;
            case SPELL_AURA_PERIODIC_HEAL:
            case SPELL_AURA_OBS_MOD_HEALTH:
                InstanceTracker::Instance().WriteForUnit(
                    victim, EventFormatter::SpellPeriodicHeal(victim, pInfo));
                break;
            case SPELL_AURA_PERIODIC_MANA_LEECH:
                InstanceTracker::Instance().WriteForUnit(
                    victim, EventFormatter::SpellPeriodicDrain(victim, pInfo));
                break;
            case SPELL_AURA_PERIODIC_ENERGIZE:
            case SPELL_AURA_OBS_MOD_POWER:
                InstanceTracker::Instance().WriteForUnit(
                    victim, EventFormatter::SpellPeriodicEnergize(victim, pInfo));
                break;
            default:
                break;
        }
    }

    // -----------------------------------------------------------------------
    // OnDealDamageShieldDamage — damage shield (thorns etc.) → DAMAGE_SHIELD
    // -----------------------------------------------------------------------
    void OnDealDamageShieldDamage(Unit* shieldOwner, Unit* attacker,
        SpellInfo const* spellInfo, uint32 damage, uint32 absorb,
        uint32 overkill) override
    {
        if (!shieldOwner || !InstanceTracker::Instance().IsEnabled())
            return;

        InstanceTracker::Instance().EnsureUnitInfo(shieldOwner);
        InstanceTracker::Instance().EnsureUnitInfo(attacker);

        InstanceTracker::Instance().WriteForUnit(
            attacker, EventFormatter::DamageShield(shieldOwner, attacker, spellInfo, damage, absorb, overkill));
    }

    // -----------------------------------------------------------------------
    // OnUnitDeath — unit died → UNIT_DIED
    // -----------------------------------------------------------------------
    void OnUnitDeath(Unit* unit, Unit* killer) override
    {
        if (!unit || !InstanceTracker::Instance().IsEnabled())
            return;

        InstanceTracker::Instance().EnsureUnitInfo(unit);
        InstanceTracker::Instance().EnsureUnitInfo(killer);

        InstanceTracker::Instance().WriteForUnit(
            unit, EventFormatter::UnitDied(killer, unit));
    }

    // -----------------------------------------------------------------------
    // OnUnitEnterEvadeMode — creature resets → CHRONICLE_UNIT_EVADE
    // -----------------------------------------------------------------------
    void OnUnitEnterEvadeMode(Unit* unit, uint8 evadeReason) override
    {
        if (!unit || !InstanceTracker::Instance().IsEnabled())
            return;

        InstanceTracker::Instance().EnsureUnitInfo(unit);

        InstanceTracker::Instance().WriteForUnit(
            unit, EventFormatter::UnitEvade(unit, evadeReason));
    }

    // -----------------------------------------------------------------------
    // OnSchoolAbsorbApplied — absorb aura soaked damage → SPELL_ABSORBED
    // -----------------------------------------------------------------------
    void OnSchoolAbsorbApplied(DamageInfo& dmgInfo, SpellInfo const* absorbSpellInfo,
                           Unit* absorbCaster, uint32 absorbAmount) override
    {
        if (!absorbSpellInfo || !absorbAmount ||
            !InstanceTracker::Instance().IsEnabled())
            return;

        Unit* attacker = dmgInfo.GetAttacker();
        Unit* victim   = dmgInfo.GetVictim();

        if (attacker)
            InstanceTracker::Instance().EnsureUnitInfo(attacker);
        if (victim)
            InstanceTracker::Instance().EnsureUnitInfo(victim);
        if (absorbCaster)
            InstanceTracker::Instance().EnsureUnitInfo(absorbCaster);

        InstanceTracker::Instance().WriteForUnit(
            victim, EventFormatter::SpellAbsorbed(
                dmgInfo, absorbSpellInfo, absorbCaster, absorbAmount));
    }

    // -----------------------------------------------------------------------
    // OnUnitEnterCombat — unit enters combat → CHRONICLE_UNIT_COMBAT
    // -----------------------------------------------------------------------
    void OnUnitEnterCombat(Unit* unit, Unit* victim) override
    {
        if (!unit || !InstanceTracker::Instance().IsEnabled())
            return;

        InstanceTracker::Instance().EnsureUnitInfo(unit);
        InstanceTracker::Instance().EnsureUnitInfo(victim);

        InstanceTracker::Instance().WriteForUnit(
            unit, EventFormatter::UnitCombat(unit, victim));
    }
};

// ===========================================================================
// GlobalScript — captures spell casts and aura application/removal.
// ===========================================================================
class ChronicleGlobalScript : public GlobalScript
{
public:
    ChronicleGlobalScript() : GlobalScript("ChronicleGlobalScript", {
        GLOBALHOOK_ON_SPELL_SEND_SPELL_GO,
        GLOBALHOOK_ON_AURA_APPLICATION_CLIENT_UPDATE,
        GLOBALHOOK_ON_SPELL_EXECUTE_LOG_SUMMON_OBJECT,
        GLOBALHOOK_ON_BEFORE_SET_BOSS_STATE,
        GLOBALHOOK_ON_AFTER_UPDATE_ENCOUNTER_STATE,
        GLOBALHOOK_ON_SPELL_INTERRUPT,
        GLOBALHOOK_ON_SPELL_DISPEL,
        GLOBALHOOK_ON_INSTANCEID_REMOVED,
    }) { }

    // -----------------------------------------------------------------------
    // OnSpellSendSpellGo — spell cast completed → SPELL_CAST_SUCCESS
    // and CHRONICLE_SPELL_TARGET_RESULT for packet-aligned per-target outcomes.
    // -----------------------------------------------------------------------
    void OnSpellSendSpellGo(Spell* spell) override
    {
        if (!spell || !InstanceTracker::Instance().IsEnabled())
            return;

        Unit* caster = spell->GetCaster();
        if (!caster)
            return;

        WorldObject* explicitTarget = spell->m_targets.GetObjectTarget();
        if (!explicitTarget)
            explicitTarget = spell->GetOriginalTarget();
        SpellInfo const* spellInfo = spell->m_spellInfo;
        if (!spellInfo)
            return;

        InstanceTracker::Instance().EnsureUnitInfo(caster);
        if (Unit* targetUnit = explicitTarget ? explicitTarget->ToUnit() : nullptr)
            InstanceTracker::Instance().EnsureUnitInfo(targetUnit);

        InstanceTracker::Instance().WriteForUnit(
            caster, EventFormatter::SpellCastSuccess(caster, explicitTarget, spellInfo));

        auto const* uniqueTargets = spell->GetUniqueTargetInfo();
        if (!uniqueTargets)
            return;

        for (TargetInfo const& targetInfo : *uniqueTargets)
        {
            SpellMissInfo missInfo = targetInfo.missCondition;
            if (missInfo == SPELL_MISS_NONE && targetInfo.effectMask == 0)
                missInfo = SPELL_MISS_IMMUNE2;

            Unit* targetUnit = caster->GetGUID() == targetInfo.targetGUID
                ? caster
                : ObjectAccessor::GetUnit(*caster, targetInfo.targetGUID);
            if (!targetUnit)
                continue;

            InstanceTracker::Instance().EnsureUnitInfo(targetUnit);
            InstanceTracker::Instance().WriteForUnit(
                targetUnit, EventFormatter::SpellTargetResult(
                    caster, targetUnit, spellInfo, missInfo,
                    targetInfo.reflectResult, targetInfo.effectMask));
        }
    }

    // -----------------------------------------------------------------------
    // OnSpellExecuteLogSummonObject — creature/object summoned → SPELL_SUMMON
    // -----------------------------------------------------------------------
    void OnSpellExecuteLogSummonObject(Spell* spell, WorldObject* obj) override
    {
        if (!spell || !spell->GetCaster() || !obj || !InstanceTracker::Instance().IsEnabled())
            return;

        Unit* caster = spell->GetCaster();
        SpellInfo const* spellInfo = spell->m_spellInfo;
        if (!spellInfo)
            return;

        InstanceTracker::Instance().EnsureUnitInfo(caster);

        InstanceTracker::Instance().WriteForUnit(
            caster, EventFormatter::SpellSummon(caster, spellInfo, obj));
    }

    // -----------------------------------------------------------------------
    // OnAuraApplicationClientUpdate — aura applied/removed
    // -----------------------------------------------------------------------
    void OnAuraApplicationClientUpdate(Unit* target, Aura* aura, bool remove) override
    {
        if (!target || !aura || !InstanceTracker::Instance().IsEnabled())
            return;

        Unit* caster = aura->GetCaster();
        SpellInfo const* spell = aura->GetSpellInfo();
        if (!spell)
            return;

        InstanceTracker::Instance().EnsureUnitInfo(caster);
        InstanceTracker::Instance().EnsureUnitInfo(target);

        if (remove)
        {
            InstanceTracker::Instance().WriteForUnit(
                target, EventFormatter::SpellAuraRemoved(caster, target, spell));
        }
        else
        {
            InstanceTracker::Instance().WriteForUnit(
                target, EventFormatter::SpellAuraApplied(caster, target, spell));
        }
    }

    // -----------------------------------------------------------------------
    // OnBeforeSetBossState — encounter pull / kill / wipe
    // -----------------------------------------------------------------------
    void OnBeforeSetBossState(uint32 id, EncounterState newState,
                              EncounterState oldState, Map* instance) override
    {
        if (!instance || !InstanceTracker::Instance().IsEnabled())
            return;

        if (newState == IN_PROGRESS && oldState != IN_PROGRESS)
        {
            InstanceTracker::Instance().WriteForMap(instance,
                EventFormatter::EncounterStart(id, instance));
        }
        else if (oldState == IN_PROGRESS && newState == DONE)
        {
            InstanceTracker::Instance().WriteForMap(instance,
                EventFormatter::EncounterEnd(id, instance, /*success=*/true));
            InstanceTracker::Instance().FlushMap(instance);
        }
        else if (oldState == IN_PROGRESS && newState == FAIL)
        {
            InstanceTracker::Instance().WriteForMap(instance,
                EventFormatter::EncounterEnd(id, instance, /*success=*/false));
            InstanceTracker::Instance().FlushMap(instance);
        }
    }

    // -----------------------------------------------------------------------
    // OnAfterUpdateEncounterState — rich encounter credit with DBC names
    // -----------------------------------------------------------------------
    void OnAfterUpdateEncounterState(Map* map, EncounterCreditType type,
        uint32 creditEntry, Unit* source, Difficulty difficulty,
        DungeonEncounterList const* encounters, uint32 dungeonCompleted,
        bool updated) override
    {
        if (!map || !updated || !InstanceTracker::Instance().IsEnabled())
            return;

        // Find the encounter that matches this creditEntry
        std::string encounterName;
        uint32 encounterDbcId = 0;
        if (encounters)
        {
            for (auto const* enc : *encounters)
            {
                if (enc->creditType == type && enc->creditEntry == creditEntry)
                {
                    encounterName = enc->dbcEntry->encounterName[0]; // English
                    encounterDbcId = enc->dbcEntry->id;
                    break;
                }
            }
        }

        InstanceTracker::Instance().WriteForMap(map,
            EventFormatter::EncounterCredit(
                map, type, creditEntry, source, difficulty,
                encounterDbcId, encounterName, dungeonCompleted));

        if (InstanceTracker::Instance().ShouldSnapshotOnEncounterCredit())
            InstanceTracker::Instance().UploadInstanceSnapshot(map);
    }

    // -----------------------------------------------------------------------
    // OnSpellInterrupt — spell interrupt (Kick, Counterspell, Pummel, etc.)
    // -----------------------------------------------------------------------
    void OnSpellInterrupt(Unit* interrupter, Unit* interrupted,
        uint32 interruptSpellId, uint32 interruptedSpellId) override
    {
        if (!interrupter || !interrupted || !InstanceTracker::Instance().IsEnabled())
            return;

        InstanceTracker::Instance().EnsureUnitInfo(interrupter);
        InstanceTracker::Instance().EnsureUnitInfo(interrupted);

        InstanceTracker::Instance().WriteForUnit(
            interrupted, EventFormatter::SpellInterrupt(
                interrupter, interrupted, interruptSpellId, interruptedSpellId));
    }

    void OnSpellDispel(Unit* dispeller, Unit* victim,
        uint32 dispelSpellId, uint32 removedSpellId, bool isSteal) override
    {
        if (!dispeller || !victim || !InstanceTracker::Instance().IsEnabled())
            return;

        InstanceTracker::Instance().EnsureUnitInfo(dispeller);
        InstanceTracker::Instance().EnsureUnitInfo(victim);

        InstanceTracker::Instance().WriteForUnit(
            victim, EventFormatter::SpellDispel(
                dispeller, victim, dispelSpellId, removedSpellId, isSteal));
    }

    void OnInstanceIdRemoved(uint32 instanceId) override
    {
        InstanceTracker::Instance().OnInstanceIdRemoved(instanceId);
    }
};

// ===========================================================================
// PlayerScript — captures environmental damage (fall, lava, etc.).
// ===========================================================================
class ChroniclePlayerScript : public PlayerScript
{
public:
    ChroniclePlayerScript() : PlayerScript("ChroniclePlayerScript", {
        PLAYERHOOK_ON_ENVIRONMENTAL_DAMAGE,
        PLAYERHOOK_ON_LOOT_ITEM,
    }) { }

    void OnEnvironmentalDamage(Player* player, EnviromentalDamage type,
                                uint32 damage) override
    {
        if (!player || !InstanceTracker::Instance().IsEnabled())
            return;

        InstanceTracker::Instance().EnsureUnitInfo(player);

        InstanceTracker::Instance().WriteForUnit(
            player, EventFormatter::EnvironmentalDamage(player, type, damage));
    }

    void OnPlayerLootItem(Player* player, Item* item, uint32 count,
                          ObjectGuid lootGuid) override
    {
        if (!player || !item || !InstanceTracker::Instance().IsEnabled())
            return;

        InstanceTracker::Instance().EnsureUnitInfo(player);
        if (!lootGuid.IsEmpty() && lootGuid.IsCreatureOrVehicle())
            if (Creature* lootUnit = player->GetMap() ? player->GetMap()->GetCreature(lootGuid) : nullptr)
                InstanceTracker::Instance().EnsureUnitInfo(lootUnit);

        InstanceTracker::Instance().WriteForUnit(
            player, EventFormatter::LootItem(player, lootGuid, item, count));
    }
};

class ChronicleLootScript : public LootScript
{
public:
    ChronicleLootScript() : LootScript("ChronicleLootScript", {
        LOOTHOOK_ON_LOOT_MONEY,
    }) { }

    void OnLootMoney(Player* player, uint32 gold) override
    {
        if (!player || !gold || !InstanceTracker::Instance().IsEnabled())
            return;

        ObjectGuid lootGuid = player->GetLootGUID();
        InstanceTracker::Instance().EnsureUnitInfo(player);
        if (!lootGuid.IsEmpty() && lootGuid.IsCreatureOrVehicle())
            if (Creature* lootUnit = player->GetMap() ? player->GetMap()->GetCreature(lootGuid) : nullptr)
                InstanceTracker::Instance().EnsureUnitInfo(lootUnit);

        InstanceTracker::Instance().WriteForUnit(
            player, EventFormatter::LootMoney(player, lootGuid, gold));
    }
};

// ===========================================================================
// AllMapScript — captures player entering/leaving instances.
// ===========================================================================
class ChronicleAllMapScript : public AllMapScript
{
public:
    ChronicleAllMapScript() : AllMapScript("ChronicleAllMapScript", {
        ALLMAPHOOK_ON_PLAYER_ENTER_ALL,
        ALLMAPHOOK_ON_PLAYER_LEAVE_ALL,
        ALLMAPHOOK_ON_DESTROY_INSTANCE,
    }) { }

    void OnPlayerEnterAll(Map* map, Player* player) override
    {
        if (!map || !player || !map->IsDungeon())
            return;

        InstanceTracker::Instance().OnPlayerEnterInstance(map, player);
    }

    void OnPlayerLeaveAll(Map* map, Player* player) override
    {
        if (!map || !player || !map->IsDungeon())
            return;

        InstanceTracker::Instance().OnPlayerLeaveInstance(map, player);
    }

    void OnDestroyInstance(MapInstanced* /*mapInstanced*/, Map* map) override
    {
        if (!map)
            return;

        InstanceTracker::Instance().RemoveInstance(map->GetInstanceId());
    }
};

// ===========================================================================
// WorldScript — loads config on startup / reload.
// ===========================================================================
class ChronicleWorldScript : public WorldScript
{
public:
    ChronicleWorldScript() : WorldScript("ChronicleWorldScript", {
        WORLDHOOK_ON_BEFORE_CONFIG_LOAD,
        WORLDHOOK_ON_STARTUP,
        WORLDHOOK_ON_SHUTDOWN,
    }) { }

    void OnBeforeConfigLoad(bool /*reload*/) override
    {
        InstanceTracker::Instance().LoadConfig();
    }

    void OnStartup() override
    {
        auto& tracker = InstanceTracker::Instance();
        if (!tracker.IsEnabled())
            return;

        std::string url    = tracker.GetUploadURL();
        std::string secret = tracker.GetUploadSecret();
        if (url.empty() || secret.empty())
        {
            LOG_WARN("module", "Chronicle: skipping startup ping — UploadURL or UploadSecret not configured");
            return;
        }

        // Upload any orphaned logs left over from a crash / unclean shutdown.
        tracker.UploadOrphanedLogs();

        // Run ping asynchronously without detached threads.
        tracker.QueuePingTask(url, secret);
    }

    void OnShutdown() override
    {
        InstanceTracker::Instance().Shutdown();
    }
};

// ===========================================================================
// Script registration — called from MP_loader.cpp
// ===========================================================================
void AddChronicleScripts()
{
    new ChronicleUnitScript();
    new ChronicleGlobalScript();
    new ChroniclePlayerScript();
    new ChronicleLootScript();
    new ChronicleAllMapScript();
    new ChronicleWorldScript();
}
