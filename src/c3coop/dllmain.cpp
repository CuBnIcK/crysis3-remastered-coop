// c3coop.dll: injected into Crysis3Remastered.exe.
// - unlocks the console (con_restricted = 0)
// - runs console commands received over a named pipe on the game's main thread (IConsole::Update hook)
// - streams every console line back to connected pipe clients (IOutputPrintSink)
//
// Pipe: \\.\pipe\c3coop_<pid>. Each line a client writes is a command; lines starting with '!' are
// handled by the DLL itself (!ping, !genv, !devmode on|off, !netids [high pool end], !forcesp on|off,
// !mpflag 0|1). Every console line is written back to all clients.

#include <winsock2.h> // before windows.h (server port probe)
#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <dbghelp.h>
#include <share.h>
#include <shlwapi.h>

#include "MinHook.h"
#include "game.h"
#include "modules.h"

#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "ws2_32.lib")

namespace {

HMODULE g_module;
std::atomic<long> g_updateCalls{0};
std::atomic<bool> g_listenFix{false}; // !listenfix on|off
std::atomic<bool> g_localServerNub{false}; // !localnub on|off
std::atomic<bool> g_ownServerSocket{true}; // !ownsocket on|off
std::atomic<bool> g_tracePackets{false};   // !tracepackets on|off
std::atomic<int> g_aiRegistrations{0};      // !aistats

// ---------------------------------------------------------------- output fan-out

struct Client {
    HANDLE pipe;
    std::mutex mtx;
    std::condition_variable cv;
    std::deque<std::string> out;
    bool closed = false;
};

std::mutex g_clientsMtx;
std::vector<std::shared_ptr<Client>> g_clients;
std::deque<std::string> g_backlog; // last lines, replayed to each new client
FILE* g_logFile;                   // <repo>/local/logs/<pid>.log, every emitted line

void Emit(const std::string& line) {
    std::lock_guard lock(g_clientsMtx);
    if (g_logFile) {
        fputs(line.c_str(), g_logFile);
        fputc('\n', g_logFile);
        fflush(g_logFile);
    }
    g_backlog.push_back(line);
    if (g_backlog.size() > 200)
        g_backlog.pop_front();
    for (auto& c : g_clients) {
        std::lock_guard l(c->mtx);
        if (c->out.size() < 20000)
            c->out.push_back(line);
        c->cv.notify_one();
    }
}

void Emitf(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Emit(std::string("[c3coop] ") + buf);
}

thread_local int g_luaQuiet = 0; // inside RunLua: the console echoes the "#<code>" line, 20 times a second for held objects

struct PrintSink final : game::IOutputPrintSink {
    void Print(const char* text) override {
        if (text && !(g_luaQuiet > 0 && text[0] == '#'))
            Emit(text);
    }
} g_sink;

// ---------------------------------------------------------------- main-thread command queue

std::mutex g_cmdMtx;
std::deque<std::string> g_cmds;

using UpdateFn = void(__fastcall*)(void* console);
using ExecuteStringFn = void(__fastcall*)(void* console, const char* cmd, bool silent, bool deferred);
using SinkFn = void(__fastcall*)(void* console, game::IOutputPrintSink* sink);
UpdateFn g_origUpdate;

void DumpGlobalEnv() {
    // Pointer slots around gEnv->pConsole, to map SSystemGlobalEnvironment later.
    auto slot = game::At<uintptr_t>(game::kRvaConsoleSlot);
    for (int i = -26; i <= 26; ++i) {
        uintptr_t v = slot[i];
        uintptr_t vt = 0;
        if (v > 0x10000 && !IsBadReadPtr(reinterpret_cast<void*>(v), 8))
            vt = *reinterpret_cast<uintptr_t*>(v);
        Emitf("gEnv%+05X: %016llX vt=%016llX%s", i * 8, (unsigned long long)v, (unsigned long long)vt,
              vt >= game::Base() && vt < game::Base() + 0x3000000 ? " (exe)" : "");
    }
}

std::atomic<bool> g_devModeForced{false}; // set by !devmode on (Lua through the console needs ISystem::IsDevMode)
std::atomic<int> g_devModeDepth{0};       // ExecuteDev calls in progress
using IsDevModeFn = bool(__fastcall*)(void*);
IsDevModeFn g_origIsDevMode;

bool __fastcall HookedIsDevMode(void* system) {
    return g_devModeForced || g_devModeDepth > 0 || g_origIsDevMode(system);
}

// The IsDevMode hook is installed once and stays enabled; forcing is a flag. (It used to be enabled and disabled
// around every ExecuteDev: each MinHook enable/disable suspends every game thread, and RunLua runs often, e.g. the
// token flush before every fgmirror send -> long freezes.)
bool InstallDevModeHook(void* console) {
    static bool installed = false;
    if (installed)
        return true;
    auto system = *reinterpret_cast<void**>(static_cast<char*>(console) + game::kConsoleSystemOffset);
    auto fn = game::Virtual<unsigned char*>(system, game::kVtSystemIsDevMode);
    if (MH_CreateHook(fn, &HookedIsDevMode, reinterpret_cast<void**>(&g_origIsDevMode)) != MH_OK ||
        MH_EnableHook(fn) != MH_OK) {
        Emitf("cannot hook IsDevMode %p", fn);
        return false;
    }
    installed = true;
    return true;
}

void ToggleDevMode(void* console, bool on) {
    if (!InstallDevModeHook(console))
        return;
    g_devModeForced = on;
    Emitf("devmode %s", on ? "forced on" : "restored");
}

// CXConsole::ExecuteString: lines starting with "c3coop " are the DLL's own commands (coop.cpp), so Lua
// (System.ExecuteCommand) and network messages can reach the DLL; everything else goes to the game.
ExecuteStringFn g_origExecuteString;

// Co-op hosting of single player sessions (!coophost on|off, default on): the front end starts campaign levels with
// "map <level> nb" (CFlashFrontEnd eFECMD_load). Such a map command (no "server"/"dedicated" argument; MapCmd matches
// arguments by prefix) becomes a listen server for up to 4 players, so friends can join any single player session.
std::atomic<DWORD> g_mainThreadId{0}; // the thread running IConsole::Update
void DumpMainStack();
std::atomic<bool> g_coopHost{true};
std::string g_delayedMap; // map command waiting for the "rejoin" messages to reach the clients (g_cmdMtx)
ULONGLONG g_delayedMapAt = 0;
void SetNetIdPools(const std::string& args);
constexpr int kCampaignNetIdHighEnd = 8464; // campaign levels need far more net ids than MP maps (notes.md)

bool StartsWithNoCase(const char* s, const char* prefix) {
    return _strnicmp(s, prefix, strlen(prefix)) == 0;
}

// "map <level> [args...]" without a server or dedicated argument
bool IsLocalMapCommand(const char* cmd) {
    if (!StartsWithNoCase(cmd, "map "))
        return false;
    char buf[512];
    strncpy_s(buf, cmd, _TRUNCATE);
    char* next = nullptr;
    strtok_s(buf, " 	", &next);                     // map
    if (!strtok_s(nullptr, " 	", &next))            // level
        return false;
    while (char* arg = strtok_s(nullptr, " 	", &next))
        if (tolower(arg[0]) == 's' || tolower(arg[0]) == 'd')
            return false;
    return true;
}

// ---------------------------------------------------------------- listen server port
// 0 when UDP 0.0.0.0:port can be bound exclusively right now, else the WinSock error (in use by our own old server
// nub, by the lobby socket of this or another local instance, or by anything else).
int UdpPortError(int port) {
    static bool wsaReady = [] {
        WSADATA wsa;
        return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    }();
    if (!wsaReady)
        return -1;
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET)
        return WSAGetLastError();
    BOOL exclusive = TRUE;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<u_short>(port));
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    int err = bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0 ? 0 : WSAGetLastError();
    closesocket(s);
    return err;
}

