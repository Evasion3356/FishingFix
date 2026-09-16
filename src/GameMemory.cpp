#include "GameMemory.h"
#include "Log.h"

#include <windows.h>
#include <cstddef>
#include <cstdio>
#include <intrin.h>

namespace GameMemory
{
	namespace
	{
		constexpr const char* kScriptThreadsPattern = "48 8D 0D ? ? ? ? E8 ? ? ? ? EB 0B 8B 0D";
		constexpr const char* kPedPoolPattern = "0F 28 F0 48 85 DB 74 56 8A 05 ? ? ? ? 84 C0 75 05";
		constexpr const char* kGetTaskFishingTaskPoolPattern = "E8 ? ? ? ? 48 8B F0 48 85 C0 0F 84 ? ? ? ? 48 8B C8 E8 ? ? ? ? 48 85 C0 0F 84 ? ? ? ? 48 8B CE E8 ? ? ? ? 48 8B C8 33 D2 E8 ? ? ? ? 48 8B C8 E8 ? ? ? ? 33 C9 84 C0 0F 95 C1 89 0B 8B 86 9C 00 00 00 25 FF FF 01 00 2B 05 ? ? ? ? 48 69 C8 48 01 00 00 48 8B 05 ? ? ? ? 48 8B 94 01 B8 00 00 00";
		constexpr const char* kFindTaskByIdPattern = "44 8B 41 10 41 83 F8 FF 0F 85 06 00 00 00 33 C0 C3";
		constexpr const char* kResolvePlayerPedPattern = "48 89 5C 24 08 57 48 83 EC 20 33 DB 38 1D ? ? ? ? 74 26 E8 ? ? ? ? 48 8B F8 48 85 C0 74 64";

		constexpr std::uintptr_t kPedPoolIndexOffset = 0x9C;
		constexpr std::uintptr_t kPoolEntryStride = 0x148;

		using FindTaskByIdFn = std::uint64_t(__fastcall*)(std::uint64_t taskManager, int taskId);
		using ResolvePlayerPedFn = std::uint64_t(__fastcall*)(int playerIndex);

		struct AtArrayPtr
		{
			std::uint64_t* data;
			std::uint16_t size;
			std::uint16_t count;
		};

		struct PoolEncryption
		{
			bool isSet;
			std::uint8_t pad[7];
			std::uint64_t first;
			std::uint64_t second;
		};

		struct RuntimePointers
		{
			AtArrayPtr* scriptThreads = nullptr;
			PoolEncryption* pedPoolEncryption = nullptr;
			std::uint32_t* linkedPoolIndexBase = nullptr;
			std::uint64_t* linkedPoolArrayBase = nullptr;
			FindTaskByIdFn findTaskById = nullptr;
			ResolvePlayerPedFn resolvePlayerPed = nullptr;
			bool initialized = false;
			bool loggedFailure = false;
			bool loggedSuccess = false;
		};

		RuntimePointers g_pointers;
		double g_qpcFrequency = 0.0;

		std::uintptr_t ModuleBase()
		{
			static const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(GetModuleHandleA(nullptr));
			return base;
		}

		int HexNibble(char c)
		{
			if (c >= '0' && c <= '9')
				return c - '0';
			if (c >= 'A' && c <= 'F')
				return c - 'A' + 10;
			if (c >= 'a' && c <= 'f')
				return c - 'a' + 10;
			return -1;
		}

		std::uint8_t* FindPattern(const char* pattern)
		{
			std::uint8_t bytes[160]{};
			bool wildcard[160]{};
			std::size_t patternSize = 0;

			for (const char* p = pattern; *p;)
			{
				while (*p == ' ')
					++p;
				if (!*p)
					break;
				if (patternSize >= sizeof(bytes))
					return nullptr;

				if (*p == '?')
				{
					wildcard[patternSize++] = true;
					while (*p == '?')
						++p;
				}
				else
				{
					int hi = HexNibble(p[0]);
					int lo = HexNibble(p[1]);
					if (hi < 0 || lo < 0)
						return nullptr;
					bytes[patternSize++] = static_cast<std::uint8_t>((hi << 4) | lo);
					p += 2;
				}
			}

			auto base = reinterpret_cast<std::uint8_t*>(ModuleBase());
			auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
			auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
			std::size_t imageSize = nt->OptionalHeader.SizeOfImage;

			for (std::size_t i = 0; i + patternSize <= imageSize; ++i)
			{
				bool match = true;
				for (std::size_t j = 0; j < patternSize; ++j)
				{
					if (!wildcard[j] && base[i + j] != bytes[j])
					{
						match = false;
						break;
					}
				}
				if (match)
					return base + i;
			}

			return nullptr;
		}

