#include "NativeHook.h"
#include "PatternScan.h"
#include "Log.h"

#include <array>
#include <cstdint>

namespace
{
	// Ported from HorseMenu's D:\Backup\Stuff\RDR2 Shit\HorseMenu\src\game\
	// pointers\Pointers.cpp -- same RIP-relative encodings GamePointers.cpp
	// (PokerCheat/BlackjackCheat) already relies on for its own
	// "ScriptThreads&RunScriptThreads" pattern, carried over from that same
	// file:
	//
	//   constexpr auto getNativeHandlerPtrn = Pattern<"E8 ? ? ? ? 42 8B 9C FE">("GetNativeHandler");
	//   GetNativeHandler = ptr.Add(1).Rip().As<Functions::GetNativeHandler>();
	//
	// The matched instruction is `CALL rel32` (E8) -- ptr.Add(1) skips the
	// 1-byte opcode to the 4-byte operand, .Rip() resolves it exactly like
	// PatternScan::ResolveRip (operand + 4 + displacement); a CALL's rel32
	// resolves identically to a LEA's, both relative to the address right
	// after the 4-byte operand.
	constexpr const char* kGetNativeHandlerPattern = "E8 ? ? ? ? 42 8B 9C FE";
	constexpr int kGetNativeHandlerOperandOffset = 1;

	//   constexpr auto scriptProgramsPtrn = Pattern<"C1 EF 0E 85 FF 74 21">("ScriptPrograms");
	//   ScriptPrograms = ptr.Sub(0x16).Add(3).Rip().Add(0xC8).As<rage::scrProgram**>();
	//
	// Backtracks 0x16 bytes from the match to an earlier `LEA reg,[rip+disp32]`
	// instruction (whose operand starts 3 bytes into it), resolves THAT
	// RIP-relative operand, then adds a fixed 0xC8 byte offset to land on
	// the actual scrProgram** array field within the struct that resolves to.
	constexpr const char* kScriptProgramsPattern = "C1 EF 0E 85 FF 74 21";
	constexpr std::ptrdiff_t kScriptProgramsBacktrack = 0x16;
	constexpr int kScriptProgramsOperandOffset = 3;
	constexpr std::uintptr_t kScriptProgramsFieldOffset = 0xC8;

	// Same bound NativeHooks.cpp's own RegisterProgramImpl() constructor
	// loop uses ("for (int i = 0; i < 160; i++)") -- the live array's real
	// length isn't otherwise exposed to us.
	constexpr int kMaxScriptPrograms = 160;

	using GetNativeHandlerFn = rage::scrNativeHandler(*)(std::uint64_t hash);

	GetNativeHandlerFn g_getNativeHandler = nullptr;
	rage::scrProgram** g_scriptPrograms = nullptr;
	bool g_resolveAttempted = false;

	struct HookEntry
	{
		rage::joaat_t scriptHash = 0;
		std::uint64_t nativeHash = 0;
		rage::scrNativeHandler replacement = nullptr;
		rage::scrNativeHandler original = nullptr;
		rage::scrNativeHandler knownAddress = nullptr; // set only for AddHookByAddress entries -- skips g_getNativeHandler entirely
		rage::scrProgram* installedOn = nullptr; // single-script case: which program instance currently holds `replacement`
		bool everInstalled = false;

		// ALL_SCRIPTS case only: per-array-slot cache of which program
		// instance we've already confirmed holds `replacement`, so Update()
		// can skip the expensive GetAddressOfNativeEntrypoint linear search
		// (over a program's ENTIRE native table) for programs already
		// handled, instead of redoing it for every one of up to 160
		// programs on EVERY tick. A slot only needs rechecking if the
		// program pointer living there changes (reload/unload/new load).
		std::array<rage::scrProgram*, 160> allScriptsCache{};
	};

	constexpr std::size_t kMaxHooks = 16;
	std::array<HookEntry, kMaxHooks> g_hooks{};
	std::size_t g_hookCount = 0;

