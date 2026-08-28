# CS2 Stage 3 — ESP + Radar Source

This directory is a function-oriented extraction of the uploaded Stage 3 project.
The existing process-memory acquisition path is preserved; the Aimbot and
Triggerbot implementation and their UI/configuration/call sites have been removed.

## Retained functional groups

### 1. Memory acquisition / game layout
- `external-cheat-base/src/core/memory/memory.cpp`
- `external-cheat-base/src/core/memory/memory.hpp`
- `external-cheat-base/src/core/memory/game_layout.hpp`
- `external-cheat-base/generated/offsets.hpp`
- `external-cheat-base/generated/client_dll.hpp`
- `external-cheat-base/generated/buttons.hpp`

### 2. ESP data acquisition and rendering
- `external-cheat-base/src/features/esp.cpp`
- `external-cheat-base/src/features/esp.hpp`

The ESP module still owns process/module discovery, entity/controller/pawn
reconstruction, player/world sampling, world-to-screen projection, skeleton,
health/equipment/distance/view-angle visual data and shared game snapshots.

### 3. Local fixed-map Radar
- `external-cheat-base/src/features/local_radar/local_fixed_radar.cpp`
- `external-cheat-base/src/features/local_radar/local_fixed_radar.hpp`
- `external-cheat-base/src/core/game/fixed_map_radar.hpp`
- `external-cheat-base/src/core/game/fixed_map_catalog.hpp`
- `external-cheat-base/src/core/game/radar_player_sample.hpp`
- `external-cheat-base/src/core/game/game_snapshot.hpp`

The map images are sourced from `web-radar/public/maps/` during the build but
are staged as native runtime assets under `local-radar/maps/`. No browser
bundle or web server is required.

### 4. Renderer / application orchestration
- `external-cheat-base/src/core/renderer/`
- `external-cheat-base/src/main.cpp`
- `external-cheat-base/src/features/menu.hpp`

## Removed
- `src/features/aimbot.cpp`
- `src/features/aimbot.hpp`
- Aimbot and Triggerbot runtime configuration
- Aimbot/Triggerbot menu pages and hotkeys
- Aimbot/Triggerbot initialization/update calls
- Aimbot FOV-circle rendering
- Aimbot-only target-data sampling branches
- Bomb Timer configuration, countdown sampling and overlay rendering
- Embedded Web Radar HTTP/WebSocket service
- Public Relay publishing and Radar snapshot recording
- Web Radar JSON serialization and CivetWeb build/link dependencies

## Build structure
Open `Stage3_ESP_Radar.sln` in Visual Studio. The C++ project retains the
required native dependencies and build configuration. MSBuild copies `SDL2.dll`
and the Local Radar map assets automatically; building the browser frontend is
not part of the native build.

## Main Stage 3 data flow

```text
cs2.exe / client.dll
       |
       v
memory + generated offsets/schema
       |
       v
EntityList -> Controller -> Pawn
       |
       v
esp::updateEntities()
       |
       +-----------------------+
       |                       |
       v                       v
Enemy/world snapshots      GameSnapshot
       |                       |
       v                       v
esp::render()          Local fixed Radar
       |                       |
       +-----------+-----------+
                   v
            SDL/ImGui overlay
```
