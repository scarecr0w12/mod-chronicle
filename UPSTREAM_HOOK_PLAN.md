# mod-chronicle Upstream Hook Contribution Plan

This document packages the current observer-hook story for upstream AzerothCore
discussion.

## Goal

Upstream the read-only hook surface that `mod-chronicle` needs so the module can
run against a cleaner, less fork-dependent core.

## Contribution split

### PR 1 — observer hooks only

Scope:

- add read-only ScriptMgr observer hooks at packet/log emission points
- no gameplay behavior changes
- no Chronicle-specific logic in core
- include targeted validation notes per hook family

### PR 2 — module cleanup/alignment

Scope:

- adjust `mod-chronicle` once the final upstream hook shapes settle
- remove or minimize fork-specific assumptions

## Hook inventory

### Custom UnitScript hooks

- `OnSendAttackStateUpdate(CalcDamageInfo const*, int32)`
- `OnSendSpellNonMeleeDamageLog(SpellNonMeleeDamage const*, int32)`
- `OnSendHealSpellLog(HealInfo const&, bool)`
- `OnSendSpellMiss(Unit*, Unit*, uint32, SpellMissInfo)`
- `OnSendSpellDamageImmune(Unit*, Unit*, uint32)`
- `OnSendSpellDamageResist(Unit*, Unit*, uint32)`
- `OnSendSpellNonMeleeReflectLog(SpellNonMeleeDamage*, Unit*)`
- `OnSendEnergizeSpellLog(Unit*, Unit*, uint32, uint32, Powers)`
- `OnSendPeriodicAuraLog(Unit*, SpellPeriodicAuraLogInfo*)`
- `OnDealDamageShieldDamage(Unit*, Unit*, SpellInfo const*, uint32, uint32, uint32)`
- `OnSchoolAbsorbApplied(DamageInfo&, SpellInfo const*, Unit*, uint32)`

### Custom GlobalScript hooks

- `OnSpellSendSpellGo(Spell*)`
- `OnSpellExecuteLogSummonObject(Spell*, WorldObject*)`
- `OnAuraApplicationClientUpdate(Unit*, Aura*, bool)`
- `OnSpellInterrupt(Unit*, Unit*, uint32, uint32)`
- `OnSpellDispel(Unit*, Unit*, uint32, uint32, bool)`

### Custom PlayerScript hooks

- `OnEnvironmentalDamage(Player*, EnviromentalDamage, uint32)`

### Custom AllCreatureScript hooks

- `OnBeforeCreatureDespawn(Creature*)`

### Mainline hooks Chronicle already uses

These do not need upstream work, but they matter for the complete integration:

- `OnInstanceIdRemoved(uint32)`
- `OnBeforeSetBossState(...)`
- `OnAfterUpdateEncounterState(...)`
- existing map enter/leave/destroy hooks used for writer lifecycle

## Why these hooks exist

Each custom hook gives Chronicle access to the same data AzerothCore is already
about to serialize or broadcast:

- final combat packet values
- resolved mitigation breakdowns
- resolved miss/interrupt/dispel outcomes
- cast-target result data after spell targeting is complete
- client-visible aura changes

This avoids re-deriving packet-visible results from lower-level gameplay state
after information has already been collapsed or lost.

## Risk statement

These hooks should be presented as:

- **observer-only**
- **no gameplay mutation**
- **zero behavior change unless a script registers them**
- **low regression risk** when inserted at existing packet/log emission points

Primary regression considerations to call out:

- avoid duplicate notifications
- avoid expensive allocations in hot paths
- preserve existing packet/control flow ordering
- ensure null-safety for optional targets/casters

## Validation notes for upstream discussion

Minimum validation set:

- melee hit/miss packet path
- direct spell hit/miss/immune/reflect path
- heal path
- periodic aura log path including mana leech
- absorb attribution path
- aura apply/remove client update path
- summon logging path
- interrupt path including silence-based cases
- dispel/steal success path
- environmental damage path
- alive despawn path that does not emit `UNIT_DIED`

## Deliverables expected alongside a PR

- hook list and signatures
- insertion points in core
- one-paragraph rationale per hook family
- regression-risk statement
- validation summary from local Chronicle runs

## Notes

Chronicle no longer depends on the older shell-out upload path or pipe-delimited
addon serialization assumptions; upstream messaging should focus on the
observer-hook value, not historical implementation details.