	HookEntry* FindEntry(rage::joaat_t scriptHash, std::uint64_t nativeHash)
	{
		for (std::size_t i = 0; i < g_hookCount; i++)
		{
			if (g_hooks[i].scriptHash == scriptHash && g_hooks[i].nativeHash == nativeHash)
				return &g_hooks[i];
		}
		return nullptr;
	}

	bool EnsureResolved()
	{
		if (g_resolveAttempted)
			return g_getNativeHandler != nullptr && g_scriptPrograms != nullptr;

		g_resolveAttempted = true;

		auto handlerMatch = PatternScan::FindInMainModule(kGetNativeHandlerPattern);
		if (handlerMatch)
		{
			g_getNativeHandler = reinterpret_cast<GetNativeHandlerFn>(
				PatternScan::ResolveRip(*handlerMatch, kGetNativeHandlerOperandOffset));
			Log::Write("NativeHook: GetNativeHandler resolved to 0x{:X}",
				reinterpret_cast<std::uintptr_t>(g_getNativeHandler));
		}
		else
		{
			Log::Write("NativeHook: GetNativeHandler pattern not found (build mismatch?)");
		}

		auto programsMatch = PatternScan::FindInMainModule(kScriptProgramsPattern);
		if (programsMatch)
		{
			std::uintptr_t backtracked = *programsMatch - kScriptProgramsBacktrack;
			std::uintptr_t resolved = PatternScan::ResolveRip(backtracked, kScriptProgramsOperandOffset);
			g_scriptPrograms = reinterpret_cast<rage::scrProgram**>(resolved + kScriptProgramsFieldOffset);
			Log::Write("NativeHook: ScriptPrograms resolved to 0x{:X}",
				reinterpret_cast<std::uintptr_t>(g_scriptPrograms));
		}
		else
		{
			Log::Write("NativeHook: ScriptPrograms pattern not found (build mismatch?)");
		}

		return g_getNativeHandler != nullptr && g_scriptPrograms != nullptr;
	}

	rage::scrProgram* FindScriptProgram(rage::joaat_t scriptHash)
	{
		if (!EnsureResolved())
			return nullptr;

		for (int i = 0; i < kMaxScriptPrograms; i++)
		{
			rage::scrProgram* program = g_scriptPrograms[i];
			if (program && program->IsValid() && program->m_NameHash == scriptHash)
				return program;
		}

		return nullptr;
	}

	// Ensures `entry.replacement` is installed in `program`'s own native
	// table specifically. Safe to call every tick -- a no-op once already
	// installed there (checked by searching for our own replacement's
	// identity first). Returns false only on a real failure (native hash
	// doesn't resolve, or this program's table doesn't contain that
	// native's slot at all -- the latter is normal/expected for a
	// program that never calls this native, not an error).
	bool InstallIntoProgram(HookEntry& entry, rage::scrProgram* program)
	{
		if (program->GetAddressOfNativeEntrypoint(entry.replacement))
			return true; // already installed here

		rage::scrNativeHandler currentGlobalHandler = entry.knownAddress;
		if (!currentGlobalHandler)
		{
			currentGlobalHandler = g_getNativeHandler(entry.nativeHash);
			if (!currentGlobalHandler)
			{
				Log::Write("NativeHook: GetNativeHandler(0x{:X}) returned null -- wrong hash for this build?", entry.nativeHash);
				return false;
			}
		}

		rage::scrNativeHandler* slot = program->GetAddressOfNativeEntrypoint(currentGlobalHandler);
		if (!slot)
			return false; // this program's table doesn't call this native -- not an error

		if (!entry.original)
			entry.original = *slot; // same global handler everywhere, capture once
		*slot = entry.replacement;
		entry.everInstalled = true;

		// Logged every time a NEW program instance gets patched -- with
		// the allScriptsCache/installedOn caches in Update(), this only
		// fires once per distinct program instance, not every tick.
		Log::Write("NativeHook: installed hook for native 0x{:X} in script '{}' (program 0x{:X})",
			entry.nativeHash, program->m_Name ? program->m_Name : "?",
			reinterpret_cast<std::uintptr_t>(program));
		return true;
	}
}

