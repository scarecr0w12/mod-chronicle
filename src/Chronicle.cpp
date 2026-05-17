/*
 * Copyright (C) 2024+ Chronicle <https://github.com/Emyrk/chronicle>
 * Released under GNU AGPL v3 license
 */

#include "Chronicle.h"

#include "Config.h"
#include "Creature.h"
#include "GameTime.h"
#include "Guild.h"
#include "Item.h"
#include "Log.h"
#include "Map.h"
#include "Pet.h"
#include "Player.h"
#include "Random.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "Util.h"
#include "World.h"
#include "DBCStores.h"

#if __has_include("mythic_plus.h")
#include "mythic_plus.h"
#define MOD_CHRONICLE_HAS_MYTHIC_PLUS 1
#else
#define MOD_CHRONICLE_HAS_MYTHIC_PLUS 0
#endif

#if __has_include("DungeonMasterMgr.h")
#include "DungeonMasterMgr.h"
#define MOD_CHRONICLE_HAS_DUNGEON_MASTER 1
#else
#define MOD_CHRONICLE_HAS_DUNGEON_MASTER 0
#endif

#include <zlib.h>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>

#include <openssl/err.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <thread>

// ===== EventFormatter =====

std::string EventFormatter::Guid(ObjectGuid guid)
{
    char buf[20];
    snprintf(buf, sizeof(buf), "0x%016llX",
             static_cast<unsigned long long>(guid.GetRawValue()));
    return buf;
}

uint64 EventFormatter::Now()
{
    // Real wall-clock milliseconds since Unix epoch.
    auto now = std::chrono::system_clock::now();
    return static_cast<uint64>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch())
            .count());
}

namespace
{
constexpr uint32 COMBATLOG_OBJECT_AFFILIATION_MINE     = 0x0001;
constexpr uint32 COMBATLOG_OBJECT_AFFILIATION_PARTY    = 0x0002;
constexpr uint32 COMBATLOG_OBJECT_AFFILIATION_RAID     = 0x0004;
constexpr uint32 COMBATLOG_OBJECT_AFFILIATION_OUTSIDER = 0x0008;
constexpr uint32 COMBATLOG_OBJECT_REACTION_FRIENDLY    = 0x0010;
constexpr uint32 COMBATLOG_OBJECT_REACTION_NEUTRAL     = 0x0020;
constexpr uint32 COMBATLOG_OBJECT_REACTION_HOSTILE     = 0x0040;
constexpr uint32 COMBATLOG_OBJECT_CONTROL_PLAYER       = 0x0100;
constexpr uint32 COMBATLOG_OBJECT_CONTROL_NPC          = 0x0200;
constexpr uint32 COMBATLOG_OBJECT_TYPE_PLAYER          = 0x0400;
constexpr uint32 COMBATLOG_OBJECT_TYPE_NPC             = 0x0800;
constexpr uint32 COMBATLOG_OBJECT_TYPE_PET             = 0x1000;
constexpr uint32 COMBATLOG_OBJECT_TYPE_GUARDIAN        = 0x2000;

uint32 BuildUnitFlags(Unit* unit, Unit const* relation)
{
    if (!unit)
        return 0;

    uint32 flags = 0;

    if (unit->IsPlayer())
        flags |= COMBATLOG_OBJECT_TYPE_PLAYER;
    else if (unit->IsGuardian())
        flags |= COMBATLOG_OBJECT_TYPE_GUARDIAN;
    else if (unit->IsPet())
        flags |= COMBATLOG_OBJECT_TYPE_PET;
    else
        flags |= COMBATLOG_OBJECT_TYPE_NPC;

    flags |= unit->IsControlledByPlayer() ? COMBATLOG_OBJECT_CONTROL_PLAYER : COMBATLOG_OBJECT_CONTROL_NPC;
    flags |= COMBATLOG_OBJECT_AFFILIATION_OUTSIDER;
    flags |= COMBATLOG_OBJECT_REACTION_NEUTRAL;

    if (!relation)
        return flags;

    flags &= ~(COMBATLOG_OBJECT_REACTION_FRIENDLY | COMBATLOG_OBJECT_REACTION_NEUTRAL | COMBATLOG_OBJECT_REACTION_HOSTILE);
    if (unit == relation || unit->IsFriendlyTo(relation) || relation->IsFriendlyTo(unit))
        flags |= COMBATLOG_OBJECT_REACTION_FRIENDLY;
    else if (unit->IsHostileTo(relation) || relation->IsHostileTo(unit))
        flags |= COMBATLOG_OBJECT_REACTION_HOSTILE;
    else
        flags |= COMBATLOG_OBJECT_REACTION_NEUTRAL;

    Player const* owner = unit->GetCharmerOrOwnerPlayerOrPlayerItself();
    Player const* otherOwner = relation->GetCharmerOrOwnerPlayerOrPlayerItself();
    if (!owner || !otherOwner)
        return flags;

    flags &= ~(COMBATLOG_OBJECT_AFFILIATION_MINE | COMBATLOG_OBJECT_AFFILIATION_PARTY |
               COMBATLOG_OBJECT_AFFILIATION_RAID | COMBATLOG_OBJECT_AFFILIATION_OUTSIDER);

    if (owner == otherOwner)
        flags |= COMBATLOG_OBJECT_AFFILIATION_MINE;
    else if (owner->IsInSameGroupWith(otherOwner))
        flags |= COMBATLOG_OBJECT_AFFILIATION_PARTY;
    else if (owner->IsInSameRaidWith(otherOwner))
        flags |= COMBATLOG_OBJECT_AFFILIATION_RAID;
    else
        flags |= COMBATLOG_OBJECT_AFFILIATION_OUTSIDER;

    return flags;
}

uint32 WorldObjectFlags(WorldObject* object, Unit const* relation)
{
    if (Unit* unit = object ? object->ToUnit() : nullptr)
        return BuildUnitFlags(unit, relation);

    return 0;
}

std::string WorldObjectGuid(WorldObject* object)
{
    return EventFormatter::Guid(object ? object->GetGUID() : ObjectGuid::Empty);
}

std::string WorldObjectName(WorldObject* object)
{
    return object ? object->GetName() : "";
}

void AppendObjectParams(std::ostringstream& ss, WorldObject* object, Unit const* relation)
{
    ss << WorldObjectGuid(object)
       << ",\"" << WorldObjectName(object) << "\""
       << ",0x" << std::hex << WorldObjectFlags(object, relation) << std::dec;
}

bool StartsWith(std::string const& value, char const* prefix)
{
    return value.rfind(prefix, 0) == 0;
}

Player* GetAffiliationReferencePlayer(Unit* unit)
{
    if (!unit)
        return nullptr;

    if (Player* player = unit->GetCharmerOrOwnerPlayerOrPlayerItself())
        return player;

    Map* map = unit->FindMap();
    if (!map)
        return nullptr;

    for (MapReference const& ref : map->GetPlayers())
        if (Player* player = ref.GetSource())
            return player;

    return nullptr;
}

char const* UnitAffiliationToString(Unit* unit, Unit const* relation)
{
    if (!unit)
        return "NEUTRAL";

    if (relation)
    {
        if (unit == relation || unit->IsFriendlyTo(relation) || relation->IsFriendlyTo(unit))
            return "FRIENDLY";

        if (unit->IsHostileTo(relation) || relation->IsHostileTo(unit))
            return "HOSTILE";
    }

    return "NEUTRAL";
}

bool IsBossUnit(Unit* unit)
{
    if (Creature* creature = unit ? unit->ToCreature() : nullptr)
        return creature->isWorldBoss() || creature->IsDungeonBoss();

    return false;
}
}

// ---------------------------------------------------------------------------
// UnitFlags — compute WotLK COMBATLOG_OBJECT_* flags from Unit properties.
// This is incomplete compared to the client side logs.
// ---------------------------------------------------------------------------
uint32 EventFormatter::UnitFlags(Unit* unit)
{
    return BuildUnitFlags(unit, nullptr);
}

// ---------------------------------------------------------------------------
// BaseParams — the 6 fields present on every standard WotLK event.
// ---------------------------------------------------------------------------
std::string EventFormatter::BaseParams(Unit* source, Unit* dest)
{
    std::ostringstream ss;
    AppendObjectParams(ss, source, dest);
    ss << ",";
    AppendObjectParams(ss, dest, source);
    return ss.str();
}

// ===== Chronicle Extension Events (CHRONICLE_*) =====

static std::string ClassToString(uint8 cls)
{
    switch (cls)
    {
        case 1:  return "WARRIOR";
        case 2:  return "PALADIN";
        case 3:  return "HUNTER";
        case 4:  return "ROGUE";
        case 5:  return "PRIEST";
        case 6:  return "DEATHKNIGHT";
        case 7:  return "SHAMAN";
        case 8:  return "MAGE";
        case 9:  return "WARLOCK";
        case 11: return "DRUID";
        default: return "UNKNOWN";
    }
}

static std::string RaceToString(uint8 race)
{
    switch (race)
    {
        case 1:  return "Human";
        case 2:  return "Orc";
        case 3:  return "Dwarf";
        case 4:  return "NightElf";
        case 5:  return "Scourge";
        case 6:  return "Tauren";
        case 7:  return "Gnome";
        case 8:  return "Troll";
        case 10: return "BloodElf";
        case 11: return "Draenei";
        default: return "Unknown";
    }
}

// ---------------------------------------------------------------------------
// CHRONICLE_HEADER — emitted once when log file is created.
// ---------------------------------------------------------------------------
std::string EventFormatter::Header(std::string const& realmName)
{
    std::ostringstream ss;
    ss << Now() << "  CHRONICLE_HEADER"
       << ",\"" << realmName << "\""
       << ",\"3.3.5a\""
       << ",12340";
    return ss.str();
}

