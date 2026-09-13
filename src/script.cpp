/*
	Entry point loop. No menu -- both fixes below are passive, always on,
	nothing for a user to toggle. DeadEyeDiag::OnTick() also polls an F9
	hotkey purely to mark the log while reproducing its bug (see
	DeadEyeDiag.cpp) -- not a menu or a toggle.

	Both FishingFix::MaybeDelayForCastRaceTest() and DeadEyeDiag::OnTick()
	apply the SAME confirmed mechanism: a precise ~3ms busy-wait, called
	directly from this ASI's own script tick (not from inside any native
	hook), gated on their own respective "is the race window currently
	open" check -- fishing's own phase field for one, the Dead Eye
	ability object's +302 byte for the other. FishingFix.cpp's header
	comment has the full derivation and the live confirmation that the
	resulting FPS hit is fully and only gated by that check.
*/

#include "script.h"
#include "FishingFix.h"
#include "DeadEyeDiag.h"
#include "Log.h"

void ScriptMain()
{
	Log::Write("FishingFix started");

	while (true)
	{
		FishingFix::MaybeDelayForCastRaceTest();
		FishingFix::OnTick();
		DeadEyeDiag::OnTick();
		WAIT(0);
	}
}
