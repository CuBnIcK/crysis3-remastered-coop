// Movement and animation of network-driven actors (remote players, server AI on clients).
//
// Network-driven actors (CNetPlayerInput: the other machines' players, and on clients every AI) animate in jerks and
// flood "Animation-queue filled up to 16 entries" because their mannequin movement fragment is re-selected many times
// per second. Causes found in the remaster binary (addresses below):
//  - aiinput:  CAIInput::GetState sends deltaMovement in m/s; the receiver (CNetPlayerInput::UpdateInterpolation)
//              multiplies it by pl_netSerialiseMaxSpeed (9) and the 'dMov' policy clamps each axis to [-1,1] first, so
//              any moving AI is predicted at ~8-12 m/s in a skewed direction (overshoot, snap, Gait tag churn).
//  - netvel:   'dMov' X/Y is 5-bit TruncateLeft (floor): a standing player's physics velocity noise (-0.0001 m/s)
//              arrives as -0.56 m/s, flickering with 0 at packet rate -> lerper desired speed 0 / 0.5 m/s ->
//              Move/Stand and body-target flip-flop. Sender zeroes tiny velocities and rounds to the nearest step.
//  - stance:   'stnc' was 0..6 and clamped HIGH_COVER(7)/SWIM(8); the data pak widens it to 0..9 (build_pak.py),
//              this toggle decides at runtime whether 7..9 are sent (off = old clamp to 6). STANCE_NULL -> 6 always.
//  - velocity: IsMoving() (|m_lastRequestedVelocity| > 0.1) drives CPlayerMovementAction / CAnimActionAIMovement
//              Move<->Idle; for remote actors that is the lerper's correction velocity, which drops to 0 whenever
//              the lerper converges or snaps. Replaced (after FinalizeMovementRequest) by the smoothed network
//              velocity with hysteresis. Physics still uses the raw lerper velocity.
//  - pseudo:   pseudo speed (AI Gait tag NoGait/Walk/Run) = |lerper velocity|; computed from the same smoothed
//              velocity while CNetPlayerInput::UpdateMoveRequest / SetState run.
// Every fix is a runtime toggle: !anim status | !anim <aiinput|netvel|stance|velocity|pseudo|all> on|off |
// !anim velsrc net|lerp | !anim trace on|off | !anim tune <enter> <exit> <hold> <tau> <deadzone>.
// aiinput/netvel/stance act on the SENDER (host for AI, each machine for its own player); velocity/pseudo act on the
// RECEIVER. AnimationToQueue is hooked for diagnostics only (queued/overflow counters per character).

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "MinHook.h"
#include "game.h"
#include "modules.h"

namespace {

// ------------------------------------------------------------------------------------------------ addresses
// All RVAs = VA - 0x37000000 in Crysis3Remastered.exe (TimeDateStamp 0x62FD4D1B). SDK = 2015 C3 SDK GameDll/CryEngine.

// CAIInput::GetState(this, SSerializedPlayerInput&) 0x3861F1C0. CAIInput vtable 0x38F7A960 (dtor 0x3861F3A0):
// slot 5 SetState = 0x3861F1A0 (only code referencing "CAIInput::SetState called: should never happen"), slot 6 =
// this function, which references "PseudoSpeed". Body matches PlayerInput.cpp:3066-3095: deltaMovement =
// movementDirection.GetNormalizedSafe() * desiredSpeed (0x3861F25F-0x3861F313), no division by the max speed.
constexpr uintptr_t kRvaAIInputGetState = 0x161F1C0;
constexpr unsigned char kAIInputGetStateBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10,
                                                   0x57, 0x48, 0x81, 0xEC, 0x30, 0x01, 0x00, 0x00};

// CPlayerInput::GetState(this, SSerializedPlayerInput&) 0x386180C0 = CPlayerInput vtable 0x38F7A8C8 slot 6. With
// pl_serialisePhysVel (CGameCVars+0x6C4, default 1) it sends (psl.vel - psl.velGround) / pl_netSerialiseMaxSpeed
// (vdivss at 0x386181BB), as PlayerInput.cpp:1134-1147.
constexpr uintptr_t kRvaPlayerInputGetState = 0x16180C0;
constexpr unsigned char kPlayerInputGetStateBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x81,
                                                       0xEC, 0x50, 0x02, 0x00, 0x00, 0x48, 0x8B, 0xF9};