// ---------------------------------------------------------------------------
// CHRONICLE_ZONE_INFO — emitted once per instance.
// ---------------------------------------------------------------------------
std::string EventFormatter::ZoneInfo(std::string const& zoneName, uint32 mapId,
                                     uint32 instanceId, std::string const& instanceType,
                                     std::string const& runType)
{
    std::ostringstream ss;
    ss << Now() << "  CHRONICLE_ZONE_INFO"
       << ",\"" << zoneName << "\""
       << "," << mapId
       << "," << instanceId
       << ",\"" << instanceType << "\""
       << ",\"" << runType << "\"";
    return ss.str();
}

// ---------------------------------------------------------------------------
// BuildTalentString — produces the same format as the vanilla Lua addon:
//   "215303100000000000}055051000050122231}00000000000000000000"
// One digit per talent slot (the invested rank, 0 if unlearned), three tabs
// separated by '}', talents ordered by (Row, Col) within each tab.
// ---------------------------------------------------------------------------
static std::string BuildTalentString(Player* player)
{
    uint32 const* tabPages = GetTalentTabPages(player->getClass());
    if (!tabPages)
        return "";

    uint8 activeSpec = player->GetActiveSpec();
    const PlayerTalentMap& talentMap = player->GetTalentMap();

    std::string result;
    for (uint8 tab = 0; tab < 3; ++tab)
    {
        if (tab > 0)
            result += '}';

        uint32 tabId = tabPages[tab];
        if (!tabId)
            continue;

        // Collect talents for this tab.
        struct TalentSlot { uint32 row; uint32 col; uint8 rank; };
        std::vector<TalentSlot> slots;

        for (uint32 i = 0; i < sTalentStore.GetNumRows(); ++i)
        {
            TalentEntry const* talent = sTalentStore.LookupEntry(i);
            if (!talent || talent->TalentTab != tabId)
                continue;

            // Determine the player's rank in this talent (0 = unlearned).
            uint8 rank = 0;
            for (uint8 r = MAX_TALENT_RANK; r > 0; --r)
            {
                uint32 spellId = talent->RankID[r - 1];
                if (!spellId)
                    continue;
                auto itr = talentMap.find(spellId);
                if (itr != talentMap.end()
                    && itr->second->State != PLAYERSPELL_REMOVED
                    && itr->second->IsInSpec(activeSpec))
                {
                    rank = r;
                    break;
                }
            }

            slots.push_back({ talent->Row, talent->Col, rank });
        }

        // Sort by (Row, Col) to match the Lua GetTalentInfo ordering.
        std::sort(slots.begin(), slots.end(), [](const TalentSlot& a, const TalentSlot& b) {
            return a.row != b.row ? a.row < b.row : a.col < b.col;
        });

        for (auto const& s : slots)
            result += static_cast<char>('0' + s.rank);
    }

    return result;
}

// ---------------------------------------------------------------------------
// CHRONICLE_COMBATANT_INFO — emitted when a player enters the instance.
// ---------------------------------------------------------------------------
std::string EventFormatter::CombatantInfo(Player* player)
{
    std::ostringstream ss;
    ss << Now() << "  CHRONICLE_COMBATANT_INFO"
       << "," << Guid(player->GetGUID())
       << ",\"" << player->GetName() << "\""
       << ",\"" << ClassToString(player->getClass()) << "\""
       << ",\"" << RaceToString(player->getRace()) << "\""
       << "," << static_cast<int>(player->getGender())
       << "," << static_cast<int>(player->GetLevel());

    Guild* guild = player->GetGuild();
    ss << ",\"" << (guild ? guild->GetName() : "") << "\"";

    // Gear: 19 slots, &-separated, each as itemId:enchantId:suffixId:0
    ss << ",\"";
    for (uint8 slot = 0; slot < 19; ++slot)
    {
        if (slot > 0)
            ss << "&";
        Item* item = player->GetItemByPos(255 /*INVENTORY_SLOT_BAG_0*/, slot);
        if (item)
        {
            char buf[64];
            snprintf(buf, sizeof(buf), "%u:%u:%d:0",
                     item->GetEntry(),
                     item->GetEnchantmentId(PERM_ENCHANTMENT_SLOT),
                     item->GetItemRandomPropertyId());
            ss << buf;
        }
        else
        {
            ss << "0";
        }
    }
    ss << "\"";

    // Talents: "ranks}ranks}ranks" — one digit per talent per tab, '}'-separated.
    ss << ",\"" << BuildTalentString(player) << "\"";

    return ss.str();
}

// ---------------------------------------------------------------------------
// CHRONICLE_UNIT_INFO — emitted first time a GUID appears in combat.
// ---------------------------------------------------------------------------
std::string EventFormatter::UnitInfo(Unit* unit)
{
    Player* relation = GetAffiliationReferencePlayer(unit);
    uint32 unitFlags = BuildUnitFlags(unit, relation);

    std::ostringstream ss;
    ss << Now() << "  CHRONICLE_UNIT_INFO"
       << "," << Guid(unit->GetGUID())
       << ",\"" << unit->GetName() << "\""
       << "," << static_cast<int>(unit->GetLevel())
       << ",0x" << std::hex << unitFlags << std::dec;

    ObjectGuid ownerGuid = unit->GetOwnerGUID();
    if (!ownerGuid.IsEmpty())
        ss << "," << Guid(ownerGuid);
    else
        ss << ",0x0000000000000000";

    ss << "," << unit->GetMaxHealth()
       << ",\"" << UnitAffiliationToString(unit, relation) << "\""
       << "," << (IsBossUnit(unit) ? "true" : "false");
    return ss.str();
}

// ---------------------------------------------------------------------------
// CHRONICLE_UNIT_EVADE — emitted when a creature enters evade mode.
// Fields: guid, "name", why (EvadeReason enum)
// ---------------------------------------------------------------------------
std::string EventFormatter::UnitEvade(Unit* unit, uint8 evadeReason)
{
    std::ostringstream ss;
    ss << Now() << "  CHRONICLE_UNIT_EVADE"
       << "," << Guid(unit->GetGUID())
       << ",\"" << unit->GetName() << "\""
       << "," << static_cast<int>(evadeReason);
    return ss.str();
}

// ---------------------------------------------------------------------------
// CHRONICLE_UNIT_COMBAT — emitted when a unit enters combat with a victim.
// Fields: unitGuid, "unitName", victimGuid, "victimName"
// ---------------------------------------------------------------------------
std::string EventFormatter::UnitCombat(Unit* unit, Unit* victim)
{
    std::ostringstream ss;
    ss << Now() << "  CHRONICLE_UNIT_COMBAT"
       << "," << Guid(unit->GetGUID())
       << ",\"" << unit->GetName() << "\""
       << "," << Guid(victim ? victim->GetGUID() : ObjectGuid::Empty)
       << ",\"" << (victim ? victim->GetName() : "") << "\"";
    return ss.str();
}

// ---------------------------------------------------------------------------
// CHRONICLE_UNIT_DESPAWN — emitted when a creature despawns alive without a
// UNIT_DIED event (e.g. scripted vanish/unsummon/forced despawn).
// Fields: guid, "name"
// ---------------------------------------------------------------------------
std::string EventFormatter::UnitDespawn(Creature* creature)
{
    std::ostringstream ss;
    ss << Now() << "  CHRONICLE_UNIT_DESPAWN"
       << "," << Guid(creature->GetGUID())
       << ",\"" << creature->GetName() << "\"";
    return ss.str();
}

// ===== Standard WotLK Combat Events =====

// Helper: map MeleeHitOutcome to miss-type string.
static const char* MeleeHitOutcomeToMissType(MeleeHitOutcome outcome)
{
    switch (outcome)
    {
        case MELEE_HIT_MISS:    return "MISS";
        case MELEE_HIT_DODGE:   return "DODGE";
        case MELEE_HIT_PARRY:   return "PARRY";
        case MELEE_HIT_BLOCK:   return "BLOCK";
        case MELEE_HIT_EVADE:   return "EVADE";
        default:                return "MISS";
    }
}

// Helper: map SpellMissInfo to miss-type string.
static const char* SpellMissInfoToString(SpellMissInfo info)
{
    switch (info)
    {
        case SPELL_MISS_MISS:    return "MISS";
        case SPELL_MISS_RESIST:  return "RESIST";
        case SPELL_MISS_DODGE:   return "DODGE";
        case SPELL_MISS_PARRY:   return "PARRY";
        case SPELL_MISS_BLOCK:   return "BLOCK";
        case SPELL_MISS_EVADE:   return "EVADE";
        case SPELL_MISS_IMMUNE:
        case SPELL_MISS_IMMUNE2: return "IMMUNE";
        case SPELL_MISS_DEFLECT: return "DEFLECT";
        case SPELL_MISS_ABSORB:  return "ABSORB";
        case SPELL_MISS_REFLECT: return "REFLECT";
        default:                 return "MISS";
    }
}

static const char* SpellTargetResultToString(SpellMissInfo info)
{
    return info == SPELL_MISS_NONE ? "HIT" : SpellMissInfoToString(info);
}

static WorldObject* ResolveLootSourceObject(Player* looter, ObjectGuid lootGuid)
{
    if (!looter)
        return nullptr;

    Map* map = looter->GetMap();
    if (!map)
        return nullptr;

    if (lootGuid.IsCreatureOrVehicle())
        return map->GetCreature(lootGuid);

    if (lootGuid.IsGameObject())
        return map->GetGameObject(lootGuid);

    return nullptr;
}

static const char* LootSourceTypeToString(ObjectGuid lootGuid)
{
    if (lootGuid.IsCreatureOrVehicle())
        return "CREATURE";
    if (lootGuid.IsGameObject())
        return "GAMEOBJECT";
    if (lootGuid.IsItem())
        return "ITEM";
    if (lootGuid.IsCorpse())
        return "CORPSE";

    return "UNKNOWN";
}