// Server port for a new hosted session (current = 0: from 64087) or for the next level (the port after `current` in
// game::NextServerPort order; never 64090 = lobby socket, audit #2). Ports that cannot be bound are skipped.
int PickServerPort(int current) {
    int port = current ? game::NextServerPort(current) : game::kServerPortFirst;
    for (int i = 0; i < 8; ++i, port = game::NextServerPort(port)) {
        if (port == current)
            continue;
        int err = UdpPortError(port);
        if (!err)
            return port;
        Emitf("server port %d not available (bind error %d), trying the next one", port, err);
    }
    port = current ? game::NextServerPort(current) : game::kServerPortFirst;
    Emitf("no free server port in %u..%u, using %d anyway", game::kServerPortFirst, game::kServerPortLast, port);
    return port;
}

int CurrentServerPort(void* console) {
    void* var = game::Virtual<void*(__fastcall*)(void*, const char*)>(console, game::kVtGetCVar)(console, "sv_port");
    return var ? game::Virtual<int(__fastcall*)(void*)>(var, game::kVtCVarGetIVal)(var) : game::kServerPortFirst;
}

// "name" or "name <args>" (whole first word, case-insensitive)
bool IsCommand(const char* cmd, const char* name) {
    size_t n = strlen(name);
    return _strnicmp(cmd, name, n) == 0 && (cmd[n] == 0 || cmd[n] == ' ' || cmd[n] == '\t');
}

void __fastcall HookedExecuteString(void* console, const char* cmd, bool silent, bool deferred) {
    if (cmd && strncmp(cmd, "c3coop ", 7) == 0) {
        c3coop::CoopCommand(cmd + 7);
        return;
    }
    // pause menu "load last checkpoint" / load game, death screen: nothing can be loaded in a network game (audit #3)
    if (cmd && (IsCommand(cmd, "loadLastSave") || IsCommand(cmd, "load")) && c3coop::BlockLoadCommand(cmd))
        return;
    if (cmd && (StartsWithNoCase(cmd, "map ") || StartsWithNoCase(cmd, "connect")))
        SetNetIdPools(std::to_string(kCampaignNetIdHighEnd));
    if (cmd && StartsWithNoCase(cmd, "connect "))
        c3coop::NoteConnect(cmd);
    if (cmd && StartsWithNoCase(cmd, "map ") && c3coop::InClientSession()) {
        // a client in a co-op session (connecting, loading or playing): its lockstep flowgraph reaches
        // Mission:LoadNextLevel too, but only the host changes the level (clients follow with "rejoin"). A former
        // client back in the menu has no game context and may start its own game (audit #10).
        Emitf("co-op client: ignoring \"%s\" (the host changes the level)", cmd);
        return;
    }
    if (cmd && g_coopHost && IsLocalMapCommand(cmd)) {
        std::string hosted = std::string(cmd) + " s";
        Emitf("co-op host: \"%s\" -> \"%s\" (sv_maxplayers 4)", cmd, hosted.c_str());
        g_origExecuteString(console, "sv_maxplayers 4", true, false);
        if (c3coop::HostingSession()) {
            // already hosting (next level): clients lose the connection with the old nub, tell them to come back.
            // The old server nub can keep its UDP socket after the forced shutdown (coop.cpp CNetNub::IsDead), so each
            // level gets the next free port of 64087..64094 (not 64090); clients connecting by address switch to it.
            int current = CurrentServerPort(console);
            int port = PickServerPort(current);
            Emitf("co-op host: level change, server port %d -> %d", current, port);
            std::string svPort = "sv_port " + std::to_string(port);
            g_origExecuteString(console, svPort.c_str(), true, false);
            c3coop::CoopCommand("send all rejoin " + std::to_string(port));
            std::lock_guard lock(g_cmdMtx);
            g_delayedMap = hosted;
            g_delayedMapAt = GetTickCount64() + 1500;
            return;
        }
        // a new hosted session: back to the base port (or the first free one after it)
        c3coop::CancelRejoin("hosting a new session");
        int port = PickServerPort(0);
        Emitf("co-op host: new session on server port %d", port);
        std::string svPort = "sv_port " + std::to_string(port);
        g_origExecuteString(console, svPort.c_str(), true, false);
        g_origExecuteString(console, hosted.c_str(), silent, deferred);
        return;
    }
    g_origExecuteString(console, cmd, silent, deferred);
}

// A "map" line typed in the console reaches MapCmd without IConsole::ExecuteString: such a line goes through
// HookedExecuteString, which hosts it (or ignores it on a co-op client) and comes back here with the server argument.
using MapCmdFn = void(__fastcall*)(void* args);
MapCmdFn g_origMapCmd;

void __fastcall HookedMapCmd(void* args) {
    std::string line = "map";
    int n = game::Virtual<int(__fastcall*)(void*)>(args, game::kVtCmdArgCount)(args);
    for (int i = 1; i < n; ++i)
        if (auto arg = game::Virtual<const char*(__fastcall*)(void*, int)>(args, game::kVtCmdArg)(args, i))
            line += std::string(" ") + arg;
    bool client = c3coop::InClientSession();
    if (n > 1 && g_origExecuteString && (client || (g_coopHost && IsLocalMapCommand(line.c_str())))) {
        HookedExecuteString(game::Console(), line.c_str(), false, false);
        return;
    }
    if (n > 1)
        c3coop::FgMirrorReset();
    g_origMapCmd(args);
}

// A line typed in the console runs its command without IConsole::ExecuteString ("Unknown command: c3coop"), so
// "c3coop" is also a real console command; it rebuilds the line from its arguments (quotes are lost).
void __fastcall CoopConsoleCmd(void* args) {
    std::string line;
    int n = game::Virtual<int(__fastcall*)(void*)>(args, game::kVtCmdArgCount)(args);
    for (int i = 1; i < n; ++i)
        if (auto arg = game::Virtual<const char*(__fastcall*)(void*, int)>(args, game::kVtCmdArg)(args, i))
            line += (line.empty() ? "" : " ") + std::string(arg);
    c3coop::CoopCommand(line);
}

void SetNetIdPools(const std::string& args) {
    auto cvars = *game::At<char*>(game::kRvaNetworkSlot) + game::kNetCVarsOffset;
    auto field = [&](size_t off) { return reinterpret_cast<int*>(cvars + off); };
    Emitf("net id pools: low end=%d, medium end=%d, high end=%d (raw C8..F0: %d %d %d %d %d %d %d %d %d %d)",
          *field(game::kNetLowEnd), *field(game::kNetMediumEnd), *field(game::kNetHighEnd), *field(0xC8),
          *field(0xCC), *field(0xD0), *field(0xD4), *field(0xD8), *field(0xDC), *field(0xE0), *field(0xE4),
          *field(0xE8), *field(0xEC));
    int highEnd;
    if (sscanf_s(args.c_str(), "%d", &highEnd) != 1)
        return;
    if (highEnd <= *field(game::kNetMediumEnd) || highEnd > 0xFFFE) {
        Emitf("refusing: high end must be in (medium end, 65534]");
        return;
    }
    *field(game::kNetHighEnd) = highEnd;
    int bits = 0;
    while ((1 << bits) < highEnd - *field(game::kNetMediumEnd))
        ++bits;
    *field(game::kNetHighBits) = bits;
    Emitf("high net id pool end set to %d", highEnd);
}

// !forcesp on: network game contexts start with gEnv->bMultiplayer = 0 (SP HUD/menus/materials).
void ForceSpMode(bool on) {
    auto site = game::At<unsigned char>(game::kRvaActionGameMpByte - 1);
    if (site[0] != 0xB2 || site[2] != 0x88 || site[3] != 0x15 || (site[1] != 0 && site[1] != 1)) {
        Emitf("forcesp: unexpected bytes, not patched");
        return;
    }
    DWORD old;
    VirtualProtect(site + 1, 1, PAGE_EXECUTE_READWRITE, &old);
    site[1] = on ? 0 : 1;
    VirtualProtect(site + 1, 1, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 4);
    Emitf("forcesp %s (bMultiplayer now %d; applies from the next game context)", on ? "on" : "off",
          *game::At<unsigned char>(game::kRvaMultiplayerFlag));
}

