#pragma once

#include "game/equipment/spell_manager.h"

#include <windows.h>
#include <xinput.h>

#include <vector>

namespace radial_menu_mod::radial_input {

void Reset();
void HandleControllerState(DWORD user_index, XINPUT_STATE* state);
void HandleKeyboardMouseState(bool spell_held, bool item_held);
void AddMouseDelta(float delta_x, float delta_y);
const std::vector<SpellSlot>& GetOpenSpellSlots();
const char* GetOpenMenuTitle();
const char* GetOpenMenuControls();

}  // namespace radial_menu_mod::radial_input
