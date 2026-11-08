// Steam integration: lobby, invites, joining through the friends list, and the game's UDP traffic relayed through
// Steam (no port forwarding).
//
// Everything goes through the flat API of the game's own Bin64\steam_api64.dll (interface versions of SDK ~1.55:
// SteamUser021, SteamFriends017, SteamMatchmaking009, SteamUtils010, SteamNetworkingMessages002,
// SteamNetworkingUtils004), resolved with GetProcAddress. The game initializes Steam and pumps SteamAPI_RunCallbacks
// itself; nothing here calls SteamAPI_Init or RunCallbacks. Missing DLL / exports / interfaces are logged and the
// affected feature is skipped.
//
// Hosting: when this machine runs a level as the server (gEnv->bServer and gamerules exist for 2 s) a friends-only
// lobby (4 members) is created with data c3coop=<version>, host=<SteamID64>, level=<sv_map>, and rich presence
// connect="+c3coop_join <host> <lobby>" so friends get "Join Game". The lobby is left 15 s after the server session
// ends. The host side of the relay accepts Steam networking sessions from friends and lobby members.
//
// Joining: a lobby invite or "Join Game" (GameLobbyJoinRequested_t / GameRichPresenceJoinRequested_t), a game start
// by Steam with "+connect_lobby <lobby>" or "+c3coop_join <host> [lobby]" on the command line, or `!steam join <id>`
// -> join the lobby (if any) -> host = lobby owner -> start the client relay -> `disconnect` (if a level is loaded)
// -> `connect 127.0.0.1 <relay port>`.
//
// Relay: raw game datagrams carried as unreliable ISteamNetworkingMessages (Steam Datagram Relay, channel 0).
//   client: game <-> UDP 127.0.0.1:<ephemeral> <-> Steam <-> host
//   host:   Steam <-> per remote player a UDP socket 127.0.0.1:<ephemeral> <-> game server 127.0.0.1:<sv_port>
// A worker thread moves the datagrams (select() with 1 ms timeout + polling ReceiveMessagesOnChannel); host peers
// that are silent for 60 s are closed. The loopback test transport replaces Steam by UDP 127.0.0.1:64190 with an
// 8-byte peer id in front of each datagram; the proxy code is the same.
//
// Commands ("!steam ..." on the pipe, "c3coop steam ..." in the console):
//   !steam status                       Steam state, hosting, lobby members, join state, relay peers and counters
//   !steam invite                       Steam overlay invite dialog for the lobby (connect string if no lobby)
//   !steam join <steamid64|lobbyid>     join a host (relay only) or a lobby (lobby -> owner -> relay)
//   !steam leave                        stop relay, leave lobbies, clear rich presence; no auto host until the
//                                       current server session ends
//   !steam host                         start hosting now (lobby + relay) even if no server level is detected
//   !steam autohost on|off              automatic lobby/relay for server levels (default on)
//   !steam relaytest host|join|stop     loopback transport test on one PC (UDP 127.0.0.1:64190)

#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <windows.h>
#include <shellapi.h>
#include <timeapi.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "game.h"
#include "modules.h"

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "winmm.lib")

namespace {

// ---------------------------------------------------------------- constants

constexpr char kVersion[] = "c3coop-2026.10.27"; // lobby data "c3coop": host and clients should run the same build
constexpr int kLobbyMaxMembers = 4;
constexpr int kLobbyTypeFriendsOnly = 1;      // ELobbyType
constexpr int kResultOK = 1;                  // EResult
constexpr int kFriendRelationshipFriend = 3;  // EFriendRelationship
constexpr uint32_t kChatRoomEnterSuccess = 1; // EChatRoomEnterResponse
constexpr int kIdentityTypeSteamID = 16;      // ESteamNetworkingIdentityType
constexpr int kSendUnreliableNoNagle = 1;     // k_nSteamNetworkingSend_UnreliableNoNagle
constexpr int kSendAutoRestartBrokenSession = 32;
constexpr int kRelayChannel = 0;
constexpr int kAccountTypeIndividual = 1; // SteamID64 bits 52..55
constexpr int kAccountTypeChat = 8;       // lobbies

// callback ids (k_iSteamFriendsCallbacks 300, k_iSteamMatchmakingCallbacks 500, k_iSteamNetworkingMessagesCallbacks 1250)
constexpr int kCbGameLobbyJoinRequested = 333;
constexpr int kCbGameRichPresenceJoinRequested = 337;
constexpr int kCbLobbyEnter = 504;
constexpr int kCbLobbyCreated = 513;
constexpr int kCbSessionRequest = 1251;
constexpr int kCbSessionFailed = 1252;

constexpr uint16_t kLoopbackPort = 64190;   // !steam relaytest: "fake Steam" UDP port of the host
constexpr uint64_t kLoopbackHostId = 1;     // peer id of the host in loopback mode
constexpr ULONGLONG kPeerIdleMs = 60000;    // host closes relay peers silent for this long
constexpr ULONGLONG kPeerGameDeadMs = 5000; // game server silent towards a peer for this long: its connection is gone
constexpr size_t kMaxHostPeers = 8;
constexpr ULONGLONG kHostDetectMs = 2000;   // server level must be up this long before the lobby is created
constexpr ULONGLONG kHostEndMs = 15000;     // ... and gone this long before the lobby is left (level changes)
constexpr ULONGLONG kStartupJoinDelayMs = 8000; // command line joins wait for the front end
constexpr ULONGLONG kPendingSessionMs = 15000;  // session requests wait this long for lobby membership

// CGame* g_pGame and CGame::GetGameRules (same as coop.cpp / hud.cpp)
constexpr uintptr_t kRvaGamePtr = 0x2529938;
constexpr uintptr_t kRvaGameGetGameRules = 0x16BDB40;
constexpr unsigned char kGameGetGameRulesBytes[] = {0x48, 0x83, 0xEC, 0x28, 0x48, 0x8B, 0x49, 0x48,
                                                    0x48, 0x8B, 0x01, 0xFF, 0x90, 0x08, 0x01, 0x00, 0x00};
// ICVar: GetIVal vtbl+0x10, GetString vtbl+0x28 (CCryAction connect/server code: GetCVar("sv_port") -> call
// [rdx+10h] -> word port at 0x376024B6; GetCVar("sv_map") -> call [rdx+28h] at 0x386AD30D)
constexpr size_t kVtCVarGetIVal = 0x10;
constexpr size_t kVtCVarGetString = 0x28;

// ---------------------------------------------------------------- Steamworks types (Windows x64: 8-byte packing)

using SteamAPICall_t = uint64_t;

// steam_api_common.h CCallbackBase, declared in the same order so MSVC builds the same vtable:
// [0] Run(void*, bool, SteamAPICall_t), [1] Run(void*) (MSVC puts overloads in reverse order), [2] GetCallbackSizeBytes.
class CCallbackBase {
public:
    CCallbackBase() : m_nCallbackFlags(0), m_iCallback(0) {}
    virtual void Run(void* pvParam) = 0;
    virtual void Run(void* pvParam, bool bIOFailure, SteamAPICall_t hSteamAPICall) = 0;
    int GetICallback() { return m_iCallback; }
    virtual int GetCallbackSizeBytes() = 0;

