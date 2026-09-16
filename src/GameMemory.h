#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace GameMemory
{
	struct FwBasePool
	{
		void* vtable;
		std::uintptr_t entries;
		std::uint8_t* flags;
		std::uint32_t size;
		std::uint32_t itemSize;
		std::uint32_t nextSlotIndex;
		std::uint32_t unk24;
		std::uint32_t freeSlotIndex;
	};

	constexpr char ToLower(char c)
	{
		return c >= 'A' && c <= 'Z' ? static_cast<char>(c | (1 << 5)) : c;
	}

	consteval std::uint32_t Joaat(std::string_view str)
	{
		std::uint32_t hash = 0;
		for (char c : str)
		{
			hash += ToLower(c);
			hash += hash << 10;
			hash ^= hash >> 6;
		}
		hash += hash << 3;
		hash ^= hash >> 11;
		hash += hash << 15;
		return hash;
	}

	bool Init();
	bool IsReady();
	bool LooksLikeValidPointer(std::uint64_t p);
	bool IsScriptRunning(std::uint32_t scriptHash);

	FwBasePool* GetPedPool();
	std::uint64_t GetPoolEntry(FwBasePool* pool, std::uint32_t index);
	std::uint64_t ResolvePlayerPed(int playerIndex);
	std::uint64_t ResolvePedLinkedPoolEntry(std::uint64_t ped, std::uintptr_t entryFieldOffset);
	std::uint64_t FindTaskById(std::uint64_t taskManager, int taskId);
	bool IsPedWithinDistance(std::uint64_t ped, std::uint64_t otherPed, float maxDistance);

	double NowMs();
	void PreciseWaitMs(double ms);

	std::string FormatRdr2Offset(const void* ptr);
}