// Socket managers used by CNetwork::CreateNub (internal +0x6A0 / external +0x6A8 of the CNetwork at 0x39966588).
void DumpSocketManagers() {
    auto net = *reinterpret_cast<char**>(game::Base() + 0x2966588);
    for (size_t off : {size_t(0x6A0), size_t(0x6A8)}) {
        auto mgr = *reinterpret_cast<void**>(net + off);
        auto vt = mgr ? *reinterpret_cast<uintptr_t**>(mgr) : nullptr;
        Emitf("CNetwork+%zX = %p vtbl %p, vtbl[0xC8] = %llX", off, mgr, vt,
              vt ? (unsigned long long)vt[0xC8 / 8] : 0ull);
    }
}

// CCryPak::m_arrZips (vector<PackDesc>, 0x38 bytes each) in lookup order: FindPakFileEntry walks it backwards,
// so the last pak that has a file wins. PackDesc: +0 bind root, +8 file name, +0x28 ICryArchive*.
void DumpPaks() {
    auto pak = *game::At<char*>(game::kRvaCryPakSlot);
    if (!pak)
        return;
    auto begin = *reinterpret_cast<char**>(pak + game::kCryPakZipsBegin);
    auto end = *reinterpret_cast<char**>(pak + game::kCryPakZipsBegin + 8);
    int i = 0;
    for (auto e = begin; e < end; e += game::kCryPakDescSize, ++i) {
        auto archive = *reinterpret_cast<void**>(e + 0x28);
        unsigned flags = archive ? game::Virtual<unsigned(__fastcall*)(void*)>(archive, 0x60)(archive) : 0;
        Emitf("pak %3d flags %08X %s -> %s", i, flags, *reinterpret_cast<const char**>(e + 8),
              *reinterpret_cast<const char**>(e));
    }
}

void SetClassicHandshake(bool on);

void RunInternal(void* console, const std::string& cmd) {
    if (c3coop::HudCommand(cmd) || c3coop::AnimCommand(cmd) || c3coop::SteamCommand(cmd) || c3coop::SpCommand(cmd) ||
        c3coop::FgMirrorCommand(cmd)) {
    } else if (cmd == "!paks") {
        DumpPaks();
    } else if (cmd == "!aistats") {
        Emitf("server-side AI registrations so far: %d", g_aiRegistrations.load());
    } else if (cmd == "!sockmgr") {
        DumpSocketManagers();
    } else if (cmd.rfind("!vcall ", 0) == 0) {
        // debug: call a no-argument virtual getter on the object stored in a global slot
        unsigned long long slotRva = 0, offset = 0;
        if (sscanf_s(cmd.c_str() + 7, "%llx %llx", &slotRva, &offset) == 2) {
            auto obj = *reinterpret_cast<void**>(game::Base() + slotRva);
            if (!obj) {
                Emitf("vcall: slot %llX is null", slotRva);
            } else {
                auto fn = game::Virtual<uintptr_t(__fastcall*)(void*)>(obj, offset);
                Emitf("vcall: obj %p vtbl+%llX -> fn %p returned %llX", obj, offset, fn,
                      (unsigned long long)fn(obj));
            }
        }
    } else if (cmd == "!classichs on" || cmd == "!classichs off") {
        SetClassicHandshake(cmd == "!classichs on");
    } else if (cmd == "!tracepackets on" || cmd == "!tracepackets off") {
        g_tracePackets = cmd == "!tracepackets on";
        Emitf("nub packet trace %s", g_tracePackets ? "on" : "off");
    } else if (cmd == "!ownsocket on" || cmd == "!ownsocket off") {
        g_ownServerSocket = cmd == "!ownsocket on";
        Emitf("own sockets for non-lobby IPv4 nubs %s", g_ownServerSocket ? "on" : "off");
    } else if (cmd == "!localnub on" || cmd == "!localnub off") {
        g_localServerNub = cmd == "!localnub on";
        Emitf("extra local server nub %s", g_localServerNub ? "on" : "off");
    } else if (cmd == "!listenfix on" || cmd == "!listenfix off") {
        g_listenFix = cmd == "!listenfix on";
        Emitf("listen-server local connect redirect %s", g_listenFix ? "on" : "off");
    } else if (cmd == "!forcesp on" || cmd == "!forcesp off") {
        ForceSpMode(cmd == "!forcesp on");
    } else if (cmd == "!mpflag 0" || cmd == "!mpflag 1") {
        *game::At<unsigned char>(game::kRvaMultiplayerFlag) = cmd.back() == '1';
        Emitf("gEnv->bMultiplayer = %d", *game::At<unsigned char>(game::kRvaMultiplayerFlag));
    } else if (cmd == "!coophost on" || cmd == "!coophost off") {
        g_coopHost = cmd == "!coophost on";
        Emitf("co-op hosting of single player maps %s", g_coopHost ? "on" : "off");
    } else if (cmd.rfind("!netids", 0) == 0) {
        SetNetIdPools(cmd.substr(7));
    } else if (cmd == "!ping") {
        Emitf("pong, update calls=%ld", g_updateCalls.load());
    } else if (cmd == "!genv") {
        DumpGlobalEnv();
    } else if (cmd == "!devmode on" || cmd == "!devmode off") {
        ToggleDevMode(console, cmd == "!devmode on");
    } else {
        Emitf("unknown internal command: %s", cmd.c_str());
    }
}

using CreateNubFn = void*(__fastcall*)(void* self, const char* address, void* gameNub, void* security, void* query);
CreateNubFn g_origCreateNub;

void* __fastcall HookedCreateNub(void* self, const char* address, void* gameNub, void* security, void* query) {
    void* nub = g_origCreateNub(self, address, gameNub, security, query);
    Emitf("CNetwork::CreateNub(\"%s\", gameNub=%p, security=%p, query=%p) -> %p", address ? address : "(null)",
          gameNub, security, query, nub);
    if (nub)
        c3coop::ForgetNub(nub); // a nub at the address of a destroyed one gets its own dead-nub grace (audit #15)
    if (nub) {
        // CNetNub::m_pSocket (+0x60): which socket implementation backs this nub
        auto sock = *reinterpret_cast<void**>(static_cast<char*>(nub) + 0x60);
        auto sockVt = sock ? *reinterpret_cast<uintptr_t*>(sock) : 0;
        Emitf("  nub socket %p vtbl %llX", sock, (unsigned long long)sockVt);
    }
    // Listen server: IP server nubs live on the shared lobby socket, so the host's own "<local>:0" client can't
    // reach them. Give the same game server nub a second, local-only nub on "<local>:<port>".
    // Only server nubs get a query handler (client nubs pass NULL).
    const char* colon = address ? strrchr(address, ':') : nullptr;
    if (nub && g_localServerNub && query && colon && strncmp(address, "<local>", 7) != 0) {
        char local[32];
        sprintf_s(local, "<local>%s", colon);
        void* localNub = g_origCreateNub(self, local, gameNub, security, query);
        Emitf("  + local server nub \"%s\" -> %p", local, localNub);
    }
    return nub;
}

using OpenSocketFn = void*(__fastcall*)(void* mgr, void* out, const uintptr_t* addr, uint32_t flags);
OpenSocketFn g_origOpenSocket;

// IPv4 sockets on a real port other than the lobby's get their own socket (flag 0x10) instead of silently
// reusing the lobby socket, so a listen server binds sv_port and gets a "<local>" twin for the host's client.
void* __fastcall HookedOpenSocket(void* mgr, void* out, const uintptr_t* addr, uint32_t flags) {
    uint32_t newFlags = flags;
    auto type = reinterpret_cast<const int*>(addr[0] & ~uintptr_t(3));
    auto raw = reinterpret_cast<const unsigned char*>(addr + 1);
    if (type && *type == 2) {
        // SIPv4Addr layout probe: log the raw bytes, the port is one of the u16 fields
        uint16_t portA = *reinterpret_cast<const uint16_t*>(raw + 4);
        uint16_t portB = *reinterpret_cast<const uint16_t*>(raw);
        uint16_t port = (portA && portA != 0xFFFF) ? portA : portB;
        if (g_ownServerSocket && port && port != game::kLobbyPort)
            newFlags |= game::kSocketFlagOwnSocket;
        Emitf("OpenSocket ipv4 raw %02X%02X%02X%02X %02X%02X%02X%02X port %u flags %X -> %X", raw[0], raw[1], raw[2],
              raw[3], raw[4], raw[5], raw[6], raw[7], port, flags, newFlags);
    } else {
        Emitf("OpenSocket type %d flags %X", type ? *type : -1, flags);
    }
    return g_origOpenSocket(mgr, out, addr, newFlags);
}

