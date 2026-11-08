#pragma once
// Addresses and engine interfaces for Crysis 3 Remastered, Steam build 9460220
// (Crysis3Remastered.exe SHA-256 B979D931...09319A06A, TimeDateStamp 0x62FD4D1B, fixed base 0x37000000).

#include <cstdint>

namespace game {

constexpr uint32_t kTimeDateStamp = 0x62FD4D1B;

// RVAs (VA - 0x37000000)
constexpr uintptr_t kRvaConsoleSlot = 0x24503D8;   // gEnv->pConsole
constexpr uintptr_t kRvaConRestricted = 0x244EC58; // int con_restricted (initial 1)
constexpr uintptr_t kRvaConsoleVtable = 0x2197C18; // CXConsole vtable

// Steam callback that starts the Saber "Leech"/Hydra backend session (crysis.hydrapi.net, now dead):
//   cmp dword [rdx+4],1; jne ret; mov byte [flag],1; jmp LeechSessionStart(0x37EA3560)
// LeechSessionStart frees a corrupt global buffer -> 0xC0000374 about 1-2 minutes after launch.
constexpr uintptr_t kRvaLeechSteamCallback = 0xEA1920;
constexpr unsigned char kLeechSteamCallbackBytes[] = {0x83, 0x7A, 0x04, 0x01, 0x75, 0x0C};

// CNetwork::CreateNub(this, address, IGameNub*, IGameSecurity*, IGameQuery*) = CNetwork vtable(0x3915F7F8)+0x48.
// gEnv->pNetwork is the gEnv slot at kRvaConsoleSlot - 0xA8.
constexpr uintptr_t kRvaNetworkCreateNub = 0xA7C640;
constexpr uintptr_t kRvaNetworkSlot = kRvaConsoleSlot - 0xA8;

// gEnv->pCryPak. ICryPak::OpenPack(bindRoot, path, flags, IMemoryBlock*, CryFixedStringT*) is vtbl+0x28
// (OpenCachePak at 0x37EA539E). Paks opened later override earlier ones (that is how Patch/*.pak work).
constexpr uintptr_t kRvaCryPakSlot = kRvaConsoleSlot - 0x58;
constexpr size_t kVtCryPakOpenPack = 0x28;
// ICryArchive::FLAGS_OVERRIDE_PAK (BIT 10): non-override paks are inserted before all override paks, and lookups
// walk the list backwards, so only an override pak opened last can replace files of the game's paks.
constexpr unsigned kPakFlagOverride = 1u << 10;
// CCryPak::m_arrZips begin/end (OpenPackCommon 0x37DBA410 inserts there) and sizeof(PackDesc)
constexpr size_t kCryPakZipsBegin = 0x180;
constexpr size_t kCryPakDescSize = 0x38;
// CCryPak::m_pLog: OpenPackCommon logs through it unconditionally, so a pak opened before it is set crashes
constexpr size_t kCryPakLogOffset = 0x1F0;
// CCryPak::OpenPackCommon(this, bindRoot, fullPath, flags, pData), 0x37DBA410. Override paks opened later (the
// game mounts Patch/patch1.pak and patch2.pak after our early mount) would land above ours.
constexpr uintptr_t kRvaOpenPackCommon = 0xDBA410;
constexpr unsigned char kOpenPackCommonBytes[] = {0x44, 0x89, 0x4C, 0x24, 0x20, 0x53, 0x55, 0x56, 0x57};

// CNetCVars is embedded in CNetwork at +0x20 (CNetwork ctor 0x37A79750). Net id allocation
// (0x37AA62E0) uses precomputed pool ends: low [0, +0xD4), medium [.., +0xE0), high [+0xE0, +0xEC).
// Defaults 16 / 272 / 784 are too few for campaign levels ("Failed to allocate a high bit netid").
// The *BitBits cvars (+0xD0/+0xD8/+0xE4) only feed the error text after startup.
constexpr size_t kNetCVarsOffset = 0x20;
constexpr size_t kNetLowEnd = 0xD4;
constexpr size_t kNetMediumEnd = 0xE0;
constexpr size_t kNetHighBits = 0xE4;
constexpr size_t kNetHighEnd = 0xEC;
constexpr unsigned char kNetworkCreateNubBytes[] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x48, 0x08, 0x55, 0x56};

