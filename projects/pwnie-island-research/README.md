# Pwnie Island Local Reverse-Engineering Research Framework

This research project targets only the **32-bit Windows educational client of Pwn Adventure 3: Pwnie Island**. It packages object relationships verified through PDB analysis, disassembly, and runtime experiments into an external console application and an injected DLL.

The current version implements and validates the following capabilities:

- locating `GameLogic.dll` at runtime and calculating every address as `module base + RVA`;
- validating the signature bytes of six functions before enabling hooks, with initialization aborted on any mismatch;
- using `Player::Tick` to identify the complete local `Player*` automatically, without requiring an initial sprint action;
- reading health, position, and movement parameters on the game tick thread;
- calling `Actor::SetPosition` through a tick-thread mailbox to provide verifiable and reversible teleportation;
- temporarily modifying `Player::CanJump` and three verified movement fields to implement experimental flight, with restoration on disable or unload;
- hooking `Player::GetSprintMultiplier()`, supporting return-value overrides and calls to the original function on the tick thread;
- temporarily changing page protection for the sprint constant in `.rdata`, followed by read-back validation and protection restoration;
- publishing status to the external console through shared memory named for the target PID; and
- retaining an unknown-value scanner whose scope is limited to writable `MEM_PRIVATE` pages.

The project does not implement offensive features, anti-cheat analysis or bypasses, stealth, drivers, or a general-purpose injector. Use it only in your own local educational game environment.

## Files

- `external_tool.cpp`: 32-bit external console, injection logic, shared-state client, and memory scanner.
- `research_dll.cpp`: tick and sprint hooks, position mailbox, flight state machine, and safe unloading.
- `shared_state.hpp`: verified RVAs, object offsets, error codes, and shared protocol (currently version 4).
- `FINAL_TUTORIAL_ZH.md`: complete Chinese-language workflow from binary identification to live validation.
- `VERIFICATION_20260812.md`: auditable runtime record dated August 12, 2026.
- `CMakeLists.txt`: 32-bit CMake/Ninja build configuration with statically linked MinGW runtimes.
- `CMakePresets.json`: MinGW32 Release preset for direct use from VS Code or PowerShell.

## Verified target build

```text
GameLogic.dll SHA-256:
8CAEB44F70A4D5F88C957756F6387B7E1C55C8E72F97E09A5B726E9C784D9570

GameLogic.pdb SHA-256:
41B78B15F205382180745FBCA0FFE2FCEB89E0D12A16D1283F5B240E29F96FEF
```

The DLL CodeView/RSDS identifier was checked against the PDB GUID and age. Every RVA, signature, and object layout must be revalidated for a different `GameLogic.dll` build; the values in this repository cannot be reused without verification.

## Build

Open **MSYS2 MINGW32**. Do not use UCRT64 or MINGW64:

```bash
pacman -S --needed git mingw-w64-i686-toolchain mingw-w64-i686-cmake mingw-w64-i686-ninja
cd /path/to/pwnie-island-research
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

You can also use the included preset from a standard PowerShell or VS Code terminal. First add `C:\msys64\mingw32\bin` and `C:\msys64\usr\bin` to `PATH` for the current terminal:

```powershell
$env:PATH = 'C:\msys64\mingw32\bin;C:\msys64\usr\bin;' + $env:PATH
cmake --preset mingw32-release
cmake --build --preset mingw32-release
```

Build outputs:

```text
build/pwnie_external.exe
build/pwnie_research.dll
```

Both files must be `PE32 Intel 80386`. CMake rejects 64-bit configurations. The MinGW runtime is linked statically, so the injected DLL does not depend on the target process finding `libgcc_s_dw2-1.dll`, `libstdc++-6.dll`, or `libwinpthread-1.dll`.

## Minimal runtime workflow

Start the local game and enter a playable world. Then run the external console from a Windows terminal:

```powershell
cd C:\path\to\pwnie-island-research\build
.\pwnie_external.exe
```

At the `pwnie>` prompt, enter:

```text
read-sprint
inject C:\path\to\pwnie-island-research\build\pwnie_research.dll
status
player
position
movement
call-internal-sprint
```

Continue with modification experiments only when `status` reports all of the following:

```text
Hook ready: 1
Initialization error: 0
Signatures verified: 1
Tick hook ready: 1
Sprint hook ready: 1
```

Reversible teleportation check:

```text
position
teleport-up 100
position
teleport <original-x> <original-y> <original-z>
position
```

Reversible flight check:

```text
movement
fly status
fly on
fly status
fly off
fly status
```

Always unload cleanly when finished:

```text
hook-sprint off
fly off
shutdown-dll
status
read-sprint
quit
```

After unloading, `status` should display `Injected DLL state: not available`, and the sprint constant should still be `3`. The `End` key can also request DLL cleanup and unloading, although the console commands make restoration easier to observe.

## Main commands

```text
status
read-sprint
write-sprint <float>
inject <path-to-pwnie_research.dll>
player
capture-player
position
teleport <x> <y> <z>
teleport-up <delta>
movement
fly status|on|off
fly speed <10..20000>
fly hold <0.2..999999>
fly walk <0..20000>
call-internal-sprint
hook-sprint on <(0,1000]>
hook-sprint off
shutdown-dll
```

Enter `help` to list every command, including the scanner commands. The legacy `set-coords` and `clear-coords` commands are deprecated. Position changes no longer depend on a guessed coordinate offset inside `Player`; they call a verified `Actor` method on the tick thread.

## Key RVAs and layouts

| Symbol or field | RVA or offset |
| --- | ---: |
| `Actor::GetPosition` | `0x16F0` |
| `Actor::SetPosition` | `0x1C80` |
| `Player::GetSprintMultiplier` | `0x13940` |
| Sprint constant | `0x78B34` |
| `Player::IsLocalPlayer` | `0x4FEF0` |
| `Player::Tick` | `0x50730` |
| `Player::CanJump` | `0x51680` |
| Complete `Player*` to `IPlayer*` | `+0x70` |
| Health | `Player + 0x30` |
| Walking speed | `Player + 0x190` |
| Jump speed | `Player + 0x194` |
| Jump hold time | `Player + 0x198` |

## Important limitations

- Addresses, fields, and function signatures apply only to the target hashes listed above. ASLR changes the module base on every run.
- Flight is an experiment using `CanJump` and movement parameters. It is not noclip and does not establish any conclusion about server-side permissions or validation.
- `CanJump` remains a five-byte global code patch protected by signature gating, read-back checks, and restoration logic. The write is not instruction-level atomic and may briefly affect other `Player` instances in the same process. Enable it only for short local educational sessions and confirm that the patch state is `0` before finishing.
- Teleportation and flight validation prove only that the local client call chain and state changed. They do not claim to bypass network or server checks.
- Do not use this framework against processes you do not own, in online matches, or on systems where you lack authorization.
- Record original values before making changes. Restore them with `fly off`, the original coordinates, and `shutdown-dll`.

For detailed principles, error codes, complete reproduction steps, and measured output, see [FINAL_TUTORIAL_ZH.md](FINAL_TUTORIAL_ZH.md) and [VERIFICATION_20260812.md](VERIFICATION_20260812.md).