using NubOnPacketFn = void(__fastcall*)(void* nub, const uintptr_t* from, const unsigned char* data, uint32_t len);
NubOnPacketFn g_origNubOnPacket;
std::atomic<int> g_packetsLogged{0};

void DescribeNetAddress(const uintptr_t* addr, char* out, size_t size) {
    auto type = reinterpret_cast<const int*>(addr[0] & ~uintptr_t(3));
    auto raw = reinterpret_cast<const unsigned char*>(addr + 1);
    if (type && *type == 2)
        sprintf_s(out, size, "%u.%u.%u.%u:%u", raw[3], raw[2], raw[1], raw[0], *reinterpret_cast<const uint16_t*>(raw + 4));
    else if (type && *type == 1)
        sprintf_s(out, size, "<local>:%u", *reinterpret_cast<const uint16_t*>(raw));
    else
        sprintf_s(out, size, "type %d", type ? *type : -1);
}

void __fastcall HookedNubOnPacket(void* nub, const uintptr_t* from, const unsigned char* data, uint32_t len) {
    if (g_tracePackets && g_packetsLogged.fetch_add(1) < 60) {
        char addr[64];
        DescribeNetAddress(from, addr, sizeof(addr));
        // CNetNub::OnPacket maps the first byte through a runtime table before dispatch (0x37AF7F1D)
        unsigned header = len ? game::At<unsigned char>(0x29668D0)[data[0]] : 0;
        Emitf("nub %p OnPacket from %s len %u first %02X header %u", nub, addr, len, len ? data[0] : 0, header);
    }
    g_origNubOnPacket(nub, from, data, len);
}

using LoadCdfFn = int(__fastcall*)(void* self, const char* path);
LoadCdfFn g_origLoadCdf;

void LogExitStack(const char* what, UINT code);
std::atomic<int> g_cdfRedirects{0};

int __fastcall HookedLoadCdf(void* self, const char* path) {
    if (path && StrStrIA(path, "nanosuit_mp/nanosuit_mp_")) {
        int n = g_cdfRedirects.fetch_add(1);
        if (n < 8) {
            Emitf("cdf %s -> %s (#%d)", path, game::kSpNanosuitCdf, n);
            LogExitStack("cdf redirect caller", n);
        }
        path = game::kSpNanosuitCdf;
    }
    return g_origLoadCdf(self, path);
}

using ClientConnectFn = uintptr_t(__fastcall*)(void* actionGame, void* connParams, void* outOk, void* arg4);
ClientConnectFn g_origClientConnect;

// Listen server experiment (!listenfix on): connect the host's own client like a remote one
// (UDP 127.0.0.1:<lobby port>). Fails while both nubs share the lobby socket ("Corrupted disconnect packet").
uintptr_t __fastcall HookedClientConnect(void* actionGame, void* connParams, void* outOk, void* arg4) {
    auto params = *reinterpret_cast<char**>(static_cast<char*>(actionGame) + 0x58);
    auto flags = reinterpret_cast<uint32_t*>(params + 0xD8);
    auto port = reinterpret_cast<uint16_t*>(params + 0xDC);
    auto host = reinterpret_cast<const char**>(connParams);
    if (!g_listenFix || !(*flags & 2))
        return g_origClientConnect(actionGame, connParams, outOk, arg4);

    uint32_t savedFlags = *flags;
    uint16_t savedPort = *port;
    const char* savedHost = *host;
    *flags &= ~2u;
    *port = game::kLobbyPort;
    *host = "127.0.0.1";
    Emitf("listen server: local client connects to 127.0.0.1:%u instead of <local>:%u", *port, savedPort);
    uintptr_t r = g_origClientConnect(actionGame, connParams, outOk, arg4);
    *flags = savedFlags;
    *port = savedPort;
    *host = savedHost;
    return r;
}

using ScriptBindFn = int(__fastcall*)(void* self, void* funcHandler);
ScriptBindFn g_origRegisterWithAI;

// On the server, register AI as in single player (the server owns the campaign AI); clients keep skipping it.
int __fastcall HookedRegisterWithAI(void* self, void* funcHandler) {
    auto mp = game::At<unsigned char>(game::kRvaMultiplayerFlag);
    bool server = *game::At<unsigned char>(game::kRvaServerFlag) != 0;
    if (!*mp || !server)
        return g_origRegisterWithAI(self, funcHandler);
    *mp = 0;
    int r = g_origRegisterWithAI(self, funcHandler);
    *mp = 1;
    g_aiRegistrations.fetch_add(1);
    return r;
}

using OpenPackCommonFn = bool(__fastcall*)(void* self, const char* bindRoot, const char* fullPath, unsigned flags,
                                           void* data);
OpenPackCommonFn g_origOpenPackCommon;
std::atomic<bool> g_dataPakMounted{false};

// Once our pak is mounted it must stay the last override pak: a later pak opened with FLAGS_OVERRIDE_PAK is opened
// as a normal pak instead, which CryPak inserts below every override pak (so still above the base game paks).
bool __fastcall HookedOpenPackCommon(void* self, const char* bindRoot, const char* fullPath, unsigned flags,
                                     void* data) {
    if (g_dataPakMounted && (flags & game::kPakFlagOverride)) {
        Emitf("pak %s opened below c3coop.pak", fullPath ? fullPath : "?");
        flags &= ~game::kPakFlagOverride;
    }
    return g_origOpenPackCommon(self, bindRoot, fullPath, flags, data);
}

using IsRemoteFn = bool(__fastcall*)(void* actor);
IsRemoteFn g_origActorIsRemote;

void* PlayerEntityClass() {
    static void* playerClass = nullptr;
    if (!playerClass) {
        auto entitySystem = *game::At<void*>(game::kRvaEntitySystemSlot);
        auto registry = game::Virtual<void*(__fastcall*)(void*)>(entitySystem,
                                                                 game::kVtEntitySystemGetClassRegistry)(entitySystem);
        playerClass = game::Virtual<void*(__fastcall*)(void*, const char*)>(
            registry, game::kVtClassRegistryFindClass)(registry, "Player");
    }
    return playerClass;
}

// The server simulates the campaign AI itself: AI actors are local there, as in single player, so their input
// aspect carries their real position to the clients. Players of other machines stay remote.
bool __fastcall HookedActorIsRemote(void* actor) {
    if (!g_origActorIsRemote(actor))
        return false;
    if (!*game::At<unsigned char>(game::kRvaServerFlag))
        return true;
    auto entity = *reinterpret_cast<void**>(static_cast<char*>(actor) + game::kActorEntityOffset);
    return game::Virtual<void*(__fastcall*)(void*)>(entity, game::kVtEntityGetClass)(entity) == PlayerEntityClass();
}

using DogTagLookupFn = void(__fastcall*)(void* self, uint32_t channel, void* out);
DogTagLookupFn g_origDogTagLookup;

void __fastcall HookedDogTagLookup(void* self, uint32_t channel, void* out) {
    auto network = *game::At<void*>(game::kRvaNetworkSlot);
    void* lobby = network ? game::Virtual<void*(__fastcall*)(void*)>(network, 0xE8)(network) : nullptr;
    void* service = lobby ? game::Virtual<void*(__fastcall*)(void*)>(lobby, 0x40)(lobby) : nullptr;
    if (!service)
        return; // offline: no dog tag data (would crash on a NULL service)
    g_origDogTagLookup(self, channel, out);
}

std::atomic<int> g_mpModelSkips{0};

void __fastcall HookedSetMpPlayerModel(void* player) {
    // keep the SP player model from player.lua Properties (nanosuit_v2)
    if (g_mpModelSkips.fetch_add(1) < 8)
        Emitf("skipped MP player model override for player %p", player);
}

