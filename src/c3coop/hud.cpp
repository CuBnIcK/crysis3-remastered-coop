// Story HUD, mission objectives and flowgraph behaviour in network games.
//
// The campaign runs as a network game (gEnv->bMultiplayer = 1 everywhere, bServer only on the host), which sends
// the HUD, its flowgraph nodes, checkpoints and the gamerules down their MP branches. Each fix below restores the
// single-player behaviour of one specific check and can be toggled at runtime for A/B tests:
//
//   !hud status                      MO system, gamerules prematch state, saturation, HUD cvars, patch states
//   !hud <fix> on|off                localfg, inputactor, boot, bootanims, prematch, cutscene, save, mo
//   !hud prematch now                ChangePrematchState(ePS_Match) on the running gamerules (grey picture)
//   !hud mo create | reload          create the mission objective system if missing / reload Objectives xml
//   !hud sat [reset]                 saturation slots of CParameterGameEffect (reset = all slots back to 1)
//   !hud reboot <0..5>               dispatch eHUDEvent_HUDBoot like HUD:Reboot (0 normal, 1 boot, 2 destroy, ...)
//   !hud state <name> | default      CHUD::ActivateState(name) / CHUD::ActivateDefaultState()
//
// All addresses are for Crysis3Remastered.exe build 0x62FD4D1B (fixed base 0x37000000), RVA = VA - 0x37000000.

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

#include "MinHook.h"
#include "game.h"
#include "modules.h"

namespace {

// ---------------------------------------------------------------- globals and object layouts

// CGame* g_pGame (VA 0x39529938). CScriptBind_HUD::SetObjectiveEntity (0x3884CB70, found through its
// "Tried to access non existing MissionObjective" warning) does `mov rax,[g_pGame]; mov rcx,[rax+1E0h]`.
constexpr uintptr_t kRvaGamePtr = 0x2529938;
// gEnv->p3DEngine (VA 0x39450328). CParameterGameEffect::SetActive (0x38421430) calls vtbl+0x4A8
// GetPostEffectParam(name, float&) and vtbl+0x490 SetPostEffectParam(name, float, bool) on it.
constexpr uintptr_t kRva3DEngineSlot = 0x2450328;
constexpr size_t kVt3DEngineGetPostEffectParam = 0x4A8;

// CGame fields
// +0x1E0 CHUDMissionObjectiveSystem* m_pMOSystem: CGame::Init 0x386B34F9 `if (!bMultiplayer && !m_pMOSystem)
//        m_pMOSystem = new (0x90) CHUDMissionObjectiveSystem` and CGame::InitGameType 0x386BC8D5 deletes it in MP.
constexpr size_t kGameMOSystem = 0x1E0;
// +0x220 CParameterGameEffect* (CGame::Init 0x386B3781 stores the 0x40-byte object built by 0x384213A0, whose
//        vtable 0x38F4B718 holds SetActive 0x38421430 / Update 0x384214E0 that write Global_User_Saturation).
constexpr size_t kGameParameterEffect = 0x220;
// +0x328 CHUD* (HUD:Reboot's DoHudEvent 0x389152B0 reads [[g_pGame+328h]+98h]+0DCh = hud_boot_enableForMP;
//        CGameRules::NetSerialize 0x388A665D calls CHUD::ActivateDefaultState on it).
constexpr size_t kGameHud = 0x328;
constexpr size_t kHudCVars = 0x98;
constexpr size_t kHudCVarBootEnableForMP = 0xDC;

// CHUDMissionObjectiveSystem: +0x18/+0x20 vector<CHUDMissionObjective> (0x40 bytes each), +0x88 bool
// m_bLoadedObjectives (LoadLevelObjectives 0x3883C510: `cmp byte [rcx+88h],0`, loop `add rbx,40h`).
constexpr size_t kMOObjectivesBegin = 0x18;
constexpr size_t kMOObjectivesEnd = 0x20;
constexpr size_t kMOObjectiveSize = 0x40;
constexpr size_t kMOLoaded = 0x88;
constexpr size_t kMOSystemSize = 0x90; // `mov ecx,90h; call new` at 0x386B3502 and 0x386BC9E2

// CGameRules +0x3854 EPrematchState m_prematchState (0 Prematch, 1 WaitingForPlayers, 2 Countdown, 3 Match):
// written by CGameRules::Init 0x388A3478/0x388A348C and ChangePrematchState 0x388BA2CB, serialized as
// "prematchState" in NetSerialize 0x388A648F.
constexpr size_t kRulesPrematchState = 0x3854;
constexpr int kPrematchMatch = 3;

// CParameterGameEffect +0x2C float m_saturation[5] (PreMatch, LeavingBattleArea, PlayerHealth, Intro, Hacking):
// ResetSaturation 0x38421910 writes 1,1,1,-1(intro),1 to +0x2C..+0x3C.
constexpr size_t kParamSaturation = 0x2C;
constexpr const char* kSaturationSlots[5] = {"prematch", "leavingBattleArea", "playerHealth", "intro", "hacking"};

// ---------------------------------------------------------------- functions we call

// operator new of the game module (0x370B9100): `cmp [allocator ready],0 ... call [alloc]`, used for every
// `new` in CGame::Init/InitGameType (e.g. 0x386B3502 `mov ecx,90h; call 370B9100h`).
constexpr uintptr_t kRvaGameNew = 0x0B9100;
constexpr unsigned char kGameNewBytes[] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x83, 0x3D};
// CHUDMissionObjectiveSystem::CHUDMissionObjectiveSystem() (0x3883C210), called right after the 0x90 allocation
// in CGame::Init (0x386B3514) and InitGameType (0x386BC9F4); returns this.
constexpr uintptr_t kRvaMOSystemCtor = 0x183C210;
constexpr unsigned char kMOSystemCtorBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10};
// CHUDMissionObjectiveSystem::LoadLevelObjectives(this, const char* levelPath, bool force) (0x3883C510): calls
// LoadLevelObjectivesInternal 0x3883C680 ("Libs/UI/Objectives_new.xml" / "%s/leveldata/Objectives.xml") with
// NULL and then with levelPath. Called by the front end on level load (0x3893387B) when m_pMOSystem != NULL.
constexpr uintptr_t kRvaMOLoadLevelObjectives = 0x183C510;
constexpr unsigned char kMOLoadLevelObjectivesBytes[] = {0x48, 0x89, 0x74, 0x24, 0x20, 0x41, 0x56, 0x48, 0x83,
                                                         0xEC, 0x40, 0x80, 0xB9, 0x88, 0x00, 0x00, 0x00, 0x00};
