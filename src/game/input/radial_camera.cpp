#include "game/input/radial_camera.h"

#include "core/common.h"

#include <MinHook.h>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace radial_menu_mod::radial_camera {
namespace {

constexpr std::uintptr_t kChrCamInputAccelerationUpdateRva = 0x3B2060;
constexpr std::uintptr_t kChrCamPadAccelerationOffset = 0x90;
constexpr std::uintptr_t kChrCamMoveAccelerationOffset = 0xA0;
constexpr std::uintptr_t kChrCamLockOnInputOffset = 0xB0;

constexpr float kAccelerationPresenceEpsilon = 0.001f;
constexpr float kPadAccelerationSelectionDeadzone = 0.28f;
constexpr float kPadAccelerationSelectionFullScale = 0.7f;
constexpr float kMoveAccelerationSelectionDeadzone = 0.15f;
constexpr float kMoveAccelerationSelectionFullScale = 0.2f;

using ChrCamInputAccelerationUpdateFn = void (*)(void* chr_cam, bool enabled);

bool g_chr_cam_input_acceleration_update_hook_installed = false;
bool g_chr_cam_input_acceleration_update_hook_failed = false;
bool g_camera_hook_installation_complete = false;
bool g_logged_camera_acceleration_suppression = false;

float g_selection_x = 0.0f;
float g_selection_y = 0.0f;
std::uintptr_t g_cached_chr_cam = 0;
CachedReadableRegion g_pad_acceleration_region = {};
CachedReadableRegion g_move_acceleration_region = {};
CachedReadableRegion g_lock_on_input_region = {};

ChrCamInputAccelerationUpdateFn g_original_chr_cam_input_acceleration_update = nullptr;
RadialActiveFn g_is_radial_active = nullptr;

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

bool ReadFloatArray(std::uintptr_t address, float* values, std::size_t count, CachedReadableRegion& region)
{
    const std::size_t bytes = sizeof(float) * count;
    if (!EnsureCachedReadableMemory(address, bytes, region)) return false;

    const auto* source = reinterpret_cast<const float*>(address);
    for (std::size_t i = 0; i < count; ++i) values[i] = source[i];
    return true;
}

bool ClearFloatArray(std::uintptr_t address, std::size_t count, CachedReadableRegion& region)
{
    const std::size_t bytes = sizeof(float) * count;
    if (!EnsureCachedReadableMemory(address, bytes, region)) return false;

    auto* values = reinterpret_cast<float*>(address);
    for (std::size_t i = 0; i < count; ++i) values[i] = 0.0f;
    return true;
}

bool ClearFloat(std::uintptr_t address, CachedReadableRegion& region)
{
    if (!EnsureCachedReadableMemory(address, sizeof(float), region)) return false;

    *reinterpret_cast<float*>(address) = 0.0f;
    return true;
}

CachedReadableRegion& RegionForAccelerationOffset(std::uintptr_t acceleration_offset)
{
    return acceleration_offset == kChrCamPadAccelerationOffset ? g_pad_acceleration_region : g_move_acceleration_region;
}

bool CaptureChrCamAccelerationAsSelection(std::uintptr_t chr_cam,
    std::uintptr_t acceleration_offset,
    float deadzone,
    float full_scale,
    float x_sign,
    float y_sign,
    bool& has_acceleration)
{
    has_acceleration = false;
    if (!chr_cam || !IsRadialActive()) return false;

    float values[4] = {};
    if (!ReadFloatArray(chr_cam + acceleration_offset, values, 4, RegionForAccelerationOffset(acceleration_offset))) {
        return false;
    }

    const float selection_x = values[1] * x_sign;
    const float selection_y = values[0] * y_sign;
    const float magnitude = std::sqrt((selection_x * selection_x) + (selection_y * selection_y));
    has_acceleration = magnitude >= kAccelerationPresenceEpsilon;
    if (magnitude < deadzone) return false;

    g_selection_x = ClampStickAxis(selection_x / full_scale);
    g_selection_y = ClampStickAxis(selection_y / full_scale);
    return true;
}

bool CaptureSelectionFromChrCam(std::uintptr_t chr_cam)
{
    bool has_pad_acceleration = false;
    if (CaptureChrCamAccelerationAsSelection(chr_cam,
        kChrCamPadAccelerationOffset,
        kPadAccelerationSelectionDeadzone,
        kPadAccelerationSelectionFullScale,
        1.0f,
        -1.0f,
        has_pad_acceleration) || has_pad_acceleration) {
        return true;
    }

    bool has_move_acceleration = false;
    const bool captured_move = CaptureChrCamAccelerationAsSelection(chr_cam,
        kChrCamMoveAccelerationOffset,
        kMoveAccelerationSelectionDeadzone,
        kMoveAccelerationSelectionFullScale,
        -1.0f,
        1.0f,
        has_move_acceleration);
    return captured_move || has_move_acceleration;
}

void ClearChrCamAcceleration(std::uintptr_t chr_cam)
{
    if (!chr_cam) return;

    constexpr std::size_t kAccelerationElementCount = 4;
    (void)ClearFloatArray(chr_cam + kChrCamPadAccelerationOffset, kAccelerationElementCount,
        g_pad_acceleration_region);
    (void)ClearFloatArray(chr_cam + kChrCamMoveAccelerationOffset, kAccelerationElementCount,
        g_move_acceleration_region);
    (void)ClearFloat(chr_cam + kChrCamLockOnInputOffset, g_lock_on_input_region);
}

void HookedChrCamInputAccelerationUpdate(void* chr_cam, bool enabled)
{
    if (g_original_chr_cam_input_acceleration_update != nullptr) {
        g_original_chr_cam_input_acceleration_update(chr_cam, enabled);
    }

    if (!IsRadialActive() || chr_cam == nullptr) return;

    const auto chr_cam_address = reinterpret_cast<std::uintptr_t>(chr_cam);
    if (g_cached_chr_cam != chr_cam_address) {
        g_cached_chr_cam = chr_cam_address;
        g_pad_acceleration_region.Reset();
        g_move_acceleration_region.Reset();
        g_lock_on_input_region.Reset();
    }
    (void)CaptureSelectionFromChrCam(chr_cam_address);
    ClearChrCamAcceleration(chr_cam_address);
    if (!g_logged_camera_acceleration_suppression) {
        g_logged_camera_acceleration_suppression = true;
        Log("Captured and suppressed ChrCam input acceleration while radial is open.");
    }
}

template <typename Fn>
void TryInstallHook(const char* name, std::uintptr_t rva, void* detour, Fn*& original, bool& installed, bool& failed)
{
    if (installed || failed) return;

    const auto hook_address = GetModuleBase() + rva;
    if (!IsReadableMemory(reinterpret_cast<const void*>(hook_address), 1)) {
        failed = true;
        Log("%s hook failed: target RVA is not readable.", name);
        return;
    }

    const MH_STATUS create_status = MH_CreateHook(reinterpret_cast<void*>(hook_address), detour,
        reinterpret_cast<void**>(&original));
    if (create_status != MH_OK) {
        failed = true;
        Log("%s hook creation failed: status=%d.", name, static_cast<int>(create_status));
        return;
    }

    const MH_STATUS enable_status = MH_EnableHook(reinterpret_cast<void*>(hook_address));
    if (enable_status != MH_OK) {
        failed = true;
        Log("%s hook enable failed: status=%d.", name, static_cast<int>(enable_status));
        return;
    }

    installed = true;
    Log("%s hook installed at rva=0x%llX.", name, static_cast<unsigned long long>(rva));
}

void TryInstallCameraHooks()
{
    if (g_camera_hook_installation_complete) return;

    TryInstallHook("ChrCam input acceleration update", kChrCamInputAccelerationUpdateRva,
        reinterpret_cast<void*>(&HookedChrCamInputAccelerationUpdate), g_original_chr_cam_input_acceleration_update,
        g_chr_cam_input_acceleration_update_hook_installed, g_chr_cam_input_acceleration_update_hook_failed);
    g_camera_hook_installation_complete = g_chr_cam_input_acceleration_update_hook_installed ||
        g_chr_cam_input_acceleration_update_hook_failed;
}

}  // namespace

bool Initialize(RadialActiveFn is_radial_active)
{
    g_is_radial_active = is_radial_active;
    TryInstallCameraHooks();
    return true;
}

void SampleFrame()
{
    TryInstallCameraHooks();
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
