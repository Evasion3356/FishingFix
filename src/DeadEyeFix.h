#pragma once

namespace DeadEyeFix
{
	// Call once per script tick (see script.cpp). Resolves the local
	// player's Dead Eye ability object every tick and, while it's active,
	// applies the same choke FishingFix uses -- see .cpp's header comment.
	void OnTick();
}