// CPlayerStateUtil::FinalizeMovementRequest(CPlayer&, const SActorFrameMovementParams&, SCharacterMoveRequest&)
// 0x38672860 (15 callers in the player state machine). Calls UpdateRemotePlayersInterpolation 0x38672A30 (reads
// pl_clientInertia CGameCVars+0x6C8 and, for remote players, request.velocity.xy = CNetPlayerInput+0x60), uses the
// "request.velocity" / "request.rotation * FORWARD_DIRECTION" trace strings, then stores request.velocity into
// CPlayer::m_lastRequestedVelocity (+0x1180, 0x386729A2) and re-initialises the request (PlayerStateUtil.cpp:477).
constexpr uintptr_t kRvaFinalizeMovementRequest = 0x1672860;
constexpr unsigned char kFinalizeMovementRequestBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48,
                                                           0x81, 0xEC, 0x90, 0x00, 0x00, 0x00};

// CNetPlayerInput::UpdateMoveRequest(this) 0x385EE970: tail-jumped from CNetPlayerInput::PreUpdate (0x385EDD55).
// Pseudo speed = player->vtbl[0x688](sprint=+0x38, |m_desiredVelocity| (+0x60..+0x68)) at 0x385EF5A0-0x385EF5D6.
constexpr uintptr_t kRvaNetInputUpdateMoveRequest = 0x15EE970;
constexpr unsigned char kNetInputUpdateMoveRequestBytes[] = {0x48, 0x8B, 0xC4, 0x55, 0x48, 0x8D, 0xA8, 0x78,
                                                             0xFD, 0xFF, 0xFF, 0x48, 0x81, 0xEC, 0x80, 0x03};

// CNetPlayerInput::SetState(this, const SSerializedPlayerInput&) 0x385EDE30 = vtable slot 5: DoSetState (0x385EDF00,
// also computes the pseudo speed from +0x60 at 0x385EE45D-0x385EE48F), then m_lastUpdate (+0x58) = now.
constexpr uintptr_t kRvaNetInputSetState = 0x15EDE30;
constexpr unsigned char kNetInputSetStateBytes[] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0xE8};

// CNetPlayerInput vtable 0x38F78388: [0] dtor 0x385EF6D0, [1] PreUpdate 0x385EDCA0 (AllowPhysicsUpdate(+0x50) &&
// IsRemote && HasReceivedUpdate -> UpdateInterpolation 0x385EE560; then +0x60 = m_lerpVel(+0xE0)), [5] SetState.
constexpr uintptr_t kRvaNetPlayerInputVtbl = 0x1F78388;
constexpr uintptr_t kRvaNetInputPreUpdate = 0x15EDCA0;

// CSkeletonAnim::AnimationToQueue(this, const ModelAnimationHeader*, int animId, f32 btime,
// const CryCharAnimationParams&) 0x3788C250: the only code using both "Animation-queue overflow" and "filled up"
// strings (0x3788C2CA, 0x3788C9CE). Queue of layer L = DynArray at this + 0x48 + L*0x40 (size at ptr-4, bit 31
// masked), params.m_nLayerID at +0x10, m_fTransTime at +0, m_nFlags at +0x18; m_pInstance at +0x500, its
// m_pDefaultSkeleton at +0x28, GetModelFilePath = vtbl+0x18 (same calls as the logger). No ca_AnimWarningLevel check
// and no DebugLogTransitionQueueState ("AnimInAFIFO" is not in the exe), unlike SkeletonAnim_Queue.cpp:532.
// The first instruction is VEX-encoded (vmovss), which MinHook's HDE64 cannot decode, so this one is patched by hand:
// the 16 signature bytes are 3 whole instructions without RIP-relative operands (vmovss [rsp+20h],xmm3 /
// mov [rsp+10h],rdx / mov [rsp+8],rcx) and are moved to the trampoline.
constexpr uintptr_t kRvaAnimationToQueue = 0x88C250;
constexpr unsigned char kAnimationToQueueBytes[] = {0xC5, 0xFA, 0x11, 0x5C, 0x24, 0x20, 0x48, 0x89,
                                                    0x54, 0x24, 0x10, 0x48, 0x89, 0x4C, 0x24, 0x08};
constexpr size_t kSkelAnimQueues = 0x48;
constexpr size_t kSkelAnimQueueStride = 0x40;
constexpr int kNumVirtualLayers = 16;
constexpr size_t kSkelAnimInstance = 0x500;
constexpr size_t kCharInstSkeleton = 0x28;
constexpr size_t kVtSkeletonGetModelFilePath = 0x18;
constexpr unsigned kMaxAnimsInQueue = 16;

