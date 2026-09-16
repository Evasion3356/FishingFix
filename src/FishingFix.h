#pragma once

namespace FishingFix
{
	// Resolve the runtime signatures once after ScriptMain starts. This
	// runs after RDR2.exe is loaded/decrypted enough for memory scanning.
	void Init();

	// Call once per script tick (see script.cpp). Applies the kDelayMs
	// busy-wait, gated on fishing_core running and any ped being in the
	// fishing task's pre-commit window -- see FishingFix.cpp's header
	// comment for the full derivation.
	void Tick();
}
