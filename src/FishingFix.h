#pragma once

namespace FishingFix
{
	// Call once per script tick (see script.cpp). Applies the kDelayMs
	// busy-wait, gated on whether the player or a nearby companion/NPC is
	// in the fishing task's pre-commit window -- see FishingFix.cpp's
	// header comment for the full derivation.
	void Tick();
}
