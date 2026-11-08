// Single player features in network games: the campaign runs as a CryEngine network session (gEnv->bMultiplayer = 1),
// which switches player-facing systems (nanosuit modes and upgrades, weapon customization, HUD elements...) to their
// multiplayer behaviour. Each fix here restores the single player branch at one specific check (byte patches only,
// every site is checked against its original bytes first). The DLL runs on every machine, so host and clients get
// the same code paths. Nothing here touches net serialization: suit mode changes still go through
// CNanoSuit::ActivateMode (client: local DoActivateMode + SvRequestSuitMode RMI, server: ASPECT_NANO_SUIT_SETTING).
//
//   !sp status                      bMultiplayer/bServer, not_yet_spawned filter state, patch states
//   !sp <fix> on|off                nys, actionmap, spmap, perks, ammo, wheel, visor, tagging, interest, nanovision, regen,
//                                   playmode, custmenu, pickup, grabenemy, throwsp, mpsound, cloak, perktiers,
//                                   stealthkill, aistim, falldamage,
//                                   bowarrows, throw, cutsceneinput, clienthit, usable, cephammo, armorenergy, sprint,
//                                   killplayer, weaponsp, aiperception, gameai, immersive, spslots, hackdata, ridephys,
//                                   radarfilter, radarvis, radarshoot, radarview, radaralert (all on)
//   !sp all on|off
//   !sp nys clear                   disable the not_yet_spawned action filter now (Q/E/B/N/zoom blocked while on)
//
// SDK references are Code/Game03/GameDll (2015 SDK). All addresses are for Crysis3Remastered.exe build 0x62FD4D1B
// (fixed base 0x37000000), RVA = VA - 0x37000000.

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

#include "game.h"
#include "modules.h"

