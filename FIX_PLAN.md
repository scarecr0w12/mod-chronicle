# mod-chronicle Fix Plan

## Goal

Harden `mod-chronicle` from a strong proof-of-concept into a production-ready AzerothCore module that is:

- correct in combat log output
- safe in upload and shutdown behavior
- efficient under raid-scale event volume
- maintainable operationally
- documented consistently

## Progress update

### Completed on 2026-05-17

- AzerothCore PR `#25641` alignment refresh completed:
  - [x] Update Chronicle to accept nullable `OnSpellInterrupt` sources from silence-aura paths
  - [x] Add `OnBeforeCreatureDespawn` coverage for alive despawns via `CHRONICLE_UNIT_DESPAWN`
  - [x] Refresh hook documentation to match final upstream hook names and signatures

### Completed on 2026-04-27

- Sprint 1 code work completed:
  - [x] Add TLS verification and hostname verification
  - [x] Add connect/read/write timeouts
  - [x] Preserve retryable orphaned `.log` and `.snap` uploads on startup
  - [x] Replace detached uploads and ping work with managed async lifecycle
  - [x] Replace per-line `std::endl` flushing
- Sprint 2 follow-up completed:
  - [x] Normalize repeated GUID/name/flag formatting across `BaseParams`, `SPELL_CAST_SUCCESS`, `SPELL_SUMMON`, and `SPELL_ABSORBED`
  - [x] Normalize `SPELL_INTERRUPT` spell prefix formatting
  - [x] Validate melee split overkill, reflect overkill, and blocked-slot semantics against AzerothCore packet builders
  - [x] Document validated packet-aligned damage semantics inline in code
- Sprint 3 initial work completed:
  - [x] Add configurable idle close / rotate behavior for per-instance logs
  - [x] Add configurable snapshot upload controls
  - [x] Add `OnInstanceIdRemoved` cleanup safety net
- Sprint 4 initial work completed:
  - [x] Implement `SPELL_PERIODIC_DRAIN` for `SPELL_AURA_PERIODIC_MANA_LEECH`
  - [x] Add silence-based interrupt handling through the existing interrupt hook
  - [x] Add `SPELL_DISPEL` / `SPELL_STOLEN` coverage through a successful-dispel observer hook
  - [x] Add packet-aligned per-target cast result logging from `Spell::m_UniqueTargetInfo`
  - [x] Make Chronicle TLS enforcement configurable for HTTP/self-signed dev environments

### Completed on 2026-04-29

- Documentation and operational artifacts refreshed:
  - [x] Replace stale `AGENTS.md` architecture/format notes with current module behavior
  - [x] Add manual smoke-test checklist in `TESTING.md`
  - [x] Add upstream hook contribution package in `UPSTREAM_HOOK_PLAN.md`
  - [x] Refresh `FUTURE.md` so it only tracks genuinely open work
- Audit result captured:
  - [ ] Formatter regression tests still not implemented
  - [ ] Owner-chain and stable spawn identity attribution still open
  - [ ] Production secret rotation/externalization remains an operational follow-up

### Deferred by request

- [ ] Rotate or externalize the upload secret

### Sprint 2 in progress

- [x] Use real unit flags in `BaseParams`
- [x] Expand `UnitFlags()` coverage with practical type/control/reaction/affiliation data
- [x] Fix `SPELL_CAST_SUCCESS` destination semantics for explicit unit/object targets
- [x] Audit format consistency
- [x] Validate overkill and mitigation semantics

### Sprint 3 in progress

- [x] Add idle log close or rotation
- [x] Review snapshot upload frequency/configuration
- [ ] Refine lock granularity only if profiling justifies it
- [x] Add `OnInstanceIdRemoved` safety net

### Sprint 4 in progress

- [x] Implement `SPELL_PERIODIC_DRAIN`
- [x] Add silence-based interrupt handling
- [x] Add `SPELL_DISPEL` / `SPELL_STOLEN`
- [x] Add richer cast target result logging
- [x] Add `LOOT`
- [ ] Improve owner-chain and spawn identity attribution

## Recommended phase order

1. Security and operational safety
2. Combat log fidelity and correctness
3. Performance and lifecycle robustness
4. Feature completeness
5. Documentation, tests, and upstreaming

---

## Phase 1 — Security and operational safety

### 1.1 Rotate and externalize the upload secret

**Priority:** Critical