		template<typename T>
		T ResolveRip(std::uint8_t* instruction, std::ptrdiff_t relOffset, std::ptrdiff_t instructionSize)
		{
			std::int32_t rel = *reinterpret_cast<std::int32_t*>(instruction + relOffset);
			return reinterpret_cast<T>(instruction + instructionSize + rel);
		}

		bool HasAllPointers()
		{
			return g_pointers.scriptThreads
				&& g_pointers.pedPoolEncryption
				&& g_pointers.linkedPoolIndexBase
				&& g_pointers.linkedPoolArrayBase
				&& g_pointers.findTaskById
				&& g_pointers.resolvePlayerPed;
		}
	}

	std::string FormatRdr2Offset(const void* ptr)
	{
		if (!ptr)
			return "<not found>";

		std::uintptr_t address = reinterpret_cast<std::uintptr_t>(ptr);
		std::uintptr_t base = ModuleBase();
		char buf[64];
		if (address >= base)
		{
			std::snprintf(buf, sizeof(buf), "RDR2.exe+0x%zX", static_cast<std::size_t>(address - base));
			return buf;
		}

		std::snprintf(buf, sizeof(buf), "0x%zX", static_cast<std::size_t>(address));
		return buf;
	}

	bool LooksLikeValidPointer(std::uint64_t p)
	{
		return p > 0x10000 && p < 0x0000800000000000ull;
	}

	bool Init()
	{
		if (g_pointers.initialized)
			return HasAllPointers();

		std::uint8_t* scriptThreads = FindPattern(kScriptThreadsPattern);
		std::uint8_t* pedPool = FindPattern(kPedPoolPattern);
		std::uint8_t* getTaskFishingTaskPool = FindPattern(kGetTaskFishingTaskPoolPattern);
		std::uint8_t* findTaskById = FindPattern(kFindTaskByIdPattern);
		std::uint8_t* resolvePlayerPed = FindPattern(kResolvePlayerPedPattern);

		if (scriptThreads)
			g_pointers.scriptThreads = ResolveRip<AtArrayPtr*>(scriptThreads, 3, 7);
		if (pedPool)
			g_pointers.pedPoolEncryption = ResolveRip<PoolEncryption*>(pedPool + 8, 2, 6);
		if (getTaskFishingTaskPool)
		{
			g_pointers.linkedPoolIndexBase = ResolveRip<std::uint32_t*>(getTaskFishingTaskPool + 0x50, 2, 6);
			g_pointers.linkedPoolArrayBase = ResolveRip<std::uint64_t*>(getTaskFishingTaskPool + 0x5D, 3, 7);
		}
		g_pointers.findTaskById = reinterpret_cast<FindTaskByIdFn>(findTaskById);
		g_pointers.resolvePlayerPed = reinterpret_cast<ResolvePlayerPedFn>(resolvePlayerPed);
		g_pointers.initialized = true;

		Log::Write("GameMemory: signature ScriptThreads {} -> {}",
			FormatRdr2Offset(scriptThreads),
			FormatRdr2Offset(g_pointers.scriptThreads));
		Log::Write("GameMemory: signature PedPool {} -> {}",
			FormatRdr2Offset(pedPool),
			FormatRdr2Offset(g_pointers.pedPoolEncryption));
		Log::Write("GameMemory: signature GetTaskFishingTaskPool {} -> linkedPoolIndex={}, linkedPoolArray={}",
			FormatRdr2Offset(getTaskFishingTaskPool),
			FormatRdr2Offset(g_pointers.linkedPoolIndexBase),
			FormatRdr2Offset(g_pointers.linkedPoolArrayBase));
		Log::Write("GameMemory: signature FindTaskById {} -> {}",
			FormatRdr2Offset(findTaskById),
			FormatRdr2Offset(reinterpret_cast<void*>(g_pointers.findTaskById)));
		Log::Write("GameMemory: signature ResolvePlayerPed {} -> {}",
			FormatRdr2Offset(resolvePlayerPed),
			FormatRdr2Offset(reinterpret_cast<void*>(g_pointers.resolvePlayerPed)));

		bool ok = HasAllPointers();
		if (ok && !g_pointers.loggedSuccess)
		{
			Log::Write("GameMemory: runtime signatures resolved");
			g_pointers.loggedSuccess = true;
		}
		else if (!ok && !g_pointers.loggedFailure)
		{
			Log::Write("GameMemory: failed to resolve runtime pointers (scriptThreads={}, pedPool={}, linkedPoolIndex={}, linkedPoolArray={}, findTaskById={}, resolvePlayerPed={})",
				FormatRdr2Offset(g_pointers.scriptThreads),
				FormatRdr2Offset(g_pointers.pedPoolEncryption),
				FormatRdr2Offset(g_pointers.linkedPoolIndexBase),
				FormatRdr2Offset(g_pointers.linkedPoolArrayBase),
				FormatRdr2Offset(reinterpret_cast<void*>(g_pointers.findTaskById)),
				FormatRdr2Offset(reinterpret_cast<void*>(g_pointers.resolvePlayerPed)));
			g_pointers.loggedFailure = true;
		}
		return ok;
	}