// CCryAction::MapCmd(IConsoleCmdArgs*) (0x37601520), the "map" command handler. Lines typed in the console reach it
// through CXConsole::ExecuteStringInternal, not through the hooked IConsole::ExecuteString. IConsoleCmdArgs vtable:
// +0x08 GetArgCount, +0x10 GetArg(i) (MapCmd calls both on rcx).
constexpr uintptr_t kRvaMapCmd = 0x601520;
constexpr unsigned char kMapCmdBytes[] = {0x40, 0x55, 0x41, 0x55, 0x48, 0x8D, 0xAC, 0x24, 0x28, 0xFE, 0xFF, 0xFF};
constexpr size_t kVtCmdArgCount = 0x08;
constexpr size_t kVtCmdArg = 0x10;

// CPlayer "set MP model" (0x386117D0): copies an entry of the MP player model table (0x3942D7E0, 0x38 bytes per
// team: nanosuit_mp_army / nanosuit_mp_ops / rebelMP_01 ...) into Properties.fileModel/shadowFileModel/
// clientFileModel. Called as `if (gEnv->bMultiplayer) ...` (0x385F3675) and from 3 other places.
constexpr uintptr_t kRvaSetMpPlayerModel = 0x16117D0;
constexpr unsigned char kSetMpPlayerModelBytes[] = {0x48, 0x8B, 0xC4, 0x55, 0x53, 0x48, 0x8D, 0x68, 0xA1};

// CharacterManager: load a .cdf from disk when it is not cached yet (this, const char* path) -> index or -1.
// Network players get nanosuit_mp/nanosuit_mp_army.cdf (C++ and Scripts/PlayerTeamVisualization.xml), which the
// remaster does not ship; without a model the remote actor fails to init and the client dies in PlayerMovement.
constexpr uintptr_t kRvaLoadCdf = 0x8CA580;
constexpr unsigned char kLoadCdfBytes[] = {0x40, 0x55, 0x41, 0x56, 0x48, 0x8D, 0x6C, 0x24, 0xB1};
constexpr const char* kSpNanosuitCdf = "objects/characters/human/us/nanosuit_v2/nanosuit_v2.cdf";

// CPlayerMovementController::Update (0x38620270): for network players `obj->vtbl[0x198]()` (likely GetAI())
// returns NULL and is dereferenced unchecked; the next call's result is already null-checked (je 0x3862231B).
//   0x38621E6F: mov rcx,rax / mov rdx,[rax] / call [rdx+208h] / mov rdi,rax / test rax,rax / je 3862231Bh
constexpr uintptr_t kRvaMoveCtrlNullSite = 0x1621E6F;
constexpr uintptr_t kRvaMoveCtrlContinue = 0x1621E87;
constexpr uintptr_t kRvaMoveCtrlNullPath = 0x162231B;
constexpr unsigned char kMoveCtrlNullSiteBytes[] = {0x48, 0x8B, 0xC8, 0x48, 0x8B, 0x10, 0xFF, 0x92, 0x08, 0x02, 0x00, 0x00,
                                                    0x48, 0x8B, 0xF8, 0x48, 0x85, 0xC0, 0x0F, 0x84, 0x94, 0x04, 0x00, 0x00};

