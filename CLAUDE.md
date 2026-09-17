# FishingFix

A ScriptHookRDR2 ASI that fixes a real bug: holding right click (aim)
then left click to cast the fishing rod fails intermittently above ~60
FPS -- Arthur pulls the rod back for ~0.5s then releases it, over and
over, never completing the cast, only when uncapped/high FPS. Also
covers companion/NPC peds fishing alongside the player, who show the
exact same bug. Sibling of `../PokerCheat`/`../BlackjackCheat` (same
ScriptHookRDR2 + native C++ toolchain).

**Status: fixed and confirmed live**, for both the player and nearby
NPCs. See `src/FishingFix.cpp`'s header comment for the full root-cause
trace, including a later regression (legendary-fish casts waggling
again) and the live-diagnostic investigation that found and fixed it.
Also carries `DeadEyeFix`, a second instance of the same confirmed
mechanism applied to Dead Eye's active window (see
`src/DeadEyeFix.cpp`).

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
`kDelayMs = 4.0`) whenever the local player, or any nearby ped (within
50m) also running the fishing task -- a companion fishing alongside the
player -- is in the task's phase 0-4 pre-commit window. This gives the
task's worker thread a deliberate window to finish its update before
script next reads it, every time, instead of relying on incidental
overhead.

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

**How the phase is read** (rewritten from both the old script-global
reader and the later native-call reader -- see "History" below): this
now mirrors `TASK::_GET_TASK_FISHING`'s true-return pointer walk directly
instead of calling the native every tick. `GameMemory.cpp` resolves the
shared script-thread, ped-pool, linked-task-pool, and task-lookup
pointers once at startup via HorseMenu-style signatures. `FishingFix.cpp`
then checks that `fishing_core` is running, scans the raw ped pool
(including the local player and companion/NPC peds), resolves each ped's
main fishing task (`0x271`), and reads only the 4-byte phase at
`task + 0xF8`.

That task field is exactly what sub_141E4F060 returns to
`_GET_TASK_FISHING` before the native bulk-copies the task's state into
the script out-struct. Confirmed by live testing across a full cast:
**0-4** is the pre-commit/preparing-to-cast window (this is where the
race happens -- waggling means bouncing within this range instead of
reaching 6), **6** is "fishing" (rod out, waiting for a bite), **7** is
"caught something", **12** is "reeling in". Only 0-4 gates the busy-wait.

RAGE script "Any" out-params are uniform 8-byte VM slots, but script
bytecode only ever reads/writes the LOW 32 bits of each slot for a plain
int/float field -- the high 32 bits are never touched by the VM at all,
so whatever's there is leftover from some earlier, unrelated use of that
memory (not zeroed on reuse). Confirmed live: field 0's high half was
seen independently oscillating on a ~10ms period totally unrelated to
the real phase transitions in that same field's low half. Only ever
read the low 32 bits of a field -- ignoring this is exactly what broke
the previous version of this fix (see "History").

`DeadEyeFix.cpp` resolves its own ability-object pointer via
`GameMemory`'s shared signature-resolved player/pool helpers; it doesn't
call this native or read any script global.

### History: the global-memory version of this fix, and why it broke

The original version of this fix read the phase from a hardcoded script
GLOBAL address (`getGlobalPtr(1900073) + 27`) instead of calling the
native directly, on the theory that the global was a per-player array
(`Global_1900073.f_26[player]`, stride 30, phase as the first UINT64 of
each element):

```
Global_1900073.f_26[player]  -- per-player fishing struct
  getGlobalPtr(1900073) + 27 (kF26ArrayOffset) + player * 30 (kF26Stride)
  -> phase is the first UINT64 of each element
```

`kF26ArrayOffset = 27`, not the decompiler's field index `26`, per the
well-known RAGE script VM convention of an array's slot 0 holding its
own length/count (`kF26ArrayOffset` = `26 (decompiler's field index) + 1
(that header slot)`).

A later regression (legendary-fish casts waggling again, the busy-wait
no longer reliably firing) led to two rounds of live diagnostics
(temporary `CastMemDiag`/`TaskFishingDiag` tooling, since removed once
their job was done). Findings:

