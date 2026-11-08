// Co-op rules on top of the SinglePlayer gamerules in network games: player death and respawn, and the
// server -> client message channel the Lua rules use.
// The respawn loop itself is Lua (SinglePlayer.lua patch in scripts/build_pak.py); this module makes the C++ side
// it calls safe with the SinglePlayer module set.
//
// Console commands (also from Lua via System.ExecuteCommand, and from c3ctl.py exec):
//   c3coop send <channel|all> <command>   server: run "c3coop <command>" on that client / all remote clients
//   c3coop lua <code>                     run a Lua chunk (no devmode needed)
//   c3coop dev <console line>             run a console line with devmode on (cheat cvars)
//   c3coop hud <args>                     same as the pipe command "!hud <args>"
//   c3coop echo <text>                    log a line (channel test)
//   c3coop steam <args>                   same as the pipe command "!steam <args>" (steam.cpp)
//   c3coop hello                          send this machine's Steam persona name to the server as the player's nick
//   c3coop nick <name>                    use <name> instead of the Steam name (empty: back to Steam)
//   c3coop friendlyfire on|off            players hurting each other (default off)
//   c3coop rejoin [port]                  client: the host changes level, reconnect until back in the game
//   c3coop rejoin stop                    client: stop reconnecting (also: a new connect or hosting a game)
//   c3coop restartlevel                   host: restart the current level for everybody (map + rejoin)
//   c3coop hiddenitems keep|remove        co-op campaign host: keep hidden items (accessories) instead of MP cleanup
//   c3coop alivecheck on|off              Actor:AliveCheck reports a dead local player as alive (network games)
//   c3coop fgmirror <args>                same as the pipe command "!fgmirror <args>" (fgmirror.cpp); the server sends
//                                         "fgmirror a <records>" to clients
// Handed to Lua (C3CoopCommand in Scripts/c3coop/Lockstep.lua, built by scripts/build_pak.py):
//   c3coop tp [host|<player>]             catch up: teleport next to the nearest (or named) living teammate (cooldown)
//   c3coop goto <x> <y> <z> [yaw]         debug: teleport to coordinates (yaw in degrees) or `goto <entity name>`
//   c3coop pos                            log this machine's player position and yaw
//   c3coop gates                          server: log the co-op gates of the level and their state
//   c3coop trig <entity name>             log a trigger's co-op mode and state on this machine
//   c3coop quorum <n>                     server: gate quorum rule (-1 alive players - 1, 0 everybody, k players)
//   c3coop zones                          server: zone token groups (streaming), each player's zone and the rearmost
//   c3coop start                          server: release the level start gates now (do not wait for rejoins)
//   c3coop token <name>                   log a game token's value on this machine

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>

#include "MinHook.h"

#include "game.h"
#include "modules.h"

namespace {

// CGameRules::RevivePlayer (0x388B0740, found by its "CGameRules::RevivePlayer() actor=%s" log string) ends with
//   if (m_statsRecordingModule /*+0x34E0*/) { state = m_stateModule /*+0x34F0*/->GetGameState(); ... }
// SinglePlayer.xml has StandardStatsRecording but no state module -> AV on every revive.
// 0x388B0AF2 `je 0x388B0B19` (skip when no stats module) -> `jmp`: never report revives to stats recording.
constexpr uintptr_t kRvaReviveStatsCheck = 0x18B0ADA;
constexpr unsigned char kReviveStatsCheckBytes[] = {0x48, 0x83, 0xBF, 0xE0, 0x34, 0x00, 0x00, 0x00, 0x4C,
                                                    0x8B, 0xA4, 0x24, 0xD8, 0x00, 0x00, 0x00, 0x48, 0x8B,
                                                    0xB4, 0x24, 0xD0, 0x00, 0x00, 0x00, 0x74, 0x25};
constexpr size_t kReviveStatsJeOffset = 24;

// CActor::NetReviveAt (0x3852F9B0) ends with `if (gEnv->bServer) m_netPhysCounter = (m_netPhysCounter+1)&3`
// (CActor+0xBED, SDK Actor.cpp:4703); only CActor::ClRevive (never sent by RevivePlayer) tells the clients, which keep
// 0 from bind time. CNetPlayerInput::PreUpdate (0x385EDD08 -> AllowPhysicsUpdate 0x3853BE50) then drops all movement
// of a revived player whose input carries the old counter: frozen on the other machines. je -> jmp: no bump.
constexpr uintptr_t kRvaNetRevivePhysCounter = 0x152FC22;
constexpr unsigned char kNetRevivePhysCounterBytes[] = {0x80, 0x3D, 0xD7, 0x08, 0xF2, 0x00, 0x00, 0x74, 0x11,
                                                        0x0F, 0xB6, 0x83, 0xED, 0x0B, 0x00, 0x00, 0xFE, 0xC0,
                                                        0x24, 0x03, 0x88, 0x83, 0xED, 0x0B, 0x00, 0x00};
constexpr size_t kNetRevivePhysCounterJe = 7;

// CGameRulesMPSpectator::ChangeSpectatorMode (0x38A49A70, "[tlh] @ CGameRulesMPSpectator::ChangeSpectatorMode()"
// log string), client side: gets the lobby (gEnv->pNetwork vtbl+0xE8), checks it, then calls
// lobby->vtbl[0x40]() (online service, NULL offline) and dereferences the result at 0x38A49C66 -> client AV when
// its player dies (same pattern as the dog tag lookup guarded in dllmain.cpp). The site
//   0x38A49C47 mov rax,[rsi]; mov rcx,rsi; call [rax+40h]; vmovups xmm0,[rsp+60h]
// jumps to a stub that skips the dog tag count (word [rsp+0F8h] stays 0) at 0x38A49C76 when the service is NULL.
constexpr uintptr_t kRvaSpectatorServiceSite = 0x1A49C47;
constexpr unsigned char kSpectatorServiceSiteBytes[] = {0x48, 0x8B, 0x06, 0x48, 0x8B, 0xCE, 0xFF, 0x50,
                                                        0x40, 0xC5, 0xF8, 0x10, 0x44, 0x24, 0x60};
constexpr uintptr_t kRvaSpectatorServiceContinue = 0x1A49C56;
constexpr uintptr_t kRvaSpectatorServiceSkip = 0x1A49C76;

void PutAbsJmp(unsigned char* at, uintptr_t target) {
    // jmp qword [rip+0]; dq target
    const unsigned char op[] = {0xFF, 0x25, 0x00, 0x00, 0x00, 0x00};
    memcpy(at, op, sizeof(op));
    memcpy(at + sizeof(op), &target, sizeof(target));
}

void PatchSpectatorServiceCheck() {
    auto site = game::At<unsigned char>(kRvaSpectatorServiceSite);
    if (memcmp(site, kSpectatorServiceSiteBytes, sizeof(kSpectatorServiceSiteBytes)) != 0) {
        c3coop::Logf("coop: spectator online service check NOT patched (bytes differ)");
        return;
    }
    auto stub = static_cast<unsigned char*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    const unsigned char body[] = {
        0x48, 0x8B, 0x06,                   // mov rax,[rsi]
        0x48, 0x8B, 0xCE,                   // mov rcx,rsi
        0xFF, 0x50, 0x40,                   // call [rax+40h]
        0x48, 0x85, 0xC0,                   // test rax,rax
        0x74, 0x14,                         // jz skip (+20)
        0xC5, 0xF8, 0x10, 0x44, 0x24, 0x60, // vmovups xmm0,[rsp+60h]
    };
    memcpy(stub, body, sizeof(body));                                         // 20 bytes
    PutAbsJmp(stub + 20, game::Base() + kRvaSpectatorServiceContinue);        // 20..33
    PutAbsJmp(stub + 34, game::Base() + kRvaSpectatorServiceSkip);            // skip: 34..47

    DWORD old;
    VirtualProtect(site, sizeof(kSpectatorServiceSiteBytes), PAGE_EXECUTE_READWRITE, &old);
    unsigned char patch[sizeof(kSpectatorServiceSiteBytes)];
    memset(patch, 0x90, sizeof(patch));
    PutAbsJmp(patch, reinterpret_cast<uintptr_t>(stub));
    memcpy(site, patch, sizeof(patch));
    VirtualProtect(site, sizeof(kSpectatorServiceSiteBytes), old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, sizeof(patch));
    c3coop::Logf("coop: spectator online service check patched at %p (stub %p)", site, stub);
}

// ---------------------------------------------------------------- message channel
// Transport: CGameRules::ClTextMessage RMI (reliable, ordered per channel). The server calls
// CGameRules::SendTextMessage (0x388AA2E0, "CGameRules::SendTextMessage() called on client sending to other clients")
// with "c3coop:<command>"; clients receive it in CGameRules::OnTextMessage (0x388AFB80, "CGameRules::OnTextMessage(
// type=%d, msg='%s')", called only from the RMI handler 0x388CF300), where the hook queues the command for the next
// frame instead of showing it on the HUD.
constexpr uintptr_t kRvaSendTextMessage = 0x18AA2E0;
constexpr unsigned char kSendTextMessageBytes[] = {0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x70, 0x80};
constexpr uintptr_t kRvaOnTextMessage = 0x18AFB80;
constexpr unsigned char kOnTextMessageBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x60, 0x48};
// CGame* g_pGame and CGame::GetGameRules (see hud.cpp)
constexpr uintptr_t kRvaGamePtr = 0x2529938;
constexpr uintptr_t kRvaGameGetGameRules = 0x16BDB40;
constexpr unsigned char kGameGetGameRulesBytes[] = {0x48, 0x83, 0xEC, 0x28, 0x48, 0x8B, 0x49, 0x48,
                                                    0x48, 0x8B, 0x01, 0xFF, 0x90, 0x08, 0x01, 0x00, 0x00};