// g_pGameCVars (read with CGameCVars+0x6C0 at 0x385EE5B0/0x385EE5C2); pl_netSerialiseMaxSpeed registered at
// 0x386DA4C1 with &cvars[0x6C0].
constexpr uintptr_t kRvaGameCVarsSlot = 0x2529940;
constexpr size_t kCVarNetSerialiseMaxSpeed = 0x6C0;

// CPlayer fields: +0x18 m_pEntity (game.h), +0x1270 m_pPlayerInput (NetSerialize_InputClient 0x3860F23D,
// UpdateRemotePlayersInterpolation 0x38672A69), +0x1180 m_lastRequestedVelocity (0x386729A2).
constexpr size_t kPlayerInputOffset = 0x1270;
constexpr size_t kLastRequestedVelocityOffset = 0x1180;
constexpr size_t kVtEntityGetId = 0x8; // IEntity: [0] dtor, [1] GetId, [2] GetEntityGUID, [3] GetClass (game.h 0x18)

// SSerializedPlayerInput (IPlayerInput.h; Serialize 0x38615440): +0 stance u8 'stnc', +4 deltaMovement 'dMov',
// +0x10 lookDirection, +0x28 sprint, +0x2D inAir, +0x34 position 'wrld' (0x3861F623), +0x40 physCounter 'ui2',
// +0x44 standingOn 'eid'.
constexpr size_t kSpiStance = 0x0;
constexpr size_t kSpiDeltaMovement = 0x4;

// CNetPlayerInput: +8 m_pPlayer, +0x10 m_curInput (deltaMovement +0x14, inAir +0x3D, physCounter +0x50),
// +0x58 m_lastUpdate, +0x60 m_desiredVelocity, +0x78 m_newInterpolation, +0x80 m_lerper, +0xE0 m_lerpVel.
constexpr size_t kNpiPlayer = 0x8;
constexpr size_t kNpiDeltaMovement = 0x14;
constexpr size_t kNpiDesiredVelocity = 0x60;

// EStance (Lua globals registered at 0x377BECBB..0x377BED66): STAND 0, CROUCH 1, PRONE 2, RELAXED 3, STEALTH 4,
// LOW_COVER 5, ALERTED 6, HIGH_COVER 7, SWIM 8 (ZEROG 9). Shipped 'stnc' = RangedInt 0..6 (clamps, StationaryInteger).
constexpr unsigned char kStanceAlerted = 6;
constexpr unsigned char kStanceMaxWide = 9;

// 'dMov' X/Y = QuantizedVec3 -1..1, 5 bits, TruncateLeft (CompressionPolicy.xml): q = floor((v+1)*16), step 1/16.
constexpr float kDMovHalfStep = 1.0f / 32.0f;

// ------------------------------------------------------------------------------------------------ state

struct Vec3 {
    float x, y, z;
};

std::atomic<bool> g_fixAiInput{true};
std::atomic<bool> g_fixNetVel{true};
std::atomic<bool> g_fixStance{true};
std::atomic<bool> g_fixVelocity{true};
std::atomic<bool> g_fixPseudo{true};
std::atomic<bool> g_velFromNet{true}; // velsrc: network velocity (true) or smoothed lerper velocity
std::atomic<bool> g_trace{false};

std::atomic<float> g_enterSpeed{0.5f}; // m/s: start "moving"
std::atomic<float> g_exitSpeed{0.3f};  // m/s: below this for g_exitHold seconds -> "standing"
std::atomic<float> g_exitHold{0.2f};
std::atomic<float> g_tau{0.1f};        // s: smoothing time constant
std::atomic<float> g_deadzone{0.25f};  // m/s: sender zeroes slower horizontal velocities

std::atomic<long> g_aiNormalized{0};
std::atomic<long> g_netZeroed{0};
std::atomic<long> g_stanceClamped{0};
std::atomic<long> g_stanceWide{0};

bool g_hooked[6];
const char* const kHookNames[6] = {"CAIInput::GetState", "CPlayerInput::GetState", "FinalizeMovementRequest",
                                   "CNetPlayerInput::UpdateMoveRequest", "CNetPlayerInput::SetState",
                                   "CSkeletonAnim::AnimationToQueue"};