// CGame::GetGameRules(this) (0x386BDB40): m_pFramework(+0x48)->GetIGameRulesSystem()->GetCurrentGameRules().
constexpr uintptr_t kRvaGameGetGameRules = 0x16BDB40;
constexpr unsigned char kGameGetGameRulesBytes[] = {0x48, 0x83, 0xEC, 0x28, 0x48, 0x8B, 0x49, 0x48,
                                                    0x48, 0x8B, 0x01, 0xFF, 0x90, 0x08, 0x01, 0x00, 0x00};
// CGameRules::ChangePrematchState(this, state) (0x388BA2B0): for ePS_Match it resets the PreMatch saturation
// (SetSaturationAmount(1, 0) at 0x388BA50C) and on the server marks aspect 0x20 dirty so clients follow.
constexpr uintptr_t kRvaChangePrematchState = 0x18BA2B0;
constexpr unsigned char kChangePrematchStateBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x18, 0x57, 0x48, 0x83,
                                                       0xEC, 0x40, 0x8B, 0x99, 0x54, 0x38, 0x00, 0x00};
// CParameterGameEffect::SetSaturationAmount(this, float amount, int usage) (0x384216E0):
// `mov eax,r8d ... vmovss [rcx+rax*4+2Ch],xmm1`; NetSerialize calls it with -1/usage 0 while not in ePS_Match.
constexpr uintptr_t kRvaSetSaturationAmount = 0x14216E0;
constexpr unsigned char kSetSaturationAmountBytes[] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x30, 0x41, 0x8B, 0xC0};
// CHUD::ActivateState(this, const char*) (0x38833E40): ActivateDefaultState ends in `mov rcx,rsi; call 38833E40h`
// with the state name in rdx ("mp_killcam", "no_hud", GameModes.xml DefaultHudState...).
constexpr uintptr_t kRvaHudActivateState = 0x1833E40;
constexpr unsigned char kHudActivateStateBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20};
// CHUD::ActivateDefaultState(this) (0x38833F10): references "mp_killcam" ... "mp_welcome_prematch_countdown".
constexpr uintptr_t kRvaHudActivateDefaultState = 0x1833F10;
constexpr unsigned char kHudActivateDefaultStateBytes[] = {0x40, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x28, 0x48, 0x8B, 0xF1};
// CFlowNode_HUDReboot::DoHudEvent(this, EBootableSystemAction) (0x389152B0, 4 callers from the node's ProcessEvent
// 0x3890A770 with edx = 0..3): `if (!bMultiplayer || hud_boot_enableForMP)` CallEvent(eHUDEvent_HUDBoot = 0xB).
// It never reads `this`.
constexpr uintptr_t kRvaHudRebootDoHudEvent = 0x19152B0;
constexpr unsigned char kHudRebootDoHudEventBytes[] = {0x48, 0x83, 0xEC, 0x68, 0x80, 0x3D, 0x46, 0xB2, 0xB3, 0x00, 0x00};

// ---------------------------------------------------------------- hooked functions

// CCryAction::SaveGame(this, path, quick, forceImmediate, reason, ignoreDelay, checkpoint) (0x3760D030, found via
// "[SAVE GAME] %s to '%s'%s%s - checkpoint=\"%s\"" and "Saving failed : player is dead!"; CCryAction vtbl slot +0x380).
// 0x3760D0DB: `cmp byte [bMultiplayer],0; jne return false` -> eAE_postSaveGame (0xA) is never sent, so every
// Game:SaveGame node waits forever and the flowgraph chain after a checkpoint stalls.
constexpr uintptr_t kRvaSaveGame = 0x60D030;
constexpr unsigned char kSaveGameBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C,
                                            0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x20, 0x44};
// CCryAction::OnActionEvent(this, const SActionEvent&) (game::kRvaCryActionOnActionEvent 0x37612850): SaveGame sends
// {9 preSaveGame} at 0x3760D29B and {0xA postSaveGame, reason, checkpoint name or NULL} at 0x3760D333 through it.
// coop.cpp hooks it later (connection events), which rewrites its first bytes: the signature is checked once in
// InstallHudHooks, before any module hooks it.
constexpr int kActionEventPostSaveGame = 0xA;
bool g_actionEventSigOk = false;