static void AppendLootSourceParams(std::ostringstream& ss, Player* looter, ObjectGuid lootGuid)
{
    if (WorldObject* source = ResolveLootSourceObject(looter, lootGuid))
    {
        AppendObjectParams(ss, source, looter);
        return;
    }

    if (Item* container = looter ? looter->GetItemByGuid(lootGuid) : nullptr)
    {
        ss << EventFormatter::Guid(container->GetGUID())
           << ",\"" << container->GetTemplate()->Name1 << "\""
           << ",0x0";
        return;
    }

    ss << EventFormatter::Guid(lootGuid)
       << ",\"\""
       << ",0x0";
}

// Helper: emit the spell prefix triple: spellId,"spellName",spellSchool(hex)
static void AppendSpellPrefix(std::ostringstream& ss, uint32 spellId,
                               char const* spellName, uint32 schoolMask)
{
    ss << "," << spellId
       << ",\"" << (spellName ? spellName : "") << "\""
       << ",0x" << std::hex << schoolMask << std::dec;
}

// Helper: emit damage suffix: amount,overkill,school,resisted,blocked,absorbed,critical,glancing,crushing
static void AppendDamageSuffix(std::ostringstream& ss, uint32 amount,
                                int32 overkill, uint32 school,
                                uint32 resisted, uint32 blocked, uint32 absorbed,
                                bool critical, bool glancing, bool crushing)
{
    ss << "," << amount
       << "," << (overkill > 0 ? overkill : 0)
       << "," << school
       << "," << resisted
       << "," << blocked
       << "," << absorbed
       << "," << (critical  ? "1" : "nil")
       << "," << (glancing  ? "1" : "nil")
       << "," << (crushing  ? "1" : "nil");
}

// ---------------------------------------------------------------------------
// SWING_DAMAGE — melee auto-attack hit.
// ---------------------------------------------------------------------------
std::string EventFormatter::SwingDamage(CalcDamageInfo const* damageInfo,
                                        uint8 slot, int32 overkill)
{
    Unit* attacker = damageInfo->attacker;
    Unit* target   = damageInfo->target;
    uint32 amount   = damageInfo->damages[slot].damage;
    uint32 school   = damageInfo->damages[slot].damageSchoolMask;
    uint32 resisted = damageInfo->damages[slot].resist;
    // AzerothCore sends one blocked_amount for the packet, not one per sub-damage
    // entry, so emit it on slot 0 only to avoid double-counting on split-school hits.
    uint32 blocked  = slot == 0 ? damageInfo->blocked_amount : 0;
    uint32 absorbed = damageInfo->damages[slot].absorb;
    bool   crit     = damageInfo->hitOutCome == MELEE_HIT_CRIT;
    bool   glancing = damageInfo->hitOutCome == MELEE_HIT_GLANCING;
    bool   crushing = damageInfo->hitOutCome == MELEE_HIT_CRUSHING;

    std::ostringstream ss;
    ss << Now() << "  SWING_DAMAGE,"
       << BaseParams(attacker, target);
    AppendDamageSuffix(ss, amount, overkill, school, resisted, blocked,
                       absorbed, crit, glancing, crushing);
    return ss.str();
}

// ---------------------------------------------------------------------------
// SWING_MISSED — melee auto-attack miss/dodge/parry/etc.
// ---------------------------------------------------------------------------
std::string EventFormatter::SwingMissed(CalcDamageInfo const* damageInfo)
{
    std::ostringstream ss;
    ss << Now() << "  SWING_MISSED,"
       << BaseParams(damageInfo->attacker, damageInfo->target)
       << "," << MeleeHitOutcomeToMissType(damageInfo->hitOutCome)
       << "," << (damageInfo->attackType == OFF_ATTACK ? "1" : "nil")
       << ",0";
    return ss.str();
}

// ---------------------------------------------------------------------------
// SPELL_DAMAGE — spell direct damage with absorb/resist/block.
// ---------------------------------------------------------------------------
std::string EventFormatter::SpellDamage(SpellNonMeleeDamage const* log, int32 overkill)
{
    bool critical = (log->HitInfo & SPELL_HIT_TYPE_CRIT) != 0;

    std::ostringstream ss;
    ss << Now() << "  SPELL_DAMAGE,"
       << BaseParams(log->attacker, log->target);
    AppendSpellPrefix(ss, log->spellInfo->Id, log->spellInfo->SpellName[0],
                      log->schoolMask);
    // Glancing and crushing don't apply to spells — always false.
    AppendDamageSuffix(ss, log->damage, overkill,
                       log->schoolMask, log->resist, log->blocked, log->absorb,
                       critical, false, false);
    return ss.str();
}

// ---------------------------------------------------------------------------
// SPELL_MISSED — spell miss/dodge/parry/immune/resist/reflect etc.
// ---------------------------------------------------------------------------
std::string EventFormatter::SpellMissed(Unit* attacker, Unit* victim,
                                         uint32 spellId, SpellMissInfo missInfo)
{
    SpellInfo const* spell = sSpellMgr->GetSpellInfo(spellId);

    std::ostringstream ss;
    ss << Now() << "  SPELL_MISSED,"
       << BaseParams(attacker, victim);
    AppendSpellPrefix(ss, spellId,
                      spell ? spell->SpellName[0] : "",
                      spell ? spell->SchoolMask : 0);
    ss << "," << SpellMissInfoToString(missInfo)
       << ",nil"   // isOffHand
       << ",0";    // amountMissed
    return ss.str();
}

// ---------------------------------------------------------------------------
// SPELL_HEAL — healing with overheal and crit.
// ---------------------------------------------------------------------------
std::string EventFormatter::SpellHeal(HealInfo const& healInfo, bool critical)
{
    Unit* healer = healInfo.GetHealer();
    Unit* target = healInfo.GetTarget();
    SpellInfo const* spell = healInfo.GetSpellInfo();
    uint32 amount      = healInfo.GetHeal();
    uint32 overhealing = amount - healInfo.GetEffectiveHeal();
    uint32 absorbed    = healInfo.GetAbsorb();

    std::ostringstream ss;
    ss << Now() << "  SPELL_HEAL,"
       << BaseParams(healer, target);
    AppendSpellPrefix(ss, spell->Id, spell->SpellName[0], spell->SchoolMask);
    ss << "," << amount
       << "," << overhealing
       << "," << absorbed
       << "," << (critical ? "1" : "nil");
    return ss.str();
}

// ---------------------------------------------------------------------------
// SPELL_ENERGIZE — mana/rage/energy restore.
// ---------------------------------------------------------------------------
std::string EventFormatter::SpellEnergize(Unit* caster, Unit* target,
                                           uint32 spellId, uint32 amount,
                                           Powers powerType)
{
    SpellInfo const* spell = sSpellMgr->GetSpellInfo(spellId);

    std::ostringstream ss;
    ss << Now() << "  SPELL_ENERGIZE,"
       << BaseParams(caster, target);
    AppendSpellPrefix(ss, spellId,
                      spell ? spell->SpellName[0] : "",
                      spell ? spell->SchoolMask : 0);
    ss << "," << amount
       << "," << static_cast<int>(powerType);
    return ss.str();
}

// ---------------------------------------------------------------------------
// SPELL_PERIODIC_DAMAGE — periodic damage tick.
// ---------------------------------------------------------------------------
std::string EventFormatter::SpellPeriodicDamage(Unit* victim,
                                                 SpellPeriodicAuraLogInfo* pInfo)
{
    AuraEffect const* auraEff = pInfo->auraEff;
    Unit* caster = auraEff->GetCaster();
    SpellInfo const* spell = auraEff->GetSpellInfo();

    std::ostringstream ss;
    ss << Now() << "  SPELL_PERIODIC_DAMAGE,"
       << BaseParams(caster, victim);
    AppendSpellPrefix(ss, spell->Id, spell->SpellName[0], spell->SchoolMask);
    AppendDamageSuffix(ss, pInfo->damage, static_cast<int32>(pInfo->overDamage),
                       spell->SchoolMask, pInfo->resist, 0, pInfo->absorb,
                       pInfo->critical, false, false);
    return ss.str();
}

// ---------------------------------------------------------------------------
// SPELL_PERIODIC_HEAL — periodic heal tick.
// ---------------------------------------------------------------------------
std::string EventFormatter::SpellPeriodicHeal(Unit* victim,
                                               SpellPeriodicAuraLogInfo* pInfo)
{
    AuraEffect const* auraEff = pInfo->auraEff;
    Unit* caster = auraEff->GetCaster();
    SpellInfo const* spell = auraEff->GetSpellInfo();

    std::ostringstream ss;
    ss << Now() << "  SPELL_PERIODIC_HEAL,"
       << BaseParams(caster, victim);
    AppendSpellPrefix(ss, spell->Id, spell->SpellName[0], spell->SchoolMask);
    ss << "," << pInfo->damage
       << "," << pInfo->overDamage
       << "," << pInfo->absorb
       << "," << (pInfo->critical ? "1" : "nil");
    return ss.str();
}

// ---------------------------------------------------------------------------
// SPELL_PERIODIC_DRAIN — periodic mana/resource drain tick.
// WotLK format suffix: amount,powerType,extraAmount
// extraAmount is derived from the packet-visible gain multiplier.
// ---------------------------------------------------------------------------
std::string EventFormatter::SpellPeriodicDrain(Unit* victim,
                                               SpellPeriodicAuraLogInfo* pInfo)
{
    AuraEffect const* auraEff = pInfo->auraEff;
    Unit* caster = auraEff->GetCaster();
    SpellInfo const* spell = auraEff->GetSpellInfo();
    int32 extraAmount = static_cast<int32>(pInfo->damage * pInfo->multiplier);

    std::ostringstream ss;
    ss << Now() << "  SPELL_PERIODIC_DRAIN,"
       << BaseParams(caster, victim);
    AppendSpellPrefix(ss, spell->Id, spell->SpellName[0], spell->SchoolMask);
    ss << "," << pInfo->damage
       << "," << static_cast<int32>(auraEff->GetMiscValue())
       << "," << extraAmount;
    return ss.str();
}

