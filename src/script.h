#pragma once

// Matches the ScriptHookRDR2 SDK sample convention, same as PokerCheat/
// BlackjackCheat's own script.h.
//
// natives.h here is the canonical allocatr/alloc8or.re-generated header
// (submodule external/ScriptHookSDK, not the stock 2019 SDK dump).
#include "..\external\ScriptHookSDK\inc\natives.h"
#include "..\external\ScriptHookSDK\inc\types.h"
#include "..\external\ScriptHookSDK\inc\enums.h"
#include "..\external\ScriptHookSDK\inc\main.h"

void ScriptMain();