#### Work
- Rotate the currently configured `Chronicle.UploadSecret`
- Ensure active deployed config is not shared or committed
- Move production secret sourcing to a safer operational path where possible

#### Areas affected
- `env/dist/etc/modules/mod_chronicle.conf`
- deployment and runbook documentation

#### Validation
- Old secret no longer authenticates
- New secret authenticates successfully
- Secret does not appear in logs or tracked files

### 1.2 Harden HTTPS and request behavior

**Priority:** Critical

#### Work
Update upload and ping behavior in `src/Chronicle.cpp` to include:
- TLS peer certificate verification
- hostname verification
- configurable HTTPS enforcement / verification for dev vs production
- connect timeout
- read timeout
- write timeout
- clearer failure logging for DNS, connect, TLS, timeout, and HTTP failures

#### Areas affected
- `modules/mod-chronicle/src/Chronicle.cpp`

#### Validation
- Valid HTTPS endpoint succeeds
- Invalid or self-signed cert is rejected
- Wrong-hostname cert is rejected
- Unreachable host fails fast
- Slow endpoint times out cleanly

### 1.3 Preserve failed uploads instead of risking data loss

**Priority:** High

#### Work
- Keep original `.log` on upload failure, or move failed snapshots to a retry/dead-letter directory
- Add startup retry sweep for pending failed uploads
- Optionally add retry configuration

#### Suggested config
- `Chronicle.RetryFailedUploads = 1`
- `Chronicle.FailedUploadDir = "chronicle_failed"`
- `Chronicle.MaxUploadRetries = 3`

#### Areas affected
- `modules/mod-chronicle/src/Chronicle.cpp`
- `modules/mod-chronicle/conf/mod_chronicle.conf.dist`
- `modules/mod-chronicle/README.md`

#### Validation
- HTTP 500 preserves file
- Network timeout preserves file
- Startup sweep retries preserved file

### 1.4 Add upload thread lifecycle control

**Priority:** High

#### Work
Replace detached background upload threads with one of:
- a small uploader queue/service with graceful shutdown
- tracked worker threads or futures joined during shutdown

Also define shutdown behavior:
- complete in-flight work up to timeout, or
- preserve pending files for retry on startup

#### Areas affected
- `modules/mod-chronicle/src/Chronicle.h`
- `modules/mod-chronicle/src/Chronicle.cpp`
- `ChronicleWorldScript` lifecycle hooks if needed

#### Validation
- Upload in progress during shutdown does not crash
- Pending file is preserved and retried later

---

## Phase 2 — Combat log fidelity and correctness

### 2.1 Use real unit flags in base event params

**Priority:** High

#### Work
- Update `EventFormatter::BaseParams()` to emit `UnitFlags(source)` and `UnitFlags(dest)`
- Expand `UnitFlags()` to cover additional WoW semantics where practical:
  - type
  - control
  - reaction
  - affiliation

#### Areas affected
- `modules/mod-chronicle/src/Chronicle.cpp`

#### Validation
- Player, NPC, pet, and guardian logs emit differentiated flags
- Downstream parser behavior improves for attribution/filtering

### 2.2 Fix `SPELL_CAST_SUCCESS` destination semantics

**Priority:** High

#### Work
Stop emitting `BaseParams(caster, caster)` blindly.

Implementation options:
1. derive target information from `Spell` target data where available
2. emit empty destination when target is ambiguous
3. extend later with per-target detail from `Spell::m_UniqueTargetInfo`

#### Areas affected
- `modules/mod-chronicle/src/Chronicle.cpp`
- `modules/mod-chronicle/src/ChronicleLogs_SC.cpp`

#### Validation
- Self-buff
- Single-target offensive spell
- AoE spell
- Ground-targeted spell
- Summon/totem spell

### 2.3 Audit and normalize event formatting consistency

**Priority:** Medium

#### Work
Review formatter methods for consistency in:
- separators
- quoting
- `nil` usage
- field ordering
- hex formatting
- empty source or destination handling

Particular audit targets:
- `EnvironmentalDamage`
- `SpellInterrupt`
- `EncounterCredit`
- `SpellSummon`
- `UnitInfo`
- `CombatantInfo`

#### Areas affected
- `modules/mod-chronicle/src/Chronicle.cpp`
- `modules/mod-chronicle/README.md`
- `modules/mod-chronicle/AGENTS.md`

#### Validation
- Sample logs remain parseable by Chronicle
- Event field ordering is stable and documented

