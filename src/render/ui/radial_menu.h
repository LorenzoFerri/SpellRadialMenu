#pragma once

#include "game/equipment/radial_slots.h"

#include <imgui.h>

namespace radial_menu_mod::radial_menu {

struct IconTextureInfo {
    ImTextureID texture = ImTextureID{};
    ImVec2 uv_min = {0.0f, 0.0f};
    ImVec2 uv_max = {1.0f, 1.0f};
};

void SetIconTextureResolver(IconTextureInfo(*resolver)(std::uint32_t icon_id));
void Open(int initial_selection);
void Close();
bool IsOpen();
int GetSelectedSlot();
void UpdateSelectionFromDirection(float selection_x, float selection_y, std::size_t slot_count);
void Draw(const std::vector<RadialSlot>& slots, const char* title, const char* controls);

}  // namespace radial_menu_mod::radial_menu
