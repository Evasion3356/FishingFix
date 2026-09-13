/*
	Entry point. Registers ScriptMain as a ScriptHookRDR2 script thread,
	same pattern as PokerCheat/BlackjackCheat's own main.cpp.

	DLL_PROCESS_DETACH calls NativeHook::RemoveAll() BEFORE
	scriptUnregister -- without it, fishing_core's own native table keeps
	pointing at a hook function that lives inside this DLL after it
	unmaps, and the next call into it jumps into unmapped memory (a real
	crash this project hit via ScriptHookRDR2's eject feature before this
	existed; see CLAUDE.md).
*/

#include "..\..\ScriptHookSDK\inc\main.h"
#include "script.h"
#include "NativeHook.h"

BOOL APIENTRY DllMain(HMODULE hInstance, DWORD reason, LPVOID lpReserved)
{
	switch (reason)
	{
	case DLL_PROCESS_ATTACH:
		scriptRegister(hInstance, ScriptMain);
		break;
	case DLL_PROCESS_DETACH:
		NativeHook::RemoveAll();
		scriptUnregister(hInstance);
		break;
	}
	return TRUE;
}