namespace {

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

constexpr unsigned char kJmpShort[] = {0xEB};
constexpr unsigned char kNop2[] = {0x66, 0x90};
constexpr unsigned char kNop4[] = {0x0F, 0x1F, 0x40, 0x00};
constexpr unsigned char kNop6[] = {0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00};
constexpr unsigned char kNop8[] = {0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00};

#define SP_PATCH(name, what, rva, off, repl)                                                                          \
    const BytePatch name = {what, rva, name##Sig, sizeof(name##Sig), off, repl, sizeof(repl)}

// ---- [nys] Q/E (armor/cloak), B (visor), N (nanovision), binoculars and zoom
// CPlayer::InitLocalPlayer (Player.cpp:1202, here inside 0x385F3C20): `if (bMultiplayer) { lobby->OnHaveLocalPlayer();
// if (m_timeFirstSpawned <= 0) g_pGameActions->FilterNotYetSpawned()->Enable(true); }`. The "not_yet_spawned"
// filter (GameActions.cpp:87) fails nanosuit_armor/stealth/visor/toggleVision, binoculars, zoom, xi_zoom, and is only
// disabled by an MP spawn revive (kRFR_Spawn) or a later revive. 0x385F4069 `jb 0x385F4096` (time already set) ->
// jmp: the filter is never enabled on spawn; the lobby call stays.
constexpr unsigned char kNysSig[] = {0xC5, 0xF8, 0x2F, 0x87, 0x34, 0x20, 0x00, 0x00, 0x72, 0x2B};
SP_PATCH(kNys, "InitLocalPlayer: no not_yet_spawned filter", 0x15F4061, 8, kJmpShort);
// g_pGameActions (VA 0x39529930, next to g_pGame) and CGameActions::m_pFilterNotYetSpawned (+0x860, stored by
// CreateFilterNotYetSpawned 0x386CAAAC). IActionFilter vtbl +0x30 Enable(bool), +0x38 Enabled() (0x385F4084/93).
constexpr uintptr_t kRvaGameActions = 0x2529930;
constexpr size_t kGameActionsFilterNotYetSpawned = 0x860;
constexpr size_t kVtFilterEnable = 0x30;
constexpr size_t kVtFilterEnabled = 0x38;

// ---- [actionmap] player_sp instead of player_mp (mouse3 suit wheel, drop, visor_hacking, 3/4 weapon keys)
// CCET_InitActionMap::OnStep (CET_ActionMap.cpp:51, 0x37565B10): `if (bMultiplayer) swap("player_sp","player_mp")`
// then disables the first and enables the second. 0x37565B4B `je` (keep SP names) -> jmp.
constexpr unsigned char kAmCetSig[] = {0x80, 0x3D, 0xCC, 0xA9, 0xEE, 0x01, 0x00, 0x4C, 0x8D, 0x25, 0x64,
                                       0x82, 0xB9, 0x01, 0x48, 0x8B, 0xD8, 0x48, 0x8D, 0x05, 0x4A, 0x82,
                                       0xB9, 0x01, 0x48, 0x89, 0x44, 0x24, 0x78, 0x74, 0x08};
SP_PATCH(kAmCet, "CET_InitActionMap: enable player_sp", 0x565B2E, 29, kJmpShort);
// ReloadActionMaps (GameCVars.cpp:504, 0x386E7034): listener for `bMultiplayer ? "player_mp" : "player_sp"`;
// 0x386E7176 `cmovne rdx,rax` -> nop (after a key rebind the listener goes to player_sp as in SP).
constexpr unsigned char kAmReloadSig[] = {0x80, 0x3D, 0xA2, 0x93, 0xD6, 0x00, 0x00, 0x48, 0x8D, 0x15, 0x3A, 0x6C,
                                          0xA1, 0x00, 0x4C, 0x8B, 0x06, 0x4C, 0x8B, 0xF8, 0x48, 0x8D, 0x05, 0x1D,
                                          0x6C, 0xA1, 0x00, 0x48, 0x8B, 0xCE, 0x48, 0x0F, 0x45, 0xD0};
SP_PATCH(kAmReload, "ReloadActionMaps: player_sp listener", 0x16E7158, 30, kNop4);
// CHUD_WeaponSelect key hint (HUD_WeaponSelect.cpp:107, 0x388A1CF8): GetInputDataFromAction("toggle_weapon",
// bMultiplayer ? "player_mp" : "player_sp") -> `cmovne r8,rax` nop.
constexpr unsigned char kAmHintSig[] = {0x80, 0x3D, 0x1D, 0xE8, 0xBA, 0x00, 0x00, 0x48, 0x8D, 0x05, 0xA5,
                                        0xC0, 0x85, 0x00, 0x4C, 0x8D, 0x05, 0xAE, 0xC0, 0x85, 0x00, 0x41,
                                        0xB9, 0x02, 0x00, 0x00, 0x00, 0x4C, 0x0F, 0x45, 0xC0};
SP_PATCH(kAmHint, "WeaponSelect toggle_weapon hint from player_sp", 0x18A1CDD, 27, kNop4);

// ---- [spmap] "singleplayer" action map (X = menu_open_customizeweapon, defaultprofile.xml)
// CGameRules::ProcessEvent(ENTITY_EVENT_START_GAME) (GameRules.cpp:1866, 0x388A7BB1): on clients
// EnableActionMap("multiplayer", bMP), EnableActionMap("singleplayer", !bMP), listener = gamerules entity. bMP is
// `movzx ebp, byte [bMultiplayer]`, used only by these calls and the MP time-of-day block (server && MP).
// -> `mov ebp,0; nop2` (mov keeps the flags of the bServer cmp before it).
constexpr unsigned char kSpMapSig[] = {0x80, 0x3D, 0x4F, 0x89, 0xBA, 0x00, 0x00, 0x0F,
                                       0xB6, 0x2D, 0x49, 0x89, 0xBA, 0x00, 0x0F, 0x84};
constexpr unsigned char kMovEbp0[] = {0xBD, 0x00, 0x00, 0x00, 0x00, 0x66, 0x90};
SP_PATCH(kSpMap, "GameRules START_GAME: enable singleplayer action map", 0x18A7BAA, 7, kMovEbp0);

// ---- [perks] suit upgrade menu (H = menu_open/close_customizeperks)
// CFlashFrontEnd::OnAction (FlashFrontEnd.cpp:2982, 0x389284C0): `else if (!bMultiplayer && ... action ==
// menu_open_customizeperks) Initialize(eFlM_SuitCustomization)` and the close branch; in MP 0x38928BF2 `jne` skips
// both to the scoreboard checks. NOP the jne (the close branch tests m_blockClose in cl, as in SP).
constexpr unsigned char kPerksSig[] = {0x80, 0x3D, 0x0F, 0x79, 0xB2, 0x00, 0x00, 0x75, 0x8C};
SP_PATCH(kPerks, "FlashFrontEnd: open/close suit customization", 0x1928BEB, 7, kNop2);

// ---- [ammo] ammo / weapon / suit energy panel (bottom right)
// CHUD_EnergyAndSuitMode::Init (HUD_EnergyAndSuitMode.cpp:55, 0x38862E50): MP calls OnSubAssetLoaded directly; SP
// loads the "Ammo" sub-asset allinone_hud_3d_ammo_pc.gfx into energybar's Root_Ammo.HUD_Loader. 0x38862E63 je -> jmp,
// and 0x38862E9A `cmovne rcx,rdx` (ammo_mp.gfx, not shipped) -> nop.
constexpr unsigned char kAmmoEnergySig[] = {0x80, 0x3D, 0xA4, 0xD6, 0xBE, 0x00, 0x00, 0x48,
                                            0x8B, 0xD9, 0x48, 0x8B, 0x01, 0x74, 0x10};
SP_PATCH(kAmmoEnergy, "HUD_EnergyAndSuitMode::Init SP path", 0x1862E56, 13, kJmpShort);
constexpr unsigned char kAmmoGfxSig[] = {0x80, 0x3D, 0x78, 0xD6, 0xBE, 0x00, 0x00, 0x48, 0x8D, 0x15,
                                         0x58, 0x97, 0x76, 0x00, 0x48, 0x8D, 0x0D, 0x79, 0x97, 0x76,
                                         0x00, 0x4C, 0x8B, 0xC0, 0x48, 0x0F, 0x45, 0xCA};
SP_PATCH(kAmmoGfx, "HUD_EnergyAndSuitMode::Init ammo_pc.gfx", 0x1862E82, 24, kNop4);
// CHUD_Ammo::Init (HUD_Ammo.cpp:69, 0x3884E410) and CHUD_WeaponSelect::Init (HUD_WeaponSelect.cpp:53, 0x3889FED0):
// `if (bMultiplayer) OnSubAssetLoaded(); else ListenForSubAssetLoad(this, "Ammo")`. je -> jmp (MP registers flash
// objects of a movie that is not loaded yet -> no ammo counter / weapon list).
constexpr unsigned char kAmmoInitSig[] = {0x80, 0x3D, 0xEA, 0x20, 0xC0, 0x00, 0x00, 0x74, 0x07};
SP_PATCH(kAmmoInit, "HUD_Ammo::Init waits for Ammo sub-asset", 0x184E410, 7, kJmpShort);
constexpr unsigned char kAmmoSelectSig[] = {0x80, 0x3D, 0x2A, 0x06, 0xBB, 0x00, 0x00, 0x74, 0x07};
SP_PATCH(kAmmoSelect, "HUD_WeaponSelect::Init waits for Ammo sub-asset", 0x189FED0, 7, kJmpShort);

// ---- [wheel] mouse3 nanosuit wheel: weapon/suit customization sectors
// CHUD_MouseWheel::Init (HUD_MouseWheel.cpp:65, 0x3887BB10): MP hides mwWeapon/mwSuit (0x3887BD1B je -> jmp).
constexpr unsigned char kWheelInitSig[] = {0x80, 0x3D, 0xE6, 0x47, 0xBD, 0x00, 0x00, 0x74, 0x24};
SP_PATCH(kWheelInit, "HUD_MouseWheel::Init keeps customization icons", 0x187BD14, 7, kJmpShort);
// CHUD_MouseWheel::UpdateMouseWheel (:376, 0x3887C9B0): MP maps those sectors to eCORE (0x3887CC5C jne -> nop).
constexpr unsigned char kWheelUpdSig[] = {0x80, 0x3D, 0xA5, 0x38, 0xBD, 0x00, 0x00, 0x0F, 0x85, 0x47, 0x02, 0x00, 0x00};
SP_PATCH(kWheelUpd, "HUD_MouseWheel::UpdateMouseWheel customization sectors", 0x187CC55, 7, kNop6);

// ---- [visor] visor HUD (CHUD_Visor, vtable 0x38FD31E0)
// Init (HUD_Visor.cpp:81, 0x3889B260): SP registers Root_Radar.StealthBar (AI awareness bar); 0x3889B2B3 jne -> nop.
constexpr unsigned char kVisorInitSig[] = {0x80, 0x3D, 0x51, 0x52, 0xBB, 0x00, 0x00, 0x4C, 0x8B, 0xE0, 0x75, 0x2A};
SP_PATCH(kVisorInit, "HUD_Visor::Init stealth bar", 0x189B2A9, 10, kNop2);
// Update (0x3889B610): `if (!MP && m_visorEnabled) TacticalManager->Ping(hud_InterestPointsPingDistance)` reveals
// interest points while the visor is up (0x3889B642 jne -> nop); stealth bar update (0x3889B9FC jne -> nop);
// range infinity icon `renderInfinityIcon = !MP ? atInfinity : false` (0x3889BA8A cmove edx,eax -> mov edx,eax).
constexpr unsigned char kVisorPingSig[] = {0x80, 0x3D, 0xD0, 0x4E, 0xBB, 0x00, 0x00, 0x48, 0x8B, 0xF9, 0xC5, 0xF8, 0x29,
                                           0x70, 0xC8, 0xC5, 0xF8, 0x29, 0x78, 0xB8, 0xC5, 0xF8, 0x28, 0xF1, 0x75, 0x32};
SP_PATCH(kVisorPing, "HUD_Visor::Update tactical ping", 0x189B62A, 24, kNop2);
constexpr unsigned char kVisorStealthSig[] = {0x40, 0x38, 0x35, 0x05, 0x4B, 0xBB, 0x00, 0x75, 0x4D};
SP_PATCH(kVisorStealth, "HUD_Visor::Update stealth bar", 0x189B9F5, 7, kNop2);
constexpr unsigned char kVisorInfSig[] = {0x40, 0x38, 0x35, 0x7D, 0x4A, 0xBB, 0x00, 0x0F,
                                          0xB6, 0xC0, 0x4C, 0x8B, 0x01, 0x0F, 0x44, 0xD0};
constexpr unsigned char kMovEdxEax[] = {0x8B, 0xD0, 0x90};
SP_PATCH(kVisorInf, "HUD_Visor::Update infinity icon", 0x189BA7D, 13, kMovEdxEax);

// ---- [tagging] visor scan / tag
// CanStartScanning (0x3889C420) and its inlined copy in StartScanning (0x3889C4E0): SP refuses already scanned
// entities (TacticalManager::GetEntityScanned) - 0x3889C492 / 0x3889C56A jne -> nop.
constexpr unsigned char kTagScan1Sig[] = {0x80, 0x3D, 0x6F, 0x40, 0xBB, 0x00, 0x00, 0x75, 0x22};
SP_PATCH(kTagScan1, "HUD_Visor::CanStartScanning SP", 0x189C48B, 7, kNop2);
constexpr unsigned char kTagScan2Sig[] = {0x80, 0x3D, 0x97, 0x3F, 0xBB, 0x00, 0x00, 0x75, 0x26};
SP_PATCH(kTagScan2, "HUD_Visor::StartScanning SP", 0x189C563, 7, kNop2);
// CanStartTagging (HUD_Visor.cpp:629, 0x3889CD80 and inlined in 0x3889CE90): SP cannot tag Stalkers.
constexpr unsigned char kTagStalker1Sig[] = {0x80, 0x3D, 0x49, 0x37, 0xBB, 0x00, 0x00, 0x75, 0x16};
SP_PATCH(kTagStalker1, "HUD_Visor::CanStartTagging SP", 0x189CDB1, 7, kNop2);
constexpr unsigned char kTagStalker2Sig[] = {0x80, 0x3D, 0x33, 0x36, 0xBB, 0x00, 0x00, 0x75, 0x16};
SP_PATCH(kTagStalker2, "HUD_Visor::StartTagging SP", 0x189CEC7, 7, kNop2);
// EndTagging (HUD_Visor.cpp:652, 0x3889D100): `if (!MP) { TacticalManager->SetEntityTagged(id, true); perk event
// EPE_VisorTag; stats }` - MP expects the gamerules tag RMI instead, so tagged AI never got their markers.
// 0x3889D1B3 jne -> nop (tags are local to each player, as in SP).
constexpr unsigned char kTagEndSig[] = {0x40, 0x38, 0x35, 0x4E, 0x33, 0xBB, 0x00, 0x75, 0x64};
SP_PATCH(kTagEnd, "HUD_Visor::EndTagging tags locally", 0x189D1AC, 7, kNop2);

// ---- [interest] visor interest points (CHUD_InterestPoints)
// Init (HUD_InterestPoints.cpp:306, 0x38870100): SP icon scaling (0x3887014F jne -> nop).
constexpr unsigned char kIpInitSig[] = {0x80, 0x3D, 0xB2, 0x03, 0xBE, 0x00, 0x00, 0x0F, 0x85, 0x4A, 0x01, 0x00, 0x00};
SP_PATCH(kIpInit, "HUD_InterestPoints::Init icon scale", 0x1870148, 7, kNop6);
// QueryInterestPoints (:1504, 0x388732C9): units (enemies) and arrows are only queried in SP -> jne nop.
constexpr unsigned char kIpUnitsSig[] = {0x80, 0x3D, 0x38, 0xD2, 0xBD, 0x00, 0x00, 0x0F, 0x85, 0xDC, 0x01, 0x00, 0x00};
SP_PATCH(kIpUnits, "HUD_InterestPoints: query units/arrows", 0x18732C2, 7, kNop6);
// Unit icon type (:1558, 0x38873746): SP marks friendly units with eIconType_Friendly -> jne nop.
constexpr unsigned char kIpFriendSig[] = {0x80, 0x3D, 0xBF, 0xCD, 0xBD, 0x00, 0x00, 0xC6, 0x45,
                                          0xFA, 0x03, 0x0F, 0x85, 0x2C, 0x01, 0x00, 0x00};
SP_PATCH(kIpFriend, "HUD_InterestPoints: friendly unit icons", 0x187373B, 11, kNop6);
// OnHUDEvent(eHUDEvent_OnEntityTagged) (:420, 0x3886FE47): SP plays the tagged anim and refreshes the icon -> nop.
constexpr unsigned char kIpTaggedSig[] = {0x80, 0x3D, 0xBA, 0x06, 0xBE, 0x00, 0x00, 0x75, 0xEC};
SP_PATCH(kIpTagged, "HUD_InterestPoints: tagged event", 0x186FE40, 7, kNop2);
// Update (:661, 0x3886FABD): SP shows the AI alertness of the looked-at enemy -> jne nop.
constexpr unsigned char kIpAwareSig[] = {0x80, 0x3D, 0x47, 0x0A, 0xBE, 0x00, 0x00, 0x44, 0x8B, 0xE8, 0x75, 0x40};
SP_PATCH(kIpAware, "HUD_InterestPoints: AI awareness", 0x186FAB3, 10, kNop2);

// ---- [nanovision] N key
// CSuitVisorLayer::CSuitVisorLayer (SuitVisorLayer.cpp:39, 0x38698E80): `if (MP) SetExclusiveVisionMode(None)`
// forbids NanoVision for every suit built in a network game. 0x38698EC9 je -> jmp. Applies to suits created after
// the patch (players spawned later); energy stays on the MP model (separate nanovision energy).
constexpr unsigned char kNanoVisionSig[] = {0x44, 0x38, 0x05, 0x38, 0x76, 0xDB, 0x00, 0x74, 0x43};
SP_PATCH(kNanoVision, "CSuitVisorLayer ctor allows NanoVision", 0x1698EC2, 7, kJmpShort);

// ---- [regen] player health regeneration
// CPlayer::GetRegenerationAmount (Player.cpp:4667, 0x38610430): MP threshold/rates (g_mpRegenerationRate) instead of
// pl_health.*SP. 0x38610496 je -> jmp.
constexpr unsigned char kRegenRateSig[] = {0x80, 0x3D, 0x7B, 0x00, 0xE4, 0x00, 0x00, 0x48, 0x8B, 0x05, 0xB3, 0x94,
                                           0xF1, 0x00, 0xC5, 0x7A, 0x5C, 0xD6, 0xC4, 0x41, 0x30, 0x57, 0xC9, 0x74, 0x40};
SP_PATCH(kRegenRate, "GetRegenerationAmount SP rates", 0x161047F, 23, kJmpShort);
// CPlayer::UpdateHealthRegeneration (:4741, 0x38610360): MP regenerates every CPlayer (also AI humans) unless the
// objectives module forbids it; SP only actors with a nanosuit. 0x38610390 je -> jmp (SP condition).
constexpr unsigned char kRegenWhoSig[] = {0x80, 0x3D, 0x71, 0x01, 0xE4, 0x00, 0x00, 0x74, 0x24};
SP_PATCH(kRegenWho, "UpdateHealthRegeneration: nanosuit wearers only", 0x1610389, 7, kJmpShort);

// ---- [playmode] Mannequin playMode tag SP (weapon sounds, SP 1P animation variants)
// CPlayer::RestartMannequin (Player.cpp:3494, 0x38600C10): `state.SetGroup(tagGroupIDs.playMode, bMultiplayer ?
// tagIDs.MP : tagIDs.SP)` for every non-AI player on every machine. MP picks the "+MP+" fragments of every
// <weapon>Sounds.adb (playerSounds.adb SubADBs) -> sounds/w_<weapon>_mp:* events, and the remaster ships no *_mp FMOD
// projects: all player weapon sounds were silent (own 1P and remote 3P). 0x38600CD7 `cmovne r8d,[tagIDs.MP]` -> nop.
// Takes effect at the next RestartMannequin (spawn/revive).
constexpr unsigned char kPlayModeSig[] = {0x80, 0x3D, 0x34, 0xF8, 0xE4, 0x00, 0x00, 0x48, 0x8B, 0xCF, 0x44, 0x8B, 0x05,
                                          0x4D, 0x77, 0xF2, 0x00, 0x44, 0x0F, 0x45, 0x05, 0x41, 0x77, 0xF2, 0x00};
SP_PATCH(kPlayMode, "RestartMannequin: playMode tag SP", 0x1600CC6, 17, kNop8);

// ---- [custmenu] weapon customization menu: attachments the player carries are selectable
// Weapon customization page push (0x389770E0, "addAttachment_new"): the "have it" flag of each attachment comes in MP
// from the MP unlock/loadout list ([rbp+0C0h], empty in the campaign) and in SP from IInventory::HasAccessory
// (vtbl+0x178, 0x389779FD). In co-op every slot showed a lock although the inventory had the accessories (Jailbreak:
// Silencer/SilencerPistol/LaserSight given by Mission_Jailbreak). 0x38977969 `je 0x389779FD` (SP branch) -> jmp.
constexpr unsigned char kCustMenuSig[] = {
    0x80, 0x3D, 0xC0, 0x8B, 0xAD, 0x00, 0x00, 0x48, 0x8B, 0x85, 0x90, 0x00, 0x00, 0x00, 0x48, 0x89, 0x85, 0x98,
    0x02, 0x00, 0x00, 0xC7, 0x85, 0x90, 0x02, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0xC5, 0xF8, 0x10, 0x85, 0x90,
    0x02, 0x00, 0x00, 0xC5, 0xF8, 0x11, 0x85, 0xF0, 0x01, 0x00, 0x00, 0x0F, 0x84, 0x8E, 0x00, 0x00, 0x00};
constexpr unsigned char kJmpCustMenuSp[] = {0xE9, 0x8F, 0x00, 0x00, 0x00, 0x90};
SP_PATCH(kCustMenu, "WeaponCustomization: attachments from the inventory", 0x197793A, 47, kJmpCustMenuSp);

// ---- [pickup] pick up physics objects (SDK PlayerPlugin_Interaction.cpp)
// GetInteractionType (0x38A02B40, :539): `!bMultiplayer && rigid body && pickable && HasObjectGrabHelper -> Grab`;
// 0x38A03088 jne -> nop. The prompt update (0x38A02260, :358) keeps Grab in MP only for environmental weapons held
// through PickAndThrowWeapon; 0x38A0285B je (skip the MP block) -> jmp.
constexpr unsigned char kPickupGrabSig[] = {0x80, 0x3D, 0x79, 0xD4, 0xA4, 0x00, 0x00, 0x0F, 0x85, 0xA3, 0x00, 0x00, 0x00};
constexpr unsigned char kPickupGrabServer[] = {0x78, 0xD4, 0xA4, 0x00, 0x00, 0x0F, 0x84}; // cmp [bServer]; je skip
SP_PATCH(kPickupGrab, "PlayerPlugin_Interaction: grab rigid bodies (server)", 0x1A03081, 2, kPickupGrabServer);
constexpr unsigned char kPickupPromptSig[] = {0x44, 0x38, 0x35, 0xA6, 0xDC, 0xA4, 0x00, 0x0F, 0x84, 0x6C, 0x01, 0x00, 0x00};
constexpr unsigned char kJmpPickupPrompt[] = {0xE9, 0x6D, 0x01, 0x00, 0x00, 0x90};
SP_PATCH(kPickupPrompt, "PlayerPlugin_Interaction: grab not only environmental weapons", 0x1A02854, 7,
         kJmpPickupPrompt);

// CPlayer::RequestEnterPickAndThrow (0x3860BC90, Player.cpp:8215): in MP only entities with an EnvironmentalWeapon or
// PossessionItem extension (0x38BA3230) are used, everything else was dropped. Rewritten from 0x3860BC97: extension ->
// the MP path (0x3860BCC6) as before; other entity -> on the server EnterPickAndThrow(id, 1, 0) (0x3860BE30) and the
// epilogue 0x3860BE14; on clients return as before (a client-side pick would fight the server's physics; that is also
// why the grab prompt above is shown on the server only). PickAndThrowWeapon::OnSelected 0x3881B63D: MP skips the grab
// animation. [grabenemy] AutoAim (AutoAimManager.cpp:124/254): targets never got the hostile/stealth-kill/grabbable
// flags in MP; the script flags are read everywhere, AI hostility only on the server (clients have no AI system).
// NotifyObjectGrabbed 0x3860B090: the SP DeathManager notification. [throwsp] cosmetic SP throw parity: cloak
// interference on grab/throw, spin, NanoStrength Maximum power throw.
constexpr unsigned char kPickupUseSig[] = {0x80, 0x3D, 0x63, 0x48, 0xE4, 0x00, 0x00, 0x8B, 0xFA, 0x48, 0x8B, 0xD9, 0x75, 0x11, 0x45, 0x33, 0xC9, 0x41, 0xB0, 0x01, 0x48, 0x83, 0xC4, 0x28, 0x5F, 0x5B, 0xE9, 0x7A, 0x01, 0x00, 0x00, 0x8B, 0xCF, 0xE8, 0x73, 0x75, 0x59, 0x00, 0x48, 0x85, 0xC0, 0x0F, 0x84, 0x4E, 0x01, 0x00, 0x00};
constexpr unsigned char kPickupUseRepl[] = {0x8B, 0xFA, 0x48, 0x8B, 0xD9, 0x8B, 0xCF, 0xE8, 0x8D, 0x75, 0x59, 0x00, 0x48, 0x85, 0xC0, 0x75, 0x1E, 0x80, 0x3D, 0x51, 0x48, 0xE4, 0x00, 0x00, 0x74, 0x10, 0x8B, 0xD7, 0x48, 0x8B, 0xCB, 0x45, 0x33, 0xC9, 0x41, 0xB0, 0x01, 0xE8, 0x6F, 0x01, 0x00, 0x00, 0xE9, 0x4E, 0x01, 0x00, 0x00};
SP_PATCH(kPickupUse, "Player: use picks up any grabbable entity (server)", 0x160BC97, 0, kPickupUseRepl);
constexpr unsigned char kPickupAnimSig[] = {0x44, 0x38, 0x35, 0xC4, 0x4E, 0xC3, 0x00, 0x0F, 0x85, 0x16, 0xFE, 0xFF, 0xFF};
constexpr unsigned char kPickupAnimRepl[] = {0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00};
SP_PATCH(kPickupAnim, "PickAndThrowWeapon OnSelected: SP grab animation", 0x181B636, 7, kPickupAnimRepl);
constexpr unsigned char kAimRegisterSig[] = {0x4C, 0x89, 0x64, 0x24, 0x48, 0x48, 0x89, 0x74, 0x24, 0x50, 0x0F, 0x85, 0xB6, 0x01, 0x00, 0x00};
constexpr unsigned char kAimRegisterRepl[] = {0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00};
SP_PATCH(kAimRegister, "AutoAim Register: stealth-kill / grab flags", 0x16474BA, 10, kAimRegisterRepl);
constexpr unsigned char kAimHostileSig[] = {0x80, 0x3D, 0x68, 0x88, 0xE0, 0x00, 0x00, 0x0F, 0x85, 0xF3, 0x06, 0x00, 0x00};
constexpr unsigned char kAimHostileRepl[] = {0x67, 0x88, 0xE0, 0x00, 0x00, 0x0F, 0x84};
SP_PATCH(kAimHostile, "AutoAim Update: AI hostility on the server", 0x1647C92, 2, kAimHostileRepl);
constexpr unsigned char kGrabDeathMgrSig[] = {0x80, 0x3D, 0x76, 0x54, 0xE4, 0x00, 0x00, 0x48, 0x8B, 0x7C, 0x24, 0x58, 0x75, 0x36};
constexpr unsigned char kGrabDeathMgrRepl[] = {0x66, 0x90};
SP_PATCH(kGrabDeathMgr, "NotifyObjectGrabbed: DeathManager on grabbed NPC", 0x160B084, 12, kGrabDeathMgrRepl);
constexpr unsigned char kThrowCloakGrabSig[] = {0x80, 0x3D, 0x8E, 0x45, 0xE4, 0x00, 0x00, 0x74, 0x10};
constexpr unsigned char kThrowCloakGrabRepl[] = {0xEB};
SP_PATCH(kThrowCloakGrab, "EnterPickAndThrow: cloak interference", 0x160BF6C, 7, kThrowCloakGrabRepl);
constexpr unsigned char kThrowCloakThrowSig[] = {0x80, 0x3D, 0x2F, 0x08, 0xC3, 0x00, 0x00, 0x74, 0x10};
constexpr unsigned char kThrowCloakThrowRepl[] = {0xEB};
SP_PATCH(kThrowCloakThrow, "ThrowObject: cloak interference", 0x181FCCB, 7, kThrowCloakThrowRepl);
constexpr unsigned char kThrowSpinSig[] = {0x80, 0x3D, 0x72, 0x10, 0xC3, 0x00, 0x00, 0x0F, 0x85, 0x9D, 0x04, 0x00, 0x00};
constexpr unsigned char kThrowSpinRepl[] = {0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00};
SP_PATCH(kThrowSpin, "ThrowObject: spin", 0x181F488, 7, kThrowSpinRepl);
constexpr unsigned char kThrowPower1Sig[] = {0x80, 0x3D, 0xAD, 0x14, 0xC3, 0x00, 0x00, 0xC4, 0x81, 0x7A, 0x10, 0x84, 0x27, 0xBC, 0x00, 0x00, 0x00, 0xC5, 0x7A, 0x59, 0x70, 0x3C, 0x0F, 0x85, 0xA9, 0x00, 0x00, 0x00};
constexpr unsigned char kThrowPower1Repl[] = {0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00};
SP_PATCH(kThrowPower1, "ThrowObject: NanoStrength Maximum (1)", 0x181F04D, 22, kThrowPower1Repl);
constexpr unsigned char kThrowPower2Sig[] = {0x80, 0x3D, 0x57, 0x17, 0xC3, 0x00, 0x00, 0xC5, 0xFA, 0x10, 0x40, 0x3C, 0xC5, 0xFA, 0x59, 0x73, 0x24, 0x75, 0x6F};
constexpr unsigned char kThrowPower2Repl[] = {0x66, 0x90};
SP_PATCH(kThrowPower2, "ThrowObject: NanoStrength Maximum (2)", 0x181EDA3, 17, kThrowPower2Repl);

// ---- audit of the game's bMultiplayer branches (signatures verified against the exe). SDK Game03/GameDll sources:
// NanoModeStealth.cpp:348/398/149, BasePerk.cpp:9/35,
// StealthKill.cpp:441, GameRulesClientServer.cpp:892/408, PlayerStateUtil.cpp:681/710/720, BowArrow.cpp:251,
// PickAndThrowWeapon.cpp:519, GameRules.cpp:1223, AlienHeavyWeapon.cpp, NanoModeArmor.cpp:278, Player.cpp:7083,
// WeaponEvent.cpp:232, Game.cpp:3481, ScriptBind_Action.cpp:165, Player.cpp:9430/955). Each turns the MP branch into
// the SP one; "gameai", "immersive" and "spslots" change behaviour a lot (tested on the host: smarter AI, exploding barrels).
// [mpsound] CSoundSystem vtable slot +0x180 (0x37D7A7E0, remaster only): with bMultiplayer it renames
// "sounds/w_<proj>:..." to "sounds/w_<proj>_mp:...", projects the remaster does not ship -> all weapon fire silent
// (own, AI, remote) while impacts played. 0x37D7A7FF `je` (SP: plain strncpy) -> nop + jmp. The s_autoMpSoundNameFixup
// cvar is never read.
constexpr unsigned char kCloakAi1Sig[] = {0x80, 0x3D, 0x78, 0x7C, 0xDC, 0x00, 0x00, 0x0F, 0x85, 0x36, 0x01, 0x00, 0x00};
constexpr unsigned char kCloakAi1Repl[] = {0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00};
SP_PATCH(kCloakAi1, "NanoModeStealth: AI cloak params (1)", 0x1688882, 7, kCloakAi1Repl);
constexpr unsigned char kCloakAi2Sig[] = {0x80, 0x3D, 0xDC, 0x7A, 0xDC, 0x00, 0x00, 0x0F, 0x85, 0x95, 0x00, 0x00, 0x00};
constexpr unsigned char kCloakAi2Repl[] = {0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00};
SP_PATCH(kCloakAi2, "NanoModeStealth: AI cloak params (2)", 0x1688A1E, 7, kCloakAi2Repl);
constexpr unsigned char kCloakAi3Sig[] = {0x80, 0x3D, 0xDB, 0x82, 0xDC, 0x00, 0x00, 0x48, 0x8B, 0xD9, 0xC4, 0xC1, 0x78, 0x29, 0x7B, 0xD8, 0xC5, 0xF8, 0x28, 0xF9, 0x0F, 0x85, 0x52, 0x01, 0x00, 0x00};
constexpr unsigned char kCloakAi3Repl[] = {0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00};
SP_PATCH(kCloakAi3, "NanoModeStealth: AI stunt events", 0x168821F, 20, kCloakAi3Repl);
constexpr unsigned char kCloakDecloakSig[] = {0x80, 0x3D, 0xD0, 0x2D, 0xDC, 0x00, 0x00, 0x75, 0x68};
constexpr unsigned char kCloakDecloakRepl[] = {0x66, 0x90};
SP_PATCH(kCloakDecloak, "NanoSuit ActivateMode: decloak timer", 0x168D72A, 7, kCloakDecloakRepl);
constexpr unsigned char kPerkTierCtorSig[] = {0x80, 0x3D, 0x1A, 0x5A, 0xF5, 0x00, 0x00, 0x74, 0x09};
constexpr unsigned char kPerkTierCtorRepl[] = {0xEB};
SP_PATCH(kPerkTierCtor, "BasePerk ctor: SP tiers", 0x14FAAE0, 7, kPerkTierCtorRepl);
constexpr unsigned char kPerkTierEnterSig[] = {0x80, 0x3D, 0x70, 0x58, 0xF5, 0x00, 0x00, 0x48, 0x8B, 0xFA, 0x48, 0x8B, 0xD9, 0x74, 0x49};
constexpr unsigned char kPerkTierEnterRepl[] = {0xEB};
SP_PATCH(kPerkTierEnter, "BasePerk Enter: SP tiers", 0x14FAC8A, 13, kPerkTierEnterRepl);
constexpr unsigned char kStealthKill1Sig[] = {0x80, 0x3D, 0x76, 0xFE, 0xE0, 0x00, 0x00, 0x74, 0x38};
constexpr unsigned char kStealthKill1Repl[] = {0xEB};
SP_PATCH(kStealthKill1, "StealthKill: not only while cloaked", 0x1640684, 7, kStealthKill1Repl);
constexpr unsigned char kStealthKill2Sig[] = {0x74, 0x38, 0x48, 0x8B, 0x05, 0x2C, 0xAA, 0xEE, 0x00, 0x8B, 0x88, 0x18, 0x06, 0x00, 0x00};
constexpr unsigned char kStealthKill2Repl[] = {0xEB};
SP_PATCH(kStealthKill2, "StealthKill: not only while cloaked (inlined)", 0x163EF0B, 0, kStealthKill2Repl);
constexpr unsigned char kStealthKillMaxSig[] = {0x44, 0x38, 0x3D, 0x43, 0x20, 0xE1, 0x00, 0xC5, 0xF8, 0x29, 0xB4, 0x24, 0x80, 0x01, 0x00, 0x00, 0x75, 0x27};
constexpr unsigned char kStealthKillMaxRepl[] = {0x66, 0x90};
SP_PATCH(kStealthKillMax, "StealthKill: Maximum perk extras", 0x163E4B7, 16, kStealthKillMaxRepl);
constexpr unsigned char kStealthKillFlagSig[] = {0x44, 0x38, 0x2D, 0x5D, 0x1E, 0xE1, 0x00, 0x74, 0x6E};
constexpr unsigned char kStealthKillFlagRepl[] = {0xEB};
SP_PATCH(kStealthKillFlag, "StealthKill: bStealthKilled", 0x163E69D, 7, kStealthKillFlagRepl);
constexpr unsigned char kStealthKillCloakSig[] = {0x44, 0x38, 0x35, 0x91, 0x00, 0xE1, 0x00, 0x75, 0x1E};
constexpr unsigned char kStealthKillCloakRepl[] = {0x66, 0x90};
SP_PATCH(kStealthKillCloak, "StealthKill: cloak interference", 0x1640469, 7, kStealthKillCloakRepl);
constexpr unsigned char kAiExplosionSig[] = {0x44, 0x38, 0x3D, 0xF6, 0x98, 0xB8, 0x00, 0x0F, 0x85, 0x44, 0x01, 0x00, 0x00};
constexpr unsigned char kAiExplosionRepl[] = {0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00};
SP_PATCH(kAiExplosion, "ClientExplosion: AI explosion stimuli", 0x18C6C04, 7, kAiExplosionRepl);
constexpr unsigned char kFall1Sig[] = {0x44, 0x38, 0x3D, 0x0A, 0xD4, 0xDD, 0x00, 0x74, 0x15};
constexpr unsigned char kFall1Repl[] = {0xEB};
SP_PATCH(kFall1, "PlayerStateUtil: SP fall damage (1)", 0x16730F0, 7, kFall1Repl);
constexpr unsigned char kFall2Sig[] = {0x44, 0x38, 0x3D, 0x57, 0xD2, 0xDD, 0x00, 0x74, 0x0A};
constexpr unsigned char kFall2Repl[] = {0xEB};
SP_PATCH(kFall2, "PlayerStateUtil: SP fall damage (2)", 0x16732A3, 7, kFall2Repl);
constexpr unsigned char kFall3Sig[] = {0x0F, 0xB6, 0x05, 0xEF, 0xD1, 0xDD, 0x00, 0xC5, 0xF9, 0x6E, 0xC0};
constexpr unsigned char kFall3Repl[] = {0x31, 0xC0, 0x0F, 0x1F, 0x44, 0x00, 0x00};
SP_PATCH(kFall3, "PlayerStateUtil: SP fall damage (3)", 0x167330B, 0, kFall3Repl);
constexpr unsigned char kArrowPickSig[] = {0x80, 0x3D, 0x25, 0x08, 0xC6, 0x00, 0x00, 0x75, 0x08};
constexpr unsigned char kArrowPickRepl[] = {0x66, 0x90};
SP_PATCH(kArrowPick, "BowArrow: pickable arrows", 0x17EFCD5, 7, kArrowPickRepl);
constexpr unsigned char kArrowIcon1Sig[] = {0x80, 0x3D, 0x37, 0x11, 0xC6, 0x00, 0x00, 0x75, 0x44};
constexpr unsigned char kArrowIcon1Repl[] = {0x66, 0x90};
SP_PATCH(kArrowIcon1, "BowArrow: visor icons (1)", 0x17EF3C3, 7, kArrowIcon1Repl);
constexpr unsigned char kArrowIcon2Sig[] = {0x80, 0x3D, 0x0D, 0x11, 0xC6, 0x00, 0x00, 0x75, 0x1A};
constexpr unsigned char kArrowIcon2Repl[] = {0x66, 0x90};
SP_PATCH(kArrowIcon2, "BowArrow: visor icons (2)", 0x17EF3ED, 7, kArrowIcon2Repl);
constexpr unsigned char kArrowAirburstSig[] = {0x80, 0x3D, 0x5E, 0xF9, 0xC5, 0x00, 0x00, 0x75, 0x4F};
constexpr unsigned char kArrowAirburstRepl[] = {0x66, 0x90};
SP_PATCH(kArrowAirburst, "BowArrow: airburst targets", 0x17F0B9C, 7, kArrowAirburstRepl);
constexpr unsigned char kPickThrowSig[] = {0x44, 0x38, 0x3D, 0xE1, 0x5F, 0xC3, 0x00, 0x44, 0x89, 0x7D, 0xD8, 0x74, 0x38};
constexpr unsigned char kPickThrowRepl[] = {0xEB};
SP_PATCH(kPickThrow, "PickAndThrowWeapon: SP input handlers", 0x181A519, 11, kPickThrowRepl);
constexpr unsigned char kCutsceneInputSig[] = {0x44, 0x38, 0x25, 0x3F, 0x97, 0xBA, 0x00, 0x75, 0x1A};
constexpr unsigned char kCutsceneInputRepl[] = {0x66, 0x90};
SP_PATCH(kCutsceneInput, "GameRules: cutscene input", 0x18A6DBB, 7, kCutsceneInputRepl);
constexpr unsigned char kClientOnHitSig[] = {0x80, 0x3D, 0x3C, 0xBA, 0xB8, 0x00, 0x00, 0x0F, 0x85, 0x82, 0x00, 0x00, 0x00};
constexpr unsigned char kClientOnHitRepl[] = {0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00};
SP_PATCH(kClientOnHit, "GameRules: client OnHit Lua", 0x18C4ABE, 7, kClientOnHitRepl);
constexpr unsigned char kUsableHlSig[] = {0x0F, 0xB6, 0x05, 0x34, 0x45, 0xBE, 0x00};
constexpr unsigned char kUsableHlRepl[] = {0x31, 0xC0, 0x0F, 0x1F, 0x44, 0x00, 0x00};
SP_PATCH(kUsableHl, "Usable highlight / player_sp prompts", 0x186BFC6, 0, kUsableHlRepl);
constexpr unsigned char kCephAmmo1Sig[] = {0x80, 0x3D, 0x8F, 0x96, 0xF5, 0x00, 0x00, 0x0F, 0x85, 0xBA, 0x00, 0x00, 0x00};
constexpr unsigned char kCephAmmo1Repl[] = {0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00};
SP_PATCH(kCephAmmo1, "AlienHeavyWeapon: SP ammo (1)", 0x14F6E6B, 7, kCephAmmo1Repl);
constexpr unsigned char kCephAmmo2Sig[] = {0x80, 0x3D, 0xDD, 0x94, 0xF5, 0x00, 0x00, 0x0F, 0x85, 0x99, 0x00, 0x00, 0x00};
constexpr unsigned char kCephAmmo2Repl[] = {0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00};
SP_PATCH(kCephAmmo2, "AlienHeavyWeapon: SP ammo (2)", 0x14F701D, 7, kCephAmmo2Repl);
constexpr unsigned char kCephAmmo3Sig[] = {0x80, 0x3D, 0x75, 0x93, 0xF5, 0x00, 0x00, 0x75, 0x1C};
constexpr unsigned char kCephAmmo3Repl[] = {0x66, 0x90};
SP_PATCH(kCephAmmo3, "AlienHeavyWeapon: SP ammo (3)", 0x14F7185, 7, kCephAmmo3Repl);
constexpr unsigned char kCephAmmo4Sig[] = {0x80, 0x3D, 0x80, 0x8E, 0xF5, 0x00, 0x00, 0x48, 0x8B, 0xF8, 0x75, 0x2B};
constexpr unsigned char kCephAmmo4Repl[] = {0x66, 0x90};
SP_PATCH(kCephAmmo4, "AlienHeavyWeapon: SP ammo (4)", 0x14F767A, 10, kCephAmmo4Repl);
constexpr unsigned char kArmorEnergy1Sig[] = {0x80, 0x3D, 0x46, 0x95, 0xDC, 0x00, 0x00, 0x0F, 0x85, 0x8D, 0x01, 0x00, 0x00};
constexpr unsigned char kArmorEnergy1Repl[] = {0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00};
SP_PATCH(kArmorEnergy1, "NanoModeArmor: energy drain on hit (1)", 0x1686FB4, 7, kArmorEnergy1Repl);
constexpr unsigned char kArmorEnergy2Sig[] = {0x80, 0x3D, 0x24, 0x90, 0xDC, 0x00, 0x00, 0x48, 0x8B, 0xD9, 0x0F, 0x85, 0xCA, 0x00, 0x00, 0x00};
constexpr unsigned char kArmorEnergy2Repl[] = {0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00};
SP_PATCH(kArmorEnergy2, "NanoModeArmor: energy drain on hit (2)", 0x16874D6, 10, kArmorEnergy2Repl);
constexpr unsigned char kSprintSig[] = {0x40, 0x38, 0x3D, 0xD6, 0xFD, 0xE3, 0x00, 0x48, 0x8B, 0x83, 0x28, 0x11, 0x00, 0x00, 0x74, 0x1B};
constexpr unsigned char kSprintRepl[] = {0xEB};
SP_PATCH(kSprint, "Player: SP sprint limit", 0x1610724, 14, kSprintRepl);
constexpr unsigned char kKillPlayerSig[] = {0x80, 0x3D, 0x52, 0x5D, 0xB6, 0x00, 0x00, 0x75, 0x10};
constexpr unsigned char kKillPlayerRepl[] = {0x66, 0x90};
SP_PATCH(kKillPlayer, "Actor:KillPlayer: two punish hits", 0x18EA7A8, 7, kKillPlayerRepl);
constexpr unsigned char kHoldBreathSig[] = {0x80, 0x3D, 0xD4, 0xB8, 0xC8, 0x00, 0x00, 0x48, 0x8B, 0xD9, 0x75, 0x59};
constexpr unsigned char kHoldBreathRepl[] = {0x66, 0x90};
SP_PATCH(kHoldBreath, "Scope: hold breath", 0x17C4C26, 10, kHoldBreathRepl);
constexpr unsigned char kLtagActorsSig[] = {0x0F, 0xB6, 0x05, 0x8F, 0x1B, 0xC5, 0x00, 0x4D, 0x8B, 0xF0, 0x4D, 0x85, 0xC0, 0x74, 0x60, 0x84, 0xC0, 0x75, 0x5C};
constexpr unsigned char kLtagActorsRepl[] = {0x66, 0x90};
SP_PATCH(kLtagActors, "L-TAG: detonate on actors", 0x17FE96B, 17, kLtagActorsRepl);
constexpr unsigned char kBurnServerSig[] = {0x80, 0x3D, 0xD0, 0x34, 0xC7, 0x00, 0x00, 0x75, 0x36};
constexpr unsigned char kBurnServerRepl[] = {0xCF, 0x34, 0xC7, 0x00, 0x00, 0x74};
SP_PATCH(kBurnServer, "Burn effect: server only", 0x17DD02A, 2, kBurnServerRepl);
constexpr unsigned char kGunfirePerceptionSig[] = {0x80, 0x3D, 0xD7, 0xF0, 0xC8, 0x00, 0x00, 0x75, 0x5B, 0x48, 0x3B, 0xDF, 0x75, 0x56};
constexpr unsigned char kGunfirePerceptionRepl[] = {0x66, 0x90};
SP_PATCH(kGunfirePerception, "WeaponEvent: gunfire undoes perception dimming", 0x17C1423, 7, kGunfirePerceptionRepl);
constexpr unsigned char kMpSoundSig[] = {0x4C, 0x8B, 0xDC, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x81, 0xEC, 0x40, 0x02, 0x00, 0x00, 0x80, 0x3D, 0x0B, 0x5D, 0x6D, 0x01, 0x00, 0x4D, 0x8B, 0xF1, 0x4D, 0x8B, 0xF8, 0x48, 0x8B, 0xFA, 0x0F, 0x84, 0x73, 0x01, 0x00, 0x00};
constexpr unsigned char kMpSoundRepl[] = {0x90, 0xE9, 0x73, 0x01, 0x00, 0x00};
SP_PATCH(kMpSound, "SoundSystem name fixup: no w_*_mp rename", 0xD7A7E0, 31, kMpSoundRepl);
constexpr unsigned char kGameAiUpdateSig[] = {0x44, 0x38, 0x3D, 0x4F, 0xBE, 0xD9, 0x00, 0x75, 0x21};
constexpr unsigned char kGameAiUpdateRepl[] = {0x4E, 0xBE, 0xD9, 0x00, 0x74};
SP_PATCH(kGameAiUpdate, "CGame::Update: game AI modules on the server", 0x16B46AB, 3, kGameAiUpdateRepl);
constexpr unsigned char kImmersiveSig[] = {0x38, 0x0D, 0xE0, 0x27, 0xE3, 0x01, 0x75, 0x07, 0xB9, 0x01, 0x00, 0x00, 0x00};
constexpr unsigned char kImmersiveRepl[] = {0x66, 0x90};
SP_PATCH(kImmersive, "IsImmersivenessEnabled: always", 0x61DD1B, 6, kImmersiveRepl);
constexpr unsigned char kSlots1Sig[] = {0x38, 0x15, 0xD5, 0xCA, 0xE5, 0x00, 0x48, 0x8B, 0x08, 0x4C, 0x8B, 0x89, 0x40, 0x02, 0x00, 0x00, 0x0F, 0x84, 0x99, 0x00, 0x00, 0x00};
constexpr unsigned char kSlots1Repl[] = {0xE9, 0x9A, 0x00, 0x00, 0x00, 0x90};
SP_PATCH(kSlots1, "Player: SP inventory slots (1)", 0x15F3A26, 16, kSlots1Repl);
constexpr unsigned char kSlots2Sig[] = {0x38, 0x15, 0xD7, 0xF4, 0xE3, 0x00, 0x48, 0x8B, 0x08, 0x4C, 0x8B, 0x89, 0x40, 0x02, 0x00, 0x00, 0x0F, 0x84, 0xAF, 0x00, 0x00, 0x00};
constexpr unsigned char kSlots2Repl[] = {0xE9, 0xB0, 0x00, 0x00, 0x00, 0x90};
SP_PATCH(kSlots2, "Player: SP inventory slots (2)", 0x1611024, 16, kSlots2Repl);
constexpr unsigned char kSlotsAiSig[] = {0x38, 0x05, 0xF2, 0xCA, 0xE5, 0x00, 0x0F, 0x84, 0xA5, 0x01, 0x00, 0x00};
constexpr unsigned char kSlotsAiRepl[] = {0xE9, 0xA6, 0x01, 0x00, 0x00, 0x90};
SP_PATCH(kSlotsAi, "Player: skip AI slot setup", 0x15F3A09, 6, kSlotsAiRepl);

// ---- [hackdata] hacking (usable terminals like DoorPanel, visor hacks of mines/turrets)
// CTacticalManager::AddEntity (TacticalManager.cpp:230, 0x38703600): `if (!bMultiplayer) { SetClassScanned(class,
// eScanned_None); HackingManager->AddEntityHackingData(id, pEntity); }`. In MP CHackingManager::m_hackingData stays
// empty: no hackable target, the plain Use prompt, and OnActionHacking (0x38553E40) returns at once.
// 0x38703685 jne -> nop. Applies to entities registered after it is on (inject before `map`).
constexpr unsigned char kHackDataSig[] = {0x80, 0x3D, 0x7C, 0xCE, 0xD4, 0x00, 0x00, 0x75, 0x55};
SP_PATCH(kHackData, "TacticalManager::AddEntity: register hacking data", 0x170367E, 7, kNop2);
// CMinigameManager::PreloadGameTypeResources (0x3848C090): skips the minigame XML preload in MP (loaded on demand at
// the first hack instead). 0x3848C096 jne -> nop.
constexpr unsigned char kMinigamePreloadSig[] = {0x48, 0x83, 0xEC, 0x28, 0x84, 0xD2, 0x0F, 0x85, 0xD1, 0x00, 0x00, 0x00};
SP_PATCH(kMinigamePreload, "MinigameManager: preload minigame data", 0x148C090, 6, kNop6);

// ---- [ridephys] CryPhysics CRigidEntity::GetPotentialColliders (rigidentity.cpp:1311, 0x37BC3602): `if
// (m_pWorld->m_vars.bMultiplayer && simclass == 3) skip ScheduleForStep` for living neighbours. A rider in
// CPlayerStateTrainRide (pe_player_dynamics.bActive = 0) only moves in the aux step pass, so in MP it froze in the air
// where the Fields wagon picked it up. 0x37BC39B8 je -> jmp (always schedule, SP path).
constexpr unsigned char kRidePhysSig[] = {0x48, 0x8B, 0x4B, 0x70, 0x83, 0xB9, 0x44, 0x01, 0x00, 0x00, 0x00,
                                          0x74, 0x1A, 0x49, 0x8B, 0x07, 0x48, 0x8B, 0x14, 0xF0, 0x8B, 0x42,
                                          0x44, 0x25, 0x00, 0x00, 0x00, 0xFF, 0x3D, 0x00, 0x00, 0x00, 0x03};
SP_PATCH(kRidePhys, "CryPhysics: moving rigid bodies step living entities on them", 0xBC39AD, 11, kJmpShort);

// ---- radar (CHUD_Radar, SDK HUD/HUD_Radar.cpp). hud.cpp [radar] routes AddEntity to the SP path, but its helpers kept
// their MP branches: every actor was added as a high priority enemy entry (64 entries / 32 icons, then "Radar data
// full"), Human got no icon, teamless rules made everybody hostile and MP visibility hid enemies without a timer.
// Nothing was drawn on host or clients.
constexpr unsigned char kMovEax0[] = {0xB8, 0x00, 0x00, 0x00, 0x00, 0x66, 0x90}; // mov eax,0 (flags kept) + nop2
// [radarfilter] ShouldBeAddedOnRadar 0x3888A030: `if (MP) return true` (0x3888A05C jne) -> nop
constexpr unsigned char kRadarShouldAddSig[] = {0x80, 0x3D, 0xB2, 0x64, 0xBC, 0x00, 0x00, 0x41, 0x0F, 0xB6, 0xD9, 0x41, 0x0F,
                                                0xB6, 0xE8, 0x8B, 0xFA, 0x4C, 0x8B, 0xF9, 0x0F, 0x85, 0x1A, 0x01, 0x00, 0x00};
SP_PATCH(kRadarShouldAdd, "CHUD_Radar::ShouldBeAddedOnRadar SP filter", 0x188A048, 20, kNop6);
// IsFriendlyToClient 0x3888AB30: MP GetThreatRating (teamless: hostile) -> SP IsFriendlyEntity (0x3888ABCC je -> jmp)
constexpr unsigned char kRadarFriendlySig[] = {0x80, 0x3D, 0x35, 0x59, 0xBC, 0x00, 0x00, 0x74, 0x3E};
SP_PATCH(kRadarFriendly, "CHUD_Radar::IsFriendlyToClient via IsFriendlyEntity", 0x188ABC5, 7, kJmpShort);
// GetIconFromEntity 0x3888A420: `class == Player || DummyPlayer || !MP` -> actor icons (movzx eax,[bMultiplayer] -> 0)
constexpr unsigned char kRadarIconSig[] = {0x0F, 0xB6, 0x05, 0x04, 0x60, 0xBC, 0x00, 0x0F, 0x84, 0xBC, 0x00, 0x00, 0x00};
SP_PATCH(kRadarIcon, "CHUD_Radar::GetIconFromEntity: Human gets SP icons", 0x188A4F6, 0, kMovEax0);
// OnHUDEvent(OnEntityTagged): SP checks ShouldBeAddedOnRadar(spotted) (0x38886AC4 jne -> nop)
constexpr unsigned char kRadarTaggedSig[] = {0x80, 0x3D, 0x44, 0x9A, 0xBC, 0x00, 0x00, 0x48, 0x8B, 0x42,
                                             0x08, 0x8B, 0x70, 0x10, 0x0F, 0x85, 0xAA, 0x00, 0x00, 0x00};
SP_PATCH(kRadarTagged, "CHUD_Radar tagged event SP check", 0x1886AB6, 14, kNop6);
// [radarvis] (only with radarfilter: SP visibility shows every entry) Update/AddEntity: SetRadarIconVisiblity SP
// 0x38889A90 instead of MP 0x38889C30 (je -> jmp)
constexpr unsigned char kRadarVisUpdateSig[] = {0x80, 0x3D, 0x5E, 0x9F, 0xBC, 0x00, 0x00, 0x4D, 0x8B, 0xCD, 0x48, 0x89, 0x44,
                                                0x24, 0x20, 0x4D, 0x8B, 0xC6, 0x48, 0x8B, 0xD6, 0x48, 0x8B, 0xCF, 0x74, 0x07};
SP_PATCH(kRadarVisUpdate, "CHUD_Radar::Update SP icon visibility", 0x188659C, 24, kJmpShort);
constexpr unsigned char kRadarVisAddSig[] = {0x80, 0x3D, 0x3F, 0x70, 0xBC, 0x00, 0x00, 0x4D, 0x8B, 0xCF, 0x48, 0x89, 0x44,
                                             0x24, 0x20, 0x4D, 0x8B, 0xC6, 0x48, 0x8B, 0xD6, 0x48, 0x8B, 0xCB, 0x74, 0x07};
SP_PATCH(kRadarVisAdd, "CHUD_Radar::AddEntity SP icon visibility", 0x18894BB, 24, kJmpShort);
// Update: `if (!MP && expired && !permanent) remove` (0x388865E3 jne -> nop)
constexpr unsigned char kRadarExpireSig[] = {0x80, 0x3D, 0x1E, 0x9F, 0xBC, 0x00, 0x00, 0x75, 0x31};
SP_PATCH(kRadarExpire, "CHUD_Radar::Update removes expired entries", 0x18865DC, 7, kNop2);
// SetupEntity 0x3888A1A0: friendly -> permanent (`friendly && !MP`), enemy added with time 0 -> permanent
constexpr unsigned char kRadarSetupPerm1Sig[] = {0x0F, 0xB6, 0x05, 0xBF, 0x61, 0xBC, 0x00, 0x45, 0x84, 0xFF};
SP_PATCH(kRadarSetupPerm1, "CHUD_Radar::SetupEntity friendly permanent", 0x188A33B, 0, kMovEax0);
constexpr unsigned char kRadarSetupPerm2Sig[] = {0x80, 0x3D, 0x9A, 0x61, 0xBC, 0x00, 0x00, 0x75, 0x24, 0xB0, 0x01};
SP_PATCH(kRadarSetupPerm2, "CHUD_Radar::SetupEntity enemy permanent", 0x188A360, 7, kNop2);
// [radarshoot] OnHUDEvent(OnShoot): SP path, shooter stays (time 0); ShowSoundWaveFromActivity `if (MP) return`
constexpr unsigned char kRadarShootMpSig[] = {0x80, 0x3D, 0x97, 0x96, 0xBC, 0x00, 0x00, 0x48,
                                              0x8B, 0x42, 0x08, 0x8B, 0x70, 0x10, 0x74, 0x24};
SP_PATCH(kRadarShootMp, "CHUD_Radar shoot event SP path", 0x1886E63, 14, kJmpShort);
constexpr unsigned char kRadarShootTimeSig[] = {0x80, 0x3D, 0x37, 0x96, 0xBC, 0x00, 0x00, 0x0F, 0x84, 0x93, 0x00, 0x00, 0x00};
constexpr unsigned char kJmpRadarShootTime[] = {0xE9, 0x94, 0x00, 0x00, 0x00, 0x90};
SP_PATCH(kRadarShootTime, "CHUD_Radar shooter stays like SP", 0x1886EC3, 7, kJmpRadarShootTime);
constexpr unsigned char kRadarSoundWaveSig[] = {0x80, 0x3D, 0x20, 0x55, 0xBC, 0x00, 0x00, 0x8B, 0xF2,
                                                0x48, 0x8B, 0xF9, 0x0F, 0x85, 0xF5, 0x00, 0x00, 0x00};
SP_PATCH(kRadarSoundWave, "CHUD_Radar sound waves", 0x188AFDA, 12, kNop6);
// [radarview] compass north hud_radarNorthRotationMP 0.57 -> SP 0; radar range hud_radarScaleMP 15 -> SP 35 (before map
// load); visor zoom; SetupEntity live positions (MP freezes enemies at the last seen spot)
constexpr unsigned char kRadarNorthSig[] = {0x80, 0x3D, 0x0A, 0xA3, 0xBC, 0x00, 0x00, 0x48, 0x8B, 0x05,
                                            0x3A, 0x37, 0xCA, 0x00, 0x48, 0x8B, 0x88, 0x28, 0x03, 0x00,
                                            0x00, 0x48, 0x8B, 0x81, 0x98, 0x00, 0x00, 0x00, 0x74, 0x0A};
SP_PATCH(kRadarNorth, "CHUD_Radar compass SP north rotation", 0x18861F0, 28, kJmpShort);
constexpr unsigned char kRadarScaleSig[] = {0x80, 0x3D, 0x0C, 0x98, 0xBC, 0x00, 0x00, 0x48, 0x8B, 0x05,
                                            0x3C, 0x2C, 0xCA, 0x00, 0x48, 0x8B, 0x88, 0x28, 0x03, 0x00,
                                            0x00, 0x48, 0x8B, 0x81, 0x98, 0x00, 0x00, 0x00, 0x74, 0x0A};
SP_PATCH(kRadarScale, "CHUD_Radar SP radar range", 0x1886CEE, 28, kJmpShort);
constexpr unsigned char kRadarVisorZoomSig[] = {0x80, 0x3D, 0x2F, 0x94, 0xBC, 0x00, 0x00, 0x0F, 0x85, 0x05, 0x02, 0x00, 0x00};
SP_PATCH(kRadarVisorZoom, "CHUD_Radar visor zoom", 0x18870CB, 7, kNop6);
constexpr unsigned char kRadarSetupPosSig[] = {0x80, 0x3D, 0x86, 0x62, 0xBC, 0x00, 0x00, 0xC5, 0xC8,
                                               0x57, 0xF6, 0x0F, 0x84, 0xE8, 0x00, 0x00, 0x00};
constexpr unsigned char kJmpRadarSetupPos[] = {0xE9, 0xE9, 0x00, 0x00, 0x00, 0x90};
SP_PATCH(kRadarSetupPos, "CHUD_Radar::SetupEntity live positions", 0x188A274, 11, kJmpRadarSetupPos);
// [radaralert] alertness colours (host only: clients have no AI objects)
constexpr unsigned char kRadarSetupAlertSig[] = {0x0F, 0xB6, 0x05, 0xCE, 0x61, 0xBC, 0x00, 0x84, 0xC0, 0x75, 0x0B};
SP_PATCH(kRadarSetupAlert, "CHUD_Radar::SetupEntity alertness flag", 0x188A32C, 0, kMovEax0);
constexpr unsigned char kRadarAlertStatsSig[] = {0x44, 0x38, 0x25, 0x39, 0x7D, 0xBC, 0x00, 0x4C, 0x8B, 0xBC, 0x24,
                                                 0xC0, 0x00, 0x00, 0x00, 0x0F, 0x85, 0x0A, 0x01, 0x00, 0x00};
SP_PATCH(kRadarAlertStats, "CHUD_Radar::UpdateEntityStats alertness", 0x18887C1, 15, kNop6);

#undef SP_PATCH

// ---------------------------------------------------------------- patch helpers

// 0 = original bytes, 1 = patched, -1 = neither (unknown build / someone else patched it)
int PatchState(const BytePatch& p) {
    auto at = game::At<unsigned char>(p.rva);
    if (memcmp(at, p.sig, p.sigLen) == 0)
        return 0;
    bool patched = memcmp(at, p.sig, p.offset) == 0 && memcmp(at + p.offset, p.repl, p.replLen) == 0 &&
                   memcmp(at + p.offset + p.replLen, p.sig + p.offset + p.replLen, p.sigLen - p.offset - p.replLen) == 0;
    return patched ? 1 : -1;
}

bool SetPatch(const BytePatch& p, bool on) {
    int state = PatchState(p);
    auto at = game::At<unsigned char>(p.rva);
    if (state < 0) {
        c3coop::Logf("sp: %s: bytes at %p do not match the signature, NOT patched", p.what, at);
        return false;
    }
    if (state == (on ? 1 : 0))
        return true;
    unsigned char* dst = at + p.offset;
    DWORD old;
    if (!VirtualProtect(dst, p.replLen, PAGE_EXECUTE_READWRITE, &old)) {
        c3coop::Logf("sp: %s: VirtualProtect failed (%lu)", p.what, GetLastError());
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

// ---------------------------------------------------------------- fixes table

constexpr size_t kMaxPatches = 5;

struct Fix {
    const char* name;
    const BytePatch* patches[kMaxPatches];
    bool defaultOn;
    const char* help;
};

Fix g_fixes[] = {
    {"nys", {&kNys}, true, "no not_yet_spawned filter on spawn (Q/E armor/cloak, B visor, N, binoculars, zoom)"},
    {"actionmap", {&kAmCet, &kAmReload, &kAmHint}, true, "player_sp action map (mouse3 suit wheel, 3/4 keys, drop)"},
    {"spmap", {&kSpMap}, true, "singleplayer action map on (X weapon customization)"},
    {"perks", {&kPerks}, true, "H opens/closes the suit upgrade (perk) menu"},
    {"ammo", {&kAmmoEnergy, &kAmmoGfx, &kAmmoInit, &kAmmoSelect}, true, "SP ammo/weapon/energy HUD panel (HUD reinit)"},
    {"wheel", {&kWheelInit, &kWheelUpd}, true, "mouse3 wheel weapon/suit customization sectors"},
    {"visor", {&kVisorInit, &kVisorPing, &kVisorStealth, &kVisorInf}, true, "visor ping, stealth bar, range icon"},
    {"tagging", {&kTagScan1, &kTagScan2, &kTagStalker1, &kTagStalker2, &kTagEnd}, true, "visor scan/tag as in SP"},
    {"interest", {&kIpInit, &kIpUnits, &kIpFriend, &kIpTagged, &kIpAware}, true, "visor enemy/friendly icons"},
    {"nanovision", {&kNanoVision}, true, "NanoVision allowed for new suits"},
    {"regen", {&kRegenRate, &kRegenWho}, true, "SP health regeneration (rates; nanosuit wearers only)"},
    {"playmode", {&kPlayMode}, true, "SP Mannequin playMode tag (weapon fire/reload sounds, SP 1P anim variants)"},
    {"custmenu", {&kCustMenu}, true, "weapon customization lists the attachments in the inventory (not MP unlocks)"},
    {"pickup", {&kPickupGrab, &kPickupPrompt, &kPickupUse, &kPickupAnim}, true,
     "host picks up physics objects (not only environmental weapons)"},
    {"grabenemy", {&kAimRegister, &kAimHostile, &kGrabDeathMgr}, true, "host grabs enemies (AutoAim flags, needs gameai)"},
    {"throwsp", {&kThrowCloakGrab, &kThrowCloakThrow, &kThrowSpin, &kThrowPower1, &kThrowPower2}, true,
     "SP throw details (cloak interference, spin, Maximum power throw)"},
    {"cloak", {&kCloakAi1, &kCloakAi2, &kCloakAi3, &kCloakDecloak}, true, "cloak hides players from AI (SP AI cloak params, decloak timer)"},
    {"perktiers", {&kPerkTierCtor, &kPerkTierEnter}, true, "suit upgrades use SP perk tiers"},
    {"stealthkill", {&kStealthKill1, &kStealthKill2, &kStealthKillMax, &kStealthKillFlag, &kStealthKillCloak}, true, "SP stealth kills (not only cloaked, Maximum perk extras)"},
    {"aistim", {&kAiExplosion}, true, "AI react to explosions"},
    {"falldamage", {&kFall1, &kFall2, &kFall3}, true, "SP fall damage"},
    {"bowarrows", {&kArrowPick, &kArrowIcon1, &kArrowIcon2, &kArrowAirburst}, true, "bow arrows pickable, visor icons, airburst targets"},
    {"throw", {&kPickThrow}, true, "left mouse throws picked-up objects and enemies"},
    {"cutsceneinput", {&kCutsceneInput}, true, "cutscene look-around, rumble, cinematic aim"},
    {"clienthit", {&kClientOnHit}, true, "client OnHit Lua (bleeding, hit sounds, doors, boids)"},
    {"usable", {&kUsableHl}, true, "usable object highlight, player_sp prompts"},
    {"cephammo", {&kCephAmmo1, &kCephAmmo2, &kCephAmmo3, &kCephAmmo4}, true, "Ceph weapon pickups give ammo"},
    {"armorenergy", {&kArmorEnergy1, &kArmorEnergy2}, true, "armor mode drains energy when hit"},
    {"sprint", {&kSprint}, true, "SP sprint limit"},
    {"killplayer", {&kKillPlayer}, true, "Actor:KillPlayer kills through mercy time"},
    {"weaponsp", {&kHoldBreath, &kLtagActors, &kBurnServer}, true, "scope hold breath, L-TAG on actors, burn on the server only"},
    {"aiperception", {&kGunfirePerception}, true, "player gunfire undoes scripted AI perception dimming"},
    {"mpsound", {&kMpSound}, true, "weapon sounds keep their SP FMOD project (no w_*_mp rename)"},
    {"gameai", {&kGameAiUpdate}, true, "game AI modules on the host (hazards, search, stealth bar); big change"},
    {"immersive", {&kImmersive}, true, "SP physics for props and explosive objects (replicated); big change"},
    {"spslots", {&kSlots1, &kSlots2, &kSlotsAi}, true, "SP inventory slots (bow, grenades); big change, before spawn"},
    {"hackdata", {&kHackData, &kMinigamePreload}, true, "hacking terminals, mines, turrets (before map load)"},
    {"ridephys", {&kRidePhys}, true, "riders follow moving rigid bodies (Fields wagon, platforms)"},
    {"radarfilter", {&kRadarShouldAdd, &kRadarFriendly, &kRadarIcon, &kRadarTagged}, true, "radar: SP filter, friends, icons"},
    {"radarvis", {&kRadarVisUpdate, &kRadarVisAdd, &kRadarExpire, &kRadarSetupPerm1, &kRadarSetupPerm2}, true,
     "radar: SP visibility and lifetime (only with radarfilter)"},
    {"radarshoot", {&kRadarShootMp, &kRadarShootTime, &kRadarSoundWave}, true, "radar: shooters and sound waves as in SP"},
    {"radarview", {&kRadarNorth, &kRadarScale, &kRadarVisorZoom, &kRadarSetupPos}, true, "radar: SP compass, range, zoom"},
    {"radaralert", {&kRadarSetupAlert, &kRadarAlertStats}, true, "radar: AI alertness colours (host)"},
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
    return s;
}

// ---------------------------------------------------------------- not_yet_spawned filter

void* NotYetSpawnedFilter() {
    char* actions = *game::At<char*>(kRvaGameActions);
    return actions ? *reinterpret_cast<void**>(actions + kGameActionsFilterNotYetSpawned) : nullptr;
}

// main thread only (commands run from the IConsole::Update hook)
void ClearNotYetSpawned(const char* why) {
    void* filter = NotYetSpawnedFilter();
    if (!filter) {
        c3coop::Logf("sp: no not_yet_spawned filter (game actions not created yet)");
        return;
    }
    bool enabled = game::Virtual<bool(__fastcall*)(void*)>(filter, kVtFilterEnabled)(filter);
    if (enabled)
        game::Virtual<void(__fastcall*)(void*, bool)>(filter, kVtFilterEnable)(filter, false);
    c3coop::Logf("sp: not_yet_spawned filter %s (%s)", enabled ? "was enabled -> disabled" : "already disabled", why);
}

// ---------------------------------------------------------------- commands

void Status() {
    c3coop::Logf("sp: bMultiplayer=%d bServer=%d", *game::At<unsigned char>(game::kRvaMultiplayerFlag),
                 *game::At<unsigned char>(game::kRvaServerFlag));
    if (void* filter = NotYetSpawnedFilter())
        c3coop::Logf("sp: not_yet_spawned filter %p enabled=%d", filter,
                     game::Virtual<bool(__fastcall*)(void*)>(filter, kVtFilterEnabled)(filter));
    for (auto& f : g_fixes)
        c3coop::Logf("sp: fix %-10s %-24s %s", f.name, FixState(f).c_str(), f.help);
}

void Help() {
    c3coop::Logf("sp: !sp status | !sp <fix> on|off | !sp all on|off | !sp nys clear");
    for (auto& f : g_fixes)
        c3coop::Logf("sp:   %-10s %s (default %s)", f.name, f.help, f.defaultOn ? "on" : "off");
}

} // namespace

namespace c3coop {

void InstallSpFeatures() {
    for (auto& f : g_fixes) {
        bool ok = SetFix(f, f.defaultOn);
        Logf("sp: fix %s %s%s", f.name, f.defaultOn ? "on" : "off", ok ? "" : " (FAILED, see above)");
    }
}

bool SpCommand(const std::string& cmd) {
    if (cmd != "!sp" && cmd.rfind("!sp ", 0) != 0)
        return false;
    std::string args = cmd.size() > 4 ? cmd.substr(4) : "";
    std::string verb = args.substr(0, args.find(' '));
    std::string rest = args.find(' ') == std::string::npos ? "" : args.substr(args.find(' ') + 1);

    if (verb.empty() || verb == "help") {
        Help();
    } else if (verb == "status") {
        Status();
    } else if (verb == "nys" && rest == "clear") {
        ClearNotYetSpawned("!sp nys clear");
    } else if (rest == "on" || rest == "off") {
        bool on = rest == "on";
        if (verb == "all") {
            for (auto& f : g_fixes)
                SetFix(f, on);
            Logf("sp: all fixes %s", rest.c_str());
        } else if (Fix* f = FindFix(verb)) {
            bool ok = SetFix(*f, on);
            Logf("sp: fix %s %s%s", f->name, rest.c_str(), ok ? "" : " (FAILED)");
            if (on && verb == "nys")
                ClearNotYetSpawned("!sp nys on"); // the patch only affects players initialised later
        } else {
            Logf("sp: unknown fix '%s'", verb.c_str());
        }
    } else {
        Logf("sp: unknown command '%s' (!sp help)", cmd.c_str());
    }
    return true;
}

} // namespace c3coop
