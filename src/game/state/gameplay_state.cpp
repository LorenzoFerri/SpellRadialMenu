#include "game/state/gameplay_state.h"

#include "core/common.h"
#include "game/state/singleton_resolver.h"

#include <cstdint>

namespace radial_spell_menu::gameplay_state {
namespace {

std::uintptr_t g_cs_fe_man_static_address = 0;
bool g_searched_cs_fe_man = false;
bool g_logged_missing_cs_fe_man = false;

std::uintptr_t ResolveCSFeMan()
{
    if (!g_searched_cs_fe_man) {
        g_searched_cs_fe_man = true;
        g_cs_fe_man_static_address = singleton_resolver::ResolveSingletonStaticAddress("CSFeMan");
        if (g_cs_fe_man_static_address) {
            Log("CSFeMan singleton resolved at 0x%p.", reinterpret_cast<void*>(g_cs_fe_man_static_address));
        }
    }

    if (!g_cs_fe_man_static_address) return 0;
    return *reinterpret_cast<std::uintptr_t*>(g_cs_fe_man_static_address);
}

}  // namespace

bool IsNormalGameplayHudState()
{
    constexpr std::uintptr_t kHudStateOffset = 0x78;
    constexpr std::uint8_t kHudStateDefault = 3;

    const auto fe_man = ResolveCSFeMan();
    if (!fe_man) {
        if (!g_logged_missing_cs_fe_man) {
            g_logged_missing_cs_fe_man = true;
            Log("CSFeMan unresolved; radial menu input is disabled until HUD state can be read.");
        }
        return false;
    }

    return *reinterpret_cast<const std::uint8_t*>(fe_man + kHudStateOffset) == kHudStateDefault;
}

}  // namespace radial_spell_menu::gameplay_state
