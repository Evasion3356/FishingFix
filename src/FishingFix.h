#pragma once

namespace FishingFix
{
	// Call once per script tick (see script.cpp). Lazily installs the
	// native hook (see .cpp) once fishing_core is loaded, and does
	// nothing else.
	void OnTick();
}