struct ActorState {
    uint32_t entityId = 0;
    LARGE_INTEGER last{};
    Vec3 smooth{};       // smoothed horizontal velocity (source per velsrc)
    Vec3 dir{0.f, 1.f, 0.f}; // last horizontal direction of travel
    Vec3 anim{};         // velocity handed to animation (zero while standing)
    bool moving = false; // hysteresis state
    float below = 0.f;   // time spent below the exit speed
    bool rawMovingPrev = false, outMovingPrev = false;
    int rawFlips = 0, outFlips = 0, frames = 0; // since the last report
    float netSpeed = 0.f, lerpSpeed = 0.f;
};

struct QueueStats {
    std::string model;
    int queued = 0, overflows = 0, nearFull = 0; // since the last report
    long totalOverflows = 0;
    LARGE_INTEGER last{};
};

std::mutex g_mtx;
std::unordered_map<const void*, ActorState> g_actors;  // key: CPlayer*
std::unordered_map<const void*, QueueStats> g_queues;  // key: CSkeletonAnim*
LARGE_INTEGER g_freq{}, g_lastReport{};
int g_traceLinesThisSecond = 0;

bool Multiplayer() {
    return *game::At<unsigned char>(game::kRvaMultiplayerFlag) != 0;
}

float MaxSpeed() {
    auto cvars = *game::At<char*>(kRvaGameCVarsSlot);
    float v = cvars ? *reinterpret_cast<float*>(cvars + kCVarNetSerialiseMaxSpeed) : 9.f;
    return (v > 0.5f && v < 100.f) ? v : 9.f;
}

double Seconds(const LARGE_INTEGER& from, const LARGE_INTEGER& to) {
    return double(to.QuadPart - from.QuadPart) / double(g_freq.QuadPart);
}

float Len2D(const Vec3& v) {
    return std::sqrt(v.x * v.x + v.y * v.y);
}

bool IsNetPlayerInput(const void* input) {
    return input && *reinterpret_cast<const uintptr_t*>(input) == game::Base() + kRvaNetPlayerInputVtbl;
}

// ------------------------------------------------------------------------------------------------ sender side

// Called on the serialized input of locally simulated actors (server AI, own player), before Serialize quantizes it.
void PrepareOutgoing(unsigned char* input, bool ai) {
    auto& stance = input[kSpiStance];
    if (stance > kStanceAlerted) {
        if (stance > kStanceMaxWide) {
            stance = kStanceAlerted; // STANCE_NULL (0xFF): what the original 0..6 clamp produced
            g_stanceClamped.fetch_add(1, std::memory_order_relaxed);
        } else if (!g_fixStance) {
            stance = kStanceAlerted; // old behaviour for HIGH_COVER/SWIM/ZEROG
            g_stanceClamped.fetch_add(1, std::memory_order_relaxed);
        } else {
            g_stanceWide.fetch_add(1, std::memory_order_relaxed);
        }
    }

    auto dm = reinterpret_cast<Vec3*>(input + kSpiDeltaMovement);
    const float maxSpeed = MaxSpeed();
    if (ai && g_fixAiInput) {
        // CNetPlayerInput::UpdateInterpolation multiplies by pl_netSerialiseMaxSpeed (0x385EE5C2..0x385EE5DF)
        const float inv = 1.f / maxSpeed;
        dm->x *= inv;
        dm->y *= inv;
        dm->z *= inv;
        g_aiNormalized.fetch_add(1, std::memory_order_relaxed);
    }
    if (g_fixNetVel) {
        if (Len2D(*dm) * maxSpeed < g_deadzone.load()) {
            if (dm->x != 0.f || dm->y != 0.f)
                g_netZeroed.fetch_add(1, std::memory_order_relaxed);
            dm->x = dm->y = 0.f;
        }
        // floor -> round to nearest; exact zero stays in bucket 16 (= 0.0)
        dm->x += kDMovHalfStep;
        dm->y += kDMovHalfStep;
    }
}

using GetStateFn = void(__fastcall*)(void* self, unsigned char* input);
GetStateFn g_origAIGetState;
GetStateFn g_origPlayerGetState;

void __fastcall HookedAIGetState(void* self, unsigned char* input) {
    g_origAIGetState(self, input);
    if (Multiplayer())
        PrepareOutgoing(input, true);
}

void __fastcall HookedPlayerGetState(void* self, unsigned char* input) {
    g_origPlayerGetState(self, input);
    if (Multiplayer())
        PrepareOutgoing(input, false);
}

// ------------------------------------------------------------------------------------------------ receiver side

uint32_t EntityIdOf(const char* player) {
    auto entity = *reinterpret_cast<void* const*>(player + game::kActorEntityOffset);
    return entity ? game::Virtual<uint32_t(__fastcall*)(void*)>(entity, kVtEntityGetId)(entity) : 0;
}