    uint8_t m_nCallbackFlags;
    int m_iCallback;
};
static_assert(sizeof(CCallbackBase) == 16 && offsetof(CCallbackBase, m_nCallbackFlags) == 8 &&
              offsetof(CCallbackBase, m_iCallback) == 12);

#pragma pack(push, 8)
struct LobbyCreated_t { // 513
    int32_t result;
    uint64_t lobby;
};
struct LobbyEnter_t { // 504
    uint64_t lobby;
    uint32_t chatPermissions;
    bool locked;
    uint32_t response;
};
struct GameLobbyJoinRequested_t { // 333 (two CSteamID)
    uint64_t lobby;
    uint64_t friendId;
};
struct GameRichPresenceJoinRequested_t { // 337
    uint64_t friendId;
    char connect[256];
};
struct SteamNetworkingIdentity {
    int32_t type;
    int32_t size;
    union {
        uint64_t steamID64;
        char raw[128];
    };
};
struct SteamNetworkingMessage_t {
    void* data;
    int size;
    uint32_t conn;
    SteamNetworkingIdentity peer;
    int64_t connUserData;
    int64_t usecTimeReceived;
    int64_t messageNumber;
    void (*freeData)(SteamNetworkingMessage_t*);
    void (*release)(SteamNetworkingMessage_t*);
    int channel;
    int flags;
    int64_t userData;
    uint16_t lane;
};
#pragma pack(pop)
static_assert(sizeof(LobbyCreated_t) == 16 && offsetof(LobbyCreated_t, lobby) == 8);
static_assert(sizeof(LobbyEnter_t) == 24 && offsetof(LobbyEnter_t, response) == 16);
static_assert(sizeof(GameLobbyJoinRequested_t) == 16);
static_assert(sizeof(GameRichPresenceJoinRequested_t) == 264);
static_assert(sizeof(SteamNetworkingIdentity) == 136 && offsetof(SteamNetworkingIdentity, steamID64) == 8);
static_assert(offsetof(SteamNetworkingMessage_t, peer) == 16 && offsetof(SteamNetworkingMessage_t, release) == 184 &&
              offsetof(SteamNetworkingMessage_t, channel) == 192 && offsetof(SteamNetworkingMessage_t, lane) == 208);
// SteamNetConnectionInfo_t (696 bytes): +0 identity, +176 state, +180 end reason, +184 char endDebug[128],
// +312 char description[128], +440 flags (16 = relayed). SteamNetConnectionRealTimeStatus_t: +0 state, +4 ping.
constexpr int kConnInfoSize = 696;
constexpr size_t kConnInfoEndReason = 180;
constexpr size_t kConnInfoEndDebug = 184;
constexpr size_t kConnInfoDescription = 312;
constexpr size_t kConnInfoFlags = 440;
constexpr int kConnFlagRelayed = 16;

// ---------------------------------------------------------------- flat API

struct Api {
    HMODULE dll = nullptr;
    bool ready = false; // interfaces resolved, user logged on
    int32_t(__cdecl* GetHSteamUser)() = nullptr;
    void*(__cdecl* SteamUser)() = nullptr;
    void*(__cdecl* SteamFriends)() = nullptr;
    void*(__cdecl* SteamMatchmaking)() = nullptr;
    void*(__cdecl* SteamUtils)() = nullptr;
    void*(__cdecl* SteamNetworkingMessages)() = nullptr;
    void*(__cdecl* SteamNetworkingUtils)() = nullptr;
    void(__cdecl* RegisterCallback)(CCallbackBase*, int) = nullptr;
    void(__cdecl* RegisterCallResult)(CCallbackBase*, SteamAPICall_t) = nullptr;
    uint64_t(__cdecl* GetSteamID)(void* user) = nullptr;
    const char*(__cdecl* GetPersonaName)(void* friends) = nullptr;
    const char*(__cdecl* GetFriendPersonaName)(void* friends, uint64_t id) = nullptr;
    int(__cdecl* GetFriendRelationship)(void* friends, uint64_t id) = nullptr;
    bool(__cdecl* SetRichPresence)(void* friends, const char* key, const char* value) = nullptr;
    void(__cdecl* ClearRichPresence)(void* friends) = nullptr;
    void(__cdecl* InviteDialog)(void* friends, uint64_t lobby) = nullptr;
    void(__cdecl* InviteDialogConnectString)(void* friends, const char* connect) = nullptr;
    SteamAPICall_t(__cdecl* CreateLobby)(void* mm, int type, int maxMembers) = nullptr;
    SteamAPICall_t(__cdecl* JoinLobby)(void* mm, uint64_t lobby) = nullptr;
    void(__cdecl* LeaveLobby)(void* mm, uint64_t lobby) = nullptr;
    uint64_t(__cdecl* GetLobbyOwner)(void* mm, uint64_t lobby) = nullptr;
    bool(__cdecl* SetLobbyData)(void* mm, uint64_t lobby, const char* key, const char* value) = nullptr;
    const char*(__cdecl* GetLobbyData)(void* mm, uint64_t lobby, const char* key) = nullptr;
    bool(__cdecl* SetLobbyJoinable)(void* mm, uint64_t lobby, bool joinable) = nullptr;
    int(__cdecl* GetNumLobbyMembers)(void* mm, uint64_t lobby) = nullptr;
    uint64_t(__cdecl* GetLobbyMemberByIndex)(void* mm, uint64_t lobby, int index) = nullptr;
    bool(__cdecl* IsAPICallCompleted)(void* utils, SteamAPICall_t call, bool* failed) = nullptr;
    bool(__cdecl* GetAPICallResult)(void* utils, SteamAPICall_t call, void* out, int size, int expected,
                                    bool* failed) = nullptr;
    int(__cdecl* SendMessageToUser)(void* msgs, const SteamNetworkingIdentity* id, const void* data, uint32_t size,
                                    int flags, int channel) = nullptr;
    int(__cdecl* ReceiveMessagesOnChannel)(void* msgs, int channel, SteamNetworkingMessage_t** out, int max) = nullptr;
    bool(__cdecl* AcceptSessionWithUser)(void* msgs, const SteamNetworkingIdentity* id) = nullptr;
    bool(__cdecl* CloseSessionWithUser)(void* msgs, const SteamNetworkingIdentity* id) = nullptr;
    int(__cdecl* GetSessionConnectionInfo)(void* msgs, const SteamNetworkingIdentity* id, void* info,
                                           void* status) = nullptr;
    void(__cdecl* ReleaseMessage)(SteamNetworkingMessage_t* msg) = nullptr;
    void(__cdecl* InitRelayNetworkAccess)(void* utils) = nullptr;
    int(__cdecl* GetRelayNetworkStatus)(void* utils, void* details) = nullptr;

    void* user = nullptr;
    void* friends = nullptr;
    void* mm = nullptr;
    void* utils = nullptr;
    void* msgs = nullptr;
    void* netUtils = nullptr;
    uint64_t self = 0;

    bool HasLobby() const {
        return mm && CreateLobby && JoinLobby && LeaveLobby && GetLobbyOwner && SetLobbyData && GetLobbyData &&
               SetLobbyJoinable && GetNumLobbyMembers && GetLobbyMemberByIndex && RegisterCallResult;
    }
    bool HasRelay() const {
        return msgs && SendMessageToUser && ReceiveMessagesOnChannel && AcceptSessionWithUser;
    }
} g_api;

// ---------------------------------------------------------------- small helpers

int AccountType(uint64_t id) {
    return static_cast<int>((id >> 52) & 0xF);
}

uint64_t ParseId(const std::string& s) {
    if (s.empty() || s.size() > 20)
        return 0;
    for (char c : s)
        if (c < '0' || c > '9')
            return 0;
    return strtoull(s.c_str(), nullptr, 10);
}

std::vector<std::string> SplitWords(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '"'))
            ++i;
        size_t j = i;
        while (j < s.size() && s[j] != ' ' && s[j] != '\t' && s[j] != '"')
            ++j;
        if (j > i)
            out.push_back(s.substr(i, j - i));
        i = j;
    }
    return out;
}

struct JoinTarget {
    uint64_t host = 0;
    uint64_t lobby = 0;
};

// "+connect_lobby <lobby>" (Steam lobby invite) and "+c3coop_join <host> [lobby]" (our rich presence connect string)
JoinTarget ParseJoinTokens(const std::vector<std::string>& t) {
    JoinTarget r;
    for (size_t i = 0; i + 1 < t.size(); ++i) {
        if (_stricmp(t[i].c_str(), "+connect_lobby") == 0) {
            r.lobby = ParseId(t[i + 1]);
        } else if (_stricmp(t[i].c_str(), "+c3coop_join") == 0) {
            r.host = ParseId(t[i + 1]);
            if (i + 2 < t.size() && ParseId(t[i + 2]))
                r.lobby = ParseId(t[i + 2]);
        }
    }
    return r;
}

std::string Narrow(const wchar_t* w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1)
        WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

void MakeIdentity(SteamNetworkingIdentity& id, uint64_t steamId) {
    memset(&id, 0, sizeof(id));
    id.type = kIdentityTypeSteamID;
    id.size = sizeof(uint64_t);
    id.steamID64 = steamId;
}

uint64_t IdentitySteamId(const SteamNetworkingIdentity& id) {
    return id.type == kIdentityTypeSteamID ? id.steamID64 : 0;
}

std::string FriendName(uint64_t id) {
    if (!g_api.ready || !g_api.friends)
        return "?";
    if (id == g_api.self && g_api.GetPersonaName) {
        const char* n = g_api.GetPersonaName(g_api.friends);
        return n ? n : "?";
    }
    const char* n = g_api.GetFriendPersonaName ? g_api.GetFriendPersonaName(g_api.friends, id) : nullptr;
    return n ? n : "?";
}

// ---------------------------------------------------------------- game side (main thread)

std::atomic<int> g_svPort{64087}; // read by the host relay worker
std::string g_level;              // sv_map

void* CVar(const char* name) {
    void* console = game::Console();
    if (!console)
        return nullptr;
    return game::Virtual<void*(__fastcall*)(void*, const char*)>(console, game::kVtGetCVar)(console, name);
}

int CVarInt(const char* name, int fallback) {
    void* v = CVar(name);
    return v ? game::Virtual<int(__fastcall*)(void*)>(v, kVtCVarGetIVal)(v) : fallback;
}

std::string CVarString(const char* name) {
    void* v = CVar(name);
    const char* s = v ? game::Virtual<const char*(__fastcall*)(void*)>(v, kVtCVarGetString)(v) : nullptr;
    return s ? s : "";
}

bool GameRulesSigOk() {
    static const bool ok = memcmp(game::At<unsigned char>(kRvaGameGetGameRules), kGameGetGameRulesBytes,
                                  sizeof(kGameGetGameRulesBytes)) == 0;
    return ok;
}

void* GameRules() {
    if (!GameRulesSigOk())
        return nullptr;
    char* g = *game::At<char*>(kRvaGamePtr);
    if (!g || !*reinterpret_cast<void**>(g + 0x48)) // CGame::m_pFramework, NULL before CGame::Init
        return nullptr;
    return game::At<void*(__fastcall)(void*)>(kRvaGameGetGameRules)(g);
}

bool IsServer() {
    return *game::At<unsigned char>(game::kRvaServerFlag) != 0;
}

// This machine runs a level as the server (listen host).
bool IsServerLevel() {
    // a plain single player level is a server too, but local-only (gEnv->bMultiplayer 0): nobody can join it
    if (!IsServer() || !*game::At<unsigned char>(game::kRvaMultiplayerFlag))
        return false;
    return GameRulesSigOk() ? GameRules() != nullptr : true;
}

// A level (as server or client) is loaded: joining has to disconnect first.
bool LevelLoaded() {
    return IsServer() || GameRules() != nullptr;
}

// Runs a console line through IConsole::ExecuteString (and so through dllmain's hook: net id pools, NoteConnect).
void Exec(const std::string& line) {
    void* console = game::Console();
    if (!console)
        return;
    c3coop::Logf("steam: > %s", line.c_str());
    game::Virtual<void(__fastcall*)(void*, const char*, bool, bool)>(console, game::kVtExecuteString)(
        console, line.c_str(), false, false);
}

// ---------------------------------------------------------------- callbacks -> main thread events

enum class Ev { LobbyCreated, LobbyEnter, LobbyJoinRequested, RichPresenceJoin, SessionRequest, SessionFailed };