namespace NativeHook
{
	void AddHook(rage::joaat_t scriptHash, std::uint64_t nativeHash, rage::scrNativeHandler replacement)
	{
		if (FindEntry(scriptHash, nativeHash) || g_hookCount >= kMaxHooks)
			return;

		HookEntry& entry = g_hooks[g_hookCount++];
		entry.scriptHash = scriptHash;
		entry.nativeHash = nativeHash;
		entry.replacement = replacement;
	}

	void AddHookByAddress(rage::joaat_t scriptHash, std::uint64_t nativeHash, rage::scrNativeHandler knownAddress, rage::scrNativeHandler replacement)
	{
		if (FindEntry(scriptHash, nativeHash) || g_hookCount >= kMaxHooks)
			return;

		HookEntry& entry = g_hooks[g_hookCount++];
		entry.scriptHash = scriptHash;
		entry.nativeHash = nativeHash;
		entry.replacement = replacement;
		entry.knownAddress = knownAddress;
	}

	void Update()
	{
		if (!EnsureResolved())
			return;

		for (std::size_t i = 0; i < g_hookCount; i++)
		{
			HookEntry& entry = g_hooks[i];

			if (entry.scriptHash == NativeHook::ALL_SCRIPTS)
			{
				for (int p = 0; p < kMaxScriptPrograms; p++)
				{
					rage::scrProgram* program = g_scriptPrograms[p];
					if (!program || !program->IsValid())
					{
						entry.allScriptsCache[p] = nullptr;
						continue;
					}

					if (entry.allScriptsCache[p] == program)
						continue; // already checked this exact program instance -- skip the expensive search either way

					InstallIntoProgram(entry, program);
					entry.allScriptsCache[p] = program; // don't re-search this instance again, success or not
				}
				continue;
			}

			rage::scrProgram* program = FindScriptProgram(entry.scriptHash);
			if (!program)
			{
				entry.installedOn = nullptr;
				continue;
			}

			InstallIntoProgram(entry, program);
			entry.installedOn = program;
		}
	}

	void CallOriginal(rage::joaat_t scriptHash, std::uint64_t nativeHash, rage::scrNativeCallContext* ctx)
	{
		HookEntry* entry = FindEntry(scriptHash, nativeHash);
		if (entry && entry->original)
			entry->original(ctx);
	}

	void RemoveAll()
	{
		for (std::size_t i = 0; i < g_hookCount; i++)
		{
			HookEntry& entry = g_hooks[i];

			if (!entry.original)
			{
				entry.installedOn = nullptr;
				entry.everInstalled = false;
				continue;
			}

			if (entry.scriptHash == NativeHook::ALL_SCRIPTS)
			{
				int restoredCount = 0;
				for (int p = 0; p < kMaxScriptPrograms; p++)
				{
					rage::scrProgram* program = g_scriptPrograms ? g_scriptPrograms[p] : nullptr;
					if (!program)
						continue;

					rage::scrNativeHandler* slot = program->GetAddressOfNativeEntrypoint(entry.replacement);
					if (slot)
					{
						*slot = entry.original;
						restoredCount++;
					}
				}
				Log::Write("NativeHook: restored original handler for native 0x{:X} across {} program(s)",
					entry.nativeHash, restoredCount);
			}
			else if (entry.installedOn)
			{
				// Search by our own replacement's identity, not the
				// (long since overwritten) original global handler's --
				// same reasoning as Update()'s own re-search fallback.
				rage::scrNativeHandler* slot = entry.installedOn->GetAddressOfNativeEntrypoint(entry.replacement);
				if (slot)
				{
					*slot = entry.original;
					Log::Write("NativeHook: restored original handler for native 0x{:X} (program 0x{:X})",
						entry.nativeHash, reinterpret_cast<std::uintptr_t>(entry.installedOn));
				}
				else
				{
					Log::Write("NativeHook: WARNING could not find our slot to restore native 0x{:X} -- program table left patched (unload is about to crash it)",
						entry.nativeHash);
				}
			}

			entry.installedOn = nullptr;
			entry.everInstalled = false;
		}

		g_hookCount = 0;
	}
}
