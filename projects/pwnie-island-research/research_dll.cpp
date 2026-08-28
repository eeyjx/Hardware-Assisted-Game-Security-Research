#include "shared_state.hpp"

#include <windows.h>
#include <MinHook.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>

#if defined(__GNUC__) && defined(__i386__)
    #define PWNIE_THISCALL __attribute__((thiscall))
    #define PWNIE_FASTCALL __attribute__((fastcall))
#else
    #define PWNIE_THISCALL __thiscall
    #define PWNIE_FASTCALL __fastcall
#endif

namespace {

struct Vector3 {
    float x;
    float y;
    float z;
};

struct MovementValues {
    float walking_speed;
    float jump_speed;
    float jump_hold_time;
};

static_assert(sizeof(Vector3) == 12, "Vector3 layout must match GameLogic");
static_assert(
    sizeof(MovementValues) == 12,
    "movement fields must remain contiguous");

using ActorGetPositionFn =
    Vector3* (PWNIE_THISCALL*)(void* actor, Vector3* output);
using ActorSetPositionFn =
    void (PWNIE_THISCALL*)(void* actor, const Vector3* position);
using GetSprintMultiplierFn =
    float (PWNIE_THISCALL*)(void* iplayer);
using IsLocalPlayerFn =
    bool (PWNIE_THISCALL*)(void* iplayer);
using PlayerTickFn =
    void (PWNIE_THISCALL*)(void* player, float delta_time);

HMODULE g_self = nullptr;
HANDLE g_mapping = nullptr;
pwnie::SharedState* g_state = nullptr;

ActorGetPositionFn g_actor_get_position = nullptr;
ActorSetPositionFn g_actor_set_position = nullptr;
GetSprintMultiplierFn g_original_get_sprint = nullptr;
IsLocalPlayerFn g_is_local_player = nullptr;
PlayerTickFn g_original_player_tick = nullptr;

void* g_tick_target = nullptr;
void* g_sprint_target = nullptr;
BYTE* g_can_jump_target = nullptr;

constexpr SIZE_T kCanJumpPatchSize = 5;
constexpr BYTE kExpectedCanJump[kCanJumpPatchSize] = {
    0x8B, 0x49, 0x9C, 0x85, 0xC9
};
constexpr BYTE kEnabledCanJump[kCanJumpPatchSize] = {
    0xB0, 0x01, 0xC3, 0x90, 0x90
};
BYTE g_original_can_jump[kCanJumpPatchSize]{};
bool g_original_can_jump_saved = false;
bool g_can_jump_patch_active = false;

CRITICAL_SECTION g_fly_lock{};
bool g_fly_lock_initialized = false;
void* g_movement_snapshot_player = nullptr;
MovementValues g_movement_snapshot{};
bool g_movement_snapshot_valid = false;

volatile LONG g_active_hook_calls = 0;

class HookActivityGuard {
public:
    HookActivityGuard() {
        InterlockedIncrement(&g_active_hook_calls);
    }

    ~HookActivityGuard() {
        InterlockedDecrement(&g_active_hook_calls);
    }