struct Event {
    Ev type;
    uint64_t a = 0;
    uint64_t b = 0;
    int64_t code = 0;
    std::string text;
};

std::mutex g_eventMtx;
std::deque<Event> g_events;

void Post(Event e) {
    std::lock_guard lock(g_eventMtx);
    if (g_events.size() < 1000)
        g_events.push_back(std::move(e));
}

using Handler = void (*)(const void* data, bool ioFailure, SteamAPICall_t call);

class Listener final : public CCallbackBase {
public:
    Listener(int id, int size, Handler handler) : size_(size), handler_(handler) { m_iCallback = id; }
    void Run(void* p) override {
        if (p)
            handler_(p, false, 0);
    }
    void Run(void* p, bool ioFailure, SteamAPICall_t call) override {
        if (p)
            handler_(p, ioFailure, call);
    }
    int GetCallbackSizeBytes() override { return size_; }

private:
    int size_;
    Handler handler_;
};

void OnLobbyCreated(const void* p, bool io, SteamAPICall_t call) {
    auto d = static_cast<const LobbyCreated_t*>(p);
    Post({Ev::LobbyCreated, io ? 0 : d->lobby, call, io ? -1 : d->result});
}

void OnLobbyEnter(const void* p, bool io, SteamAPICall_t call) {
    auto d = static_cast<const LobbyEnter_t*>(p);
    Post({Ev::LobbyEnter, io ? 0 : d->lobby, call, io ? -1 : static_cast<int64_t>(d->response)});
}

void OnGameLobbyJoinRequested(const void* p, bool, SteamAPICall_t) {
    auto d = static_cast<const GameLobbyJoinRequested_t*>(p);
    Post({Ev::LobbyJoinRequested, d->lobby, d->friendId});
}

void OnRichPresenceJoin(const void* p, bool, SteamAPICall_t) {
    auto d = static_cast<const GameRichPresenceJoinRequested_t*>(p);
    char connect[257];
    memcpy(connect, d->connect, 256);
    connect[256] = 0;
    Post({Ev::RichPresenceJoin, d->friendId, 0, 0, connect});
}

void OnSessionRequest(const void* p, bool, SteamAPICall_t) {
    auto id = static_cast<const SteamNetworkingIdentity*>(p);
    Post({Ev::SessionRequest, IdentitySteamId(*id), 0, id->type});
}

void OnSessionFailed(const void* p, bool, SteamAPICall_t) {
    auto info = static_cast<const unsigned char*>(p);
    char debug[129];
    memcpy(debug, info + kConnInfoEndDebug, 128);
    debug[128] = 0;
    int reason;
    memcpy(&reason, info + kConnInfoEndReason, sizeof(reason));
    Post({Ev::SessionFailed, IdentitySteamId(*reinterpret_cast<const SteamNetworkingIdentity*>(info)), 0, reason,
          debug});
}

Listener g_cbLobbyCreated(kCbLobbyCreated, sizeof(LobbyCreated_t), &OnLobbyCreated);
Listener g_cbLobbyEnter(kCbLobbyEnter, sizeof(LobbyEnter_t), &OnLobbyEnter);
Listener g_cbLobbyJoinRequested(kCbGameLobbyJoinRequested, sizeof(GameLobbyJoinRequested_t), &OnGameLobbyJoinRequested);
Listener g_cbRichPresenceJoin(kCbGameRichPresenceJoinRequested, sizeof(GameRichPresenceJoinRequested_t),
                              &OnRichPresenceJoin);
Listener g_cbSessionRequest(kCbSessionRequest, sizeof(SteamNetworkingIdentity), &OnSessionRequest);
Listener g_cbSessionFailed(kCbSessionFailed, kConnInfoSize, &OnSessionFailed);
// call results of CreateLobby / JoinLobby (separate objects: a call result must not be a registered callback)
Listener g_crLobbyCreated(kCbLobbyCreated, sizeof(LobbyCreated_t), &OnLobbyCreated);
Listener g_crLobbyEnter(kCbLobbyEnter, sizeof(LobbyEnter_t), &OnLobbyEnter);

// ---------------------------------------------------------------- sockets

SOCKET OpenUdp(uint16_t port) { // bound to 127.0.0.1:port (0 = ephemeral), non-blocking
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET)
        return s;
    u_long nonBlocking = 1;
    ioctlsocket(s, FIONBIO, &nonBlocking);
    // no WSAECONNRESET from recvfrom after an ICMP port unreachable (game server not listening yet)
    BOOL off = FALSE;
    DWORD ret = 0;
    WSAIoctl(s, SIO_UDP_CONNRESET, &off, sizeof(off), nullptr, 0, &ret, nullptr, nullptr);
    int bufSize = 1 << 20;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&bufSize), sizeof(bufSize));
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&bufSize), sizeof(bufSize));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(port);
    if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

uint16_t SocketPort(SOCKET s) {
    sockaddr_in a{};
    int len = sizeof(a);
    if (getsockname(s, reinterpret_cast<sockaddr*>(&a), &len) != 0)
        return 0;
    return ntohs(a.sin_port);
}

sockaddr_in LoopbackAddr(uint16_t port) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(port);
    return a;
}

// Receives one datagram; returns its size, or -1 when nothing (more) is queued.
int RecvOne(SOCKET s, char* buf, int size, sockaddr_in* from) {
    for (int tries = 0; tries < 16; ++tries) {
        sockaddr_in a{};
        int len = sizeof(a);
        int n = recvfrom(s, buf, size, 0, reinterpret_cast<sockaddr*>(&a), &len);
        if (n != SOCKET_ERROR) {
            if (from)
                *from = a;
            return n;
        }
        int e = WSAGetLastError();
        if (e != WSAECONNRESET && e != WSAEMSGSIZE) // skip ICMP errors and oversized datagrams
            return -1;
    }
    return -1;
}

// ---------------------------------------------------------------- transports (used by the relay worker only)

using Deliver = std::function<void(uint64_t peer, const char* data, int len)>;

class Transport {
public:
    virtual ~Transport() = default;
    virtual const char* Name() const = 0;
    virtual bool Open() = 0;
    virtual bool Send(uint64_t peer, const char* data, int len) = 0;
    virtual void Poll(const Deliver& deliver) = 0;
    virtual SOCKET Socket() const { return INVALID_SOCKET; }
    virtual void Close(uint64_t) {}
    virtual std::string Describe(uint64_t) { return {}; }
    virtual bool IsSteam() const { return false; }
};

// ISteamNetworkingMessages: unreliable no-nagle messages on channel 0 (thread-safe interface).
class SteamTransport final : public Transport {
public:
    explicit SteamTransport(uint64_t onlyPeer) : onlyPeer_(onlyPeer) {}
    const char* Name() const override { return "steam"; }
    bool IsSteam() const override { return true; }
    bool Open() override { return g_api.HasRelay(); }

    bool Send(uint64_t peer, const char* data, int len) override {
        int r = g_api.SendMessageToUser(g_api.msgs, &Identity(peer), data, static_cast<uint32_t>(len),
                                        kSendUnreliableNoNagle | kSendAutoRestartBrokenSession, kRelayChannel);
        if (r != kResultOK && failLogs_ < 10) {
            ++failLogs_;
            c3coop::Logf("steam relay: SendMessageToUser(%llu, %d bytes) -> EResult %d", (unsigned long long)peer, len, r);
        }
        return r == kResultOK;
    }

    void Poll(const Deliver& deliver) override {
        SteamNetworkingMessage_t* batch[64];
        for (int round = 0; round < 8; ++round) {
            int n = g_api.ReceiveMessagesOnChannel(g_api.msgs, kRelayChannel, batch, 64);
            if (n <= 0)
                break;
            for (int i = 0; i < n; ++i) {
                SteamNetworkingMessage_t* m = batch[i];
                uint64_t peer = IdentitySteamId(m->peer);
                if (peer && (!onlyPeer_ || peer == onlyPeer_) && m->data && m->size > 0)
                    deliver(peer, static_cast<const char*>(m->data), m->size);
                if (g_api.ReleaseMessage)
                    g_api.ReleaseMessage(m);
                else if (m->release)
                    m->release(m);
            }
            if (n < 64)
                break;
        }
    }

    void Close(uint64_t peer) override {
        if (g_api.CloseSessionWithUser)
            g_api.CloseSessionWithUser(g_api.msgs, &Identity(peer));
        ids_.erase(peer);
    }

    std::string Describe(uint64_t peer) override {
        if (!g_api.GetSessionConnectionInfo)
            return "steam";
        alignas(8) unsigned char info[1024] = {};
        alignas(8) unsigned char status[512] = {};
        int state = g_api.GetSessionConnectionInfo(g_api.msgs, &Identity(peer), info, status);
        static const char* const kStates[] = {"none", "connecting", "finding route", "connected", "closed by peer",
                                              "problem detected"};
        int ping, flags;
        memcpy(&ping, status + 4, sizeof(ping));
        memcpy(&flags, info + kConnInfoFlags, sizeof(flags));
        char desc[129];
        memcpy(desc, info + kConnInfoDescription, 128);
        desc[128] = 0;
        char out[256];
        snprintf(out, sizeof(out), "steam %s, ping %d ms, %s (%s)",
                 state >= 0 && state <= 5 ? kStates[state] : "state ?", state == 3 ? ping : -1,
                 (flags & kConnFlagRelayed) ? "relayed" : "direct", desc);
        return out;
    }

private:
    const SteamNetworkingIdentity& Identity(uint64_t peer) {
        auto& id = ids_[peer];
        if (id.steamID64 != peer || id.type != kIdentityTypeSteamID)
            MakeIdentity(id, peer);
        return id;
    }

