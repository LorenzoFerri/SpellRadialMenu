#pragma once

#include "game/equipment/spell_manager.h"

#include <imgui.h>

namespace radial_spell_menu::radial_menu {

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
void UpdateSelectionFromStick(float stick_x, float stick_y, std::size_t slot_count);
void Draw(const std::vector<SpellSlot>& slots, const char* title, const char* controls);

}  // namespace radial_spell_menu::radial_menu