void __fastcall HookedLeechSteamCallback(void* self, const int* param) {
    Emitf("blocked Leech/Hydra session start (steam callback, param[1]=%d)", param ? param[1] : -1);
}

void __fastcall HookedUpdate(void* console) {
    g_mainThreadId = GetCurrentThreadId();
    static bool registered = false;
    if (!registered) {
        registered = true; // on the main thread: the console's command map is not locked
        game::Virtual<void(__fastcall*)(void*, const char*, void(__fastcall*)(void*), int, const char*)>(
            console, game::kVtAddCommand)(console, "c3coop", &CoopConsoleCmd, 0, "Co-op mod: c3coop steam status|invite, c3coop tp [host]");
        Emitf("console command \"c3coop\" registered (typed lines)");
        // Lua Log lines ("[c3coop] ..." revives, gates, tp, client messages) only print from verbosity 3; a Steam
        // launch has less, so remote sessions had no Lua trace at all (local test launches pass +log_Verbosity 4)
        void* verbosity = game::Virtual<void*(__fastcall*)(void*, const char*)>(console, game::kVtGetCVar)(console,
                                                                                                         "log_Verbosity");
        if (verbosity && game::Virtual<int(__fastcall*)(void*)>(verbosity, game::kVtCVarGetIVal)(verbosity) < 3) {
            g_origExecuteString(console, "log_Verbosity 3", true, false);
            Emitf("log_Verbosity raised to 3 (Lua log lines)");
        }
    }
    g_origUpdate(console);
    g_updateCalls.fetch_add(1, std::memory_order_relaxed);
    c3coop::HudTick();
    c3coop::CoopTick();
    c3coop::SteamTick();
    c3coop::FgMirrorTick();
    std::string delayedMap;
    {
        std::lock_guard lock(g_cmdMtx);
        if (!g_delayedMap.empty() && GetTickCount64() >= g_delayedMapAt)
            delayedMap.swap(g_delayedMap);
    }
    if (!delayedMap.empty())
        g_origExecuteString(console, delayedMap.c_str(), false, false);

    std::deque<std::string> cmds;
    {
        std::lock_guard lock(g_cmdMtx);
        cmds.swap(g_cmds);
    }
    for (auto& cmd : cmds) {
        if (!cmd.empty() && cmd[0] == '!') {
            RunInternal(console, cmd);
            continue;
        }
        Emitf("> %s", cmd.c_str());
        game::Virtual<ExecuteStringFn>(console, game::kVtExecuteString)(console, cmd.c_str(), false, false);
    }
}

// ---------------------------------------------------------------- pipe server
// The pipe is overlapped: with a synchronous handle a pending ReadFile would block WriteFile.

bool PipeIo(HANDLE pipe, bool write, void* buf, DWORD size, DWORD* done) {
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    BOOL ok = write ? WriteFile(pipe, buf, size, nullptr, &ov) : ReadFile(pipe, buf, size, nullptr, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING)
        ok = TRUE;
    ok = ok && GetOverlappedResult(pipe, &ov, done, TRUE);
    CloseHandle(ov.hEvent);
    return ok != FALSE;
}

void ClientReader(std::shared_ptr<Client> c) {
    std::string pending;
    char buf[1024];
    DWORD got = 0;
    while (PipeIo(c->pipe, false, buf, sizeof(buf), &got) && got) {
        pending.append(buf, got);
        size_t nl;
        while ((nl = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, nl);
            pending.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line.empty())
                continue;
            if (line == "!mainstack") {
                DumpMainStack();
                continue;
            }
            if (line == "!members") { // CNetwork::m_vMembers (+0x148/+0x150, see SyncWithGame 0x37A7D630)
                auto net = *game::At<char*>(game::kRvaNetworkSlot);
                auto begin = net ? *reinterpret_cast<void***>(net + 0x148) : nullptr;
                auto end = net ? *reinterpret_cast<void***>(net + 0x150) : nullptr;
                for (auto it = begin; it && it < end; ++it) {
                    auto vt = *reinterpret_cast<uintptr_t*>(*it);
                    auto m = static_cast<char*>(*it);
                    // CNetNub member interface (vtbl 0x2168ED8): IsDead 0x37B00070 = [+0x78] bDead &&
                    // [+0xF4] keepAliveLocks == 0 && [+0x40] == [+0x48] (no channels)
                    Emitf("member %p vtbl rva %llX: +78 %d +F4 %d +40 %p +48 %p", *it, (unsigned long long)(vt - game::Base()),
                          m[0x78], *reinterpret_cast<int*>(m + 0xF4), *reinterpret_cast<void**>(m + 0x40),
                          *reinterpret_cast<void**>(m + 0x48));
                }
                continue;
            }
            std::lock_guard lock(g_cmdMtx);
            g_cmds.push_back(line);
        }
    }
    std::lock_guard l(c->mtx);
    c->closed = true;
    c->cv.notify_one();
}

void ClientWriter(std::shared_ptr<Client> c) {
    for (;;) {
        std::string line;
        {
            std::unique_lock l(c->mtx);
            c->cv.wait(l, [&] { return c->closed || !c->out.empty(); });
            if (c->closed)
                break;
            line = std::move(c->out.front());
            c->out.pop_front();
        }
        line += '\n';
        DWORD wrote = 0;
        if (!PipeIo(c->pipe, true, line.data(), static_cast<DWORD>(line.size()), &wrote))
            break;
    }
    {
        std::lock_guard lock(g_clientsMtx);
        std::erase(g_clients, c);
    }
    DisconnectNamedPipe(c->pipe);
    CloseHandle(c->pipe);
}

void PipeServer() {
    wchar_t name[64];
    swprintf_s(name, L"\\\\.\\pipe\\c3coop_%lu", GetCurrentProcessId());
    for (;;) {
        HANDLE pipe = CreateNamedPipeW(name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                                       PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, PIPE_UNLIMITED_INSTANCES,
                                       64 * 1024, 64 * 1024, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE)
            return;
        OVERLAPPED ov{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        DWORD unused = 0;
        bool connected = ConnectNamedPipe(pipe, &ov) || GetLastError() == ERROR_PIPE_CONNECTED ||
                         (GetLastError() == ERROR_IO_PENDING && GetOverlappedResult(pipe, &ov, &unused, TRUE));
        CloseHandle(ov.hEvent);
        if (!connected) {
            CloseHandle(pipe);
            continue;
        }
        auto c = std::make_shared<Client>();
        c->pipe = pipe;
        {
            std::lock_guard lock(g_clientsMtx);
            c->out.assign(g_backlog.begin(), g_backlog.end());
            g_clients.push_back(c);
        }
        std::thread(ClientReader, c).detach();
        std::thread(ClientWriter, c).detach();
        Emitf("client connected (pid %lu, update calls=%ld)", GetCurrentProcessId(), g_updateCalls.load());
    }
}

// ---------------------------------------------------------------- crash capture
// Writes <repo>/local/crash/<pid>_<tick>.txt (stack as module+offset) and a minidump next to it.

std::wstring g_crashDir;
std::atomic<bool> g_crashWritten{false};

void DescribeAddress(FILE* f, void* addr) {
    HMODULE mod = nullptr;
    wchar_t path[MAX_PATH] = L"?";
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCWSTR>(addr), &mod))
        GetModuleFileNameW(mod, path, MAX_PATH);
    const wchar_t* name = wcsrchr(path, L'\\');
    fwprintf(f, L"  %p  %s+%llX\n", addr, name ? name + 1 : path,
             (unsigned long long)(reinterpret_cast<uintptr_t>(addr) - reinterpret_cast<uintptr_t>(mod)));
}

