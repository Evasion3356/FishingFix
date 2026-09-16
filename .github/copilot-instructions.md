# Copilot instructions for FishingFix

## IDA / disassembly workflow

- There is no dedicated IDA database MCP/tool exposed in this Copilot CLI environment. If IDA itself is not installed or on `PATH`, use the dumped executable beside the IDA database instead:
  - IDB: `D:\Backup\Stuff\RDR2 Shit\EXEs\1491.50\RDR2_Dumped.exe.i64`
  - EXE: `D:\Backup\Stuff\RDR2 Shit\EXEs\1491.50\RDR2_Dumped.exe`
- The dumped EXE can be inspected directly with Python `pefile` + `capstone`, which are available in this environment. Convert IDA VAs to file offsets with `pe.get_offset_from_rva(va - pe.OPTIONAL_HEADER.ImageBase)` and disassemble from the IDA VA.
- IDA addresses in this project assume image base `0x140000000`. When hardcoding an IDA address temporarily for diagnostics, runtime addresses must be rebased as:
  `live = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr)) + (idaAddress - 0x140000000)`.
- Prefer HorseMenu-style signatures over permanent hardcoded addresses. `GameMemory.cpp` intentionally keeps a tiny local one-time scanner instead of vendoring HorseMenu's full scanner/pointer system.
- Runtime signatures are resolved by `GameMemory::Init()` via `FishingFix::Init()`, called immediately after `ScriptMain()` logs `FishingFix started`. This keeps resolution after the game EXE is loaded/decrypted in memory, but before the per-tick gate begins.
- For `TASK::_GET_TASK_FISHING` in build 1491.50, the labeled implementation is `sub_141077798`. The raw phase path confirmed from that function is:
  - only run the gate while `fishing_core` is running; current code checks the raw script thread array resolved by HorseMenu's `ScriptThreads&RunScriptThreads` signature against `joaat("fishing_core")`
  - enumerate peds from the raw ped pool resolved by HorseMenu's `PedPool` signature; do not call `worldGetAllPeds` for the fishing gate
  - `rawPoolIndex = *(DWORD*)(ped + 0x9C) & 0x1FFFF`
  - resolve `dword_1439ECE40` and `qword_1439ECE48` from the `GET_TASK_FISHING` instruction pattern, then read `poolEntry = *(QWORD*)(qword_1439ECE48 + (rawPoolIndex - dword_1439ECE40) * 0x148 + 0xB8) & ~1ULL`
  - `taskManager = *(QWORD*)(poolEntry + 0x170)`
  - `task = sub_142B2EF3C(taskManager, 0x271)`, with `sub_142B2EF3C` resolved by signature
  - fishing phase is the 4-byte int at `task + 0xF8`
- Current runtime signatures used by `GameMemory.cpp`:
  - script threads: `48 8D 0D ? ? ? ? E8 ? ? ? ? EB 0B 8B 0D`
  - ped pool encryption: `0F 28 F0 48 85 DB 74 56 8A 05 ? ? ? ? 84 C0 75 05`
  - task pool globals inside `GET_TASK_FISHING`: use the long `GET_TASK_FISHING`-anchored pattern in `GameMemory.cpp`; the shorter `8B 86 9C ... 0xB8` sequence is not unique across the EXE
  - task lookup helper: `44 8B 41 10 41 83 F8 FF 0F 85 06 00 00 00 33 C0 C3`
  - local player ped resolver (`sub_140EE4DB0`, used by `DeadEyeFix`): `48 89 5C 24 08 57 48 83 EC 20 33 DB 38 1D ? ? ? ? 74 26 E8 ? ? ? ? 48 8B F8 48 85 C0 74 64`
- `GET_TASK_FISHING` returns true only when task `0x271` is found and copied. If task `0x271` is missing, the native may still write fallback/default out-struct fields, but returns false; the fix should match the true-return path.
- Only read the 4-byte low half for script-visible scalar fields. Do not compare or copy an 8-byte `Any` slot when the script field is a plain int/float; the high 32 bits can contain unrelated leftover data.
- Keep runtime proof logs temporary. Once a raw pointer path is confirmed live, remove one-shot diagnostic breadcrumbs so the tick path stays as cheap as possible.