    uint64_t onlyPeer_;
    std::map<uint64_t, SteamNetworkingIdentity> ids_;
    int failLogs_ = 0;
};

// "Fake Steam" for tests on one PC: UDP 127.0.0.1:64190, each datagram = 8-byte peer id + game datagram.
// The client sends its own id; the host answers with the same id to the address it last heard that id from.
class LoopbackTransport final : public Transport {
public:
    explicit LoopbackTransport(bool host) : host_(host) {
        selfId_ = 0x7E57000000000000ull | (static_cast<uint64_t>(GetCurrentProcessId()) << 16) |
                  (GetTickCount64() & 0xFFFF);
    }
    ~LoopbackTransport() override {
        if (sock_ != INVALID_SOCKET)
            closesocket(sock_);
    }
    const char* Name() const override { return "loopback"; }
    SOCKET Socket() const override { return sock_; }

    bool Open() override {
        sock_ = OpenUdp(host_ ? kLoopbackPort : 0);
        if (sock_ == INVALID_SOCKET)
            c3coop::Logf("steam relay: loopback transport cannot bind 127.0.0.1:%u (error %d)",
                         host_ ? kLoopbackPort : 0, WSAGetLastError());
        return sock_ != INVALID_SOCKET;
    }

    bool Send(uint64_t peer, const char* data, int len) override {
        if (len <= 0 || len > static_cast<int>(sizeof(buf_)) - 8)
            return false;
        sockaddr_in to;
        uint64_t id;
        if (host_) {
            auto it = addrs_.find(peer);
            if (it == addrs_.end())
                return false;
            to = it->second;
            id = peer;
        } else {
            to = LoopbackAddr(kLoopbackPort);
            id = selfId_;
        }
        memcpy(buf_, &id, 8);
        memcpy(buf_ + 8, data, len);
        return sendto(sock_, buf_, len + 8, 0, reinterpret_cast<sockaddr*>(&to), sizeof(to)) == len + 8;
    }

    void Poll(const Deliver& deliver) override {
        char in[65536];
        for (int i = 0; i < 256; ++i) {
            sockaddr_in from{};
            int n = RecvOne(sock_, in, sizeof(in), &from);
            if (n < 0)
                break;
            if (n <= 8)
                continue;
            uint64_t id;
            memcpy(&id, in, 8);
            if (host_) {
                addrs_[id] = from;
                deliver(id, in + 8, n - 8);
            } else {
                deliver(kLoopbackHostId, in + 8, n - 8);
            }
        }
    }

    void Close(uint64_t peer) override { addrs_.erase(peer); }

    std::string Describe(uint64_t peer) override {
        char out[96];
        if (host_) {
            auto it = addrs_.find(peer);
            snprintf(out, sizeof(out), "loopback from 127.0.0.1:%u", it == addrs_.end() ? 0 : ntohs(it->second.sin_port));
        } else {
            snprintf(out, sizeof(out), "loopback to 127.0.0.1:%u as id %llX", kLoopbackPort, (unsigned long long)selfId_);
        }
        return out;
    }

private:
    bool host_;
    SOCKET sock_ = INVALID_SOCKET;
    uint64_t selfId_;
    std::map<uint64_t, sockaddr_in> addrs_;
    char buf_[65536 + 8];
};

// ---------------------------------------------------------------- relay

struct PeerStats {
    uint64_t id = 0;
    uint16_t localPort = 0;
    uint64_t toGamePkts = 0, toGameBytes = 0;     // remote -> game
    uint64_t fromGamePkts = 0, fromGameBytes = 0; // game -> remote
    uint64_t sendFails = 0;
    ULONGLONG created = 0, lastRemote = 0, lastGame = 0;
    std::string conn;
};

class Relay {
public:
    enum class Role { Host, Client };

    Relay(Role role, std::unique_ptr<Transport> transport, uint64_t hostPeer)
        : role_(role), transport_(std::move(transport)), hostPeer_(hostPeer) {}
    ~Relay() { Stop(); }

    bool Start() {
        if (!transport_->Open())
            return false;
        if (role_ == Role::Client) {
            local_ = OpenUdp(0);
            if (local_ == INVALID_SOCKET) {
                c3coop::Logf("steam relay: cannot open the local UDP socket (error %d)", WSAGetLastError());
                return false;
            }
            clientPort_ = SocketPort(local_);
            client_.id = hostPeer_;
            client_.localPort = clientPort_;
            client_.created = GetTickCount64();
        }
        thread_ = std::thread(&Relay::Run, this);
        return true;
    }

    void Stop() {
        stop_ = true;
        if (thread_.joinable())
            thread_.join();
        for (auto& [id, p] : peers_) {
            transport_->Close(id);
            closesocket(p.sock);
        }
        peers_.clear();
        if (local_ != INVALID_SOCKET) {
            if (transport_)
                transport_->Close(hostPeer_);
            closesocket(local_);
            local_ = INVALID_SOCKET;
        }
    }

    Role GetRole() const { return role_; }
    bool IsSteam() const { return transport_->IsSteam(); }
    const char* TransportName() const { return transport_->Name(); }
    uint16_t ClientPort() const { return clientPort_; }
    uint64_t HostPeer() const { return hostPeer_; }
    uint64_t Dropped() const { return dropped_; }

    std::vector<PeerStats> Snapshot() {
        std::lock_guard lock(snapMtx_);
        return snap_;
    }

private:
    struct Peer {
        PeerStats st;
        SOCKET sock = INVALID_SOCKET;
    };

    void Run() {
        timeBeginPeriod(1);
        std::vector<char> buf(65536);
        ULONGLONG nextHouse = 0, nextDescribe = 0;
        while (!stop_) {
            fd_set rd;
            FD_ZERO(&rd);
            auto add = [&](SOCKET s) {
                if (s != INVALID_SOCKET && rd.fd_count < FD_SETSIZE)
                    FD_SET(s, &rd);
            };
            add(local_);
            for (auto& [id, p] : peers_)
                add(p.sock);
            add(transport_->Socket());
            if (rd.fd_count) {
                timeval tv{0, 1000};
                select(0, &rd, nullptr, nullptr, &tv);
            } else {
                Sleep(1);
            }
            ULONGLONG now = GetTickCount64();

            // game -> remote
            if (role_ == Role::Client) {
                sockaddr_in from{};
                for (int i = 0; i < 256; ++i) {
                    int n = RecvOne(local_, buf.data(), static_cast<int>(buf.size()), &from);
                    if (n < 0)
                        break;
                    if (!haveGame_ || from.sin_port != game_.sin_port)
                        c3coop::Logf("steam relay: game client at 127.0.0.1:%u -> host %llu over %s", ntohs(from.sin_port),
                                     (unsigned long long)hostPeer_, transport_->Name());
                    game_ = from;
                    haveGame_ = true;
                    if (n == 0)
                        continue;
                    client_.fromGamePkts++;
                    client_.fromGameBytes += n;
                    client_.lastGame = now;
                    if (!transport_->Send(hostPeer_, buf.data(), n))
                        client_.sendFails++;
                }
            } else {
                for (auto& [id, p] : peers_) {
                    for (int i = 0; i < 256; ++i) {
                        int n = RecvOne(p.sock, buf.data(), static_cast<int>(buf.size()), nullptr);
                        if (n < 0)
                            break;
                        if (n == 0)
                            continue;
                        p.st.fromGamePkts++;
                        p.st.fromGameBytes += n;
                        p.st.lastGame = now;
                        if (!transport_->Send(id, buf.data(), n))
                            p.st.sendFails++;
                    }
                }
            }

            // remote -> game
            transport_->Poll([&](uint64_t peer, const char* data, int len) { FromRemote(peer, data, len, now); });

            if (now >= nextHouse) {
                nextHouse = now + 500;
                bool describe = now >= nextDescribe;
                if (describe)
                    nextDescribe = now + 2000;
                Housekeeping(now, describe);
            }
        }
        timeEndPeriod(1);
    }

    void FromRemote(uint64_t peer, const char* data, int len, ULONGLONG now) {
        if (role_ == Role::Client) {
            if (peer != hostPeer_ || !haveGame_) {
                dropped_++;
                return;
            }
            client_.toGamePkts++;
            client_.toGameBytes += len;
            client_.lastRemote = now;
            sendto(local_, data, len, 0, reinterpret_cast<const sockaddr*>(&game_), sizeof(game_));
            return;
        }
        auto it = peers_.find(peer);
        if (it != peers_.end() && it->second.st.fromGamePkts && now - it->second.st.lastGame > kPeerGameDeadMs) {
            // the game server stopped answering this peer (its client left: "User left the game") and the friend sends
            // again: a reconnect. The old local port belongs to the closed connection, so the server ignored it.
            c3coop::Logf("steam relay: peer %llu reconnects (game server silent for %llu s), new local port",
                         (unsigned long long)peer, (now - it->second.st.lastGame) / 1000);
            closesocket(it->second.sock);
            peers_.erase(it);
            it = peers_.end();
        }
        if (it == peers_.end()) {
            if (peers_.size() >= kMaxHostPeers) {
                dropped_++;
                return;
            }
            SOCKET s = OpenUdp(0);
            if (s == INVALID_SOCKET) {
                dropped_++;
                return;
            }
            Peer p;
            p.sock = s;
            p.st.id = peer;
            p.st.localPort = SocketPort(s);
            p.st.created = now;
            it = peers_.emplace(peer, p).first;
            c3coop::Logf("steam relay: new peer %llu over %s: 127.0.0.1:%u -> game server 127.0.0.1:%d",
                         (unsigned long long)peer, transport_->Name(), p.st.localPort, g_svPort.load());
        }
        Peer& p = it->second;
        p.st.toGamePkts++;
        p.st.toGameBytes += len;
        p.st.lastRemote = now;
        sockaddr_in server = LoopbackAddr(static_cast<uint16_t>(g_svPort.load()));
        sendto(p.sock, data, len, 0, reinterpret_cast<sockaddr*>(&server), sizeof(server));
    }