    HookActivityGuard(const HookActivityGuard&) = delete;
    HookActivityGuard& operator=(const HookActivityGuard&) = delete;
};

[[nodiscard]] LONG AtomicRead(volatile LONG* value) {
    return InterlockedCompareExchange(value, 0, 0);
}

[[nodiscard]] bool CheckedAdd(
    std::uintptr_t base,
    std::uint32_t offset,
    std::uintptr_t& result) {
    if (offset >
        (std::numeric_limits<std::uintptr_t>::max)() - base) {
        return false;
    }

    result = base + offset;
    return true;
}

[[nodiscard]] bool ReadBytes(
    std::uintptr_t address,
    void* output,
    SIZE_T size) {
    SIZE_T bytes_read = 0;
    return ReadProcessMemory(
               GetCurrentProcess(),
               reinterpret_cast<LPCVOID>(address),
               output,
               size,
               &bytes_read) != FALSE &&
           bytes_read == size;
}

template <typename T>
[[nodiscard]] bool ReadLocal(
    std::uintptr_t address,
    T& output) {
    return ReadBytes(address, &output, sizeof(T));
}

template <typename T>
[[nodiscard]] bool WriteLocal(
    std::uintptr_t address,
    const T& value) {
    SIZE_T bytes_written = 0;
    return WriteProcessMemory(
               GetCurrentProcess(),
               reinterpret_cast<LPVOID>(address),
               &value,
               sizeof(T),
               &bytes_written) != FALSE &&
           bytes_written == sizeof(T);
}

template <SIZE_T N>
[[nodiscard]] bool MatchBytes(
    std::uintptr_t address,
    const BYTE (&expected)[N]) {
    BYTE actual[N]{};
    return ReadBytes(address, actual, N) &&
           std::memcmp(actual, expected, N) == 0;
}

[[nodiscard]] bool WriteExecutableBytes(
    BYTE* address,
    const BYTE* bytes,
    SIZE_T size) {
    if (address == nullptr || bytes == nullptr || size == 0) {
        return false;
    }

    DWORD old_protection = 0;
    if (!VirtualProtect(
            address,
            size,
            PAGE_EXECUTE_READWRITE,
            &old_protection)) {
        return false;
    }

    std::memcpy(address, bytes, size);
    FlushInstructionCache(GetCurrentProcess(), address, size);

    for (int attempt = 0; attempt < 5; ++attempt) {
        DWORD ignored = 0;
        if (VirtualProtect(
                address,
                size,
                old_protection,
                &ignored)) {
            return true;
        }
        Sleep(0);
    }
    return false;
}

[[nodiscard]] LONG VerifyRuntimeSignatures(
    std::uintptr_t game_logic_base) {
    constexpr BYTE actor_get_position[] = {
        0x55, 0x8B, 0xEC, 0x8B, 0x49, 0x0C
    };
    constexpr BYTE actor_set_position[] = {
        0x55, 0x8B, 0xEC, 0x8B, 0x55, 0x08
    };
    constexpr BYTE is_local_player[] = {
        0x33, 0xC0, 0x39, 0x81, 0x48, 0x01
    };
    constexpr BYTE player_tick[] = {
        0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xC0
    };

    if (!MatchBytes(
            game_logic_base + pwnie::kActorGetPositionRva,
            actor_get_position)) {
        return pwnie::kInitErrorActorGetPositionSignature;
    }

    if (!MatchBytes(
            game_logic_base + pwnie::kActorSetPositionRva,
            actor_set_position)) {
        return pwnie::kInitErrorActorSetPositionSignature;
    }

    if (!MatchBytes(
            game_logic_base + pwnie::kIsLocalPlayerRva,
            is_local_player)) {
        return pwnie::kInitErrorIsLocalPlayerSignature;
    }

    if (!MatchBytes(
            game_logic_base + pwnie::kPlayerTickRva,
            player_tick)) {
        return pwnie::kInitErrorPlayerTickSignature;
    }

    if (!MatchBytes(
            game_logic_base + pwnie::kPlayerCanJumpRva,
            kExpectedCanJump)) {
        return pwnie::kInitErrorPlayerCanJumpSignature;
    }

    BYTE sprint_bytes[7]{};
    std::uint32_t sprint_operand = 0;
    const auto sprint_address =
        game_logic_base + pwnie::kGetSprintMultiplierRva;
    if (!ReadBytes(
            sprint_address,
            sprint_bytes,
            sizeof(sprint_bytes)) ||
        sprint_bytes[0] != 0xD9 ||
        sprint_bytes[1] != 0x05 ||
        sprint_bytes[6] != 0xC3 ||
        !ReadLocal(sprint_address + 2, sprint_operand) ||
        sprint_operand != static_cast<std::uint32_t>(
            game_logic_base + pwnie::kSprintMultiplierDataRva)) {
        return pwnie::kInitErrorGetSprintSignature;
    }

    if (!ReadBytes(
            game_logic_base + pwnie::kPlayerCanJumpRva,
            g_original_can_jump,
            sizeof(g_original_can_jump))) {
        return pwnie::kInitErrorPlayerCanJumpSignature;
    }

    g_original_can_jump_saved = true;
    return pwnie::kInitErrorNone;
}

[[nodiscard]] bool InitializeSharedState() {
    std::wstring mapping_name = pwnie::kSharedMappingPrefix;
    mapping_name += std::to_wstring(GetCurrentProcessId());

    g_mapping = CreateFileMappingW(
        INVALID_HANDLE_VALUE,
        nullptr,
        PAGE_READWRITE,
        0,
        sizeof(pwnie::SharedState),
        mapping_name.c_str());

    if (g_mapping == nullptr) {
        return false;
    }

    g_state = static_cast<pwnie::SharedState*>(
        MapViewOfFile(
            g_mapping,
            FILE_MAP_ALL_ACCESS,
            0,
            0,
            sizeof(pwnie::SharedState)));

    if (g_state == nullptr) {
        CloseHandle(g_mapping);
        g_mapping = nullptr;
        return false;
    }

    std::memset(g_state, 0, sizeof(*g_state));
    g_state->magic = pwnie::kSharedMagic;
    g_state->version = pwnie::kSharedVersion;
    g_state->owner_process_id = GetCurrentProcessId();
    g_state->capture_player_requested = 1;
    g_state->sprint_override_value = 3.0F;
    g_state->fly_walking_speed = pwnie::kDefaultFlyWalkingSpeed;
    g_state->fly_jump_speed = pwnie::kDefaultFlyJumpSpeed;
    g_state->fly_jump_hold_time = pwnie::kDefaultFlyJumpHoldTime;

    MemoryBarrier();
    return true;
}

void CleanupSharedState() {
    if (g_state != nullptr) {
        UnmapViewOfFile(g_state);
        g_state = nullptr;
    }

    if (g_mapping != nullptr) {
        CloseHandle(g_mapping);
        g_mapping = nullptr;
    }
}

[[noreturn]] void ExitWorker(DWORD code) {
    CleanupSharedState();
    FreeLibraryAndExitThread(g_self, code);
}

[[noreturn]] void FailAndExit(LONG error, DWORD code) {
    if (g_state != nullptr) {
        InterlockedExchange(&g_state->init_error, error);
        InterlockedExchange(&g_state->hook_ready, -1);
        MemoryBarrier();

        // Leave the diagnostic mapping observable for the injector handshake.
        Sleep(2500);
    }

    ExitWorker(code);
}

[[nodiscard]] bool IsCompleteLocalPlayer(void* player) {
    if (player == nullptr || g_is_local_player == nullptr) {
        return false;
    }

    std::uintptr_t iplayer_address = 0;
    if (!CheckedAdd(
            reinterpret_cast<std::uintptr_t>(player),
            pwnie::kIPlayerSubobjectOffset,
            iplayer_address)) {
        return false;
    }

    return g_is_local_player(
        reinterpret_cast<void*>(iplayer_address));
}

void PublishPlayer(void* player) {
    if (g_state == nullptr || player == nullptr) {
        return;
    }

    InterlockedExchange(
        &g_state->capture_player_requested,
        0);
    InterlockedExchange(
        reinterpret_cast<volatile LONG*>(
            &g_state->player_address),
        static_cast<LONG>(
            reinterpret_cast<std::uintptr_t>(player)));
}

void PublishHealth(void* player) {
    if (g_state == nullptr || player == nullptr) {
        return;
    }

    std::uintptr_t address = 0;
    std::int32_t health = 0;
    if (CheckedAdd(
            reinterpret_cast<std::uintptr_t>(player),
            pwnie::kPlayerHealthOffset,
            address) &&
        ReadLocal(address, health)) {
        InterlockedExchange(&g_state->health_valid, 0);
        g_state->health = health;
        MemoryBarrier();
        InterlockedExchange(&g_state->health_valid, 1);
    } else {
        InterlockedExchange(&g_state->health_valid, 0);
    }
}

bool PublishPosition(void* player) {
    if (g_state == nullptr ||
        player == nullptr ||
        g_actor_get_position == nullptr) {
        if (g_state != nullptr) {
            InterlockedExchange(&g_state->coordinates_valid, 0);
        }
        return false;
    }

    Vector3 position{};
    Vector3* result = g_actor_get_position(player, &position);
    if (result != nullptr &&
        std::isfinite(position.x) &&
        std::isfinite(position.y) &&
        std::isfinite(position.z) &&
        (position.x != 0.0F ||
         position.y != 0.0F ||
         position.z != 0.0F)) {
        InterlockedExchange(&g_state->coordinates_valid, 0);
        InterlockedIncrement(&g_state->position_sequence);
        g_state->coordinate_x = position.x;
        g_state->coordinate_y = position.y;
        g_state->coordinate_z = position.z;
        MemoryBarrier();
        InterlockedIncrement(&g_state->position_sequence);
        InterlockedIncrement(&g_state->position_update_count);
        InterlockedExchange(&g_state->coordinates_valid, 1);
        return true;
    } else {
        InterlockedExchange(&g_state->coordinates_valid, 0);
        return false;
    }
}

[[nodiscard]] bool ReadMovementValues(
    void* player,
    MovementValues& values) {
    if (player == nullptr) {
        return false;
    }

    std::uintptr_t address = 0;
    return CheckedAdd(
               reinterpret_cast<std::uintptr_t>(player),
               pwnie::kPlayerWalkingSpeedOffset,
               address) &&
           ReadLocal(address, values) &&
           std::isfinite(values.walking_speed) &&
           std::isfinite(values.jump_speed) &&
           std::isfinite(values.jump_hold_time);
}

[[nodiscard]] bool WriteMovementValues(
    void* player,
    const MovementValues& values) {
    if (player == nullptr) {
        return false;
    }

    std::uintptr_t address = 0;
    return CheckedAdd(
               reinterpret_cast<std::uintptr_t>(player),
               pwnie::kPlayerWalkingSpeedOffset,
               address) &&
           WriteLocal(address, values);
}

void PublishMovement(void* player) {
    if (g_state == nullptr) {
        return;
    }

    MovementValues values{};
    if (ReadMovementValues(player, values)) {
        InterlockedExchange(&g_state->movement_valid, 0);
        g_state->movement_walking_speed = values.walking_speed;
        g_state->movement_jump_speed = values.jump_speed;
        g_state->movement_jump_hold_time = values.jump_hold_time;
        MemoryBarrier();
        InterlockedExchange(&g_state->movement_valid, 1);
    } else {
        InterlockedExchange(&g_state->movement_valid, 0);
    }
}

void UpdateTelemetry(void* player) {
    PublishHealth(player);
    PublishPosition(player);
    PublishMovement(player);
}

void SetFlyError(LONG error) {
    if (g_state != nullptr) {
        InterlockedExchange(&g_state->fly_error, error);
    }
}

[[nodiscard]] bool SetCanJumpPatchLocked(bool enabled) {
    if (g_can_jump_target == nullptr ||
        !g_original_can_jump_saved) {
        SetFlyError(pwnie::kFlyErrorCanJumpBytes);
        return false;
    }

    if (enabled) {
        if (g_can_jump_patch_active) {
            return true;
        }

        BYTE current[kCanJumpPatchSize]{};
        if (!ReadBytes(
                reinterpret_cast<std::uintptr_t>(g_can_jump_target),
                current,
                sizeof(current)) ||
            std::memcmp(
                current,
                g_original_can_jump,
                sizeof(current)) != 0) {
            SetFlyError(pwnie::kFlyErrorCanJumpBytes);
            return false;
        }

        const bool protect_ok = WriteExecutableBytes(
            g_can_jump_target,
            kEnabledCanJump,
            sizeof(kEnabledCanJump));
        const bool bytes_ok = MatchBytes(
            reinterpret_cast<std::uintptr_t>(g_can_jump_target),
            kEnabledCanJump);
        g_can_jump_patch_active = bytes_ok;
        if (g_state != nullptr) {
            InterlockedExchange(
                &g_state->can_jump_patch_active,
                bytes_ok ? 1 : 0);
        }

        if (!protect_ok || !bytes_ok) {
            if (bytes_ok) {
                // Do not leave a global CanJump override behind when page
                // protection restoration failed. Mark it active so the
                // ordinary disable path can roll the bytes back and verify.
                g_can_jump_patch_active = true;
                if (g_state != nullptr) {
                    InterlockedExchange(
                        &g_state->can_jump_patch_active,
                        1);
                }
                const bool rolled_back =
                    SetCanJumpPatchLocked(false);
                (void)rolled_back;
            }
            SetFlyError(pwnie::kFlyErrorCanJumpProtect);
            return false;
        }

        return true;
    }

    if (!g_can_jump_patch_active) {
        if (g_state != nullptr) {
            InterlockedExchange(
                &g_state->can_jump_patch_active,
                0);
        }
        return true;
    }

    BYTE current[kCanJumpPatchSize]{};
    if (!ReadBytes(
            reinterpret_cast<std::uintptr_t>(g_can_jump_target),
            current,
            sizeof(current)) ||
        std::memcmp(
            current,
            kEnabledCanJump,
            sizeof(current)) != 0) {
        SetFlyError(pwnie::kFlyErrorCanJumpBytes);
        return false;
    }

    const bool protect_ok = WriteExecutableBytes(
        g_can_jump_target,
        g_original_can_jump,
        sizeof(g_original_can_jump));
    const bool bytes_ok = MatchBytes(
        reinterpret_cast<std::uintptr_t>(g_can_jump_target),
        g_original_can_jump);
    g_can_jump_patch_active = !bytes_ok;
    if (g_state != nullptr) {
        InterlockedExchange(
            &g_state->can_jump_patch_active,
            bytes_ok ? 0 : 1);
    }

    if (!protect_ok || !bytes_ok) {
        SetFlyError(pwnie::kFlyErrorCanJumpProtect);
        return false;
    }

    return true;
}

[[nodiscard]] bool RestoreMovementSnapshotLocked() {
    if (!g_movement_snapshot_valid) {
        if (g_state != nullptr) {
            InterlockedExchange(
                &g_state->movement_snapshot_valid,
                0);
        }
        return true;
    }

    if (!WriteMovementValues(
            g_movement_snapshot_player,
            g_movement_snapshot)) {
        SetFlyError(pwnie::kFlyErrorRestore);
        return false;
    }

    g_movement_snapshot_player = nullptr;
    g_movement_snapshot = MovementValues{};
    g_movement_snapshot_valid = false;
    if (g_state != nullptr) {
        InterlockedExchange(
            &g_state->movement_snapshot_valid,
            0);
        InterlockedIncrement(&g_state->fly_restore_count);
    }
    return true;
}

[[nodiscard]] bool SaveMovementSnapshotLocked(void* player) {
    if (g_movement_snapshot_valid &&
        g_movement_snapshot_player == player) {
        return true;
    }

    if (g_movement_snapshot_valid &&
        g_movement_snapshot_player != player) {
        // A Player object is not owned by this DLL.  Across a map transition
        // the old pointer may already be destroyed or reused, so never write
        // a saved snapshot through it from the new Player's Tick.  The active
        // local object's own Tick will receive a fresh snapshot below.
        g_movement_snapshot_player = nullptr;
        g_movement_snapshot = MovementValues{};
        g_movement_snapshot_valid = false;
        if (g_state != nullptr) {
            InterlockedExchange(
                &g_state->movement_snapshot_valid,
                0);
        }
    }

    MovementValues values{};
    if (!ReadMovementValues(player, values)) {
        SetFlyError(pwnie::kFlyErrorMovementRead);
        return false;
    }

    g_movement_snapshot_player = player;
    g_movement_snapshot = values;
    g_movement_snapshot_valid = true;
    if (g_state != nullptr) {
        InterlockedExchange(
            &g_state->movement_snapshot_valid,
            1);
    }
    return true;
}

[[nodiscard]] bool ValidFlyConfig(
    const MovementValues& values) {
    return std::isfinite(values.walking_speed) &&
           std::isfinite(values.jump_speed) &&
           std::isfinite(values.jump_hold_time) &&
           values.walking_speed >= 0.0F &&
           values.walking_speed <= 20000.0F &&
           values.jump_speed >= 10.0F &&
           values.jump_speed <= 20000.0F &&
           values.jump_hold_time >= 0.2F &&
           values.jump_hold_time <= 999999.0F;
}

void ProcessFlyOnGameThread(void* player) {
    if (g_state == nullptr ||
        !g_fly_lock_initialized) {
        return;
    }

    EnterCriticalSection(&g_fly_lock);

    if (AtomicRead(&g_state->fly_enabled) == 0) {
        const bool movement_ok = RestoreMovementSnapshotLocked();
        const bool patch_ok = SetCanJumpPatchLocked(false);
        if (movement_ok && patch_ok) {
            SetFlyError(pwnie::kFlyErrorNone);
        }
        LeaveCriticalSection(&g_fly_lock);
        return;
    }

    MemoryBarrier();
    const MovementValues requested{
        g_state->fly_walking_speed,
        g_state->fly_jump_speed,
        g_state->fly_jump_hold_time
    };

    if (!ValidFlyConfig(requested)) {
        const bool movement_ok = RestoreMovementSnapshotLocked();
        const bool patch_ok = SetCanJumpPatchLocked(false);
        if (movement_ok && patch_ok) {
            SetFlyError(pwnie::kFlyErrorInvalidConfig);
        }
        LeaveCriticalSection(&g_fly_lock);
        return;
    }

    if (!SaveMovementSnapshotLocked(player)) {
        LeaveCriticalSection(&g_fly_lock);
        return;
    }

    if (!SetCanJumpPatchLocked(true)) {
        const bool movement_restored =
            RestoreMovementSnapshotLocked();
        (void)movement_restored;
        LeaveCriticalSection(&g_fly_lock);
        return;
    }

    if (!WriteMovementValues(player, requested)) {
        SetFlyError(pwnie::kFlyErrorMovementWrite);
        const bool patch_restored = SetCanJumpPatchLocked(false);
        const bool movement_restored =
            RestoreMovementSnapshotLocked();
        (void)patch_restored;
        (void)movement_restored;
        LeaveCriticalSection(&g_fly_lock);
        return;
    }

    InterlockedIncrement(&g_state->fly_update_count);
    SetFlyError(pwnie::kFlyErrorNone);
    LeaveCriticalSection(&g_fly_lock);
}

void ProcessInternalSprintOnGameThread(void* player) {
    if (g_state == nullptr ||
        player == nullptr ||
        AtomicRead(&g_state->call_internal_sprint_requested) == 0) {
        return;
    }

    if (InterlockedExchange(
            &g_state->call_internal_sprint_requested,
            0) == 0) {
        return;
    }

    std::uintptr_t iplayer_address = 0;
    if (!CheckedAdd(
            reinterpret_cast<std::uintptr_t>(player),
            pwnie::kIPlayerSubobjectOffset,
            iplayer_address) ||
        g_original_get_sprint == nullptr) {
        return;
    }

    const float result = g_original_get_sprint(
        reinterpret_cast<void*>(iplayer_address));
    g_state->last_internal_sprint_result = result;
    MemoryBarrier();
    InterlockedIncrement(&g_state->internal_sprint_call_count);
}

[[nodiscard]] bool ValidTeleport(const Vector3& position) {
    constexpr float kCoordinateLimit = 1000000.0F;
    return std::isfinite(position.x) &&
           std::isfinite(position.y) &&
           std::isfinite(position.z) &&
           std::fabs(position.x) <= kCoordinateLimit &&
           std::fabs(position.y) <= kCoordinateLimit &&
           std::fabs(position.z) <= kCoordinateLimit;
}

void ProcessTeleportAfterOriginalTick(void* player) {
    if (g_state == nullptr ||
        player == nullptr ||
        g_actor_set_position == nullptr) {
        return;
    }

    const LONG request_id =
        AtomicRead(&g_state->teleport_request_id);
    if (request_id == 0 ||
        (request_id & 1) != 0 ||
        request_id ==
        AtomicRead(&g_state->teleport_completed_id)) {
        return;
    }

    MemoryBarrier();
    const Vector3 requested{
        g_state->teleport_x,
        g_state->teleport_y,
        g_state->teleport_z
    };
    MemoryBarrier();

    // A later writer may have replaced the single-slot mailbox while these
    // floats were copied.  In that case, wait for the next Tick and take a
    // coherent snapshot of the newest request instead of mixing requests.
    if (request_id !=
        AtomicRead(&g_state->teleport_request_id)) {
        return;
    }

    LONG succeeded = 0;
    if (ValidTeleport(requested)) {
        g_actor_set_position(player, &requested);
        // Publish the post-call position before acknowledging the request so
        // the external controller never reports the previous Tick's tuple as
        // the result of a completed teleport.
        if (PublishPosition(player)) {
            constexpr float kPositionTolerance = 2.0F;
            succeeded =
                std::fabs(g_state->coordinate_x - requested.x) <=
                        kPositionTolerance &&
                    std::fabs(g_state->coordinate_y - requested.y) <=
                        kPositionTolerance &&
                    std::fabs(g_state->coordinate_z - requested.z) <=
                        kPositionTolerance
                    ? 1
                    : 0;
        }
    }

    InterlockedExchange(
        &g_state->teleport_succeeded,
        succeeded);
    MemoryBarrier();
    InterlockedExchange(
        &g_state->teleport_completed_id,
        request_id);
}

float PWNIE_FASTCALL HookGetSprintMultiplier(
    void* iplayer,
    void* /*unused_edx*/) {
    HookActivityGuard activity;

    const bool local =
        iplayer != nullptr &&
        g_is_local_player != nullptr &&
        g_is_local_player(iplayer);

    if (g_state != nullptr &&
        local &&
        AtomicRead(&g_state->sprint_override_enabled) != 0) {
        MemoryBarrier();
        const float value = g_state->sprint_override_value;
        if (std::isfinite(value)) {
            return value;
        }
    }

    return g_original_get_sprint != nullptr
               ? g_original_get_sprint(iplayer)
               : 3.0F;
}

void PWNIE_FASTCALL HookPlayerTick(
    void* player,
    void* /*unused_edx*/,
    float delta_time) {
    HookActivityGuard activity;

    const bool local = IsCompleteLocalPlayer(player);
    if (local && g_state != nullptr) {
        PublishPlayer(player);
        InterlockedIncrement(&g_state->tick_count);
        InterlockedExchange(
            &g_state->last_tick_ms,
            static_cast<LONG>(GetTickCount()));
        ProcessFlyOnGameThread(player);
        ProcessInternalSprintOnGameThread(player);
    }

    if (g_original_player_tick != nullptr) {
        g_original_player_tick(player, delta_time);
    }

    if (local && g_state != nullptr) {
        ProcessTeleportAfterOriginalTick(player);
        UpdateTelemetry(player);
    }
}

[[nodiscard]] bool RestoreFlyBeforeShutdown() {
    if (g_state == nullptr ||
        !g_fly_lock_initialized) {
        return true;
    }

    InterlockedExchange(&g_state->fly_enabled, 0);

    // Prefer restoration by the game-thread Tick.  If the game is paused,
    // perform the simple field/code restoration from this lifecycle thread.
    for (int attempt = 0; attempt < 25; ++attempt) {
        if (AtomicRead(&g_state->movement_snapshot_valid) == 0 &&
            AtomicRead(&g_state->can_jump_patch_active) == 0) {
            return true;
        }
        Sleep(10);
    }

    EnterCriticalSection(&g_fly_lock);
    const bool movement_ok = RestoreMovementSnapshotLocked();
    const bool patch_ok = SetCanJumpPatchLocked(false);
    if (movement_ok && patch_ok) {
        SetFlyError(pwnie::kFlyErrorNone);
    }
    LeaveCriticalSection(&g_fly_lock);
    return movement_ok && patch_ok;
}

[[nodiscard]] MH_STATUS DisableAllHooksWithRetry() {
    MH_STATUS status = MH_UNKNOWN;
    for (int attempt = 0; attempt < 10; ++attempt) {
        status = MH_DisableHook(MH_ALL_HOOKS);
        if (status == MH_OK || status == MH_ERROR_DISABLED) {
            return status;
        }
        Sleep(10);
    }
    return status;
}

DWORD WINAPI WorkerThread(void*) {
    if (!InitializeSharedState()) {
        ExitWorker(1);
    }

    HMODULE game_logic = nullptr;
    for (int attempt = 0;
         attempt < 500 && game_logic == nullptr;
         ++attempt) {
        game_logic = GetModuleHandleW(pwnie::kGameLogicName);
        if (game_logic == nullptr) {
            Sleep(10);
        }
    }

    if (game_logic == nullptr) {
        FailAndExit(pwnie::kInitErrorGameLogicMissing, 2);
    }

    const auto base =
        reinterpret_cast<std::uintptr_t>(game_logic);
    const LONG signature_error =
        VerifyRuntimeSignatures(base);
    if (signature_error != pwnie::kInitErrorNone) {
        FailAndExit(signature_error, 3);
    }

    g_actor_get_position =
        reinterpret_cast<ActorGetPositionFn>(
            base + pwnie::kActorGetPositionRva);
    g_actor_set_position =
        reinterpret_cast<ActorSetPositionFn>(
            base + pwnie::kActorSetPositionRva);
    g_is_local_player =
        reinterpret_cast<IsLocalPlayerFn>(
            base + pwnie::kIsLocalPlayerRva);
    g_tick_target = reinterpret_cast<void*>(
        base + pwnie::kPlayerTickRva);
    g_sprint_target = reinterpret_cast<void*>(
        base + pwnie::kGetSprintMultiplierRva);
    g_can_jump_target = reinterpret_cast<BYTE*>(
        base + pwnie::kPlayerCanJumpRva);

    InterlockedExchange(&g_state->signatures_verified, 1);

    InitializeCriticalSection(&g_fly_lock);
    g_fly_lock_initialized = true;

    if (MH_Initialize() != MH_OK) {
        DeleteCriticalSection(&g_fly_lock);
        g_fly_lock_initialized = false;
        FailAndExit(pwnie::kInitErrorMinHookInitialize, 4);
    }

    const MH_STATUS tick_create = MH_CreateHook(
        g_tick_target,
        reinterpret_cast<void*>(&HookPlayerTick),
        reinterpret_cast<void**>(&g_original_player_tick));
    if (tick_create != MH_OK) {
        MH_Uninitialize();
        DeleteCriticalSection(&g_fly_lock);
        g_fly_lock_initialized = false;
        FailAndExit(pwnie::kInitErrorTickHookCreate, 5);
    }

    const MH_STATUS sprint_create = MH_CreateHook(
        g_sprint_target,
        reinterpret_cast<void*>(&HookGetSprintMultiplier),
        reinterpret_cast<void**>(&g_original_get_sprint));
    if (sprint_create != MH_OK) {
        MH_RemoveHook(g_tick_target);
        MH_Uninitialize();
        DeleteCriticalSection(&g_fly_lock);
        g_fly_lock_initialized = false;
        FailAndExit(pwnie::kInitErrorSprintHookCreate, 6);
    }

    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
        const MH_STATUS rollback_status = DisableAllHooksWithRetry();
        if (rollback_status != MH_OK &&
            rollback_status != MH_ERROR_DISABLED) {
            // MH_EnableHook can partially enable targets. If rollback also
            // fails, keep this module and all trampoline storage resident;
            // unloading would leave a possible live jump into freed code.
            InterlockedExchange(
                &g_state->init_error,
                pwnie::kInitErrorHookDisable);
            InterlockedExchange(&g_state->hook_ready, -1);
            return 8;
        }

        // Give any thread that had already entered a detour time to execute
        // its prologue and register with g_active_hook_calls.
        Sleep(50);
        while (InterlockedCompareExchange(
                   &g_active_hook_calls,
                   0,
                   0) != 0) {
            Sleep(1);
        }
        MH_RemoveHook(g_sprint_target);
        MH_RemoveHook(g_tick_target);
        MH_Uninitialize();
        DeleteCriticalSection(&g_fly_lock);
        g_fly_lock_initialized = false;
        FailAndExit(pwnie::kInitErrorHookEnable, 7);
    }