// ---------------------------------------------------------------------------
// SPELL_PERIODIC_ENERGIZE — periodic mana/energy tick.
// ---------------------------------------------------------------------------
std::string EventFormatter::SpellPeriodicEnergize(Unit* victim,
                                                   SpellPeriodicAuraLogInfo* pInfo)
{
    AuraEffect const* auraEff = pInfo->auraEff;
    Unit* caster = auraEff->GetCaster();
    SpellInfo const* spell = auraEff->GetSpellInfo();

    std::ostringstream ss;
    ss << Now() << "  SPELL_PERIODIC_ENERGIZE,"
       << BaseParams(caster, victim);
    AppendSpellPrefix(ss, spell->Id, spell->SpellName[0], spell->SchoolMask);
    // pInfo->damage is the energy amount; overDamage is meaningless here
    ss << "," << pInfo->damage
       << "," << static_cast<int32>(auraEff->GetMiscValue());
    return ss.str();
}

// ---------------------------------------------------------------------------
// DAMAGE_SHIELD — thorns, retribution aura, etc.
// ---------------------------------------------------------------------------
std::string EventFormatter::DamageShield(Unit* shieldOwner, Unit* attacker,
    SpellInfo const* spell, uint32 damage, uint32 absorb, uint32 overkill)
{
    uint32 school = spell ? static_cast<uint32>(spell->SchoolMask) : 0;

    std::ostringstream ss;
    ss << Now() << "  DAMAGE_SHIELD,"
       << BaseParams(shieldOwner, attacker);
    if (spell)
        AppendSpellPrefix(ss, spell->Id, spell->SpellName[0], spell->SchoolMask);
    else
        AppendSpellPrefix(ss, 0, "", school);
    AppendDamageSuffix(ss, damage, static_cast<int32>(overkill), school,
                       0 /*resist*/, 0 /*block*/, absorb, false, false, false);
    return ss.str();
}

// ---------------------------------------------------------------------------
// SPELL_ABSORBED — per-aura absorb attribution (PW:S, Mana Shield, etc.).
//
// Two variants matching the WoW combat log convention:
//   Melee:  SPELL_ABSORBED,<src>,<dst>,<absorbCaster>,<absorbSpell>,<amount>
//   Spell:  SPELL_ABSORBED,<src>,<dst>,<dmgSpell>,<absorbCaster>,<absorbSpell>,<amount>
// ---------------------------------------------------------------------------
std::string EventFormatter::SpellAbsorbed(DamageInfo& dmgInfo,
    SpellInfo const* absorbSpell, Unit* absorbCaster, uint32 absorbAmount)
{
    Unit* victim = dmgInfo.GetVictim();

    std::ostringstream ss;
    ss << Now() << "  SPELL_ABSORBED,"
       << BaseParams(dmgInfo.GetAttacker(), victim);

    // If damage was from a spell, emit the damage spell prefix.
    SpellInfo const* dmgSpell = dmgInfo.GetSpellInfo();
    if (dmgSpell)
        AppendSpellPrefix(ss, dmgSpell->Id, dmgSpell->SpellName[0],
                          dmgSpell->SchoolMask);

    // Absorb caster (3 fields: GUID, name, flags).
    ss << ",";
    AppendObjectParams(ss, absorbCaster, victim);

    // Absorb spell (id, name, school).
    AppendSpellPrefix(ss, absorbSpell->Id, absorbSpell->SpellName[0],
                      absorbSpell->SchoolMask);

    // Amount absorbed by this aura.
    ss << "," << absorbAmount;

    return ss.str();
}

// ---------------------------------------------------------------------------
// SPELL_AURA_APPLIED — aura applied with caster info.
// ---------------------------------------------------------------------------
std::string EventFormatter::SpellAuraApplied(Unit* caster, Unit* target,
                                              SpellInfo const* spell)
{
    bool isBuff = spell->IsPositive();

    std::ostringstream ss;
    ss << Now() << "  SPELL_AURA_APPLIED,"
       << BaseParams(caster, target);
    AppendSpellPrefix(ss, spell->Id, spell->SpellName[0], spell->SchoolMask);
    ss << "," << (isBuff ? "BUFF" : "DEBUFF");
    return ss.str();
}

// ---------------------------------------------------------------------------
// SPELL_AURA_REMOVED — aura removed with caster info.
// ---------------------------------------------------------------------------
std::string EventFormatter::SpellAuraRemoved(Unit* caster, Unit* target,
                                              SpellInfo const* spell)
{
    bool isBuff = spell->IsPositive();

    std::ostringstream ss;
    ss << Now() << "  SPELL_AURA_REMOVED,"
       << BaseParams(caster, target);
    AppendSpellPrefix(ss, spell->Id, spell->SpellName[0], spell->SchoolMask);
    ss << "," << (isBuff ? "BUFF" : "DEBUFF");
    return ss.str();
}

// ---------------------------------------------------------------------------
// SPELL_CAST_SUCCESS — spell cast completed.
// ---------------------------------------------------------------------------
std::string EventFormatter::SpellCastSuccess(Unit* caster, WorldObject* explicitTarget,
                                                            SpellInfo const* spell)
{
    Unit* targetUnit = explicitTarget ? explicitTarget->ToUnit() : nullptr;

    std::ostringstream ss;
    ss << Now() << "  SPELL_CAST_SUCCESS,";
    AppendObjectParams(ss, caster, targetUnit);
    ss << ",";
    AppendObjectParams(ss, explicitTarget, caster);
    AppendSpellPrefix(ss, spell->Id, spell->SpellName[0], spell->SchoolMask);
    return ss.str();
}

// ---------------------------------------------------------------------------
// SPELL_SUMMON — a unit summoned a creature or game object.
// WotLK format: SPELL_SUMMON,srcGUID,"srcName",srcFlags,dstGUID,"dstName",dstFlags,spellId,"spellName",0xSchool
// ---------------------------------------------------------------------------
std::string EventFormatter::SpellSummon(Unit* caster, SpellInfo const* spell,
                                         WorldObject* summoned)
{
    Unit* summonedUnit = summoned ? summoned->ToUnit() : nullptr;

    std::ostringstream ss;
    ss << Now() << "  SPELL_SUMMON,";
    AppendObjectParams(ss, caster, summonedUnit);
    ss << ",";
    AppendObjectParams(ss, summoned, caster);
    AppendSpellPrefix(ss, spell->Id, spell->SpellName[0], spell->SchoolMask);
    return ss.str();
}

// ---------------------------------------------------------------------------
// UNIT_DIED — a unit has died.
// ---------------------------------------------------------------------------
std::string EventFormatter::UnitDied(Unit* killer, Unit* victim)
{
    std::ostringstream ss;
    ss << Now() << "  UNIT_DIED,"
       << BaseParams(killer, victim);
    return ss.str();
}

// ---------------------------------------------------------------------------
// ENVIRONMENTAL_DAMAGE — fall, lava, drowning, etc.
// ---------------------------------------------------------------------------
static const char* EnvTypeToString(uint8 type)
{
    switch (type)
    {
        case 0:  return "Fatigue";   // DAMAGE_EXHAUSTED
        case 1:  return "Drowning";  // DAMAGE_DROWNING
        case 2:  return "Falling";   // DAMAGE_FALL
        case 3:  return "Lava";      // DAMAGE_LAVA
        case 4:  return "Slime";     // DAMAGE_SLIME
        case 5:  return "Fire";      // DAMAGE_FIRE
        case 6:  return "Falling";   // DAMAGE_FALL_TO_VOID
        default: return "Unknown";
    }
}

static uint32 EnvSchool(uint8 type)
{
    switch (type)
    {
        case 3:  return 4;  // DAMAGE_LAVA  → Fire
        case 4:  return 8;  // DAMAGE_SLIME → Nature
        case 5:  return 4;  // DAMAGE_FIRE  → Fire
        default: return 1;  // Physical
    }
}

std::string EventFormatter::EnvironmentalDamage(Player* victim,
                                                EnviromentalDamage type,
                                                uint32 damage)
{
    uint8 envType = static_cast<uint8>(type);
    uint32 school = EnvSchool(envType);

    std::ostringstream ss;
    ss << Now() << "  ENVIRONMENTAL_DAMAGE,"
       << "0x0000000000000000,\"\",0x0"   // source (environment — no unit)
       << "," << Guid(victim->GetGUID())
       << ",\"" << victim->GetName() << "\""
         << ",0x" << std::hex << UnitFlags(victim) << std::dec
       << "," << EnvTypeToString(envType);
    AppendDamageSuffix(ss, damage, 0, school, 0, 0, 0, false, false, false);
    return ss.str();
}

std::string EventFormatter::SpellInterrupt(Unit* interrupter, Unit* interrupted,
                                           uint32 interruptSpellId, uint32 interruptedSpellId)
{
    SpellInfo const* intSpell = sSpellMgr->GetSpellInfo(interruptSpellId);
    SpellInfo const* victimSpell = sSpellMgr->GetSpellInfo(interruptedSpellId);

    std::ostringstream ss;
    ss << Now() << "  SPELL_INTERRUPT,"
         << BaseParams(interrupter, interrupted);
     AppendSpellPrefix(ss, interruptSpellId,
                             intSpell ? intSpell->SpellName[0] : "",
                             intSpell ? static_cast<uint32>(intSpell->SchoolMask) : 0u);
     AppendSpellPrefix(ss, interruptedSpellId,
                             victimSpell ? victimSpell->SpellName[0] : "",
                             victimSpell ? static_cast<uint32>(victimSpell->SchoolMask) : 0u);
    return ss.str();
}

