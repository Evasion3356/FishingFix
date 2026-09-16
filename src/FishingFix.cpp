/*
	FIX for the "hold right click (aim), then left click" bug -- Arthur
	repeatedly pulls the rod back then releases it, over and over, only
	above ~60 FPS. Also covers companion/NPC peds fishing alongside the
	player, who show the exact same bug.

	ROOT CAUSE, as far as this investigation reaches: TASK::_GET_TASK_FISHING's
	real implementation (sub_141077798 in RDR2_Dumped.exe.i64, build
	1491.50) is a pure REPORTER -- it resolves the ped's fishing task
	component and, if it exists, bulk-copies the task's ALREADY-DECIDED
	internal state (via a call to sub_141E4F060 on the task object)
	straight into the caller-supplied out-struct. It does not decide
	anything itself.

	A hardware write-breakpoint placed directly on that struct's "phase"
	field caught this copy happening on the SCRIPT's own thread -- but
	one of the frames in that same call chain (below _GET_TASK_FISHING's
	own dispatcher) is a Windows thread-pool worker callback, confirming
	the fishing task's OWN internal per-frame update runs on a SEPARATE
	thread from the script. That worker thread is what's actually
	deciding/writing the task's live internal state (never located --
	same RTTI-less, vtable-only wall hit earlier this session); the
	script thread just reads whatever's there at the moment it asks.

	LIVE, ACCIDENTAL CONFIRMATION: installing that hardware breakpoint
	(pure overhead -- traps into the kernel and back on every write,
	changes no data) measurably let a cast succeed that had been reliably
	waggling moments before. The most likely explanation: the trap adds a
	small delay exactly at the point the script thread reads the task's
	cross-thread state, shifting the odds of catching a FRESH update from
	the worker thread instead of a STALE one from before it finished --
	a classic read/update race between two threads, worse at high FPS
	simply because there are more read attempts per second and less real
	time between them for the worker thread to land its update first.

	THE FIX: a precise ~3ms busy-wait (tuned to 3ms -- see kDelayMs),
	gated on fishing_core running and any ped having the main fishing
	task's phase in 0-4 (see kPhaseMin/kPhaseMax below), called directly
	from ScriptMain's own loop (see script.cpp) once per THIS ASI's
	script tick. This gives the fishing task's worker thread a deliberate
	window to finish its own update before script next reads it, every
	time, instead of relying on incidental overhead.

	This originally worked by hooking TASK::_GET_TASK_FISHING itself
	(scoped to fishing_core's own script via NativeHook) and inserting the
	same delay immediately before calling through to the real native. A
	side-by-side test moved the delay out of that hook and directly into
	ScriptMain's loop instead, gated on the exact same phase check but not
	sitting at any specific point relative to _GET_TASK_FISHING's own
	call -- and the cast still fixed reliably. That confirmed the fix
	works by giving the racing worker thread wall-clock time ANYWHERE on
	the shared script thread during the race window, not by intercepting
	the native call itself, so the hook (and the whole per-script native-
	hooking mechanism it needed) was removed as unnecessary.

	HOW PHASE IS READ: this mirrors TASK::_GET_TASK_FISHING's internal
	pointer walk directly instead of calling the native every tick. The
	native's real handler for build 1491.50 is sub_141077798. Peds are
	read by iterating the raw ped pool directly, so there is no per-ped
	script-handle native call. GameMemory signature-resolves the shared
	runtime pointers used below at startup. The successful fishing task
	path is:

		rawPoolIndex = *(DWORD*)(ped + 0x9C) & 0x1FFFF
		poolIndex = rawPoolIndex - dword_1439ECE40
		poolEntry = *(QWORD*)(qword_1439ECE48 + poolIndex * 0x148 + 0xB8) & ~1
		taskMgr = *(QWORD*)(poolEntry + 0x170)
		task = sub_142B2EF3C(taskMgr, 0x271)
		phase = *(DWORD*)(task + 0xF8)

	That final task+0xF8 address is exactly what sub_141E4F060 returns
	to _GET_TASK_FISHING before the native bulk-copies the task's state
	into the script out-struct. Reading only the low 32 bits is still
	important: RAGE script "Any" out-params are uniform 8-byte VM slots,
	but script bytecode only ever reads/writes the LOW 32 bits of each
	slot for plain int/float fields; the high 32 bits can contain
	unrelated leftover noise (confirmed live during the earlier native-
	call version of this fix).

	PHASE VALUES, confirmed by live testing across a full cast: 0-4 is
	the pre-commit/preparing-to-cast window (this is where the race
	happens -- waggling means bouncing within this range instead of
	reaching 6), 6 is "fishing" (rod out, waiting for a bite), 7 is
	"caught something", 12 is "reeling in". Only 0-4 gates the busy-wait;
	6/7/12 mean the cast already succeeded and the race no longer
	applies, so the extra wait would be pure waste.

	History: the original version of this fix read the phase from a
	hardcoded script GLOBAL address (getGlobalPtr(1900073) + 27) instead
	of calling the native directly, on the theory that the global was a
	per-player array (Global_1900073.f_26[player], stride 30, phase as
	the first UINT64 of each element -- see this project's CLAUDE.md/git
	history for that derivation). A later regression (legendary-fish
	casts waggling again, gated busy-wait no longer reliably firing) led
	to two rounds of live diagnostics (temporary CastMemDiag/
	TaskFishingDiag tooling, since removed): the global offset itself
	turned out to still be landing on the exact right slot -- but the
	old code compared the FULL 64-bit slot value against 1-3, and that
	slot's high 32 bits are the unrelated noise described above, so
	whenever that noise happened to be non-zero, the combined 64-bit
	comparison would spuriously fail even though the real (low-32) phase
	was legitimately in range. The follow-up native-call version fixed
	that and proved this generalized to any ped, which surfaced the
	companion-fishing case. The current version keeps that any-ped
	coverage but removes the expensive native call by scanning the raw
	ped pool and reading the 4-byte phase field directly from the task
	state object.
*/