### 2.4 Validate overkill and damage-slot splitting logic

**Priority:** Medium

#### Work
Review and verify:
- melee slot 0 / slot 1 overkill allocation
- reflect overkill behavior
- slot-specific absorbed, resisted, and blocked semantics

#### Areas affected
- `modules/mod-chronicle/src/ChronicleLogs_SC.cpp`
- comments in `Chronicle.cpp` if needed

#### Validation
- dual-school melee case
- reflect-damage case
- target death on first slot vs second slot

### 2.5 Confirm absorb and mitigation semantics

**Priority:** Medium

#### Work
Audit:
- `SWING_DAMAGE`
- `SPELL_DAMAGE`
- `DAMAGE_SHIELD`
- `SPELL_ABSORBED`

Ensure absorbed, blocked, and resisted amounts are coherent and not double-counted.

#### Areas affected
- `modules/mod-chronicle/src/Chronicle.cpp`
- `modules/mod-chronicle/src/ChronicleLogs_SC.cpp`

#### Validation
- shield absorb case
- resist-heavy spell case
- block-heavy melee case

---

## Phase 3 — Performance and lifecycle robustness

### 3.1 Remove per-line flushing

**Priority:** High

#### Work
- Replace `std::endl` in `CombatLogWriter::WriteLine()` with `\n`
- Keep explicit flushes only where needed:
  - encounter boundary
  - snapshot creation
  - instance close

#### Areas affected
- `modules/mod-chronicle/src/Chronicle.cpp`

#### Validation
- Event-heavy logging produces correct output
- No missing tail lines on clean shutdown

### 3.2 Review lock granularity in `InstanceTracker`

**Priority:** Medium

#### Work
Only if profiling justifies it, separate locking concerns for:
- writer map access
- seen-unit tracking
- upload lifecycle state

Do not refactor preemptively without evidence of contention.

#### Areas affected
- `modules/mod-chronicle/src/Chronicle.h`
- `modules/mod-chronicle/src/Chronicle.cpp`

#### Validation
- Stress test with many periodic events
- Compare contention behavior if profiling is available

### 3.3 Add idle log rotation or inactivity close policy

**Priority:** High

#### Work
Implement configurable behavior to stop logs growing forever during idle periods:
- flush and close after inactivity timeout
- reopen or rotate when combat resumes

#### Suggested config
- `Chronicle.IdleCloseSeconds = 900`
- `Chronicle.RotateOnIdle = 1`

#### Areas affected
- `modules/mod-chronicle/src/Chronicle.h`
- `modules/mod-chronicle/src/Chronicle.cpp`
- `modules/mod-chronicle/conf/mod_chronicle.conf.dist`
- `modules/mod-chronicle/README.md`

#### Validation
- Idle raid closes log after timeout
- Combat resume reopens or rotates correctly
- Upload semantics still behave predictably

### 3.4 Bound and configure snapshot upload behavior

**Priority:** Medium

#### Work
Review whether every encounter-credit event should trigger a snapshot upload, and make it configurable if needed.

#### Suggested config
- `Chronicle.UploadSnapshots = 1`
- `Chronicle.SnapshotOnEncounterCredit = 1`

#### Validation
- Repeated encounter updates do not flood snapshot uploads
- Successful upload removes only the snapshot copy, not the active log

---

## Phase 4 — Feature completeness

### 4.1 Use `OnInstanceIdRemoved` as cleanup safety net

**Priority:** Medium

#### Work
Add `OnInstanceIdRemoved(uint32 instanceId)` support as a backup cleanup path for stale instance writers.

#### Areas affected
- `modules/mod-chronicle/src/ChronicleLogs_SC.cpp`
- related hook integration points if needed

#### Validation
- Instance save removal path triggers cleanup correctly

### 4.2 Implement missing event families already identified in `FUTURE.md`

**Priority:** Medium

#### Recommended sub-order
1. `SPELL_PERIODIC_DRAIN`
2. silence-based interrupts
3. `SPELL_DISPEL` / `SPELL_STOLEN`
4. richer per-target `SPELL_CAST_SUCCESS`
5. `LOOT`

#### Areas affected
- `modules/mod-chronicle/src/Chronicle.h`
- `modules/mod-chronicle/src/Chronicle.cpp`
- `modules/mod-chronicle/src/ChronicleLogs_SC.cpp`

#### Validation
- One reproducible gameplay scenario per event family