// CGame::InitGameType(this, bool multiplayer, bool fromInit, bool) (0x386BC450, "CGame::InitGameType() multiplayer=%s,
// fromInit=%s"): the MP branch deletes m_pMOSystem (0x386BC8D5..0x386BC8ED), the SP branch creates it.
constexpr uintptr_t kRvaInitGameType = 0x16BC450;
constexpr unsigned char kInitGameTypeBytes[] = {0x40, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x48};

// ---------------------------------------------------------------- byte patches
// Each patch: signature at `rva` (original bytes), and `repl` written at rva + offset.

struct BytePatch {
    const char* what;
    uintptr_t rva;
    const unsigned char* sig;
    size_t sigLen;
    size_t offset;
    const unsigned char* repl;
    size_t replLen;
};

// [localfg] InputEntityIsLocalPlayer (0x3743C830, 51 callers: every CFlowBaseNode/CGameFlowBaseNode copy was
// folded into this one; HUD:Reboot ProcessEvent 0x3890A78A calls it): with no entity on the node it returns
// `bMultiplayer ? false : true` via 0x3743C8D4 `cmp [bMultiplayer],cl` / 0x3743C8DF `cmovne eax,ecx`.
// NOP the cmovne -> unwired HUD:ShowVisor/DrawSprite/ShowSuitMessage, Game:ForceFeedback, Image faders... act
// on the local player of every machine, as in single player.
constexpr unsigned char kLocalFgSig[] = {0x33, 0xC9, 0x40, 0x0F, 0xB6, 0xC5, 0x38, 0x0D, 0x27, 0x3C,
                                         0x01, 0x02, 0x48, 0x8B, 0x6C, 0x24, 0x40, 0x0F, 0x45, 0xC1};
constexpr unsigned char kNop3[] = {0x0F, 0x1F, 0x00};
const BytePatch kPatchLocalFg = {"InputEntityIsLocalPlayer: no entity = local player",
                                 0x43C8CE, kLocalFgSig, sizeof(kLocalFgSig), 17, kNop3, sizeof(kNop3)};

// [inputactor] GetInputActor (0x3746EA60, 12 callers, all Inventory:* nodes): with no entity it returns
// GetClientActor() only `if (!bMultiplayer)` (0x3746EAC3 cmp / 0x3746EACA jne 1F). NOP the jne.
constexpr unsigned char kInputActorSig[] = {0x33, 0xC0, 0x80, 0x3D, 0x37, 0x1A, 0xFE, 0x01, 0x00, 0x75, 0x1F};
constexpr unsigned char kNop2[] = {0x90, 0x90};
const BytePatch kPatchInputActor = {"GetInputActor: no entity = local player",
                                    0x46EAC1, kInputActorSig, sizeof(kInputActorSig), 9, kNop2, sizeof(kNop2)};

// [boot] HUD:Reboot DoHudEvent 0x389152B4: `cmp [bMultiplayer],0; je dispatch` then `if (!hud_boot_enableForMP)
// return` -> je becomes jmp (same as hud_boot_enableForMP 1, without depending on the cvar).
constexpr unsigned char kBootRebootSig[] = {0x80, 0x3D, 0x46, 0xB2, 0xB3, 0x00, 0x00, 0x74, 0x22};
constexpr unsigned char kJmpShort[] = {0xEB};
const BytePatch kPatchBootReboot = {"HUD:Reboot boot event in MP",
                                    0x19152B4, kBootRebootSig, sizeof(kBootRebootSig), 7, kJmpShort, 1};
// HUD:Hide ProcessEvent (0x38905CF0, "m_bHidingHud" in its Serialize 0x38905DA0) at 0x38905D3C: same check.
constexpr unsigned char kBootHideSig[] = {0x80, 0x3D, 0xBE, 0xA7, 0xB4, 0x00, 0x00, 0x74, 0x10};
const BytePatch kPatchBootHide = {"HUD:Hide in MP", 0x1905D3C, kBootHideSig, sizeof(kBootHideSig), 7, kJmpShort, 1};

// [bootanims] CHUD_BootSystem::Init (0x38856700, "bootsystem"/"BootAnims"): in MP it skips the BootAnims
// sub-asset and calls OnSubAssetLoaded directly (0x38856713 `je` not taken -> `jmp [rax+40h]`), and picks
// "/libs/ui/allinone_hud_3d_ammo_mp_bootanims.gfx" (not shipped) via 0x38856743 `cmovne rcx,rdx`.
// Takes effect when the HUD (re)initialises its objects.
constexpr unsigned char kBootInitSig[] = {0x80, 0x3D, 0xF4, 0x9D, 0xBF, 0x00, 0x00, 0x48,
                                          0x8B, 0xD9, 0x48, 0x8B, 0x01, 0x74, 0x09};
const BytePatch kPatchBootInit = {"HUD_BootSystem::Init SP path",
                                  0x1856706, kBootInitSig, sizeof(kBootInitSig), 13, kJmpShort, 1};
