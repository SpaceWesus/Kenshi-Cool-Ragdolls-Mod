# Cool Ragdolls — experimental delayed-force build

Build: `ragdoll-impulse-poc12-delayed-localized-experimental`

For the inspected **Steam Kenshi 1.0.65 x64**, RE_Kenshi 0.3.5 / KenshiLib 0.5.0.
Other executable layouts disable the plugin before hooks are installed.

**Crash-prone experiment, requested for testing reactions first. Use a disposable
save. Visual quality and gameplay stability still need in-game validation.**

## What changed

Restores delayed, cached-actor impulse application: capture the ragdoll's actors,
wait at least 40 real-time milliseconds, then apply force once on a later unpaused
main-loop tick, after the original loop function returns. This can run on a
different thread from capture. It is not a byte-for-byte rollback: it retains
POC11's per-character, named-bone collection after native setup, rather than the
old global joint-construction bucket. Actor addresses and calculated impulses
are retained across the delay, not reacquired at application.

The restrictive POC11 speed limiter is disabled by default. Horizontal impulse
remains capped at 7500, total vertical impulse at +/-1050. An 84-damage hit now
plans the full 7500 horizontal budget, not about 35.

One procedural model handles all impacts:

- Damage determines the capped force budget.
- 85% targets the hit bone/area; 15% spreads across eligible bodies by mass.
- Left/right cuts steer oppositely; rising/falling cuts add up/down force.
- Thrusts push away from the attacker without sideways cut steering.
- Current pose and native joints determine any folding, turning or fall.

No canned gut-punch/uppercut routines, artificial opposing-force spins, pose
overrides, teleports, torque API calls, or weapon hitbox collisions are added.
Location is Kenshi's approximate body-part-to-bone mapping, not measured blade
contact. Direction comes from positions and the cut enum, not weapon velocity.
Force acts at each selected actor's centre. Particular spins or poses are not
guaranteed; precise contact-point twisting is not implemented.

## Install

1. Exit Kenshi.
2. Copy **all four files** into the existing **Cool Ragdolls** mod folder,
   replacing matching files: `RagdollImpulse.dll`, `RagdollImpulse_Config.ini`,
   `RE_Kenshi.json`, and `README.md`.
3. Keep the existing `.mod` file. Do not install a second copy of the DLL.
4. Launch through RE_Kenshi with the mod enabled.

Copy the new INI too: retaining `MaxActorSpeedChange=30` will still suppress force.
Nothing is deployed into Kenshi automatically.

## Tuning

Restart after editing. Keep INI comments on separate lines.

- `BaseImpulse=1000`, `DamageScale=100`, `MaxImpulse=7500`:
  `H = min(BaseImpulse + FinalDamage * DamageScale, MaxImpulse)`.
  Hits of 10/30/84 damage request 2000/4000/7500. Hits of 65 or more reach the cap
  and intentionally have the same horizontal budget.
- `ImpactSpread=0.15`: strongly localized. `0` concentrates everything on the
  matched region; `1` spreads by mass over the whole ragdoll. Full strength on
  one bone can be much more violent than the older whole-body shove.
- `SwingDirectionCoefficient=0.15`: steering strength. Suggestions: 0.05 subtle,
  0.15 balanced, 0.30 strong; these are preferences, not validated animation presets.
- `UpwardImpulse=6`: small constant lift. Up/down cuts additionally add
  `+/- H * SwingDirectionCoefficient`, capped by `MaxDirectionalVerticalImpulse=1050`.
- `MaxActorSpeedChange=0`: optional per-actor speed limiter OFF. Overall caps
  still apply. Positive values limit `length(ActorImpulse) / ActorMass`, reducing
  all shares together. Engine velocity units are uncalibrated; POC11's 30 was too
  restrictive for the mass-one rigs observed. Leave 0 for this first test.
- `ImpulseDelayMs=40`: minimum wall-clock delay, allowed 1..500 ms, not adjusted
  for 0.5x gameplay. Application waits for an eligible main-loop tick, not a sleep.
- `CrashTrace=1`: append/flush actor-call breadcrumbs to
  `RagdollImpulse_CrashTrace.log` beside the DLL. Adds disk I/O; the log grows
  across runs. Preserve it before disabling tracing or clearing old logs manually.

Old `ExpectedActorCount`, `CaptureGraceMs`, angular impulse settings, area
spin/lift multipliers and variation keys are ignored. There is no 13-person limit:
bounds are 64 eligible actors per ragdoll, 512 native bone records, 512 recent
character hits, and 128 queued impulses. Queue overflow is logged and skipped.
Animals use their actual actor counts; exact-bone matching falls back to area/side,
then whole body. Animal visuals remain untested.

## First test and logs

1. Use a disposable save, shipped INI, and one melee knockout of Kazz.
2. Observe movement, the struck region leading, directional folding/turning and
   excessive launch. Keep 0.5x gameplay if desired.
3. **Leave Kenshi open and report the result** so its live `RE_Kenshi_log.txt`
   can be preserved. Do not relaunch first: startup can replace that log.
4. If successful, try other hit locations/cuts, recovery/re-knockout, then several
   combatants. Test animals separately. Queued events expire after 1500 ms;
   a long pause/stall deliberately drops the hit instead of applying old force.

Expected sequence (match the same hit ID):

```text
build_marker=ragdoll-impulse-poc12-delayed-localized-experimental
ready=1 actor_calls=delayed_main_loop
melee_final ... recorded=1
ragdoll_armed hit=...
deferred_queued hit=... bone="..." match=... planned_horizontal=... safety_scale=1.00000
deferred_apply_begin hit=... age_ms=... capture_thread=...
deferred_actor_begin hit=... actor=... bone="..." impulse=(...)
deferred_force_enter hit=... actor=... method=...
deferred_actor_returned hit=... actor=... call_returned=1
deferred_apply_end hit=...
```

`planned_horizontal` is a mathematical budget, not measured motion.
`call_returned=1` does not prove physics retained force. Report what you see.

## Crash risk and rollback

Cached actors can be destroyed or changed between capture and use. Snapshot
method checks and guarded reads reject some changes, but do not prove lifetime
or thread-safe physics access. Same-address reuse, destruction after validation,
and pure-virtual calls elsewhere in the engine can still crash. No global
purecall handler is installed to conceal failures.

Recovery/new setup, failed transitions and save loading cancel queued events;
cancellation cannot stop an actor call already begun. Invalid native data disables
custom physics for the session. No-body/expired/full-queue events are logged and
skipped. If native setup precedes completion of the final melee hook, that hit can
still be missed. Not every coverage/lifetime edge is solved by this experiment.

After a crash, preserve `RE_Kenshi_log.txt`, the mod's
`RagdollImpulse_CrashTrace.log`, and the newest crash archive before relaunching.
The last flushed breadcrumb narrows the investigation; it is not a stack trace
or proof of cause. Matching DLL/PDB symbols remain in the development workspace.

Set `Enabled=0` and restart for no plugin hooks. POC11 is backed up in the workspace:
`diagnostics/before-poc12-20260914-044654/Ragdoll`.
BetterPhysics may suppress intentional lift; FallDamage can punish launches.
Compatibility, visuals and large-fight stability require gameplay tests.