	bool IsReady()
	{
		return Init();
	}

	bool IsScriptRunning(std::uint32_t scriptHash)
	{
		if (!Init()
			|| !LooksLikeValidPointer(reinterpret_cast<std::uint64_t>(g_pointers.scriptThreads))
			|| !g_pointers.scriptThreads->data)
			return false;

		for (std::uint16_t i = 0; i < g_pointers.scriptThreads->size; ++i)
		{
			std::uint64_t thread = g_pointers.scriptThreads->data[i];
			if (!LooksLikeValidPointer(thread))
				continue;

			std::uint32_t threadId = *reinterpret_cast<std::uint32_t*>(thread + 0x8);
			std::uint32_t runningScriptHash = *reinterpret_cast<std::uint32_t*>(thread + 0xC);
			if (threadId != 0 && runningScriptHash == scriptHash)
				return true;
		}

		return false;
	}

	FwBasePool* GetPedPool()
	{
		if (!Init() || !g_pointers.pedPoolEncryption->isSet)
			return nullptr;

		std::uint64_t x = _rotl64(g_pointers.pedPoolEncryption->second, 30);
		return reinterpret_cast<FwBasePool*>(~_rotl64(_rotl64(x ^ g_pointers.pedPoolEncryption->first, ((x & 0xFF) & 31) + 3), 32));
	}

	std::uint64_t GetPoolEntry(FwBasePool* pool, std::uint32_t index)
	{
		if (!pool || index >= pool->size || !pool->flags || !pool->entries || pool->flags[index] == 0 || (pool->flags[index] & 0x80))
			return 0;

		std::uint64_t entry = pool->entries + static_cast<std::uint64_t>(index) * pool->itemSize;
		return *reinterpret_cast<std::uint64_t*>(entry + 0x10) ? entry : 0;
	}

	std::uint64_t ResolvePlayerPed(int playerIndex)
	{
		if (!Init())
			return 0;

		std::uint64_t ped = g_pointers.resolvePlayerPed(playerIndex);
		return LooksLikeValidPointer(ped) ? ped : 0;
	}

	std::uint64_t ResolvePedLinkedPoolEntry(std::uint64_t ped, std::uintptr_t entryFieldOffset)
	{
		if (!Init() || !LooksLikeValidPointer(ped))
			return 0;

		std::uint32_t rawPoolIndex = *reinterpret_cast<std::uint32_t*>(ped + kPedPoolIndexOffset) & 0x1FFFFu;
		std::uint32_t poolIndexBase = *g_pointers.linkedPoolIndexBase;
		if (rawPoolIndex < poolIndexBase)
			return 0;

		std::uint64_t poolArrayBase = *g_pointers.linkedPoolArrayBase;
		if (!LooksLikeValidPointer(poolArrayBase))
			return 0;

		std::uint64_t entryAddress =
			poolArrayBase
			+ (static_cast<std::uint64_t>(rawPoolIndex - poolIndexBase) * kPoolEntryStride)
			+ entryFieldOffset;
		if (!LooksLikeValidPointer(entryAddress))
			return 0;

		std::uint64_t taggedEntry = *reinterpret_cast<std::uint64_t*>(entryAddress);
		if (taggedEntry == 0)
			return 0;

		std::uint64_t entry = taggedEntry & ~1ull;
		return LooksLikeValidPointer(entry) ? entry : 0;
	}

	std::uint64_t FindTaskById(std::uint64_t taskManager, int taskId)
	{
		if (!Init() || !LooksLikeValidPointer(taskManager))
			return 0;

		std::uint64_t task = g_pointers.findTaskById(taskManager, taskId);
		return LooksLikeValidPointer(task) ? task : 0;
	}

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
}