constexpr unsigned char kBootGfxSig[] = {0x80, 0x3D, 0xCF, 0x9D, 0xBF, 0x00, 0x00, 0x48, 0x8D, 0x15,
                                         0x6F, 0x4B, 0x77, 0x00, 0x48, 0x8D, 0x0D, 0x98, 0x4B, 0x77,
                                         0x00, 0x4C, 0x8B, 0xC0, 0x48, 0x0F, 0x45, 0xCA};
constexpr unsigned char kNop4[] = {0x0F, 0x1F, 0x40, 0x00};
const BytePatch kPatchBootGfx = {"HUD_BootSystem::Init pc_bootanims.gfx",
                                 0x185672B, kBootGfxSig, sizeof(kBootGfxSig), 24, kNop4, sizeof(kNop4)};

// [prematch] CGameRules::Init (0x388A24F0) at 0x388A3468: `if (bMultiplayer) m_prematchState = ePS_Prematch
// (+ "PregameLoop" audio) else m_prematchState = ePS_Match`. SinglePlayer gamerules have no state module, so
// CGameRules::Update never leaves ePS_Prematch, and CGameRules::NetSerialize (0x388A671B) calls
// SetSaturationAmount(-1, PreMatch) on host and client while the state != ePS_Match -> the grey picture.
// je -> jmp: every new gamerules starts in ePS_Match, as in single player.
constexpr unsigned char kPrematchSig[] = {0x80, 0x3D, 0x92, 0xD0, 0xBA, 0x00, 0x00, 0x74, 0x1B};
const BytePatch kPatchPrematch = {"CGameRules::Init starts in ePS_Match",
                                  0x18A3468, kPrematchSig, sizeof(kPrematchSig), 7, kJmpShort, 1};

// [cutscene] CHUD::OnBeginCutScene (IMovieListener thunk 0x388337F0) at 0x38833937: for eSeqFlags_NoHUD sequences
// `if (bMultiplayer) ActivateState("no_hud")` (0x38833942 jne 0x388339DB), otherwise the SP path
// ("sp_cutscene" / menu). NOP the jne.
constexpr unsigned char kCutsceneSig[] = {0x80, 0x3D, 0xC3, 0xCB, 0xC1, 0x00, 0x00, 0x48, 0x8D,
                                          0x4F, 0xF8, 0x0F, 0x85, 0x93, 0x00, 0x00, 0x00};
constexpr unsigned char kNop6[] = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
const BytePatch kPatchCutscene = {"CHUD::OnBeginCutScene SP state",
                                  0x1833937, kCutsceneSig, sizeof(kCutsceneSig), 11, kNop6, sizeof(kNop6)};

// [deadhud] CHUD::ActivateDefaultState (0x38833F10): for a dead/spectating client actor whose player stats lack the
// "spawned this game" flag (stats byte +0x2E bit 4) it takes the MP welcome-screen branch at 0x38834265, which
// calls the gamerules state module (NULL in SinglePlayer) -> AV when the host's player dies and follows a
// teammate. 0x38834209 `je 0x38834265` -> `jmp 0x38834210` ("no_hud", defined in HUDStateDef_sp.xml).
constexpr unsigned char kDeadHudSig[] = {0x48, 0x8B, 0x07, 0x48, 0x8B, 0xCF, 0xFF, 0x90, 0xF8,
                                         0x03, 0x00, 0x00, 0x40, 0x84, 0xED, 0x74, 0x5A};
constexpr unsigned char kJmpNoHud[] = {0xEB, 0x05};
const BytePatch kPatchDeadHud = {"CHUD::ActivateDefaultState dead spectator -> no_hud",
                                 0x18341FA, kDeadHudSig, sizeof(kDeadHudSig), 15, kJmpNoHud, sizeof(kJmpNoHud)};

// [radar] CHUD_Radar::OnHUDEvent(eHUDEvent_AddEntity) at 0x3888689D: `if (bMultiplayer) AddEntity(6, id)` for every
// entity (no ShouldBeAddedOnRadar check: all campaign AI -> "Radar data full" warnings), else the SP path that adds
// mission objectives, the local player and filtered actors. 0x388868AC `je 0x388868DF` (SP path) -> `jmp`.
constexpr unsigned char kRadarAddSig[] = {0x80, 0x3D, 0x5D, 0x9C, 0xBC, 0x00, 0x00, 0x48, 0x8B,
                                          0x42, 0x08, 0x44, 0x8B, 0x70, 0x10, 0x74, 0x31};
const BytePatch kPatchRadarAdd = {"CHUD_Radar AddEntity event SP path",
                                  0x188689D, kRadarAddSig, sizeof(kRadarAddSig), 15, kJmpShort, 1};

// [teamcross] CHUD_Crosshair::GetTargetTypeFromTeam(clientId, targetId) (0x3885F850, two CGameRules::GetTeam calls,
// returns eTT_Friend 3 / eTT_Enemy 2): `if (ownTeam != targetTeam || ownTeam == 0) enemy`. Co-op players are all in
// team 0, so teammates got the red enemy brackets. 0x3885F893 `jne` -> `jmp`: same team (0 included) = friend.
constexpr unsigned char kTeamCrossSig[] = {0x3B, 0xD8, 0x75, 0x09, 0xB8, 0x03, 0x00, 0x00, 0x00, 0x85, 0xDB, 0x75, 0x05};
const BytePatch kPatchTeamCross = {"crosshair: same team (also 0) is a friend",
                                   0x185F888, kTeamCrossSig, sizeof(kTeamCrossSig), 11, kJmpShort, 1};

