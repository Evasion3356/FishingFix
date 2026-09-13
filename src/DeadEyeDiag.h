#pragma once

namespace DeadEyeDiag
{
	// Call once per script tick (see script.cpp), passing the current FPS
	// estimate (computed once in ScriptMain, shared with FishingFix).
	// Resolves the local player's Dead Eye ability object every tick and,
	// while it's active, applies the same choke FishingFix uses -- see
	// .cpp's header comment.
	void OnTick(double fps);
}