void ReportLocked(const LARGE_INTEGER& now, bool force);

using FinalizeFn = void(__fastcall*)(char* player, const void* movement, void* request);
FinalizeFn g_origFinalize;

void __fastcall HookedFinalize(char* player, const void* movement, void* request) {
    g_origFinalize(player, movement, request);
    if (!Multiplayer())
        return;
    auto input = *reinterpret_cast<char**>(player + kPlayerInputOffset);
    if (!IsNetPlayerInput(input))
        return; // locally simulated actor

    auto lastVel = reinterpret_cast<Vec3*>(player + kLastRequestedVelocityOffset);
    const Vec3 raw = *lastVel; // xy = lerper velocity (UpdateRemotePlayersInterpolation)
    const float maxSpeed = MaxSpeed();
    const auto dm = reinterpret_cast<const Vec3*>(input + kNpiDeltaMovement);
    const Vec3 net{dm->x * maxSpeed, dm->y * maxSpeed, 0.f};

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    std::lock_guard lock(g_mtx);
    auto& s = g_actors[player];
    float dt = s.last.QuadPart ? float(Seconds(s.last, now)) : 0.f;
    if (dt < 0.f || dt > 1.f) { // new or reused CPlayer
        s = ActorState{};
        dt = 0.f;
    }
    if (!s.entityId)
        s.entityId = EntityIdOf(player);
    s.last = now;

    const Vec3 src = g_velFromNet ? net : Vec3{raw.x, raw.y, 0.f};
    const float tau = (std::max)(g_tau.load(), 0.001f);
    const float a = s.frames == 0 && dt == 0.f ? 1.f : 1.f - std::exp(-(std::min)(dt, 0.25f) / tau);
    s.smooth.x += (src.x - s.smooth.x) * a;
    s.smooth.y += (src.y - s.smooth.y) * a;
    const float speed = Len2D(s.smooth);
    if (!s.moving) {
        if (speed >= g_enterSpeed.load()) {
            s.moving = true;
            s.below = 0.f;
        }
    } else if (speed < g_exitSpeed.load()) {
        s.below += dt;
        if (s.below >= g_exitHold.load())
            s.moving = false;
    } else {
        s.below = 0.f;
    }
    if (speed > 0.05f)
        s.dir = {s.smooth.x / speed, s.smooth.y / speed, 0.f};
    if (s.moving) {
        // keep IsMoving() (|v|^2 > 0.01) true while the hysteresis says moving, in the last known direction
        const float mag = (std::max)(speed, 0.15f);
        s.anim = {s.dir.x * mag, s.dir.y * mag, 0.f};
    } else {
        s.anim = {0.f, 0.f, 0.f};
    }

    if (g_fixVelocity) {
        lastVel->x = s.anim.x;
        lastVel->y = s.anim.y;
    }

    const bool rawMoving = raw.x * raw.x + raw.y * raw.y + raw.z * raw.z > 0.01f;
    const bool outMoving = lastVel->x * lastVel->x + lastVel->y * lastVel->y + lastVel->z * lastVel->z > 0.01f;
    if (s.frames > 0) {
        s.rawFlips += rawMoving != s.rawMovingPrev;
        s.outFlips += outMoving != s.outMovingPrev;
    }
    s.rawMovingPrev = rawMoving;
    s.outMovingPrev = outMoving;
    s.frames++;
    s.netSpeed = Len2D(net);
    s.lerpSpeed = Len2D(raw);

    if (g_trace)
        ReportLocked(now, false);
}

// Runs fn with CNetPlayerInput::m_desiredVelocity replaced by the animation velocity (pseudo speed only; the
// physics request reads +0x60 later in FinalizeMovementRequest and gets the restored lerper velocity).
template <typename Fn>
void WithAnimVelocity(char* self, Fn&& fn) {
    Vec3 anim;
    bool have = false;
    if (g_fixPseudo && Multiplayer()) {
        auto player = *reinterpret_cast<char**>(self + kNpiPlayer);
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        std::lock_guard lock(g_mtx);
        auto it = g_actors.find(player);
        // only while FinalizeMovementRequest keeps the state fresh (not dead / in a vehicle / animated slave)
        if (it != g_actors.end() && it->second.frames > 0 && Seconds(it->second.last, now) < 0.5) {
            anim = it->second.anim;
            have = true;
        }
    }
    if (!have) {
        fn();
        return;
    }
    auto dv = reinterpret_cast<Vec3*>(self + kNpiDesiredVelocity);
    const Vec3 saved = *dv;
    *dv = {anim.x, anim.y, saved.z};
    fn();
    *dv = saved;
}

