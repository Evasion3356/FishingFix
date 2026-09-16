/*
	FIX for Dead Eye, wired up the same way FishingFix.cpp's own
	confirmed mechanism is: a precise busy-wait applied from this ASI's
	own script tick (see script.cpp), gated on whether Dead Eye is
	currently active -- not from any native hook. FishingFix's own
	version of this exact test (delay moved out of the _GET_TASK_FISHING
	hook and into ScriptMain, gated on the script's own fishing phase)
	confirmed live that the fix works by giving the racing worker thread
	wall-clock time ANYWHERE on the shared script thread during the race
	window, and that the resulting FPS hit is fully and only gated by
	that phase check (one ENTER, one EXIT, nothing outside the bracket).
	This applies that same confirmed approach directly to Dead Eye.

	"Dead Eye active" is *ability+302 != 0* -- confirmed live earlier
	this session: probing ability+301 and ability+302 while repeatedly
	entering/exiting Dead Eye showed +301 constant at 1 the whole time (a
	static "ability equipped" flag, not an activity signal), while +302
	toggled exactly in sync with actually engaging Dead Eye and was
	sitting at 1 continuously through a real reproduction of the
	"clicking on people makes the gun animate weirdly" bug.

	The ability object's address comes from the exact pointer chain the
	two natives the user labeled both use -- PLAYER::_GET_PLAYER_DEAD_EYE
	(0xA81D24AE0AF99A5E) and PLAYER::_ACTIVATE_DEAD_EYE
	(0xBBA140062B15A8AC), decompiled in IDA (RDR2_Dumped.exe.i64,
	1491.50). GameMemory signature-resolves the shared function/global
	pointers at startup:

		ped       = sub_140EE4DB0(playerIndex)
		poolIndex = *(DWORD*)(ped + 156) & 0x1FFFF
		poolEntry = *(QWORD*)(328 * (poolIndex - dword_1439ECE40)
		                      + qword_1439ECE48 + 240) & ~1ULL
		ability   = *(QWORD*)(poolEntry + 0x93B0)
*/

#include "DeadEyeFix.h"
#include "GameMemory.h"
#include "Log.h"

#include <cstdint>

namespace DeadEyeFix
{
	namespace
	{
		// Same value FishingFix confirmed live -- see its own header
		// comment for how it was tuned.
		constexpr double kDelayMs = 3.0;

		constexpr std::uintptr_t kAbilityPointerOffset = 0x93B0; // 37808 decimal
		constexpr std::uintptr_t kPoolEntryFieldOffset = 240;

		// Confirmed live: +301 is a static "ability equipped" flag
		// (never changes), +302 toggles with actually being engaged in
		// Dead Eye.
		constexpr std::uintptr_t kActiveFlagOffset = 302;

		constexpr int kLocalPlayerIndex = 0;

		// Mirrors the exact tagged-pointer resolution _GET_PLAYER_DEAD_EYE
		// and _ACTIVATE_DEAD_EYE both perform -- see header comment.
		// Returns 0 if any stage is invalid.
		std::uint64_t ResolveAbilityPointer(int playerIndex)
		{
			std::uint64_t ped = GameMemory::ResolvePlayerPed(playerIndex);
			if (!ped)
				return 0;

			std::uint64_t poolEntry = GameMemory::ResolvePedLinkedPoolEntry(ped, kPoolEntryFieldOffset);
			if (!poolEntry)
				return 0;

			std::uint64_t ability = *reinterpret_cast<std::uint64_t*>(poolEntry + kAbilityPointerOffset);
			return GameMemory::LooksLikeValidPointer(ability) ? ability : 0;
		}

		bool IsDeadEyeActive()
		{
			std::uint64_t ability = ResolveAbilityPointer(kLocalPlayerIndex);
			if (!ability)
				return false;
			return *reinterpret_cast<std::uint8_t*>(ability + kActiveFlagOffset) != 0;
		}

		bool g_wasActive = false;
		double g_windowEnterMs = 0.0;

		void MaybeDelayWhileDeadEyeActive()
		{
			bool active = IsDeadEyeActive();
			double now = GameMemory::NowMs();

			if (active && !g_wasActive)
			{
				g_windowEnterMs = now;
				Log::Write("DeadEyeFix: DEAD EYE ACTIVE -- choking");
			}
			else if (!active && g_wasActive)
			{
				Log::Write("DeadEyeFix: DEAD EYE INACTIVE -- choke released after {:.0f}ms",
					now - g_windowEnterMs);
			}
			g_wasActive = active;

			// The choke lives behind IsDeadEyeActive() alone now (reads
			// ability+302, see header comment). The FPS floor that used to
			// also gate this was dropped -- see FishingFix.cpp's Tick() for
			// why (the FPS estimate proved unreliable enough to sometimes
			// skip the choke exactly when it was needed).
			if (active)
				GameMemory::PreciseWaitMs(kDelayMs);
		}
	}

	void OnTick()
	{
		MaybeDelayWhileDeadEyeActive();
	}
}
