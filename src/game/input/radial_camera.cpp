#include "game/input/radial_camera.h"

#include "core/common.h"
#include "game/equipment/equip_access.h"
#include "game/state/singleton_resolver.h"

#include <MinHook.h>
#include <cmath>
#include <cstdint>

namespace radial_menu_mod::radial_camera {
namespace {

constexpr std::uintptr_t kChrCamUpdateRva = 0x3B12D0;
constexpr std::uintptr_t kWorldChrManChrCamOffset = 0x1ECE0;
constexpr std::uintptr_t kGameDataManSettingsOffset = 0x58;
constexpr std::uintptr_t kCameraSpeedSettingOffset = 0x00;
constexpr std::uintptr_t kCameraMatrixOffset = 0x10;
constexpr std::uintptr_t kChrCamFollowCamOffset = 0x60;
constexpr std::uintptr_t kChrCamAimCamOffset = 0x68;
constexpr std::uintptr_t kChrCamDistViewCamOffset = 0x70;
constexpr std::uintptr_t kChrCamPadAccelerationOffset = 0x90;
constexpr std::uintptr_t kChrCamMoveAccelerationOffset = 0xA0;

constexpr std::size_t kChrCamMatrixCount = 4;
constexpr std::size_t kChrCamStateSnapshotSize = 0x1130;
constexpr std::size_t kFollowCamStateSnapshotSize = 0x4A0;
constexpr std::size_t kAimCamStateSnapshotSize = 0x150;
constexpr std::size_t kDistViewCamStateSnapshotSize = 0x150;
constexpr float kAttemptedChrCamDeltaDeadzone = 0.006f;
constexpr float kAttemptedChrCamDeltaFullScale = 0.025f;

struct CameraMatrixSnapshot {
    float values[16] = {};
    bool valid = false;
};

template <std::size_t Size>
struct ObjectStateSnapshot {
    std::uintptr_t address = 0;
    std::uint8_t bytes[Size] = {};
    bool valid = false;
};

using ChrCamUpdateFn = void (*)(void* chr_cam, void* arg2, void* chr_ins, bool enabled);

bool g_chr_cam_update_hook_installed = false;
bool g_chr_cam_update_hook_failed = false;
bool g_logged_camera_speed_suppression = false;
bool g_logged_chr_cam_freeze = false;
bool g_logged_full_chr_cam_state_freeze = false;
bool g_logged_chr_cam_update_freeze = false;

std::uint8_t g_original_camera_speed = 0;
bool g_camera_speed_suppressed = false;

float g_selection_x = 0.0f;
float g_selection_y = 0.0f;

CameraMatrixSnapshot g_chr_cam_matrix_snapshots[kChrCamMatrixCount] = {};
bool g_chr_cam_matrix_frozen = false;
ObjectStateSnapshot<kChrCamStateSnapshotSize> g_chr_cam_state_snapshot = {};
ObjectStateSnapshot<kFollowCamStateSnapshotSize> g_follow_cam_state_snapshot = {};
ObjectStateSnapshot<kAimCamStateSnapshotSize> g_aim_cam_state_snapshot = {};
ObjectStateSnapshot<kDistViewCamStateSnapshotSize> g_dist_view_cam_state_snapshot = {};
bool g_full_chr_cam_state_frozen = false;

ChrCamUpdateFn g_original_chr_cam_update = nullptr;
RadialActiveFn g_is_radial_active = nullptr;

template <typename T>
bool ReadGameMemory(std::uintptr_t address, T& value)
{
    if (!IsReadableMemory(reinterpret_cast<const void*>(address), sizeof(T))) return false;
    value = *reinterpret_cast<const T*>(address);
    return true;
}

template <typename T>
bool WriteGameMemory(std::uintptr_t address, const T& value)
{
    if (!IsReadableMemory(reinterpret_cast<const void*>(address), sizeof(T))) return false;
    *reinterpret_cast<T*>(address) = value;
    return true;
}

float ClampStickAxis(float value)
{
    if (value < -1.0f) return -1.0f;
    if (value > 1.0f) return 1.0f;
    return value;
}

bool IsRadialActive()
{
    return g_is_radial_active != nullptr && g_is_radial_active();
}

std::uintptr_t ResolveWorldChrMan()
{
    static std::uintptr_t static_address = 0;
    static bool searched = false;
    if (!searched) {
        searched = true;
        static_address = singleton_resolver::ResolveSingletonStaticAddress("WorldChrMan");
        if (static_address) Log("WorldChrMan static resolved at 0x%llX.", static_cast<unsigned long long>(static_address));
    }

    if (!static_address) return 0;
    std::uintptr_t world_chr_man = 0;
    return ReadGameMemory(static_address, world_chr_man) ? world_chr_man : 0;
}

std::uintptr_t ResolveChrCam()
{
    const auto world_chr_man = ResolveWorldChrMan();
    if (!world_chr_man) return 0;

    std::uintptr_t chr_cam = 0;
    return ReadGameMemory(world_chr_man + kWorldChrManChrCamOffset, chr_cam) ? chr_cam : 0;
}

std::uintptr_t ResolveGameSettings()
{
    const auto game_data_man_static = equip_access::ResolveGameDataManAddress();
    if (!game_data_man_static) return 0;

    std::uintptr_t game_data_man = 0;
    if (!ReadGameMemory(game_data_man_static, game_data_man) || !game_data_man) return 0;

    std::uintptr_t settings = 0;
    return ReadGameMemory(game_data_man + kGameDataManSettingsOffset, settings) ? settings : 0;
}

void ResetCameraSnapshots()
{
    for (auto& snapshot : g_chr_cam_matrix_snapshots) snapshot = {};
    g_chr_cam_matrix_frozen = false;
    g_chr_cam_state_snapshot = {};
    g_follow_cam_state_snapshot = {};
    g_aim_cam_state_snapshot = {};
    g_dist_view_cam_state_snapshot = {};
    g_full_chr_cam_state_frozen = false;
}

bool ReadCameraMatrixAt(std::uintptr_t pers_cam, CameraMatrixSnapshot& snapshot)
{
    if (!IsReadableMemory(reinterpret_cast<const void*>(pers_cam + kCameraMatrixOffset), sizeof(snapshot.values))) return false;

    const auto* source = reinterpret_cast<const float*>(pers_cam + kCameraMatrixOffset);
    for (std::size_t i = 0; i < 16; ++i) snapshot.values[i] = source[i];
    snapshot.valid = true;
    return true;
}

bool WriteCameraMatrixAt(std::uintptr_t pers_cam, const CameraMatrixSnapshot& snapshot)
{
    if (!snapshot.valid || !IsReadableMemory(reinterpret_cast<const void*>(pers_cam + kCameraMatrixOffset),
        sizeof(snapshot.values))) {
        return false;
    }

    auto* target = reinterpret_cast<float*>(pers_cam + kCameraMatrixOffset);
    for (std::size_t i = 0; i < 16; ++i) target[i] = snapshot.values[i];
    return true;
}

bool ReadChrCamMatrixPointer(std::uintptr_t chr_cam, std::size_t index, std::uintptr_t& pers_cam)
{
    pers_cam = 0;
    if (!chr_cam) return false;
    if (index == 0) {
        pers_cam = chr_cam;
        return true;
    }

    constexpr std::uintptr_t offsets[] = {
        kChrCamFollowCamOffset,
        kChrCamAimCamOffset,
        kChrCamDistViewCamOffset,
    };
    if (index - 1 >= (sizeof(offsets) / sizeof(offsets[0]))) return false;
    return ReadGameMemory(chr_cam + offsets[index - 1], pers_cam) && pers_cam != 0;
}

template <std::size_t Size>
bool CaptureObjectState(std::uintptr_t address, ObjectStateSnapshot<Size>& snapshot)
{
    if (!address || !IsReadableMemory(reinterpret_cast<const void*>(address), Size)) return false;

    snapshot.address = address;
    const auto* source = reinterpret_cast<const std::uint8_t*>(address);
    for (std::size_t i = 0; i < Size; ++i) snapshot.bytes[i] = source[i];
    snapshot.valid = true;
    return true;
}

template <std::size_t Size>
bool RestoreObjectState(const ObjectStateSnapshot<Size>& snapshot)
{
    if (!snapshot.valid || !snapshot.address || !IsReadableMemory(reinterpret_cast<const void*>(snapshot.address), Size)) {
        return false;
    }

    auto* target = reinterpret_cast<std::uint8_t*>(snapshot.address);
    for (std::size_t i = sizeof(std::uintptr_t); i < Size; ++i) target[i] = snapshot.bytes[i];
    return true;
}

void CaptureAttemptedChrCamDeltaAsSelection(const CameraMatrixSnapshot& frozen, const CameraMatrixSnapshot& current)
{
    if (!frozen.valid || !current.valid || !IsRadialActive()) return;

    const float yaw_delta = frozen.values[8] * current.values[10] - frozen.values[10] * current.values[8];
    const float pitch_delta = current.values[9] - frozen.values[9];
    const float magnitude = std::sqrt((yaw_delta * yaw_delta) + (pitch_delta * pitch_delta));
    if (magnitude < kAttemptedChrCamDeltaDeadzone) return;

    g_selection_x = ClampStickAxis(-yaw_delta / kAttemptedChrCamDeltaFullScale);
    g_selection_y = ClampStickAxis(pitch_delta / kAttemptedChrCamDeltaFullScale);
}

void ZeroChrCamAcceleration(std::uintptr_t chr_cam)
{
    float zero_vector[4] = {};
    if (IsReadableMemory(reinterpret_cast<const void*>(chr_cam + kChrCamPadAccelerationOffset), sizeof(zero_vector))) {
        auto* target = reinterpret_cast<float*>(chr_cam + kChrCamPadAccelerationOffset);
        for (float value : zero_vector) *target++ = value;
    }
    if (IsReadableMemory(reinterpret_cast<const void*>(chr_cam + kChrCamMoveAccelerationOffset), sizeof(zero_vector))) {
        auto* target = reinterpret_cast<float*>(chr_cam + kChrCamMoveAccelerationOffset);
        for (float value : zero_vector) *target++ = value;
    }
}

void ApplyFullChrCamStateFreeze(std::uintptr_t chr_cam)
{
    std::uintptr_t follow_cam = 0;
    std::uintptr_t aim_cam = 0;
    std::uintptr_t dist_view_cam = 0;
    (void)ReadGameMemory(chr_cam + kChrCamFollowCamOffset, follow_cam);
    (void)ReadGameMemory(chr_cam + kChrCamAimCamOffset, aim_cam);
    (void)ReadGameMemory(chr_cam + kChrCamDistViewCamOffset, dist_view_cam);

    if (!g_full_chr_cam_state_frozen) {
        const bool captured_chr = CaptureObjectState(chr_cam, g_chr_cam_state_snapshot);
        const bool captured_follow = CaptureObjectState(follow_cam, g_follow_cam_state_snapshot);
        const bool captured_aim = CaptureObjectState(aim_cam, g_aim_cam_state_snapshot);
        const bool captured_dist = CaptureObjectState(dist_view_cam, g_dist_view_cam_state_snapshot);
        g_full_chr_cam_state_frozen = captured_chr || captured_follow || captured_aim || captured_dist;
        if (g_full_chr_cam_state_frozen && !g_logged_full_chr_cam_state_freeze) {
            g_logged_full_chr_cam_state_freeze = true;
            Log("Freezing full ChrCam state while radial is open (chr=%d follow=%d aim=%d dist=%d).",
                static_cast<int>(captured_chr),
                static_cast<int>(captured_follow),
                static_cast<int>(captured_aim),
                static_cast<int>(captured_dist));
        }
    }

    if (!g_full_chr_cam_state_frozen) return;

    (void)RestoreObjectState(g_chr_cam_state_snapshot);
    (void)RestoreObjectState(g_follow_cam_state_snapshot);
    (void)RestoreObjectState(g_aim_cam_state_snapshot);
    (void)RestoreObjectState(g_dist_view_cam_state_snapshot);
}

void ApplyChrCamFreeze()
{
    const auto chr_cam = ResolveChrCam();
    if (!chr_cam) return;

    if (!g_chr_cam_matrix_frozen) {
        bool captured_any = false;
        for (std::size_t i = 0; i < kChrCamMatrixCount; ++i) {
            std::uintptr_t pers_cam = 0;
            if (!ReadChrCamMatrixPointer(chr_cam, i, pers_cam)) continue;
            captured_any = ReadCameraMatrixAt(pers_cam, g_chr_cam_matrix_snapshots[i]) || captured_any;
        }
        g_chr_cam_matrix_frozen = captured_any;
        if (captured_any && !g_logged_chr_cam_freeze) {
            g_logged_chr_cam_freeze = true;
            Log("Freezing ChrCam matrices and acceleration while radial is open.");
        }
    }

    if (g_chr_cam_matrix_frozen) {
        std::uintptr_t pers_cam = 0;
        CameraMatrixSnapshot current = {};
        if (ReadChrCamMatrixPointer(chr_cam, 0, pers_cam) && ReadCameraMatrixAt(pers_cam, current)) {
            CaptureAttemptedChrCamDeltaAsSelection(g_chr_cam_matrix_snapshots[0], current);
        }
    }

    ApplyFullChrCamStateFreeze(chr_cam);
    ZeroChrCamAcceleration(chr_cam);

    if (!g_chr_cam_matrix_frozen) return;

    for (std::size_t i = 0; i < kChrCamMatrixCount; ++i) {
        std::uintptr_t pers_cam = 0;
        if (!ReadChrCamMatrixPointer(chr_cam, i, pers_cam)) continue;
        (void)WriteCameraMatrixAt(pers_cam, g_chr_cam_matrix_snapshots[i]);
    }
}

void HookedChrCamUpdate(void* chr_cam, void* arg2, void* chr_ins, bool enabled)
{
    const bool should_freeze = IsRadialActive();
    if (g_original_chr_cam_update != nullptr) g_original_chr_cam_update(chr_cam, arg2, chr_ins, enabled);

    if (!should_freeze) {
        if (g_chr_cam_matrix_frozen || g_full_chr_cam_state_frozen) ResetCameraSnapshots();
        return;
    }

    ApplyChrCamFreeze();
    if (!g_logged_chr_cam_update_freeze) {
        g_logged_chr_cam_update_freeze = true;
        Log("Restored ChrCam state immediately after camera update while radial is open.");
    }
}

void ApplyCameraSpeedSuppression()
{
    const auto settings = ResolveGameSettings();
    if (!settings) return;

    const bool should_suppress = IsRadialActive();
    const auto camera_speed_address = settings + kCameraSpeedSettingOffset;
    if (should_suppress) {
        std::uint8_t speed = 0;
        if (!ReadGameMemory(camera_speed_address, speed)) return;
        if (!g_camera_speed_suppressed) {
            g_original_camera_speed = speed;
            g_camera_speed_suppressed = true;
        }

        const std::uint8_t suppressed_speed = 0;
        if (speed != suppressed_speed) WriteGameMemory(camera_speed_address, suppressed_speed);
        if (!g_logged_camera_speed_suppression) {
            g_logged_camera_speed_suppression = true;
            Log("Suppressed camera speed setting while radial is open (original=%u).", g_original_camera_speed);
        }
        return;
    }

    if (!g_camera_speed_suppressed) return;

    WriteGameMemory(camera_speed_address, g_original_camera_speed);
    g_camera_speed_suppressed = false;
}

void TryInstallChrCamHook()
{
    if (g_chr_cam_update_hook_installed || g_chr_cam_update_hook_failed) return;

    const auto hook_address = GetModuleBase() + kChrCamUpdateRva;
    if (!IsReadableMemory(reinterpret_cast<const void*>(hook_address), 1)) {
        g_chr_cam_update_hook_failed = true;
        Log("ChrCam update hook failed: target RVA is not readable.");
        return;
    }

    const MH_STATUS create_status = MH_CreateHook(reinterpret_cast<void*>(hook_address),
        reinterpret_cast<void*>(&HookedChrCamUpdate), reinterpret_cast<void**>(&g_original_chr_cam_update));
    if (create_status != MH_OK) {
        g_chr_cam_update_hook_failed = true;
        Log("ChrCam update hook creation failed: status=%d.", static_cast<int>(create_status));
        return;
    }

    const MH_STATUS enable_status = MH_EnableHook(reinterpret_cast<void*>(hook_address));
    if (enable_status != MH_OK) {
        g_chr_cam_update_hook_failed = true;
        Log("ChrCam update hook enable failed: status=%d.", static_cast<int>(enable_status));
        return;
    }

    g_chr_cam_update_hook_installed = true;
    Log("ChrCam update hook installed at rva=0x%llX.", static_cast<unsigned long long>(kChrCamUpdateRva));
}

}  // namespace

bool Initialize(RadialActiveFn is_radial_active)
{
    g_is_radial_active = is_radial_active;
    TryInstallChrCamHook();
    return true;
}

void SampleFrame()
{
    TryInstallChrCamHook();
    ApplyCameraSpeedSuppression();
}

float ConsumeSelectionX()
{
    const float value = g_selection_x;
    g_selection_x = 0.0f;
    return value;
}

float ConsumeSelectionY()
{
    const float value = g_selection_y;
    g_selection_y = 0.0f;
    return value;
}

}  // namespace radial_menu_mod::radial_camera
