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
    bool occupied = false;
    bool is_current = false;
};

struct SpellOffsets {
    std::uintptr_t local_player_rva = 0;
    std::uintptr_t current_spell_slot_offset = 0;
    std::uintptr_t memorized_spells_offset = 0;
    std::uintptr_t spell_entry_stride = sizeof(std::uint64_t);
    std::uintptr_t spell_id_offset = 0;
    std::uintptr_t equip_spell_function_rva = 0;
    std::size_t max_slots = 14;
};

bool InitializeSpellManager();
void SetOffsets(const SpellOffsets& offsets);
const SpellOffsets& GetOffsets();
int GetCurrentSpellSlot();
std::vector<SpellSlot> GetMemorizedSpells();
bool SwitchToSpellSlot(std::size_t slot_index);

}  // namespace radial_spell_menu
