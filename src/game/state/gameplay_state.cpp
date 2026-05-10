#include "game/state/gameplay_state.h"

#include "core/common.h"
#include "game/state/singleton_resolver.h"

#include <cstdint>

namespace radial_menu_mod::gameplay_state {
namespace {

std::uintptr_t g_cs_fe_man_static_address = 0;
bool g_searched_cs_fe_man = false;
bool g_logged_missing_cs_fe_man = false;
bool g_logged_unreadable_cs_fe_man = false;
bool g_cached_hud_state_valid = false;
bool g_cached_normal_hud_state = false;

template <typename T>
bool ReadGameMemory(std::uintptr_t address, T& value)
{
    if (!IsReadableMemory(reinterpret_cast<const void*>(address), sizeof(T))) return false;
    value = *reinterpret_cast<const T*>(address);
    return true;
}

std::uintptr_t ResolveCSFeMan()
{
    if (!g_searched_cs_fe_man) {
        g_searched_cs_fe_man = true;
        g_cs_fe_man_static_address = singleton_resolver::ResolveSingletonStaticAddress("CSFeMan");
    }

    if (!g_cs_fe_man_static_address) return 0;
    std::uintptr_t fe_man = 0;
    return ReadGameMemory(g_cs_fe_man_static_address, fe_man) ? fe_man : 0;
}

}  // namespace

bool RefreshNormalGameplayHudState()
{
    constexpr std::uintptr_t kHudStateOffset = 0x78;
    constexpr std::uint8_t kHudStateDefault = 3;

    const auto fe_man = ResolveCSFeMan();
    if (!fe_man) {
        if (!g_logged_missing_cs_fe_man) {
            g_logged_missing_cs_fe_man = true;
            Log("CSFeMan unresolved; radial menu input is disabled until HUD state can be read.");
        }
        g_cached_hud_state_valid = true;
        g_cached_normal_hud_state = false;
        return false;
    }

    std::uint8_t hud_state = 0;
    if (!ReadGameMemory(fe_man + kHudStateOffset, hud_state)) {
        if (!g_logged_unreadable_cs_fe_man) {
            Log("CSFeMan HUD state unreadable; radial menu input is disabled until HUD state can be read.");
            g_logged_unreadable_cs_fe_man = true;
        }
        g_cached_hud_state_valid = true;
        g_cached_normal_hud_state = false;
        return false;
    }

    g_logged_unreadable_cs_fe_man = false;
    g_cached_hud_state_valid = true;
    g_cached_normal_hud_state = hud_state == kHudStateDefault;
    return g_cached_normal_hud_state;
}

bool GetCachedNormalGameplayHudState()
{
    return g_cached_hud_state_valid ? g_cached_normal_hud_state : RefreshNormalGameplayHudState();
}

bool IsNormalGameplayHudState()
{
    return GetCachedNormalGameplayHudState();
}

}  // namespace radial_menu_mod::gameplay_state
