# Native initialization for legacy TAS recordings

Legacy `.tas` records contain sampled keys and frame deltas, but no initial
physics RNG or movement-scheduler state. The original game's startup physicalizes
the paper, stone and wood fragment resources and later retires them. Loading-frame
timing affects how many physics steps run during that lifetime. Consequently,
even identical native binaries can inherit different level baselines.

On macOS, BallanceTAS observes the actual startup fragment owners. At the first
retirement it calls `TT_PhysicsCompleteFreefallPreloadV1` through the registered
Physics Manager DLL. The runtime validates that these owners comprise the entire
live physics world: independent single-core units, gravity-only controllers,
no pending impulse or contact, and pairwise collision exclusion. Unsupported
worlds are rejected before stepping.

Completion uses actual physics steps until all units have the engine's existing
fast-object flag. In the original preload, movement-check RNG consumption stops
at that state. The operation does not assign a fitted RNG seed, impose a fixed
warm-up duration, change record deltas or add artificial sleeps. Its iteration
limit is a failure bound. Failures after stepping report the number of completed
steps; this initialization operation is not a transactional rollback API.

The session belongs to the mod instance, records owner identity and retirement,
and closes at the first menu event. Later gameplay fragments, map reloads and
checkpoint restores cannot rearm it. Original resources must load while the mod
is present; loading the mod after startup does not establish a qualified baseline.

## Repeated level loads

`TAS.PreserveLevelHistory` defaults to off. In that mode a TAS load restores
the first load's physics time factor, movement-check countdown and random seed.
Turn it on for old recordings that require a later playback in the same process:
subsequent loads then inherit those values from the previous level, without
injecting a seed, advancing a fitted number of frames or changing the record.
The setting is captured when loading begins and does not modify a running level
or an in-memory checkpoint. To reproduce an old second-play environment, start
a fresh process and perform the same first playback and normal exit/load flow;
switching it on after unrelated gameplay cannot reconstruct that history.

Both modes retain behavior execution ordering and the existing native clock
reset. Keeping physics history does not imply keeping arbitrary script-list
ordering: disabling that ordering made the second load one frame early in the
SR03 comparison. Nor is the option a complete standard-A environment preset:
TASSupport 0.3.0 preserves the raw physics generation on clock reset, while the
native reset currently clears it. The legacy format carries no environment
metadata with which to choose these contracts automatically.

Two same-process SR03_1.46.066_240 playbacks with preserved history matched the
original A runs' 19,284 and 19,289 captured input, SR timer, pose, core and physics
clock records, with no time shift. Raw generation differs. Both original runs
failed to finish, so this is failed-path parity, not successful-tape qualification.

Playback and recording requests can be prepared before startup finishes.
Readiness is checked when they activate, before samples, recorder state or
record callbacks start. An unsupported or incomplete preload produces an explicit
initialization error. Windows retains its existing initialization path.

## Validation and limits

Native integration was checked using minimum, captured fast, captured slow and
natural loading clocks. All four completed the SR07 recording and matched the
captured original player trajectory; the full native player and pusher streams
matched the successful control after initialization. Original preload observations
also matched chronological positions, movement state, RNG/countdown and relative
physics-step times. Test clocks are diagnostic fixtures, not runtime defaults.

`LegacyPreloadSessionTest` covers owner lifecycle, failure handling and session
scope. BuildingBlocks' `physics_preload_api_tests` uses real CK/IVP worlds to
check rejection without simulation changes, completion, idempotence and separate
contexts in supported fast/precise solver modes.

These checks do not establish full-world parity, all-recording compatibility or
checkpoint correctness. Absolute raw physics-generation offsets from the original
capture remain under investigation. New recording formats still need explicit
initial-state and resource-version metadata; this legacy initialization contract
does not make arbitrary modified assets or inherited mid-game state portable.