// gEnv->bMultiplayer byte. CActionGame::Init sets it for every non-LocalOnly game (0x375EAC28: mov dl,1 /
// 0x375EAC2A: mov [bMultiplayer],dl), which sends clients down removed MP paths (HUD, pause menu, team materials).
constexpr uintptr_t kRvaMultiplayerFlag = 0x2450501;
constexpr uintptr_t kRvaActionGameMpByte = 0x5EAC29; // imm8 of `mov dl,1`
constexpr unsigned char kActionGameMpBytes[] = {0xB2, 0x01, 0x88, 0x15}; // at kRvaActionGameMpByte - 1

// CActionGame client connect (0x375EB6F0, called from CActionGame::Init): with eGSF_Server it connects the local
// client to "<local>:<sv_port>" through a "<local>:0" nub, which fails (0x8001) because server nubs share the lobby
// UDP socket (net_lobby_default_port 64090) and never bind sv_port. Remote clients reach the same server over UDP.
//   rcx = CActionGame (+0x58 -> SGameStartParams: +0xD8 flags (bit1 = eGSF_Server), +0xDC u16 port)
//   rdx = connection params: [+0] const char* hostname, [+0x20] int session (-1 = none)
constexpr uintptr_t kRvaActionGameClientConnect = 0x5EB6F0;
constexpr unsigned char kActionGameClientConnectBytes[] = {0x4C, 0x89, 0x4C, 0x24, 0x20, 0x53, 0x55, 0x56, 0x57};
constexpr uint16_t kLobbyPort = 64090;

// Socket manager OpenSocket(this, out, const TNetAddress* addr, uint32 flags) (vtbl 0x39166D08 + 0xC8).
// Keeps a vector of {address, socket, flags, refs}; for an IPv4 request it REUSES any existing IPv4 socket whose
// flag bit 0x8 matches, without comparing IP or port, unless the request has flag 0x10 (0x37AD09A9). That is why
// every IP nub lands on the lobby socket (UDP 64090). TNetAddress: [0] tagged type ptr (*(type&~3) == 2 for IPv4).
constexpr uintptr_t kRvaOpenSocket = 0xAD07D0;
constexpr unsigned char kOpenSocketBytes[] = {0x40, 0x55, 0x53, 0x56, 0x41, 0x54, 0x41, 0x55, 0x41};
constexpr uint32_t kSocketFlagOwnSocket = 0x10;

// CNetNub IDatagramListener::OnPacket(this, const TNetAddress& from, const uint8* data, uint32 len)
// (CNetNub vtbl 0x39168E80 [9]; [10] = OnError, logs "CNetNub::OnError() received ...").
constexpr uintptr_t kRvaNubOnPacket = 0xAF7EE0;
constexpr unsigned char kNubOnPacketBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x55, 0x56, 0x57, 0x41};

// CNetNub::OnPacket handshake branches: `if (bMultiplayer && byte[0x39450500] && byte[0x39450582])` take the lobby
// variant (0x37AF85B0 for ConnectionSetup, 0x37AFA540 for KeyExchange1), else the classic one (0x37AFA9E0 / inline).
// Remote clients never come through OnPacket (lobby socket path); the host's "<local>" client does and the lobby
// variant answers it with header 10 forever. Turning both `je` into `jmp` selects the classic handshake.
constexpr uintptr_t kRvaHsConnectionSetupJe = 0xAF80CA; // 74 2B
constexpr uintptr_t kRvaHsKeyExchange1Je = 0xAF812A;    // 74 12
// Inside the classic ConnectionSetup handler (0x37AFA9E0): another `if (bMultiplayer && ...)` jump to the lobby
// variant, and a lobby-session check that disconnects with "Invalid session" when session == -1.
constexpr uintptr_t kRvaHsClassicLobbyJe = 0xAFAA0E;    // 74 28
constexpr uintptr_t kRvaHsSessionCheckJe = 0xAFAFBE;    // 74 5D

// gEnv->bServer (byte before bMultiplayer).
constexpr uintptr_t kRvaServerFlag = 0x2450500;