// ---------------------------------------------------------------- state

std::atomic<bool> g_fixSave{true};
std::atomic<bool> g_fixMO{true};
std::atomic<bool> g_fixRealSave{false};
std::atomic<bool> g_fixAutoState{true};
bool g_saveHooked = false;
bool g_initGameTypeHooked = false;
std::atomic<int> g_fakeSaves{0};

template <size_t N>
bool Sig(uintptr_t rva, const unsigned char (&bytes)[N]) {
    return memcmp(game::At<unsigned char>(rva), bytes, N) == 0;
}

// 0 = original bytes, 1 = patched, -1 = neither (unknown build / someone else patched it)
int PatchState(const BytePatch& p) {
    auto at = game::At<unsigned char>(p.rva);
    bool orig = memcmp(at, p.sig, p.sigLen) == 0;
    if (orig)
        return 0;
    bool patched = memcmp(at, p.sig, p.offset) == 0 && memcmp(at + p.offset, p.repl, p.replLen) == 0 &&
                   memcmp(at + p.offset + p.replLen, p.sig + p.offset + p.replLen, p.sigLen - p.offset - p.replLen) == 0;
    return patched ? 1 : -1;
}

bool SetPatch(const BytePatch& p, bool on) {
    int state = PatchState(p);
    auto at = game::At<unsigned char>(p.rva);
    if (state < 0) {
        c3coop::Logf("hud: %s: bytes at %p do not match the signature, NOT patched", p.what, at);
        return false;
    }
    if (state == (on ? 1 : 0))
        return true;
    unsigned char* dst = at + p.offset;
    DWORD old;
    if (!VirtualProtect(dst, p.replLen, PAGE_EXECUTE_READWRITE, &old)) {
        c3coop::Logf("hud: %s: VirtualProtect failed (%lu)", p.what, GetLastError());
        return false;
    }
    memcpy(dst, on ? p.repl : p.sig + p.offset, p.replLen);
    VirtualProtect(dst, p.replLen, old, &old);
    FlushInstructionCache(GetCurrentProcess(), dst, p.replLen);
    return true;
}

const char* PatchStateName(const BytePatch& p) {
    switch (PatchState(p)) {
    case 0:
        return "off";
    case 1:
        return "on";
    default:
        return "MISMATCH";
    }
}

// ---------------------------------------------------------------- game object access

char* Game() {
    return *game::At<char*>(kRvaGamePtr);
}

template <typename T>
T& Field(void* obj, size_t off) {
    return *reinterpret_cast<T*>(static_cast<char*>(obj) + off);
}

void* Hud() {
    char* g = Game();
    return g ? Field<void*>(g, kGameHud) : nullptr;
}

void* GameRules() {
    char* g = Game();
    if (!g || !Sig(kRvaGameGetGameRules, kGameGetGameRulesBytes))
        return nullptr;
    // CGame::m_pFramework (+0x48) is NULL only before CGame::Init
    if (!Field<void*>(g, 0x48))
        return nullptr;
    return game::At<void*(__fastcall)(void*)>(kRvaGameGetGameRules)(g);
}

bool IsMultiplayer() {
    return *game::At<unsigned char>(game::kRvaMultiplayerFlag) != 0;
}

bool IsServer() {
    return *game::At<unsigned char>(game::kRvaServerFlag) != 0;
}

// ---------------------------------------------------------------- mission objectives

void* CreateMOSystem(const char* why) {
    char* g = Game();
    if (!g)
        return nullptr;
    void*& mo = Field<void*>(g, kGameMOSystem);
    if (mo)
        return mo;
    if (!Sig(kRvaGameNew, kGameNewBytes) || !Sig(kRvaMOSystemCtor, kMOSystemCtorBytes)) {
        c3coop::Logf("hud: cannot create the MO system (allocator/ctor bytes differ)");
        return nullptr;
    }
    void* mem = game::At<void*(__fastcall)(size_t)>(kRvaGameNew)(kMOSystemSize);
    if (!mem) {
        c3coop::Logf("hud: MO system allocation failed");
        return nullptr;
    }
    mo = game::At<void*(__fastcall)(void*)>(kRvaMOSystemCtor)(mem);
    c3coop::Logf("hud: created CHUDMissionObjectiveSystem %p (%s)", mo, why);
    return mo;
}

void LoadObjectives(void* mo, bool force) {
    if (!mo || !Sig(kRvaMOLoadLevelObjectives, kMOLoadLevelObjectivesBytes))
        return;
    // NULL level path: Libs/UI/Objectives_new.xml only (the Jailbreak level.pak has no leveldata/Objectives.xml)
    game::At<void(__fastcall)(void*, const char*, bool)>(kRvaMOLoadLevelObjectives)(mo, nullptr, force);
    auto begin = Field<char*>(mo, kMOObjectivesBegin);
    auto end = Field<char*>(mo, kMOObjectivesEnd);
    c3coop::Logf("hud: objectives loaded: %lld (loaded flag %d)", (long long)((end - begin) / kMOObjectiveSize),
                 Field<unsigned char>(mo, kMOLoaded));
}

using InitGameTypeFn = void(__fastcall*)(void* game, bool multiplayer, bool fromInit, bool arg4);
InitGameTypeFn g_origInitGameType;