#include "FishingFix.h"
#include "GameMemory.h"
#include "Log.h"

#include <cstdint>

namespace FishingFix
{
	namespace
	{
		// Same value confirmed live for FishingFix and reused by DeadEyeFix.
		constexpr double kDelayMs = 3.0;

		constexpr int kPhaseMin = 0;
		constexpr int kPhaseMax = 4;
		constexpr std::uint32_t kFishingCoreScriptHash = GameMemory::Joaat("fishing_core");

		constexpr std::uintptr_t kFishingTaskTreeOffset = 0xB8;
		constexpr std::uintptr_t kTaskManagerOffset = 0x170;
		constexpr int kFishingTaskId = 0x271;
		constexpr std::uintptr_t kFishingTaskPhaseOffset = 0xF8;
		constexpr float kMaxDistanceToCheckNpcs = 50.0f;

		bool TryReadFishingPhaseFromPed(std::uint64_t ped, int& outPhase)
		{
			std::uint64_t poolEntry = GameMemory::ResolvePedLinkedPoolEntry(ped, kFishingTaskTreeOffset);
			if (!poolEntry)
				return false;

			std::uint64_t taskManager = *reinterpret_cast<std::uint64_t*>(poolEntry + kTaskManagerOffset);
			if (!GameMemory::LooksLikeValidPointer(taskManager))
				return false;

			// This validates the task slot cheaply without copying the
			// native's full script-facing fishing state buffer.
			std::uint64_t task = GameMemory::FindTaskById(taskManager, kFishingTaskId);
			if (!task)
				return false;

			outPhase = *reinterpret_cast<int*>(task + kFishingTaskPhaseOffset);
			return true;
		}

		bool IsPhaseInPreCommitWindow(int phase)
		{
			return phase >= kPhaseMin && phase <= kPhaseMax;
		}

		// True if fishing_core is active and any ped currently has the
		// main fishing task in the pre-commit/waggling window.
		bool IsAnyoneAttemptingCast()
		{
			if (!GameMemory::IsScriptRunning(kFishingCoreScriptHash))
				return false;

			// The local player is the overwhelmingly common case. Check it
			// before touching the pool so normal casts short-circuit without
			// decrypting or validating the raw ped pool.
			std::uint64_t localPlayerPed = GameMemory::ResolvePlayerPed(0);
			if (localPlayerPed)
			{
				int phase;
				if (TryReadFishingPhaseFromPed(localPlayerPed, phase)
					&& IsPhaseInPreCommitWindow(phase))
					return true;
			}

			GameMemory::FwBasePool* pedPool = GameMemory::GetPedPool();
			if (!GameMemory::LooksLikeValidPointer(reinterpret_cast<std::uint64_t>(pedPool)))
				return false;

			for (std::uint32_t i = 0; i < pedPool->size; ++i)
			{
				std::uint64_t pedPtr = GameMemory::GetPoolEntry(pedPool, i);
				if (!pedPtr || pedPtr == localPlayerPed)
					continue;

				if (localPlayerPed
					&& !GameMemory::IsPedWithinDistance(
						pedPtr, localPlayerPed, kMaxDistanceToCheckNpcs))
					continue;

				int phase;
				if (!TryReadFishingPhaseFromPed(pedPtr, phase) || !IsPhaseInPreCommitWindow(phase))
					continue;

				return true;
			}

			return false;
		}

		bool g_wasAttempting = false;
		double g_windowEnterMs = 0.0;
	}

	void Init()
	{
		GameMemory::Init();
	}

	void Tick()
	{
		bool attempting = IsAnyoneAttemptingCast();

		if (attempting && !g_wasAttempting)
		{
			g_windowEnterMs = GameMemory::NowMs();
			Log::Write("choking");
		}
		else if (!attempting && g_wasAttempting)
		{
			Log::Write("choke released after {:.0f}ms",
				GameMemory::NowMs() - g_windowEnterMs);
		}
		g_wasAttempting = attempting;

		if (attempting)
			GameMemory::PreciseWaitMs(kDelayMs);
	}
}
