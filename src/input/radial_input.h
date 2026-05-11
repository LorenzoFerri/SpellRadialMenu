#pragma once

#include "game/equipment/spell_manager.h"

#include <windows.h>

#include <vector>

namespace radial_menu_mod::radial_input {

void Reset();
void HandleActionState(bool spell_held, bool item_held, float right_stick_x, float right_stick_y);
void GetActionSuppressionState(bool& suppress_spell_switch, bool& suppress_item_switch);
const std::vector<SpellSlot>& GetOpenSpellSlots();
const char* GetOpenMenuTitle();
const char* GetOpenMenuControls();

}  // namespace radial_menu_mod::radial_input
