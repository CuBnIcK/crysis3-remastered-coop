# Crysis 3 Remastered Co-op

Co-op for the Crysis 3 Remastered campaign (Steam), 2–4 players. The host plays the campaign as a listen server;
friends join through Steam invites or "Join Game", the game traffic is relayed through Steam (no port forwarding).

## How it works

The remaster has no co-op. The mod starts a campaign level as a network game (`map <level> s`) and fixes everything
that breaks because of it. Three layers:

1. **c3coop.dll** (`src/c3coop`), injected by the launcher. Byte patches and MinHook hooks in `Crysis3Remastered.exe`:
   the engine and the game code take their multiplayer branches as soon as `gEnv->bMultiplayer` is set, and most
   fixes put one specific check back on its single-player branch.
2. **c3coop.pak** (`scripts/build_pak.py`), mounted over the game data. SP versions of data that has `GAME="MP"`
   variants, patched entity and gamerules Lua, and the co-op Lua (`Scripts/c3coop/*.lua`): trigger lockstep, gates,
   respawn, token sync. Part of it is generated from each level's flowgraphs.
3. **Steam** (`steam.cpp`): lobby, invites, rich presence join, and a UDP relay over `ISteamNetworkingMessages`
   between the game's own sockets on 127.0.0.1.

`bMultiplayer` is set on every machine, `bServer` only on the host. AI, physics authority and the gamerules run on the
host; every machine runs the whole level flowgraph and its own local player.

## Layout

| Path | What |
|---|---|
| `src/c3coop/dllmain.cpp` | DLL entry, console unlock, named pipe `\\.\pipe\c3coop_<pid>`, main-thread command queue, log, crash dumps, data pak mount |
| `src/c3coop/sp.cpp` | Single-player behaviour restored in network games: one `SP_PATCH` per check, grouped into named fixes (`!sp status`) |
| `src/c3coop/hud.cpp` | Story HUD, mission objectives, cutscenes, checkpoints in network games (`!hud status`) |
| `src/c3coop/coop.cpp` | Co-op rules on the C++ side, the `c3coop ...` console verbs, server ↔ client text channel, held objects, threat meter |
| `src/c3coop/fgmirror.cpp` | Flowgraph output mirror: outputs that fire on one machine only (AI nodes on the server, hacking on a client) |
| `src/c3coop/anim.cpp` | Movement and animation of network-driven actors (remote players, AI on clients) |
| `src/c3coop/steam.cpp` | Lobby, invites, joining, relay (`!steam status`) |
| `src/c3coop/game.h` | Addresses, offsets and signatures shared by the modules |
| `src/launcher/launcher.cpp` | `c3coop_launch.exe`: Steam launch option wrapper, starts the game and injects the DLL |
| `scripts/build_pak.py` | Builds `c3coop.pak` from the installed game paks; holds the per-level co-op tables (gates, zones, rooms) |
| `scripts/make_dist.py` | Package for friends (`dist/`, with a README in Russian) |
| `scripts/c3ctl.py` | Control a running game through the pipe; host and a second local client for testing |

## Build and run

Visual Studio 2022 (x64) and Python 3. The game folder is found in the Steam libraries (`C3_GAME_DIR` overrides it).

```
build.bat                            build\c3coop.dll, build\c3coop_launch.exe
python scripts\build_pak.py          build\c3coop.pak
python scripts\make_dist.py --zip    dist\c3coop.zip
```

