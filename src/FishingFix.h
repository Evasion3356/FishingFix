#pragma once

namespace FishingFix
{
	// Call once per script tick (see script.cpp). Lazily installs the
	// native hook (see .cpp) once fishing_core is loaded, and does
	// nothing else.
	void OnTick();

	// TEST ONLY -- see script.cpp and FishingFix.cpp's header comment.
	// Applies the exact same kDelayMs busy-wait, gated by the exact same
	// fishing-phase check the real fix uses, but called directly from
	// ScriptMain's own loop instead of from inside the _GET_TASK_FISHING
	// hook. If this ALSO fixes the cast, that proves the fix works by
	// stalling the shared script thread during the race window -- giving
	// the fishing task's worker thread more wall-clock time to land its
	// update -- rather than needing to sit at any specific point relative
	// to _GET_TASK_FISHING's own call.
	void MaybeDelayForCastRaceTest();
}
