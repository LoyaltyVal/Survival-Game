# Survival Game - Phase 2: Playable Alpha

Custom C++ engine for an open-world survival game (gather, craft, build, PvE + PvP).

## What's in

- **Main menu → server list → game.** The list comes from `servers.txt` (created on first run with two
  servers). It pings each server and shows players and ping. Every listed server on your own PC
  (127.0.0.1) is **hosted automatically** when the game starts and keeps running until you quit, so the
  worlds are always online and friends on your network can join.
- **Resources**: trees (wood), stone, metal ore and sulfur rocks on the hills, hemp (cloth), berry bushes,
  mushrooms. Hit nodes with tools; pick plants with E. Nodes run out and respawn.
- **Inventory**: 30 slots + 6 hotbar, stacks, drag and drop, right-click quick move, shift-drag to split,
  drag outside the window to drop. Storage boxes (18 slots) keep loot safe when you die.
- **Crafting**: 15 recipes with a craft queue; guns and rifle ammo need a workbench nearby.
- **Building**: snap-grid foundations, walls, doorways, floors and stairs (Building Plan, R to change piece).
  Doors lock to their owner. A hammer upgrades pieces: twig → wood → stone → metal.
- **Combat**: rock, hatchet, pickaxe, pistol, hunting rifle. Hitscan guns, headshots ×2, server-side lag
  compensation. Death drops a loot bag; respawn on a beach or at your sleeping bag.
- **Survival stats**: health, food, saturation (a buffer that drains before food), stamina (sprinting and
  jumping cost it; starving halves regen). Well fed = slow healing; starving = slow death.
- **Saving**: each server saves its world to `world_<port>.sav` every 30 s, whenever a player leaves, and on
  shutdown: buildings, doors, boxes and their contents, loot bags, used-up resources (with respawn timers),
  and every player's position, health, food, inventory and unfinished crafts (by name until Steam login).
- **Graphics**: sun shadows, procedural sky with sun and drifting clouds, see-through animated water with
  reflections, low-poly broadleaf and pine trees, lumpy rocks with ore veins and moss, wind-blown grass and
  leaves, wood-plank / stone-brick / metal-panel / see-through twig building tiers, modelled tools and guns
  with first-person arms, players with walk animations, and 3D item icons in the inventory.

- **Ranks and commands**: Owner > Admin > Moderator > Player, saved per world. Whoever hosts a world owns it
  (on a dedicated server: `--owner Name`, otherwise the first player to join). Type `/help` in chat (Enter).
  Everyone: `/help [topic] /players /msg /r /pos /time /rules /kill`. Moderators: `/kick /mute /unmute /tp
  /announce`. Admins: `/ban /unban /tphere /give <item> [amount] [player] /items /heal /god /settime /rank /save`.
  Press **P** for the player list with one-click staff actions.
- **Day and night**: a 40-minute cycle with sunsets, moonlight, stars; `/settime day|night|<hour>`.
- **Looks**: smooth shading, leafy broadleaf trees and needle pines (leaf-shaped shadows), lumpy mossy rocks,
  ground textured per pixel (grass, dirt, sand, wet sand, layered rock, snow) with surface relief, curved
  wind-blown grass, shore foam and depth-coloured water, rounded characters.

## Playing with friends

- **Same Wi-Fi / home network**: they open the game, type your home address shown under "Invite friends" in the
  server list (e.g. `192.168.1.23:27015`) into Direct connect, and press Connect.
- **Over the internet**: when the game starts it asks your router to forward its ports (UPnP). If that works the
  server list shows your public address, e.g. `203.0.113.7:27015`; your friend types that into Direct connect.
- If your router says no (UPnP off), either forward **UDP 27015 and 27016** to your PC in the router settings, or
  install **Tailscale** (free) on both PCs and have your friend connect to your Tailscale IP.
- The first time you host, Windows Firewall asks about `sg_client.exe`: tick **Private** (and Public if your
  network is set to public) and click **Allow access**, or friends can't connect.
- `--upnp off` disables the automatic router step.

## Build

