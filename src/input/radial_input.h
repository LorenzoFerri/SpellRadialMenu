#pragma once

#include "game/equipment/radial_slots.h"

#include <windows.h>

#include <vector>

namespace radial_menu_mod::radial_input {

void Reset();
void UpdateRadialHoldState(bool spell_hold_active, bool item_hold_active, float selection_x, float selection_y);
const std::vector<RadialSlot>& GetOpenRadialSlots();
const char* GetOpenMenuTitle();
const char* GetOpenMenuControls();

}  // namespace radial_menu_mod::radial_input