// Lua CryAction.RegisterWithAI (CScriptBind_Action, 0x376208C0) starts with `if (gEnv->bMultiplayer) return;`, so in a
// network game no Human gets an AI object: campaign NPCs stay hidden/disabled and the level flowgraph stalls.
constexpr uintptr_t kRvaRegisterWithAI = 0x6208C0;
constexpr unsigned char kRegisterWithAIBytes[] = {0x40, 0x55, 0x56, 0x48, 0x8D, 0xAC, 0x24, 0x08, 0xF9};

// CActor::IsRemote (0x3853BE70): `bMultiplayer && !IsClient() && !(bServer && (Hologram || DummyPlayer class))`.
// On the server this makes every AI actor "remote", so CPlayer::NetSerialize_InputClient never writes its real
// position and clients pull the AI towards (0,0,0). +0x18 is m_pEntity; IEntity::GetClass is vtbl+0x18.
constexpr uintptr_t kRvaActorIsRemote = 0x153BE70;
constexpr unsigned char kActorIsRemoteBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20};
constexpr size_t kActorEntityOffset = 0x18;
constexpr size_t kVtEntityGetClass = 0x18;
// gEnv->pEntitySystem; IEntitySystem::GetClassRegistry is vtbl+0x40, IEntityClassRegistry::FindClass vtbl+0x18
constexpr uintptr_t kRvaEntitySystemSlot = 0x24503D0;
constexpr size_t kVtEntitySystemGetClassRegistry = 0x40;
constexpr size_t kVtClassRegistryFindClass = 0x18;

// MP dog tag lookup for the killer (0x38AEB240, from CActor::ClKill -> NetKill -> ... -> 0x389CD620):
// pNetwork->vtbl[0xE8]()->vtbl[0x40]() (online/lobby service) is NULL offline and then gets vtbl[0x110] called.
constexpr uintptr_t kRvaDogTagLookup = 0x1AEB240;
constexpr unsigned char kDogTagLookupBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24};

// ---------------------------------------------------------------- co-op session state (2026-10-26 audit fixes)

// Listen server ports. Clients follow the host's port through "c3coop rejoin <port>" only inside 64087..64094
// (coop.cpp); 64090 is net_lobby_default_port (lobby socket, never a working server port, see notes "Listen server
// WORKS"), and a second local instance takes the next lobby port (64091...). Ports are probed before use.
constexpr uint16_t kServerPortFirst = 64087; // sv_port default
constexpr uint16_t kServerPortLast = 64094;
// Rotation order shared by the host (dllmain.cpp, next free port per level change) and a client guessing the host's
// port after missing its "rejoin" message (coop.cpp): 64087, 64088, 64089, 64091, ..., 64094, 64087...
constexpr int NextServerPort(int port) {
    do
        port = (port < kServerPortFirst || port >= kServerPortLast) ? kServerPortFirst : port + 1;
    while (port == kLobbyPort);
    return port;
}

// ICVar: GetIVal vtbl+0x10, GetString vtbl+0x28 (CCryAction connect code: GetCVar("sv_port") -> call [rdx+10h] at
// 0x376024B6; GetCVar("sv_map") -> call [rdx+28h] at 0x386AD30D; same values as steam.cpp).
constexpr size_t kVtCVarGetIVal = 0x10;
constexpr size_t kVtCVarGetString = 0x28;

// CGame* g_pGame (VA 0x39529938) and CGame::m_pFramework (+0x48, IGameFramework* = CCryAction, vtable 0x3910B290).
constexpr uintptr_t kRvaGamePointer = 0x2529938;
constexpr size_t kGameFrameworkOffset = 0x48;
// CCryAction::GetClientChannel (vtbl+0x260 = 0x3760F560; the `connect` handler 0x376029F0 calls it before
// Disconnect(eDC_UserRequested 0x11, "User left the game")): m_pGame(+0x50 CActionGame*) -> +0x48 CGameClientNub* ->
// +0x10 CGameClientChannel* -> +8 INetChannel*. m_pGame exists from `connect` (StartGameContext) until the context
// ends after a failed connect / disconnect, so it is the "connection or load in progress" signal on a client.
constexpr size_t kVtFrameworkGetClientChannel = 0x260;
constexpr uintptr_t kRvaFrameworkGetClientChannel = 0x60F560;
constexpr unsigned char kFrameworkGetClientChannelBytes[] = {0x48, 0x8B, 0x41, 0x50, 0x48, 0x85, 0xC0,
                                                             0x74, 0x17, 0x48, 0x8B, 0x40, 0x48};
