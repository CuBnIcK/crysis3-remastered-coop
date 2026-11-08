// Flowgraph output mirror: server <-> client activations of flow node outputs that fire on one machine only.
//
// Every machine runs the whole level flowgraph, but some outputs only fire on one machine:
//   - server-only sources: AISequence:* (and other AI) nodes only work where the AI system runs, i.e. on the server.
//     Their outputs often drive local-player nodes (Fields AB0/00_LeadIn: AISequence:End.Done -> Actor:PlayerStaging /
//     PlayerCinematicControl / GameTokenSet ...), so on clients these chains never run;
//   - server-authoritative ports: entity:AIWave Dead/BodyCount/ActiveCount/Spawned, Dead/Spawned of AI actors,
//     Actor:OnDeath of AI victims ("port rules": the class itself is not server-only, its inputs still run on clients),
//     and random nodes in graphs whose owner matches a pattern (Cave boss: every machine would pick its own attack);
//   - uplink classes (Hacking:*): fire only on the machine of the player who acted.
// This module:
//   server: a server-only/authoritative output that has at least one link into a node that is not server-only ->
//           queue "<graph owner entity>,<node>,<port>,<value>,<class>,<owner field>" and send batches to all clients
//           over the coop.cpp text channel ("c3coop:fgmirror a <rec>|<rec>...", < 200 chars, rate-limited);
//   client: find the same graph (owner entity id, checked by the owner field and the node's class; falls back to the
//           owner field) and activate the input ports that the node's output links to, skipping server-only nodes.
//           Local outputs of server-only/authoritative outputs are suppressed on clients.
//   uplink: an uplink-class output on a client runs locally and is sent to the server through Lua
//           (g_localActor.server:C3ToServer("fg <records>"), records with ^XX / ~ instead of %XX / ;); the server's Lua
//           runs "c3coop fgmirror u <channel> <records>": the server activates the output's links through the original
//           (un-hooked) activation and relays the record to all clients except that channel ("fgmirror ax <ch> ...";
//           every client learns its own channel from "fgmirror me <ch>", sent on join and on its first uplink).
//           An uplink-class output on the server (the host acted) is mirrored to all clients.
//           Loops/doubles: per (graph, node, port) "fired or applied recently" marks (window, default 3 s): the server
//           drops an uplink of an output it fired/applied recently, a client drops a record of an output it fired
//           recently and suppresses its own output when the server's copy was applied recently; outputs firing while
//           a record is being applied are never uplinked.
//   late join: the last record per (graph owner, node, port) is kept; "replay <channel>" sends them (except those whose
//           links reach staging/cinematic/token/player-position nodes) through a separate queue, then asks the server's
//           Lua to resync tokens for that player (C3CoopTokenResyncFor(channel)).
// EntityId values travel as "n<id>;<entity name>" (resolved by name on the receiver, id as fallback) or "e<id>".
//
// Hooks: the 7 typed CFlowGraph::DoActivatePort variants + ActivatePortAny + ActivatePortCString (all output activation
// entry points of IFlowGraph).
//
// Commands: pipe "!fgmirror ...", console / Lua / network "c3coop fgmirror ...":
//   status | on | off | suppress on|off | verbose on|off | window <ms>
//   classes                              all class lists and port rules
//   mirror|skip|uplink|random|rgraph|replayskip|replayplayer add|del <pattern>   (patterns: '*' wildcards, no case)
//       mirror: server-only sources; skip: never activated on clients; uplink: client -> server sources;
//       random: random node classes, mirrored only in graphs whose owner name matches an rgraph pattern;
//       replayskip: records linked to these classes are not replayed; replayplayer: same when fed by Actor:LocalPlayer
//   port add <class> <port|#index|*>[,<port>...] [ai] | port del <class>   server-authoritative output ports
//   graphs [owner name filter]           list flow graphs + an order-independent checksum (compare host and client)
//   node <owner name|entity id> <node id>   class of a node, its output ports and links
//   fire <owner name|entity id> <node id> <port>   server: mirror that output (void) to clients; client: apply locally
//   a <records> | ax <channel> <records> | me <channel>   (network, clients) apply / apply unless from that channel /
//                                        this client's channel id on the server
//   u <channel> <records>                (server, from Lua) uplinked records of that client
//   replay <channel>                     (server, from Lua C3CoopJoinReplay) late-join replay for that client

#include <windows.h>

#include <shlwapi.h>

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
#include <tuple>
#include <vector>

#include "MinHook.h"

#include "game.h"
#include "modules.h"

