# Changelog

All notable changes to this project are documented in this file.

## [1.3] - 2026-09-23

### Fixed
- The game no longer risks crashing at startup when `FishingFix.log` can't
  be written.
- If the game folder can't be written (e.g. a `C:\Program Files` install,
  or a read-only/locked log file), the log now goes to
  `%LOCALAPPDATA%\RDR2ASIMods\FishingFix.log` instead, and its first line
  names the path that couldn't be used.
- Companions fishing alongside you: a ped that despawned is no longer read
  from, since its memory may already have been reused.
- Dead Eye fix: the Dead Eye state is looked up fresh every frame instead
  of trusting a cached pointer that could go stale (e.g. after a player
  model change).

### Changed
- Nearby fishing NPCs are found by scanning the ped pool once a second
  instead of every frame.

## [1.2] - 2026-09-15

### Added
- Release metadata file so the project exposes a canonical release version.

### Changed
- Finalized the confirmed cast-race and Dead Eye fixes into the mainline
  release, keeping the delay in `ScriptMain`'s tick loop and removing the
  legacy native-hooking scaffolding.
- Updated the project notes and release summary to reflect the confirmed raw
  task-memory reader, the 0-4 pre-commit gate, and the local/companion NPC
  coverage.

## [1.1] - 2026-09-13

### Added
- `DeadEyeFix`: the same confirmed script-thread choke applied to Dead
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
  `DeadEyeFix`'s F9 mark-log hotkey.

## [1.0] - 2026-09-12

### Added
- Initial release: fixes a cross-thread read/update race in
  `TASK::_GET_TASK_FISHING` that causes fishing rod casts to fail
  intermittently above ~60 FPS, by hooking the native (scoped to
  `fishing_core`'s own script) and inserting a tuned precise wait
  before the real call so the task's worker thread finishes its update
  before script reads it.
