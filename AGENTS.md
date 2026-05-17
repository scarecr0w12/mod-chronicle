# mod-chronicle — Contributor Guide

## What this module is

`mod-chronicle` is a server-side AzerothCore module that writes per-instance
combat logs for Chronicle in **comma-separated WotLK-style log lines** with
Chronicle-specific extension events.

Each line currently follows this shape:

```text
<unix_millis>  EVENT_NAME,field,field,...
```

This is **not** the older pipe-delimited V2 addon format. Keep docs and test
fixtures aligned with the formatter in `src/Chronicle.cpp`.

## Main runtime pieces

- `EventFormatter`
  - Pure formatting helpers for standard combat log events plus `CHRONICLE_*`
    extension events.
- `CombatLogWriter`
  - Owns one output stream per active dungeon/raid instance.
- `InstanceTracker`
  - Singleton that maps `instanceId -> writer`, deduplicates unit/combatant
    preamble lines, coordinates idle rotation, snapshots, orphan retries, and
    upload/shutdown behavior.

## Hook entry points

The module is wired through `src/ChronicleLogs_SC.cpp` and currently uses:

- `ChronicleUnitScript`
  - melee, spell damage, heals, misses, periodic effects, absorbs, death,
    evade, and combat-entry events
- `ChronicleGlobalScript`
  - cast success, per-target cast result logging, aura updates, summons,
    encounter state, interrupts, dispels, and instance-save cleanup
- `ChronicleAllCreatureScript`
  - alive creature despawns that would otherwise disappear without `UNIT_DIED`
- `ChroniclePlayerScript`
  - environmental damage and item loot
- `ChronicleLootScript`
  - money loot
- `ChronicleAllMapScript`
  - instance enter/leave/destroy lifecycle
- `ChronicleWorldScript`
  - config load, startup orphan sweep/ping, and shutdown

## Important logging behavior

### Unit preamble

Before most event writes, hooks call `InstanceTracker::EnsureUnitInfo(unit)`.
That emits one `CHRONICLE_UNIT_INFO` line per GUID per instance before the
first meaningful event using that unit.

Current `CHRONICLE_UNIT_INFO` payload:

```text
guid,"name",level,0xflags,ownerGuid,maxHealth,"affiliation",isBoss
```

Notes:

- `flags` are relation-aware WotLK combat-log style flags.
- `affiliation` is emitted as `FRIENDLY`, `HOSTILE`, or `NEUTRAL` from a
  representative player perspective in the instance.
- `isBoss` is `true` when AzerothCore reports `isWorldBoss()` or
  `IsDungeonBoss()`.

### File lifecycle

- A writer is created lazily when Chronicle first needs to log for a dungeon
  or raid instance.
- `CHRONICLE_HEADER`, `CHRONICLE_ZONE_INFO`, and current
  `CHRONICLE_COMBATANT_INFO` lines are emitted when a writer is created.
- Idle handling is configurable with `Chronicle.IdleCloseSeconds` and
  `Chronicle.RotateOnIdle`.
- Encounter-credit snapshot uploads are configurable with
  `Chronicle.UploadSnapshots` and `Chronicle.SnapshotOnEncounterCredit`.
- Startup sweeps leftover `.log` and `.snap` files for retry when upload is
  configured.

### Upload path

Uploads are implemented with:

- zlib for in-memory gzip
- Boost.Beast / Boost.Asio for HTTP and HTTPS transport
- optional TLS enforcement and verification controls

Do **not** document this as a shell-out to `curl`; that is stale.

## Current feature coverage

Implemented Chronicle extensions include:

- `CHRONICLE_HEADER`
- `CHRONICLE_ZONE_INFO`
- `CHRONICLE_COMBATANT_INFO`
- `CHRONICLE_UNIT_INFO`
- `CHRONICLE_UNIT_EVADE`
- `CHRONICLE_UNIT_COMBAT`
- `CHRONICLE_UNIT_DESPAWN`
- `CHRONICLE_SPELL_TARGET_RESULT`
- `CHRONICLE_LOOT_ITEM`
- `CHRONICLE_LOOT_MONEY`
- `CHRONICLE_ENCOUNTER_START`
- `CHRONICLE_ENCOUNTER_END`
- `CHRONICLE_ENCOUNTER_CREDIT`

Implemented standard event families include, among others:

- `SWING_DAMAGE`, `SWING_MISSED`
- `SPELL_DAMAGE`, `SPELL_MISSED`, `SPELL_HEAL`, `SPELL_ENERGIZE`
- `SPELL_PERIODIC_DAMAGE`, `SPELL_PERIODIC_HEAL`, `SPELL_PERIODIC_DRAIN`,
  `SPELL_PERIODIC_ENERGIZE`
- `DAMAGE_SHIELD`, `SPELL_ABSORBED`
- `SPELL_AURA_APPLIED`, `SPELL_AURA_REMOVED`
- `SPELL_CAST_SUCCESS`, `SPELL_SUMMON`
- `SPELL_DISPEL`, `SPELL_STOLEN`, `SPELL_INTERRUPT`
- `UNIT_DIED`, `ENVIRONMENTAL_DAMAGE`

`SPELL_INTERRUPT` can legitimately log an empty source GUID/name/flags when the
interrupt came from a silence aura whose original caster is no longer in world.

## Remaining engineering gaps

The major repo-local gaps still open are:

- formatter regression tests do not exist yet
- owner-chain attribution is still shallow
- creature spawn identity is still GUID-based rather than stable spawn based
- production secret rotation/externalization is an operational task, not a
  repo-only code change

## Testing and validation

- Use `README.md` for module behavior and configuration.
- Use `TESTING.md` for the manual smoke-test checklist.
- Keep `azerothcore-module-ingestion.md` aligned with field order expectations.

## When editing this module

- Prefer targeted formatter changes over broad refactors.
- Preserve existing field ordering unless you are intentionally versioning or
  coordinating parser changes.
- If you append new fields, keep existing parser-aligned prefixes stable when
  possible.
- Update docs in the same change when formatter output changes.
