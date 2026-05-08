#pragma once

#include "render/ui/radial_menu.h"

namespace radial_menu_mod::radial_menu {

void DrawMenuContents(
    const std::vector<SpellSlot>& slots,
    const char* title,
    const char* controls,
    int selected_slot,
    IconTextureInfo(*icon_texture_resolver)(std::uint32_t icon_id));

}  // namespace radial_menu_mod::radial_menu
