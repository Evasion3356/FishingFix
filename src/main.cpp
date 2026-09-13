/*
	Entry point. Registers ScriptMain as a ScriptHookRDR2 script thread,
	same pattern as PokerCheat/BlackjackCheat's own main.cpp.
*/

#include "..\..\ScriptHookSDK\inc\main.h"
#include "script.h"

BOOL APIENTRY DllMain(HMODULE hInstance, DWORD reason, LPVOID lpReserved)
{
	switch (reason)
	{
	case DLL_PROCESS_ATTACH:
		scriptRegister(hInstance, ScriptMain);
		break;
	case DLL_PROCESS_DETACH:
		scriptUnregister(hInstance);
		break;
	}
	return TRUE;
}