constexpr size_t kFrameworkActionGame = 0x50;
// CCryAction::GetLevelName (vtbl+0x328 = 0x37610210): current level name after the last '/' ('\\'); the front end's
// RestartLevel handler (0x3893A10E) calls it to get the level to restart.
constexpr size_t kVtFrameworkGetLevelName = 0x328;
constexpr uintptr_t kRvaFrameworkGetLevelName = 0x610210;
constexpr unsigned char kFrameworkGetLevelNameBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20,
                                                         0x48, 0x8B, 0x01, 0x33, 0xFF, 0x40, 0x38, 0x3D};

// CCryAction::OnActionEvent(this, const SActionEvent* {int event; int value; const char* description}) (0x37612850,
// hud.cpp). EActionEvent matches CE3: eAE_connectFailed 2 (CGameClientNub "Failed connecting to server: %s"
// 0x3756E06A, value = cause), eAE_connected 3, eAE_disconnected 4 (CGameClientChannel::OnDisconnect 0x3756A9A4,
// value = cause, description = text), eAE_inGame 0xB, eAE_preSaveGame 9 / eAE_postSaveGame 0xA (SaveGame).
constexpr uintptr_t kRvaCryActionOnActionEvent = 0x612850;
constexpr unsigned char kCryActionOnActionEventBytes[] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x50, 0x10, 0x48, 0x89, 0x48, 0x08};
constexpr int kActionEventConnectFailed = 2;
constexpr int kActionEventConnected = 3;
constexpr int kActionEventDisconnected = 4;
constexpr int kActionEventInGame = 0xB;
// EDisconnectionCause (CE3 order, confirmed by two values): eDC_Timeout 0, eDC_NubDestroyed 0xB (CNetNub shutdown
// 0x37AF592E/0x37AF5992 with "Nub destroyed"), eDC_ICMPError 0xC, eDC_UserRequested 0x11 (`connect` handler).
constexpr int kDisconnectTimeout = 0;
constexpr int kDisconnectNubDestroyed = 0xB;
constexpr int kDisconnectIcmpError = 0xC;
constexpr int kDisconnectUserRequested = 0x11;

// CGameRules::ScheduleEntityRemoval(this, EntityId, float timer, bool visibility) (0x388AAFE0, CGameRules vtable
// 0x38FD4428 slot +0x1A0; logs "[REMOVALS] Scheduling %s %s '%s' for removal %s %.2f seconds"): server only, inserts
// into the removal map (+0x32E8) that CGameRules::Update (0x388B8A80, "[REMOVALS] Removing ...") deletes from. In MP
// the visibility (off-screen) condition is dropped (0x388AB054 `cmp [bMultiplayer]`), and every hidden weapon
// accessory (Reflex, ForeGrip, AssaultScope, LaserSight, Silencer...) of the campaign gets deleted after 10 s.
// IEntity::IsHidden is vtbl+0x180, IEntity::GetName vtbl+0x60, IEntityClass::GetName vtbl+0x10 (same function).
constexpr uintptr_t kRvaScheduleEntityRemoval = 0x18AAFE0;
constexpr unsigned char kScheduleEntityRemovalBytes[] = {0x40, 0x53, 0x55, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x70,
                                                         0x80, 0x3D, 0x10, 0x55, 0xBA, 0x00, 0x00};