LONG WriteCrash(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (g_crashWritten.exchange(true))
        return EXCEPTION_CONTINUE_SEARCH;

    wchar_t base[MAX_PATH];
    swprintf_s(base, L"%s\\%lu_%llu", g_crashDir.c_str(), GetCurrentProcessId(), GetTickCount64());
    FILE* f = nullptr;
    if (_wfopen_s(&f, (std::wstring(base) + L".txt").c_str(), L"w") == 0 && f) {
        fwprintf(f, L"code %08lX at %p, thread %lu, update calls %ld\n", code, ep->ExceptionRecord->ExceptionAddress,
                 GetCurrentThreadId(), g_updateCalls.load());
        DescribeAddress(f, ep->ExceptionRecord->ExceptionAddress);
        fwprintf(f, L"stack:\n");
        void* frames[62];
        USHORT n = RtlCaptureStackBackTrace(0, 62, frames, nullptr);
        for (USHORT i = 0; i < n; ++i)
            DescribeAddress(f, frames[i]);
        fclose(f);
    }
    HANDLE dump = CreateFileW((std::wstring(base) + L".dmp").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (dump != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei{GetCurrentThreadId(), ep, FALSE};
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), dump,
                          static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory),
                          &mei, nullptr, nullptr);
        CloseHandle(dump);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// First-chance access violations are routine (SEH, IsBadReadPtr), so the vectored handler only takes
// fatal heap/stack codes; everything else is caught when it goes unhandled.
std::atomic<int> g_firstChanceLogged{0};
void LogExitStack(const char* what, UINT code);

LONG CALLBACK CrashVeh(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == 0xC0000374 || code == 0xC0000409)
        return WriteCrash(ep);
    // log the first few first-chance faults even if the game handles them (silent exits hide them otherwise)
    if ((code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_STACK_OVERFLOW ||
         code == EXCEPTION_ILLEGAL_INSTRUCTION) && g_firstChanceLogged.fetch_add(1) < 20) {
        auto addr = reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress);
        Emitf("first-chance %08lX at %p (exe+%llX) thread %lu, info %llX %llX, rip=%llX rsp=%llX", code,
              ep->ExceptionRecord->ExceptionAddress, (unsigned long long)(addr - game::Base()), GetCurrentThreadId(),
              (unsigned long long)ep->ExceptionRecord->ExceptionInformation[0],
              (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1], ep->ContextRecord->Rip,
              ep->ContextRecord->Rsp);
        Emitf("  rcx=%llX rdx=%llX r8=%llX [rsp]=%llX (leaf return address if the fault is in a leaf)",
              ep->ContextRecord->Rcx, ep->ContextRecord->Rdx, ep->ContextRecord->R8,
              *reinterpret_cast<unsigned long long*>(ep->ContextRecord->Rsp));
        if (g_firstChanceLogged.load() <= 5)
            LogExitStack("first-chance stack", code);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

LONG WINAPI CrashUnhandled(EXCEPTION_POINTERS* ep) {
    return WriteCrash(ep);
}

void InstallCrashHandler() {
    // the injected copy lives in <repo>/local/inject/, dumps go to <repo>/local/crash/, logs to local/logs/
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(g_module, path, MAX_PATH);
    std::wstring dir(path);
    dir.resize(dir.find_last_of(L'\\'));
    dir.resize(dir.find_last_of(L'\\'));
    g_crashDir = dir + L"\\crash";
    CreateDirectoryW(g_crashDir.c_str(), nullptr);
    CreateDirectoryW((dir + L"\\logs").c_str(), nullptr);
    wchar_t logPath[MAX_PATH];
    swprintf_s(logPath, L"%s\\logs\\%lu.log", dir.c_str(), GetCurrentProcessId());
    g_logFile = _wfsopen(logPath, L"a", _SH_DENYNO); // readable while the game runs
    AddVectoredExceptionHandler(1, CrashVeh);
    SetUnhandledExceptionFilter(CrashUnhandled);
}

// ---------------------------------------------------------------- exit tracing
// The game sometimes ends itself without an error dialog; log who calls ExitProcess/TerminateProcess.

using ExitProcessFn = void(WINAPI*)(UINT);
using TerminateProcessFn = BOOL(WINAPI*)(HANDLE, UINT);
using NtTerminateProcessFn = LONG(NTAPI*)(HANDLE, LONG);
ExitProcessFn g_origExitProcess;
TerminateProcessFn g_origTerminateProcess;
NtTerminateProcessFn g_origNtTerminateProcess;
ExitProcessFn g_origRtlExitUserProcess;


// !mainstack (handled on the pipe thread, works while the main thread hangs): suspend the main thread, unwind
// its stack with the x64 unwind data, resume it, then log the frames.
void DumpMainStack() {
    DWORD tid = g_mainThreadId;
    HANDLE thread = tid ? OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid)
                        : nullptr;
    if (!thread) {
        Emitf("mainstack: main thread unknown");
        return;
    }
    DWORD64 frames[48];
    int n = 0;
    if (SuspendThread(thread) != (DWORD)-1) {
        CONTEXT ctx{};
        ctx.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(thread, &ctx)) {
            for (; n < 48 && ctx.Rip; ++n) {
                frames[n] = ctx.Rip;
                DWORD64 imageBase = 0;
                auto entry = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
                if (!entry) {
                    ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
                    ctx.Rsp += 8;
                } else {
                    PVOID handlerData = nullptr;
                    DWORD64 establisher = 0;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, entry, &ctx, &handlerData, &establisher,
                                     nullptr);
                }
            }
        }
        ResumeThread(thread);
    }
    CloseHandle(thread);
    Emitf("mainstack: thread %lu, %d frames", tid, n);
    for (int i = 0; i < n; ++i) {
        HMODULE mod = nullptr;
        char name[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(frames[i]), &mod))
            GetModuleFileNameA(mod, name, MAX_PATH);
        const char* base = strrchr(name, '\\');
        Emitf("  %016llX  %s+%llX", (unsigned long long)frames[i], base ? base + 1 : name,
              (unsigned long long)(frames[i] - reinterpret_cast<uintptr_t>(mod)));
    }
}

void LogExitStack(const char* what, UINT code) {
    Emitf("%s(%u) called on thread %lu, update calls=%ld; stack:", what, code, GetCurrentThreadId(),
          g_updateCalls.load());
    void* frames[48];
    USHORT n = RtlCaptureStackBackTrace(1, 48, frames, nullptr);
    for (USHORT i = 0; i < n; ++i) {
        HMODULE mod = nullptr;
        char name[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               static_cast<LPCSTR>(frames[i]), &mod))
            GetModuleFileNameA(mod, name, MAX_PATH);
        const char* base = strrchr(name, '\\');
        Emitf("  %p  %s+%llX", frames[i], base ? base + 1 : name,
              (unsigned long long)(reinterpret_cast<uintptr_t>(frames[i]) - reinterpret_cast<uintptr_t>(mod)));
    }
}

void WINAPI HookedExitProcess(UINT code) {
    LogExitStack("ExitProcess", code);
    g_origExitProcess(code);
}

BOOL WINAPI HookedTerminateProcess(HANDLE process, UINT code) {
    if (process == GetCurrentProcess() || GetProcessId(process) == GetCurrentProcessId())
        LogExitStack("TerminateProcess(self)", code);
    return g_origTerminateProcess(process, code);
}

LONG NTAPI HookedNtTerminateProcess(HANDLE process, LONG status) {
    // NULL = "all other threads" (part of a normal exit); only a self-terminate ends the game
    if (process == GetCurrentProcess() || (process && GetProcessId(process) == GetCurrentProcessId()))
        LogExitStack("NtTerminateProcess(self)", static_cast<UINT>(status));
    return g_origNtTerminateProcess(process, status);
}

void WINAPI HookedRtlExitUserProcess(UINT code) {
    LogExitStack("RtlExitUserProcess", code);
    g_origRtlExitUserProcess(code);
}

template <typename Fn>
bool HookExport(const wchar_t* module, const char* name, void* detour, Fn* original) {
    void* target = reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(module), name));
    return target && MH_CreateHook(target, detour, reinterpret_cast<void**>(original)) == MH_OK &&
           MH_EnableHook(target) == MH_OK;
}

