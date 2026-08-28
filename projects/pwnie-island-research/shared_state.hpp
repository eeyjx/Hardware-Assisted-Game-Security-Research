#pragma once

#include <windows.h>

#include <cstdint>

namespace pwnie {

inline constexpr wchar_t kProcessName[] =
    L"PwnAdventure3-Win32-Shipping.exe";
inline constexpr wchar_t kGameLogicName[] = L"GameLogic.dll";
inline constexpr wchar_t kSharedMappingPrefix[] =
    L"Local\\PwnieIslandResearchState_";

// RVAs verified against the tutorial's 32-bit GameLogic.dll.  Always add
// these to the module's run-time base; ASLR means they are not VAs.
inline constexpr std::uintptr_t kActorGetPositionRva = 0x000016F0;
inline constexpr std::uintptr_t kActorSetPositionRva = 0x00001C80;
inline constexpr std::uintptr_t kGetSprintMultiplierRva = 0x00013940;
inline constexpr std::uintptr_t kSprintMultiplierDataRva = 0x00078B34;
inline constexpr std::uintptr_t kIsLocalPlayerRva = 0x0004FEF0;
inline constexpr std::uintptr_t kPlayerTickRva = 0x00050730;
inline constexpr std::uintptr_t kPlayerCanJumpRva = 0x00051680;

// Player implements IPlayer as a secondary base at +0x70.  Player::Tick and
// Actor methods receive the complete Player*, while IPlayer methods receive
// complete Player + 0x70.
inline constexpr std::uint32_t kIPlayerSubobjectOffset = 0x70;
inline constexpr std::uint32_t kPlayerHealthOffset = 0x30;
inline constexpr std::uint32_t kPlayerWalkingSpeedOffset = 0x190;
inline constexpr std::uint32_t kPlayerJumpSpeedOffset = 0x194;
inline constexpr std::uint32_t kPlayerJumpHoldTimeOffset = 0x198;

inline constexpr float kDefaultFlyWalkingSpeed = 800.0F;
inline constexpr float kDefaultFlyJumpSpeed = 999.0F;
inline constexpr float kDefaultFlyJumpHoldTime = 99999.0F;

// init_error values.  Zero is success.
inline constexpr LONG kInitErrorNone = 0;
inline constexpr LONG kInitErrorGameLogicMissing = 1;
inline constexpr LONG kInitErrorMinHookInitialize = 3;
inline constexpr LONG kInitErrorTickHookCreate = 4;
inline constexpr LONG kInitErrorSprintHookCreate = 5;
inline constexpr LONG kInitErrorHookEnable = 6;
inline constexpr LONG kInitErrorHookDisable = 7;
inline constexpr LONG kInitErrorActorGetPositionSignature = 20;
inline constexpr LONG kInitErrorActorSetPositionSignature = 21;
inline constexpr LONG kInitErrorIsLocalPlayerSignature = 22;
inline constexpr LONG kInitErrorPlayerTickSignature = 23;
inline constexpr LONG kInitErrorPlayerCanJumpSignature = 24;
inline constexpr LONG kInitErrorGetSprintSignature = 25;

// fly_error values.  They are deliberately separate from initialization
// errors because fly can be toggled and retried without reinjecting the DLL.
inline constexpr LONG kFlyErrorNone = 0;
inline constexpr LONG kFlyErrorCanJumpBytes = 1;
inline constexpr LONG kFlyErrorCanJumpProtect = 2;
inline constexpr LONG kFlyErrorMovementRead = 3;
inline constexpr LONG kFlyErrorMovementWrite = 4;
inline constexpr LONG kFlyErrorInvalidConfig = 5;
inline constexpr LONG kFlyErrorRestore = 6;

inline constexpr std::uint32_t kSharedMagic = 0x504E5733; // "PNW3"
// Increment this whenever SharedState changes so an old EXE cannot silently
// interpret a mapping created by a newer DLL (or vice versa).
inline constexpr std::uint32_t kSharedVersion = 4;

struct SharedState {
    std::uint32_t magic;
    std::uint32_t version;
    std::uint32_t owner_process_id;

    volatile LONG hook_ready;
    volatile LONG init_error;
    volatile LONG signatures_verified;
    volatile LONG tick_hook_ready;
    volatile LONG sprint_hook_ready;
    volatile LONG shutdown_requested;

    // Player::Tick continuously publishes the complete local Player address.
    // capture_player_requested is retained as a re-arm request for clients
    // written against the earlier research workflow.
    volatile LONG capture_player_requested;
    volatile std::uint32_t player_address;
    volatile std::int32_t health;
    volatile LONG health_valid;
    volatile LONG tick_count;
    volatile LONG last_tick_ms;

    // Actor::GetPosition publishes these from the game's own Tick thread.
    volatile float coordinate_x;
    volatile float coordinate_y;
    volatile float coordinate_z;
    // Even values are stable snapshots; odd values mean Tick is publishing.
    volatile LONG position_sequence;
    volatile LONG coordinates_valid;
    volatile LONG position_update_count;

    // Current values read from the verified Player fields.
    volatile float movement_walking_speed;
    volatile float movement_jump_speed;
    volatile float movement_jump_hold_time;
    volatile LONG movement_valid;

    // Hook-controlled override of Player::GetSprintMultiplier().
    volatile LONG sprint_override_enabled;
    volatile float sprint_override_value;

    // The original sprint method is invoked by the next local Player::Tick,
    // never from the external process or the DLL worker thread.
    volatile LONG call_internal_sprint_requested;
    volatile LONG internal_sprint_call_count;
    volatile float last_internal_sprint_result;

    // Transactional teleport mailbox. request_id doubles as a seqlock: odd
    // means a producer owns the tuple, and an even value publishes a complete
    // request. Only one request may remain outstanding. Tick calls
    // Actor::SetPosition after the original Tick and acknowledges that same
    // even id in completed_id after publishing the observed post-call position.
    volatile float teleport_x;
    volatile float teleport_y;
    volatile float teleport_z;
    volatile LONG teleport_request_id;
    volatile LONG teleport_completed_id;
    volatile LONG teleport_succeeded;

    // Fly is implemented by patching Player::CanJump and applying verified
    // movement fields on the local game thread.  Original values are restored
    // when disabled, on a player change, and before unload.
    volatile LONG fly_enabled;
    volatile float fly_walking_speed;
    volatile float fly_jump_speed;
    volatile float fly_jump_hold_time;
    volatile LONG fly_update_count;
    volatile LONG fly_restore_count;
    volatile LONG can_jump_patch_active;
    volatile LONG movement_snapshot_valid;
    volatile LONG fly_error;
};

} // namespace pwnie