// ERMInvocation (CryAction IGameObject.h)
constexpr unsigned kRmiToClientChannel = 0x01;
constexpr unsigned kRmiToRemoteClients = 0x10008; // eRMI_NoLocalCalls | eRMI_ToAllClients
constexpr char kPrefix[] = "c3coop:";

using SendTextMessageFn = void(__fastcall*)(void* rules, int type, const char* msg, unsigned to, int channelId,
                                            const char* p0, const char* p1, const char* p2, const char* p3);
using OnTextMessageFn = void(__fastcall*)(void* rules, int type, const char* msg, const char* p0, const char* p1,
                                          const char* p2, const char* p3);
OnTextMessageFn g_origOnTextMessage;
bool g_sendOk = false;

std::mutex g_inboxMtx;
std::deque<std::string> g_inbox; // commands received from the server, run on the main thread

void* GameRules() {
    char* game = *game::At<char*>(kRvaGamePtr);
    if (!game || !*reinterpret_cast<void**>(game + 0x48)) // CGame::m_pFramework, NULL before CGame::Init
        return nullptr;
    return game::At<void*(__fastcall)(void*)>(kRvaGameGetGameRules)(game);
}

void __fastcall HookedOnTextMessage(void* rules, int type, const char* msg, const char* p0, const char* p1,
                                    const char* p2, const char* p3) {
    if (msg && strncmp(msg, kPrefix, sizeof(kPrefix) - 1) == 0) {
        std::lock_guard lock(g_inboxMtx);
        g_inbox.emplace_back(msg + sizeof(kPrefix) - 1);
        return;
    }
    g_origOnTextMessage(rules, type, msg, p0, p1, p2, p3);
}

void Send(const std::string& target, const std::string& command) {
    if (!*game::At<unsigned char>(game::kRvaServerFlag)) {
        c3coop::Logf("coop: send: not the server");
        return;
    }
    void* rules = GameRules();
    if (!g_sendOk || !rules) {
        c3coop::Logf("coop: send: no gamerules or SendTextMessage signature mismatch");
        return;
    }
    std::string msg = kPrefix + command;
    if (target == "all") {
        game::At<std::remove_pointer_t<SendTextMessageFn>>(kRvaSendTextMessage)(rules, 0, msg.c_str(), kRmiToRemoteClients,
                                                                              -1, nullptr, nullptr, nullptr, nullptr);
    } else {
        int channel = atoi(target.c_str());
        if (channel <= 0) {
            c3coop::Logf("coop: send: bad channel '%s'", target.c_str());
            return;
        }
        game::At<std::remove_pointer_t<SendTextMessageFn>>(kRvaSendTextMessage)(rules, 0, msg.c_str(), kRmiToClientChannel,
                                                                              channel, nullptr, nullptr, nullptr, nullptr);
    }
}

void InstallChannel() {
    g_sendOk = memcmp(game::At<unsigned char>(kRvaSendTextMessage), kSendTextMessageBytes, sizeof(kSendTextMessageBytes)) == 0 &&
               memcmp(game::At<unsigned char>(kRvaGameGetGameRules), kGameGetGameRulesBytes, sizeof(kGameGetGameRulesBytes)) == 0;
    auto onText = game::At<unsigned char>(kRvaOnTextMessage);
    bool hooked = memcmp(onText, kOnTextMessageBytes, sizeof(kOnTextMessageBytes)) == 0 &&
                  MH_CreateHook(onText, &HookedOnTextMessage, reinterpret_cast<void**>(&g_origOnTextMessage)) == MH_OK &&
                  MH_EnableHook(onText) == MH_OK;
    c3coop::Logf("coop: message channel: send %s, receive %s", g_sendOk ? "ok" : "UNAVAILABLE",
                 hooked ? "ok" : "UNAVAILABLE");
}

// Steam persona name of the signed-in user (steam_api64.dll flat API, already loaded by the game), UTF-8.
std::string SteamPersonaName() {
    HMODULE steam = GetModuleHandleA("steam_api64.dll");
    if (!steam)
        return {};
    using GetFriendsFn = void*(__cdecl*)();
    using GetPersonaNameFn = const char*(__cdecl*)(void*);
    auto getFriends = reinterpret_cast<GetFriendsFn>(GetProcAddress(steam, "SteamAPI_SteamFriends_v017"));
    auto getName = reinterpret_cast<GetPersonaNameFn>(GetProcAddress(steam, "SteamAPI_ISteamFriends_GetPersonaName"));
    void* friends = getFriends ? getFriends() : nullptr;
    const char* name = friends && getName ? getName(friends) : nullptr;
    return name ? name : "";
}

// User text (nicks, console arguments) that ends up in a console '#' Lua line: such lines silently fail with some '%'
// sequences and when they get long (notes.md "Console Lua quirks"), so '%', '"', '\' and control characters are
// dropped and the text is cut to maxBytes on a UTF-8 character boundary.
std::string CleanUserText(const std::string& text, size_t maxBytes) {
    std::string out;
    for (char ch : text) {
        auto c = static_cast<unsigned char>(ch);
        if (c < 0x20 || c == 0x7F || c == '%' || c == '"' || c == '\\')
            continue;
        out += ch;
    }
    if (out.size() > maxBytes) {
        size_t n = maxBytes;
        while (n > 0 && (static_cast<unsigned char>(out[n]) & 0xC0) == 0x80) // out[n] is the first byte cut off
            --n;
        out.resize(n);
    }
    size_t b = out.find_first_not_of(' ');
    size_t e = out.find_last_not_of(' ');
    return b == std::string::npos ? std::string() : out.substr(b, e - b + 1);
}

constexpr size_t kMaxNickBytes = 32;
constexpr size_t kMaxLuaArgBytes = 160;

// Quotes (cleaned) user text as a Lua string literal.
std::string LuaQuote(const std::string& text) {
    std::string out = "'";
    for (char c : CleanUserText(text, kMaxLuaArgBytes)) {
        if (c == '\'')
            out += '\\';
        out += c;
    }
    return out + "'";
}

// ---------------------------------------------------------------- session state
bool IsMultiplayer() {
    return *game::At<unsigned char>(game::kRvaMultiplayerFlag) != 0;
}

bool IsServer() {
    return *game::At<unsigned char>(game::kRvaServerFlag) != 0;
}

uintptr_t VtableSlot(void* obj, size_t offset) {
    return (*reinterpret_cast<uintptr_t**>(obj))[offset / sizeof(uintptr_t)];
}

char* Framework() {
    char* g = *game::At<char*>(game::kRvaGamePointer);
    return g ? *reinterpret_cast<char**>(g + game::kGameFrameworkOffset) : nullptr;
}

// 1: a game context exists (client: connecting, loading or in game), 0: none, -1: unknown (layout check failed).
// Main thread.
int GameContextState() {
    static int layoutOk = -1;
    char* fw = Framework();
    if (!fw)
        return 0; // before CGame::Init
    if (layoutOk < 0) {
        auto fn = VtableSlot(fw, game::kVtFrameworkGetClientChannel);
        layoutOk = fn == game::Base() + game::kRvaFrameworkGetClientChannel &&
                   memcmp(game::At<unsigned char>(game::kRvaFrameworkGetClientChannel), game::kFrameworkGetClientChannelBytes,
                          sizeof(game::kFrameworkGetClientChannelBytes)) == 0;
        c3coop::Logf("coop: CCryAction::GetClientChannel %s (game context / client channel checks %s)",
                     layoutOk ? "verified" : "bytes differ", layoutOk ? "on" : "OFF, old behaviour");
    }
    if (!layoutOk)
        return -1;
    return *reinterpret_cast<void**>(fw + game::kFrameworkActionGame) ? 1 : 0;
}

void* ClientChannel() {
    char* fw = Framework();
    if (GameContextState() != 1)
        return nullptr;
    return game::Virtual<void*(__fastcall*)(void*)>(fw, game::kVtFrameworkGetClientChannel)(fw);
}

// Name of the loaded level (CCryAction::GetLevelName), empty when unknown.
std::string CurrentLevelName() {
    char* fw = Framework();
    if (!fw || VtableSlot(fw, game::kVtFrameworkGetLevelName) != game::Base() + game::kRvaFrameworkGetLevelName ||
        memcmp(game::At<unsigned char>(game::kRvaFrameworkGetLevelName), game::kFrameworkGetLevelNameBytes,
               sizeof(game::kFrameworkGetLevelNameBytes)) != 0)
        return {};
    const char* name = game::Virtual<const char*(__fastcall*)(void*)>(fw, game::kVtFrameworkGetLevelName)(fw);
    return name ? name : "";
}

void* CVar(const char* name) {
    void* console = game::Console();
    return console ? game::Virtual<void*(__fastcall*)(void*, const char*)>(console, game::kVtGetCVar)(console, name)
                   : nullptr;
}

// The co-op campaign: a network game running the SinglePlayer gamerules (MP gamerules keep their MP behaviour).
bool CoopCampaign() {
    if (!IsMultiplayer())
        return false;
    static void* rulesVar = nullptr;
    if (!rulesVar)
        rulesVar = CVar("sv_gamerules");
    const char* rules =
        rulesVar ? game::Virtual<const char*(__fastcall*)(void*)>(rulesVar, game::kVtCVarGetString)(rulesVar) : nullptr;
    return rules && _stricmp(rules, "SinglePlayer") == 0;
}

