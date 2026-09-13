/*
	FIX for the "hold right click (aim), then left click" bug -- Arthur
	repeatedly pulls the rod back then releases it, over and over, only
	above ~60 FPS.

	ROOT CAUSE, as far as this session's live IDA + hardware-breakpoint
	investigation reaches: TASK::_GET_TASK_FISHING's real implementation
	(sub_141077798 in RDR2_Dumped.exe.i64) is a pure REPORTER -- it
	resolves the ped's fishing task component and, if it exists, bulk-
	copies the task's ALREADY-DECIDED internal state (via a call to
	sub_141E4F060 on the task object) straight into the script-visible
	struct fishing_core.ysc.c reads back every tick. It does not decide
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

	THE FIX: a precise ~3ms busy-wait (PreciseWaitMs, tuned to 3ms -- see
	kDelayMs), gated on the script's OWN fishing phase (Global_1900073.
	f_26[player], see kGlobalId/kF26ArrayOffset/kF26Stride below) and
	called directly from ScriptMain's own loop (see script.cpp) once per
	THIS ASI's script tick, giving the fishing task's worker thread a
	deliberate window to finish its own update before script reads it.

	This used to be implemented by hooking TASK::_GET_TASK_FISHING itself
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

	Phase 1-3 is the pre-commit/waggling window where the race actually
	matters; once phase reaches 4+ the cast has already committed and the
	extra wait would be pure waste. This scopes the ~3ms hit to only the
	moments it's needed instead of paying it on every tick the rod is
	simply out.
*/

#include "FishingFix.h"
#include "Log.h"

#include "..\..\ScriptHookSDK\inc\main.h"

#include <windows.h>
#include <cstdint>

namespace FishingFix
{
	namespace
	{
		// A 500ms Sleep() confirmed the theory (dropped the game to ~6
		// FPS, but the cast succeeded reliably at that rate -- consistent
		// with the worker thread always having enough real time to finish
		// its update before script reads it). Now narrowing down: 0.5ms
		// is far too short for Sleep()'s own scheduler granularity
		// (typically 1-15ms -- it would either round up to a full
		// millisecond-plus or, on some systems, effectively do nothing),
		// so this is a real busy-wait against QueryPerformanceCounter
		// instead, precise well below 1ms.
		constexpr double kDelayMs = 3.0;

		// The race this delay covers only manifests at high tick rates --
		// at lower FPS the naturally longer per-tick gap already gives the
		// worker thread enough wall-clock time to land its update first
		// (this is exactly what the 500ms Sleep() test above confirmed:
		// reliable at ~6fps with zero fix applied). Below this threshold
		// the choke is pure unconditional cost for no benefit, so it's
		// gated off entirely rather than paying it on slower hardware.
		constexpr double kMinFpsForChoke = 120.0;

		double g_qpcFrequency = 0.0;

		void PreciseWaitMs(double ms)
		{
			if (g_qpcFrequency == 0.0)
			{
				LARGE_INTEGER freq;
				QueryPerformanceFrequency(&freq);
				g_qpcFrequency = static_cast<double>(freq.QuadPart);
			}

			LARGE_INTEGER start;
			QueryPerformanceCounter(&start);
			double targetTicks = (ms / 1000.0) * g_qpcFrequency;

			LARGE_INTEGER now;
			do
			{
				QueryPerformanceCounter(&now);
			} while (static_cast<double>(now.QuadPart - start.QuadPart) < targetTicks);
		}

		// Global_1900073.f_26[player] -- the per-player fishing struct the
		// script and the native task component both read/write. Confirmed
		// against decompiled script source: an array store on this field
		// appears as `Global_1900073.f_26[i /*30*/] = 1;` -- the array
		// actually starts one slot after the decompiler's field index (26)
		// because RAGE script arrays reserve slot 0 for their own
		// length/count (a header the decompiler's field index doesn't
		// account for), and phase is the first UINT64 of each 30-slot-
		// stride element (the `/*30*/` annotation matches kF26Stride).
		constexpr int kGlobalId = 1900073;
		constexpr int kF26ArrayOffset = 27;
		constexpr int kF26Stride = 30;
		constexpr int kPlayerIndex = 0;

		// 1-3 is the pre-commit window (still aiming/waggling) where the
		// cross-thread race actually matters; 4+ means the cast already
		// committed/is airborne and reads are no longer racy against it.
		constexpr UINT64 kPhasePreCommitMin = 1;
		constexpr UINT64 kPhasePreCommitMax = 3;

		UINT64* GetPhaseSlot()
		{
			UINT64* base = getGlobalPtr(kGlobalId);
			if (!base)
				return nullptr;
			return base + kF26ArrayOffset + kPlayerIndex * kF26Stride;
		}

		bool IsAttemptingCast()
		{
			UINT64* phase = GetPhaseSlot();
			if (!phase)
				return false;
			UINT64 value = *phase;
			return value >= kPhasePreCommitMin && value <= kPhasePreCommitMax;
		}

		// Instrumentation only -- confirmed live that the FPS hit during
		// the test tracks arithmetically with kDelayMs (baseline ~190-
		// 205fps/~5.2ms-per-frame; +3ms/frame -> ~122fps, which is
		// exactly the ~110-139fps band observed) and that it holds for
		// EXACTLY as long as IsAttemptingCast() is true -- one ENTER, one
		// EXIT, nothing outside that bracket. The choke is fully and
		// only gated by the fishing phase check; there is no other path
		// to PreciseWaitMs in this file.
		double NowMs()
		{
			if (g_qpcFrequency == 0.0)
			{
				LARGE_INTEGER freq;
				QueryPerformanceFrequency(&freq);
				g_qpcFrequency = static_cast<double>(freq.QuadPart);
			}
			LARGE_INTEGER now;
			QueryPerformanceCounter(&now);
			return (static_cast<double>(now.QuadPart) / g_qpcFrequency) * 1000.0;
		}

		bool g_wasAttemptingCast = false;
		double g_windowEnterMs = 0.0;
	}

	void Tick(double fps)
	{
		bool attempting = IsAttemptingCast();
		double now = NowMs();

		if (attempting && !g_wasAttemptingCast)
		{
			g_windowEnterMs = now;
			Log::Write("FishingFix: DELAY WINDOW ENTER fps~={:.1f}", fps);
		}
		else if (!attempting && g_wasAttemptingCast)
		{
			Log::Write("FishingFix: DELAY WINDOW EXIT after {:.0f}ms fps~={:.1f}",
				now - g_windowEnterMs, fps);
		}
		g_wasAttemptingCast = attempting;

		// The choke lives behind these two checks -- IsAttemptingCast() is
		// the fishing phase gate (Global_1900073 phase 1-3, see above), and
		// fps > kMinFpsForChoke skips it entirely on hardware where the
		// underlying race doesn't occur in the first place.
		if (attempting && fps > kMinFpsForChoke)
			PreciseWaitMs(kDelayMs);
	}
}
