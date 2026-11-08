# Crysis 3 Remastered Co-op

Co-op for the Crysis 3 Remastered campaign (Steam), 2–4 players. The host plays the campaign as a listen server;
friends join through Steam invites or "Join Game", no port forwarding.

## Build

Requires Visual Studio 2022 (x64) and Python 3.

```
build.bat                                  build\c3coop.dll, build\c3coop_launch.exe
python scripts\build_pak.py [--game DIR]   build\c3coop.pak (reads the game's paks)
python scripts\make_dist.py --zip          dist\c3coop.zip
```

## Run

Steam launch options of Crysis 3 Remastered:

```
"<path>\c3coop_launch.exe" %command%
```

Host and clients must use the same build. `scripts\c3ctl.py` controls a running game (console, second local
instance for testing).

## Third-party

[MinHook](https://github.com/TsudaKageyu/minhook) (BSD 2-Clause), `third_party/minhook`.
