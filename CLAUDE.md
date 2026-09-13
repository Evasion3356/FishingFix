# FishingFix

A ScriptHookRDR2 ASI that fixes a real bug: holding right click (aim)
then left click to cast the fishing rod fails intermittently above ~60
FPS -- Arthur pulls the rod back for ~0.5s then releases it, over and
over, never completing the cast, only when uncapped/high FPS. Sibling of
`../PokerCheat`/`../BlackjackCheat` (same ScriptHookRDR2 + native C++
toolchain) but architecturally different from both -- see "Architecture"
below.

**Status: fixed and confirmed live.** See `src/FishingFix.cpp`'s header
comment for the full root-cause trace.

## Root cause

`TASK::_GET_TASK_FISHING`'s real implementation (`sub_141077798` in
`RDR2_Dumped.exe.i64`, build 1491.50) is a pure REPORTER: it resolves the
ped's fishing task component and bulk-copies the task's internal state
(via `sub_141E4F060`) into the script-visible struct `fishing_core.ysc.c`
reads every tick. It does not decide anything itself -- the task's own
per-frame FSM is updated on a SEPARATE worker thread (confirmed via a
captured call stack containing a Windows thread-pool callback frame
underneath the copy). The script thread just reads whatever's there the
moment it asks, with no synchronization between the two.

This is a classic cross-thread read/update race: at high FPS the script
thread calls `_GET_TASK_FISHING` far more often per second, increasing
the odds of reading a STALE value from before the worker thread finished
its update instead of a FRESH one -- producing the observed phase
oscillation (2 -> 3 -> 2 -> 3, never reaching 4/committed) and the
audible reset tick each time it falls back.

Confirmed two ways during investigation: (1) a genuine x86 hardware
write-breakpoint on the phase field, pure trap overhead with zero data
change, measurably let a cast succeed that had been reliably waggling
moments before -- the trap's own latency was enough to let the worker
thread land its update first; (2) directly forcing the phase field
3->4 in script memory was silently reverted by the engine every time,
proving the engine validates/owns this state rather than script being
free to just set a flag.

## The fix

`src/FishingFix.cpp` hooks `TASK::_GET_TASK_FISHING`
(`0xF3735ACD11ACD500`, scoped to `fishing_core`'s own script -- see
"Architecture" below) and inserts a precise busy-wait (`PreciseWaitMs`,
via `QueryPerformanceCounter`, tuned empirically to `kDelayMs = 3.0`)
immediately before calling through to the real native. This gives the
task's worker thread a deliberate window to finish its update before
script reads it, every time, instead of relying on incidental overhead.

The delay is gated on the script's own fishing phase rather than raw
mouse state, so it only fires during the actual pre-commit/waggling
window (phase 1-3) and costs nothing the rest of the time (idle, reeling,
already-committed cast, rod not out, etc.):

```
Global_1900073.f_26[player]  -- per-player fishing struct
  getGlobalPtr(1900073) + 27 (kF26ArrayOffset) + player * 30 (kF26Stride)
  -> phase is the first UINT64 of each element
```

`kF26ArrayOffset = 27`, not the decompiler's field index `26` -- there's
a 1-slot header before the array that only showed up via live
calibration, not in the decompiled script source. If a future build
needs re-deriving this, don't trust the decompiler's field index alone;
confirm the real offset live.

## Architecture: per-script native hooking

`src/NativeHook.h/.cpp` patches a single `rage::scrProgram`'s own
`m_NativeEntrypoints` table (via `rage::scrProgram::
GetAddressOfNativeEntrypoint`, vendored in `external/RDR-Classes/script/
scrProgram.hpp`) -- a hook registered for `"fishing_core"_J` only fires
for natives called from fishing_core's own bytecode. No global detour,
no MinHook/trampoline, no effect on any other script or this ASI's own
`invoke<>()` calls.

This is a deliberately trimmed port of `..\HorseMenu\src\game\backend\
NativeHooks.cpp`'s technique -- not a copy. HorseMenu's version sits on a
generic "call any of thousands of natives by a generated NativeIndex
enum" system (`Crossmap.hpp`/`Natives.hpp`, both 100k+ line generated
files) built for calling any native from anywhere in that large
codebase. This project only ever hooks one native by its already-known
hash, so it skips that system entirely and calls
`GetAddressOfNativeEntrypoint` directly.

