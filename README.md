# Cool Ragdolls — localized linear + rotational impulses

Build: `ragdoll-impulse-poc13-localized-rotation-experimental`

For the inspected **Steam Kenshi 1.0.65 x64**, RE_Kenshi 0.3.5 / KenshiLib 0.5.0.
Other executable layouts disable the plugin before hooks are installed.

**Crash-prone experiment, requested for testing reactions first. Use a disposable
save. Visual quality and gameplay stability still need in-game validation.**

## What changed

Retains POC12's delayed, cached-actor application: capture the ragdoll's actors,
wait at least 40 real-time milliseconds, then apply force once on a later unpaused
main-loop tick, after the original loop function returns. This can run on a
different thread from capture. It is not a byte-for-byte rollback: it retains
POC11's per-character, named-bone collection after native setup, rather than the
old global joint-construction bucket. Actor addresses and calculated impulses
are retained across the delay, not reacquired at application.

Adds a separate capped angular impulse to the matched hit bone/area, in addition
to the unchanged linear impulse. For a side cut to the head, the head receives
rotation whose estimated contact point moves WITH the cut. Native joints determine
how that rotation pulls the rest of the body; no whole-skeleton spin is imposed.

Your successful tuning is preserved: base 10, damage scale 15, horizontal cap 4000,
swing 0.30, spread 0.15, and the restrictive speed limiter OFF. An 84-damage hit
still plans 1270 horizontal impulse. Rotation has its own coefficient and cap.

One procedural model handles all impacts:

- Damage determines the capped force budget.
- 85% targets the hit bone/area; 15% spreads across eligible bodies by mass.
- Left/right cuts steer oppositely; rising/falling cuts add up/down force.
- Thrusts push away from the attacker without sideways cut steering.
- Matched regions get added angular impulse from an estimated attacker-facing
  contact offset crossed with the cut-directed force. Reverse the cut and the
  rotation reverses. Up/down cuts tip oppositely; straight thrusts add no twist.
- Current pose and native joints determine any folding, turning or fall.

No canned gut-punch/uppercut routines, artificial opposing-force spins, pose
overrides, teleports, or weapon hitbox collisions are added. The new torque call
uses the verified world-space angular-impulse entry in the installed PhysX 2.8.4 DLL.
Location is Kenshi's approximate body-part-to-bone mapping, not measured blade
contact. Direction comes from positions and the cut enum, not weapon velocity.
Linear force still acts at each selected actor's centre. Angular impulse uses an
estimated contact offset, not an observed blade-contact point or measured bone
radius. Current pose, inertia, damping and joints determine the actual response;
particular spins or poses are not guaranteed.

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

- `BaseImpulse=10`, `DamageScale=15`, `MaxImpulse=4000`:
  `H = min(BaseImpulse + FinalDamage * DamageScale, MaxImpulse)`.
  Hits of 10/30/84 damage request 160/460/1270. Hits of 266 or more reach the cap
  and intentionally have the same horizontal budget.
- `ImpactSpread=0.15`: strongly localized. `0` concentrates everything on the
  matched region; `1` spreads by mass over the whole ragdoll. Full strength on
  one bone can be much more violent than the older whole-body shove.
- `SwingDirectionCoefficient=0.30`: preserved steering strength. Suggestions: 0.05 subtle,
  0.15 balanced, 0.30 strong; these are preferences, not validated animation presets.
- `UpwardImpulse=6`: small constant lift. Up/down cuts additionally add
  `+/- H * SwingDirectionCoefficient`, capped by `MaxDirectionalVerticalImpulse=1050`.
- `MaxActorSpeedChange=0`: optional per-actor speed limiter OFF. Overall caps
  still apply. Positive values limit `length(ActorImpulse) / ActorMass`, reducing
  all shares together. Engine velocity units are uncalibrated; POC11's 30 was too
  restrictive for the mass-one rigs observed. Leave 0 for this first test.
- `ImpulseDelayMs=40`: minimum wall-clock delay, allowed 1..500 ms, not adjusted
  for 0.5x gameplay. Application waits for an eligible main-loop tick, not a sleep.
- `RotationalImpulseCoefficient=1.0`: new rotation strength. It acts as an
  effective lever arm on the attacker-facing side of the struck body. Try 0.5
  restrained, 1.0 initial, or 2.0 stronger. Units are uncalibrated; these are test
  preferences, not validated presets. Allowed 0..100; **0 disables added rotation**.
- `MaxRotationalImpulse=1000`: cap on total angular impulse per hit, not per actor.
  Allowed 0..100000; 0 also disables rotation. It never reduces linear knockback.
  At 84 damage with swing 0.30 and coefficient 1, a side cut requests about 365
  angular impulse, an up/down cut 381. Values are not degrees, RPM, or a pose.