std::string EventFormatter::SpellDispel(Unit* dispeller, Unit* victim,
                                        uint32 dispelSpellId, uint32 removedSpellId,
                                        bool isSteal)
{
    SpellInfo const* dispelSpell = sSpellMgr->GetSpellInfo(dispelSpellId);
    SpellInfo const* removedSpell = sSpellMgr->GetSpellInfo(removedSpellId);

    std::ostringstream ss;
    ss << Now() << "  " << (isSteal ? "SPELL_STOLEN," : "SPELL_DISPEL,")
       << BaseParams(dispeller, victim);
    AppendSpellPrefix(ss, dispelSpellId,
                      dispelSpell ? dispelSpell->SpellName[0] : "",
                      dispelSpell ? static_cast<uint32>(dispelSpell->SchoolMask) : 0u);
    AppendSpellPrefix(ss, removedSpellId,
                      removedSpell ? removedSpell->SpellName[0] : "",
                      removedSpell ? static_cast<uint32>(removedSpell->SchoolMask) : 0u);
    ss << "," << ((removedSpell && removedSpell->IsPositive()) ? "BUFF" : "DEBUFF");
    return ss.str();
}

std::string EventFormatter::SpellTargetResult(Unit* caster, Unit* target,
                                              SpellInfo const* spell,
                                              SpellMissInfo missInfo,
                                              SpellMissInfo reflectResult,
                                              uint8 effectMask)
{
    std::ostringstream ss;
    ss << Now() << "  CHRONICLE_SPELL_TARGET_RESULT,"
       << BaseParams(caster, target);
    AppendSpellPrefix(ss, spell ? spell->Id : 0,
                      spell ? spell->SpellName[0] : "",
                      spell ? static_cast<uint32>(spell->SchoolMask) : 0u);
    ss << ",\"" << SpellTargetResultToString(missInfo) << "\"";
    if (missInfo == SPELL_MISS_REFLECT)
        ss << ",\"" << SpellTargetResultToString(reflectResult) << "\"";
    else
        ss << ",nil";
    ss << ",0x" << std::hex << static_cast<uint32>(effectMask) << std::dec;
    return ss.str();
}

std::string EventFormatter::LootItem(Player* looter, ObjectGuid lootGuid,
                                     Item* item, uint32 count)
{
    std::ostringstream ss;
    WorldObject* source = ResolveLootSourceObject(looter, lootGuid);
    ss << Now() << "  CHRONICLE_LOOT_ITEM,";
    AppendLootSourceParams(ss, looter, lootGuid);
    ss << ",";
    AppendObjectParams(ss, looter, source ? source->ToUnit() : looter);
    ss << ",\"" << LootSourceTypeToString(lootGuid) << "\""
       << "," << (item ? item->GetEntry() : 0)
       << ",\"" << (item ? item->GetTemplate()->Name1 : "") << "\""
       << "," << count;
    return ss.str();
}

std::string EventFormatter::LootMoney(Player* looter, ObjectGuid lootGuid,
                                      uint32 amount)
{
    std::ostringstream ss;
    WorldObject* source = ResolveLootSourceObject(looter, lootGuid);
    ss << Now() << "  CHRONICLE_LOOT_MONEY,";
    AppendLootSourceParams(ss, looter, lootGuid);
    ss << ",";
    AppendObjectParams(ss, looter, source ? source->ToUnit() : looter);
    ss << ",\"" << LootSourceTypeToString(lootGuid) << "\""
       << "," << amount;
    return ss.str();
}

// --- Encounter events ---

std::string EventFormatter::EncounterStart(uint32 bossId, Map* instance)
{
    std::ostringstream ss;
    ss << Now() << "  CHRONICLE_ENCOUNTER_START,"
       << bossId << "," << instance->GetId() << "," << instance->GetInstanceId();
    return ss.str();
}

std::string EventFormatter::EncounterEnd(uint32 bossId, Map* instance, bool success)
{
    std::ostringstream ss;
    ss << Now() << "  CHRONICLE_ENCOUNTER_END,"
       << bossId << "," << instance->GetId() << "," << instance->GetInstanceId()
       << "," << (success ? 1 : 0);
    return ss.str();
}

std::string EventFormatter::EncounterCredit(
    Map* map, EncounterCreditType type, uint32 creditEntry,
    Unit* source, Difficulty difficulty,
    uint32 encounterDbcId, std::string const& encounterName,
    uint32 dungeonCompleted)
{
    std::ostringstream ss;
    ss << Now() << "  CHRONICLE_ENCOUNTER_CREDIT,"
       << map->GetId() << "," << map->GetInstanceId() << ","
       << static_cast<uint32>(type) << "," << creditEntry << ","
       << static_cast<uint32>(difficulty) << ","
       << encounterDbcId << ",\"" << encounterName << "\","
       << dungeonCompleted;

    if (source)
        ss << "," << Guid(source->GetGUID()) << ",\"" << source->GetName() << "\"";

    return ss.str();
}

// ===== CombatLogWriter =====

CombatLogWriter::CombatLogWriter(std::string const& dir, uint32 mapId, uint32 instanceId,
                                 std::string const& mapName, std::string const& realmName)
    : _dir(dir), _mapId(mapId), _instanceId(instanceId), _mapName(mapName), _realmName(realmName),
      _lastWriteTime(std::chrono::steady_clock::now())
{
    OpenNewFile();
}

void CombatLogWriter::OpenNewFile()
{
    std::filesystem::create_directories(_dir);

    auto now = std::chrono::system_clock::now();
    auto epoch = std::chrono::duration_cast<std::chrono::seconds>(
                     now.time_since_epoch())
                     .count();

    char filename[128];
    snprintf(filename, sizeof(filename), "instance_%u_%u_%lld.log",
             _mapId, _instanceId, static_cast<long long>(epoch));

    _path = _dir + "/" + filename;
    _file.open(_path, std::ios::out | std::ios::app);
    _lastWriteTime = std::chrono::steady_clock::now();

    if (_file.is_open())
    {
        LOG_INFO("module", "Chronicle: opened log file {}", _path);
    }
    else
    {
        LOG_ERROR("module", "Chronicle: FAILED to open log file {}", _path);
    }
}

CombatLogWriter::~CombatLogWriter()
{
    Close();
}

void CombatLogWriter::WriteLine(std::string const& line)
{
    if (_file.is_open())
    {
        _file << line << '\n';
        _lastWriteTime = std::chrono::steady_clock::now();
    }
}

void CombatLogWriter::Flush()
{
    if (_file.is_open())
        _file.flush();
}

void CombatLogWriter::Close()
{
    if (_file.is_open())
    {
        _file.flush();
        _file.close();
        LOG_INFO("module", "Chronicle: closed log file {}", _path);
    }
}

bool CombatLogWriter::Reopen()
{
    if (_file.is_open())
        return true;

    _file.open(_path, std::ios::out | std::ios::app);
    if (_file.is_open())
    {
        _lastWriteTime = std::chrono::steady_clock::now();
        LOG_INFO("module", "Chronicle: reopened log file {}", _path);
        return true;
    }

    LOG_ERROR("module", "Chronicle: FAILED to reopen log file {}", _path);
    return false;
}

bool CombatLogWriter::IsOpen() const
{
    return _file.is_open();
}

bool CombatLogWriter::IsIdleFor(std::chrono::seconds idleTimeout) const
{
    return idleTimeout.count() > 0 &&
           std::chrono::steady_clock::now() - _lastWriteTime >= idleTimeout;
}

// ===== InstanceTracker =====

namespace
{
constexpr auto ShutdownTaskWaitTimeout = std::chrono::seconds(45);
}

InstanceTracker& InstanceTracker::Instance()
{
    static InstanceTracker instance;
    return instance;
}

void InstanceTracker::LoadConfig()
{
    _enabled      = sConfigMgr->GetOption<bool>("Chronicle.Enable", true);
    _logDir       = sConfigMgr->GetOption<std::string>("Chronicle.LogDir", "chronicle_logs");
    _uploadURL    = sConfigMgr->GetOption<std::string>("Chronicle.UploadURL", "");
    _uploadSecret = sConfigMgr->GetOption<std::string>("Chronicle.UploadSecret", "");
    _requireTls = sConfigMgr->GetOption<bool>("Chronicle.RequireTLS", false);
    _verifyTls = sConfigMgr->GetOption<bool>("Chronicle.VerifyTLS", true);
    _trackDungeonRuns = sConfigMgr->GetOption<bool>("Chronicle.TrackDungeonRuns", true);
    _idleCloseSeconds = sConfigMgr->GetOption<uint32>("Chronicle.IdleCloseSeconds", 0);
    _rotateOnIdle = sConfigMgr->GetOption<bool>("Chronicle.RotateOnIdle", false);
    _uploadSnapshots = sConfigMgr->GetOption<bool>("Chronicle.UploadSnapshots", false);
    _snapshotOnEncounterCredit = sConfigMgr->GetOption<bool>("Chronicle.SnapshotOnEncounterCredit", false);

    if (!_uploadURL.empty())
    {
        if (_requireTls && !StartsWith(_uploadURL, "https://"))
            LOG_WARN("module", "Chronicle: Chronicle.RequireTLS is enabled, but UploadURL is not https:// ({})", _uploadURL);

        if (_verifyTls && !StartsWith(_uploadURL, "https://"))
            LOG_WARN("module", "Chronicle: Chronicle.VerifyTLS has no effect unless UploadURL uses https:// ({})", _uploadURL);
    }

    if (_rotateOnIdle && !_idleCloseSeconds)
        LOG_WARN("module", "Chronicle: Chronicle.RotateOnIdle is enabled, but Chronicle.IdleCloseSeconds is 0; idle rotation is disabled");

    if (_snapshotOnEncounterCredit && !_uploadSnapshots)
        LOG_WARN("module", "Chronicle: Chronicle.SnapshotOnEncounterCredit is enabled, but Chronicle.UploadSnapshots is disabled; encounter-credit snapshots will not run");

    if (_uploadSnapshots && !_uploadURL.empty() && !_uploadSecret.empty())
        LOG_WARN("module", "Chronicle: snapshot uploads contain overlapping copies of the active log and may duplicate later finalized uploads unless the ingestion endpoint handles snapshots separately");

    LOG_INFO("module", "Chronicle: enabled={}, logDir={}, upload={}, requireTls={}, verifyTls={}, trackDungeonRuns={}, idleCloseSeconds={}, rotateOnIdle={}, uploadSnapshots={}, snapshotOnEncounterCredit={}",
             _enabled, _logDir, _uploadURL.empty() ? "disabled" : _uploadURL,
             _requireTls, _verifyTls, _trackDungeonRuns, _idleCloseSeconds, _rotateOnIdle, _uploadSnapshots, _snapshotOnEncounterCredit);
}