void __fastcall HookedInitGameType(void* gamePtr, bool multiplayer, bool fromInit, bool arg4) {
    void* before = Field<void*>(gamePtr, kGameMOSystem);
    g_origInitGameType(gamePtr, multiplayer, fromInit, arg4);
    void* after = Field<void*>(gamePtr, kGameMOSystem);
    c3coop::Logf("hud: CGame::InitGameType(multiplayer=%d, fromInit=%d): MO system %p -> %p", multiplayer, fromInit,
                 before, after);
    if (g_fixMO && !after)
        CreateMOSystem("InitGameType deleted it / MP game type");
}

// ---------------------------------------------------------------- checkpoints

struct SActionEvent {
    int event;
    int value;
    const char* description;
};

using SaveGameFn = bool(__fastcall*)(void* self, const char* path, bool quick, bool forceImmediate, int reason,
                                     bool ignoreDelay, const char* checkpoint);
SaveGameFn g_origSaveGame;

// In a network game nothing is written (a co-op state cannot be reloaded anyway); the listeners get the
// eAE_postSaveGame they wait for, with the checkpoint name as the "success" description (as CryAction does).
bool __fastcall HookedSaveGame(void* self, const char* path, bool quick, bool forceImmediate, int reason,
                               bool ignoreDelay, const char* checkpoint) {
    if (!g_fixSave || !IsMultiplayer() || !g_actionEventSigOk)
        return g_origSaveGame(self, path, quick, forceImmediate, reason, ignoreDelay, checkpoint);
    if (g_fixRealSave && IsServer()) {
        // experiment: the host owns the whole world state, save it as single player would
        auto mp = game::At<unsigned char>(game::kRvaMultiplayerFlag);
        *mp = 0;
        bool ok = g_origSaveGame(self, path, quick, forceImmediate, reason, ignoreDelay, checkpoint);
        *mp = 1;
        c3coop::Logf("hud: SaveGame('%s', reason %d, checkpoint '%s') on the host as single player -> %s",
                     path ? path : "", reason, checkpoint ? checkpoint : "", ok ? "saved" : "FAILED");
        if (ok)
            return true;
    }
    int n = g_fakeSaves.fetch_add(1) + 1;
    c3coop::Logf("hud: SaveGame('%s', reason %d, checkpoint '%s') in network game -> postSaveGame without saving (#%d)",
                 path ? path : "", reason, checkpoint ? checkpoint : "", n);
    SActionEvent ev{kActionEventPostSaveGame, reason, checkpoint ? checkpoint : ""};
    game::At<void(__fastcall)(void*, const SActionEvent*)>(game::kRvaCryActionOnActionEvent)(self, &ev);
    return true;
}

// ---------------------------------------------------------------- prematch / saturation

void ForceMatchState(const char* why) {
    void* rules = GameRules();
    if (!rules) {
        c3coop::Logf("hud: no gamerules yet");
        return;
    }
    int& state = Field<int>(rules, kRulesPrematchState);
    if (state == kPrematchMatch) {
        c3coop::Logf("hud: gamerules %p already in ePS_Match", rules);
        return;
    }
    if (!Sig(kRvaChangePrematchState, kChangePrematchStateBytes)) {
        c3coop::Logf("hud: ChangePrematchState bytes differ, not called");
        return;
    }
    int old = state;
    game::At<void(__fastcall)(void*, int)>(kRvaChangePrematchState)(rules, kPrematchMatch);
    c3coop::Logf("hud: gamerules %p prematch state %d -> %d (%s)%s", rules, old, state, why,
                 IsServer() ? ", clients follow on the next serialization" : " (local only on a client)");
}

void DumpSaturation(bool reset) {
    char* g = Game();
    void* effect = g ? Field<void*>(g, kGameParameterEffect) : nullptr;
    if (!effect) {
        c3coop::Logf("hud: no CParameterGameEffect");
        return;
    }
    auto slots = reinterpret_cast<float*>(static_cast<char*>(effect) + kParamSaturation);
    if (reset) {
        if (!Sig(kRvaSetSaturationAmount, kSetSaturationAmountBytes)) {
            c3coop::Logf("hud: SetSaturationAmount bytes differ, not reset");
        } else {
            auto set = game::At<void(__fastcall)(void*, float, int)>(kRvaSetSaturationAmount);
            for (int i = 0; i < 5; ++i)
                set(effect, i == 3 ? -1.0f : 1.0f, i); // intro: -1 = disabled
        }
    }
    float current = -99.0f;
    if (void* engine = *game::At<void*>(kRva3DEngineSlot))
        game::Virtual<void(__fastcall*)(void*, const char*, float*)>(engine, kVt3DEngineGetPostEffectParam)(
            engine, "Global_User_Saturation", &current);
    c3coop::Logf("hud: saturation slots %s=%.2f %s=%.2f %s=%.2f %s=%.2f %s=%.2f; Global_User_Saturation=%.2f%s",
                 kSaturationSlots[0], slots[0], kSaturationSlots[1], slots[1], kSaturationSlots[2], slots[2],
                 kSaturationSlots[3], slots[3], kSaturationSlots[4], slots[4], current, reset ? " (after reset)" : "");
}

// ---------------------------------------------------------------- fixes table

struct Fix {
    const char* name;
    const BytePatch* patches[2];
    std::atomic<bool>* flag; // hook-based fixes
    bool defaultOn;
    const char* help;
};