void InstallExitTrace() {
    bool ok = HookExport(L"kernel32.dll", "ExitProcess", &HookedExitProcess, &g_origExitProcess) &&
              HookExport(L"kernel32.dll", "TerminateProcess", &HookedTerminateProcess, &g_origTerminateProcess) &&
              HookExport(L"ntdll.dll", "NtTerminateProcess", &HookedNtTerminateProcess, &g_origNtTerminateProcess) &&
              HookExport(L"ntdll.dll", "RtlExitUserProcess", &HookedRtlExitUserProcess, &g_origRtlExitUserProcess);
    Emitf("exit tracing %s", ok ? "on" : "FAILED");
}

// ---------------------------------------------------------------- code patches

void PutAbsJmp(unsigned char* at, uintptr_t target) {
    // jmp qword [rip+0]; dq target
    const unsigned char op[] = {0xFF, 0x25, 0x00, 0x00, 0x00, 0x00};
    memcpy(at, op, sizeof(op));
    memcpy(at + sizeof(op), &target, sizeof(target));
}

// Adds the missing NULL check in CPlayerMovementController::Update (see game.h).
void PatchMoveCtrlNullCheck() {
    auto site = game::At<unsigned char>(game::kRvaMoveCtrlNullSite);
    if (memcmp(site, game::kMoveCtrlNullSiteBytes, sizeof(game::kMoveCtrlNullSiteBytes)) != 0) {
        Emitf("movement controller null check NOT patched (bytes differ)");
        return;
    }
    auto stub = static_cast<unsigned char*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    const unsigned char body[] = {
        0x48, 0x85, 0xC0,                   // test rax,rax
        0x74, 0x22,                         // jz null_path (+34)
        0x48, 0x8B, 0xC8,                   // mov rcx,rax
        0x48, 0x8B, 0x10,                   // mov rdx,[rax]
        0xFF, 0x92, 0x08, 0x02, 0x00, 0x00, // call [rdx+208h]
        0x48, 0x8B, 0xF8,                   // mov rdi,rax
        0x48, 0x85, 0xC0,                   // test rax,rax
        0x74, 0x0E,                         // jz null_path (+14)
    };
    memcpy(stub, body, sizeof(body));                                  // 25 bytes
    PutAbsJmp(stub + 25, game::Base() + game::kRvaMoveCtrlContinue);   // 25..38
    stub[39] = 0x31;                                                   // null_path: xor edi,edi
    stub[40] = 0xFF;
    PutAbsJmp(stub + 41, game::Base() + game::kRvaMoveCtrlNullPath);   // 41..54

    DWORD old;
    VirtualProtect(site, sizeof(game::kMoveCtrlNullSiteBytes), PAGE_EXECUTE_READWRITE, &old);
    unsigned char patch[sizeof(game::kMoveCtrlNullSiteBytes)];
    memset(patch, 0x90, sizeof(patch));
    PutAbsJmp(patch, reinterpret_cast<uintptr_t>(stub));
    memcpy(site, patch, sizeof(patch));
    VirtualProtect(site, sizeof(game::kMoveCtrlNullSiteBytes), old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, sizeof(patch));
    Emitf("movement controller null check patched at %p (stub %p)", site, stub);
}

// ---------------------------------------------------------------- data pak
// c3ctl copies build/c3coop.pak next to the injected dll (same name, .pak); mount it over C3/ as early as
// possible so the overrides are in place before the game reads its scripts.

using OpenPackFn = bool(__fastcall*)(void* pak, const char* bindRoot, const char* path, unsigned flags, void* block,
                                     void* fullPath);

void MountDataPak() {
    char path[MAX_PATH];
    GetModuleFileNameA(g_module, path, MAX_PATH);
    char* dot = strrchr(path, '.');
    if (!dot)
        return;
    strcpy_s(dot, path + MAX_PATH - dot, ".pak");
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
        Emitf("no data pak next to the dll (%s)", path);
        return;
    }
    void* pak = nullptr;
    for (int i = 0; i < 3000 && !(pak = *game::At<void*>(game::kRvaCryPakSlot)); ++i)
        Sleep(10);
    for (int i = 0; i < 3000 && pak && !*reinterpret_cast<void**>(static_cast<char*>(pak) + game::kCryPakLogOffset); ++i)
        Sleep(10);
    if (!pak || !*reinterpret_cast<void**>(static_cast<char*>(pak) + game::kCryPakLogOffset)) {
        Emitf("gEnv->pCryPak (or its log) never appeared, data pak not mounted");
        return;
    }
    // FLAGS_OVERRIDE_PAK: placed after the game's override (patch/cache) paks, so its files win every lookup
    bool ok = game::Virtual<OpenPackFn>(pak, game::kVtCryPakOpenPack)(pak, "C3/", path, game::kPakFlagOverride, nullptr,
                                                                       nullptr);
    g_dataPakMounted = ok;
    Emitf("data pak %s: %s", path, ok ? "mounted over C3/" : "OpenPack FAILED");
}

// Classic (non-lobby) handshake for connections that arrive through CNetNub::OnPacket (see game.h).
void SetClassicHandshake(bool on) {
    struct Site {
        uintptr_t rva;
        unsigned char disp;
    } sites[] = {{game::kRvaHsConnectionSetupJe, 0x2B},
                 {game::kRvaHsKeyExchange1Je, 0x12},
                 {game::kRvaHsClassicLobbyJe, 0x28},
                 {game::kRvaHsSessionCheckJe, 0x5D}};
    for (auto& s : sites) {
        auto p = game::At<unsigned char>(s.rva);
        if ((p[0] != 0x74 && p[0] != 0xEB) || p[1] != s.disp) {
            Emitf("classic handshake: unexpected bytes at %p, not patched", p);
            return;
        }
    }
    for (auto& s : sites) {
        auto p = game::At<unsigned char>(s.rva);
        DWORD old;
        VirtualProtect(p, 1, PAGE_EXECUTE_READWRITE, &old);
        p[0] = on ? 0xEB : 0x74;
        VirtualProtect(p, 1, old, &old);
        FlushInstructionCache(GetCurrentProcess(), p, 2);
    }
    Emitf("classic handshake for OnPacket connections %s", on ? "on" : "off");
}

// ---------------------------------------------------------------- init

