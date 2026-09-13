/*
	Per-SCRIPT native hooking -- patches a single rage::scrProgram's own
	m_NativeEntrypoints table, so a hook installed for e.g. "fishing_core"
	only fires for natives called FROM fishing_core's own bytecode. Every
	other script's calls to the same native (including this ASI's own
	invoke<>() calls via ScriptHookRDR2's SDK) are completely untouched --
	no global detour, no trampoline.

	This is a deliberately trimmed-down port of HorseMenu's own
	src/game/backend/NativeHooks.cpp technique -- NOT a copy of that file.
	HorseMenu's version is built on a generic "call any of thousands of
	natives by a generated NativeIndex enum" system (Crossmap.hpp,
	Natives.hpp -- both 100k+ line generated files) that exists to make
	EVERY native in the game callable fast from anywhere in that codebase.
	This project only ever needs to hook two or three specific natives by
	their already-known hash (see FishingFix.cpp), so it skips that
	whole system and calls scrProgram::GetAddressOfNativeEntrypoint()
	directly -- same underlying mechanism, none of the generated-code
	weight.

	The two raw engine pointers this needs (RDR2.exe's global native-hash
	-> current-handler resolver, and the live array of loaded
	rage::scrProgram*) are resolved via AOB signature, ported from
	HorseMenu's own D:\Backup\Stuff\RDR2 Shit\HorseMenu\src\game\pointers\
	Pointers.cpp ("GetNativeHandler"/"ScriptPrograms" patterns) -- see
	NativeHook.cpp's header comment for the exact byte patterns and the
	one open risk (HorseMenu tracks whatever RDR2 build is currently live
	on Steam, which may not be 1491.50; unlike GamePointers.cpp's own
	borrowed "ScriptThreads&RunScriptThreads" pattern from the same file,
	which IS already confirmed working on 1491.50 via PokerCheat/
	BlackjackCheat, these two are NOT yet independently confirmed on this
	build).
*/

#pragma once

#include "..\external\RDR-Classes\script\scrProgram.hpp"
#include "..\external\RDR-Classes\script\scrNativeHandler.hpp"
#include "..\external\RDR-Classes\rage\joaat.hpp"

#include <cstdint>

namespace NativeHook
{
	// Pass as `scriptHash` to AddHook to install into EVERY currently
	// loaded scrProgram instead of one specific script -- for when the
	// interrupting caller isn't known in advance (e.g. checking whether
	// some OTHER script is calling DISABLE_CONTROL_ACTION, not just the
	// one script this project otherwise cares about). Costs more (scans
	// every loaded program every tick) and is broader by nature -- use a
	// specific script hash instead whenever the caller is already known.
	inline const rage::joaat_t ALL_SCRIPTS = rage::Joaat("ALL_SCRIPTS");

	// Registers a desired hook: from now on, every Update() call ensures
	// `replacement` is installed in place of the native `nativeHash`
	// inside `scriptHash`'s own scrProgram native table specifically (or
	// every loaded program's, if scriptHash is ALL_SCRIPTS). Call once at
	// startup per (scriptHash, nativeHash) pair -- safe to call before
	// the target script is even loaded, since Update() installs it
	// lazily once it is.
	void AddHook(rage::joaat_t scriptHash, std::uint64_t nativeHash, rage::scrNativeHandler replacement);

	// Same as AddHook, but for a native whose HASH isn't known -- only its
	// real handler address (e.g. resolved statically via IDA + ASLR
	// rebase: liveAddress = GetModuleHandle(nullptr) + (idaAddress -
	// 0x140000000)). Skips the hash->handler resolver entirely and
	// searches each program's table directly for `knownAddress`'s
	// identity. Use `nativeHash` (e.g. rage::Joaat of a made-up label) as
	// this hook's own lookup key for CallOriginal -- it's never used to
	// resolve anything, only to find this entry again later.
	void AddHookByAddress(rage::joaat_t scriptHash, std::uint64_t nativeHash, rage::scrNativeHandler knownAddress, rage::scrNativeHandler replacement);

	// Re-applies every registered hook whose target script is currently
	// loaded -- both to install ones that weren't loaded yet, and to
	// re-patch ones a script reload would otherwise silently reset. Call
	// once per tick.
	void Update();

	// Invokes whatever handler a given native had BEFORE this file
	// patched it, for the given (scriptHash, nativeHash) hook -- a
	// replacement handler calls this to preserve the native's real
	// behavior instead of reimplementing it. No-op if that hook was never
	// successfully installed yet.
	void CallOriginal(rage::joaat_t scriptHash, std::uint64_t nativeHash, rage::scrNativeCallContext* ctx);

	// Restores every currently-installed hook's ORIGINAL native handler
	// back into its program's table, and forgets all registrations.
	// MUST be called from DLL_PROCESS_DETACH before this module unmaps --
	// a hooked program's table otherwise keeps pointing at replacement
	// functions that live inside this DLL, and the next call into one of
	// them after unload jumps into unmapped memory (a real crash this
	// project hit via ScriptHookRDR2's eject feature before this existed;
	// see CLAUDE.md). Safe to call even if nothing was ever installed.
	void RemoveAll();
}