The two raw engine pointers this needs (RDR2.exe's global native-hash ->
current-handler resolver, and the live array of loaded
`rage::scrProgram*`) are resolved via AOB signature, ported from
HorseMenu's own `Pointers.cpp` (`"GetNativeHandler"`/`"ScriptPrograms"`
patterns) -- see `NativeHook.cpp`'s header comment for the exact bytes
and offsets. Confirmed working live on build 1491.50 this session.

`NativeHook.cpp` also caches, per hook entry, which `scrProgram*`
instances have already been checked/patched (`allScriptsCache` /
`installedOn`) so `Update()` doesn't redo an expensive native-table scan
every tick once a hook is installed -- only a script reload/unload
invalidates the cache slot.

`getGlobalPtr(int globalId)` (declared in `..\ScriptHookSDK\inc\main.h`)
is used directly for reading the phase field -- no AOB needed for
globals, ScriptHookRDR2's SDK exports it.

## Build & deploy

```
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" FishingFix.vcxproj /p:Configuration=Debug /p:Platform=x64 /nologo /v:minimal
```

`/p:Configuration=Release` also works. The project's `PostBuildEvent`
copies the built `.asi` (+ PDB in Debug) straight into the game folder
(`E:\SteamLibrary\steamapps\common\Red Dead Redemption 2`).
**RDR2.exe must have the ASI ejected first** (this project's dev loop
used ScriptHookRDR2's eject/hot-reinject feature, not closing the game)
or the copy fails with a file-in-use error -- check
`tasklist //FI "IMAGENAME eq RDR2.exe"` if unsure, but the real
requirement is the loaded module being unmapped, not the process being
gone.

Runtime log: `<game folder>\FishingFix.log`.

## Source layout

- `src/main.cpp` -- `DllMain`, registers `ScriptMain`.
  `DLL_PROCESS_DETACH` calls `NativeHook::RemoveAll()` before
  `scriptUnregister` -- without it, fishing_core's own native table keeps
  pointing at a hook function living inside this DLL after it unmaps,
  and the next call into it jumps into unmapped memory (a real crash
  this project hit via ScriptHookRDR2's eject feature before this
  existed).
- `src/script.h/.cpp` -- `ScriptMain`'s loop: `FishingFix::OnTick()` then
  `WAIT(0)`, nothing else. No menu, no keyboard handler -- always on.
- `src/FishingFix.h/.cpp` -- the actual fix. Registers the
  `_GET_TASK_FISHING` hook once, calls `NativeHook::Update()` every
  tick. **Its `.cpp` header comment has the full root-cause trace** --
  read it before changing the delay or the phase gating.
- `src/NativeHook.h/.cpp` -- generic (not fishing-specific) per-script
  native-table patcher. See "Architecture" above.
- `src/PatternScan.h/.cpp` -- AOB pattern scanner, vendored unchanged
  from Poker/BlackjackCheat's own copy.
- `src/Log.h` -- spdlog file logger, adapted from BlackjackCheat's own
  (see that file's header comment for why it's synchronous in both
  configs -- same DLL_PROCESS_DETACH deadlock risk applies here).
- `external/RDR-Classes/` -- vendored subset (not the full copy
  Poker/BlackjackCheat carry): `script/` (scrThread, scrThreadContext,
  scrProgram, scrNativeHandler), `rage/` (atArray, joaat), `base/`
  (pgBase, needed by scrProgram.hpp). Copied from BlackjackCheat's own
  `external/RDR-Classes`, not a submodule.
- `external/spdlog/` -- vendored copy, copied wholesale from
  BlackjackCheat's own checked-out `external/spdlog`.

## If a future RDR2 build changes this

- Re-derive the `GetNativeHandler`/`ScriptPrograms` AOB patterns if
  `NativeHook.cpp`'s `EnsureResolved()` logs a pattern-not-found (fails
  safe -- no hooks installed, never a crash).
- Re-derive `kF26ArrayOffset`/`kF26Stride` live if `Global_1900073`'s
  layout shifts -- don't trust the decompiler's raw field index, confirm
  against a live memory dump the way this session did.
- `kDelayMs = 3.0` was tuned empirically on this session's hardware/build
  combination (worked reliably at 2ms and 3ms; failed at 0.5ms in
  practice, though the true minimum was never pinned down exactly). If
  it stops being reliable on different hardware, this is the first knob
  to revisit -- consider making it adaptive (e.g. scaling with observed
  frame time) rather than a fixed constant if a single value ever proves
  insufficient across machines.
