/*
	Entry point loop. No menu -- both fixes below are passive, always on,
	nothing for a user to toggle.

	Both FishingFix::Tick() and DeadEyeDiag::OnTick() apply the SAME
	confirmed mechanism: a precise ~3ms busy-wait, called directly from
	this ASI's own script tick, gated on their own respective "is the
	race window currently open" check -- fishing's own phase field for
	one, the Dead Eye ability object's +302 byte for the other.
	FishingFix.cpp's header comment has the full derivation and the live
	confirmation that the resulting FPS hit is fully and only gated by
	that check.

	The FPS estimate both checks gate on is computed ONCE here and passed
	to both -- it used to be computed independently inside each of
	FishingFix.cpp and DeadEyeDiag.cpp (two separate QueryPerformanceCounter
	deltas per tick estimating the same thing). Single source of truth now.
*/

#include "script.h"
#include "FishingFix.h"
#include "DeadEyeDiag.h"
#include "Log.h"

#include <windows.h>

namespace
{
	double g_qpcFrequency = 0.0;

	double NowMs()
	{
		if (g_qpcFrequency == 0.0)
		{
			LARGE_INTEGER freq;
			QueryPerformanceFrequency(&freq);
			g_qpcFrequency = static_cast<double>(freq.QuadPart);
		}
		LARGE_INTEGER now;
		QueryPerformanceCounter(&now);
		return (static_cast<double>(now.QuadPart) / g_qpcFrequency) * 1000.0;
	}

	double g_lastTickMs = 0.0;

	// Returns 0.0 on the very first tick (no prior sample yet) -- both
	// callers already treat "fps > kMinFpsForChoke" as false in that case,
	// same as before this was centralized.
	double UpdateFpsEstimate()
	{
		double now = NowMs();
		double fps = 0.0;
		if (g_lastTickMs != 0.0)
		{
			double delta = now - g_lastTickMs;
			if (delta > 0.0)
				fps = 1000.0 / delta;
		}
		g_lastTickMs = now;
		return fps;
	}
}

void ScriptMain()
{
	Log::Write("FishingFix started");

	while (true)
	{
		double fps = UpdateFpsEstimate();
		FishingFix::Tick(fps);
		DeadEyeDiag::OnTick(fps);
		WAIT(0);
	}
}
