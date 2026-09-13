# FishingFix

A ScriptHookRDR2 ASI that fixes a real bug: holding right click (aim)
then left click to cast the fishing rod fails intermittently above ~60
FPS -- Arthur pulls the rod back for ~0.5s then releases it, over and
over, never completing the cast, only when uncapped/high FPS. Sibling of
`../PokerCheat`/`../BlackjackCheat` (same ScriptHookRDR2 + native C++
toolchain).

**Status: fixed and confirmed live.** See `src/FishingFix.cpp`'s header
comment for the full root-cause trace. Also carries `DeadEyeDiag`, a
second instance of the same confirmed mechanism applied to Dead Eye's
active window (see `src/DeadEyeDiag.cpp`).

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

`src/FishingFix.cpp`'s `Tick()` (called once per script tick from
`ScriptMain`, see `script.cpp`) inserts a precise busy-wait
(`PreciseWaitMs`, via `QueryPerformanceCounter`, tuned empirically to
`kDelayMs = 3.0`) whenever the script's own fishing phase is 1-3. This
gives the task's worker thread a deliberate window to finish its update
before script next reads it, every time, instead of relying on
incidental overhead.

This originally worked by hooking `TASK::_GET_TASK_FISHING`
(`0xF3735ACD11ACD500`) directly and inserting the delay immediately
before calling through to the real native, scoped to `fishing_core`'s
own script via a per-script native-table patch. A side-by-side test
moved the delay out of that hook and directly into `ScriptMain`'s loop
instead (same phase gate, no longer tied to the native call at all) and
the cast still fixed reliably -- proving the fix works by giving the
racing worker thread wall-clock time anywhere on the shared script
thread during the race window, not by intercepting the native call
itself. The hook (and the per-script native-hooking machinery it
needed, including the vendored `external/RDR-Classes` tree) was removed
as a result -- this project no longer patches any native's table.

The delay is gated on the script's own fishing phase rather than raw
mouse state, so it only fires during the actual pre-commit/waggling
window (phase 1-3) and costs nothing the rest of the time (idle, reeling,
already-committed cast, rod not out, etc.):

```
Global_1900073.f_26[player]  -- per-player fishing struct
  getGlobalPtr(1900073) + 27 (kF26ArrayOffset) + player * 30 (kF26Stride)
  -> phase is the first UINT64 of each element
```

`kF26ArrayOffset = 27`, not the decompiler's field index `26` -- confirmed
against decompiled script source itself: an array store on this same
field appears as `Global_1900073.f_26[i /*30*/] = 1;` (the `/*30*/` is
the decompiler's own per-element stride annotation, matching
`kF26Stride` exactly). The `+1` beyond the field's declared index is the
well-known RAGE script VM convention of an array's slot 0 holding its
own length/count, with real elements starting one slot after -- so
`kF26ArrayOffset` is just `26 (decompiler's field index) + 1 (that
header slot)`, derivable statically from any array-store on this field
rather than only via live memory calibration.

`getGlobalPtr(int globalId)` (declared in `..\ScriptHookSDK\inc\main.h`)
is used directly for reading the phase field -- no AOB needed for
globals, ScriptHookRDR2's SDK exports it. `DeadEyeDiag.cpp` instead
resolves its own ability-object pointer via a small chain of raw reads
off a statically-known (IDA-derived, ASLR-rebased) function and fields
-- see that file's header comment.

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
- `src/script.h/.cpp` -- `ScriptMain`'s loop: `FishingFix::Tick()`, then
  `DeadEyeDiag::OnTick()`, then `WAIT(0)`, nothing else. No menu, no
  keyboard handler -- always on.
- `src/FishingFix.h/.cpp` -- the actual fix. **Its `.cpp` header comment
  has the full root-cause trace** -- read it before changing the delay
  or the phase gating.
- `src/DeadEyeDiag.h/.cpp` -- the same confirmed mechanism applied to
  Dead Eye's active window. See that file's header comment for the
  ability-pointer derivation.
- `src/Log.h` -- spdlog file logger, adapted from BlackjackCheat's own
  (see that file's header comment for why it's synchronous in both
  configs -- same DLL_PROCESS_DETACH deadlock risk applies here).
- `external/spdlog/` -- vendored copy, copied wholesale from
  BlackjackCheat's own checked-out `external/spdlog`.

This project does not hook or patch any native -- it used to (a
per-script `rage::scrProgram` native-table patch, ported from
HorseMenu's `NativeHooks.cpp` technique, living in `src/NativeHook.h/
.cpp` + `src/PatternScan.h/.cpp` + a vendored `external/RDR-Classes`
subset), but that was removed once the delay was confirmed to work
identically from `ScriptMain`'s own loop -- see "The fix" above. If
resurrecting per-native hooking is ever needed again, `../PokerCheat`/
`../BlackjackCheat` and `../HorseMenu` still carry the pattern.

## If a future RDR2 build changes this

- Re-derive `kF26ArrayOffset`/`kF26Stride` if `Global_1900073`'s layout
  shifts -- this no longer requires live calibration: find any
  `Global_1900073.f_NN[i /*stride*/] = ...` array store in the new
  build's decompiled scripts and take `kF26ArrayOffset = NN + 1` (the
  `+1` is the RAGE VM's array-length header slot, not something the
  decompiler shows in the field index) and `kF26Stride` straight from
  the `/*stride*/` annotation. Only fall back to a live memory dump if
  the field can't be found in decompiled source at all.
- `kDelayMs = 3.0` was tuned empirically on this session's hardware/build
  combination (worked reliably at 2ms and 3ms; failed at 0.5ms in
  practice, though the true minimum was never pinned down exactly). If
  it stops being reliable on different hardware, this is the first knob
  to revisit -- consider making it adaptive (e.g. scaling with observed
  frame time) rather than a fixed constant if a single value ever proves
  insufficient across machines.