namespace {

// ---------------------------------------------------------------- addresses (RVA = VA - 0x37000000)
// CFlowGraph vtable (ctor 0x3742A4E0) and a class derived from it (ctor 0x3743FBD0); both share every function used here.
constexpr uintptr_t kRvaVtFlowGraph = 0x20DADD0;
constexpr uintptr_t kRvaVtFlowGraphDerived = 0x20DC470;
// IFlowGraph vtable offsets (both vtables)
constexpr size_t kVtGetGraphEntity = 0x58;      // EntityId (int index) -> this->[0x130 + 4*index]
constexpr size_t kVtGetNodeTypeId = 0xE8;       // flowData[id].+0x38 & 0xFFF
constexpr size_t kVtGetNodeTypeName = 0xF0;     // m_pSys->typeRegistry[typeId].name
constexpr size_t kVtActivatePortAny = 0x168;    // (SFlowAddress, const TFlowInputData&)
constexpr size_t kVtActivatePortCString = 0x170; // (SFlowAddress, const char*)
// CFlowGraphBase fields
constexpr size_t kGraphFlowDataBegin = 0xA8; // vector<CFlowData>, 0x40 bytes each
constexpr size_t kGraphFlowDataEnd = 0xB0;
constexpr size_t kGraphEdgesBegin = 0xD8; // vector<SEdge> {u16 fromNode, u16 toNode, u8 fromPort, u8 toPort}
constexpr size_t kGraphEdgesEnd = 0xE0;
constexpr size_t kGraphEnabled = 0xF0;
constexpr size_t kGraphActive = 0xF1;
constexpr size_t kGraphSystem = 0xF8; // CFlowSystem*
// CFlowSystem::m_graphs (vector<IFlowGraph*>, entries may be NULL while the system iterates)
constexpr size_t kSystemGraphsBegin = 0x40;
constexpr size_t kSystemGraphsEnd = 0x48;
// CFlowData::GetConfiguration(this = flowData, SFlowNodeConfig&) 0x37427F40 (calls flowData+0x18 IFlowNode vtbl+0x20).
// CFlowGraph::ResolveAddress 0x3742B7B0 (port names of the level XML links -> SFlowAddress) shows the layout:
// config 0x28 bytes {+0 SInputPortConfig* (0x30 each), +8 SOutputPortConfig* (0x20 each, name first), +0x10 flags
// (0x40 before the call)}, output count = byte flowData+0x31, input count = byte flowData+0x30.
constexpr uintptr_t kRvaFlowDataGetConfiguration = 0x427F40;
constexpr size_t kFlowDataOutputCount = 0x31;
constexpr size_t kConfigOutputs = 0x08;
constexpr size_t kConfigFlags = 0x10;
constexpr size_t kOutputPortConfigSize = 0x20;
// IEntitySystem::GetEntity, FindEntityByName (callers pass "AB3_AIWave1", "ThisIsNorth" ... then IEntity vtbl+8),
// IEntity::GetId, GetName
constexpr size_t kVtEntitySystemGetEntity = 0x58;
constexpr size_t kVtEntitySystemFindEntityByName = 0x60;
constexpr size_t kVtEntityGetId = 0x08;
constexpr size_t kVtEntityGetName = 0x60;

// Activation entry points. The typed DoActivatePort functions live in the IFlowSystemTyped sub-objects of CFlowGraph
// (one per flow data type, in reverse type order) and get the sub-object pointer as `this`.
enum Kind { kBool, kString, kVec3, kEntity, kFloat, kInt, kVoid, kAny, kCString, kKinds };
struct Site {
    const char* name;
    uintptr_t rva;
    size_t subobject;  // `this` - graph
    uintptr_t vtRva;   // vtable of that sub-object in CFlowGraph (slot 1 = rva); 0: main vtable slot vtSlot
    size_t vtSlot;
    unsigned char sig[22];
    size_t sigLen;
};
// clang-format off
const Site kSites[kKinds] = {
    {"DoActivatePort<bool>",   0x430A60, 0x00, 0x20DADD0, 0x08,  {0x89,0x54,0x24,0x10,0x4C,0x8B,0xDC,0x55,0x57,0x41,0x57,0x48,0x83,0xEC,0x50,0x80,0xB9,0xF1,0x00,0x00,0x00,0x00}, 22},
    {"DoActivatePort<string>", 0x430CF0, 0x08, 0x20DAF98, 0x08,  {0x89,0x54,0x24,0x10,0x4C,0x8B,0xDC,0x55,0x57,0x41,0x55,0x48,0x83,0xEC,0x50,0x48,0x8D,0x69,0xF8,0x8B,0xFA,0x80}, 22},
    {"DoActivatePort<Vec3>",   0x430F80, 0x10, 0x20DAFA8, 0x08,  {0x89,0x54,0x24,0x10,0x4C,0x8B,0xDC,0x55,0x57,0x41,0x57,0x48,0x83,0xEC,0x50,0x48,0x8D,0x69,0xF0,0x8B,0xFA,0x80}, 22},
    {"DoActivatePort<EntityId>",0x431220,0x18, 0x20DAFB8, 0x08,  {0x89,0x54,0x24,0x10,0x4C,0x8B,0xDC,0x55,0x57,0x41,0x57,0x48,0x83,0xEC,0x50,0x48,0x8D,0x69,0xE8,0x8B,0xFA,0x80}, 22},
    {"DoActivatePort<float>",  0x4314C0, 0x20, 0x20DADA0, 0x08,  {0x89,0x54,0x24,0x10,0x4C,0x8B,0xDC,0x55,0x57,0x41,0x57,0x48,0x83,0xEC,0x50,0x48,0x8D,0x69,0xE0,0x8B,0xFA,0x80}, 22},
    {"DoActivatePort<int>",    0x431760, 0x28, 0x20DADB0, 0x08,  {0x89,0x54,0x24,0x10,0x4C,0x8B,0xDC,0x55,0x57,0x41,0x57,0x48,0x83,0xEC,0x50,0x48,0x8D,0x69,0xD8,0x8B,0xFA,0x80}, 22},
    {"DoActivatePort<void>",   0x431A00, 0x30, 0x20DADC0, 0x08,  {0x89,0x54,0x24,0x10,0x4C,0x8B,0xDC,0x55,0x57,0x41,0x55,0x48,0x83,0xEC,0x50,0x48,0x8D,0x69,0xD0,0x8B,0xFA,0x80}, 22},
    {"ActivatePortAny",        0x42DEE0, 0x00, 0x20DADD0, kVtActivatePortAny,     {0x89,0x54,0x24,0x10,0x4C,0x8B,0xDC,0x55,0x57,0x41,0x55,0x48,0x83,0xEC,0x50,0x80,0xB9,0xF1,0x00,0x00,0x00,0x00}, 22},
    {"ActivatePortCString",    0x42E170, 0x00, 0x20DADD0, kVtActivatePortCString, {0x48,0x89,0x5C,0x24,0x18,0x89,0x54,0x24,0x10,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57}, 20},
};
// clang-format on

// Further code facts the raw field accesses rely on (checked at install).
struct Check {
    uintptr_t rva;
    unsigned char bytes[48];
    size_t len;
    const char* what;
};
const Check kChecks[] = {
    // GetGraphEntity: cmp edx,1 / ja / movsxd rax,edx / mov eax,[rcx+rax*4+130h]
    {0x42B370, {0x83, 0xFA, 0x01, 0x77, 0x0B, 0x48, 0x63, 0xC2, 0x8B, 0x84, 0x81, 0x30, 0x01, 0x00, 0x00, 0xC3}, 16,
     "GetGraphEntity"},
    // GetNodeTypeId: flowData [rcx+A8h]..[rcx+B0h], 0x40 per node
    {0x42CFA0, {0x4C, 0x8B, 0x89, 0xA8, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x81, 0xB0, 0x00, 0x00, 0x00, 0x49, 0x2B, 0xC1}, 17,
     "GetNodeTypeId"},
    {0x42CFE0, {0x4C, 0x8B, 0x89, 0xA8, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x81, 0xB0, 0x00, 0x00, 0x00, 0x49, 0x2B, 0xC1}, 17,
     "GetNodeTypeName"},
    // DoActivatePort<bool> edge walk: mov rax,[rbp+A8h] ... mov rsi,[rbp+E0h] (edges end) ... mov rax,[rbp+D8h] (begin)
    {0x430B05, {0x48, 0x8B, 0x85, 0xA8, 0x00, 0x00, 0x00, 0x44, 0x0F, 0xB6, 0x64, 0x24, 0x7A, 0x48, 0x8B, 0xB5, 0xE0, 0x00,
                0x00, 0x00}, 20, "edge walk (m_edges +D8h/+E0h)"},
    // CFlowGraphBase ctor: mov rcx,[rdi+F8h] / add rcx,40h -> m_pSys->m_graphs.push_back(this)
    {0x42A97A, {0x48, 0x8B, 0x8F, 0xF8, 0x00, 0x00, 0x00, 0x48, 0x83, 0xC1, 0x40}, 11, "graph registration (m_pSys+40h)"},
    // CFlowData::GetConfiguration: push rbp/rsi, lea rbp,[rsp-4Fh], sub rsp,B8h, mov rcx,[rcx+18h] (IFlowNode)
    {0x427F40, {0x40, 0x55, 0x56, 0x48, 0x8D, 0x6C, 0x24, 0xB1, 0x48, 0x81, 0xEC, 0xB8, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x49,
                0x18}, 19, "CFlowData::GetConfiguration"},
    // ResolveAddress: config.flags = 40h, call GetConfiguration, output loop over byte [flowData+31h] with
    // config+8 [index*20h] = name
    {0x42B921, {0xC7, 0x44, 0x24, 0x30, 0x40, 0x00, 0x00, 0x00, 0xC5, 0xFA, 0x7F, 0x44, 0x24, 0x20, 0xC5, 0xFA,
                0x7F, 0x4C, 0x24, 0x38, 0xE8, 0x06, 0xC6, 0xFF, 0xFF, 0x0F, 0xB6, 0xAC, 0x24, 0xA0, 0x00, 0x00,
                0x00, 0x33, 0xDB, 0x40, 0x84, 0xED, 0x74, 0x5A, 0x38, 0x5E, 0x31, 0x76, 0x32, 0x66, 0x90, 0x48}, 48,
     "output port config layout (ResolveAddress)"},
};

// Main vtable slots used through the object (both graph classes).
struct Slot {
    size_t offset;
    uintptr_t rva;
};
const Slot kMainSlots[] = {{0x08, 0x430A60},
                           {kVtGetGraphEntity, 0x42B370},
                           {kVtGetNodeTypeId, 0x42CFA0},
                           {kVtGetNodeTypeName, 0x42CFE0},
                           {kVtActivatePortAny, 0x42DEE0},
                           {kVtActivatePortCString, 0x42E170}};

// ---------------------------------------------------------------- state (main thread unless noted)
using ActivateFn = void(__fastcall*)(void* self, uint32_t addr, const void* value);
ActivateFn g_orig[kKinds];
std::atomic<bool> g_installed{false};
std::atomic<bool> g_enabled{true};
bool g_suppress = true;  // clients drop local server-authoritative outputs
bool g_verbose = true;
int g_logBudget = 400;   // verbose lines left (reset by "verbose on")
void* g_flowSystem;      // CFlowSystem, captured from graphs seen in the hooks
ULONGLONG g_windowMs = 3000; // "fired/applied recently" window of uplink-class outputs
bool g_applying = false;     // a received record is being applied (its effects are never uplinked)
int g_myChannel = 0;         // client: this client's channel id on the server ("fgmirror me")

// Animations:AnimationEventListener: events of AI characters' animations (Psycho's hand scan opens the Fields intro
// airlock), which play on the server only; listeners on the local player stay local (FedByLocalPlayer)
std::vector<std::string> g_mirrorClasses = {"AISequence:*", "AI:GroupAlertness", "AI:AlertnessState",
                                            "AI:AIAwarenessToPlayer", "Animations:AnimationEventListener"};
std::vector<std::string> g_skipClasses = {"AISequence:*", "AI:*", "AIAssignments:*"};
std::vector<std::string> g_uplinkClasses = {"Hacking:*"};
std::vector<std::string> g_randomClasses = {"Logic:RandomSelect", "Logic:RandomTrigger", "Time:RandomDelay",
                                            "Math:Random"};
// Cave (all-level model): AB3A_Boss/Boss_B_Main, Boss_C_Actions, Boss_D_Control, Boss_E_Tagging, Boss_X_Transition,
// Boss_Y_Drilling, Boss_Z_Encounter, Cinematics/20_Boss, Boss_FG_holder, Flash_BossFight, Dynamics.ICV_BossArena1
std::vector<std::string> g_randomGraphs = {"*Boss*"};
// late-join replay: never replay records whose links reach these classes
std::vector<std::string> g_replaySkip = {"Actor:Player*", "Actor:KillPlayer", "Mission:GameTokenSet",
                                         "Mission:GameTokenModify", "Animations:PlaySequence"};
// ... or these, when their entity comes from Actor:LocalPlayer (moving the player)
std::vector<std::string> g_replaySkipPlayer = {"Movement:*", "Entity:EntityPos", "Entity:BeamEntity",
                                               "Entity:EntityFaceAt"};

// Server-authoritative output ports of classes that are not server-only (their inputs still run on clients).
struct PortRule {
    std::string cls;
    std::vector<std::string> ports; // port names, "#<index>" or "*"
    bool aiOnly;                    // only when the victim is not a player (Actor:OnDeath)
};
std::vector<PortRule> g_portRules = {
    {"entity:AIWave", {"Dead", "BodyCount", "ActiveCount", "Spawned"}, false},
    {"entity:Human", {"Dead", "Spawned"}, false},
    {"entity:AlienGrunt", {"Dead", "Spawned"}, false},
    {"entity:AlienHeavy", {"Dead", "Spawned"}, false},
    {"entity:AlienPinger", {"Dead", "Spawned"}, false},
    {"entity:AlienStalker", {"Dead", "Spawned"}, false},
    {"entity:Scorcher", {"Dead", "Spawned"}, false},
    {"entity:Drone", {"Dead", "Spawned"}, false},
    {"entity:Mastermind", {"Dead", "Spawned"}, false},
    {"Actor:OnDeath", {"*"}, true},
};

enum : unsigned char { kResolved = 1, kMirror = 2, kSkip = 4, kUplink = 8, kPortRule = 16, kRandom = 32 };
unsigned char g_classCache[4096]; // by flow node type id
struct PortInfo {
    bool done;
    uint64_t mask;   // ports covered by a port rule
    uint64_t aiMask; // ... with the AI-victim condition
};
PortInfo g_portInfo[4096];                          // by flow node type id
std::map<int, std::vector<std::string>> g_portNames; // output port names of port-rule classes, by type id
std::map<void*, std::pair<uint32_t, bool>> g_randomGraphCache; // graph -> (owner, owner matches g_randomGraphs)
std::map<std::pair<void*, uint16_t>, std::pair<ULONGLONG, bool>> g_lastVictim; // Actor:OnDeath: victim is AI

struct Mark {
    ULONGLONG time;
    bool applied; // applied from a record (else fired here)
};
std::map<std::tuple<void*, uint16_t, uint8_t>, Mark> g_marks; // uplink-class outputs fired/applied recently

struct Stats {
    unsigned seen, queued, noEntity, noTargets, sent, messages, dropped, byPortRule, byRandom, hostUplinkClass; // server
    unsigned received, applied, inputs, unresolved, inactive, suppressed, ownRelayIgnored, dupDropped,
        nameFallback;                                                                                    // client
    unsigned upQueued, upSent, upMessages, upDropped, upTooLong, upSkippedApplying, upSuppressedDup; // client uplink
    unsigned upReceived, upApplied, upDup, upUnresolved, upRejected, upInactive, upRelayed;         // server uplink
    unsigned replays, replayQueued, replaySkipped, replaySent, historyFull;                          // server replay
} g_stats;

std::mutex g_outMtx;
struct Outgoing {
    int channel; // 0 = all remote clients
    int exclude; // with channel 0: not for this channel ("fgmirror ax")
    bool raw;    // a whole "fgmirror ..." command (not a record)
    std::string record;
};
std::deque<Outgoing> g_outbox; // live records waiting for the tick (server)
struct ReplayItem {
    int channel;
    std::string record; // empty: end of that channel's replay
};
std::deque<ReplayItem> g_replayQ; // late-join replays (server), sent after the live records
std::map<int, std::pair<unsigned, unsigned>> g_replayCount; // channel -> (records queued, history keys)
std::deque<std::string> g_uplinkQ; // uplink records in wire form (client)
// last record per (graph owner, node, port, owner field), in firing order (server)
std::map<std::string, std::pair<unsigned long long, std::string>> g_last;
unsigned long long g_lastSeq = 0;
std::vector<int> g_toldChannels; // channels that got "fgmirror me" in this level (server)
constexpr size_t kHistoryMax = 8192;
ULONGLONG g_nextSend = 0;
constexpr size_t kOutboxMax = 512;
constexpr size_t kUplinkQueueMax = 128;
constexpr size_t kMessageMax = 190; // "c3coop:" + message stays < 200 chars
constexpr size_t kUplinkMax = 135;  // records per Lua line: the whole "#if g_localActor ... end" stays < 200 chars
constexpr int kMessagesPerSlot = 3;
constexpr ULONGLONG kSlotMs = 100;
constexpr size_t kOwnerFieldMax = 48; // escaped owner name, cut the same way on both sides
constexpr size_t kTextMax = 60;       // escaped string values / entity names (server records)
constexpr size_t kUplinkTextMax = 24; // ... in uplink records

void Log(const char* fmt, ...) {
    char buf[900];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    c3coop::Logf("fgmirror: %s", buf);
}

bool VerboseLine() {
    if (!g_verbose)
        return false;
    if (g_logBudget > 0) {
        --g_logBudget;
        return true;
    }
    if (g_logBudget == 0) {
        --g_logBudget;
        Log("verbose log budget used up (\"!fgmirror verbose on\" for more); counters in \"!fgmirror status\"");
    }
    return false;
}

bool IsServer() {
    return *game::At<unsigned char>(game::kRvaServerFlag) != 0;
}
bool IsMultiplayer() {
    return *game::At<unsigned char>(game::kRvaMultiplayerFlag) != 0;
}

bool KnownGraph(const void* graph) {
    if (!graph)
        return false;
    uintptr_t vt = *static_cast<const uintptr_t*>(graph);
    return vt == game::Base() + kRvaVtFlowGraph || vt == game::Base() + kRvaVtFlowGraphDerived;
}

// ---------------------------------------------------------------- SEH-guarded raw access (POD only)
struct Edge {
    uint16_t from, to;
    uint8_t fromPort, toPort;
};
static_assert(sizeof(Edge) == 6, "SEdge is 6 bytes");

unsigned SafeNodeCount(const char* graph) {
    __try {
        auto b = *reinterpret_cast<const char* const*>(graph + kGraphFlowDataBegin);
        auto e = *reinterpret_cast<const char* const*>(graph + kGraphFlowDataEnd);
        return b && e >= b ? static_cast<unsigned>((e - b) >> 6) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

int SafeEdgeCount(const char* graph) {
    __try {
        auto b = *reinterpret_cast<const char* const*>(graph + kGraphEdgesBegin);
        auto e = *reinterpret_cast<const char* const*>(graph + kGraphEdgesEnd);
        return b && e >= b ? static_cast<int>((e - b) / sizeof(Edge)) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// Output links of (node, port): number found (out holds up to max), -1 on a fault.
int SafeCollectEdges(const char* graph, uint16_t node, uint8_t port, Edge* out, int max) {
    __try {
        auto b = *reinterpret_cast<const Edge* const*>(graph + kGraphEdgesBegin);
        auto e = *reinterpret_cast<const Edge* const*>(graph + kGraphEdgesEnd);
        if (!b || e < b || e - b > 1000000)
            return -1;
        int n = 0;
        for (auto p = b; p < e; ++p) {
            if (p->from == node && p->fromPort == port) {
                if (n < max)
                    out[n] = *p;
                ++n;
            }
        }
        return n;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

uint32_t SafeGraphEntity(void* graph) {
    __try {
        return game::Virtual<uint32_t(__fastcall*)(void*, int)>(graph, kVtGetGraphEntity)(graph, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

int SafeNodeTypeId(void* graph, uint16_t node) {
    __try {
        return game::Virtual<uint16_t(__fastcall*)(void*, uint16_t)>(graph, kVtGetNodeTypeId)(graph, node) & 0xFFF;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// Class name of a node (NULL for a node id outside the graph or a fault).
const char* SafeNodeClass(void* graph, uint16_t node) {
    if (node >= SafeNodeCount(static_cast<const char*>(graph)))
        return nullptr;
    __try {
        return game::Virtual<const char*(__fastcall*)(void*, uint16_t)>(graph, kVtGetNodeTypeName)(graph, node);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// Output port names of a node (CFlowData::GetConfiguration): count (<= max), -1 on a fault.
using GetConfigurationFn = void(__fastcall*)(void* flowData, void* config);
constexpr size_t kPortNameMax = 48;
int SafeOutputNames(const char* graph, uint16_t node, char (*out)[kPortNameMax], int max) {
    if (node >= SafeNodeCount(graph))
        return -1;
    __try {
        auto flowData = *reinterpret_cast<char* const*>(graph + kGraphFlowDataBegin) + (size_t(node) << 6);
        int count = *reinterpret_cast<const unsigned char*>(flowData + kFlowDataOutputCount);
        alignas(16) unsigned char config[0x40];
        memset(config, 0, sizeof(config));
        *reinterpret_cast<uint32_t*>(config + kConfigFlags) = 0x40;
        reinterpret_cast<GetConfigurationFn>(game::Base() + kRvaFlowDataGetConfiguration)(flowData, config);
        auto outputs = *reinterpret_cast<const char* const*>(config + kConfigOutputs);
        if (!outputs)
            return 0;
        int n = 0;
        for (; n < count && n < max; ++n) {
            const char* name = *reinterpret_cast<const char* const*>(outputs + n * kOutputPortConfigSize);
            if (!name)
                break;
            strncpy_s(out[n], kPortNameMax, name, _TRUNCATE);
        }
        return n;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

void* SafeEntity(uint32_t id) {
    auto entitySystem = *game::At<void*>(game::kRvaEntitySystemSlot);
    if (!id || !entitySystem)
        return nullptr;
    __try {
        return game::Virtual<void*(__fastcall*)(void*, uint32_t)>(entitySystem, kVtEntitySystemGetEntity)(entitySystem,
                                                                                                          id);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

const char* SafeEntityName(uint32_t id) {
    void* entity = SafeEntity(id);
    if (!entity)
        return nullptr;
    __try {
        return game::Virtual<const char*(__fastcall*)(void*)>(entity, kVtEntityGetName)(entity);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

uint32_t SafeFindEntityId(const char* name) {
    auto entitySystem = *game::At<void*>(game::kRvaEntitySystemSlot);
    if (!name || !*name || !entitySystem)
        return 0;
    __try {
        auto entity = game::Virtual<void*(__fastcall*)(void*, const char*)>(
            entitySystem, kVtEntitySystemFindEntityByName)(entitySystem, name);
        return entity ? game::Virtual<uint32_t(__fastcall*)(void*)>(entity, kVtEntityGetId)(entity) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// An entity of class "Player" (same lookup as coop.cpp IsPlayerEntity).
bool SafeIsPlayer(uint32_t id) {
    auto entitySystem = *game::At<void*>(game::kRvaEntitySystemSlot);
    void* entity = SafeEntity(id);
    if (!entity || !entitySystem)
        return false;
    static void* playerClass = nullptr;
    __try {
        if (!playerClass) {
            auto registry = game::Virtual<void*(__fastcall*)(void*)>(
                entitySystem, game::kVtEntitySystemGetClassRegistry)(entitySystem);
            playerClass = game::Virtual<void*(__fastcall*)(void*, const char*)>(
                registry, game::kVtClassRegistryFindClass)(registry, "Player");
        }
        return playerClass && game::Virtual<void*(__fastcall*)(void*)>(entity, game::kVtEntityGetClass)(entity) ==
                                  playerClass;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Copies CFlowSystem::m_graphs (skipping NULL and unknown graph classes).
size_t SafeCopyGraphs(void* system, void** out, size_t max) {
    if (!system)
        return 0;
    __try {
        auto b = *reinterpret_cast<void* const* const*>(static_cast<char*>(system) + kSystemGraphsBegin);
        auto e = *reinterpret_cast<void* const* const*>(static_cast<char*>(system) + kSystemGraphsEnd);
        if (!b || e < b || e - b > 100000)
            return 0;
        size_t n = 0;
        for (auto p = b; p < e && n < max; ++p)
            if (KnownGraph(*p))
                out[n++] = *p;
        return n;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// A flow value as read from an activation.
struct RawValue {
    char tag; // v b i f e p s
    unsigned char b;
    int32_t i;
    float f;
    uint32_t e;
    float v[3];
    char s[128];
};

void CopyStr(char* dst, const char* src) {
    size_t n = 0;
    if (src)
        for (; src[n] && n + 1 < sizeof(RawValue::s); ++n)
            dst[n] = src[n];
    dst[n] = 0;
}

// EFlowDataTypes (type id in the TFlowInputData type descriptor) -> Kind
constexpr int kTypeToKind[] = {kVoid, kInt, kFloat, kEntity, kVec3, kString, kBool};

bool SafeReadValue(int kind, const void* p, RawValue* out) {
    memset(out, 0, sizeof(*out));
    out->tag = 'v';
    __try {
        if (kind == kAny) {
            // TFlowInputData: [+0] type descriptor pointer (low 2 bits = flags, desc+0 = type id), [+8] storage
            auto desc = *static_cast<const uintptr_t*>(p) & ~uintptr_t(3);
            int type = desc ? *reinterpret_cast<const int*>(desc) : 0;
            auto storage = static_cast<const char*>(p) + 8;
            if (type < 0 || type > 6)
                return true;
            kind = kTypeToKind[type];
            if (kind == kVec3) {
                p = *reinterpret_cast<const float* const*>(storage); // Vec3 is stored out of line
                if (!p)
                    return true;
            } else {
                p = storage; // string storage is a CryString (char*), like DoActivatePort<string>'s value
            }
        }
        switch (kind) {
        case kBool: out->tag = 'b'; out->b = *static_cast<const unsigned char*>(p) != 0; break;
        case kInt: out->tag = 'i'; out->i = *static_cast<const int32_t*>(p); break;
        case kFloat: out->tag = 'f'; out->f = *static_cast<const float*>(p); break;
        case kEntity: out->tag = 'e'; out->e = *static_cast<const uint32_t*>(p); break;
        case kVec3: out->tag = 'p'; memcpy(out->v, p, sizeof(out->v)); break;
        case kString: out->tag = 's'; CopyStr(out->s, *static_cast<const char* const*>(p)); break;
        case kCString: out->tag = 's'; CopyStr(out->s, static_cast<const char*>(p)); break;
        default: break;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out->tag = 'v';
        return false;
    }
}

// ---------------------------------------------------------------- class lists
// '*' wildcard match, case-insensitive ("AISequence:*", "*Boss*", "Math:Random")
bool WildMatch(const char* p, const char* s) {
    while (*p) {
        if (*p == '*') {
            while (*p == '*')
                ++p;
            if (!*p)
                return true;
            for (; *s; ++s)
                if (WildMatch(p, s))
                    return true;
            return false;
        }
        if (!*s || tolower(static_cast<unsigned char>(*p)) != tolower(static_cast<unsigned char>(*s)))
            return false;
        ++p;
        ++s;
    }
    return !*s;
}

bool Matches(const std::vector<std::string>& list, const char* cls) {
    if (!cls)
        return false;
    for (auto& pattern : list)
        if (!pattern.empty() && WildMatch(pattern.c_str(), cls))
            return true;
    return false;
}

unsigned char ClassFlagsByName(const char* cls) {
    unsigned char f = kResolved;
    if (!cls)
        return f;
    if (Matches(g_mirrorClasses, cls))
        f |= kMirror;
    if (Matches(g_skipClasses, cls))
        f |= kSkip;
    if (Matches(g_uplinkClasses, cls))
        f |= kUplink;
    if (Matches(g_randomClasses, cls))
        f |= kRandom;
    for (auto& rule : g_portRules)
        if (WildMatch(rule.cls.c_str(), cls))
            f |= kPortRule;
    return f;
}

unsigned char ClassFlags(void* graph, uint16_t node) {
    int type = SafeNodeTypeId(graph, node);
    if (type < 0)
        return kResolved;
    unsigned char& c = g_classCache[type];
    if (!c)
        c = ClassFlagsByName(SafeNodeClass(graph, node));
    return c;
}

void ResetCaches() {
    memset(g_classCache, 0, sizeof(g_classCache));
    memset(g_portInfo, 0, sizeof(g_portInfo));
    g_portNames.clear();
    g_randomGraphCache.clear();
}

// Output port names of the node's class (cached by type id).
const std::vector<std::string>& OutputNames(void* graph, uint16_t node, int type) {
    auto it = g_portNames.find(type);
    if (it != g_portNames.end())
        return it->second;
    static char names[64][kPortNameMax];
    int n = SafeOutputNames(static_cast<const char*>(graph), node, names, 64);
    std::vector<std::string> v;
    for (int i = 0; i < n; ++i)
        v.emplace_back(names[i]);
    return g_portNames[type] = v;
}

const char* OutputName(void* graph, uint16_t node, uint8_t port) {
    int type = SafeNodeTypeId(graph, node);
    if (type < 0)
        return nullptr;
    auto& names = OutputNames(graph, node, type);
    return port < names.size() ? names[port].c_str() : nullptr;
}

// Ports of the node's class covered by port rules (cached by type id).
const PortInfo& PortBits(void* graph, uint16_t node) {
    static PortInfo none{};
    int type = SafeNodeTypeId(graph, node);
    if (type < 0)
        return none;
    PortInfo& info = g_portInfo[type];
    if (info.done)
        return info;
    info.done = true;
    const char* cls = SafeNodeClass(graph, node);
    if (!cls)
        return info;
    auto& names = OutputNames(graph, node, type);
    std::string list;
    for (auto& rule : g_portRules) {
        if (!WildMatch(rule.cls.c_str(), cls))
            continue;
        for (auto& p : rule.ports) {
            uint64_t bits = 0;
            if (p == "*") {
                bits = names.size() >= 64 ? ~0ull : (1ull << names.size()) - 1;
                if (names.empty())
                    bits = ~0ull; // names unknown: every port
            } else if (p.size() > 1 && p[0] == '#') {
                int index = atoi(p.c_str() + 1);
                if (index >= 0 && index < 64)
                    bits = 1ull << index;
            } else {
                for (size_t i = 0; i < names.size() && i < 64; ++i)
                    if (_stricmp(names[i].c_str(), p.c_str()) == 0)
                        bits |= 1ull << i;
            }
            info.mask |= bits;
            if (rule.aiOnly)
                info.aiMask |= bits;
        }
    }
    for (size_t i = 0; i < 64; ++i)
        if (info.mask & (1ull << i)) {
            char buf[80];
            sprintf_s(buf, " %zu:%s%s", i, i < names.size() ? names[i].c_str() : "?",
                      info.aiMask & (1ull << i) ? "(ai)" : "");
            list += buf;
        }
    std::string all;
    for (size_t i = 0; i < names.size(); ++i)
        all += (i ? "," : "") + names[i];
    Log("port rule %s: outputs [%s], server-authoritative:%s", cls, all.c_str(), list.empty() ? " NONE" : list.c_str());
    return info;
}

// A random node's graph whose owner name matches g_randomGraphs.
bool RandomGraph(void* graph) {
    uint32_t owner = SafeGraphEntity(graph);
    auto it = g_randomGraphCache.find(graph);
    if (it != g_randomGraphCache.end() && it->second.first == owner)
        return it->second.second;
    const char* name = SafeEntityName(owner);
    bool match = owner && name && Matches(g_randomGraphs, name);
    g_randomGraphCache[graph] = {owner, match};
    if (match)
        Log("random nodes of graph '%s' (%u) are server-authoritative", name, owner);
    return match;
}

// A node whose input is linked from Actor:LocalPlayer watches this machine's own player (an AnimationEventListener on
// the player's kick animation, a tutorial step): every machine runs it for itself, it is never mirrored.
bool FedByLocalPlayer(const char* graph, uint16_t node) {
    __try {
        auto b = *reinterpret_cast<const Edge* const*>(graph + kGraphEdgesBegin);
        auto e = *reinterpret_cast<const Edge* const*>(graph + kGraphEdgesEnd);
        if (!b || e < b || e - b > 1000000)
            return false;
        for (auto p = b; p < e; ++p)
            if (p->to == node) {
                const char* cls = SafeNodeClass(const_cast<char*>(graph), p->from);
                if (cls && strcmp(cls, "Actor:LocalPlayer") == 0)
                    return true;
            }
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// A mirror class node that runs on the server only (not one watching the local player).
bool MirrorNode(void* graph, uint16_t node) {
    return (ClassFlags(graph, node) & kMirror) && !FedByLocalPlayer(static_cast<const char*>(graph), node);
}

// Destinations a client must not activate: server-only nodes (mirror classes) and AI-system nodes.
bool ServerOnlyTarget(void* graph, uint16_t node) {
    return (ClassFlags(graph, node) & kSkip) || MirrorNode(graph, node);
}

// Actor:OnDeath (port rule "ai"): the "victim" output decides (AI = not a Player entity); the other outputs of the
// same node follow the last victim decision (within 1 s), else count as AI.
bool AiVictimOutput(void* graph, uint16_t node, uint8_t port, int kind, const void* value) {
    const char* name = OutputName(graph, node, port);
    auto key = std::make_pair(graph, node);
    ULONGLONG now = GetTickCount64();
    if (name && _stricmp(name, "victim") == 0) {
        RawValue v;
        SafeReadValue(kind, value, &v);
        bool ai = v.tag != 'e' || (v.e && !SafeIsPlayer(v.e));
        if (g_lastVictim.size() > 256)
            g_lastVictim.clear();
        g_lastVictim[key] = {now, ai};
        return ai;
    }
    auto it = g_lastVictim.find(key);
    return it == g_lastVictim.end() || now - it->second.first > 1000 || it->second.second;
}

enum Role { kLocal, kServerAuth, kUplinkRole };

// Who owns this output: every machine (local), the server (mirrored to clients, suppressed on them) or the machine
// where it fires (uplink classes).
Role OutputRole(char* graph, uint16_t node, uint8_t port, int kind, const void* value, const char** why) {
    unsigned char f = ClassFlags(graph, node);
    if (!(f & (kMirror | kUplink | kPortRule | kRandom)))
        return kLocal;
    if (f & kUplink) {
        *why = "uplink class";
        return kUplinkRole;
    }
    if ((f & kMirror) && !FedByLocalPlayer(graph, node)) {
        *why = "server-only class";
        return kServerAuth;
    }
    if (f & kPortRule) {
        const PortInfo& info = PortBits(graph, node);
        uint64_t bit = port < 64 ? 1ull << port : 0;
        if (info.mask & bit) {
            if ((info.aiMask & bit) && !AiVictimOutput(graph, node, port, kind, value))
                return kLocal;
            *why = "port rule";
            return kServerAuth;
        }
    }
    if ((f & kRandom) && RandomGraph(graph)) {
        *why = "random in a matching graph";
        return kServerAuth;
    }
    return kLocal;
}

// ---------------------------------------------------------------- recently fired / applied (uplink classes)
bool Recent(void* graph, uint16_t node, uint8_t port, int which /* 0 any, 1 fired here, 2 applied */) {
    auto it = g_marks.find({graph, node, port});
    if (it == g_marks.end() || GetTickCount64() - it->second.time > g_windowMs)
        return false;
    return which == 0 || (which == 2) == it->second.applied;
}

void SetMark(void* graph, uint16_t node, uint8_t port, bool applied) {
    ULONGLONG now = GetTickCount64();
    if (g_marks.size() > 256)
        for (auto it = g_marks.begin(); it != g_marks.end();)
            it = now - it->second.time > g_windowMs ? g_marks.erase(it) : std::next(it);
    g_marks[{graph, node, port}] = {now, applied};
}

// ---------------------------------------------------------------- wire format
// record: <owner entity id>,<node>,<port>,<value>,<class>,<owner field>; records joined by '|'
// value: v | b0 b1 | i<int> | f<float> | e<entity id> | n<entity id>;<entity name> | p<x>;<y>;<z> | s<text>
// text fields are %XX-escaped; the owner field is the escaped owner name cut to kOwnerFieldMax (compared as is).
// Uplink (Lua console path, which mangles '%'): '%' -> '^' and ';' -> '~' (both are always escaped in text).
std::string Escape(const char* s, size_t max = std::string::npos) {
    std::string out;
    for (; s && *s; ++s) {
        unsigned char c = *s;
        bool esc = c <= ' ' || c >= 0x7F || strchr("%,|;^~\"\\'", c);
        if (out.size() + (esc ? 3 : 1) > max)
            break;
        if (esc) {
            char hex[4];
            sprintf_s(hex, "%%%02X", c);
            out += hex;
        } else {
            out += static_cast<char>(c);
        }
    }
    return out;
}

std::string Unescape(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            out += static_cast<char>(strtol(s.substr(i + 1, 2).c_str(), nullptr, 16));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

std::string OwnerField(const char* ownerName) {
    return Escape(ownerName ? ownerName : "", kOwnerFieldMax);
}

std::string ToUplinkForm(std::string s) {
    for (char& c : s)
        c = c == '%' ? '^' : c == ';' ? '~' : c;
    return s;
}

std::string FromUplinkForm(std::string s) {
    for (char& c : s)
        c = c == '^' ? '%' : c == '~' ? ';' : c;
    return s;
}

std::string EncodeValue(const RawValue& v, size_t textMax = kTextMax) {
    char buf[96];
    switch (v.tag) {
    case 'b': return v.b ? "b1" : "b0";
    case 'i': sprintf_s(buf, "i%d", v.i); return buf;
    case 'f': sprintf_s(buf, "f%.9g", v.f); return buf;
    case 'e': {
        // entity ids of players and spawned AI differ between machines: the name is resolved on the receiver
        const char* name = SafeEntityName(v.e);
        sprintf_s(buf, name && *name ? "n%u;" : "e%u", v.e);
        return name && *name ? buf + Escape(name, textMax) : std::string(buf);
    }
    case 'p': sprintf_s(buf, "p%.9g;%.9g;%.9g", v.v[0], v.v[1], v.v[2]); return buf;
    case 's': return "s" + Escape(v.s, textMax);
    default: return "v";
    }
}

bool DecodeValue(const std::string& text, RawValue* v) {
    memset(v, 0, sizeof(*v));
    v->tag = text.empty() ? 'v' : text[0];
    const char* rest = text.c_str() + (text.empty() ? 0 : 1);
    switch (v->tag) {
    case 'v': return true;
    case 'b': v->b = atoi(rest) != 0; return true;
    case 'i': v->i = atoi(rest); return true;
    case 'f': v->f = static_cast<float>(atof(rest)); return true;
    case 'e': v->e = strtoul(rest, nullptr, 10); return true;
    case 'n': {
        char* end = nullptr;
        uint32_t id = strtoul(rest, &end, 10);
        std::string name = end && *end == ';' ? Unescape(end + 1) : "";
        uint32_t byName = SafeFindEntityId(name.c_str());
        if (!byName) {
            ++g_stats.nameFallback;
            if (VerboseLine())
                Log("entity '%s' not found by name, using id %u", name.c_str(), id);
        }
        v->tag = 'e';
        v->e = byName ? byName : id;
        return true;
    }
    case 'p': return sscanf_s(rest, "%f;%f;%f", &v->v[0], &v->v[1], &v->v[2]) == 3;
    case 's': CopyStr(v->s, Unescape(rest).c_str()); return true;
    default: return false;
    }
}

std::vector<std::string> Split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t start = 0;
    for (;;) {
        size_t p = s.find(sep, start);
        out.push_back(s.substr(start, p == std::string::npos ? std::string::npos : p - start));
        if (p == std::string::npos)
            return out;
        start = p + 1;
    }
}

// "<owner>,<node>,<port>,<value>,<class>,<owner field>" for this graph; empty when the graph has no owner entity
std::string BuildRecord(void* graph, uint16_t node, uint8_t port, const RawValue& v, const char* cls,
                        size_t textMax = kTextMax) {
    uint32_t owner = SafeGraphEntity(graph);
    if (!owner)
        return {};
    char head[64];
    sprintf_s(head, "%u,%u,%u,", owner, node, port);
    return head + EncodeValue(v, textMax) + "," + cls + "," + OwnerField(SafeEntityName(owner));
}

// Parsed record.
struct Record {
    uint32_t owner;
    uint16_t node;
    uint8_t port;
    std::string value, cls, ownerField;
};

bool ParseRecord(const std::string& text, Record* r) {
    auto f = Split(text, ',');
    if (f.size() < 6)
        return false;
    r->owner = strtoul(f[0].c_str(), nullptr, 10);
    r->node = static_cast<uint16_t>(strtoul(f[1].c_str(), nullptr, 10));
    r->port = static_cast<uint8_t>(strtoul(f[2].c_str(), nullptr, 10));
    r->value = f[3];
    r->cls = f[4];
    r->ownerField = f[5];
    return true;
}

// ---------------------------------------------------------------- server: capture
void QueueRecord(const std::string& record, int channel = 0, int exclude = 0) {
    std::lock_guard lock(g_outMtx);
    if (g_outbox.size() >= kOutboxMax) {
        g_outbox.pop_front();
        ++g_stats.dropped;
    }
    g_outbox.push_back({channel, exclude, false, record});
    ++g_stats.queued;
    if (channel)
        return;
    // late-join history: the last record per (graph owner, node, port)
    Record r;
    if (!ParseRecord(record, &r))
        return;
    char key[64];
    sprintf_s(key, "%u,%u,%u,", r.owner, r.node, r.port);
    std::string k = key + r.ownerField;
    auto it = g_last.find(k);
    if (it != g_last.end())
        it->second = {++g_lastSeq, record};
    else if (g_last.size() < kHistoryMax)
        g_last[k] = {++g_lastSeq, record};
    else
        ++g_stats.historyFull;
}

void QueueRaw(const std::string& command, int channel) {
    std::lock_guard lock(g_outMtx);
    g_outbox.push_back({channel, 0, true, command});
}

// Tells a client its channel id (it ignores relays of its own uplinks), once per level and channel.
void TellChannel(int channel) {
    if (std::find(g_toldChannels.begin(), g_toldChannels.end(), channel) != g_toldChannels.end())
        return;
    g_toldChannels.push_back(channel);
    QueueRaw("me " + std::to_string(channel), channel);
}

void MirrorOutput(int kind, char* graph, uint16_t node, uint8_t port, const void* value, const char* cls,
                  const char* why) {
    ++g_stats.seen;
    uint32_t owner = SafeGraphEntity(graph);
    if (!owner) {
        if (++g_stats.noEntity <= 10)
            Log("skip #%u %s port %u: graph %p has no owner entity (not identifiable on clients)", node, cls, port,
                graph);
        return;
    }
    Edge edges[64];
    int n = SafeCollectEdges(graph, node, port, edges, 64);
    int targets = 0;
    for (int i = 0; i < n && i < 64; ++i)
        if (!ServerOnlyTarget(graph, edges[i].to))
            ++targets;
    if (targets == 0) {
        ++g_stats.noTargets; // unlinked, or only feeds other server-only nodes (Start.Link -> Move ...)
        return;
    }
    RawValue v;
    SafeReadValue(kind, value, &v);
    std::string record = BuildRecord(graph, node, port, v, cls);
    QueueRecord(record);
    const char* ownerName = SafeEntityName(owner);
    if (VerboseLine())
        Log("server: %s(%u) #%u %s out%u %s -> %d client target(s) queued (%s)", ownerName ? ownerName : "?", owner,
            node, cls, port, EncodeValue(v).c_str(), targets, why);
}

// Client: an uplink-class output fired here (it runs here too): send it to the server.
void UplinkOutput(int kind, char* graph, uint16_t node, uint8_t port, const void* value, const char* cls) {
    Edge edges[1];
    if (SafeCollectEdges(graph, node, port, edges, 1) <= 0) {
        ++g_stats.noTargets;
        return;
    }
    RawValue v;
    SafeReadValue(kind, value, &v);
    std::string record = BuildRecord(graph, node, port, v, cls, kUplinkTextMax);
    if (record.empty()) {
        ++g_stats.noEntity;
        Log("client: uplink #%u %s out%u: graph %p has no owner entity, not sent", node, cls, port, graph);
        return;
    }
    std::string wire = ToUplinkForm(record);
    if (wire.size() > kUplinkMax) {
        ++g_stats.upTooLong;
        Log("client: uplink record too long (%zu > %zu chars), NOT sent: %s", wire.size(), kUplinkMax, wire.c_str());
        return;
    }
    {
        std::lock_guard lock(g_outMtx);
        if (g_uplinkQ.size() >= kUplinkQueueMax) {
            g_uplinkQ.pop_front();
            ++g_stats.upDropped;
        }
        g_uplinkQ.push_back(wire);
    }
    ++g_stats.upQueued;
    Log("client: uplink queued %s", record.c_str());
}

// true: drop the activation
bool OnOutput(int kind, char* graph, uint32_t addr, const void* value) {
    if (!IsMultiplayer() || !KnownGraph(graph))
        return false;
    void* system = *reinterpret_cast<void**>(graph + kGraphSystem);
    if (system)
        g_flowSystem = system;
    auto node = static_cast<uint16_t>(addr & 0xFFFF);
    auto port = static_cast<uint8_t>((addr >> 16) & 0xFF);
    const char* why = "";
    Role role = OutputRole(graph, node, port, kind, value, &why);
    if (role == kLocal)
        return false;
    const char* cls = SafeNodeClass(graph, node);
    bool live = graph[kGraphEnabled] && graph[kGraphActive] && cls;
    if (role == kUplinkRole) {
        if (!live)
            return false;
        if (IsServer()) {
            // the host acted: like a mirror class, to every client
            SetMark(graph, node, port, false);
            ++g_stats.hostUplinkClass;
            MirrorOutput(kind, graph, node, port, value, cls, why);
            return false;
        }
        if (g_applying) {
            ++g_stats.upSkippedApplying; // effect of a record from the server: runs, never sent back
            return false;
        }
        if (Recent(graph, node, port, 2)) {
            // the server's copy of this output was applied here a moment ago: do not run it twice
            ++g_stats.upSuppressedDup;
            Log("client: local #%u %s out%u suppressed (the server's copy was applied %llu ms ago or less)", node, cls,
                port, g_windowMs);
            return true;
        }
        SetMark(graph, node, port, false);
        UplinkOutput(kind, graph, node, port, value, cls);
        return false;
    }
    if (!IsServer()) {
        if (!g_suppress)
            return false;
        if (++g_stats.suppressed <= 20 || VerboseLine())
            Log("client: suppressed local output #%u %s out%u (%s: the server mirrors it)", node, cls ? cls : "?", port,
                why);
        return true;
    }
    if (live) {
        if (strcmp(why, "port rule") == 0)
            ++g_stats.byPortRule;
        else if (why[0] == 'r')
            ++g_stats.byRandom;
        MirrorOutput(kind, graph, node, port, value, cls, why);
    }
    return false;
}

template <int K>
void __fastcall Detour(void* self, uint32_t addr, const void* value) {
    if ((addr >> 24) & 0xFF) { // SFlowAddress.isOutput
        if (g_enabled.load(std::memory_order_relaxed) &&
            OnOutput(K, static_cast<char*>(self) - kSites[K].subobject, addr, value))
            return;
    }
    g_orig[K](self, addr, value);
}

void* const kDetours[kKinds] = {
    reinterpret_cast<void*>(&Detour<kBool>),   reinterpret_cast<void*>(&Detour<kString>),
    reinterpret_cast<void*>(&Detour<kVec3>),   reinterpret_cast<void*>(&Detour<kEntity>),
    reinterpret_cast<void*>(&Detour<kFloat>),  reinterpret_cast<void*>(&Detour<kInt>),
    reinterpret_cast<void*>(&Detour<kVoid>),   reinterpret_cast<void*>(&Detour<kAny>),
    reinterpret_cast<void*>(&Detour<kCString>),
};

// ---------------------------------------------------------------- apply
void* FlowSystem() {
    return g_flowSystem;
}

std::vector<void*> Graphs() {
    std::vector<void*> graphs(8192);
    graphs.resize(SafeCopyGraphs(FlowSystem(), graphs.data(), graphs.size()));
    return graphs;
}

bool SameText(const char* a, const char* b) {
    return a && b && strcmp(a, b) == 0;
}

// The graph owned by entity `owner` (checked by the owner field when it has one) whose node `node` has class `cls`;
// falls back to the owner field when the entity id differs on this machine.
void* ResolveGraph(uint32_t owner, const std::string& ownerField, uint16_t node, const char* cls, int* candidates) {
    void* byId = nullptr;
    void* byName = nullptr;
    *candidates = 0;
    for (void* g : Graphs()) {
        uint32_t ent = SafeGraphEntity(g);
        if (!ent || (cls && !SameText(SafeNodeClass(g, node), cls)))
            continue;
        bool nameMatch = !ownerField.empty() && OwnerField(SafeEntityName(ent)) == ownerField;
        if (owner && ent == owner && (ownerField.empty() || nameMatch)) {
            ++*candidates;
            if (!byId)
                byId = g;
        } else if (nameMatch) {
            ++*candidates;
            if (!byName)
                byName = g;
        }
    }
    return byId ? byId : byName;
}

// Kind of the typed entry point for a decoded value, and the argument pointer.
int ValueKind(const RawValue& v, const void** arg) {
    static unsigned char dummy = 0;
    switch (v.tag) {
    case 'b': *arg = &v.b; return kBool;
    case 'i': *arg = &v.i; return kInt;
    case 'f': *arg = &v.f; return kFloat;
    case 'e': *arg = &v.e; return kEntity;
    case 'p': *arg = v.v; return kVec3;
    case 's': *arg = v.s; return kCString;
    default: *arg = &dummy; return kVoid;
    }
}

// Activates one input port of `graph` with the value through the graph's own typed entry point.
bool ActivateInput(void* graph, uint16_t node, uint8_t port, const RawValue& v) {
    uint32_t addr = node | (uint32_t(port) << 16); // isOutput = 0
    const void* arg;
    int kind = ValueKind(v, &arg);
    char* self = static_cast<char*>(graph) + kSites[kind].subobject;
    uintptr_t expected = game::Base() + kSites[kind].rva;
    size_t slot = kind == kCString ? kVtActivatePortCString : 0x08;
    if ((*reinterpret_cast<uintptr_t**>(self))[slot / sizeof(uintptr_t)] != expected)
        return false; // another graph class: do not guess
    reinterpret_cast<ActivateFn>(expected)(self, addr, arg);
    return true;
}

// Server: activates an output of `graph` through the original (un-hooked) entry point: every link runs (AI too),
// and the hook never sees it as a new server activation.
bool ActivateOutputOriginal(void* graph, uint16_t node, uint8_t port, const RawValue& v) {
    uint32_t addr = node | (uint32_t(port) << 16) | (1u << 24); // isOutput = 1
    const void* arg;
    int kind = ValueKind(v, &arg);
    if (!KnownGraph(graph) || !g_orig[kind])
        return false;
    g_orig[kind](static_cast<char*>(graph) + kSites[kind].subobject, addr, arg);
    return true;
}

// Client: apply a record from the server.
void ApplyRecord(const std::string& text) {
    ++g_stats.received;
    Record r;
    if (!ParseRecord(text, &r)) {
        Log("client: bad record '%s'", text.c_str());
        return;
    }
    RawValue v;
    if (!DecodeValue(r.value, &v)) {
        Log("client: bad value in '%s'", text.c_str());
        return;
    }
    std::string ownerName = Unescape(r.ownerField);
    int candidates = 0;
    void* graph = ResolveGraph(r.owner, r.ownerField, r.node, r.cls.c_str(), &candidates);
    if (!graph) {
        ++g_stats.unresolved;
        Log("client: UNRESOLVED %s(%u) #%u %s out%u (no graph with that owner and node class; %s)", ownerName.c_str(),
            r.owner, r.node, r.cls.c_str(), r.port, FlowSystem() ? "flow system known" : "flow system not seen yet");
        return;
    }
    auto g = static_cast<char*>(graph);
    if (!g[kGraphEnabled] || !g[kGraphActive]) {
        ++g_stats.inactive;
        Log("client: %s(%u) #%u %s out%u: graph %p is %s here, activation dropped", ownerName.c_str(), r.owner, r.node,
            r.cls.c_str(), r.port, graph, !g[kGraphEnabled] ? "disabled" : "inactive");
        return;
    }
    bool uplinkClass = ClassFlags(graph, r.node) & kUplink;
    if (uplinkClass) {
        if (Recent(graph, r.node, r.port, 0)) {
            ++g_stats.dupDropped;
            Log("client: %s #%u %s out%u fired/applied here within %llu ms, record dropped (double)", ownerName.c_str(),
                r.node, r.cls.c_str(), r.port, g_windowMs);
            return;
        }
        SetMark(graph, r.node, r.port, true);
    }
    Edge edges[64];
    int n = SafeCollectEdges(g, r.node, r.port, edges, 64);
    std::string done;
    int applied = 0, skipped = 0;
    g_applying = true;
    for (int i = 0; i < n && i < 64; ++i) {
        if (ServerOnlyTarget(graph, edges[i].to)) {
            ++skipped;
            continue;
        }
        if (ActivateInput(graph, edges[i].to, edges[i].toPort, v)) {
            ++applied;
            char buf[32];
            sprintf_s(buf, " #%u.%u", edges[i].to, edges[i].toPort);
            done += buf;
        }
    }
    g_applying = false;
    ++g_stats.applied;
    g_stats.inputs += applied;
    if (VerboseLine() || applied == 0 || uplinkClass)
        Log("client: applied %s(%u) #%u %s out%u %s -> %d input(s)%s%s, %d server-only skipped%s", ownerName.c_str(),
            r.owner, r.node, r.cls.c_str(), r.port, r.value.c_str(), applied, applied ? ":" : "", done.c_str(), skipped,
            candidates > 1 ? " (AMBIGUOUS owner, first graph used)" : "");
}

// Server: apply a record uplinked by the client on `channel`, then relay it to the other clients.
void ApplyUplink(const std::string& text, int channel) {
    ++g_stats.upReceived;
    Record r;
    RawValue v;
    if (!ParseRecord(text, &r) || !DecodeValue(r.value, &v)) {
        ++g_stats.upRejected;
        Log("server: bad uplink record '%s' from channel %d", text.c_str(), channel);
        return;
    }
    std::string ownerName = Unescape(r.ownerField);
    int candidates = 0;
    void* graph = ResolveGraph(r.owner, r.ownerField, r.node, r.cls.c_str(), &candidates);
    if (!graph) {
        ++g_stats.upUnresolved;
        Log("server: uplink UNRESOLVED %s(%u) #%u %s out%u from channel %d", ownerName.c_str(), r.owner, r.node,
            r.cls.c_str(), r.port, channel);
        return;
    }
    if (!(ClassFlags(graph, r.node) & kUplink)) {
        ++g_stats.upRejected;
        Log("server: uplink %s #%u %s from channel %d rejected: not an uplink class here", ownerName.c_str(), r.node,
            r.cls.c_str(), channel);
        return;
    }
    auto g = static_cast<char*>(graph);
    if (!g[kGraphEnabled] || !g[kGraphActive]) {
        ++g_stats.upInactive;
        Log("server: uplink %s #%u %s out%u from channel %d: graph is %s here, dropped", ownerName.c_str(), r.node,
            r.cls.c_str(), r.port, channel, !g[kGraphEnabled] ? "disabled" : "inactive");
        return;
    }
    if (Recent(graph, r.node, r.port, 0)) {
        ++g_stats.upDup;
        Log("server: uplink %s #%u %s out%u from channel %d: fired/applied here within %llu ms, dropped (double)",
            ownerName.c_str(), r.node, r.cls.c_str(), r.port, channel, g_windowMs);
        return;
    }
    SetMark(graph, r.node, r.port, true);
    Edge edges[1];
    int links = SafeCollectEdges(g, r.node, r.port, edges, 0);
    g_applying = true;
    bool ok = ActivateOutputOriginal(graph, r.node, r.port, v);
    g_applying = false;
    if (!ok) {
        ++g_stats.upRejected;
        Log("server: uplink %s #%u %s: graph %p not activatable", ownerName.c_str(), r.node, r.cls.c_str(), graph);
        return;
    }
    ++g_stats.upApplied;
    // relay with this machine's owner id/name and entity names
    std::string relay = BuildRecord(graph, r.node, r.port, v, r.cls.c_str());
    TellChannel(channel);
    QueueRecord(relay, 0, channel);
    ++g_stats.upRelayed;
    Log("server: uplink applied from channel %d: %s(%u) #%u %s out%u %s -> %d link(s)%s; relayed to all clients "
        "except channel %d",
        channel, ownerName.c_str(), r.owner, r.node, r.cls.c_str(), r.port, r.value.c_str(), links,
        candidates > 1 ? " (AMBIGUOUS owner, first graph used)" : "", channel);
}

// Server: a history record whose links reach staging / cinematic / token / player-position nodes (not replayed).
bool ReplayHarmful(const std::string& text, std::map<std::string, void*>& graphCache, const char** reason) {
    Record r;
    if (!ParseRecord(text, &r)) {
        *reason = "bad record";
        return true;
    }
    std::string key = std::to_string(r.owner) + "," + std::to_string(r.node) + "," + r.cls + "," + r.ownerField;
    auto it = graphCache.find(key);
    void* graph;
    if (it != graphCache.end()) {
        graph = it->second;
    } else {
        int candidates = 0;
        graph = graphCache[key] = ResolveGraph(r.owner, r.ownerField, r.node, r.cls.c_str(), &candidates);
    }
    if (!graph) {
        *reason = "graph gone";
        return true;
    }
    Edge edges[64];
    int n = SafeCollectEdges(static_cast<char*>(graph), r.node, r.port, edges, 64);
    for (int i = 0; i < n && i < 64; ++i) {
        const char* to = SafeNodeClass(graph, edges[i].to);
        if (Matches(g_replaySkip, to)) {
            *reason = to;
            return true;
        }
        if (Matches(g_replaySkipPlayer, to) && FedByLocalPlayer(static_cast<char*>(graph), edges[i].to)) {
            *reason = to;
            return true;
        }
    }
    return false;
}

void Replay(int channel) {
    ++g_stats.replays;
    std::vector<std::pair<unsigned long long, std::string>> records;
    {
        std::lock_guard lock(g_outMtx);
        for (auto& [key, entry] : g_last)
            records.push_back(entry);
    }
    std::sort(records.begin(), records.end());
    g_toldChannels.erase(std::remove(g_toldChannels.begin(), g_toldChannels.end(), channel), g_toldChannels.end());
    TellChannel(channel); // a (re)joined client: its new channel id
    std::map<std::string, void*> graphCache;
    std::map<std::string, int> reasons;
    unsigned queued = 0;
    std::vector<std::string> keep;
    for (auto& [seq, record] : records) {
        const char* reason = "";
        if (ReplayHarmful(record, graphCache, &reason)) {
            ++reasons[reason];
            ++g_stats.replaySkipped;
            continue;
        }
        keep.push_back(record);
    }
    {
        std::lock_guard lock(g_outMtx);
        for (auto& record : keep)
            g_replayQ.push_back({channel, record});
        g_replayQ.push_back({channel, std::string()}); // end marker: token resync
        queued = static_cast<unsigned>(keep.size());
        g_replayCount[channel] = {queued, static_cast<unsigned>(records.size())};
    }
    g_stats.replayQueued += queued;
    std::string why;
    for (auto& [reason, count] : reasons)
        why += " " + reason + "x" + std::to_string(count);
    Log("server: replay to channel %d: %u of %zu last-per-port records queued%s%s", channel, queued, records.size(),
        why.empty() ? "" : ", skipped:", why.c_str());
}

// ---------------------------------------------------------------- commands
void* FindGraphByOwner(const std::string& key, uint32_t* ownerOut) {
    uint32_t id = strtoul(key.c_str(), nullptr, 10);
    for (void* g : Graphs()) {
        uint32_t ent = SafeGraphEntity(g);
        if (!ent)
            continue;
        if ((id && ent == id) || SameText(SafeEntityName(ent), key.c_str())) {
            *ownerOut = ent;
            return g;
        }
    }
    return nullptr;
}

void PrintStatus() {
    Log("%s, hooks %s, role %s, suppress %s, verbose %s, window %llu ms, flow system %p, my channel %d",
        g_enabled ? "ON" : "OFF", g_installed ? "installed" : "NOT installed",
        !IsMultiplayer() ? "single player" : IsServer() ? "server" : "client", g_suppress ? "on" : "off",
        g_verbose ? "on" : "off", g_windowMs, g_flowSystem, g_myChannel);
    std::lock_guard lock(g_outMtx);
    Log("server: mirror outputs seen %u (port rules %u, random %u, host uplink-class %u), queued %u, sent %u in %u "
        "messages, outbox %zu, dropped %u, no owner %u, no client targets %u",
        g_stats.seen, g_stats.byPortRule, g_stats.byRandom, g_stats.hostUplinkClass, g_stats.queued, g_stats.sent,
        g_stats.messages, g_outbox.size(), g_stats.dropped, g_stats.noEntity, g_stats.noTargets);
    Log("server: uplinks received %u, applied %u, relayed %u, doubles dropped %u, unresolved %u, rejected %u, "
        "inactive %u",
        g_stats.upReceived, g_stats.upApplied, g_stats.upRelayed, g_stats.upDup, g_stats.upUnresolved,
        g_stats.upRejected, g_stats.upInactive);
    Log("server: replay history %zu keys (full %u), replays %u, queued %u, skipped %u, sent %u, replay queue %zu",
        g_last.size(), g_stats.historyFull, g_stats.replays, g_stats.replayQueued, g_stats.replaySkipped,
        g_stats.replaySent, g_replayQ.size());
    Log("client: received %u, applied %u (%u inputs), unresolved %u, graph inactive %u, local outputs suppressed %u, "
        "own relays ignored %u, doubles dropped %u, entity names not found %u",
        g_stats.received, g_stats.applied, g_stats.inputs, g_stats.unresolved, g_stats.inactive, g_stats.suppressed,
        g_stats.ownRelayIgnored, g_stats.dupDropped, g_stats.nameFallback);
    Log("client: uplinks queued %u, sent %u in %u messages, queue %zu, dropped %u, too long %u, not sent (applying) "
        "%u, local suppressed (server copy applied) %u",
        g_stats.upQueued, g_stats.upSent, g_stats.upMessages, g_uplinkQ.size(), g_stats.upDropped, g_stats.upTooLong,
        g_stats.upSkippedApplying, g_stats.upSuppressedDup);
}

std::string Joined(const std::vector<std::string>& list) {
    std::string s;
    for (auto& c : list)
        s += " " + c;
    return s;
}

void PrintClasses() {
    Log("mirror (server-only sources):%s", Joined(g_mirrorClasses).c_str());
    Log("skip (never activated on clients):%s (+ mirror classes)", Joined(g_skipClasses).c_str());
    Log("uplink (client -> server sources):%s", Joined(g_uplinkClasses).c_str());
    Log("random:%s, server-authoritative in graphs (rgraph):%s", Joined(g_randomClasses).c_str(),
        Joined(g_randomGraphs).c_str());
    std::string rules;
    for (auto& rule : g_portRules) {
        rules += " " + rule.cls + "[";
        for (size_t i = 0; i < rule.ports.size(); ++i)
            rules += (i ? "," : "") + rule.ports[i];
        rules += rule.aiOnly ? "]ai" : "]";
    }
    Log("port rules:%s", rules.c_str());
    Log("replayskip:%s; replayplayer (fed by Actor:LocalPlayer):%s", Joined(g_replaySkip).c_str(),
        Joined(g_replaySkipPlayer).c_str());
}

// Order-independent checksum over graphs with an owner entity: compare host and client.
void PrintGraphs(const std::string& filter) {
    auto graphs = Graphs();
    unsigned withOwner = 0;
    uint32_t checksum = 0;
    for (void* g : graphs) {
        uint32_t ent = SafeGraphEntity(g);
        if (!ent)
            continue;
        ++withOwner;
        const char* name = SafeEntityName(ent);
        unsigned nodes = SafeNodeCount(static_cast<char*>(g));
        int edges = SafeEdgeCount(static_cast<char*>(g));
        uint32_t h = 2166136261u;
        auto mix = [&](const void* p, size_t n) {
            for (size_t i = 0; i < n; ++i)
                h = (h ^ static_cast<const unsigned char*>(p)[i]) * 16777619u;
        };
        mix(&ent, 4);
        mix(&nodes, 4);
        mix(&edges, 4);
        if (name)
            mix(name, strlen(name));
        checksum += h;
        if (!filter.empty() && name && StrStrIA(name, filter.c_str()))
            Log("graph %p owner %u '%s' nodes %u edges %d %s%s", g, ent, name, nodes, edges,
                static_cast<char*>(g)[kGraphEnabled] ? "enabled" : "disabled",
                static_cast<char*>(g)[kGraphActive] ? " active" : " inactive");
    }
    Log("%zu flow graphs, %u with an owner entity, checksum %08X (same level on host and client should match)",
        graphs.size(), withOwner, checksum);
}

void PrintNode(const std::string& key, uint16_t node) {
    uint32_t owner = 0;
    void* g = FindGraphByOwner(key, &owner);
    if (!g) {
        Log("no graph owned by '%s'", key.c_str());
        return;
    }
    const char* cls = SafeNodeClass(g, node);
    unsigned char f = ClassFlags(g, node);
    Log("graph %p owner %u '%s' #%u %s flags %s%s%s%s%s", g, owner, SafeEntityName(owner), node, cls ? cls : "(none)",
        f & kMirror ? "mirror " : "", f & kSkip ? "skip " : "", f & kUplink ? "uplink " : "",
        f & kPortRule ? "portrule " : "", f & kRandom ? (RandomGraph(g) ? "random(graph matches)" : "random") : "");
    int type = SafeNodeTypeId(g, node);
    const std::vector<std::string> none;
    auto& names = type >= 0 ? OutputNames(g, node, type) : none;
    std::string all;
    for (size_t i = 0; i < names.size(); ++i)
        all += " " + std::to_string(i) + ":" + names[i];
    Log("  outputs:%s", all.empty() ? " (unknown)" : all.c_str());
    if (f & kPortRule) {
        const PortInfo& info = PortBits(g, node);
        Log("  port rule mask %016llX (ai %016llX)", info.mask, info.aiMask);
    }
    for (unsigned port = 0; port < 64; ++port) {
        Edge edges[64];
        int n = SafeCollectEdges(static_cast<char*>(g), node, static_cast<uint8_t>(port), edges, 64);
        for (int i = 0; i < n && i < 64; ++i) {
            const char* to = SafeNodeClass(g, edges[i].to);
            Log("  out%u(%s) -> #%u %s in%u%s", port, port < names.size() ? names[port].c_str() : "?", edges[i].to,
                to ? to : "?", edges[i].toPort, ServerOnlyTarget(g, edges[i].to) ? " (server-only, not applied on clients)" : "");
        }
    }
}

void Fire(const std::string& key, uint16_t node, uint8_t port) {
    uint32_t owner = 0;
    void* g = FindGraphByOwner(key, &owner);
    const char* cls = g ? SafeNodeClass(g, node) : nullptr;
    if (!g || !cls) {
        Log("fire: no graph owned by '%s' with node #%u", key.c_str(), node);
        return;
    }
    char rec[256];
    sprintf_s(rec, "%u,%u,%u,v,%s,%s", owner, node, port, cls, OwnerField(SafeEntityName(owner)).c_str());
    if (IsServer()) {
        QueueRecord(rec);
        Log("fire: queued %s for clients", rec);
    } else {
        Log("fire: applying %s locally", rec);
        ApplyRecord(rec);
    }
}

void EditList(std::vector<std::string>& list, const std::string& op, const std::string& cls) {
    if (cls.empty() || (op != "add" && op != "del")) {
        Log("usage: <mirror|skip|uplink|random|rgraph|replayskip|replayplayer> add|del <pattern>");
        return;
    }
    std::erase(list, cls);
    if (op == "add")
        list.push_back(cls);
    ResetCaches();
    PrintClasses();
}

// port add <class> <port,port...> [ai] | port del <class>
void EditPortRule(const std::string& rest) {
    char op[8] = {}, cls[128] = {}, ports[256] = {}, flag[8] = {};
    int n = sscanf_s(rest.c_str(), "%7s %127s %255s %7s", op, (unsigned)sizeof(op), cls, (unsigned)sizeof(cls), ports,
                     (unsigned)sizeof(ports), flag, (unsigned)sizeof(flag));
    std::string o = op;
    if (n < 2 || (o == "add" && n < 3) || (o != "add" && o != "del")) {
        Log("usage: port add <class> <port|#index|*>[,<port>...] [ai] | port del <class>");
        return;
    }
    std::erase_if(g_portRules, [&](const PortRule& r) { return _stricmp(r.cls.c_str(), cls) == 0; });
    if (o == "add") {
        PortRule rule{cls, {}, _stricmp(flag, "ai") == 0};
        for (auto& p : Split(ports, ','))
            if (!p.empty())
                rule.ports.push_back(p);
        g_portRules.push_back(rule);
    }
    ResetCaches();
    PrintClasses();
}

// "<channel> <records>"
bool ChannelAndRecords(const std::string& rest, int* channel, std::string* records) {
    auto sp = rest.find(' ');
    *channel = atoi(rest.c_str());
    *records = sp == std::string::npos ? "" : rest.substr(sp + 1);
    return *channel > 0;
}

} // namespace

namespace c3coop {

void InstallFgMirror() {
    const uintptr_t base = game::Base();
    std::string bad;
    for (auto& s : kSites) {
        if (memcmp(reinterpret_cast<void*>(base + s.rva), s.sig, s.sigLen) != 0)
            bad += std::string(" code:") + s.name;
        if (*reinterpret_cast<uintptr_t*>(base + s.vtRva + s.vtSlot) != base + s.rva)
            bad += std::string(" vtable:") + s.name;
    }
    for (auto& c : kChecks)
        if (memcmp(reinterpret_cast<void*>(base + c.rva), c.bytes, c.len) != 0)
            bad += std::string(" code:") + c.what;
    for (uintptr_t vt : {kRvaVtFlowGraph, kRvaVtFlowGraphDerived})
        for (auto& slot : kMainSlots)
            if (*reinterpret_cast<uintptr_t*>(base + vt + slot.offset) != base + slot.rva)
                bad += " vtable slot";
    if (!bad.empty()) {
        g_enabled = false;
        Log("DISABLED, signature mismatch:%s", bad.c_str());
        return;
    }
    int hooked = 0;
    for (int k = 0; k < kKinds; ++k) {
        void* target = reinterpret_cast<void*>(base + kSites[k].rva);
        if (MH_CreateHook(target, kDetours[k], reinterpret_cast<void**>(&g_orig[k])) == MH_OK &&
            MH_EnableHook(target) == MH_OK)
            ++hooked;
        else
            Log("hook %s at %p FAILED", kSites[k].name, target);
    }
    if (hooked != kKinds) {
        g_enabled = false; // detours stay as pass-throughs
        Log("DISABLED, only %d of %d activation hooks installed", hooked, int(kKinds));
        return;
    }
    g_installed = true;
    Log("ready: %d flowgraph activation entry points hooked (CFlowGraph vtable %p); \"!fgmirror status\"", hooked,
        reinterpret_cast<void*>(base + kRvaVtFlowGraph));
    PrintClasses();
}

bool FgMirrorCommand(const std::string& cmd) {
    if (cmd != "!fgmirror" && cmd.rfind("!fgmirror ", 0) != 0)
        return false;
    std::string args = cmd.size() > 10 ? cmd.substr(10) : "";
    auto sp = args.find(' ');
    std::string verb = args.substr(0, sp);
    std::string rest = sp == std::string::npos ? "" : args.substr(sp + 1);
    if (verb == "a" || verb == "ax") {
        if (!g_installed || !g_enabled) {
            ++g_stats.received;
            return true;
        }
        if (IsServer()) {
            Log("ignoring mirrored activations on the server");
            return true;
        }
        std::string records = rest;
        if (verb == "ax") {
            // relay of an uplink: not for the client it came from
            int from = 0;
            ChannelAndRecords(rest, &from, &records);
            if (from > 0 && from == g_myChannel) {
                ++g_stats.ownRelayIgnored;
                if (VerboseLine())
                    Log("client: relay of my own uplink ignored (channel %d)", from);
                return true;
            }
        }
        for (auto& record : Split(records, '|'))
            if (!record.empty())
                ApplyRecord(record);
    } else if (verb == "me") {
        if (!IsServer()) {
            g_myChannel = atoi(rest.c_str());
            Log("client: my channel on the server is %d", g_myChannel);
        }
    } else if (verb == "u") {
        // uplinked records (server Lua C3CoopOnClientMessage "fg ..." -> "c3coop fgmirror u <channel> <records>")
        int channel = 0;
        std::string records;
        if (!ChannelAndRecords(rest, &channel, &records) || !IsServer()) {
            Log("usage (server): !fgmirror u <channel> <records>");
            return true;
        }
        if (!g_installed || !g_enabled) {
            ++g_stats.upReceived;
            return true;
        }
        for (auto& record : Split(FromUplinkForm(records), '|'))
            if (!record.empty())
                ApplyUplink(record, channel);
    } else if (verb == "replay") {
        // a player joined (Lua C3CoopJoinReplay): the last record per port, for that client only
        int channel = atoi(rest.c_str());
        if (channel <= 0 || !IsServer()) {
            Log("usage (server): !fgmirror replay <channel>");
            return true;
        }
        Replay(channel);
    } else if (verb == "reset") {
        FgMirrorReset();
    } else if (verb.empty() || verb == "status") {
        PrintStatus();
    } else if (verb == "on" || verb == "off") {
        if (verb == "on" && !g_installed) {
            Log("hooks are not installed (signature mismatch at startup), staying off");
            return true;
        }
        g_enabled = verb == "on";
        PrintStatus();
    } else if (verb == "suppress" && (rest == "on" || rest == "off")) {
        g_suppress = rest == "on";
        Log("client suppression of local server-authoritative outputs %s", g_suppress ? "on" : "off");
    } else if (verb == "verbose" && (rest == "on" || rest == "off")) {
        g_verbose = rest == "on";
        g_logBudget = 400;
        Log("verbose %s", g_verbose ? "on" : "off");
    } else if (verb == "window" && atoi(rest.c_str()) > 0) {
        g_windowMs = static_cast<ULONGLONG>(atoi(rest.c_str()));
        Log("uplink double-application window %llu ms", g_windowMs);
    } else if (verb == "classes") {
        PrintClasses();
    } else if (verb == "mirror" || verb == "skip" || verb == "uplink" || verb == "random" || verb == "rgraph" ||
               verb == "replayskip" || verb == "replayplayer") {
        auto& list = verb == "mirror"       ? g_mirrorClasses
                     : verb == "skip"       ? g_skipClasses
                     : verb == "uplink"     ? g_uplinkClasses
                     : verb == "random"     ? g_randomClasses
                     : verb == "rgraph"     ? g_randomGraphs
                     : verb == "replayskip" ? g_replaySkip
                                            : g_replaySkipPlayer;
        auto s2 = rest.find(' ');
        EditList(list, rest.substr(0, s2), s2 == std::string::npos ? "" : rest.substr(s2 + 1));
    } else if (verb == "port") {
        EditPortRule(rest);
    } else if (verb == "graphs") {
        PrintGraphs(rest);
    } else if (verb == "node" || verb == "fire") {
        char key[128] = {};
        unsigned node = 0, port = 0;
        int n = sscanf_s(rest.c_str(), "%127s %u %u", key, (unsigned)sizeof(key), &node, &port);
        if (n < 2 || (verb == "fire" && n < 3))
            Log("usage: !fgmirror node <owner name|entity id> <node id> | fire <owner> <node id> <port>");
        else if (verb == "node")
            PrintNode(key, static_cast<uint16_t>(node));
        else
            Fire(key, static_cast<uint16_t>(node), static_cast<uint8_t>(port));
    } else {
        Log("usage: !fgmirror [status|on|off|suppress on|off|verbose on|off|window <ms>|classes|"
            "mirror|skip|uplink|random|rgraph|replayskip|replayplayer add|del <pattern>|"
            "port add <cls> <ports> [ai]|port del <cls>|graphs [filter]|node <owner> <id>|fire <owner> <id> <port>]");
    }
    return true;
}

// Main thread, every frame.
// Server: send queued records to clients, batched into messages < 200 chars, at most kMessagesPerSlot messages per
// kSlotMs (the token sync showed bursts of RMIs can stall the server): live records first, then late-join replays.
// Client: send queued uplink records to the server through Lua, same rate.
void FgMirrorTick() {
    if (!IsMultiplayer()) {
        std::lock_guard lock(g_outMtx);
        g_outbox.clear();
        g_replayQ.clear();
        g_uplinkQ.clear();
        return;
    }
    ULONGLONG now = GetTickCount64();
    if (now < g_nextSend)
        return;
    if (!IsServer()) {
        std::vector<std::string> lines;
        {
            std::lock_guard lock(g_outMtx);
            g_outbox.clear();
            g_replayQ.clear();
            while (!g_uplinkQ.empty() && lines.size() < kMessagesPerSlot) {
                std::string records;
                while (!g_uplinkQ.empty() &&
                       (records.empty() || records.size() + 1 + g_uplinkQ.front().size() <= kUplinkMax)) {
                    if (!records.empty())
                        records += '|';
                    records += g_uplinkQ.front();
                    g_uplinkQ.pop_front();
                    ++g_stats.upSent;
                }
                lines.push_back(records);
            }
        }
        if (lines.empty())
            return;
        g_nextSend = now + kSlotMs;
        for (auto& records : lines) {
            ++g_stats.upMessages;
            Log("client: uplink sent \"%s\"", records.c_str());
            c3coop::RunLua("if g_localActor then g_localActor.server:C3ToServer(\"fg " + records + "\") end");
        }
        return;
    }
    std::vector<std::pair<std::string, std::string>> messages; // (target, "fgmirror ...")
    std::vector<int> drained;                                   // replays finished: token resync for these channels
    {
        std::lock_guard lock(g_outMtx);
        g_uplinkQ.clear();
        while (!g_outbox.empty() && messages.size() < kMessagesPerSlot) {
            Outgoing first = g_outbox.front();
            std::string target = first.channel ? std::to_string(first.channel) : std::string("all");
            if (first.raw) {
                g_outbox.pop_front();
                messages.push_back({target, "fgmirror " + first.record});
                continue;
            }
            std::string msg = first.exclude ? "fgmirror ax " + std::to_string(first.exclude) + " " : "fgmirror a ";
            size_t header = msg.size();
            size_t records = 0;
            while (!g_outbox.empty() && !g_outbox.front().raw && g_outbox.front().channel == first.channel &&
                   g_outbox.front().exclude == first.exclude &&
                   (records == 0 || msg.size() + 1 + g_outbox.front().record.size() <= kMessageMax)) {
                if (records)
                    msg += '|';
                msg += g_outbox.front().record.substr(0, kMessageMax - header);
                g_outbox.pop_front();
                ++records;
            }
            g_stats.sent += static_cast<unsigned>(records);
            messages.push_back({target, msg});
        }
        while (!g_replayQ.empty() && (messages.size() < kMessagesPerSlot || g_replayQ.front().record.empty())) {
            int channel = g_replayQ.front().channel;
            if (g_replayQ.front().record.empty()) {
                drained.push_back(channel);
                g_replayQ.pop_front();
                continue;
            }
            std::string msg = "fgmirror a ";
            size_t records = 0;
            while (!g_replayQ.empty() && g_replayQ.front().channel == channel && !g_replayQ.front().record.empty() &&
                   (records == 0 || msg.size() + 1 + g_replayQ.front().record.size() <= kMessageMax)) {
                if (records)
                    msg += '|';
                msg += g_replayQ.front().record.substr(0, kMessageMax - 11);
                g_replayQ.pop_front();
                ++records;
            }
            g_stats.replaySent += static_cast<unsigned>(records);
            messages.push_back({std::to_string(channel), msg});
        }
    }
    if (!messages.empty()) {
        g_nextSend = now + kSlotMs;
        // pending token changes first, so the client's flowgraph sees current tokens when these records arrive
        c3coop::RunLua("if C3CoopTokenFlush then C3CoopTokenFlush() end");
        for (auto& [target, msg] : messages) {
            ++g_stats.messages;
            if (VerboseLine())
                Log("server: send %s \"%s\"", target.c_str(), msg.c_str());
            CoopCommand("send " + target + " " + msg);
        }
    }
    for (int channel : drained) {
        std::pair<unsigned, unsigned> count;
        {
            std::lock_guard lock(g_outMtx);
            count = g_replayCount[channel];
        }
        Log("server: replay sent %u of %u records to channel %d; token resync requested", count.first, count.second,
            channel);
        c3coop::RunLua("if C3CoopTokenResyncFor then C3CoopTokenResyncFor(" + std::to_string(channel) + ") end");
    }
}

// A level is loading: its graphs start over, so does the history for late joiners.
void FgMirrorReset() {
    std::lock_guard lock(g_outMtx);
    if (!g_last.empty())
        Log("server: level change, %zu last-per-port records forgotten", g_last.size());
    g_last.clear();
    g_outbox.clear();
    g_replayQ.clear();
    g_replayCount.clear();
    g_uplinkQ.clear();
    g_marks.clear();
    g_lastVictim.clear();
    g_randomGraphCache.clear();
    g_toldChannels.clear();
    g_myChannel = 0;
}

} // namespace c3coop
