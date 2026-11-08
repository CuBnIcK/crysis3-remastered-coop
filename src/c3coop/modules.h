#pragma once
// Interfaces between dllmain.cpp and the feature modules (hud.cpp, anim.cpp, coop.cpp, steam.cpp).
// Each module installs its own MinHook hooks (MH_Initialize has already run) and may handle '!' commands.

#include <string>

namespace c3coop {

// Logs one line to the pipe clients and local/logs/<pid>.log, prefixed with "[c3coop] ".
void Logf(const char* fmt, ...);

// Runs a console line with devmode forced on (cheat cvars), or a Lua chunk; main thread only.
void ExecuteDev(const std::string& line);
void RunLua(const std::string& code);

// Story HUD / mission objectives / flowgraph fixes for network games (hud.cpp).
void InstallHudHooks();
bool HudCommand(const std::string& cmd); // true when the command was handled
void HudTick();                          // every frame, main thread

// Smooth movement and animation of network-driven actors (anim.cpp).
void InstallAnimHooks();
bool AnimCommand(const std::string& cmd);

// Co-op rules and the server -> client message channel (coop.cpp).
void InstallCoopHooks();
void CoopCommand(const std::string& args); // console line "c3coop <args>" (also from Lua System.ExecuteCommand)
void CoopTick();                           // every frame, main thread
void NoteConnect(const std::string& line); // last "connect ..." command, repeated by "c3coop rejoin"
bool InClientSession();                    // network client with a connection / load in progress (main thread)
bool HostingSession();                     // this machine hosts a running network game (main thread)
void CancelRejoin(const char* why);        // the user hosts / leaves: no automatic reconnect any more
void ForgetNub(void* nub);                 // CNetwork::CreateNub returned this nub (dead-nub bookkeeping)
bool BlockLoadCommand(const std::string& cmd); // loadLastSave / load in a co-op session: explained, true = blocked

// Single player features (nanosuit, weapon customization, HUD...) restored in network games (sp.cpp).
void InstallSpFeatures();
bool SpCommand(const std::string& cmd); // "!sp ..." commands, true when handled

// Flowgraph output mirror: server-only node classes (AISequence:*...) -> clients (fgmirror.cpp).
void InstallFgMirror();
bool FgMirrorCommand(const std::string& cmd); // "!fgmirror ..." / "c3coop fgmirror ...", true when handled
void FgMirrorTick();                          // every frame, main thread (server: sends queued activations)
void FgMirrorReset();                         // a level is loading: forget the late-join history

// Steam lobby, invites, join requests and the Steam-relayed connection (steam.cpp).
void InstallSteam();
bool SteamCommand(const std::string& cmd); // "c3coop steam ..." / "!steam ..." handling, true when handled
void SteamTick();                          // every frame, main thread

} // namespace c3coop