// ---------------------------------------------------------------- rejoin after a level change / lost host
// Two ways in: the host's "c3coop rejoin <port>" text message before it changes the level, or (message missed, e.g.
// the client was still loading) this client's own disconnect with "nub destroyed" / timeout while it was a co-op
// client. Attempts only start while there is no game context (no connection or load in progress): `connect` first
// drops the current channel, so repeating it during a long load restarted the load forever (audit #5).
std::mutex g_rejoinMtx;
std::string g_lastConnect; // last "connect <host> [port]" line typed / sent by steam.cpp
bool g_remoteClient = false; // this machine has been a client of a remote host since that connect

struct Rejoin {
    ULONGLONG until = 0;       // attempts until then (0: no rejoin pending)
    ULONGLONG nextTry = 0;
    ULONGLONG requestedAt = 0;
    bool guessing = false;     // no port from the host: try its rotation ports in turn
    int guessFirst = 0;        // port of the first guess
    bool connectIssued = false;
    bool sawBusy = false;      // a connection / load was seen since the last attempt
    bool attemptPending = false; // the last attempt has not been seen ending yet
    ULONGLONG attemptAt = 0;
    int attempts = 0;
    std::string lastLine;      // the connect line of the last attempt
} g_rejoin;

constexpr ULONGLONG kRejoinWindowMs = 180000;    // host told us (level change)
constexpr ULONGLONG kLostHostWindowMs = 120000;  // we noticed it ourselves
constexpr ULONGLONG kRejoinFirstDelayMs = 8000;  // the host needs a few seconds to bring the next level up
constexpr ULONGLONG kRejoinRetryDelayMs = 3000;  // after an attempt ended without getting back in
constexpr ULONGLONG kRejoinAttemptMs = 20000;    // an attempt that never showed a game context
constexpr ULONGLONG kOldChannelGraceMs = 30000;  // the old connection did not drop by itself: connect anyway

// "connect <host> [port]": host and port; direct = a game server port of the host (Steam relay lines use a local
// ephemeral port and stay as they are, the host side relay follows sv_port itself).
bool ParseConnect(const std::string& line, std::string& host, int& port) {
    char h[256] = {};
    port = game::kServerPortFirst;
    if (sscanf_s(line.c_str(), "connect %255s %d", h, (unsigned)sizeof(h), &port) < 1)
        return false;
    host = h;
    return port >= game::kServerPortFirst && port <= game::kServerPortLast;
}

void ResetRejoin() {
    g_rejoin = Rejoin{};
}

std::string NextRejoinLine() {
    std::string host;
    int port = 0;
    bool direct = ParseConnect(g_lastConnect, host, port);
    if (!g_rejoin.guessing || !direct)
        return g_lastConnect;
    int guess = g_rejoin.guessFirst;
    for (int i = 0; i < g_rejoin.attempts; ++i)
        guess = game::NextServerPort(guess);
    return "connect " + host + " " + std::to_string(guess);
}

std::string g_nickOverride; // c3coop nick <name>

void SendNick() {
    {
        std::lock_guard lock(g_rejoinMtx);
        if (g_rejoin.until) {
            if (!g_rejoin.lastLine.empty())
                g_lastConnect = g_rejoin.lastLine; // the port that worked (next level change starts from it)
            c3coop::Logf("coop: rejoined the host after %d attempt(s) (%s)", g_rejoin.attempts, g_lastConnect.c_str());
            ResetRejoin();
        }
    }
    std::string nick = CleanUserText(g_nickOverride.empty() ? SteamPersonaName() : g_nickOverride, kMaxNickBytes);
    if (nick.empty())
        nick = "Player";
    c3coop::Logf("coop: nick '%s'", nick.c_str());
    c3coop::RunLua("if g_localActor then g_localActor.server:C3ToServer(" + LuaQuote("nick " + nick) + ") end");
}

// eAE_* connection events of CCryAction::OnActionEvent (client side), see game.h.
using OnActionEventFn = void(__fastcall*)(void* self, const int* ev); // SActionEvent {int event; int value; char* text}
OnActionEventFn g_origOnActionEvent;

void OnConnectionEvent(int event, int value, const char* text) {
    if (event != game::kActionEventConnectFailed && event != game::kActionEventConnected &&
        event != game::kActionEventDisconnected && event != game::kActionEventInGame)
        return;
    if (IsServer())
        return; // the host's own local client
    std::lock_guard lock(g_rejoinMtx);
    if (event == game::kActionEventConnected || event == game::kActionEventInGame) {
        if (IsMultiplayer() && !g_lastConnect.empty())
            g_remoteClient = true;
        return;
    }
    c3coop::Logf("coop: %s (cause %d, \"%s\")%s",
                 event == game::kActionEventConnectFailed ? "connecting to the server failed" : "disconnected from the server",
                 value, text ? text : "", g_rejoin.until ? ", rejoin in progress" : "");
    if (event != game::kActionEventDisconnected || g_rejoin.until)
        return;
    bool hostGone = value == game::kDisconnectNubDestroyed || value == game::kDisconnectTimeout ||
                    value == game::kDisconnectIcmpError;
    if (!g_remoteClient || !hostGone || g_lastConnect.empty()) {
        if (value == game::kDisconnectUserRequested || !hostGone)
            g_remoteClient = false; // left on purpose, kicked, version mismatch...
        return;
    }
    // the host went away without telling us (we were loading, or it changed level / crashed): try to follow it
    std::string host;
    int port = 0;
    ULONGLONG now = GetTickCount64();
    g_rejoin = Rejoin{};
    g_rejoin.until = now + kLostHostWindowMs;
    g_rejoin.requestedAt = now;
    g_rejoin.nextTry = now + kRejoinFirstDelayMs;
    g_rejoin.connectIssued = true; // the old channel is already gone
    if (ParseConnect(g_lastConnect, host, port)) {
        // a level change moves the host to the next port of its rotation (dllmain.cpp); a timeout may be anything
        g_rejoin.guessing = true;
        g_rejoin.guessFirst = value == game::kDisconnectNubDestroyed ? game::NextServerPort(port) : port;
    }
    c3coop::Logf("coop: lost the host while in a co-op session, reconnecting for %llu s (%s)",
                 (unsigned long long)(kLostHostWindowMs / 1000),
                 g_rejoin.guessing ? ("ports from " + std::to_string(g_rejoin.guessFirst)).c_str() : g_lastConnect.c_str());
}

void __fastcall HookedOnActionEvent(void* self, const int* ev) {
    g_origOnActionEvent(self, ev);
    if (ev)
        OnConnectionEvent(ev[0], ev[1], *reinterpret_cast<const char* const*>(ev + 2));
}

// Every frame: start the next reconnect attempt when nothing is connecting / loading.
void RejoinTick() {
    std::string line;
    int attempt = 0;
    {
        std::lock_guard lock(g_rejoinMtx);
        if (!g_remoteClient && IsMultiplayer() && !IsServer() && !g_lastConnect.empty() && ClientChannel())
            g_remoteClient = true;
        if (!g_rejoin.until)
            return;
        ULONGLONG now = GetTickCount64();
        if (now >= g_rejoin.until) {
            c3coop::Logf("coop: gave up reconnecting after %d attempt(s)", g_rejoin.attempts);
            ResetRejoin();
            return;
        }
        int ctx = GameContextState();
        if (ctx == 1) {
            // our attempt is connecting / loading (SendNick ends the rejoin once the level runs), or the old
            // connection is still up: it drops when the host's nub goes away, `connect` drops it after a grace time
            g_rejoin.sawBusy = true;
            if (g_rejoin.connectIssued || now - g_rejoin.requestedAt < kOldChannelGraceMs || now < g_rejoin.nextTry)
                return;
            c3coop::Logf("coop: the old connection is still up after %llu s, reconnecting anyway",
                         (unsigned long long)(kOldChannelGraceMs / 1000));
        } else if (ctx == 0 && g_rejoin.sawBusy) {
            g_rejoin.sawBusy = false;
            g_rejoin.attemptPending = false;
            g_rejoin.nextTry = (std::max)(g_rejoin.nextTry, now + kRejoinRetryDelayMs);
            if (g_rejoin.attempts)
                c3coop::Logf("coop: reconnect attempt %d ended without getting back in", g_rejoin.attempts);
            return;
        }
        if (now < g_rejoin.nextTry)
            return;
        // an attempt that has not shown a game context yet may still start; without the layout check (ctx -1) this
        // is the only spacing (old behaviour: 15 s)
        if (g_rejoin.attemptPending && now - g_rejoin.attemptAt < (ctx < 0 ? 15000 : kRejoinAttemptMs))
            return;
        line = NextRejoinLine();
        g_rejoin.lastLine = line;
        g_rejoin.connectIssued = true;
        g_rejoin.attemptPending = true;
        g_rejoin.attemptAt = now;
        attempt = ++g_rejoin.attempts;
        g_rejoin.nextTry = now + kRejoinRetryDelayMs;
    }
    c3coop::Logf("coop: reconnecting (attempt %d): %s", attempt, line.c_str());
    c3coop::ExecuteDev(line);
}

// ---------------------------------------------------------------- friendly fire
// CGameRulesSPDamageHandling::SvOnHitScaled(this, const HitInfo&) (0x38BAF170: the only function referencing both
// "OnDeadHit" and "IsInvulnerable"; SvOnHit ends in it). HitInfo: +0 shooterId, +4 targetId. In a network game
// players are hostile to each other (no teams), so co-op drops player -> other player hits on the server.
constexpr uintptr_t kRvaSvOnHitScaled = 0x1BAF170;
constexpr unsigned char kSvOnHitScaledBytes[] = {0x48, 0x89, 0x54, 0x24, 0x10, 0x48, 0x89, 0x4C,
                                                 0x24, 0x08, 0x55, 0x53, 0x56, 0x57, 0x41, 0x55};