    void Housekeeping(ULONGLONG now, bool describe) {
        for (auto it = peers_.begin(); it != peers_.end();) {
            ULONGLONG last = it->second.st.lastRemote ? it->second.st.lastRemote : it->second.st.created;
            if (now - last > kPeerIdleMs) {
                c3coop::Logf("steam relay: peer %llu silent for %llu s, closed (to game %llu pkts, from game %llu pkts)",
                             (unsigned long long)it->first, (now - last) / 1000,
                             (unsigned long long)it->second.st.toGamePkts, (unsigned long long)it->second.st.fromGamePkts);
                transport_->Close(it->first);
                closesocket(it->second.sock);
                it = peers_.erase(it);
            } else {
                ++it;
            }
        }
        std::vector<PeerStats> snap;
        if (role_ == Role::Client) {
            if (describe)
                client_.conn = transport_->Describe(hostPeer_);
            snap.push_back(client_);
        } else {
            for (auto& [id, p] : peers_) {
                if (describe)
                    p.st.conn = transport_->Describe(id);
                snap.push_back(p.st);
            }
        }
        std::lock_guard lock(snapMtx_);
        snap_.swap(snap);
    }

    Role role_;
    std::unique_ptr<Transport> transport_;
    uint64_t hostPeer_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<uint64_t> dropped_{0};
    // client role
    SOCKET local_ = INVALID_SOCKET;
    uint16_t clientPort_ = 0;
    sockaddr_in game_{};
    bool haveGame_ = false;
    PeerStats client_;
    // host role
    std::map<uint64_t, Peer> peers_;
    std::mutex snapMtx_;
    std::vector<PeerStats> snap_;
};

// main thread only; never destroyed at process exit (the worker is gone by then)
std::unique_ptr<Relay>& g_relay = *new std::unique_ptr<Relay>;

void StopRelay(const char* why) {
    if (!g_relay)
        return;
    c3coop::Logf("steam relay: %s %s relay stopped (%s)", g_relay->TransportName(),
                 g_relay->GetRole() == Relay::Role::Host ? "host" : "client", why);
    g_relay.reset();
}

bool StartRelay(Relay::Role role, bool loopback, uint64_t hostPeer) {
    StopRelay("replaced");
    std::unique_ptr<Transport> t;
    if (loopback)
        t = std::make_unique<LoopbackTransport>(role == Relay::Role::Host);
    else
        t = std::make_unique<SteamTransport>(role == Relay::Role::Client ? hostPeer : 0);
    auto relay = std::make_unique<Relay>(role, std::move(t), hostPeer);
    if (!relay->Start()) {
        c3coop::Logf("steam relay: cannot start the %s %s relay", loopback ? "loopback" : "steam",
                     role == Relay::Role::Host ? "host" : "client");
        return false;
    }
    g_relay = std::move(relay);
    if (role == Relay::Role::Host)
        c3coop::Logf("steam relay: host relay over %s up%s, game server 127.0.0.1:%d", g_relay->TransportName(),
                     loopback ? " (listening on 127.0.0.1:64190)" : "", g_svPort.load());
    else
        c3coop::Logf("steam relay: client relay over %s up: game connects to 127.0.0.1:%u, host %llu",
                     g_relay->TransportName(), g_relay->ClientPort(), (unsigned long long)hostPeer);
    return true;
}

bool RelayIs(Relay::Role role, bool steam) {
    return g_relay && g_relay->GetRole() == role && g_relay->IsSteam() == steam;
}

// ---------------------------------------------------------------- hosting and joining state (main thread)

struct HostState {
    bool autoHost = true;
    bool forced = false;     // !steam host
    bool active = false;     // hosting: relay + lobby wanted
    bool suppressed = false; // !steam leave or joined a friend: no auto host until this server session ends
    ULONGLONG serverSince = 0, notServerSince = 0;
    uint64_t lobby = 0;
    SteamAPICall_t createCall = 0;
    ULONGLONG createAt = 0, createRetryAt = 0, relayRetryAt = 0;
    std::string publishedKey; // level + member list last published
    std::vector<uint64_t> members;
} g_host;

enum class JoinStage { None, WaitStartup, WaitLobby, Connect };
const char* const kJoinStageNames[] = {"none", "waiting for Steam / front end", "joining lobby", "connecting"};

struct JoinState {
    JoinStage stage = JoinStage::None;
    uint64_t host = 0, lobby = 0;
    uint64_t joinedLobby = 0; // lobby we are a member of as a client
    SteamAPICall_t joinCall = 0;
    ULONGLONG at = 0, callAt = 0;
    uint16_t port = 0;
    bool loopback = false;
} g_join;

std::map<uint64_t, ULONGLONG> g_pendingSessions; // host: session requests waiting for lobby membership
ULONGLONG g_firstTick = 0;
ULONGLONG g_nextInitTry = 0;
ULONGLONG g_nextSlow = 0;
std::string g_initState; // last logged init problem (logged once per change)

void InitNote(const std::string& state) {
    if (state != g_initState) {
        g_initState = state;
        c3coop::Logf("steam: %s", state.c_str());
    }
}

template <typename Fn>
void Resolve(Fn& fn, const char* name, std::string& missing) {
    fn = reinterpret_cast<Fn>(GetProcAddress(g_api.dll, name));
    if (!fn)
        missing += std::string(" ") + name;
}

void TryInitSteam() {
    if (!g_api.dll) {
        HMODULE dll = GetModuleHandleW(L"steam_api64.dll");
        if (!dll) {
            InitNote("steam_api64.dll is not loaded (yet); Steam features off");
            return;
        }
        g_api.dll = dll;
        std::string missing;
        Resolve(g_api.GetHSteamUser, "SteamAPI_GetHSteamUser", missing);
        Resolve(g_api.SteamUser, "SteamAPI_SteamUser_v021", missing);
        Resolve(g_api.SteamFriends, "SteamAPI_SteamFriends_v017", missing);
        Resolve(g_api.SteamMatchmaking, "SteamAPI_SteamMatchmaking_v009", missing);
        Resolve(g_api.SteamUtils, "SteamAPI_SteamUtils_v010", missing);
        Resolve(g_api.SteamNetworkingMessages, "SteamAPI_SteamNetworkingMessages_SteamAPI_v002", missing);
        Resolve(g_api.SteamNetworkingUtils, "SteamAPI_SteamNetworkingUtils_SteamAPI_v004", missing);
        Resolve(g_api.RegisterCallback, "SteamAPI_RegisterCallback", missing);
        Resolve(g_api.RegisterCallResult, "SteamAPI_RegisterCallResult", missing);
        Resolve(g_api.GetSteamID, "SteamAPI_ISteamUser_GetSteamID", missing);
        Resolve(g_api.GetPersonaName, "SteamAPI_ISteamFriends_GetPersonaName", missing);
        Resolve(g_api.GetFriendPersonaName, "SteamAPI_ISteamFriends_GetFriendPersonaName", missing);
        Resolve(g_api.GetFriendRelationship, "SteamAPI_ISteamFriends_GetFriendRelationship", missing);
        Resolve(g_api.SetRichPresence, "SteamAPI_ISteamFriends_SetRichPresence", missing);
        Resolve(g_api.ClearRichPresence, "SteamAPI_ISteamFriends_ClearRichPresence", missing);
        Resolve(g_api.InviteDialog, "SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialog", missing);
        Resolve(g_api.InviteDialogConnectString, "SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialogConnectString",
                missing);
        Resolve(g_api.CreateLobby, "SteamAPI_ISteamMatchmaking_CreateLobby", missing);
        Resolve(g_api.JoinLobby, "SteamAPI_ISteamMatchmaking_JoinLobby", missing);
        Resolve(g_api.LeaveLobby, "SteamAPI_ISteamMatchmaking_LeaveLobby", missing);
        Resolve(g_api.GetLobbyOwner, "SteamAPI_ISteamMatchmaking_GetLobbyOwner", missing);
        Resolve(g_api.SetLobbyData, "SteamAPI_ISteamMatchmaking_SetLobbyData", missing);
        Resolve(g_api.GetLobbyData, "SteamAPI_ISteamMatchmaking_GetLobbyData", missing);
        Resolve(g_api.SetLobbyJoinable, "SteamAPI_ISteamMatchmaking_SetLobbyJoinable", missing);
        Resolve(g_api.GetNumLobbyMembers, "SteamAPI_ISteamMatchmaking_GetNumLobbyMembers", missing);
        Resolve(g_api.GetLobbyMemberByIndex, "SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex", missing);
        Resolve(g_api.IsAPICallCompleted, "SteamAPI_ISteamUtils_IsAPICallCompleted", missing);
        Resolve(g_api.GetAPICallResult, "SteamAPI_ISteamUtils_GetAPICallResult", missing);
        Resolve(g_api.SendMessageToUser, "SteamAPI_ISteamNetworkingMessages_SendMessageToUser", missing);
        Resolve(g_api.ReceiveMessagesOnChannel, "SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel", missing);
        Resolve(g_api.AcceptSessionWithUser, "SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser", missing);
        Resolve(g_api.CloseSessionWithUser, "SteamAPI_ISteamNetworkingMessages_CloseSessionWithUser", missing);
        Resolve(g_api.GetSessionConnectionInfo, "SteamAPI_ISteamNetworkingMessages_GetSessionConnectionInfo", missing);
        Resolve(g_api.ReleaseMessage, "SteamAPI_SteamNetworkingMessage_t_Release", missing);
        Resolve(g_api.InitRelayNetworkAccess, "SteamAPI_ISteamNetworkingUtils_InitRelayNetworkAccess", missing);
        Resolve(g_api.GetRelayNetworkStatus, "SteamAPI_ISteamNetworkingUtils_GetRelayNetworkStatus", missing);
        if (!missing.empty())
            c3coop::Logf("steam: steam_api64.dll lacks:%s", missing.c_str());
    }
    if (!g_api.GetHSteamUser || !g_api.GetHSteamUser()) {
        InitNote("Steam is not initialized (yet)");
        return;
    }
    g_api.user = g_api.SteamUser ? g_api.SteamUser() : nullptr;
    g_api.friends = g_api.SteamFriends ? g_api.SteamFriends() : nullptr;
    g_api.mm = g_api.SteamMatchmaking ? g_api.SteamMatchmaking() : nullptr;
    g_api.utils = g_api.SteamUtils ? g_api.SteamUtils() : nullptr;
    g_api.msgs = g_api.SteamNetworkingMessages ? g_api.SteamNetworkingMessages() : nullptr;
    g_api.netUtils = g_api.SteamNetworkingUtils ? g_api.SteamNetworkingUtils() : nullptr;
    if (!g_api.user || !g_api.GetSteamID || !g_api.friends || !g_api.RegisterCallback) {
        InitNote("SteamUser/SteamFriends interfaces unavailable; Steam features off");
        return;
    }
    g_api.self = g_api.GetSteamID(g_api.user);
    if (!g_api.self) {
        InitNote("not logged on to Steam");
        return;
    }
    for (Listener* l : {&g_cbLobbyCreated, &g_cbLobbyEnter, &g_cbLobbyJoinRequested, &g_cbRichPresenceJoin,
                        &g_cbSessionRequest, &g_cbSessionFailed})
        g_api.RegisterCallback(l, l->GetICallback());
    if (g_api.netUtils && g_api.InitRelayNetworkAccess)
        g_api.InitRelayNetworkAccess(g_api.netUtils); // warm up the relay network before anyone joins
    g_api.ready = true;
    c3coop::Logf("steam: ready, %llu \"%s\", lobbies %s, relay %s", (unsigned long long)g_api.self,
                 FriendName(g_api.self).c_str(), g_api.HasLobby() ? "ok" : "UNAVAILABLE",
                 g_api.HasRelay() ? "ok" : "UNAVAILABLE");
}