    InterlockedExchange(&g_state->tick_hook_ready, 1);
    InterlockedExchange(&g_state->sprint_hook_ready, 1);
    MemoryBarrier();
    InterlockedExchange(&g_state->hook_ready, 1);

    while (AtomicRead(&g_state->shutdown_requested) == 0 &&
           (GetAsyncKeyState(VK_END) & 1) == 0) {
        Sleep(16);

        const DWORD last_tick = static_cast<DWORD>(
            AtomicRead(&g_state->last_tick_ms));
        if (last_tick != 0 &&
            GetTickCount() - last_tick > 2000) {
            // Menu/loading pauses can destroy the cached Player while the DLL
            // remains loaded. Invalidate externally visible pointers and
            // telemetry; the next verified local Tick republishes them.
            InterlockedExchange(
                reinterpret_cast<volatile LONG*>(
                    &g_state->player_address),
                0);
            InterlockedExchange(&g_state->health_valid, 0);
            InterlockedExchange(&g_state->coordinates_valid, 0);
            InterlockedExchange(&g_state->movement_valid, 0);
            InterlockedExchange(
                &g_state->capture_player_requested,
                1);
        }
    }

    if (!RestoreFlyBeforeShutdown()) {
        // Keep code/trampolines resident when original fields or bytes cannot
        // be proven restored. The controller can retry fly off/shutdown after
        // the world resumes; unloading here would hide a live modification.
        InterlockedExchange(
            &g_state->init_error,
            pwnie::kInitErrorHookDisable);
        InterlockedExchange(&g_state->hook_ready, -1);
        return 9;
    }
    InterlockedExchange(&g_state->hook_ready, 0);