constexpr size_t kVtEntityIsHidden = 0x180;
constexpr size_t kVtEntityGetName = 0x60;
constexpr size_t kVtEntityClassGetName = 0x10;

// Actor:AliveCheck ProcessEvent (0x388E87F0, flow node vtable slot at 0x38FDAFC8; its error path 0x388E89A6 logs
// "[flow] Actor:AliveCheck - flowgraph entity ..."): actor = GetIActorSystem()->GetActor(entityId), then
//   0x388E8877: mov rdx,[rax] / mov rcx,rax / call [rdx+100h] (IActor::IsDead) / mov rcx,[rsi] / (lea r8,...)
// and outputs Status = !dead, then Alive (port 1) or Dead (port 2). The 15-byte site jumps to a stub that asks
// coop.cpp instead. IActor::IsClient (local player) is vtbl+0x220 (CActor::IsRemote 0x3853BECC).
constexpr uintptr_t kRvaAliveCheckIsDeadSite = 0x18E8877;
constexpr unsigned char kAliveCheckIsDeadSiteBytes[] = {0x48, 0x8B, 0x10, 0x48, 0x8B, 0xC8, 0xFF, 0x92,
                                                        0x00, 0x01, 0x00, 0x00, 0x48, 0x8B, 0x0E};
constexpr uintptr_t kRvaAliveCheckIsDeadContinue = 0x18E8886;
constexpr size_t kVtActorIsDead = 0x100;
constexpr size_t kVtActorIsClient = 0x220;

// RMI "at sync" items (IGameObject.h templates, 25 instantiations): an RMI for an entity the client does not have
// ("Entity %d for RMI %s not found", e.g. RMI:CPlayer:ClSwarmerPerkComplete) logs "Error handling RMI %s" and then,
// unless gEnv->bServer or the channel is local, calls channel->Disconnect(7 = eDC_ContextCorruption, msg):
//   call [rax+100h] / test al,al / jne <skip> / ... mov edx,7 / ... call [rax+50h]
// The co-op client turns each jne into jmp (drops the RMI like the server does). Sites: the jne byte (0x75).
constexpr uintptr_t kRvaRmiDisconnectJne[] = {
    0x386437,  0x386627,  0x3DC7FF,  0x3DC9BF,  0x3DCB2F,  0x3DCCAF,  0x1543B40, 0x1543C9F, 0x1543DFF,
    0x1543F5F, 0x16151EF, 0x1615370, 0x17C031F, 0x17C047F, 0x18D2BA0, 0x18D2D20, 0x18D2EA0, 0x18D2FFF,
    0x18D3165, 0x18D32E0, 0x18D343F, 0x18D35C0, 0x18D3740, 0x1B51C77, 0x1B51DC7};
// bytes before the jne: call [rax+100h] / test al,al
constexpr unsigned char kRmiDisconnectPrefix[] = {0xFF, 0x90, 0x00, 0x01, 0x00, 0x00, 0x84, 0xC0};

// Net message SetAspectProfile (0x37A89330, client context view): reads "profile" first, then looks up the object
// being updated; a missing one logs "Object %s not found for SetAspectProfile" and kicks the client:
//   0x37A8940A: mov rcx,[rbx+0B0h] / lea r8,"SetAspectProfile - object not found" / mov edx,7 / mov rax,[rcx] /
//               call [rax+50h] / xor al,al / (0x37A89425: epilogue)
// The co-op client returns true there instead (mov al,1 / jmp epilogue): the message was read, only dropped.
constexpr uintptr_t kRvaSetAspectProfileKick = 0xA8940A;
constexpr unsigned char kSetAspectProfileKickBytes[] = {0x48, 0x8B, 0x8B, 0xB0, 0x00, 0x00, 0x00, 0x4C, 0x8D, 0x05,
                                                        0xF8, 0x7D, 0x6D, 0x01, 0xBA, 0x07, 0x00, 0x00, 0x00};
