#pragma once

#include "game/metadata/item_metadata.h"
#include "game/metadata/spell_metadata.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace radial_menu_mod {

struct RadialSlot {
    std::size_t slot_index = 0;
    std::uint32_t id = 0;
    std::string name;
    std::uint32_t icon_id = 0;
    SpellCategory category = SpellCategory::unknown;
    bool is_item = false;
    bool occupied = false;
    bool is_current = false;
};

bool InitializeRadialSlots();
void InvalidateRadialSlotCaches();
void RefreshRadialSlotCachesIfChanged();
int GetCurrentSpellSlot();
std::vector<RadialSlot> GetMemorizedSpells();
bool SwitchToSpellSlot(std::size_t slot_index);
int GetCurrentQuickItemSlot();
std::vector<RadialSlot> GetQuickItems();
bool SwitchToQuickItemSlot(std::size_t slot_index);

}  // namespace radial_menu_mod