const char* RelayNetworkStatus() {
    if (!g_api.netUtils || !g_api.GetRelayNetworkStatus)
        return "unknown";
    switch (g_api.GetRelayNetworkStatus(g_api.netUtils, nullptr)) { // ESteamNetworkingAvailability
    case 100: return "current";
    case 10: return "attempting";
    case 8: return "waiting";
    case 1: return "never tried";
    case -100: return "retrying";
    case -101: return "failed";
    case -102: return "previously worked, now failed";
    case -103: return "cannot try";
    default: return "?";
    }
}

std::vector<uint64_t> LobbyMembers(uint64_t lobby) {
    std::vector<uint64_t> out;
    if (!lobby || !g_api.HasLobby())
        return out;
    int n = g_api.GetNumLobbyMembers(g_api.mm, lobby);
    for (int i = 0; i < n && i < 64; ++i)
        out.push_back(g_api.GetLobbyMemberByIndex(g_api.mm, lobby, i));
    return out;
}

void SetPresence(const char* key, const std::string& value) {
    if (g_api.SetRichPresence)
        g_api.SetRichPresence(g_api.friends, key, value.c_str());
}

void ClearPresence() {
    if (g_api.ready && g_api.ClearRichPresence)
        g_api.ClearRichPresence(g_api.friends);
}

std::string ConnectString(uint64_t host, uint64_t lobby) {
    std::string s = "+c3coop_join " + std::to_string(host);
    if (lobby)
        s += " " + std::to_string(lobby);
    return s;
}

// Lobby data and rich presence of the host; only pushed when level or members change.
void PublishHost(bool force) {
    if (!g_api.ready || !g_host.lobby)
        return;
    auto members = LobbyMembers(g_host.lobby);
    std::string key = g_level;
    for (auto m : members)
        key += "|" + std::to_string(m);
    if (!force && key == g_host.publishedKey)
        return;
    for (auto m : members)
        if (std::find(g_host.members.begin(), g_host.members.end(), m) == g_host.members.end() && m != g_api.self)
            c3coop::Logf("steam: %llu \"%s\" joined the lobby", (unsigned long long)m, FriendName(m).c_str());
    for (auto m : g_host.members)
        if (std::find(members.begin(), members.end(), m) == members.end() && m != g_api.self)
            c3coop::Logf("steam: %llu \"%s\" left the lobby", (unsigned long long)m, FriendName(m).c_str());
    g_host.members = members;
    g_host.publishedKey = key;
    g_api.SetLobbyData(g_api.mm, g_host.lobby, "level", g_level.c_str());
    char status[200];
    snprintf(status, sizeof(status), "Crysis 3 co-op: %s (%d/%d)", g_level.empty() ? "campaign" : g_level.c_str(),
             static_cast<int>(members.size()), kLobbyMaxMembers);
    SetPresence("status", status);
    SetPresence("connect", ConnectString(g_api.self, g_host.lobby));
    SetPresence("steam_player_group", std::to_string(g_host.lobby));
    SetPresence("steam_player_group_size", std::to_string(members.size()));
}

void RequestLobby(ULONGLONG now) {
    g_host.createCall = g_api.CreateLobby(g_api.mm, kLobbyTypeFriendsOnly, kLobbyMaxMembers);
    g_host.createAt = now;
    if (!g_host.createCall) {
        c3coop::Logf("steam: CreateLobby failed, retrying in 30 s");
        g_host.createRetryAt = now + 30000;
        return;
    }
    g_api.RegisterCallResult(&g_crLobbyCreated, g_host.createCall);
    c3coop::Logf("steam: creating a friends-only lobby (call %llX)", (unsigned long long)g_host.createCall);
}

void StartHosting(const char* why) {
    g_host.active = true;
    g_host.publishedKey.clear();
    c3coop::Logf("steam: hosting (%s), level '%s', sv_port %d", why, g_level.c_str(), g_svPort.load());
    if (RelayIs(Relay::Role::Host, false)) {
        c3coop::Logf("steam: keeping the loopback test host relay (Steam players cannot join until relaytest stop)");
    } else if (g_api.ready && g_api.HasRelay()) {
        StartRelay(Relay::Role::Host, false, 0);
    } else if (g_api.ready) {
        c3coop::Logf("steam: no Steam networking messages: friends cannot join through Steam");
    }
    // the lobby is requested by UpdateHosting as soon as Steam is ready
}

void StopHosting(const char* why) {
    if (!g_host.active)
        return;
    if (g_host.lobby && g_api.ready)
        g_api.LeaveLobby(g_api.mm, g_host.lobby);
    if (g_host.lobby)
        c3coop::Logf("steam: left lobby %llu", (unsigned long long)g_host.lobby);
    g_host.lobby = 0;
    g_host.createCall = 0;
    g_host.members.clear();
    g_host.publishedKey.clear();
    ClearPresence();
    if (RelayIs(Relay::Role::Host, true))
        StopRelay(why);
    g_host.active = false;
    g_host.forced = false;
    g_pendingSessions.clear();
    c3coop::Logf("steam: hosting stopped (%s)", why);
}

void OnLobbyCreatedEvent(int64_t result, uint64_t lobby) {
    if (lobby && lobby == g_host.lobby)
        return; // second notification (call result + callback)
    g_host.createCall = 0;
    if (result != kResultOK || !lobby) {
        c3coop::Logf("steam: lobby creation failed (EResult %lld), retrying in 30 s", (long long)result);
        g_host.createRetryAt = GetTickCount64() + 30000;
        return;
    }
    if (!g_host.active || g_host.lobby) {
        c3coop::Logf("steam: lobby %llu no longer needed, leaving it", (unsigned long long)lobby);
        g_api.LeaveLobby(g_api.mm, lobby);
        return;
    }
    g_host.lobby = lobby;
    g_api.SetLobbyData(g_api.mm, lobby, "c3coop", kVersion);
    g_api.SetLobbyData(g_api.mm, lobby, "host", std::to_string(g_api.self).c_str());
    g_api.SetLobbyJoinable(g_api.mm, lobby, true);
    PublishHost(true);
    c3coop::Logf("steam: lobby %llu ready; friends can use \"Join Game\", invite with Shift+Tab or "
                 "`c3coop steam invite`", (unsigned long long)lobby);
}

void BeginConnect(uint16_t port) {
    g_join.port = port;
    g_join.stage = JoinStage::Connect;
    ULONGLONG now = GetTickCount64();
    if (LevelLoaded()) {
        Exec("disconnect");
        g_join.at = now + 3000;
    } else {
        g_join.at = now + 300;
    }
}

void BeginRelayClient(uint64_t host, bool loopback) {
    g_join.stage = JoinStage::None;
    if (!loopback && (!g_api.ready || !g_api.HasRelay())) {
        c3coop::Logf("steam: cannot join %llu: Steam networking unavailable", (unsigned long long)host);
        return;
    }
    if (!host || (!loopback && host == g_api.self)) {
        c3coop::Logf("steam: no host to join");
        return;
    }
    g_join.host = host;
    g_join.loopback = loopback;
    if (!StartRelay(Relay::Role::Client, loopback, host))
        return;
    BeginConnect(g_relay->ClientPort());
}