bool InstanceTracker::ShouldTrackMap(Map const* map) const
{
    if (!map || !map->IsDungeon())
        return false;

    if (!_trackDungeonRuns && !map->IsRaid())
        return false;

    return true;
}

namespace
{
std::string DetermineChronicleRunType(Map* map)
{
    if (!map)
        return "NORMAL";

#if MOD_CHRONICLE_HAS_MYTHIC_PLUS
    if (sMythicPlus && sMythicPlus->IsEnabled() && sMythicPlus->IsMapInMythicPlus(map))
        return "MYTHIC_PLUS";
#endif

#if MOD_CHRONICLE_HAS_DUNGEON_MASTER
    if (sDungeonMasterMgr && sDungeonMasterMgr->HasActiveSessionForInstance(map->GetInstanceId()))
        return "DUNGEON_MASTER";
#endif

    return "NORMAL";
}
}

void InstanceTracker::Shutdown()
{
    std::vector<BackgroundTask> pendingUploads;

    bool uploadsConfigured = !_uploadURL.empty() && !_uploadSecret.empty();
    {
        std::lock_guard<std::mutex> lock(_mutex);
        for (auto& [instanceId, writer] : _writers)
        {
            if (writer)
            {
                writer->Flush();
                writer->Close();

                if (uploadsConfigured)
                {
                    BackgroundTask task;
                    task.type = BackgroundTaskType::Upload;
                    task.path = writer->GetPath();
                    task.url = _uploadURL;
                    task.secret = _uploadSecret;
                    task.instanceId = writer->GetInstanceId();
                    task.mapName = writer->GetMapName();
                    task.realmName = writer->GetRealmName();
                    auto tokenIt = _instanceTokens.find(instanceId);
                    if (tokenIt != _instanceTokens.end())
                        task.instanceToken = tokenIt->second;
                    task.requireTls = _requireTls;
                    task.verifyTls = _verifyTls;
                    pendingUploads.push_back(std::move(task));
                }
            }
        }

        _writers.clear();
        _seenUnits.clear();
        _seenCombatants.clear();
        _instanceTokens.clear();
    }

    {
        std::lock_guard<std::mutex> taskLock(_taskMutex);
        _shuttingDown = true;
        for (BackgroundTask& task : pendingUploads)
            _taskQueue.emplace_back(std::move(task));

        if (!_taskQueue.empty())
            EnsureTaskWorkerStartedLocked();
    }

    _taskCv.notify_all();

    std::unique_lock<std::mutex> lock(_taskMutex);
    if (!_taskCv.wait_for(lock, ShutdownTaskWaitTimeout, [this]() { return _taskQueue.empty() && _activeBackgroundTasks == 0; }))
        LOG_WARN("module", "Chronicle: shutdown timed out waiting for {} queued / active background task(s); remaining files will be retried on next startup", static_cast<uint32>(_taskQueue.size()) + _activeBackgroundTasks);

    lock.unlock();

    if (_taskWorker.joinable())
        _taskWorker.join();
}

void InstanceTracker::EnsureTaskWorkerStartedLocked()
{
    if (_taskWorker.joinable())
        return;

    _taskWorker = std::thread([this]() { RunTaskWorker(); });
}

void InstanceTracker::EnqueueUploadTaskLocked(std::string path, std::string url,
                                              std::string secret, uint32 instanceId,
                                              std::string mapName, std::string realmName,
                                              std::string instanceToken, bool requireTls,
                                              bool verifyTls)
{
    BackgroundTask task;
    task.type = BackgroundTaskType::Upload;
    task.path = std::move(path);
    task.url = std::move(url);
    task.secret = std::move(secret);
    task.instanceId = instanceId;
    task.mapName = std::move(mapName);
    task.realmName = std::move(realmName);
    task.instanceToken = std::move(instanceToken);
    task.requireTls = requireTls;
    task.verifyTls = verifyTls;
    _taskQueue.emplace_back(std::move(task));
    EnsureTaskWorkerStartedLocked();
}

void InstanceTracker::EnqueuePingTaskLocked(std::string url, std::string secret,
                                            bool requireTls, bool verifyTls)
{
    BackgroundTask task;
    task.type = BackgroundTaskType::Ping;
    task.url = std::move(url);
    task.secret = std::move(secret);
    task.requireTls = requireTls;
    task.verifyTls = verifyTls;
    _taskQueue.emplace_back(std::move(task));
    EnsureTaskWorkerStartedLocked();
}

void InstanceTracker::RunTaskWorker()
{
    for (;;)
    {
        BackgroundTask task;
        {
            std::unique_lock<std::mutex> lock(_taskMutex);
            _taskCv.wait(lock, [this]() { return _shuttingDown || !_taskQueue.empty(); });

            if (_taskQueue.empty())
            {
                if (_shuttingDown)
                    break;

                continue;
            }

            task = std::move(_taskQueue.front());
            _taskQueue.pop_front();
            ++_activeBackgroundTasks;
        }

        try
        {
            if (task.type == BackgroundTaskType::Upload)
            {
                UploadAndDelete(std::move(task.path), std::move(task.url), std::move(task.secret),
                                task.instanceId, std::move(task.mapName), std::move(task.realmName),
                                std::move(task.instanceToken), task.requireTls, task.verifyTls);
            }
            else
            {
                PingRemote(std::move(task.url), std::move(task.secret), task.requireTls, task.verifyTls);
            }
        }
        catch (std::exception const& e)
        {
            LOG_ERROR("module", "Chronicle: unexpected background task exception: {}", e.what());
        }

        {
            std::lock_guard<std::mutex> lock(_taskMutex);
            if (_activeBackgroundTasks > 0)
                --_activeBackgroundTasks;
        }

        _taskCv.notify_all();
    }
}

void InstanceTracker::QueueUploadTask(std::string path, uint32 instanceId,
                                      std::string mapName, std::string realmName,
                                      std::string instanceToken)
{
    std::lock_guard<std::mutex> lock(_taskMutex);
    if (_shuttingDown)
    {
        LOG_WARN("module", "Chronicle: skipping background upload for {} during shutdown; file remains on disk for orphan sweep retry", path);
        return;
    }

    EnqueueUploadTaskLocked(std::move(path), _uploadURL, _uploadSecret, instanceId,
                            std::move(mapName), std::move(realmName), std::move(instanceToken),
                            _requireTls, _verifyTls);
    _taskCv.notify_all();
}

void InstanceTracker::QueuePingTask(std::string url, std::string secret)
{
    bool requireTls = _requireTls;
    bool verifyTls = _verifyTls;

    {
        std::lock_guard<std::mutex> lock(_taskMutex);
        if (_shuttingDown)
            return;

        EnqueuePingTaskLocked(std::move(url), std::move(secret), requireTls, verifyTls);
    }

    _taskCv.notify_all();
}

void InstanceTracker::UploadOrphanedLogs()
{
    if (_uploadURL.empty() || _uploadSecret.empty())
    {
        LOG_INFO("module", "Chronicle: skipping orphaned-log sweep — upload not configured");
        return;
    }

    // Resolve the same log directory that GetOrCreateWriter uses.
    std::string logsDir = sConfigMgr->GetOption<std::string>("LogsDir", "");
    std::string logPath;
    if (logsDir.empty())
        logPath = _logDir;
    else
        logPath = logsDir + "/" + _logDir;

    std::error_code ec;
    if (!std::filesystem::exists(logPath, ec) || !std::filesystem::is_directory(logPath, ec))
    {
        LOG_INFO("module", "Chronicle: no log directory at {} — nothing to sweep", logPath);
        return;
    }

    uint32 count = 0;
    for (auto const& entry : std::filesystem::directory_iterator(logPath, ec))
    {
        if (ec)
            break;
        if (!entry.is_regular_file())
            continue;
        auto const& p = entry.path();
        if (p.extension() != ".log" && p.extension() != ".snap")
            continue;

        std::string filePath = p.string();
        LOG_INFO("module", "Chronicle: uploading orphaned log {}", filePath);

        // We don't have the original instanceId/mapName/realmName/token, so these
        // logs will not concatenate with any existing logs on the server.
        // But it's better than losing them entirely.
        QueueUploadTask(filePath,
                        /*instanceId=*/0, /*mapName=*/std::string(""),
                        /*realmName=*/std::string(""), /*instanceToken=*/std::string(""));
        ++count;
    }

    if (count > 0)
        LOG_INFO("module", "Chronicle: queued {} orphaned log file(s) for upload", count);
    else
        LOG_INFO("module", "Chronicle: no orphaned logs found in {}", logPath);
}

