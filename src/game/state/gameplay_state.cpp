#include "game/state/gameplay_state.h"

#include "core/common.h"
#include "game/state/singleton_resolver.h"

#include <cstdint>

namespace radial_menu_mod::gameplay_state {
namespace {

struct ReadableRegion {
    std::uintptr_t begin = 0;
    std::uintptr_t end = 0;
};

std::uintptr_t g_cs_fe_man_static_address = 0;
bool g_searched_cs_fe_man = false;
bool g_logged_missing_cs_fe_man = false;
bool g_logged_unreadable_cs_fe_man = false;
bool g_cached_hud_state_valid = false;
bool g_cached_normal_hud_state = false;
ReadableRegion g_cs_fe_man_static_region = {};
ReadableRegion g_cs_fe_man_hud_region = {};

bool IsInRegion(const ReadableRegion& region, std::uintptr_t address, std::size_t bytes)
{
    return region.begin != 0 && address >= region.begin && address + bytes >= address && address + bytes <= region.end;
}

bool RefreshReadableRegion(std::uintptr_t address, std::size_t bytes, ReadableRegion& region)
{
    MEMORY_BASIC_INFORMATION mbi{};
    if (!address || VirtualQuery(reinterpret_cast<const void*>(address), &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD) || (mbi.Protect & PAGE_NOACCESS)) return false;

    const DWORD protect = mbi.Protect & 0xFFu;
    const bool readable = protect == PAGE_READONLY || protect == PAGE_READWRITE || protect == PAGE_WRITECOPY ||
        protect == PAGE_EXECUTE_READ || protect == PAGE_EXECUTE_READWRITE || protect == PAGE_EXECUTE_WRITECOPY;
    if (!readable) return false;

    const auto begin = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
    const auto end = begin + static_cast<std::uintptr_t>(mbi.RegionSize);
    if (address < begin || address + bytes < address || address + bytes > end) return false;

    region.begin = begin;
    region.end = end;
    return true;
}

template <typename T>
bool ReadGameMemory(std::uintptr_t address, T& value, ReadableRegion* region = nullptr)
{
    if (region != nullptr) {
        if (!IsInRegion(*region, address, sizeof(T)) && !RefreshReadableRegion(address, sizeof(T), *region)) {
            return false;
        }
    } else if (!IsReadableMemory(reinterpret_cast<const void*>(address), sizeof(T))) {
        return false;
    }
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
    return ReadGameMemory(g_cs_fe_man_static_address, fe_man, &g_cs_fe_man_static_region) ? fe_man : 0;
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
    if (!ReadGameMemory(fe_man + kHudStateOffset, hud_state, &g_cs_fe_man_hud_region)) {
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