void StartJoin(uint64_t host, uint64_t lobby, const char* source) {
    c3coop::Logf("steam: join request (%s): host %llu, lobby %llu", source, (unsigned long long)host,
                 (unsigned long long)lobby);
    if (!g_api.ready) {
        g_join.stage = JoinStage::WaitStartup;
        g_join.host = host;
        g_join.lobby = lobby;
        c3coop::Logf("steam: waiting for Steam before joining");
        return;
    }
    if (host == g_api.self || (lobby && lobby == g_host.lobby)) {
        c3coop::Logf("steam: that is this machine's own session");
        return;
    }
    if (g_host.active) {
        StopHosting("joining a friend");
        g_host.suppressed = true;
    }
    if (g_join.joinedLobby && g_join.joinedLobby != lobby) {
        g_api.LeaveLobby(g_api.mm, g_join.joinedLobby);
        g_join.joinedLobby = 0;
    }
    g_join.host = host;
    g_join.lobby = lobby;
    if (lobby && g_api.HasLobby()) {
        g_join.joinCall = g_api.JoinLobby(g_api.mm, lobby);
        if (g_join.joinCall)
            g_api.RegisterCallResult(&g_crLobbyEnter, g_join.joinCall);
        g_join.stage = JoinStage::WaitLobby;
        g_join.callAt = GetTickCount64();
        g_join.at = g_join.callAt + 20000;
        c3coop::Logf("steam: joining lobby %llu", (unsigned long long)lobby);
    } else if (host) {
        BeginRelayClient(host, false);
    } else {
        c3coop::Logf("steam: nothing to join (lobby support unavailable)");
    }
}

void OnLobbyEnterEvent(uint64_t lobby, int64_t response) {
    if (g_join.stage == JoinStage::WaitLobby && lobby == g_join.lobby) {
        g_join.joinCall = 0;
        if (response != kChatRoomEnterSuccess) {
            c3coop::Logf("steam: could not enter lobby %llu (response %lld)", (unsigned long long)lobby,
                         (long long)response);
            if (g_join.host)
                BeginRelayClient(g_join.host, false);
            else
                g_join.stage = JoinStage::None;
            return;
        }
        g_join.joinedLobby = lobby;
        uint64_t owner = g_api.GetLobbyOwner(g_api.mm, lobby);
        const char* hostData = g_api.GetLobbyData(g_api.mm, lobby, "host");
        const char* version = g_api.GetLobbyData(g_api.mm, lobby, "c3coop");
        uint64_t host = owner && owner != g_api.self ? owner : ParseId(hostData ? hostData : "");
        if (!host)
            host = g_join.host;
        c3coop::Logf("steam: in lobby %llu, owner %llu \"%s\", host data %s, version %s", (unsigned long long)lobby,
                     (unsigned long long)owner, FriendName(owner).c_str(), hostData ? hostData : "-",
                     version ? version : "-");
        if (version && *version && strcmp(version, kVersion) != 0)
            c3coop::Logf("steam: WARNING: host runs %s, this machine %s", version, kVersion);
        BeginRelayClient(host, false);
        return;
    }
    if (g_host.active && !g_host.lobby && g_api.GetLobbyOwner && g_api.GetLobbyOwner(g_api.mm, lobby) == g_api.self) {
        OnLobbyCreatedEvent(kResultOK, lobby); // LobbyEnter_t arrived before LobbyCreated_t
        return;
    }
    if (lobby != g_host.lobby && lobby != g_join.joinedLobby)
        c3coop::Logf("steam: entered lobby %llu (not ours, response %lld)", (unsigned long long)lobby, (long long)response);
}

bool SessionAllowed(uint64_t peer) {
    if (g_api.GetFriendRelationship && g_api.GetFriendRelationship(g_api.friends, peer) == kFriendRelationshipFriend)
        return true;
    auto members = LobbyMembers(g_host.lobby);
    return std::find(members.begin(), members.end(), peer) != members.end();
}

void AcceptSession(uint64_t peer) {
    SteamNetworkingIdentity id;
    MakeIdentity(id, peer);
    bool ok = g_api.AcceptSessionWithUser(g_api.msgs, &id);
    c3coop::Logf("steam: accepted relay session from %llu \"%s\"%s", (unsigned long long)peer, FriendName(peer).c_str(),
                 ok ? "" : " (AcceptSessionWithUser returned false)");
}

void OnSessionRequestEvent(uint64_t peer, int64_t identityType) {
    if (!peer) {
        c3coop::Logf("steam: session request from a non-SteamID identity (type %lld) ignored", (long long)identityType);
        return;
    }
    if (!g_api.HasRelay())
        return;
    if (RelayIs(Relay::Role::Client, true) && peer == g_relay->HostPeer()) {
        AcceptSession(peer);
    } else if (RelayIs(Relay::Role::Host, true)) {
        if (SessionAllowed(peer)) {
            AcceptSession(peer);
        } else {
            c3coop::Logf("steam: session request from %llu \"%s\": not a friend or lobby member yet, waiting",
                         (unsigned long long)peer, FriendName(peer).c_str());
            g_pendingSessions[peer] = GetTickCount64();
        }
    } else {
        c3coop::Logf("steam: session request from %llu \"%s\" ignored (not hosting)", (unsigned long long)peer,
                     FriendName(peer).c_str());
    }
}

void UpdatePendingSessions(ULONGLONG now) {
    for (auto it = g_pendingSessions.begin(); it != g_pendingSessions.end();) {
        if (RelayIs(Relay::Role::Host, true) && SessionAllowed(it->first)) {
            AcceptSession(it->first);
            it = g_pendingSessions.erase(it);
        } else if (now - it->second > kPendingSessionMs || !RelayIs(Relay::Role::Host, true)) {
            c3coop::Logf("steam: session request from %llu rejected (not a friend / lobby member)",
                         (unsigned long long)it->first);
            it = g_pendingSessions.erase(it);
        } else {
            ++it;
        }
    }
}

void ProcessEvents() {
    std::deque<Event> events;
    {
        std::lock_guard lock(g_eventMtx);
        events.swap(g_events);
    }
    for (auto& e : events) {
        switch (e.type) {
        case Ev::LobbyCreated:
            if (g_api.ready)
                OnLobbyCreatedEvent(e.code, e.a);
            break;
        case Ev::LobbyEnter:
            if (g_api.ready)
                OnLobbyEnterEvent(e.a, e.code);
            break;
        case Ev::LobbyJoinRequested:
            c3coop::Logf("steam: invite / join from %llu \"%s\" for lobby %llu", (unsigned long long)e.b,
                         FriendName(e.b).c_str(), (unsigned long long)e.a);
            StartJoin(0, e.a, "lobby join request");
            break;
        case Ev::RichPresenceJoin: {
            JoinTarget t = ParseJoinTokens(SplitWords(e.text));
            c3coop::Logf("steam: \"Join Game\" on %llu \"%s\": \"%s\"", (unsigned long long)e.a,
                         FriendName(e.a).c_str(), e.text.c_str());
            if (!t.host && !t.lobby)
                t.host = e.a; // unknown connect string: the friend is the host
            StartJoin(t.host, t.lobby, "rich presence join");
            break;
        }
        case Ev::SessionRequest:
            OnSessionRequestEvent(e.a, e.code);
            break;
        case Ev::SessionFailed:
            c3coop::Logf("steam: relay session with %llu \"%s\" failed: reason %lld, %s", (unsigned long long)e.a,
                         FriendName(e.a).c_str(), (long long)e.code, e.text.c_str());
            break;
        }
    }
}

// Fallback for call results the dispatcher did not deliver: poll ISteamUtils.
template <typename T>
bool PollCall(SteamAPICall_t call, int callbackId, T* out) {
    if (!call || !g_api.utils || !g_api.IsAPICallCompleted || !g_api.GetAPICallResult)
        return false;
    bool failed = false;
    if (!g_api.IsAPICallCompleted(g_api.utils, call, &failed))
        return false;
    return g_api.GetAPICallResult(g_api.utils, call, out, static_cast<int>(sizeof(T)), callbackId, &failed) && !failed;
}

void UpdateHosting(ULONGLONG now) {
    bool server = IsServerLevel();
    if (server) {
        g_host.notServerSince = 0;
        if (!g_host.serverSince)
            g_host.serverSince = now;
    } else {
        g_host.serverSince = 0;
        if (!g_host.notServerSince)
            g_host.notServerSince = now;
        if (now - g_host.notServerSince >= kHostEndMs)
            g_host.suppressed = false; // that server session is over
    }
    if (!g_host.active) {
        bool detected = g_host.autoHost && !g_host.suppressed && server && now - g_host.serverSince >= kHostDetectMs &&
                        g_join.stage == JoinStage::None;
        if (g_host.forced || detected)
            StartHosting(g_host.forced ? "forced" : "server level detected");
    } else if (!g_host.forced && !server && now - g_host.notServerSince >= kHostEndMs) {
        StopHosting("server session ended");
    }
    if (!g_host.active || !g_api.ready)
        return;
    if (!g_relay && g_api.HasRelay() && now >= g_host.relayRetryAt) {
        // Steam came up after hosting started, or relaytest stop
        if (!StartRelay(Relay::Role::Host, false, 0))
            g_host.relayRetryAt = now + 30000;
    }
    if (!g_api.HasLobby())
        return;
    if (!g_host.lobby && !g_host.createCall && now >= g_host.createRetryAt)
        RequestLobby(now);
    if (g_host.createCall && now - g_host.createAt > 2000) {
        LobbyCreated_t r{};
        if (PollCall(g_host.createCall, kCbLobbyCreated, &r))
            OnLobbyCreatedEvent(r.result, r.lobby);
        else if (now - g_host.createAt > 30000) {
            c3coop::Logf("steam: no answer to CreateLobby in 30 s, retrying");
            g_host.createCall = 0;
        }
    }
    PublishHost(false);
}