You need CMake 3.20+ and a C++20 compiler. GLFW and Dear ImGui download automatically.

**Windows** (Visual Studio 2022 or Build Tools, "Desktop development with C++"):

```
cmake -S . -B build
cmake --build build --config Release
.\build\Release\sg_client.exe
```

**Linux**: use the launcher. It installs what's missing (asks first), builds once, then starts the game:

```
bash play-linux.sh              # play (double-clicking it works too)
bash play-linux.sh shortcut     # add "Survival Game" to the app menu and desktop
bash play-linux.sh server --port 27015 --name "My Server" --owner YourName
```

Saves and settings on Linux live in `~/.local/share/survival-game`. Supported out of the box: Ubuntu/Debian/Mint,
Fedora, Arch/Manjaro, openSUSE.

**Sending the game to a friend on Linux**: on Windows run `.\make-linux-zip.ps1`; it creates
`survival-game-linux-source.zip`. They unzip it and run `bash play-linux.sh`.
On a Linux PC, `bash package-linux.sh` makes a ready-to-play `dist/survival-game-linux-x86_64.tar.gz` that needs no
compiler. If the project is on GitHub, `.github/workflows/build.yml` builds Windows and Linux downloads on every push.

## Run

| Command | What it does |
| --- | --- |
| `sg_client` | Main menu and server list |
| `sg_client --connect 1.2.3.4:27015 --name Ramiz` | Skip the menu and join a server |
| `sg_server --port 27015 --name "Balanced #1" --seed 1337 --owner Ramiz` | Dedicated server (saves to `world_27015.sav`) |
| `sg_bot 127.0.0.1 27015 50 30` | 50 bots walk around for 30 s (load test) |
| `sg_tests` or `ctest --test-dir build` | Unit + network tests |

To play with friends: run `sg_server` (or host from the menu), forward UDP port 27015, and add the
server to their `servers.txt` as `My Server | <your ip> | 27015 | 1337`.

## Controls

WASD move · Shift sprint · Space jump · mouse look · Left click hit / gather / shoot / eat / build ·
1-6 or wheel hotbar · Tab inventory & crafting · E open doors and boxes, pick plants ·
R change building piece · Enter chat · V camera · Esc menu.

**First 10 minutes**: hit trees with your rock → craft a Building Plan (20 wood) → place a foundation,
walls and a doorway → craft a Wooden Door and a Sleeping Bag (hemp → cloth) → craft a Hammer and upgrade
to wood. Metal ore + wood smelts into fragments for a workbench, then guns.

## Layout

```
shared/      used by client AND server, so they can never disagree
  items.*        item stats, recipes, inventory operations
  world.*        resource nodes, building pieces, collision, building rules
  movement.*     player physics + stamina; server runs it for truth, client for prediction
  protocol.*     packet format + reliable message ids (strict decoding)
  reliable.*     reliable ordered messages over UDP (inventory, building, chat)
  bytes.*        byte streams for packets and save files
  net.*          non-blocking UDP for Windows and Linux
  net_client.*   handshake, prediction, reconciliation, interpolation, game state
  terrain.*      deterministic island heightmap
server/      authoritative server: all game rules, saving
client/      main.cpp (loop, input), ui.cpp (menus, HUD, inventory), renderer.* (shadows, sky, water),
             meshes.* (procedural models), gl.*
tests/       tests.cpp (173 checks), bot.cpp
```

## Verified

- 152 automated checks pass, including: a world surviving a server restart (buildings, inventory,
  unfinished crafts); a real client chopping a tree over UDP, crafting, placing a
  foundation, eating, starving and respawning; reliable delivery of 500 messages with 30% packet loss
  and reordering; doorway/door collision; prediction error 0.0000 m; speed hack capped at sprint speed.
- Builds with zero warnings (MSVC /W4).

## Next

1. Tool cupboard (building privilege) and team system (up to 10)
2. Raid windows, offline protection, new-player shield, PvE coast zones
3. Animals (food from hunting), thirst, furnace with smelting time
4. Bow and arrows (projectiles), reloading and magazines
5. Stability for building pieces; terrain LOD chunks; real art
