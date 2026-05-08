#pragma once

#include "spell_manager.h"

namespace radial_spell_menu::input_hook {

bool Install();
void Shutdown();
bool IsMenuOpen();
const std::vector<SpellSlot>& GetOpenSpellSlots();
const char* GetOpenMenuTitle();
const char* GetOpenMenuControls();

}  // namespace radial_spell_menu::input_hook
