/*
	CHOKE TEST for Dead Eye, wired up the same way FishingFix.cpp's own
	confirmed mechanism is: a precise busy-wait applied from this ASI's
	own script tick (see script.cpp), gated on whether Dead Eye is
	currently active -- not from any native hook. FishingFix's own
	version of this exact test (delay moved out of the _GET_TASK_FISHING
	hook and into ScriptMain, gated on the script's own fishing phase)
	confirmed live that the fix works by giving the racing worker thread
	wall-clock time ANYWHERE on the shared script thread during the race
	window, and that the resulting FPS hit is fully and only gated by
	that phase check (one ENTER, one EXIT, nothing outside the bracket).
	This applies that same confirmed approach directly to Dead Eye,
	without first needing to find its own equivalent of
	_GET_TASK_FISHING.

	The FPS estimate this gates on is computed once in ScriptMain
	(script.cpp) and passed in -- it used to be tracked independently
	here too (a second QueryPerformanceCounter-based estimate of the same
	thing FishingFix.cpp was already computing every tick).

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
	1491.50):

		ped       = sub_140EE4DB0(playerIndex)
		poolIndex = *(DWORD*)(ped + 156) & 0x1FFFF
		poolEntry = *(QWORD*)(328 * (poolIndex - dword_1439ECE40)
		                      + qword_1439ECE48 + 240) & ~1ULL
		ability   = *(QWORD*)(poolEntry + 0x93B0)
*/

#include "DeadEyeDiag.h"
#include "Log.h"

#include <windows.h>
#include <cstdint>

namespace DeadEyeDiag
{
	namespace
	{
		// Same value FishingFix confirmed live -- see its own header
		// comment for how it was tuned.
		constexpr double kDelayMs = 3.0;

		// Static addresses read directly out of IDA (RDR2_Dumped.exe.i64,
		// 1491.50) -- rebased to this process's actual load address:
		// liveAddress = GetModuleHandle(nullptr) + (idaAddress - 0x140000000).
		constexpr std::uintptr_t kIdaImageBase = 0x140000000ull;
		constexpr std::uintptr_t kIdaResolvePedFn = 0x140EE4DB0ull; // sub_140EE4DB0
		constexpr std::uintptr_t kIdaPoolIndexBase = 0x1439ECE40ull; // dword_1439ECE40
		constexpr std::uintptr_t kIdaPoolArrayBase = 0x1439ECE48ull; // qword_1439ECE48

		constexpr std::uintptr_t kAbilityPointerOffset = 0x93B0; // 37808 decimal
		constexpr std::uintptr_t kPoolEntryStride = 328;
		constexpr std::uintptr_t kPoolEntryFieldOffset = 240;

		// Confirmed live: +301 is a static "ability equipped" flag
		// (never changes), +302 toggles with actually being engaged in
		// Dead Eye.
		constexpr std::uintptr_t kActiveFlagOffset = 302;

		constexpr int kLocalPlayerIndex = 0;

		using ResolvePedFn = std::uint64_t(__fastcall*)(int playerIndex);

		std::uintptr_t Rebase(std::uintptr_t idaAddress)
		{
			static const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(GetModuleHandleA(nullptr));
			return base + (idaAddress - kIdaImageBase);
		}

		// Cheap sanity check before dereferencing something computed
		// ourselves instead of the game -- not a full validity check,
		// just enough to avoid an obvious wild read if the pool math
		// ever produces garbage (e.g. no local player yet).
		bool LooksLikeValidPointer(std::uint64_t p)
		{
			return p > 0x10000 && p < 0x0000800000000000ull;
		}

		// Mirrors the exact tagged-pointer resolution _GET_PLAYER_DEAD_EYE
		// and _ACTIVATE_DEAD_EYE both perform -- see header comment.
		// Returns 0 if any stage is invalid.
		std::uint64_t ResolveAbilityPointer(int playerIndex)
		{
			static ResolvePedFn resolvePed = reinterpret_cast<ResolvePedFn>(Rebase(kIdaResolvePedFn));

			std::uint64_t ped = resolvePed(playerIndex);
			if (!LooksLikeValidPointer(ped))
				return 0;

			std::uint32_t poolIndex = *reinterpret_cast<std::uint32_t*>(ped + 156) & 0x1FFFFu;
			std::uint32_t poolIndexBase = *reinterpret_cast<std::uint32_t*>(Rebase(kIdaPoolIndexBase));
			std::uint64_t poolArrayBase = *reinterpret_cast<std::uint64_t*>(Rebase(kIdaPoolArrayBase));

			std::uint64_t entryAddr = kPoolEntryStride * (static_cast<std::uint64_t>(poolIndex) - poolIndexBase)
				+ poolArrayBase + kPoolEntryFieldOffset;
			if (!LooksLikeValidPointer(entryAddr))
				return 0;

			std::uint64_t tagged = *reinterpret_cast<std::uint64_t*>(entryAddr);
			if (tagged == 0)
				return 0;

			std::uint64_t poolEntry = tagged & ~1ull;
			if (!LooksLikeValidPointer(poolEntry))
				return 0;

			std::uint64_t ability = *reinterpret_cast<std::uint64_t*>(poolEntry + kAbilityPointerOffset);
			return LooksLikeValidPointer(ability) ? ability : 0;
		}

		bool IsDeadEyeActive()
		{
			std::uint64_t ability = ResolveAbilityPointer(kLocalPlayerIndex);
			if (!ability)
				return false;
			return *reinterpret_cast<std::uint8_t*>(ability + kActiveFlagOffset) != 0;
		}

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

		void PreciseWaitMs(double ms)
		{
			double start = NowMs();
			while (NowMs() - start < ms)
			{
			}
		}

		bool g_wasActive = false;
		double g_windowEnterMs = 0.0;

		void MaybeDelayWhileDeadEyeActive()
		{
			bool active = IsDeadEyeActive();
			double now = NowMs();

			if (active && !g_wasActive)
			{
				g_windowEnterMs = now;
				Log::Write("DeadEyeDiag: DEAD EYE ACTIVE -- choking");
			}
			else if (!active && g_wasActive)
			{
				Log::Write("DeadEyeDiag: DEAD EYE INACTIVE -- choke released after {:.0f}ms",
					now - g_windowEnterMs);
			}
			g_wasActive = active;

			// The choke lives behind IsDeadEyeActive() alone now (reads
			// ability+302, see header comment). The FPS floor that used to
			// also gate this was dropped -- see FishingFix.cpp's Tick() for
			// why (the FPS estimate proved unreliable enough to sometimes
			// skip the choke exactly when it was needed).
			if (active)
				PreciseWaitMs(kDelayMs);
		}
	}

	void OnTick()
	{
		MaybeDelayWhileDeadEyeActive();
	}
}
