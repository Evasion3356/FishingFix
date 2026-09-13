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

	THE FIX: replaces the (heavier, riskier -- global debug registers + a
	vectored exception handler) hardware breakpoint with the same idea
	applied narrowly and safely: hook TASK::_GET_TASK_FISHING itself
	(scoped to fishing_core's own script, see NativeHook.h) and insert a
	brief precise busy-wait (PreciseWaitMs, tuned to 3ms -- see kDelayMs)
	immediately before calling through to the real native, giving the
	fishing task's worker thread a deliberate window to finish its own
	update first every time script is about to read it, instead of
	relying on incidental trap overhead to sometimes provide one.

	The delay is gated on the script's OWN fishing phase (Global_1900073.
	f_26[player], see kGlobalId/kF26ArrayOffset/kF26Stride below) rather
	than raw mouse state -- phase 1-3 is the pre-commit/waggling window
	where the race actually matters; once phase reaches 4+ the cast has
	already committed and the extra wait would be pure waste. This scopes
	the ~3ms hit to only the moments it's needed instead of paying it on
	every tick the rod is simply out.

	CURRENT TEST (see MaybeDelayForCastRaceTest, script.cpp): the delay
	has been moved OUT of OnGetTaskFishing (now a transparent passthrough)
	and INTO ScriptMain's own loop, called once per THIS ASI's script
	tick instead of being wrapped specifically around _GET_TASK_FISHING's
	call. Same kDelayMs, same IsAttemptingCast() phase gate -- the only
	variable changed is WHERE on the shared script thread the stall
	happens. If the cast still doesn't waggle with the delay here, that
	confirms the fix works by giving the worker thread wall-clock time
	anywhere on the shared script thread during the race window, not by
	sitting at any specific point relative to _GET_TASK_FISHING's own
	call. Revert by moving the `if (IsAttemptingCast()) PreciseWaitMs(...)`
	back into OnGetTaskFishing if this test says placement DOES matter.
*/

#include "FishingFix.h"
#include "NativeHook.h"
#include "Log.h"

#include "..\..\ScriptHookSDK\inc\main.h"

#include <windows.h>
#include <cstdint>

namespace FishingFix
{
	namespace
	{
		constexpr std::uint64_t kNativeGetTaskFishing = 0xF3735ACD11ACD500ull;

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

		const rage::joaat_t kFishingCoreHash = rage::Joaat("fishing_core");

		// Global_1900073.f_26[player] -- the per-player fishing struct the
		// script and the native task component both read/write. Calibrated
		// live earlier this session: the array actually starts one slot
		// after where the decompiler's field index (26) would suggest (a
		// 1-slot header the decompiler doesn't show), and phase is the
		// first UINT64 of each 30-slot-stride element.
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

		double g_lastTickMs = 0.0;
		double g_estimatedFps = 0.0;
		bool g_wasAttemptingCast = false;
		double g_windowEnterMs = 0.0;

		void UpdateFpsEstimate()
		{
			double now = NowMs();
			if (g_lastTickMs != 0.0)
			{
				double delta = now - g_lastTickMs;
				if (delta > 0.0)
					g_estimatedFps = 1000.0 / delta;
			}
			g_lastTickMs = now;
		}

		void OnGetTaskFishing(rage::scrNativeCallContext* ctx)
		{
			// TEST: delay moved out of this hook and into ScriptMain's own
			// loop -- see MaybeDelayForCastRaceTest() below and
			// script.cpp. This call is now a transparent passthrough.
			NativeHook::CallOriginal(kFishingCoreHash, kNativeGetTaskFishing, ctx);
		}

		bool g_hooksRegistered = false;
	}

	void OnTick()
	{
		if (!g_hooksRegistered)
		{
			NativeHook::AddHook(kFishingCoreHash, kNativeGetTaskFishing, OnGetTaskFishing);
			g_hooksRegistered = true;
		}

		NativeHook::Update();
	}

	void MaybeDelayForCastRaceTest()
	{
		UpdateFpsEstimate();

		bool attempting = IsAttemptingCast();
		double now = NowMs();

		if (attempting && !g_wasAttemptingCast)
		{
			g_windowEnterMs = now;
			Log::Write("FishingFix: TEST DELAY WINDOW ENTER fps~={:.1f}", g_estimatedFps);
		}
		else if (!attempting && g_wasAttemptingCast)
		{
			Log::Write("FishingFix: TEST DELAY WINDOW EXIT after {:.0f}ms fps~={:.1f}",
				now - g_windowEnterMs, g_estimatedFps);
		}
		g_wasAttemptingCast = attempting;

		// The ENTIRE choke lives behind this one check -- IsAttemptingCast()
		// is the fishing phase gate (Global_1900073 phase 1-3, see above).
		// False the rest of the time: no wait, no cost, nothing to gate
		// around anywhere else.
		if (attempting)
			PreciseWaitMs(kDelayMs);
	}
}