constexpr size_t kVtEntitySystemGetEntity = 0x58; // the first call in SvOnHitScaled: GetEntity(targetId)

using SvOnHitScaledFn = bool(__fastcall*)(void* self, const unsigned char* hit);
SvOnHitScaledFn g_origSvOnHitScaled;
std::atomic<bool> g_friendlyFire{false};

void* EntityById(uint32_t id) {
    auto entitySystem = *game::At<void*>(game::kRvaEntitySystemSlot);
    if (!id || !entitySystem)
        return nullptr;
    return game::Virtual<void*(__fastcall*)(void*, uint32_t)>(entitySystem, kVtEntitySystemGetEntity)(entitySystem, id);
}

// IEntity* of class "Player" (players of every machine; AI humans are other classes)
bool IsPlayerEntityPtr(void* entity) {
    static void* playerClass = nullptr;
    auto entitySystem = *game::At<void*>(game::kRvaEntitySystemSlot);
    if (!entity || !entitySystem)
        return false;
    if (!playerClass) {
        auto registry = game::Virtual<void*(__fastcall*)(void*)>(entitySystem,
                                                                 game::kVtEntitySystemGetClassRegistry)(entitySystem);
        playerClass = game::Virtual<void*(__fastcall*)(void*, const char*)>(
            registry, game::kVtClassRegistryFindClass)(registry, "Player");
    }
    return playerClass && game::Virtual<void*(__fastcall*)(void*)>(entity, game::kVtEntityGetClass)(entity) == playerClass;
}

bool IsPlayerEntity(uint32_t id) {
    return IsPlayerEntityPtr(EntityById(id));
}

bool __fastcall HookedSvOnHitScaled(void* self, const unsigned char* hit) {
    uint32_t shooter, target;
    memcpy(&shooter, hit, 4);
    memcpy(&target, hit + 4, 4);
    if (!g_friendlyFire && shooter != target && IsPlayerEntity(shooter) && IsPlayerEntity(target))
        return false;
    return g_origSvOnHitScaled(self, hit);
}

// ---------------------------------------------------------------- friend or foe
// CActor::IsFriendlyEntity(this, entityId, bUsingAIIgnorePlayer) (0x3853AB80, found by its shape: entity lookup,
// `cmp [bMultiplayer]`, GetTeamCount() >= 2): in MP without teams only the actor itself is friendly, so teammates
// get the red enemy crosshair. A player is always a friend of a player; when both sides have AI objects (server) the SP rule
// applies: !targetAI->IsHostile(myAI, ignorePlayer) (IEntity::GetAI vtbl+0x198, IAIObject::IsHostile vtbl+0x168).
constexpr uintptr_t kRvaActorIsFriendlyEntity = 0x153AB80;
constexpr unsigned char kActorIsFriendlyEntityBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C,
                                                         0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18};
constexpr size_t kVtEntityGetAI = 0x198;
constexpr size_t kVtAIObjectIsHostile = 0x168;

using IsFriendlyEntityFn = bool(__fastcall*)(void* actor, uint32_t entityId, bool ignorePlayer);
IsFriendlyEntityFn g_origIsFriendlyEntity;

// Only player <-> player is forced friendly (audit #12): an AI actor asking about a player keeps the faction rule /
// original answer, so hostile AI does not treat players as friends.
bool __fastcall HookedIsFriendlyEntity(void* actor, uint32_t entityId, bool ignorePlayer) {
    auto self = *reinterpret_cast<void**>(static_cast<char*>(actor) + game::kActorEntityOffset);
    if (IsPlayerEntityPtr(self) && IsPlayerEntity(entityId))
        return true;
    auto entitySystem = *game::At<void*>(game::kRvaEntitySystemSlot);
    if (*game::At<unsigned char>(game::kRvaMultiplayerFlag) && entitySystem) {
        auto target = game::Virtual<void*(__fastcall*)(void*, uint32_t)>(entitySystem, kVtEntitySystemGetEntity)(entitySystem, entityId);
        using GetAIFn = void*(__fastcall*)(void*);
        void* targetAI = target ? game::Virtual<GetAIFn>(target, kVtEntityGetAI)(target) : nullptr;
        void* selfAI = self ? game::Virtual<GetAIFn>(self, kVtEntityGetAI)(self) : nullptr;
        if (targetAI && selfAI)
            return !game::Virtual<bool(__fastcall*)(void*, void*, bool)>(targetAI, kVtAIObjectIsHostile)(targetAI, selfAI,
                                                                                                         ignorePlayer);
    }
    return g_origIsFriendlyEntity(actor, entityId, ignorePlayer);
}

// ---------------------------------------------------------------- level change with clients
// CNetwork::SyncWithGame(eNGS_Shutdown) (0x37A7D630) loops until every network member is dead while all of them
// stay "suicidal". After a remote client has been connected, the listen server's nub keeps that client's channel
// forever (dead nub, no keep-alive locks, one channel left), so "map <next level>" on the host hung. CNetNub::IsDead
// (0x37B00070, member interface vtbl 0x2168ED8: [+0x78] bDead && [+0xF4] keepAliveLocks == 0 && [+0x40] == [+0x48]
// channels empty): a dead nub whose only obstacle is leftover channels counts as dead after 3 s.
constexpr uintptr_t kRvaNubIsDead = 0xB00070;
constexpr unsigned char kNubIsDeadBytes[] = {0x80, 0x79, 0x78, 0x00, 0x74, 0x16, 0x83, 0xB9,
                                             0xF4, 0x00, 0x00, 0x00, 0x00, 0x75, 0x0D};

using NubIsDeadFn = bool(__fastcall*)(char* nub);
NubIsDeadFn g_origNubIsDead;
std::mutex g_nubMtx;
std::map<void*, ULONGLONG> g_lingeringNubs; // dead nub -> first time it was seen holding channels

bool __fastcall HookedNubIsDead(char* nub) {
    if (g_origNubIsDead(nub)) {
        std::lock_guard lock(g_nubMtx);
        g_lingeringNubs.erase(nub);
        return true;
    }
    if (!nub[0x78] || *reinterpret_cast<int*>(nub + 0xF4) != 0)
        return false;
    std::lock_guard lock(g_nubMtx);
    ULONGLONG now = GetTickCount64();
    auto it = g_lingeringNubs.find(nub);
    if (it == g_lingeringNubs.end()) {
        g_lingeringNubs[nub] = now;
        return false;
    }
    if (now - it->second < 3000)
        return false;
    if (it->second != 0) {
        c3coop::Logf("coop: dead nub %p still holds channels after 3 s, letting the network shut it down", nub);
        it->second = 0; // from now on it stays dead (answering "alive" again made the shutdown wait ~90 s)
    }
    return true;
}

// ---------------------------------------------------------------- hidden weapon accessories (audit #13)
// CGameRules::ScheduleEntityRemoval (game.h): in the co-op campaign the host must not delete hidden entities (weapon
// accessories of AI weapons and inventories: picking them up is how attachments get unlocked). Visible entities and
// MP gamerules keep the original behaviour.
using ScheduleRemovalFn = void(__fastcall*)(void* rules, uint32_t entityId, float timer, bool visibility);
ScheduleRemovalFn g_origScheduleRemoval;
std::atomic<bool> g_keepHiddenItems{true}; // c3coop hiddenitems keep|remove
std::atomic<int> g_keptHiddenItems{0};

void __fastcall HookedScheduleRemoval(void* rules, uint32_t entityId, float timer, bool visibility) {
    if (g_keepHiddenItems && IsServer() && CoopCampaign()) {
        void* entity = EntityById(entityId);
        if (entity && game::Virtual<bool(__fastcall*)(void*)>(entity, game::kVtEntityIsHidden)(entity)) {
            int n = ++g_keptHiddenItems;
            if (n <= 10 || n % 100 == 0) {
                void* cls = game::Virtual<void*(__fastcall*)(void*)>(entity, game::kVtEntityGetClass)(entity);
                const char* clsName = cls ? game::Virtual<const char*(__fastcall*)(void*)>(cls, game::kVtEntityClassGetName)(cls)
                                          : nullptr;
                const char* name = game::Virtual<const char*(__fastcall*)(void*)>(entity, game::kVtEntityGetName)(entity);
                c3coop::Logf("coop: keeping hidden %s '%s' (MP cleanup would remove it in %.0f s), #%d",
                             clsName ? clsName : "?", name ? name : "?", timer, n);
            }
            return;
        }
    }
    g_origScheduleRemoval(rules, entityId, timer, visibility);
}

// ---------------------------------------------------------------- Actor:AliveCheck (audit #4)
// Story cutscenes are gated by Actor:AliveCheck fed from Actor:LocalPlayer: a host dead at that moment (8 s before the
// co-op respawn) skipped the cutscene and the story tokens after it. In network games the local player counts as
// alive for this node; AI and remote actors keep the real answer. Called from the stub at game::kRvaAliveCheckIsDeadSite
// instead of IActor::IsDead.
std::atomic<bool> g_aliveCheckFix{true}; // c3coop alivecheck on|off
std::atomic<int> g_aliveCheckOverrides{0};