Steam launch options: `"<path>\c3coop_launch.exe" %command%`. The launcher adds `-devmode` (without it the game died
during startup on the test install, also without the mod; it also turns the single-instance block into a prompt)
and `+g_skipIntro 1`. The DLL and
pak are copied to `inject\` next to the launcher before injection, so `build\` stays writable while the game runs.

**Host and clients must run the same build.** Trigger classes, script RMIs and the network policy are not
negotiated: a different pak on one machine means kicks ("RMI ... not found") or silently diverging scripts.

Testing on one PC: `python scripts\c3ctl.py host jailbreak`, then
`python scripts\c3ctl.py client connect=127.0.0.1:64087` starts a second instance (directly, with `SteamAppId` set,
`-devmode`, the "Too many apps" prompt answered automatically) and connects it to the host's listen port.

## Debugging

- **Logs**: `logs\<pid>.log` next to the launcher (`local\logs\` for games started by `c3ctl.py`). Every console line
  plus the DLL's own lines (`[c3coop] ...`). Lua errors are not logged by the game: co-op Lua runs through
  `C3CoopCall` (pcall), which logs them.
- **Pipe**: `python scripts\c3ctl.py exec "<command>" --pid <pid>`. Console lines, `!` commands (`!sp`, `!hud`,
  `!anim`, `!steam`, `!fgmirror`, `!ping`, `!genv`), and `c3coop lua <code>` for Lua on that machine.
- **Clients from the host**: `c3coop send <channel|all> <command>` runs `c3coop <command>` on a client, e.g.
  `c3coop send all lua <code>`. A client can answer through `C3CoopToServer("...")`, which the host logs as
  `message from PlayerN`. Channels change when a player rejoins: send to `all`, or look the channel up first.
- **Live fixes**: Lua functions can be redefined on a running game with `c3coop lua`, on the host and on clients.
  This is the fastest way to test a Lua change; C++ changes need a restart.
- **Hot toggles**: every byte patch can be switched at runtime (`!sp <fix> off`) for A/B tests.

## Adding a fix

Most bugs are a single-player feature that checks `gEnv->bMultiplayer` (byte at VA `0x39450501`, read as
`cmp byte ptr [rip+X],0`), `IsServer` / `IsClient`, or "is this the local player".

1. Find the feature in the Crysis 3 SDK source (GameDll / CryEngine). It is older than the remaster: use it as a map
   and confirm every branch in the exe.
2. Find the function in `Crysis3Remastered.exe`: fixed base `0x37000000`, build TimeDateStamp `0x62FD4D1B`.
   Unique strings and their cross-references are the usual way in.
3. Add an `SP_PATCH` in `sp.cpp`: RVA, the original bytes as signature (unique in the file, long enough to fail on
   another build), the offset and the replacement. Add it to a fix in `g_fixes`. A signature mismatch disables
   the patch and logs it instead of writing anything.
4. If the problem is data or Lua, change `build_pak.py`; each patch of a game file there fails the build when its
   anchor text is not found.

## Things learned the hard way

- **Pak priority.** The data pak must be mounted above `Patch/patch*.pak`. The `OpenPackCommon` hook has to be in
  place before the mount, otherwise fast machines get the game's scripts instead of the mod's (different RMIs, kicks).
- **MinHook enable/disable suspends all threads.** Never toggle a hook per call; keep hooks enabled and switch
  behaviour with a flag (toggling per Lua call froze the game).
- **Sounds.** The MP Mannequin `playMode` tag and `CSoundSystem`'s `_mp` rename point to FMOD banks the remaster does
  not ship: weapons were silent until both were patched (`playmode`, `mpsound`).
- **Remote players on the server** are created without `ENTITY_FLAG_TRIGGER_AREAS`: they never entered an area
  trigger. The co-op tick sets the flag.
- **Death disables the player's AI object** and a revive never enables it again (SP reloads a checkpoint): the AI
  ignored revived players until the revive sent `AIEVENT_ENABLE`.
- **Inventory between levels** is restored from a store that is empty in network games. `Game.Global.Previous_Level`
  is cleared at level start so the start graphs give the level's equipment pack.
- **Late joiners** stayed black: start graphs fade out on `Game:Start` and only the intro fades in. The host sends
  the fade-in outputs (found per level by `build_pak.py`).
- **Flowgraph node numbers** at runtime are the node's index in the graph without comment nodes, not the XML id.
- **Pick and throw** on the host's copy of a remote player does not stick (its current item follows the client), so
  the host carries objects for remote players; the client still holds them in its own hands.
- **Moving rigid bodies do not carry players** in MP physics (`ridephys`): the Fields train ride froze its riders.
- **`Entity:SetScale` from Lua** is a no-op in network games.

## Co-op scripting (Lua, `build_pak.py`)

- **Triggers.** Each local trigger of a level is classified from the flowgraph: *cosmetic* (local effects only, each
  machine for its own player), *gameplay* (the server decides and sends the event to the clients), *kill* (the server
  kills the player who entered). Events travel as `kind~entity~sender~count`; kinds are listed in `C3CoopTrigApply`.
- **Entity keys.** Prefab copies share names, so synced entities travel as `name@x,y,z` and resolve to the nearest
  copy (`C3CoopEntKey`, `C3CoopEntByKey`).
- **Gates** (`GATES`). One-way moments of a level (doors that close behind the players, geometry hidden behind them):
  the server holds the activation until the quorum is in the zone, then brings the others to a target and fires it.
- **Zones** (`ZONES`). Tokens like `Level.Global.PlayerPosition` drive layer streaming; the server publishes the
  zone of the rearmost player and fires the zones in between on a jump, so layers are not unloaded under anyone.
- **Tokens.** The server syncs the level's story tokens to the clients (`TOKEN_POLICY` for exceptions).

## Known issues

- The HUD of a remote client reloads now and then (cause not found).
- The ICV can be entered but not driven; the assault buggy has one seat.
- Remote players riding the Fields train are not invulnerable on the host.
- Objects held by a remote player float in front of it for the others (no third-person hold animation).
- New game builds move addresses: patches with a changed signature switch themselves off, the rest of the mod still
  loads. Check `!sp status` and the log after a game update.

## Third-party

[MinHook](https://github.com/TsudaKageyu/minhook) (BSD 2-Clause), `third_party/minhook`.