- The global offset itself was still landing on the exact right memory
  -- confirmed by calling the native fresh and comparing its output
  bit-for-bit, at the same tick, against the old global read. Offset
  drift was NOT the problem.
- The old code compared the FULL 64-bit slot value against the range
  1-3. That slot's high 32 bits are the unrelated noise described above
  -- so whenever that noise happened to be non-zero, the combined
  64-bit comparison would spuriously fail even though the real (low-32)
  phase was legitimately in range. This -- not legendary fish having any
  special-cased logic of its own (confirmed absent from `fishing_core.c`)
  -- is the most likely explanation for the regression.
- The phase range needed for the pre-commit window is actually 0-4, not
  1-3 -- refined during this same investigation via live testing across
  full casts.
- `fishing_core.c` (the decompiled script, confirmed against build
  1491.50) never calls `_GET_TASK_FISHING`/`_SET_TASK_FISHING` on any
  ped other than the local player and network-player slots -- there is
  no companion/NPC path in that script at all. Whatever assigns a
  companion ped its fishing task, it isn't `fishing_core.c`; the native
  call approach doesn't need to know, since it can read ANY ped's task
  state regardless of what assigned it.

## Build & deploy

```
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" FishingFix.vcxproj /p:Configuration=Debug /p:Platform=x64 /nologo /v:minimal
```

`/p:Configuration=Release` also works. The project's `PostBuildEvent`
auto-locates the RDR2 install directory
(`BuildTools\Find-RDR2GameDir.ps1` -- vendored identically into every
sibling project, since each is its own separate git repo) and copies the
built `.asi` (+ PDB in Debug) straight into it, on every build regardless
of whether the build itself was up to date.
`DisableFastUpToDateCheck` is set in the `.vcxproj.user` so this also
holds for Visual Studio IDE builds, not just command-line MSBuild.
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
  `DeadEyeFix::OnTick()`, then `WAIT(0)`, nothing else. No menu, no
  keyboard handler -- always on.
- `src/FishingFix.h/.cpp` -- the actual fix. **Its `.cpp` header comment
  has the full root-cause trace** -- read it before changing the delay
  or the phase gating.
- `src/GameMemory.h/.cpp` -- shared one-time signature resolver and raw
  memory helpers used by both FishingFix and DeadEyeFix: script-thread
  checks, ped pool iteration, linked ped pool entry resolution,
  `sub_142B2EF3C` task lookup, local-player ped resolution,
  module-relative signature logging, and QPC busy-wait timing.
- `src/DeadEyeFix.h/.cpp` -- the same confirmed mechanism applied to
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

- The current fix reads raw task memory through signatures instead of
  calling `AI::_0xF3735ACD11ACD500` (`TASK::_GET_TASK_FISHING`) every
  tick. If a content patch ships, first check whether `GameMemory.cpp`'s
  signatures still resolve; then re-confirm phase values (currently 0-4
  pre-commit, 6 fishing, 7 caught, 12 reeling) by logging the low 32 bits
  at `task + 0xF8` across a full cast, the same technique the removed
  `CastMemDiag`/`TaskFishingDiag` diagnostics used.
- Only ever read the LOW 32 bits of any field in the out-struct. The
  high 32 bits are not part of the VM's data model for a plain int/float
  field and carry unrelated leftover noise -- see "History" above for
  what happens if this gets missed again.
- `kDelayMs = 4.0` was tuned empirically on this session's hardware/build
  combination after 3ms still showed occasional failures (2ms and 3ms
  had worked in earlier testing; 0.5ms failed in practice). If
  it stops being reliable on different hardware, this is the first knob
  to revisit -- consider making it adaptive (e.g. scaling with observed
  frame time) rather than a fixed constant if a single value ever proves
  insufficient across machines.
- `kMaxDistanceToCheckNpcs = 50.0f` (in `FishingFix.cpp`) is how far from
  the player to look for a companion also running the fishing task. If a
  fishing partner ever stands further away than that and doesn't get the
  choke, widen it.