bool __fastcall AliveCheckIsDead(void* actor) {
    bool dead = game::Virtual<bool(__fastcall*)(void*)>(actor, game::kVtActorIsDead)(actor);
    if (dead && g_aliveCheckFix && IsMultiplayer() && game::Virtual<bool(__fastcall*)(void*)>(actor, game::kVtActorIsClient)(actor)) {
        int n = ++g_aliveCheckOverrides;
        if (n <= 20)
            c3coop::Logf("coop: Actor:AliveCheck: dead local player reported alive (network game), #%d", n);
        return false;
    }
    return dead;
}

void PatchAliveCheck() {
    auto site = game::At<unsigned char>(game::kRvaAliveCheckIsDeadSite);
    if (memcmp(site, game::kAliveCheckIsDeadSiteBytes, sizeof(game::kAliveCheckIsDeadSiteBytes)) != 0) {
        c3coop::Logf("coop: Actor:AliveCheck NOT patched (bytes differ)");
        return;
    }
    auto stub = static_cast<unsigned char*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!stub) {
        c3coop::Logf("coop: Actor:AliveCheck NOT patched (no stub memory)");
        return;
    }
    // rsp is 16-byte aligned at the site (push rbp/rsi + sub rsp,58h), and the frame's outgoing area is free there
    unsigned char* p = stub;
    const unsigned char movRcxRax[] = {0x48, 0x8B, 0xC8}; // mov rcx,rax (the actor)
    memcpy(p, movRcxRax, sizeof(movRcxRax));
    p += sizeof(movRcxRax);
    *p++ = 0x48; // mov rax, imm64
    *p++ = 0xB8;
    auto fn = reinterpret_cast<uintptr_t>(&AliveCheckIsDead);
    memcpy(p, &fn, sizeof(fn));
    p += sizeof(fn);
    *p++ = 0xFF; // call rax
    *p++ = 0xD0;
    const unsigned char movRcxRsi[] = {0x48, 0x8B, 0x0E}; // mov rcx,[rsi] (replaced instruction)
    memcpy(p, movRcxRsi, sizeof(movRcxRsi));
    p += sizeof(movRcxRsi);
    PutAbsJmp(p, game::Base() + game::kRvaAliveCheckIsDeadContinue);

    DWORD old;
    VirtualProtect(site, sizeof(game::kAliveCheckIsDeadSiteBytes), PAGE_EXECUTE_READWRITE, &old);
    unsigned char patch[sizeof(game::kAliveCheckIsDeadSiteBytes)];
    memset(patch, 0x90, sizeof(patch));
    PutAbsJmp(patch, reinterpret_cast<uintptr_t>(stub)); // 14 of 15 bytes
    memcpy(site, patch, sizeof(patch));
    VirtualProtect(site, sizeof(game::kAliveCheckIsDeadSiteBytes), old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, sizeof(patch));
    c3coop::Logf("coop: Actor:AliveCheck: local player counts as alive in network games (site %p, stub %p)", site, stub);
}

// ---------------------------------------------------------------- held objects (pick and throw)
// CPlayer::EnterPickAndThrow (0x3860BE30) stores m_stats.pickAndThrowEntity at CPlayer+0x1238 and sets
// isInPickAndThrowMode at +0x1225. A held object hangs on the player's hand with its physics asleep, so the network
// sends nothing until it is thrown: the server asks for the held objects ("c3coop held <player ids>") and Lua
// (C3CoopHeldUpdate) streams their position to the clients.
constexpr uintptr_t kRvaEnterPickAndThrowStats = 0x160BEC0;
constexpr unsigned char kEnterPickAndThrowStatsBytes[] = {0xC7, 0x87, 0x3C, 0x12, 0x00, 0x00, 0x00, 0x00,
                                                          0x00, 0x00, 0x89, 0xB7, 0x38, 0x12, 0x00, 0x00};
constexpr size_t kPlayerPickAndThrowEntity = 0x1238;
constexpr size_t kPlayerInPickAndThrowMode = 0x1225;
// IGameFramework::GetIActorSystem (vtbl+0xD0), IActorSystem::GetActor (vtbl+0x18): Actor:AliveCheck 0x388E8850/6B
constexpr size_t kVtFrameworkGetActorSystem = 0xD0;
constexpr size_t kVtActorSystemGetActor = 0x18;
// CPlayer::RequestEnterPickAndThrow (0x3860BC90; sp.cpp "pickup" rewrites it from +7): a client's use key on a grabbable
// object that is no environmental weapon ends there. The hook asks the host to carry it for this player instead
// (Lua C3CoopClientPick). 0x38BA3230(entityId): the EnvironmentalWeapon / PossessionItem extension or NULL.
constexpr uintptr_t kRvaRequestEnterPickAndThrow = 0x160BC90;
constexpr unsigned char kRequestEnterPickAndThrowBytes[] = {0x40, 0x53, 0x57, 0x48, 0x83, 0xEC, 0x28};
constexpr uintptr_t kRvaEnvWeaponOf = 0x1BA3230;
constexpr unsigned char kEnvWeaponOfBytes[] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x8B, 0xD9};
bool g_heldLayoutOk = false;
// CPlayer::EnterPickAndThrow(this, entity, selectImmediately, forceSelect) 0x3860BE30 and ExitPickAndThrow(this,
// forceInstantDrop) 0x3860C0D0 (SDK Player.cpp:8231/8282): local state only (m_stats, PickAndThrowWeapon selected).
// Native client pickup ("c3coop nativepick on", default): the client enters it for its own player (hands, animation,
// the object in hand) and the host enters it for that player too, so the host holds it there and streams it.
constexpr uintptr_t kRvaEnterPickAndThrow = 0x160BE30;
constexpr unsigned char kEnterPickAndThrowBytes[] = {0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48,
                                                     0x89, 0x7C, 0x24, 0x20, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x60, 0x48,
                                                     0x8B, 0xF9, 0x45, 0x0F, 0xB6, 0xF1, 0x0F, 0xB6, 0x89, 0x34};
constexpr uintptr_t kRvaExitPickAndThrow = 0x160C0D0;
constexpr unsigned char kExitPickAndThrowBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x56, 0x48, 0x83, 0xEC, 0x20, 0x80, 0xB9,
                                                    0x25, 0x12, 0x00, 0x00, 0x00, 0x0F, 0xB6, 0xF2, 0x48, 0x8B, 0xD9};
constexpr size_t kVtFrameworkGetClientActor = 0x250;
bool g_pickAndThrowOk = false;
bool g_nativePick = true;
unsigned g_myHeld = 0; // client: the entity its own player holds

using RequestEnterPickAndThrowFn = void(__fastcall*)(void* player, unsigned entityId);
RequestEnterPickAndThrowFn g_origRequestEnterPickAndThrow;

const char* EntityName(unsigned id) {
    void* entities = *game::At<void*>(game::kRvaEntitySystemSlot);
    void* entity = entities && id ? game::Virtual<void*(__fastcall*)(void*, unsigned)>(entities, kVtEntitySystemGetEntity)(
                                        entities, id)
                                  : nullptr;
    return entity ? game::Virtual<const char*(__fastcall*)(void*)>(entity, game::kVtEntityGetName)(entity) : nullptr;
}

void EnterPickAndThrow(void* player, unsigned entityId) {
    game::At<void(__fastcall)(void*, unsigned, bool, bool)>(kRvaEnterPickAndThrow)(player, entityId, true, false);
}

void __fastcall HookedRequestEnterPickAndThrow(void* player, unsigned entityId) {
    if (IsMultiplayer() && !IsServer() &&
        !game::At<void*(__fastcall)(unsigned)>(kRvaEnvWeaponOf)(entityId)) {
        const char* name = EntityName(entityId);
        if (!name || !*name)
            return;
        bool native = g_nativePick && g_pickAndThrowOk;
        if (native)
            EnterPickAndThrow(player, entityId);
        // Lua takes the entity by name, the copy nearest to the local player (prefab copies share names)
        c3coop::RunLua("if C3CoopClientPick then C3CoopClientPick(" + LuaQuote(name) + (native ? ", true" : "") +
                       ") end");
        return;
    }
    g_origRequestEnterPickAndThrow(player, entityId);
}

// client, every frame: its own player's held object; dropping or throwing it is reported to the host
void MyHeldTick() {
    if (!g_pickAndThrowOk || !IsMultiplayer() || IsServer())
        return;
    char* fw = Framework();
    auto actor = fw ? static_cast<char*>(game::Virtual<void*(__fastcall*)(void*)>(fw, kVtFrameworkGetClientActor)(fw))
                    : nullptr;
    unsigned held = actor && *reinterpret_cast<unsigned char*>(actor + kPlayerInPickAndThrowMode)
                        ? *reinterpret_cast<unsigned*>(actor + kPlayerPickAndThrowEntity)
                        : 0;
    if (held == g_myHeld)
        return;
    const char* before = EntityName(g_myHeld);
    if (g_myHeld && before && *before)
        c3coop::RunLua("if C3CoopClientDrop then C3CoopClientDrop(" + LuaQuote(before) + ") end");
    g_myHeld = held;
}

void* ActorById(unsigned id);

