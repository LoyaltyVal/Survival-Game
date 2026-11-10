# Survival Game — Phase 1: Foundations

Custom C++ engine for an open-world survival game (gather, craft, build, PvE + PvP).
This first milestone is the networked foundation that everything else sits on:

- **Dedicated authoritative server**: 30 Hz tick, UDP, up to 100 players
- **Client**: own OpenGL renderer (no engine), first/third-person camera, 4 km island
- **Netcode**: client-side prediction, server reconciliation, 100 ms interpolation of other players
- **Anti-cheat basics**: server sanitizes every input and rate-limits inputs, so speed hacks don't work
- **Interest management**: each player only receives players within 300 m
- **Solo mode**: the client starts a local server automatically, same code path as online

## Build

You need CMake 3.20+ and a C++20 compiler. GLFW (window + input) downloads automatically.

**Windows** (Visual Studio 2022 with "Desktop development with C++"):

```
cmake -S . -B build
cmake --build build --config Release
build\Release\sg_client.exe
```

**Linux**:

```
sudo apt install build-essential cmake xorg-dev libgl1-mesa-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/sg_client
```

## Run

| Command | What it does |
| --- | --- |
| `sg_client` | Solo: starts a local server and joins it |
| `sg_server --port 27015` | Dedicated server (headless) |
| `sg_client --connect 1.2.3.4:27015 --name Ramiz` | Join a server |
| `sg_bot 127.0.0.1 27015 50 30` | 50 bots walk around for 30 s (load test) |
| `sg_tests` or `ctest --test-dir build` | Unit + network tests |

To test multiplayer on one PC: run `sg_server`, then start two `sg_client --connect 127.0.0.1`.
Over the internet, forward UDP port 27015 on the server machine.

**Controls**: WASD move · Shift sprint · Space jump · Mouse look · V first/third person · Esc free mouse.
The window title shows FPS, position, nearby players and prediction error (should stay ~0).

## Layout

```
shared/      used by client AND server, so they can never disagree
  vecmath.h      vectors, matrices, camera math
  terrain.*      deterministic island heightmap (same seed = same world, never sent over the network)
  movement.*     player physics; server runs it for truth, client for prediction
  protocol.*     packet format + strict decoding (NaN, truncation, oversize all rejected)
  net.*          non-blocking UDP for Windows and Linux
  net_client.*   handshake, prediction, reconciliation, interpolation
server/      authoritative server
client/      gl.* (our own OpenGL loader), renderer.*, main.cpp
tests/       tests.cpp, bot.cpp
```

## Verified so far

- 37 automated checks pass, including a real UDP client/server session (prediction error 0.0000 m)
  and a speed-hack test (a client sending 4× too many inputs moves no faster than sprint speed).
- 50 bots connected to one server at once, all in sync.
- The client compiles and links with zero warnings; it has not been run on a GPU yet. First run on
  your PC is the real test — send me any errors from the console.

## Next (rest of Phase 1, then Phase 2)

1. Bullet physics for rocks, trees and later buildings (terrain stays a heightfield)
2. Reliable message channel (inventory, chat) on top of UDP, or switch transport to ENet
3. Terrain chunks with LOD instead of one window around the player
4. Phase 2: gathering, inventory, crafting, health/hunger, death and respawn, SQLite saves
