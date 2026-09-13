# Changelog

All notable changes to this project are documented in this file.

## [1.1] - 2026-09-13

### Added
- `DeadEyeDiag`: the same confirmed script-thread choke applied to Dead
  Eye's active window, resolving the local player's ability object via
  the pointer chain shared by `_GET_PLAYER_DEAD_EYE`/`_ACTIVATE_DEAD_EYE`
  and busy-waiting while the "Dead Eye active" flag is set.

### Changed
- The cast-race delay moved out of the `TASK::_GET_TASK_FISHING` hook
  and directly into `ScriptMain`'s loop, confirmed to still fix the cast
  reliably from there -- proving the fix works by giving the racing
  worker thread wall-clock time anywhere on the shared script thread,
  not by intercepting the native call itself.
- `MaybeDelayForCastRaceTest` renamed to `FishingFix::Tick()` now that
  it's the confirmed fix rather than a test.
- The FPS estimate used by both chokes is now computed once in
  `ScriptMain` and passed to both, instead of being tracked
  independently by each.

### Removed
- The 120fps floor gating both chokes -- the FPS estimate proved
  unreliable in testing and could skip the choke exactly when the race
  it guards against was happening, sometimes causing the very bug it
  fixes. Both chokes now run unconditionally every script tick, gated
  only by their own phase/active check.
- The now-dead per-script native-hooking machinery (`NativeHook`,
  `PatternScan`, the vendored `external/RDR-Classes` tree) and
  `DeadEyeDiag`'s F9 mark-log hotkey.

## [1.0] - 2026-09-12

### Added
- Initial release: fixes a cross-thread read/update race in
  `TASK::_GET_TASK_FISHING` that causes fishing rod casts to fail
  intermittently above ~60 FPS, by hooking the native (scoped to
  `fishing_core`'s own script) and inserting a tuned precise wait
  before the real call so the task's worker thread finishes its update
  before script reads it.