using UpdateMoveRequestFn = void(__fastcall*)(char* self);
UpdateMoveRequestFn g_origUpdateMoveRequest;

void __fastcall HookedUpdateMoveRequest(char* self) {
    WithAnimVelocity(self, [&] { g_origUpdateMoveRequest(self); });
}

using SetStateFn = void(__fastcall*)(char* self, const void* input);
SetStateFn g_origNetSetState;

void __fastcall HookedNetSetState(char* self, const void* input) {
    WithAnimVelocity(self, [&] { g_origNetSetState(self, input); });
}

// ------------------------------------------------------------------------------------------------ diagnostics

using AnimToQueueFn = uint32_t(__fastcall*)(char* self, const void* anim, int animId, float btime, const char* params);
AnimToQueueFn g_origAnimToQueue;

unsigned QueueSize(const char* skelAnim, int layer) {
    auto arr = *reinterpret_cast<const char* const*>(skelAnim + kSkelAnimQueues + layer * kSkelAnimQueueStride);
    return arr ? (*reinterpret_cast<const uint32_t*>(arr - 4) & 0x7FFFFFFFu) : 0;
}

std::string ModelPath(const char* skelAnim) {
    auto inst = *reinterpret_cast<char* const*>(skelAnim + kSkelAnimInstance);
    auto skel = inst ? *reinterpret_cast<void* const*>(inst + kCharInstSkeleton) : nullptr;
    auto path = skel ? game::Virtual<const char*(__fastcall*)(void*)>(skel, kVtSkeletonGetModelFilePath)(skel) : nullptr;
    return path ? path : "?";
}

uint32_t __fastcall HookedAnimToQueue(char* self, const void* anim, int animId, float btime, const char* params) {
    const int layer = *reinterpret_cast<const int*>(params + 0x10);
    const bool validLayer = layer >= 0 && layer < kNumVirtualLayers;
    const unsigned before = validLayer ? QueueSize(self, layer) : 0;
    const uint32_t r = g_origAnimToQueue(self, anim, animId, btime, params);
    if (!validLayer)
        return r;
    if (before + 4 < kMaxAnimsInQueue && !g_trace)
        return r; // fast path: only track characters whose queue fills up (and everything while tracing)

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    std::lock_guard lock(g_mtx);
    auto& q = g_queues[self];
    if (q.model.empty() || Seconds(q.last, now) > 30.0)
        q.model = ModelPath(self);
    q.last = now;
    q.queued++;
    if (before + 4 >= kMaxAnimsInQueue)
        q.nearFull++;
    if (before >= kMaxAnimsInQueue) {
        q.overflows++;
        q.totalOverflows++;
    }
    if (g_trace && before + 4 >= kMaxAnimsInQueue && g_traceLinesThisSecond < 40) {
        g_traceLinesThisSecond++;
        auto inst = *reinterpret_cast<void* const*>(self + kSkelAnimInstance);
        c3coop::Logf("anim: queue %u/%u CharInst %p layer %d animId %d trans %.2f flags %08X -> %u (%s)", before,
                     kMaxAnimsInQueue, inst, layer, animId, *reinterpret_cast<const float*>(params),
                     *reinterpret_cast<const uint32_t*>(params + 0x18), r, q.model.c_str());
    }
    return r;
}

// Per-second (trace) or on-demand (status) summary; caller holds g_mtx.
void ReportLocked(const LARGE_INTEGER& now, bool force) {
    const double window = g_lastReport.QuadPart ? Seconds(g_lastReport, now) : 0.0;
    if (!force && window < 1.0)
        return;
    g_lastReport = now;
    g_traceLinesThisSecond = 0;
    for (auto it = g_actors.begin(); it != g_actors.end();) {
        auto& s = it->second;
        if (Seconds(s.last, now) > 5.0) {
            it = g_actors.erase(it);
            continue;
        }
        if (s.frames)
            c3coop::Logf("anim: actor %u (%p): %d frames in %.1fs, IsMoving flips raw %d -> out %d, now %s, speed "
                         "net %.2f lerp %.2f anim %.2f",
                         s.entityId, it->first, s.frames, window, s.rawFlips, s.outFlips,
                         s.moving ? "moving" : "standing", s.netSpeed, s.lerpSpeed, Len2D(s.anim));
        s.rawFlips = s.outFlips = 0;
        s.frames = s.frames ? 1 : 0;
        ++it;
    }
    for (auto it = g_queues.begin(); it != g_queues.end();) {
        auto& q = it->second;
        if (Seconds(q.last, now) > 30.0) {
            it = g_queues.erase(it);
            continue;
        }
        if (q.queued || force)
            c3coop::Logf("anim: queue CharInst %p %s: %d queued while >=12 deep, %d overflows (total %ld) in %.1fs",
                         *reinterpret_cast<void* const*>(static_cast<const char*>(it->first) + kSkelAnimInstance),
                         q.model.c_str(), q.nearFull, q.overflows, q.totalOverflows, window);
        q.queued = q.overflows = q.nearFull = 0;
        ++it;
    }
}

