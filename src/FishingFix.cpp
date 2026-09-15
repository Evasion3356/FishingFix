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

	THE FIX: a precise ~3ms busy-wait (PreciseWaitMs, tuned to 3ms -- see
	kDelayMs), gated on whether ANY relevant ped -- the local player, or
	a nearby ped also running the fishing task (a companion fishing
	alongside the player) -- currently has phase 0-4 (see kPhaseMin/
	kPhaseMax below), called directly from ScriptMain's own loop (see
	script.cpp) once per THIS ASI's script tick. This gives the fishing
	task's worker thread a deliberate window to finish its own update
	before script next reads it, every time, instead of relying on
	incidental overhead.

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

	HOW PHASE IS READ (rewritten from the original global-memory read --
	see "History" below for why): this calls TASK::_GET_TASK_FISHING
	(hash 0xF3735ACD11ACD500, declared in the ScriptHookRDR2 SDK's own
	natives.h as AI::_0xF3735ACD11ACD500(Any ped, Any* outStruct) -> BOOL
	-- "AI" is just that SDK's own category label for the hash, unrelated
	to the decompiler's "TASK::" alias for the same native) DIRECTLY,
	with our own local buffer, for the player AND for every nearby ped
	within kMaxDistance. This is a plain native CALL, not a hook/patch --
	consistent with this project no longer touching any native's table.
	Confirmed against the 1491.50 decompile (fishing_core.c line 3418:
	`Global_1902822.f_35 = TASK::_GET_TASK_FISHING(PLAYER::PLAYER_PED_ID(),
	&(Global_1902822.f_5));`) and confirmed safe to call on any ped,
	fishing or not, every tick -- it's a pure reporter, no side effects.

	The out-struct's field 0 is the phase int. RAGE script "Any" out-
	params are uniform 8-byte VM slots, but script bytecode only ever
	reads/writes the LOW 32 bits of each slot -- the high 32 bits are
	never touched by the VM at all, so whatever bit pattern sits there is
	leftover from some earlier, unrelated use of that memory. Confirmed
	live: field 0's high half was seen independently oscillating on a
	~10ms period totally unrelated to the real phase transitions in that
	same field's low half. Only ever read the low 32 bits.

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
	was legitimately in range. Switching to a direct native call (a)
	sidesteps the whole global-offset question, since there's no address
	to guess or drift, and (b) trivially generalizes to any ped, which is
	what surfaced the companion-fishing case this file now also covers.
*/

#include "FishingFix.h"
#include "Log.h"

#include "..\..\ScriptHookSDK\inc\natives.h"

#include <windows.h>
#include <cstring>
#include <cmath>

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

		// See this file's header comment: the true out-struct size was
		// never pinned down statically (the decompile only shows the
		// handful of sub-fields the SCRIPT happens to read back, not the
		// struct's real extent) -- 2048 slots (16KB) is a generous upper
		// bound confirmed safe live, well past every field index seen in
		// fishing_core.c (whose caller passes this struct as
		// uLocal_14.f_3089 out of a frame whose highest local is
		// uLocal_4608).
		constexpr size_t kBufSlots = 2048;

		constexpr int kPhaseMin = 0;
		constexpr int kPhaseMax = 4;

		// How far from the player to look for a companion/NPC also
		// running the fishing task. Fishing partners stand right next to
		// the player in practice; this is generous headroom past that.
		constexpr float kMaxDistanceToCheckNpcs = 50.0f;
		constexpr int kMaxPedCandidates = 1024;

		// AI::_0xF3735ACD11ACD500 == TASK::_GET_TASK_FISHING. See this
		// file's header comment for the hash/namespace note.
		bool GetFishingPhase(Ped ped, int& outPhase)
		{
			UINT64 buf[kBufSlots];
			std::memset(buf, 0, sizeof(buf));

			BOOL hasTask = AI::_0xF3735ACD11ACD500(static_cast<Any>(ped), reinterpret_cast<Any*>(buf));
			if (!hasTask)
				return false;

			UINT32 lo = static_cast<UINT32>(buf[0] & 0xFFFFFFFFu);
			std::memcpy(&outPhase, &lo, sizeof(outPhase));
			return true;
		}

		bool IsPhaseInPreCommitWindow(int phase)
		{
			return phase >= kPhaseMin && phase <= kPhaseMax;
		}

		float DistanceBetween(const Vector3& a, const Vector3& b)
		{
			float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
			return std::sqrt(dx * dx + dy * dy + dz * dz);
		}

		// True if the local player, or any nearby ped also running the
		// fishing task (a companion fishing alongside the player), is
		// currently in the pre-commit/waggling window.
		bool IsAnyoneAttemptingCast()
		{
			Ped playerPed = PLAYER::PLAYER_PED_ID();

			int phase;
			if (GetFishingPhase(playerPed, phase) && IsPhaseInPreCommitWindow(phase))
				return true;

			Vector3 playerCoords = ENTITY::GET_ENTITY_COORDS(playerPed, true, false);

			static int pedArr[kMaxPedCandidates];
			int count = worldGetAllPeds(pedArr, kMaxPedCandidates);
			for (int i = 0; i < count; ++i)
			{
				Ped ped = static_cast<Ped>(pedArr[i]);
				if (ped == playerPed)
					continue;
				if (!ENTITY::DOES_ENTITY_EXIST(ped) || ENTITY::IS_ENTITY_DEAD(ped))
					continue;

				Vector3 coords = ENTITY::GET_ENTITY_COORDS(ped, true, false);
				if (DistanceBetween(coords, playerCoords) > kMaxDistanceToCheckNpcs)
					continue;

				if (GetFishingPhase(ped, phase) && IsPhaseInPreCommitWindow(phase))
					return true;
			}

			return false;
		}

		bool g_wasAttempting = false;
		double g_windowEnterMs = 0.0;

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
	}

	void Tick()
	{
		bool attempting = IsAnyoneAttemptingCast();
		double now = NowMs();

		if (attempting && !g_wasAttempting)
		{
			g_windowEnterMs = now;
			Log::Write("FishingFix: DELAY WINDOW ENTER");
		}
		else if (!attempting && g_wasAttempting)
		{
			Log::Write("FishingFix: DELAY WINDOW EXIT after {:.0f}ms",
				now - g_windowEnterMs);
		}
		g_wasAttempting = attempting;

		if (attempting)
			PreciseWaitMs(kDelayMs);
	}
}
