#pragma once

#include "game/equipment/spell_manager.h"

#include <windows.h>
#include <xinput.h>

#include <vector>

namespace radial_spell_menu::radial_input {

void Reset();
void HandleControllerState(DWORD user_index, XINPUT_STATE* state);
const std::vector<SpellSlot>& GetOpenSpellSlots();
const char* GetOpenMenuTitle();
const char* GetOpenMenuControls();

}  // namespace radial_spell_menu::radial_input