void Status() {
    c3coop::Logf("anim: bMultiplayer=%d bServer=%d pl_netSerialiseMaxSpeed=%.2f", Multiplayer() ? 1 : 0,
                 *game::At<unsigned char>(game::kRvaServerFlag), MaxSpeed());
    c3coop::Logf("anim: fixes aiinput=%d netvel=%d stance=%d (sender) | velocity=%d pseudo=%d velsrc=%s (receiver)",
                 g_fixAiInput.load(), g_fixNetVel.load(), g_fixStance.load(), g_fixVelocity.load(), g_fixPseudo.load(),
                 g_velFromNet ? "net" : "lerp");
    c3coop::Logf("anim: tune enter=%.2f exit=%.2f hold=%.2f tau=%.2f deadzone=%.2f, trace=%d", g_enterSpeed.load(),
                 g_exitSpeed.load(), g_exitHold.load(), g_tau.load(), g_deadzone.load(), g_trace.load());
    c3coop::Logf("anim: counters ai normalized %ld, net zeroed %ld, stance 7..9 sent %ld, stance clamped %ld",
                 g_aiNormalized.load(), g_netZeroed.load(), g_stanceWide.load(), g_stanceClamped.load());
    std::string hooks;
    for (int i = 0; i < 6; ++i)
        hooks += std::string(kHookNames[i]) + (g_hooked[i] ? "=ok " : "=MISSING ");
    c3coop::Logf("anim: hooks %s", hooks.c_str());
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    std::lock_guard lock(g_mtx);
    c3coop::Logf("anim: %zu network-driven actors, %zu characters with a nearly full queue", g_actors.size(),
                 g_queues.size());
    ReportLocked(now, true);
}

template <typename Fn>
bool Hook(int index, uintptr_t rva, const unsigned char* sig, size_t sigLen, void* detour, Fn* original) {
    auto target = game::At<unsigned char>(rva);
    if (memcmp(target, sig, sigLen) != 0) {
        c3coop::Logf("anim: %s NOT hooked at %p (bytes differ)", kHookNames[index], target);
        return false;
    }
    if (MH_CreateHook(target, detour, reinterpret_cast<void**>(original)) != MH_OK || MH_EnableHook(target) != MH_OK) {
        c3coop::Logf("anim: %s NOT hooked at %p (MinHook failed)", kHookNames[index], target);
        return false;
    }
    g_hooked[index] = true;
    c3coop::Logf("anim: %s hooked at %p", kHookNames[index], target);
    return true;
}

void PutAbsJmp(unsigned char* at, uintptr_t target) {
    // jmp qword [rip+0]; dq target
    const unsigned char op[] = {0xFF, 0x25, 0x00, 0x00, 0x00, 0x00};
    memcpy(at, op, sizeof(op));
    memcpy(at + sizeof(op), &target, sizeof(target));
}

// Hand-made detour for a function whose first `len` (>= 14) bytes are `sig`: whole instructions, position independent.
template <typename Fn>
bool RawHook(int index, uintptr_t rva, const unsigned char* sig, size_t len, void* detour, Fn* original) {
    auto target = game::At<unsigned char>(rva);
    if (len < 14 || memcmp(target, sig, len) != 0) {
        c3coop::Logf("anim: %s NOT hooked at %p (bytes differ)", kHookNames[index], target);
        return false;
    }
    auto tramp = static_cast<unsigned char*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!tramp)
        return false;
    memcpy(tramp, target, len);
    PutAbsJmp(tramp + len, reinterpret_cast<uintptr_t>(target + len));
    *original = reinterpret_cast<Fn>(tramp);

    unsigned char patch[32];
    memset(patch, 0x90, len);
    PutAbsJmp(patch, reinterpret_cast<uintptr_t>(detour));
    DWORD old;
    VirtualProtect(target, len, PAGE_EXECUTE_READWRITE, &old);
    memcpy(target, patch, len);
    VirtualProtect(target, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, len);
    g_hooked[index] = true;
    c3coop::Logf("anim: %s hooked at %p (manual detour, trampoline %p)", kHookNames[index], target, tramp);
    return true;
}

