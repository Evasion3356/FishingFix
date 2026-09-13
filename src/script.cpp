/*
	Entry point loop. No menu, no keyboard handler -- this is a passive
	diagnostic (see FishingFix.cpp's header comment), always on, nothing
	for a user to toggle.
*/

#include "script.h"
#include "FishingFix.h"
#include "Log.h"

void ScriptMain()
{
	Log::Write("FishingFix started");

	while (true)
	{
		FishingFix::OnTick();
		WAIT(0);
	}
}