void UpdateJoin(ULONGLONG now) {
    switch (g_join.stage) {
    case JoinStage::None:
        break;
    case JoinStage::WaitStartup:
        if (g_api.ready && now - g_firstTick >= kStartupJoinDelayMs) {
            g_join.stage = JoinStage::None;
            StartJoin(g_join.host, g_join.lobby, "startup");
        } else if (!g_api.ready && now - g_firstTick > 120000) {
            c3coop::Logf("steam: Steam never became ready, join cancelled");
            g_join.stage = JoinStage::None;
        }
        break;
    case JoinStage::WaitLobby:
        if (g_join.joinCall && now - g_join.callAt > 2000) {
            LobbyEnter_t r{};
            if (PollCall(g_join.joinCall, kCbLobbyEnter, &r)) {
                OnLobbyEnterEvent(r.lobby, r.response);
                break;
            }
        }
        if (now >= g_join.at) {
            c3coop::Logf("steam: lobby %llu did not answer in 20 s", (unsigned long long)g_join.lobby);
            if (g_join.host)
                BeginRelayClient(g_join.host, false);
            else
                g_join.stage = JoinStage::None;
        }
        break;
    case JoinStage::Connect:
        if (now >= g_join.at) {
            g_join.stage = JoinStage::None;
            if (!g_relay || g_relay->GetRole() != Relay::Role::Client) {
                c3coop::Logf("steam: client relay gone, not connecting");
                break;
            }
            Exec("connect 127.0.0.1 " + std::to_string(g_join.port));
            if (!g_join.loopback && g_api.ready) {
                SetPresence("status", "Crysis 3 co-op with " + FriendName(g_join.host));
                SetPresence("connect", ConnectString(g_join.host, g_join.joinedLobby));
                if (g_join.joinedLobby) {
                    SetPresence("steam_player_group", std::to_string(g_join.joinedLobby));
                    SetPresence("steam_player_group_size",
                                std::to_string(LobbyMembers(g_join.joinedLobby).size()));
                }
            }
        }
        break;
    }
}

void LeaveAll() {
    g_join.stage = JoinStage::None;
    if (g_host.active) {
        StopHosting("!steam leave");
        g_host.suppressed = true;
    }
    if (g_join.joinedLobby && g_api.ready) {
        g_api.LeaveLobby(g_api.mm, g_join.joinedLobby);
        c3coop::Logf("steam: left lobby %llu", (unsigned long long)g_join.joinedLobby);
    }
    g_join.joinedLobby = 0;
    StopRelay("!steam leave");
    ClearPresence();
}

void PrintStatus() {
    using ull = unsigned long long;
    if (g_api.ready)
        c3coop::Logf("steam: ready, %llu \"%s\", relay network %s, version %s", (ull)g_api.self,
                     FriendName(g_api.self).c_str(), RelayNetworkStatus(), kVersion);
    else
        c3coop::Logf("steam: NOT ready (%s), version %s", g_initState.c_str(), kVersion);
    c3coop::Logf("steam: server level %s, level '%s', sv_port %d, hosting %s (auto %s%s%s)",
                 IsServerLevel() ? "yes" : "no", g_level.c_str(), g_svPort.load(), g_host.active ? "yes" : "no",
                 g_host.autoHost ? "on" : "off", g_host.forced ? ", forced" : "", g_host.suppressed ? ", suppressed" : "");
    if (g_host.lobby) {
        c3coop::Logf("steam: lobby %llu, %d/%d members", (ull)g_host.lobby, static_cast<int>(g_host.members.size()),
                     kLobbyMaxMembers);
        for (auto m : g_host.members)
            c3coop::Logf("steam:   member %llu \"%s\"", (ull)m, FriendName(m).c_str());
    } else if (g_host.createCall) {
        c3coop::Logf("steam: lobby creation pending");
    }
    c3coop::Logf("steam: join: %s, host %llu, lobby %llu, client of lobby %llu",
                 kJoinStageNames[static_cast<int>(g_join.stage)], (ull)g_join.host, (ull)g_join.lobby,
                 (ull)g_join.joinedLobby);
    if (!g_pendingSessions.empty())
        c3coop::Logf("steam: %d session request(s) waiting for lobby membership",
                     static_cast<int>(g_pendingSessions.size()));
    if (!g_relay) {
        c3coop::Logf("steam: relay off");
        return;
    }
    bool client = g_relay->GetRole() == Relay::Role::Client;
    if (client)
        c3coop::Logf("steam: client relay over %s: game -> 127.0.0.1:%u -> host %llu, dropped %llu",
                     g_relay->TransportName(), g_relay->ClientPort(), (ull)g_relay->HostPeer(), (ull)g_relay->Dropped());
    else
        c3coop::Logf("steam: host relay over %s -> game server 127.0.0.1:%d, dropped %llu", g_relay->TransportName(),
                     g_svPort.load(), (ull)g_relay->Dropped());
    ULONGLONG now = GetTickCount64();
    auto snap = g_relay->Snapshot();
    if (!client && snap.empty())
        c3coop::Logf("steam:   no peers");
    for (auto& p : snap) {
        ULONGLONG last = p.lastRemote > p.lastGame ? p.lastRemote : p.lastGame;
        c3coop::Logf("steam:   peer %llu \"%s\" port %u: to game %llu pkts / %llu B, from game %llu pkts / %llu B, "
                     "send fails %llu, idle %.1f s, %s",
                     (ull)p.id, g_relay->IsSteam() ? FriendName(p.id).c_str() : "-", p.localPort, (ull)p.toGamePkts,
                     (ull)p.toGameBytes, (ull)p.fromGamePkts, (ull)p.fromGameBytes, (ull)p.sendFails,
                     last ? (now - last) / 1000.0 : -1.0, p.conn.c_str());
    }
}

void Help() {
    c3coop::Logf("steam: commands (pipe \"!steam ...\", console \"c3coop steam ...\"): status | invite | "
                 "join <steamid64|lobbyid> | leave | host | autohost on|off | relaytest host|join|stop");
}

} // namespace

namespace c3coop {

void InstallSteam() {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    // started by Steam for an invite / "Join Game": "+connect_lobby <id>" or our "+c3coop_join <host> [lobby]"
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> tokens;
    for (int i = 0; argv && i < argc; ++i)
        tokens.push_back(Narrow(argv[i]));
    if (argv)
        LocalFree(argv);
    JoinTarget t = ParseJoinTokens(tokens);
    if (t.host || t.lobby) {
        // read on the main thread by SteamTick (not started yet)
        g_join.stage = JoinStage::WaitStartup;
        g_join.host = t.host;
        g_join.lobby = t.lobby;
        Logf("steam: command line join: host %llu, lobby %llu (after startup)", (unsigned long long)t.host,
             (unsigned long long)t.lobby);
    }
    Logf("steam: module installed (%s), Steam is initialized lazily on the main thread", kVersion);
}

bool SteamCommand(const std::string& cmd) {
    if (cmd != "!steam" && cmd.rfind("!steam ", 0) != 0)
        return false;
    auto args = SplitWords(cmd.substr(6));
    std::string verb = args.empty() ? "help" : args[0];
    std::string arg = args.size() > 1 ? args[1] : "";
    if (verb == "status") {
        PrintStatus();
    } else if (verb == "invite") {
        if (!g_api.ready) {
            Logf("steam: not ready");
        } else if (g_host.lobby && g_api.InviteDialog) {
            g_api.InviteDialog(g_api.friends, g_host.lobby);
            Logf("steam: invite dialog opened for lobby %llu (Steam overlay must be enabled)",
                 (unsigned long long)g_host.lobby);
        } else if (g_host.active && g_api.InviteDialogConnectString) {
            g_api.InviteDialogConnectString(g_api.friends, ConnectString(g_api.self, 0).c_str());
            Logf("steam: no lobby yet, invite dialog opened with the connect string");
        } else {
            Logf("steam: not hosting: start or load a level first (or `c3coop steam host`)");
        }
    } else if (verb == "join") {
        uint64_t id = ParseId(arg);
        if (!id) {
            Logf("steam: usage: join <steamid64 of the host | lobby id>");
        } else if (AccountType(id) == kAccountTypeChat) {
            StartJoin(0, id, "command");
        } else {
            if (AccountType(id) != kAccountTypeIndividual)
                Logf("steam: %llu does not look like a user SteamID64, trying anyway", (unsigned long long)id);
            StartJoin(id, 0, "command");
        }
    } else if (verb == "leave") {
        LeaveAll();
    } else if (verb == "host") {
        if (!IsServerLevel())
            Logf("steam: warning: no server level detected, friends can only join once one runs");
        g_host.forced = true;
        g_host.suppressed = false;
        if (!g_host.active)
            StartHosting("forced");
    } else if (verb == "autohost") {
        if (arg == "on" || arg == "off")
            g_host.autoHost = arg == "on";
        Logf("steam: automatic hosting %s", g_host.autoHost ? "on" : "off");
    } else if (verb == "relaytest") {
        if (arg == "host") {
            StartRelay(Relay::Role::Host, true, 0);
        } else if (arg == "join") {
            g_join.stage = JoinStage::None;
            BeginRelayClient(kLoopbackHostId, true);
        } else if (arg == "stop") {
            StopRelay("relaytest stop");
        } else {
            Logf("steam: usage: relaytest host|join|stop");
        }
    } else {
        Help();
    }
    return true;
}

void SteamTick() {
    ULONGLONG now = GetTickCount64();
    if (!g_firstTick)
        g_firstTick = now;
    if (!g_api.ready && now >= g_nextInitTry) {
        g_nextInitTry = now + 2000;
        TryInitSteam();
    }
    ProcessEvents();
    if (now >= g_nextSlow) {
        g_nextSlow = now + 500;
        int port = CVarInt("sv_port", 64087);
        if (port > 0 && port < 65536)
            g_svPort = port;
        g_level = CVarString("sv_map");
        if (g_level == "nolevel")
            g_level.clear();
        UpdateHosting(now);
        UpdatePendingSessions(now);
    }
    UpdateJoin(now);
}

} // namespace c3coop