// static
std::string InstanceTracker::GenerateInstanceToken()
{
    std::array<uint8, 16> bytes{};
    for (auto& b : bytes)
        b = static_cast<uint8>(rand32() & 0xFF);
    return Acore::Impl::ByteArrayToHexStr(bytes.data(), bytes.size());
}

std::string InstanceTracker::GetInstanceToken(uint32 instanceId) const
{
    auto it = _instanceTokens.find(instanceId);
    return it != _instanceTokens.end() ? it->second : "";
}

void InstanceTracker::EmitCombatantInfoIfNeeded(CombatLogWriter& writer, Player* player, uint32 instanceId)
{
    if (!player)
        return;

    uint64 raw = player->GetGUID().GetRawValue();
    auto& seenCombatants = _seenCombatants[instanceId];
    if (!seenCombatants.insert(raw).second)
        return;

    writer.WriteLine(EventFormatter::CombatantInfo(player));
}

void InstanceTracker::EmitWriterPreamble(CombatLogWriter& writer, Map* map)
{
    uint32 instanceId = map->GetInstanceId();
    writer.WriteLine(EventFormatter::Header(writer.GetRealmName()));
    std::string instanceType = map->IsRaid() ? "raid" : "party";
    writer.WriteLine(EventFormatter::ZoneInfo(map->GetMapName(), map->GetId(), instanceId,
                                              instanceType, DetermineChronicleRunType(map)));

    for (MapReference const& ref : map->GetPlayers())
        EmitCombatantInfoIfNeeded(writer, ref.GetSource(), instanceId);

    writer.Flush();
}

void InstanceTracker::FinalizeWriterForIdleRotation(uint32 instanceId, std::unique_ptr<CombatLogWriter> writer,
                                                    std::string instanceToken)
{
    if (!writer)
        return;

    writer->Close();

    if (!_uploadURL.empty() && !_uploadSecret.empty())
        QueueUploadTask(writer->GetPath(), writer->GetInstanceId(), writer->GetMapName(), writer->GetRealmName(), std::move(instanceToken));

    _seenUnits.erase(instanceId);
    _seenCombatants.erase(instanceId);
}

CombatLogWriter* InstanceTracker::GetOrCreateWriter(Map* map)
{
    if (!ShouldTrackMap(map))
        return nullptr;

    uint32 instanceId = map->GetInstanceId();
    auto it = _writers.find(instanceId);
    if (it != _writers.end())
    {
        if (_idleCloseSeconds > 0 && it->second->IsIdleFor(std::chrono::seconds(_idleCloseSeconds)))
        {
            if (_rotateOnIdle)
            {
                LOG_INFO("module", "Chronicle: rotating idle log for instance {} after {} second(s) of inactivity", instanceId, _idleCloseSeconds);
                std::string token = GetInstanceToken(instanceId);
                auto idleWriter = std::move(it->second);
                _writers.erase(it);
                FinalizeWriterForIdleRotation(instanceId, std::move(idleWriter), std::move(token));
            }
            else
            {
                it->second->Close();
                if (!it->second->Reopen())
                    return nullptr;

                return it->second.get();
            }
        }
        else
        {
            return it->second.get();
        }
    }

    std::string logsDir = sConfigMgr->GetOption<std::string>("LogsDir", "");
    std::string logPath;
    if (logsDir.empty())
        logPath = _logDir;
    else
        logPath = logsDir + "/" + _logDir;

    std::string realmName = sWorld->GetRealmName();
    auto writer = std::make_unique<CombatLogWriter>(logPath, map->GetId(), instanceId,
                                                     map->GetMapName(), realmName);
    if (!writer->IsOpen())
        return nullptr;

    if (!_instanceTokens.count(instanceId))
        _instanceTokens[instanceId] = GenerateInstanceToken();

    _seenUnits.erase(instanceId);
    _seenCombatants.erase(instanceId);
    EmitWriterPreamble(*writer, map);

    CombatLogWriter* ptr = writer.get();
    _writers[instanceId] = std::move(writer);
    return ptr;
}

void InstanceTracker::OnPlayerEnterInstance(Map* map, Player* player)
{
    if (!_enabled || !ShouldTrackMap(map))
        return;

    std::lock_guard<std::mutex> lock(_mutex);

    CombatLogWriter* writer = GetOrCreateWriter(map);
    if (!writer)
        return;

    EmitCombatantInfoIfNeeded(*writer, player, map->GetInstanceId());
    writer->Flush();
}

void InstanceTracker::OnPlayerLeaveInstance(Map* map, Player* /*player*/)
{
    if (!_enabled || !ShouldTrackMap(map))
        return;
}

void InstanceTracker::OnInstanceIdRemoved(uint32 instanceId)
{
    if (!_enabled || !instanceId)
        return;

    RemoveInstance(instanceId);
}

void InstanceTracker::RemoveInstance(uint32 instanceId)
{
    std::string filePath;
    uint32 instId = 0;
    std::string mapName;
    std::string realmName;
    std::string token;
    {
        std::lock_guard<std::mutex> lock(_mutex);

        auto it = _writers.find(instanceId);
        if (it != _writers.end())
        {
            it->second->Close();
            filePath  = it->second->GetPath();
            instId    = it->second->GetInstanceId();
            mapName   = it->second->GetMapName();
            realmName = it->second->GetRealmName();
            _writers.erase(it);
        }
        auto tokenIt = _instanceTokens.find(instanceId);
        if (tokenIt != _instanceTokens.end())
        {
            token = tokenIt->second;
            _instanceTokens.erase(tokenIt);
        }
        _seenUnits.erase(instanceId);
        _seenCombatants.erase(instanceId);
    }

    // Upload in background without blocking the game thread.
    if (!filePath.empty() && !_uploadURL.empty() && !_uploadSecret.empty())
        QueueUploadTask(filePath, instId, std::move(mapName), std::move(realmName), std::move(token));
}

