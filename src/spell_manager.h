#pragma once

#include "spell_metadata.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace radial_spell_menu {

struct SpellSlot {
    std::size_t slot_index = 0;
    std::uint32_t spell_id = 0;
    std::string name;
    std::uint32_t icon_id = 0;
    SpellCategory category = SpellCategory::unknown;
    bool is_item = false;
    bool occupied = false;
    bool is_current = false;
};

bool InitializeSpellManager();
int GetCurrentSpellSlot();
std::vector<SpellSlot> GetMemorizedSpells();
bool SwitchToSpellSlot(std::size_t slot_index);
int GetCurrentQuickItemSlot();
std::vector<SpellSlot> GetQuickItems();
bool SwitchToQuickItemSlot(std::size_t slot_index);

}  // namespace radial_spell_menu
