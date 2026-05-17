# mod-chronicle Smoke Test Checklist

This checklist is the manual validation pass for `mod-chronicle` after formatter,
hook, upload, or lifecycle changes.

## Preconditions

- `worldserver` is built with `mod-chronicle` enabled.
- Chronicle is enabled in `mod_chronicle.conf`.
- If upload behavior is being tested, set both:
  - `Chronicle.UploadURL`
  - `Chronicle.UploadSecret`
- Know the Chronicle log directory (default: `env/dist/logs/chronicle_logs/`).

## Core file lifecycle

### 1. Instance preamble

- Enter a dungeon or raid with at least one player.
- Verify a fresh log file is created for that instance.
- Verify the file begins with:
  - `CHRONICLE_HEADER`
  - `CHRONICLE_ZONE_INFO`
  - one `CHRONICLE_COMBATANT_INFO` per player currently in the instance

### 2. Re-entry behavior

- Leave and re-enter without destroying the instance.
- Verify Chronicle does not duplicate old unit preamble lines unless a new log
  segment was intentionally started.

## Combat events

### 3. Unit metadata

- Attack at least one hostile NPC.
- Verify a `CHRONICLE_UNIT_INFO` line appears before or alongside the first
  combat event involving that NPC.
- Confirm the line includes:
  - GUID
  - unit name
  - level
  - non-empty flag field
  - owner GUID or zero GUID
  - max health
  - `"HOSTILE"`, `"FRIENDLY"`, or `"NEUTRAL"`
  - `true`/`false` boss marker

### 4. Melee and spell damage

- Produce at least one melee hit.
- Produce at least one direct-damage spell hit.
- Verify:
  - `SWING_DAMAGE` or `SWING_MISSED`
  - `SPELL_DAMAGE` or `SPELL_MISSED`
  - expected spell prefix ordering
  - mitigation fields are present in the right slots

### 5. Healing and periodic effects

- Cast a direct heal.
- Apply a HoT or DoT.
- Test a mana-drain effect if available (for example Viper Sting / Drain Mana).
- Verify:
  - `SPELL_HEAL`
  - `SPELL_PERIODIC_DAMAGE`
  - `SPELL_PERIODIC_HEAL`
  - `SPELL_PERIODIC_DRAIN` where applicable

### 6. Interrupts, dispels, and absorbs

- Interrupt a cast with a standard interrupt.
- If available, test a silence-style interrupt path.
- Dispel or steal a buff/debuff.
- Trigger an absorb effect such as Power Word: Shield.
- Verify:
  - `SPELL_INTERRUPT`
  - `SPELL_DISPEL` or `SPELL_STOLEN`
  - `SPELL_ABSORBED`

If you can reproduce a silence-aura interrupt where the aura caster despawns or
leaves the world before the interrupt fires, verify Chronicle still logs
`SPELL_INTERRUPT` with an empty source GUID/name/flags rather than dropping the
event.

### 7. Summon and target-result coverage

- Use a summon spell or summon-capable item.
- Cast a spell that can hit, miss, immune, or reflect on specific targets.
- Verify:
  - `SPELL_SUMMON`
  - `CHRONICLE_SPELL_TARGET_RESULT`

### 8. Loot and environmental damage

- Loot at least one item from a creature or container.
- Loot money.
- Trigger environmental damage if safe to do so.
- Verify:
  - `CHRONICLE_LOOT_ITEM`
  - `CHRONICLE_LOOT_MONEY`
  - `ENVIRONMENTAL_DAMAGE`

## Encounter and lifecycle behavior

### 9. Encounter boundaries

- Pull a boss that uses AzerothCore encounter state.
- Kill it once and, if practical, test a wipe/reset on another encounter.
- Verify:
  - `CHRONICLE_ENCOUNTER_START`
  - `CHRONICLE_ENCOUNTER_END`
  - `CHRONICLE_ENCOUNTER_CREDIT`

### 9a. Alive despawn coverage

- Trigger a mechanic that removes a creature without killing it (temporary
  summons expiring, scripted vanish/despawn, forced despawn cleanup, etc.).
- Verify Chronicle logs:
  - `CHRONICLE_UNIT_DESPAWN`
- Confirm Chronicle does not emit a fake `UNIT_DIED` for that same alive
  despawn path.

### 10. Snapshot behavior

- With uploads enabled, trigger encounter credit.
- Verify a temporary `.snap` file is created and uploaded.
- Confirm successful snapshot upload deletes only the snapshot copy and not the
  active `.log` file.

### 11. Failed upload retention

- Point `Chronicle.UploadURL` at an invalid or unavailable endpoint.
- End an instance or force a snapshot upload.
- Verify failed upload files remain on disk for retry.

### 12. Startup orphan retry

- Leave a failed `.log` or `.snap` in the Chronicle log directory.
- Restart `worldserver` with upload configured.
- Verify Chronicle queues the orphaned file for retry on startup.

### 13. Idle rotation

- Set:
  - `Chronicle.IdleCloseSeconds > 0`
  - `Chronicle.RotateOnIdle = 1`
- Allow the instance log to sit idle past the configured threshold.
- Resume combat.
- Verify Chronicle rotates to a fresh segment and preserves/uploads the idle
  segment according to configuration.

## Quick sign-off

The smoke test is considered passed when:

- preamble lines are correct
- representative combat families serialize correctly
- loot and encounter extensions appear as expected
- upload success deletes only successfully uploaded files
- upload failure preserves retryable files
- idle rotation behaves predictably