// ---------------------------------------------------------------------------
// Upload helpers — zlib gzip + Boost.Beast HTTP/HTTPS, no shell-out.
// ---------------------------------------------------------------------------
namespace {

struct ParsedUrl
{
    std::string scheme;  // "http" or "https"
    std::string host;
    std::string port;
    std::string target;  // path: configured root path + /azerothcore/upload
};

// Parses the configured root URL and appends the given suffix to its path.
// Trailing slashes on the configured value are stripped before appending, so
// "https://host", "https://host/", "https://host/api", and "https://host/api/"
// all produce well-formed targets.
static bool ParseUrl(std::string const& rootUrl, std::string const& pathSuffix, ParsedUrl& out, bool requireTls)
{
    std::string rest;
    if (rootUrl.substr(0, 8) == "https://") { out.scheme = "https"; rest = rootUrl.substr(8); }
    else if (rootUrl.substr(0, 7) == "http://") { out.scheme = "http";  rest = rootUrl.substr(7); }
    else return false;

    if (requireTls && out.scheme != "https")
        return false;

    // Split host[:port] from the path component.
    auto slash = rest.find('/');
    std::string hostport = (slash == std::string::npos) ? rest : rest.substr(0, slash);
    std::string path     = (slash == std::string::npos) ? "" : rest.substr(slash);

    auto colon = hostport.find(':');
    if (colon != std::string::npos)
    {
        out.host = hostport.substr(0, colon);
        out.port = hostport.substr(colon + 1);
    }
    else
    {
        out.host = hostport;
        out.port = (out.scheme == "https") ? "443" : "80";
    }

    // Strip trailing slash(es) from the configured path, then append the suffix.
    while (!path.empty() && path.back() == '/')
        path.pop_back();
    out.target = path + pathSuffix;
    return !out.host.empty();
}

// Gzip-compress bytes from a file. Returns empty vector on error.
static std::vector<Bytef> GzipFile(std::string const& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return {};
    std::vector<Bytef> input(std::istreambuf_iterator<char>(file), {});

    z_stream zs{};
    // windowBits=31 → gzip wrapper (15 + 16)
    if (deflateInit2(&zs, Z_BEST_COMPRESSION, Z_DEFLATED,
                     31, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        return {};

    std::vector<Bytef> output(deflateBound(&zs, static_cast<uLong>(input.size())) + 64);
    zs.next_in   = input.data();
    zs.avail_in  = static_cast<uInt>(input.size());
    zs.next_out  = output.data();
    zs.avail_out = static_cast<uInt>(output.size());

    int ret = deflate(&zs, Z_FINISH);
    deflateEnd(&zs);
    if (ret != Z_STREAM_END)
        return {};

    output.resize(zs.total_out);
    return output;
}

// Build a multipart/form-data body with the compressed log as the sole part.
static std::string BuildMultipart(std::vector<Bytef> const& compressed,
                                   std::string& contentTypeOut)
{
    static constexpr char kBoundary[] = "----ChronicleUploadBoundary7x83kq";
    contentTypeOut = "multipart/form-data; boundary=";
    contentTypeOut += kBoundary;

    std::string body;
    body += "--";
    body += kBoundary;
    body += "\r\nContent-Disposition: form-data; name=\"combat_log\"; filename=\"combat_log.gz\"\r\n";
    body += "Content-Type: application/gzip\r\n\r\n";
    body.append(reinterpret_cast<char const*>(compressed.data()), compressed.size());
    body += "\r\n--";
    body += kBoundary;
    body += "--\r\n";
    return body;
}

// Send an HTTP request and return the response status code.
// Throws boost::system::system_error or std::exception on network error.
static int DoSend(ParsedUrl const& parsed,
                  boost::beast::http::request<boost::beast::http::string_body>& req,
                  bool verifyTls)
{
    namespace beast = boost::beast;
    namespace http  = beast::http;
    namespace net   = boost::asio;
    namespace ssl   = net::ssl;
    using tcp       = net::ip::tcp;

    static constexpr auto ConnectTimeout = std::chrono::seconds(10);
    static constexpr auto RequestTimeout = std::chrono::seconds(30);

    net::io_context ioc;
    tcp::resolver   resolver(ioc);
    auto const endpoints = resolver.resolve(parsed.host, parsed.port);

    if (parsed.scheme == "https")
    {
        ssl::context ctx(ssl::context::tlsv12_client);
        if (verifyTls)
            ctx.set_default_verify_paths();
        beast::ssl_stream<beast::tcp_stream> stream(ioc, ctx);
        stream.set_verify_mode(verifyTls ? ssl::verify_peer : ssl::verify_none);
        if (verifyTls)
            stream.set_verify_callback(ssl::host_name_verification(parsed.host));
        if (!SSL_set_tlsext_host_name(stream.native_handle(), parsed.host.c_str()))
        {
            unsigned long sslError = ::ERR_get_error();
            char errorBuffer[256] = {};
            if (sslError)
                ::ERR_error_string_n(sslError, errorBuffer, sizeof(errorBuffer));

            throw std::runtime_error(sslError ? std::string("failed to set TLS SNI: ") + errorBuffer
                                              : std::string("failed to set TLS SNI"));
        }

        beast::get_lowest_layer(stream).expires_after(ConnectTimeout);
        beast::get_lowest_layer(stream).connect(endpoints);
        beast::get_lowest_layer(stream).expires_after(RequestTimeout);
        stream.handshake(ssl::stream_base::client);
        beast::get_lowest_layer(stream).expires_after(RequestTimeout);
        http::write(stream, req);
        beast::flat_buffer buf;
        http::response<http::string_body> res;
        beast::get_lowest_layer(stream).expires_after(RequestTimeout);
        http::read(stream, buf, res);
        beast::error_code ec;
        beast::get_lowest_layer(stream).expires_never();
        stream.shutdown(ec);  // ignore graceful-shutdown errors
        return static_cast<int>(res.result_int());
    }
    else
    {
        beast::tcp_stream stream(ioc);
        stream.expires_after(ConnectTimeout);
        stream.connect(endpoints);
        stream.expires_after(RequestTimeout);
        http::write(stream, req);
        beast::flat_buffer buf;
        http::response<http::string_body> res;
        stream.expires_after(RequestTimeout);
        http::read(stream, buf, res);
        beast::error_code ec;
        stream.socket().shutdown(tcp::socket::shutdown_both, ec);
        return static_cast<int>(res.result_int());
    }
}

} // anonymous namespace

// static
void InstanceTracker::UploadAndDelete(std::string path,
                                       std::string url,
                                       std::string secret,
                                       uint32 instanceId,
                                       std::string mapName,
                                       std::string realmName,
                                       std::string instanceToken,
                                       bool requireTls,
                                       bool verifyTls)
{
    // 1. Gzip-compress the log file in memory (zlib, windowBits=31 → gzip format).
    auto compressed = GzipFile(path);
    if (compressed.empty())
    {
        LOG_ERROR("module", "Chronicle: failed to gzip {}", path);
        return;
    }

    // 2. Parse the configured root URL and append the fixed upload path.
    //    Trailing slash on the configured value is handled: both
    //    "https://host" and "https://host/" resolve to /azerothcore/upload.
    ParsedUrl parsed;
    if (!ParseUrl(url, "/api/v1/azerothcore/upload", parsed, requireTls))
    {
        LOG_ERROR("module", "Chronicle: invalid upload URL or TLS policy mismatch: {}", url);
        return;
    }

    // 3. Build multipart/form-data body.
    std::string contentType;
    std::string body = BuildMultipart(compressed, contentType);

    // 4. Build the HTTP request.
    namespace http = boost::beast::http;
    http::request<http::string_body> req{http::verb::post, parsed.target, 11};
    req.set(http::field::host,          parsed.host);
    req.set(http::field::content_type,  contentType);
    req.set(http::field::authorization, "Bearer " + secret);
    req.set("X-Chronicle-Instance-Id",    std::to_string(instanceId));
    req.set("X-Chronicle-Instance-Name", mapName);
    req.set("X-Chronicle-Instance-Token", instanceToken);
    req.set("X-Chronicle-Realm-Name",    realmName);
    req.body() = std::move(body);
    req.prepare_payload();

    // 5. Send and handle the response.
    try
    {
        int status = DoSend(parsed, req, verifyTls);
        if (status == 201)
        {
            std::error_code ec;
            bool removed = std::filesystem::remove(path, ec);
            if (ec)
            {
                LOG_ERROR("module", "Chronicle: uploaded {} but failed to delete it: {}", path, ec.message());
            }
            else if (!removed)
            {
                LOG_WARN("module", "Chronicle: uploaded {} but it was already missing during delete", path);
            }
            else
            {
                LOG_INFO("module", "Chronicle: uploaded and deleted {}", path);
            }
        }
        else
        {
            LOG_ERROR("module", "Chronicle: upload failed for {} (HTTP {}) url={}", path, status, url);
        }
    }
    catch (std::exception const& e)
    {
        LOG_ERROR("module", "Chronicle: upload exception for {}: {}", path, e.what());
    }
}

// static
void InstanceTracker::PingRemote(std::string url, std::string secret, bool requireTls, bool verifyTls)
{
    ParsedUrl parsed;
    if (!ParseUrl(url, "/api/v1/azerothcore/ping", parsed, requireTls))
    {
        LOG_ERROR("module", "Chronicle: ping failed — invalid URL or TLS policy mismatch: {}", url);
        return;
    }

    namespace http = boost::beast::http;
    http::request<http::string_body> req{http::verb::get, parsed.target, 11};
    req.set(http::field::host,          parsed.host);
    req.set(http::field::authorization, "Bearer " + secret);
    req.prepare_payload();

    try
    {
        int status = DoSend(parsed, req, verifyTls);
        if (status == 200)
        {
            LOG_INFO("module", "Chronicle: ping OK (HTTP 200) — connected to {}", url);
        }
        else
        {
            LOG_ERROR("module", "Chronicle: ping failed (HTTP {}) — check UploadURL/UploadSecret", status);
        }
    }
    catch (std::exception const& e)
    {
        LOG_ERROR("module", "Chronicle: ping exception — {}", e.what());
    }
}

void InstanceTracker::EnsureUnitInfo(Unit* unit)
{
    if (!_enabled || !unit)
        return;

    Map* map = unit->FindMap();
    if (!ShouldTrackMap(map))
        return;

    std::lock_guard<std::mutex> lock(_mutex);

    CombatLogWriter* writer = GetOrCreateWriter(map);
    if (!writer)
        return;

    uint32 instId = map->GetInstanceId();
    uint64 raw = unit->GetGUID().GetRawValue();

    auto& seen = _seenUnits[instId];
    if (seen.count(raw))
        return;
    seen.insert(raw);

    writer->WriteLine(EventFormatter::UnitInfo(unit));
}

void InstanceTracker::WriteForUnit(Unit* unit, std::string const& line)
{
    if (!_enabled || !unit)
        return;

    Map* map = unit->FindMap();
    if (!ShouldTrackMap(map))
        return;

    std::lock_guard<std::mutex> lock(_mutex);

    CombatLogWriter* writer = GetOrCreateWriter(map);
    if (!writer)
        return;

    writer->WriteLine(line);
}

void InstanceTracker::WriteForMap(Map* map, std::string const& line)
{
    if (!_enabled || !ShouldTrackMap(map))
        return;

    std::lock_guard<std::mutex> lock(_mutex);
    CombatLogWriter* writer = GetOrCreateWriter(map);
    if (!writer)
        return;

    writer->WriteLine(line);
}

void InstanceTracker::WriteForInstance(uint32 instanceId, std::string const& line)
{
    if (!_enabled)
        return;

    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _writers.find(instanceId);
    if (it != _writers.end())
        it->second->WriteLine(line);
}

void InstanceTracker::FlushInstance(uint32 instanceId)
{
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _writers.find(instanceId);
    if (it != _writers.end())
        it->second->Flush();
}

void InstanceTracker::FlushMap(Map* map)
{
    if (!_enabled || !ShouldTrackMap(map))
        return;

    std::lock_guard<std::mutex> lock(_mutex);
    CombatLogWriter* writer = GetOrCreateWriter(map);
    if (writer)
        writer->Flush();
}

void InstanceTracker::UploadInstanceSnapshot(uint32 instanceId)
{
    if (!_uploadSnapshots || _uploadURL.empty() || _uploadSecret.empty())
        return;

    std::string srcPath;
    uint64 snapshotId = 0;
    uint32 instId = 0;
    std::string mapName;
    std::string realmName;
    std::string token;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _writers.find(instanceId);
        if (it == _writers.end())
            return;

        it->second->Flush();
        srcPath   = it->second->GetPath();
        instId    = it->second->GetInstanceId();
        mapName   = it->second->GetMapName();
        realmName = it->second->GetRealmName();
        token     = GetInstanceToken(instanceId);
        snapshotId = ++_nextSnapshotId;
    }

    // Copy the log to a unique temporary file so each queued upload references
    // immutable bytes, even if multiple snapshots are triggered quickly.
    std::string snapPath = srcPath + "." + std::to_string(snapshotId) + ".snap";
    std::error_code ec;
    std::filesystem::copy_file(srcPath, snapPath, std::filesystem::copy_options::none, ec);
    if (ec)
    {
        LOG_ERROR("module", "Chronicle: snapshot copy failed for {}: {}", srcPath, ec.message());
        return;
    }

    // Upload (and delete the snapshot copy on success) in the background.
    QueueUploadTask(snapPath, instId, std::move(mapName), std::move(realmName), std::move(token));
}

void InstanceTracker::UploadInstanceSnapshot(Map* map)
{
    if (!ShouldTrackMap(map))
        return;

    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (!GetOrCreateWriter(map))
            return;
    }

    UploadInstanceSnapshot(map->GetInstanceId());
}