void Init() {
    InstallCrashHandler();
    std::thread(PipeServer).detach();

    if (game::TimeDateStamp() != game::kTimeDateStamp) {
        Emitf("unexpected exe build (TimeDateStamp %08X), hooks not installed", game::TimeDateStamp());
        return;
    }
    if (MH_Initialize() != MH_OK) {
        Emitf("MH_Initialize failed");
        return;
    }
    // The priority hook goes in before the pak is mounted: with an early injection the game opened patch1-3.pak (and
    // _fastload/startup.pak) as override paks between the mount and a later hook, above c3coop.pak, so that machine
    // ran the game's scripts instead of the mod's and its net messages did not match the host's.
    auto openPack = game::At<unsigned char>(game::kRvaOpenPackCommon);
    if (memcmp(openPack, game::kOpenPackCommonBytes, sizeof(game::kOpenPackCommonBytes)) == 0 &&
        MH_CreateHook(openPack, &HookedOpenPackCommon, reinterpret_cast<void**>(&g_origOpenPackCommon)) == MH_OK &&
        MH_EnableHook(openPack) == MH_OK)
        Emitf("data pak priority kept at %p", openPack);
    else
        Emitf("data pak priority hook NOT installed");
    std::thread(MountDataPak).detach();
    InstallExitTrace();
    c3coop::InstallHudHooks();
    c3coop::InstallAnimHooks();
    c3coop::InstallCoopHooks();
    c3coop::InstallSteam();
    c3coop::InstallSpFeatures();
    c3coop::InstallFgMirror();

    auto leech = game::At<unsigned char>(game::kRvaLeechSteamCallback);
    if (memcmp(leech, game::kLeechSteamCallbackBytes, sizeof(game::kLeechSteamCallbackBytes)) == 0 &&
        MH_CreateHook(leech, &HookedLeechSteamCallback, nullptr) == MH_OK && MH_EnableHook(leech) == MH_OK)
        Emitf("Leech steam callback disabled at %p", leech);
    else
        Emitf("Leech steam callback NOT disabled (bytes differ or hook failed)");

    PatchMoveCtrlNullCheck();
    SetClassicHandshake(true);

    auto onPacket = game::At<unsigned char>(game::kRvaNubOnPacket);
    if (memcmp(onPacket, game::kNubOnPacketBytes, sizeof(game::kNubOnPacketBytes)) == 0 &&
        MH_CreateHook(onPacket, &HookedNubOnPacket, reinterpret_cast<void**>(&g_origNubOnPacket)) == MH_OK &&
        MH_EnableHook(onPacket) == MH_OK)
        Emitf("CNetNub::OnPacket traced at %p (!tracepackets on)", onPacket);

    auto openSocket = game::At<unsigned char>(game::kRvaOpenSocket);
    if (memcmp(openSocket, game::kOpenSocketBytes, sizeof(game::kOpenSocketBytes)) == 0 &&
        MH_CreateHook(openSocket, &HookedOpenSocket, reinterpret_cast<void**>(&g_origOpenSocket)) == MH_OK &&
        MH_EnableHook(openSocket) == MH_OK)
        Emitf("socket manager OpenSocket hooked at %p", openSocket);
    else
        Emitf("socket manager OpenSocket NOT hooked");

    auto clientConnect = game::At<unsigned char>(game::kRvaActionGameClientConnect);
    if (memcmp(clientConnect, game::kActionGameClientConnectBytes, sizeof(game::kActionGameClientConnectBytes)) == 0 &&
        MH_CreateHook(clientConnect, &HookedClientConnect, reinterpret_cast<void**>(&g_origClientConnect)) == MH_OK &&
        MH_EnableHook(clientConnect) == MH_OK)
        Emitf("listen-server local connect redirected at %p", clientConnect);
    else
        Emitf("listen-server local connect NOT redirected");

    auto dogTag = game::At<unsigned char>(game::kRvaDogTagLookup);
    if (memcmp(dogTag, game::kDogTagLookupBytes, sizeof(game::kDogTagLookupBytes)) == 0 &&
        MH_CreateHook(dogTag, &HookedDogTagLookup, reinterpret_cast<void**>(&g_origDogTagLookup)) == MH_OK &&
        MH_EnableHook(dogTag) == MH_OK)
        Emitf("offline dog tag lookup guarded at %p", dogTag);
    else
        Emitf("dog tag lookup guard NOT installed");

    auto isRemote = game::At<unsigned char>(game::kRvaActorIsRemote);
    if (memcmp(isRemote, game::kActorIsRemoteBytes, sizeof(game::kActorIsRemoteBytes)) == 0 &&
        MH_CreateHook(isRemote, &HookedActorIsRemote, reinterpret_cast<void**>(&g_origActorIsRemote)) == MH_OK &&
        MH_EnableHook(isRemote) == MH_OK)
        Emitf("server-side AI actors kept local at %p", isRemote);
    else
        Emitf("AI actor locality hook NOT installed");

    auto regAI = game::At<unsigned char>(game::kRvaRegisterWithAI);
    if (memcmp(regAI, game::kRegisterWithAIBytes, sizeof(game::kRegisterWithAIBytes)) == 0 &&
        MH_CreateHook(regAI, &HookedRegisterWithAI, reinterpret_cast<void**>(&g_origRegisterWithAI)) == MH_OK &&
        MH_EnableHook(regAI) == MH_OK)
        Emitf("server-side AI registration enabled at %p", regAI);
    else
        Emitf("server-side AI registration NOT enabled");

    auto setMpModel = game::At<unsigned char>(game::kRvaSetMpPlayerModel);
    if (memcmp(setMpModel, game::kSetMpPlayerModelBytes, sizeof(game::kSetMpPlayerModelBytes)) == 0 &&
        MH_CreateHook(setMpModel, &HookedSetMpPlayerModel, nullptr) == MH_OK && MH_EnableHook(setMpModel) == MH_OK)
        Emitf("MP player model override disabled at %p", setMpModel);
    else
        Emitf("MP player model override NOT disabled");

    auto loadCdf = game::At<unsigned char>(game::kRvaLoadCdf);
    if (memcmp(loadCdf, game::kLoadCdfBytes, sizeof(game::kLoadCdfBytes)) == 0 &&
        MH_CreateHook(loadCdf, &HookedLoadCdf, reinterpret_cast<void**>(&g_origLoadCdf)) == MH_OK &&
        MH_EnableHook(loadCdf) == MH_OK)
        Emitf("MP nanosuit cdf redirected to SP nanosuit at %p", loadCdf);
    else
        Emitf("MP nanosuit cdf redirect NOT installed");

    auto createNub = game::At<unsigned char>(game::kRvaNetworkCreateNub);
    if (memcmp(createNub, game::kNetworkCreateNubBytes, sizeof(game::kNetworkCreateNubBytes)) == 0 &&
        MH_CreateHook(createNub, &HookedCreateNub, reinterpret_cast<void**>(&g_origCreateNub)) == MH_OK &&
        MH_EnableHook(createNub) == MH_OK)
        Emitf("CNetwork::CreateNub traced at %p", createNub);
    else
        Emitf("CNetwork::CreateNub NOT traced");

    auto mapCmd = game::At<unsigned char>(game::kRvaMapCmd);
    if (memcmp(mapCmd, game::kMapCmdBytes, sizeof(game::kMapCmdBytes)) == 0 &&
        MH_CreateHook(mapCmd, &HookedMapCmd, reinterpret_cast<void**>(&g_origMapCmd)) == MH_OK &&
        MH_EnableHook(mapCmd) == MH_OK)
        Emitf("console \"map\" lines hosted too (MapCmd %p)", mapCmd);
    else
        Emitf("MapCmd NOT hooked: a \"map\" typed in the console starts a local-only game");

    void* console = nullptr;
    for (int i = 0; i < 600 && !(console = game::Console()); ++i)
        Sleep(100);
    if (!console) {
        Emitf("gEnv->pConsole is still null, giving up");
        return;
    }
    auto vtbl = *reinterpret_cast<uintptr_t*>(console);
    if (vtbl != game::Base() + game::kRvaConsoleVtable) {
        Emitf("console vtable %llX does not match CXConsole, hooks not installed", (unsigned long long)vtbl);
        return;
    }

    auto update = game::Virtual<void*>(console, game::kVtUpdate);
    if (MH_CreateHook(update, &HookedUpdate, reinterpret_cast<void**>(&g_origUpdate)) != MH_OK ||
        MH_EnableHook(update) != MH_OK) {
        Emitf("failed to hook IConsole::Update");
        return;
    }

    auto execute = game::Virtual<void*>(console, game::kVtExecuteString);
    if (MH_CreateHook(execute, &HookedExecuteString, reinterpret_cast<void**>(&g_origExecuteString)) == MH_OK &&
        MH_EnableHook(execute) == MH_OK)
        Emitf("console commands \"c3coop ...\" routed to the DLL (ExecuteString %p)", execute);
    else
        Emitf("ExecuteString NOT hooked: \"c3coop ...\" commands unavailable");

    *game::At<int>(game::kRvaConRestricted) = 0;
    game::Virtual<SinkFn>(console, game::kVtAddOutputPrintSink)(console, &g_sink);
    Emitf("ready: console %p, Update hooked at %p, con_restricted=0", console, update);
}

} // namespace

// Runs a console line with ISystem::IsDevMode forced on for its duration (Lua '#' lines, cheat cvars).
void c3coop::ExecuteDev(const std::string& line) {
    void* console = game::Console();
    if (!console || line.empty())
        return;
    if (!InstallDevModeHook(console))
        return;
    ++g_devModeDepth;
    g_origExecuteString(console, line.c_str(), true, false);
    --g_devModeDepth;
}

void c3coop::RunLua(const std::string& code) {
    if (code.empty())
        return;
    ++g_luaQuiet;
    ExecuteDev("#" + code);
    --g_luaQuiet;
}

void c3coop::Logf(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Emit(std::string("[c3coop] ") + buf);
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        DisableThreadLibraryCalls(module);
        std::thread(Init).detach();
    }
    return TRUE;
}