Fix g_fixes[] = {
    {"localfg", {&kPatchLocalFg, nullptr}, nullptr, true, "flow nodes without entity act on the local player"},
    {"inputactor", {&kPatchInputActor, nullptr}, nullptr, false, "Inventory:* nodes without entity use the local player"},
    {"boot", {&kPatchBootReboot, &kPatchBootHide}, nullptr, true, "HUD:Reboot / HUD:Hide work in MP"},
    {"bootanims", {&kPatchBootInit, &kPatchBootGfx}, nullptr, true, "HUD boot system loads SP boot animations"},
    {"prematch", {&kPatchPrematch, nullptr}, nullptr, true, "gamerules start in ePS_Match (no grey picture)"},
    {"cutscene", {&kPatchCutscene, nullptr}, nullptr, true, "NoHUD cutscenes use the SP HUD state"},
    {"teamcross", {&kPatchTeamCross, nullptr}, nullptr, true, "teammates are friends for the crosshair"},
    {"radar", {&kPatchRadarAdd, nullptr}, nullptr, true, "radar adds objectives/filtered actors as in SP"},
    {"deadhud", {&kPatchDeadHud, nullptr}, nullptr, true, "dead spectating player gets no_hud, not MP screens"},
    {"save", {nullptr, nullptr}, &g_fixSave, true, "Game:SaveGame completes in network games"},
    {"realsave", {nullptr, nullptr}, &g_fixRealSave, false, "host really saves checkpoints (experiment)"},
    {"autostate", {nullptr, nullptr}, &g_fixAutoState, true, "activate the default HUD state after level start"},
    {"mo", {nullptr, nullptr}, &g_fixMO, true, "mission objective system kept alive in MP"},
};

Fix* FindFix(const std::string& name) {
    for (auto& f : g_fixes)
        if (name == f.name)
            return &f;
    return nullptr;
}

bool SetFix(Fix& f, bool on) {
    bool ok = true;
    for (auto p : f.patches)
        if (p)
            ok = SetPatch(*p, on) && ok;
    if (f.flag)
        *f.flag = on;
    return ok;
}

std::string FixState(const Fix& f) {
    std::string s;
    for (auto p : f.patches) {
        if (!p)
            continue;
        if (!s.empty())
            s += "/";
        s += PatchStateName(*p);
    }
    if (f.flag)
        s = f.flag->load() ? "on" : "off";
    if (&f == FindFix("save") && !g_saveHooked)
        s += " (hook NOT installed)";
    if (&f == FindFix("mo") && !g_initGameTypeHooked)
        s += " (hook NOT installed)";
    return s;
}

// ---------------------------------------------------------------- commands

void Status() {
    char* g = Game();
    c3coop::Logf("hud: bMultiplayer=%d bServer=%d g_pGame=%p", IsMultiplayer(), IsServer(), g);
    if (g) {
        void* mo = Field<void*>(g, kGameMOSystem);
        if (mo) {
            auto begin = Field<char*>(mo, kMOObjectivesBegin);
            auto end = Field<char*>(mo, kMOObjectivesEnd);
            c3coop::Logf("hud: MO system %p, objectives loaded=%d, count=%lld", mo, Field<unsigned char>(mo, kMOLoaded),
                         (long long)((end - begin) / kMOObjectiveSize));
        } else {
            c3coop::Logf("hud: MO system NULL (HUD.SetObjectiveStatus/SetObjectiveEntity are no-ops)");
        }
        void* hud = Field<void*>(g, kGameHud);
        void* cvars = hud ? Field<void*>(hud, kHudCVars) : nullptr;
        c3coop::Logf("hud: CHUD %p, hud_boot_enableForMP=%d", hud, cvars ? Field<int>(cvars, kHudCVarBootEnableForMP) : -1);
        if (void* rules = GameRules())
            c3coop::Logf("hud: gamerules %p prematch state %d (3 = ePS_Match)", rules, Field<int>(rules, kRulesPrematchState));
        else
            c3coop::Logf("hud: no gamerules");
        DumpSaturation(false);
    }
    c3coop::Logf("hud: fake checkpoint saves so far: %d", g_fakeSaves.load());
    for (auto& f : g_fixes)
        c3coop::Logf("hud: fix %-10s %-24s %s", f.name, FixState(f).c_str(), f.help);
}

void Reboot(int action) {
    if (!Sig(kRvaHudRebootDoHudEvent, kHudRebootDoHudEventBytes)) {
        c3coop::Logf("hud: DoHudEvent bytes differ");
        return;
    }
    game::At<void(__fastcall)(void*, int)>(kRvaHudRebootDoHudEvent)(nullptr, action);
    c3coop::Logf("hud: HUD boot event %d dispatched (ignored in MP unless `!hud boot on` or hud_boot_enableForMP 1)",
                 action);
}

void ActivateState(const std::string& name) {
    void* hud = Hud();
    if (!hud) {
        c3coop::Logf("hud: no CHUD");
        return;
    }
    if (name == "default") {
        if (Sig(kRvaHudActivateDefaultState, kHudActivateDefaultStateBytes)) {
            game::At<void(__fastcall)(void*)>(kRvaHudActivateDefaultState)(hud);
            c3coop::Logf("hud: ActivateDefaultState called");
        }
        return;
    }
    if (Sig(kRvaHudActivateState, kHudActivateStateBytes)) {
        game::At<void(__fastcall)(void*, const char*)>(kRvaHudActivateState)(hud, name.c_str());
        c3coop::Logf("hud: ActivateState(\"%s\") called", name.c_str());
    }
}