// host: "c3coop enterpt <player raw id> <entity raw id>" (a client's native pickup) and "c3coop exitpt <player>
// <entity>" (its drop/throw, when the host's copy of that player still holds it: C3CoopPtThrow throws it)
void PickAndThrowCommand(bool enter, const std::string& rest) {
    unsigned playerId = 0, entityId = 0;
    if (!g_pickAndThrowOk || !IsServer() || sscanf_s(rest.c_str(), "%u %u", &playerId, &entityId) != 2)
        return;
    auto actor = static_cast<char*>(ActorById(playerId));
    if (!actor)
        return;
    bool holding = *reinterpret_cast<unsigned char*>(actor + kPlayerInPickAndThrowMode) != 0;
    unsigned held = *reinterpret_cast<unsigned*>(actor + kPlayerPickAndThrowEntity);
    if (enter) {
        if (!holding)
            EnterPickAndThrow(actor, entityId);
        c3coop::Logf("coop: player %u picks up %s%s", playerId, EntityName(entityId) ? EntityName(entityId) : "?",
                     holding ? " (already holding something)" : "");
    } else if (holding && held == entityId) {
        game::At<void(__fastcall)(void*, bool)>(kRvaExitPickAndThrow)(actor, true);
        const char* name = EntityName(entityId);
        if (name && *name)
            c3coop::RunLua("if C3CoopPtThrow then C3CoopPtThrow(" + std::to_string(playerId) + "," + LuaQuote(name) +
                           ") end");
    }
}

void* ActorById(unsigned id) {
    char* fw = Framework();
    if (!fw)
        return nullptr;
    void* actors = game::Virtual<void*(__fastcall*)(void*)>(fw, kVtFrameworkGetActorSystem)(fw);
    return actors ? game::Virtual<void*(__fastcall*)(void*, unsigned)>(actors, kVtActorSystemGetActor)(actors, id)
                  : nullptr;
}

// "c3coop held <player raw entity id> ...": C3CoopHeldUpdate({{player raw id, 'held entity name'}, ...}) for every
// player in pick-and-throw mode (Lua has no raw id -> entity lookup; it takes the copy of that name nearest the player)
void HeldQuery(const std::string& ids) {
    if (!g_heldLayoutOk)
        return;
    void* entities = *game::At<void*>(game::kRvaEntitySystemSlot);
    std::string out;
    const char* p = ids.c_str();
    char* end;
    for (unsigned long id; (id = strtoul(p, &end, 10)), end != p; p = end) {
        auto actor = static_cast<char*>(ActorById(static_cast<unsigned>(id)));
        if (!actor || !entities || !*reinterpret_cast<unsigned char*>(actor + kPlayerInPickAndThrowMode))
            continue;
        unsigned held = *reinterpret_cast<unsigned*>(actor + kPlayerPickAndThrowEntity);
        void* entity = held ? game::Virtual<void*(__fastcall*)(void*, uint32_t)>(entities, kVtEntitySystemGetEntity)(
                                  entities, held)
                            : nullptr;
        const char* name = entity ? game::Virtual<const char*(__fastcall*)(void*)>(entity, game::kVtEntityGetName)(entity)
                                  : nullptr;
        if (name && *name)
            out += "{" + std::to_string(id) + "," + LuaQuote(name) + "},";
    }
    c3coop::RunLua("if C3CoopHeldUpdate then C3CoopHeldUpdate({" + out + "}) end");
}

void InstallHeldObjects() {
    g_heldLayoutOk = memcmp(game::At<unsigned char>(kRvaEnterPickAndThrowStats), kEnterPickAndThrowStatsBytes,
                            sizeof(kEnterPickAndThrowStatsBytes)) == 0;
    auto request = game::At<unsigned char>(kRvaRequestEnterPickAndThrow);
    bool hooked = g_heldLayoutOk &&
                  memcmp(request, kRequestEnterPickAndThrowBytes, sizeof(kRequestEnterPickAndThrowBytes)) == 0 &&
                  memcmp(game::At<unsigned char>(kRvaEnvWeaponOf), kEnvWeaponOfBytes, sizeof(kEnvWeaponOfBytes)) == 0 &&
                  MH_CreateHook(request, &HookedRequestEnterPickAndThrow,
                                reinterpret_cast<void**>(&g_origRequestEnterPickAndThrow)) == MH_OK &&
                  MH_EnableHook(request) == MH_OK;
    g_pickAndThrowOk = g_heldLayoutOk &&
                       memcmp(game::At<unsigned char>(kRvaEnterPickAndThrow), kEnterPickAndThrowBytes,
                              sizeof(kEnterPickAndThrowBytes)) == 0 &&
                       memcmp(game::At<unsigned char>(kRvaExitPickAndThrow), kExitPickAndThrowBytes,
                              sizeof(kExitPickAndThrowBytes)) == 0;
    c3coop::Logf("coop: held objects %s, client pick requests %s, native client pickup %s",
                 g_heldLayoutOk ? "streamed" : "NOT streamed (bytes differ)", hooked ? "go to the host" : "NOT hooked",
                 g_pickAndThrowOk ? "available" : "NOT available (bytes differ)");
}

// ---------------------------------------------------------------- threat meter (AI awareness) for remote players
// The stealth bar shows CAIAwarenessToPlayerHelper at CGameAISystem (g_pGame+0x310) +0x40: +0 int level, +4 float
// actual 0..100, +0xC recalc timer, +0x28 awareness entities (turrets). Its Update (0x384D3C30, this, dt) smooths and
// notifies CHUD_AIUpdater; RecalculateAwareness (0x384D42A0) reads the AI around IGameFramework::GetClientActor. Only the
// host has AI: it runs Recalc once per remote player with GetClientActor (CCryAction 0x3760F520) answering that player
// and sends "aware <n>" to its channel; the client writes the value into its own helper and runs Update every frame.
constexpr size_t kGameAiSystem = 0x310;
constexpr size_t kAiAwarenessHelper = 0x40;
constexpr uintptr_t kRvaGetClientActor = 0x60F520;
constexpr unsigned char kGetClientActorBytes[] = {0x48, 0x8B, 0x49, 0x50, 0x48, 0x85, 0xC9, 0x0F,
                                                  0x85, 0x33, 0xCE, 0xFD, 0xFF, 0x33, 0xC0, 0xC3};
constexpr uintptr_t kRvaAwarenessUpdate = 0x14D3C30;
constexpr unsigned char kAwarenessUpdateBytes[] = {0x40, 0x57, 0x48, 0x83, 0xEC, 0x30, 0xC5, 0xFA, 0x10, 0x41, 0x0C, 0xC5};
constexpr uintptr_t kRvaAwarenessRecalc = 0x14D42A0;
constexpr unsigned char kAwarenessRecalcBytes[] = {0x40, 0x55, 0x56, 0x41, 0x54, 0x41, 0x55, 0x41,
                                                   0x56, 0x48, 0x81, 0xEC, 0xC0, 0x00, 0x00, 0x00};
bool g_awareOk = false;
thread_local void* g_clientActorOverride = nullptr;
using GetClientActorFn = void*(__fastcall*)(void* framework);
GetClientActorFn g_origGetClientActor;

void* __fastcall HookedGetClientActor(void* framework) {
    return g_clientActorOverride ? g_clientActorOverride : g_origGetClientActor(framework);
}

char* AwarenessHelper() {
    char* game = *game::At<char*>(kRvaGamePtr);
    char* ai = game ? *reinterpret_cast<char**>(game + kGameAiSystem) : nullptr;
    return ai ? ai + kAiAwarenessHelper : nullptr;
}

struct RemoteAwareness {
    alignas(16) unsigned char helper[0x40]{}; // the player's own helper state (Recalc reads the last value at +4)
    int sent = -1;
    ULONGLONG sentAt = 0;
};
std::map<unsigned, RemoteAwareness> g_remoteAware;

// host: "c3coop aware <player raw id>:<channel> ..." (Lua C3CoopAwareTick, every 0.3 s)
void AwareHost(const std::string& list) {
    char* real = AwarenessHelper();
    if (!g_awareOk || !real)
        return;
    const char* p = list.c_str();
    unsigned id, channel;
    int used;
    while (sscanf_s(p, " %u:%u%n", &id, &channel, &used) == 2) {
        p += used;
        void* actor = ActorById(id);
        if (!actor)
            continue;
        RemoteAwareness& ra = g_remoteAware[id];
        memcpy(ra.helper + 0x28, real + 0x28, 0x18); // the host's awareness entity list, read only
        g_clientActorOverride = actor;
        game::At<void(__fastcall)(void*)>(kRvaAwarenessRecalc)(ra.helper);
        g_clientActorOverride = nullptr;
        memset(ra.helper + 0x28, 0, 0x18);
        int level = static_cast<int>(*reinterpret_cast<float*>(ra.helper + 4) / 5.0f + 0.5f) * 5;
        ULONGLONG now = GetTickCount64();
        if (level != ra.sent || now - ra.sentAt > 2000) {
            ra.sent = level;
            ra.sentAt = now;
            Send(std::to_string(channel), "aware " + std::to_string(level));
        }
    }
}

std::atomic<float> g_clientAware{-1.0f};

// client, every frame: the host's value drives the local helper (its own Recalc never runs: no AI system here)
void AwareClientTick() {
    static LARGE_INTEGER last{}, freq{};
    float value = g_clientAware;
    char* helper = AwarenessHelper();
    if (!g_awareOk || value < 0 || !helper || !IsMultiplayer() || IsServer())
        return;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!freq.QuadPart)
        QueryPerformanceFrequency(&freq);
    float dt = last.QuadPart ? static_cast<float>(now.QuadPart - last.QuadPart) / freq.QuadPart : 0.0f;
    last = now;
    *reinterpret_cast<float*>(helper + 4) = value;
    *reinterpret_cast<float*>(helper + 0xC) = 1e9f;
    game::At<void(__fastcall)(void*, float)>(kRvaAwarenessUpdate)(helper, dt > 0.1f ? 0.1f : dt);
}