- `CrashTrace=1`: append/flush actor-call breadcrumbs to
  `RagdollImpulse_CrashTrace.log` beside the DLL. Adds disk I/O; the log grows
  across runs. Preserve it before disabling tracing or clearing old logs manually.

Old `ExpectedActorCount`, `CaptureGraceMs`, `AngularImpulseCoefficient`,
`MaxAngularImpulse`, area
spin/lift multipliers and variation keys are ignored. There is no 13-person limit:
bounds are 64 eligible actors per ragdoll, 512 native bone records, 512 recent
character hits, and 128 queued impulses. Queue overflow is logged and skipped.
Animals use their actual actor counts; exact-bone matching falls back to area/side,
then whole body. Animal visuals remain untested.

Rotation uses only exact-bone or body-area matches; unknown-region whole-body
fallback receives no invented twist. Area matches share the one angular budget by
mass. `ImpactSpread` still controls linear force only; it does not spread the new
torque onto unrelated limbs. Rotation does not alter the 40 ms delay or add a
second later-frame event. The constant `UpwardImpulse` is excluded from rotation.

## First test and logs

1. Use a disposable save, shipped INI, and one melee knockout of Kazz.
2. Observe movement, the struck region leading, directional folding/turning and
   excessive launch. Keep 0.5x gameplay if desired.
   Prioritize a head side-cut, an opposite side-cut, then a torso hit and a thrust.
   For an A/B comparison, restart with `RotationalImpulseCoefficient=0`, leaving
   every other setting unchanged. There should be no new torque records in that run.
3. **Leave Kenshi open and report the result** so its live `RE_Kenshi_log.txt`
   can be preserved. Do not relaunch first: startup can replace that log.
4. If successful, try other hit locations/cuts, recovery/re-knockout, then several
   combatants. Test animals separately. Queued events expire after 1500 ms;
   a long pause/stall deliberately drops the hit instead of applying old force.

Expected sequence (match the same hit ID):

```text
build_marker=ragdoll-impulse-poc13-localized-rotation-experimental
rotation_adapter ready=1 ...
ready=1 actor_calls=delayed_main_loop
melee_final ... recorded=1
ragdoll_armed hit=...
deferred_queued hit=... bone="..." match=... planned_horizontal=... safety_scale=1.00000 requested_rotation=... planned_rotation=...
deferred_apply_begin hit=... age_ms=... capture_thread=...
deferred_actor_begin hit=... actor=... bone="..." impulse=(...)
deferred_force_enter hit=... actor=... method=...
deferred_actor_returned hit=... actor=... call_returned=1
deferred_torque_begin hit=... actor=... bone="..." angular_impulse=(...)
deferred_torque_enter hit=... actor=... method=...
deferred_torque_returned hit=... actor=... call_returned=1
deferred_apply_end hit=...
```

Torque records occur only for the targeted region and a nonzero planned rotation.
`planned_horizontal` and `planned_rotation` are mathematical budgets, not measured motion.
`call_returned=1` does not prove physics retained force. Report what you see.

## Crash risk and rollback

Cached actors can be destroyed or changed between capture and use. Snapshot
method checks and guarded reads reject some changes, but do not prove lifetime
or thread-safe physics access. Same-address reuse, destruction after validation,
and pure-virtual calls elsewhere in the engine can still crash. No global
purecall handler is installed to conceal failures.

The added torque adapter is separately gated to the inspected PhysX binary and
actor vtable. An unsupported adapter skips rotation while keeping linear impulses.
A caught torque validation/call failure logs `rotation_disabled_for_session` and
stops further custom torque; the existing linear path retains its own checks.
These guards do not guarantee crash protection or prevent extreme solver motion.

Recovery/new setup, failed transitions and save loading cancel queued events;
cancellation cannot stop an actor call already begun. Invalid native data disables
custom physics for the session. No-body/expired/full-queue events are logged and
skipped. If native setup precedes completion of the final melee hook, that hit can
still be missed. Not every coverage/lifetime edge is solved by this experiment.

After a crash, preserve `RE_Kenshi_log.txt`, the mod's
`RagdollImpulse_CrashTrace.log`, and the newest crash archive before relaunching.
The last flushed breadcrumb narrows the investigation; it is not a stack trace
or proof of cause. Matching DLL/PDB symbols remain in the development workspace.

Set `RotationalImpulseCoefficient=0` and restart for linear-only behavior, or
`Enabled=0` for no plugin hooks. The previous POC12 package is backed up in:
`diagnostics/before-poc13-20260914-155445/Ragdoll`.
Its sibling `UserTuning.ini` preserves your successful 10/15/4000, swing 0.30
configuration (the old staged POC12 INI still contained stronger original defaults).
BetterPhysics may suppress intentional lift; FallDamage can punish launches.
Compatibility, visuals and large-fight stability require gameplay tests.