void Help() {
    c3coop::Logf("hud: !hud status | !hud <fix> on|off | !hud all on|off | !hud prematch now | !hud mo create|reload");
    c3coop::Logf("hud: !hud sat [reset] | !hud reboot <0 normal,1 boot,2 destroy,3 malfunction,5 hidden> | "
                 "!hud state <name>|default");
    for (auto& f : g_fixes)
        c3coop::Logf("hud:   %-10s %s (default %s)", f.name, f.help, f.defaultOn ? "on" : "off");
}

template <size_t N>
bool HookAt(uintptr_t rva, const unsigned char (&bytes)[N], void* detour, void** original, const char* what) {
    auto target = game::At<unsigned char>(rva);
    bool ok = memcmp(target, bytes, N) == 0 && MH_CreateHook(target, detour, original) == MH_OK &&
              MH_EnableHook(target) == MH_OK;
    c3coop::Logf("hud: %s %s at %p", what, ok ? "hooked" : "NOT hooked (bytes differ or MinHook failed)", target);
    return ok;
}

} // namespace

namespace c3coop {

// Single player activates the default HUD state (sp_core_hud) when the prematch state changes
// (CGameRules::NetSerialize 0x388A665D); with [prematch] the network game starts directly in ePS_Match, so nothing
// does. Called every frame on the main thread: once per gamerules instance, 3 s and again 10 s after it appears
// (the local player may spawn late).
void HudTick() {
    static void* seenRules = nullptr;
    static int ticks = 0;
    if (!g_fixAutoState || !IsMultiplayer())
        return;
    void* rules = GameRules();
    if (rules != seenRules) {
        seenRules = rules;
        ticks = 0;
    }
    if (!rules || !Hud() || ++ticks > 600)
        return;
    if (ticks == 180 || ticks == 600) {
        c3coop::Logf("hud: autostate: activating the default HUD state (tick %d)", ticks);
        ActivateState("default");
    }
}

void InstallHudHooks() {
    g_actionEventSigOk = Sig(game::kRvaCryActionOnActionEvent, game::kCryActionOnActionEventBytes);
    if (!g_actionEventSigOk)
        Logf("hud: CCryAction::OnActionEvent bytes differ, checkpoint saves are not faked");
    g_saveHooked = HookAt(kRvaSaveGame, kSaveGameBytes, reinterpret_cast<void*>(&HookedSaveGame),
                          reinterpret_cast<void**>(&g_origSaveGame), "CCryAction::SaveGame");
    g_initGameTypeHooked = HookAt(kRvaInitGameType, kInitGameTypeBytes, reinterpret_cast<void*>(&HookedInitGameType),
                                  reinterpret_cast<void**>(&g_origInitGameType), "CGame::InitGameType");
    for (auto& f : g_fixes) {
        bool ok = SetFix(f, f.defaultOn);
        Logf("hud: fix %s %s%s", f.name, f.defaultOn ? "on" : "off", ok ? "" : " (FAILED, see above)");
    }
}

bool HudCommand(const std::string& cmd) {
    if (cmd != "!hud" && cmd.rfind("!hud ", 0) != 0)
        return false;
    std::string args = cmd.size() > 5 ? cmd.substr(5) : "";
    std::string verb = args.substr(0, args.find(' '));
    std::string rest = args.find(' ') == std::string::npos ? "" : args.substr(args.find(' ') + 1);

    if (verb.empty() || verb == "help") {
        Help();
    } else if (verb == "status") {
        Status();
    } else if (verb == "prematch" && rest == "now") {
        ForceMatchState("!hud prematch now");
    } else if (verb == "mo" && (rest == "create" || rest == "reload")) {
        char* g = Game();
        void* mo = g ? Field<void*>(g, kGameMOSystem) : nullptr;
        if (!mo)
            mo = CreateMOSystem("!hud mo create");
        LoadObjectives(mo, true);
    } else if (verb == "sat") {
        DumpSaturation(rest == "reset");
    } else if (verb == "reboot") {
        int action = 0;
        if (sscanf_s(rest.c_str(), "%d", &action) == 1)
            Reboot(action);
        else
            Logf("hud: usage !hud reboot <action>");
    } else if (verb == "state") {
        if (rest.empty())
            Logf("hud: usage !hud state <name>|default");
        else
            ActivateState(rest);
    } else if (rest == "on" || rest == "off") {
        bool on = rest == "on";
        if (verb == "all") {
            for (auto& f : g_fixes)
                SetFix(f, on);
            Logf("hud: all fixes %s", rest.c_str());
        } else if (Fix* f = FindFix(verb)) {
            bool ok = SetFix(*f, on);
            Logf("hud: fix %s %s%s", f->name, rest.c_str(), ok ? "" : " (FAILED)");
            if (on && verb == "prematch")
                ForceMatchState("!hud prematch on"); // the Init patch only affects gamerules created later
        } else {
            Logf("hud: unknown fix '%s'", verb.c_str());
        }
    } else {
        Logf("hud: unknown command '%s' (!hud help)", cmd.c_str());
    }
    return true;
}

} // namespace c3coop