void InstallAwareness() {
    auto get = game::At<unsigned char>(kRvaGetClientActor);
    g_awareOk = memcmp(game::At<unsigned char>(kRvaAwarenessUpdate), kAwarenessUpdateBytes, sizeof(kAwarenessUpdateBytes)) == 0 &&
                memcmp(game::At<unsigned char>(kRvaAwarenessRecalc), kAwarenessRecalcBytes, sizeof(kAwarenessRecalcBytes)) == 0 &&
                memcmp(get, kGetClientActorBytes, sizeof(kGetClientActorBytes)) == 0 &&
                MH_CreateHook(get, &HookedGetClientActor, reinterpret_cast<void**>(&g_origGetClientActor)) == MH_OK &&
                MH_EnableHook(get) == MH_OK;
    c3coop::Logf("coop: threat meter for remote players %s", g_awareOk ? "on" : "OFF (bytes differ)");
}

// ---------------------------------------------------------------- HUD notice
// Short message for the local player, drawn by the Lua C3CoopHudDraw (Lockstep.lua) like the gate messages; only
// plain ASCII text from this file (no user text).
void HudNotice(const char* text, int seconds) {
    c3coop::Logf("coop: notice: %s", text);
    c3coop::RunLua(std::string("C3COOP_HUD_TEXT=") + LuaQuote(text) + " C3COOP_HUD_UNTIL=System.GetCurrTime()+" +
                   std::to_string(seconds));
}

// ---------------------------------------------------------------- engine warnings into the log
// A remote client was kicked by net errors whose details (object names) only go to NetWarning, which is off. Both
// warning functions are copied into the DLL log (first 400 lines of each, from any thread).
using WarningFn = void(__cdecl*)(const char* fmt, ...);
WarningFn g_origNetWarning;
WarningFn g_origGameWarning;
std::atomic<int> g_netWarnings{0};
std::atomic<int> g_gameWarnings{0};

void LogWarning(const char* kind, std::atomic<int>& count, const char* fmt, va_list args) {
    int n = ++count;
    if (!fmt || n > 400)
        return;
    char text[1024];
    vsnprintf(text, sizeof(text), fmt, args);
    c3coop::Logf("%s warning: %s%s", kind, text, n == 400 ? " (no more of these)" : "");
}

void __cdecl HookedNetWarning(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    LogWarning("net", g_netWarnings, fmt, args);
    va_end(args);
    // the original prints only with its log cvar on; it gets the formatted text
    va_start(args, fmt);
    char text[1024];
    vsnprintf(text, sizeof(text), fmt ? fmt : "", args);
    va_end(args);
    g_origNetWarning("%s", text);
}

void __cdecl HookedGameWarning(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    LogWarning("game", g_gameWarnings, fmt, args);
    va_end(args);
    va_start(args, fmt);
    char text[1024];
    vsnprintf(text, sizeof(text), fmt ? fmt : "", args);
    va_end(args);
    if (fmt)
        g_origGameWarning("%s", text);
}

void HookWarning(uintptr_t rva, const unsigned char* bytes, size_t size, void* detour, WarningFn* orig,
                 const char* name) {
    auto fn = game::At<unsigned char>(rva);
    if (memcmp(fn, bytes, size) == 0 && MH_CreateHook(fn, detour, reinterpret_cast<void**>(orig)) == MH_OK &&
        MH_EnableHook(fn) == MH_OK)
        c3coop::Logf("coop: %s copied into this log (%p)", name, fn);
    else
        c3coop::Logf("coop: %s hook NOT installed", name);
}

bool PatchByte(unsigned char* at, unsigned char value) {
    DWORD old;
    if (!VirtualProtect(at, 1, PAGE_EXECUTE_READWRITE, &old))
        return false;
    *at = value;
    VirtualProtect(at, 1, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, 1);
    return true;
}

} // namespace