### 4.3 Improve attribution and stable identity

**Priority:** Medium

#### Work
From `FUTURE.md`:
- add recursive owner-chain resolution
- use stable spawn identity for creatures where appropriate
- filter irrelevant scope/noise where useful

#### Areas affected
- `modules/mod-chronicle/src/Chronicle.cpp`
- `modules/mod-chronicle/src/ChronicleLogs_SC.cpp`

#### Validation
- Hunter pet
- Warlock pet
- Totem/guardian
- Respawning NPC identity behavior

### 4.4 Design account-linking support before implementation

**Priority:** Low

#### Work
Create a design note before code for:
- emitted account-linking data
- privacy implications
- proof-of-ownership flow
- account ID vs hashed identifier

---

## Phase 5 — Documentation, tests, and upstreaming

### 5.1 Fix documentation drift

**Priority:** High

#### Problems to correct
- `AGENTS.md` describes older architecture and format assumptions
- `mod_chronicle.conf.dist` still references legacy pipe-delimited wording
- current code emits comma-separated WotLK-style lines
- docs need to reflect Beast/zlib upload behavior instead of older shell-out assumptions

#### Work
Make the public and contributor docs match actual implementation:
- `README.md`
- `AGENTS.md`
- `conf/mod_chronicle.conf.dist`
- optionally tighten wording in `FUTURE.md`

#### Validation
- A new contributor can understand current behavior without guessing

### 5.2 Add formatter regression tests

**Priority:** High

#### Work
Add tests for `EventFormatter` output with golden or snapshot-style assertions covering:
- GUID formatting
- quoting
- field ordering
- spell prefix formatting
- damage suffix formatting
- encounter event formatting

If fully mocking AzerothCore types is heavy, start with helper-level tests and sample-line regression tests.

#### Validation
- Tests fail on accidental log-format drift

### 5.3 Add operational smoke-test checklist

**Priority:** Medium

#### Work
Document a manual validation pass covering:
- dungeon entry
- melee damage
- spell damage
- healing
- environmental damage
- encounter kill/wipe
- upload success
- upload failure retention

#### Areas affected
- `modules/mod-chronicle/README.md`
- or a dedicated `TESTING.md`

### 5.4 Prepare upstream hook contribution plan

**Priority:** Medium

#### Work
Split upstreaming into:
1. core hook PR with read-only observer hooks and no gameplay changes
2. module cleanup once hook interfaces stabilize

#### Deliverables
- list of all custom hooks
- justification per hook
- regression-risk statement
- validation notes

---

## Suggested sprint backlog

### Sprint 1 — blockers and hardening
- [ ] Rotate secret and scrub operational exposure
- [x] Add TLS verification and host verification
- [x] Add connect/read/write timeouts
- [x] Preserve failed uploads
- [x] Replace detached uploads with managed lifecycle
- [x] Replace `std::endl` flushing

### Sprint 2 — correctness
- [x] Use real unit flags in `BaseParams`
- [x] Expand `UnitFlags` coverage
- [x] Fix `SPELL_CAST_SUCCESS` destination semantics
- [x] Audit format consistency
- [x] Validate overkill and mitigation semantics

### Sprint 3 — robustness
- [x] Add idle log close or rotation
- [x] Review snapshot upload frequency/configuration
- [ ] Refine lock granularity only if profiling justifies it
- [x] Add `OnInstanceIdRemoved` safety net

### Sprint 4 — coverage
- [x] Implement `SPELL_PERIODIC_DRAIN`
- [x] Add silence-based interrupt handling
- [x] Add `SPELL_DISPEL` / `SPELL_STOLEN`
- [x] Add richer cast target result logging
- [x] Add `LOOT`
- [ ] Improve owner-chain and spawn identity attribution

### Sprint 5 — polish
- [x] Update `AGENTS.md`
- [x] Fix config comments
- [ ] Add formatter regression tests
- [x] Add manual smoke-test checklist
- [x] Draft upstream hook contribution package

---

## Definition of done

`mod-chronicle` should be considered production-ready when all of the following are true:

- uploads are TLS-verified and timeout-bounded
- failed uploads are retained and retryable
- log lines include meaningful unit flags
- `SPELL_CAST_SUCCESS` no longer misstates destination
- per-line flush overhead is removed
- idle instance logs no longer grow forever
- documentation reflects actual implementation
- basic formatter regression tests exist