    const MH_STATUS disable_status = DisableAllHooksWithRetry();

    if (disable_status != MH_OK &&
        disable_status != MH_ERROR_DISABLED) {
        // Never unload code that may still be the destination of a live hook.
        InterlockedExchange(
            &g_state->init_error,
            pwnie::kInitErrorHookDisable);
        InterlockedExchange(&g_state->hook_ready, -1);
        return 8;
    }

    InterlockedExchange(&g_state->tick_hook_ready, 0);
    InterlockedExchange(&g_state->sprint_hook_ready, 0);

    // MinHook has removed future entry points. A short grace period narrows
    // the prologue-before-counter race before the explicit activity drain.
    Sleep(50);
    while (InterlockedCompareExchange(
               &g_active_hook_calls,
               0,
               0) != 0) {
        Sleep(1);
    }

    MH_RemoveHook(g_sprint_target);
    MH_RemoveHook(g_tick_target);
    MH_Uninitialize();

    DeleteCriticalSection(&g_fly_lock);
    g_fly_lock_initialized = false;

    ExitWorker(0);
}

} // namespace

BOOL WINAPI DllMain(
    HINSTANCE instance,
    DWORD reason,
    LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = instance;
        DisableThreadLibraryCalls(instance);

        HANDLE thread = CreateThread(
            nullptr,
            0,
            &WorkerThread,
            nullptr,
            0,
            nullptr);

        if (thread != nullptr) {
            CloseHandle(thread);
        } else {
            return FALSE;
        }
    }

    return TRUE;
}