namespace c3coop {

void InstallCoopHooks() {
    auto check = game::At<unsigned char>(kRvaReviveStatsCheck);
    if (memcmp(check, kReviveStatsCheckBytes, sizeof(kReviveStatsCheckBytes)) == 0 &&
        PatchByte(check + kReviveStatsJeOffset, 0xEB))
        Logf("coop: RevivePlayer skips stats recording at %p", check + kReviveStatsJeOffset);
    else
        Logf("coop: RevivePlayer stats patch NOT applied (bytes differ)");
    auto physCounter = game::At<unsigned char>(kRvaNetRevivePhysCounter);
    if (memcmp(physCounter, kNetRevivePhysCounterBytes, sizeof(kNetRevivePhysCounterBytes)) == 0 &&
        PatchByte(physCounter + kNetRevivePhysCounterJe, 0xEB))
        Logf("coop: revives keep the net physics counter (NetReviveAt %p)", physCounter + kNetRevivePhysCounterJe);
    else
        Logf("coop: NetReviveAt physics counter patch NOT applied (bytes differ)");
    PatchSpectatorServiceCheck();
    InstallChannel();

    auto onHit = game::At<unsigned char>(kRvaSvOnHitScaled);
    if (memcmp(onHit, kSvOnHitScaledBytes, sizeof(kSvOnHitScaledBytes)) == 0 &&
        MH_CreateHook(onHit, &HookedSvOnHitScaled, reinterpret_cast<void**>(&g_origSvOnHitScaled)) == MH_OK &&
        MH_EnableHook(onHit) == MH_OK)
        Logf("coop: no friendly fire between players (SvOnHitScaled %p)", onHit);
    else
        Logf("coop: friendly fire hook NOT installed");

    auto nubIsDead = game::At<unsigned char>(kRvaNubIsDead);
    if (memcmp(nubIsDead, kNubIsDeadBytes, sizeof(kNubIsDeadBytes)) == 0 &&
        MH_CreateHook(nubIsDead, &HookedNubIsDead, reinterpret_cast<void**>(&g_origNubIsDead)) == MH_OK &&
        MH_EnableHook(nubIsDead) == MH_OK)
        Logf("coop: level changes with connected clients unblocked (CNetNub::IsDead %p)", nubIsDead);
    else
        Logf("coop: CNetNub::IsDead hook NOT installed");

    auto isFriendly = game::At<unsigned char>(kRvaActorIsFriendlyEntity);
    if (memcmp(isFriendly, kActorIsFriendlyEntityBytes, sizeof(kActorIsFriendlyEntityBytes)) == 0 &&
        MH_CreateHook(isFriendly, &HookedIsFriendlyEntity, reinterpret_cast<void**>(&g_origIsFriendlyEntity)) == MH_OK &&
        MH_EnableHook(isFriendly) == MH_OK)
        Logf("coop: players are friends, AI by faction (IsFriendlyEntity %p)", isFriendly);
    else
        Logf("coop: IsFriendlyEntity hook NOT installed");

    // hud.cpp checked these bytes before (it calls the function for fake checkpoint saves)
    auto onActionEvent = game::At<unsigned char>(game::kRvaCryActionOnActionEvent);
    if (memcmp(onActionEvent, game::kCryActionOnActionEventBytes, sizeof(game::kCryActionOnActionEventBytes)) == 0 &&
        MH_CreateHook(onActionEvent, &HookedOnActionEvent, reinterpret_cast<void**>(&g_origOnActionEvent)) == MH_OK &&
        MH_EnableHook(onActionEvent) == MH_OK)
        Logf("coop: connection events watched (CCryAction::OnActionEvent %p): a client that loses its host rejoins",
             onActionEvent);
    else
        Logf("coop: CCryAction::OnActionEvent hook NOT installed: clients only rejoin on the host's message");

    auto removal = game::At<unsigned char>(game::kRvaScheduleEntityRemoval);
    if (memcmp(removal, game::kScheduleEntityRemovalBytes, sizeof(game::kScheduleEntityRemovalBytes)) == 0 &&
        MH_CreateHook(removal, &HookedScheduleRemoval, reinterpret_cast<void**>(&g_origScheduleRemoval)) == MH_OK &&
        MH_EnableHook(removal) == MH_OK)
        Logf("coop: hidden items are not removed in the co-op campaign (ScheduleEntityRemoval %p)", removal);
    else
        Logf("coop: ScheduleEntityRemoval hook NOT installed (bytes differ or MinHook failed)");

    PatchAliveCheck();
    InstallHeldObjects();
    InstallAwareness();

    // a friend joining a running level was kicked by "Entity 1342 for RMI RMI:CPlayer:ClSwarmerPerkComplete not found"
    int rmiPatched = 0;
    for (auto rva : game::kRvaRmiDisconnectJne) {
        auto jne = game::At<unsigned char>(rva);
        if (memcmp(jne - sizeof(game::kRmiDisconnectPrefix), game::kRmiDisconnectPrefix,
                   sizeof(game::kRmiDisconnectPrefix)) == 0 && *jne == 0x75 && PatchByte(jne, 0xEB))
            ++rmiPatched;
    }
    Logf("coop: RMI for a missing entity no longer disconnects the client (%d of %zu sites)", rmiPatched,
         std::size(game::kRvaRmiDisconnectJne));

    // next kick of the same join: "SetAspectProfile - object not found"
    auto aspect = game::At<unsigned char>(game::kRvaSetAspectProfileKick);
    DWORD old;
    if (memcmp(aspect, game::kSetAspectProfileKickBytes, sizeof(game::kSetAspectProfileKickBytes)) == 0 &&
        VirtualProtect(aspect, sizeof(game::kSetAspectProfileKickPatch), PAGE_EXECUTE_READWRITE, &old)) {
        memcpy(aspect, game::kSetAspectProfileKickPatch, sizeof(game::kSetAspectProfileKickPatch));
        VirtualProtect(aspect, sizeof(game::kSetAspectProfileKickPatch), old, &old);
        FlushInstructionCache(GetCurrentProcess(), aspect, sizeof(game::kSetAspectProfileKickPatch));
        Logf("coop: SetAspectProfile for a missing object no longer disconnects the client (%p)", aspect);
    } else {
        Logf("coop: SetAspectProfile kick NOT patched (bytes differ)");
    }

    HookWarning(game::kRvaNetWarning, game::kNetWarningBytes, sizeof(game::kNetWarningBytes),
                reinterpret_cast<void*>(&HookedNetWarning), &g_origNetWarning, "NetWarning");
    HookWarning(game::kRvaGameWarning, game::kGameWarningBytes, sizeof(game::kGameWarningBytes),
                reinterpret_cast<void*>(&HookedGameWarning), &g_origGameWarning, "GameWarning");
}

bool InClientSession() {
    if (!IsMultiplayer() || IsServer())
        return false;
    return GameContextState() != 0; // unknown layout (-1): old behaviour, every MP non-server is a client
}

bool HostingSession() {
    if (!IsMultiplayer() || !IsServer())
        return false;
    return GameContextState() != 0;
}

void CancelRejoin(const char* why) {
    std::lock_guard lock(g_rejoinMtx);
    if (g_rejoin.until)
        Logf("coop: automatic rejoin cancelled (%s)", why);
    ResetRejoin();
    g_remoteClient = false;
}

void ForgetNub(void* nub) {
    std::lock_guard lock(g_nubMtx);
    if (g_lingeringNubs.erase(nub))
        Logf("coop: new nub %p reuses the address of a dead one, its 3 s grace starts over", nub);
}

// "loadLastSave" / "load <save>" (pause menu, death screen, console) in a co-op session: the engine refuses to load
// in a network game anyway (game.h); say so instead of leaving the player wondering. True when blocked.
bool BlockLoadCommand(const std::string& cmd) {
    if (!IsMultiplayer() || !(InClientSession() || HostingSession()))
        return false;
    Logf("coop: \"%s\" blocked: saves cannot be loaded in a co-op session", cmd.c_str());
    if (IsServer())
        HudNotice("Co-op: no checkpoint loading. Host console: c3coop restartlevel", 6);
    else
        HudNotice("Co-op: no checkpoint loading. You respawn near a teammate.", 6);
    return true;
}

void CoopCommand(const std::string& args) {
    auto space = args.find(' ');
    std::string verb = args.substr(0, space);
    std::string rest = space == std::string::npos ? "" : args.substr(space + 1);
    if (verb == "send") {
        auto sp = rest.find(' ');
        if (sp == std::string::npos) {
            Logf("coop: usage: c3coop send <channel|all> <command>");
            return;
        }
        Send(rest.substr(0, sp), rest.substr(sp + 1));
        if (rest.substr(sp + 1).rfind("rejoin", 0) == 0 && *game::At<unsigned char>(game::kRvaServerFlag)) {
            // the host changes the level: the next level's start gates wait for the players coming back
            RunLua("if C3CoopLevelChange then C3CoopLevelChange() end");
        }
    } else if (verb == "lua") {
        RunLua(rest);
    } else if (verb == "dev") {
        ExecuteDev(rest);
    } else if (verb == "hud") {
        HudCommand("!hud " + rest);
    } else if (verb == "hello") {
        SendNick();
    } else if (verb == "nick") {
        g_nickOverride = CleanUserText(rest, kMaxNickBytes);
        SendNick();
    } else if (verb == "hiddenitems") {
        if (rest == "keep" || rest == "remove")
            g_keepHiddenItems = rest == "keep";
        Logf("coop: hidden items in the co-op campaign: %s (%d kept so far)", g_keepHiddenItems ? "kept" : "removed (MP)",
             g_keptHiddenItems.load());
    } else if (verb == "held") {
        HeldQuery(rest);
    } else if (verb == "enterpt" || verb == "exitpt") {
        PickAndThrowCommand(verb == "enterpt", rest);
    } else if (verb == "nativepick") {
        if (rest == "on" || rest == "off")
            g_nativePick = rest == "on";
        Logf("coop: native client pickup %s", g_nativePick ? "on" : "off (the host carries it)");
    } else if (verb == "aware") {
        if (IsServer())
            AwareHost(rest);
        else
            g_clientAware = static_cast<float>(atof(rest.c_str()));
    } else if (verb == "physcounter") {
        auto at = game::At<unsigned char>(kRvaNetRevivePhysCounter) + kNetRevivePhysCounterJe;
        if ((rest == "on" || rest == "off") && (*at == 0x74 || *at == 0xEB))
            PatchByte(at, rest == "on" ? 0xEB : 0x74);
        Logf("coop: NetReviveAt physics counter bump %s", *at == 0xEB ? "skipped (co-op fix)" : "active (stock)");
    } else if (verb == "alivecheck") {
        if (rest == "on" || rest == "off")
            g_aliveCheckFix = rest == "on";
        Logf("coop: Actor:AliveCheck local player always alive: %s (%d overrides so far)", g_aliveCheckFix ? "on" : "off",
             g_aliveCheckOverrides.load());
    } else if (verb == "restartlevel") {
        // host: restart the current level for everybody through the hosted map path (dllmain.cpp: "rejoin" to the
        // clients, next server port, map 1.5 s later)
        std::string level = CurrentLevelName();
        if (!HostingSession() || level.empty()) {
            Logf("coop: restartlevel: only the host of a running co-op level can restart it (level '%s')", level.c_str());
            return;
        }
        Logf("coop: restarting level %s for everybody", level.c_str());
        void* console = game::Console();
        game::Virtual<game::ExecuteStringFn>(console, game::kVtExecuteString)(console, ("map " + level).c_str(), false,
                                                                              false);
    } else if (verb == "friendlyfire") {
        g_friendlyFire = rest == "on";
        Logf("coop: friendly fire %s", g_friendlyFire ? "on" : "off");
    } else if (verb == "rejoin" && rest == "stop") {
        CancelRejoin("c3coop rejoin stop");
    } else if (verb == "rejoin") {
        if (IsServer()) {
            Logf("coop: rejoin ignored on the host");
            return;
        }
        std::lock_guard lock(g_rejoinMtx);
        if (g_lastConnect.empty()) {
            Logf("coop: rejoin requested, but no connect command is known");
        } else {
            // "rejoin <port>": the host's new server port; direct connections (to its game ports 64087..64094) follow
            // it, a Steam relay connection (local ephemeral port) stays as it is
            int newPort = atoi(rest.c_str());
            std::string host;
            int oldPort = 0;
            if (newPort >= game::kServerPortFirst && newPort <= game::kServerPortLast &&
                ParseConnect(g_lastConnect, host, oldPort))
                g_lastConnect = "connect " + host + " " + std::to_string(newPort);
            ULONGLONG now = GetTickCount64();
            ResetRejoin();
            g_rejoin.until = now + kRejoinWindowMs;
            g_rejoin.requestedAt = now;
            g_rejoin.nextTry = now + kRejoinFirstDelayMs;
            g_remoteClient = true;
            Logf("coop: host changes level, will reconnect with \"%s\" once the old connection is gone",
                 g_lastConnect.c_str());
        }
    } else if (verb == "steam") {
        SteamCommand("!steam " + rest);
    } else if (verb == "fgmirror") {
        FgMirrorCommand(rest.empty() ? "!fgmirror" : "!fgmirror " + rest);
    } else if (verb == "echo") {
        Logf("coop: echo %s", rest.c_str());
    } else if (!verb.empty()) {
        // tp, goto, pos, gates, zones, start, trig, token, quorum, ... (C3CoopCommand logs unknown verbs)
        Logf("coop: %s -> Lua", args.c_str());
        RunLua("if C3CoopCommand then C3CoopCommand(" + LuaQuote(args) +
               ") else Log('[c3coop] Scripts/c3coop/Lockstep.lua is not loaded') end");
    } else {
        Logf("coop: unknown command '%s'", args.c_str());
    }
}

// A connect typed by the user or sent by steam.cpp (the rejoin attempts bypass the ExecuteString hook).
void NoteConnect(const std::string& line) {
    std::lock_guard lock(g_rejoinMtx);
    if (g_rejoin.until) {
        Logf("coop: automatic rejoin cancelled (new connect command)");
        ResetRejoin();
    }
    g_lastConnect = line;
    g_remoteClient = false; // set again once this connection is up
}

// A co-op session that ended leaves gEnv->bMultiplayer set, and back in the menu the single player front end breaks
// (CCryAction::LoadGame returns early in network games: "Load game" did nothing). After 5 s in the menu (no game
// context, no rejoin pending) it goes back to single player; the next hosted map / connect sets it again.
void ResetMultiplayerInMenu() {
    static ULONGLONG menuSince = 0;
    auto mp = game::At<unsigned char>(game::kRvaMultiplayerFlag);
    bool pending;
    {
        std::lock_guard lock(g_rejoinMtx);
        pending = g_rejoin.until != 0;
    }
    if (!*mp || pending || GameContextState() != 0) { // -1: unknown layout, leave it alone
        menuSince = 0;
        return;
    }
    ULONGLONG now = GetTickCount64();
    if (!menuSince) {
        menuSince = now;
    } else if (now - menuSince >= 5000) {
        *mp = 0;
        menuSince = 0;
        Logf("coop: back in the menu after a network game: gEnv->bMultiplayer = 0 (single player load works again)");
    }
}

void CoopTick() {
    RejoinTick();
    ResetMultiplayerInMenu();
    std::deque<std::string> commands;
    {
        std::lock_guard lock(g_inboxMtx);
        commands.swap(g_inbox);
    }
    for (auto& command : commands)
        CoopCommand(command);
    AwareClientTick();
    MyHeldTick();
}

} // namespace c3coop
