/*
	Entry point loop. No menu -- both fixes below are passive, always on,
	nothing for a user to toggle.

	Both FishingFix::Tick() and DeadEyeFix::OnTick() apply the SAME
	confirmed mechanism: a precise ~3ms busy-wait, called directly from
	this ASI's own script tick, gated on their own respective "is the
	race window currently open" check -- fishing's own phase field for
	one, the Dead Eye ability object's +302 byte for the other.
	FishingFix.cpp's header comment has the full derivation.

	This used to also gate on an estimated FPS floor (skip the choke
	below ~120fps, on the theory the race doesn't occur at lower tick
	rates). Dropped after live testing showed the estimate itself was
	unreliable enough to sometimes skip the choke exactly when the race
	it guards against was happening -- i.e. it could cause the very bug
	this ASI fixes. Both ticks now run unconditionally; each one's own
	phase/active check is still what actually gates the busy-wait.
*/

#include "script.h"
#include "FishingFix.h"
#include "DeadEyeFix.h"
#include "Log.h"

void ScriptMain()
{
	Log::Write("FishingFix started");
	FishingFix::Init();

	while (true)
	{
		FishingFix::Tick();
		DeadEyeFix::OnTick();
		WAIT(0);
	}
}
