#pragma once

namespace FishingFix
{
	// Call once per script tick (see script.cpp). Applies the kDelayMs
	// busy-wait, gated on the fishing phase check, directly from
	// ScriptMain's own loop -- see FishingFix.cpp's header comment for why
	// this replaced the earlier _GET_TASK_FISHING native hook.
	void Tick();
}