constexpr unsigned char kSetAspectProfileKickPatch[] = {0xB0, 0x01, 0xEB, 0x17};

// NetWarning(fmt, ...) (0x37A78420): prints only while a CryNetwork log cvar ([0x39966548]+8) is on, so object names of
// net errors ("Object %s not found for SetAspectProfile") never reach the log. GameWarning(fmt, ...) (0x373068A0)
// -> ISystem::Warning (validator, "Error handling RMI %s"). The DLL copies both into its own log.
constexpr uintptr_t kRvaNetWarning = 0xA78420;
constexpr unsigned char kNetWarningBytes[] = {0x48, 0x89, 0x4C, 0x24, 0x08, 0x48, 0x89, 0x54, 0x24, 0x10};
constexpr uintptr_t kRvaGameWarning = 0x3068A0;
constexpr unsigned char kGameWarningBytes[] = {0x48, 0x85, 0xC9, 0x74, 0x4D, 0x4C, 0x8B, 0xDC};

// Loading saves in a network game (pause menu / death screen). The front end's warning handler (0x38926AD0) runs
// "loadLastSave" (LoadLastSave warning, 0x38927149) or "load <file>" (LoadSavegame, 0x3892719F) through
// IGameFramework::ExecuteCommandNextFrame (vtbl+0x3D8) -> IConsole::ExecuteString. CGame::LoadLastSave (0x386BF260,
// "loadLastSave" command handler 0x386D1CB0) and CCryAction::LoadGame (0x3760D4A0, vtbl+0x2B8) already return early
// when bMultiplayer (0x386BF266 / 0x3760D4F8), so nothing is loaded; the DLL intercepts both commands to explain it.

// IConsole vtable offsets (verified against CXConsole vtable bodies)
constexpr size_t kVtAddOutputPrintSink = 0x58;
constexpr size_t kVtRemoveOutputPrintSink = 0x60;
constexpr size_t kVtGetCVar = 0xB0;
constexpr size_t kVtUpdate = 0xE8;
// AddCommand(name, void(*)(IConsoleCmdArgs*), flags, help): "map" is registered so at 0x3760EA74
constexpr size_t kVtAddCommand = 0x100;
constexpr size_t kVtExecuteString = 0x110;

// CXConsole::m_pSystem (CXConsole::Update starts with `cmp qword [rcx+0x1F8],0`)
constexpr size_t kConsoleSystemOffset = 0x1F8;
// ISystem::IsDevMode: con_restricted init does `if (m_pSystem->vtbl[0x300]() || ...) con_restricted = 0`
constexpr size_t kVtSystemIsDevMode = 0x300;

// CXConsole::ExecuteString(this, const char* cmd, bool silent, bool deferred) = vtbl+0x110 (kVtExecuteString)
using ExecuteStringFn = void(__fastcall*)(void* console, const char* cmd, bool silent, bool deferred);

// CXConsole calls sinks via `call [vtbl+8]` (0x37E178C9), so slot 0 is the destructor.
struct IOutputPrintSink {
    virtual ~IOutputPrintSink() = default;
    virtual void Print(const char* text) = 0;
};

inline uintptr_t Base() {
    static uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    return base;
}

template <typename T>
inline T* At(uintptr_t rva) {
    return reinterpret_cast<T*>(Base() + rva);
}

inline void* Console() {
    return *At<void*>(kRvaConsoleSlot);
}

template <typename Fn>
inline Fn Virtual(void* obj, size_t offset) {
    auto vtbl = *reinterpret_cast<uintptr_t**>(obj);
    return reinterpret_cast<Fn>(vtbl[offset / sizeof(uintptr_t)]);
}

inline uint32_t TimeDateStamp() {
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(Base());
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(Base() + dos->e_lfanew);
    return nt->FileHeader.TimeDateStamp;
}

} // namespace game
