#pragma once

#include "render/ui/radial_menu.h"

namespace radial_menu_mod::radial_menu {

void DrawMenuContents(
    const std::vector<RadialSlot>& slots,
    const char* title,
    const char* controls,
    int selected_slot,
    const std::vector<IconTextureInfo>& icon_textures);
void InvalidateMenuDrawCache();

}  // namespace radial_menu_mod::radial_menu