bool SetToggle(const std::string& name, bool on) {
    if (name == "aiinput") g_fixAiInput = on;
    else if (name == "netvel") g_fixNetVel = on;
    else if (name == "stance") g_fixStance = on;
    else if (name == "velocity") g_fixVelocity = on;
    else if (name == "pseudo") g_fixPseudo = on;
    else if (name == "trace") g_trace = on;
    else if (name == "all") g_fixAiInput = g_fixNetVel = g_fixStance = g_fixVelocity = g_fixPseudo = on;
    else return false;
    return true;
}

} // namespace

namespace c3coop {

void InstallAnimHooks() {
    QueryPerformanceFrequency(&g_freq);

    // the CNetPlayerInput vtable constant is used to recognise network-driven actors
    auto vtbl = game::At<uintptr_t>(kRvaNetPlayerInputVtbl);
    if (vtbl[1] != game::Base() + kRvaNetInputPreUpdate) {
        Logf("anim: CNetPlayerInput vtable mismatch (%llX), receiver fixes not installed", (unsigned long long)vtbl[1]);
    } else {
        Hook(2, kRvaFinalizeMovementRequest, kFinalizeMovementRequestBytes, sizeof(kFinalizeMovementRequestBytes),
             &HookedFinalize, &g_origFinalize);
        Hook(3, kRvaNetInputUpdateMoveRequest, kNetInputUpdateMoveRequestBytes, sizeof(kNetInputUpdateMoveRequestBytes),
             &HookedUpdateMoveRequest, &g_origUpdateMoveRequest);
        Hook(4, kRvaNetInputSetState, kNetInputSetStateBytes, sizeof(kNetInputSetStateBytes), &HookedNetSetState,
             &g_origNetSetState);
    }
    Hook(0, kRvaAIInputGetState, kAIInputGetStateBytes, sizeof(kAIInputGetStateBytes), &HookedAIGetState,
         &g_origAIGetState);
    Hook(1, kRvaPlayerInputGetState, kPlayerInputGetStateBytes, sizeof(kPlayerInputGetStateBytes),
         &HookedPlayerGetState, &g_origPlayerGetState);
    RawHook(5, kRvaAnimationToQueue, kAnimationToQueueBytes, sizeof(kAnimationToQueueBytes), &HookedAnimToQueue,
            &g_origAnimToQueue);
}

bool AnimCommand(const std::string& cmd) {
    if (cmd != "!anim" && cmd.rfind("!anim ", 0) != 0)
        return false;
    std::istringstream in(cmd.substr(5));
    std::vector<std::string> args;
    for (std::string a; in >> a;)
        args.push_back(a);

    if (args.empty() || args[0] == "status") {
        Status();
    } else if (args[0] == "velsrc" && args.size() == 2 && (args[1] == "net" || args[1] == "lerp")) {
        g_velFromNet = args[1] == "net";
        Logf("anim: animation velocity source = %s", args[1].c_str());
    } else if (args[0] == "tune" && args.size() >= 2) {
        std::atomic<float>* vars[] = {&g_enterSpeed, &g_exitSpeed, &g_exitHold, &g_tau, &g_deadzone};
        for (size_t i = 1; i < args.size() && i <= 5; ++i)
            *vars[i - 1] = static_cast<float>(atof(args[i].c_str()));
        Logf("anim: tune enter=%.2f exit=%.2f hold=%.2f tau=%.2f deadzone=%.2f", g_enterSpeed.load(),
             g_exitSpeed.load(), g_exitHold.load(), g_tau.load(), g_deadzone.load());
    } else if (args.size() == 2 && (args[1] == "on" || args[1] == "off") && SetToggle(args[0], args[1] == "on")) {
        Logf("anim: %s %s", args[0].c_str(), args[1].c_str());
    } else {
        Logf("anim: usage: !anim status | !anim aiinput|netvel|stance|velocity|pseudo|all|trace on|off | "
             "!anim velsrc net|lerp | !anim tune <enter m/s> <exit m/s> <hold s> <tau s> <deadzone m/s>");
    }
    return true;
}

} // namespace c3coop
