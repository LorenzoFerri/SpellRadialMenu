#pragma once

#include "core/common.h"

#include <cstdint>

namespace radial_menu_mod::equip_access {

constexpr std::size_t kMaxSpellSlots = 14;
constexpr std::size_t kMaxQuickItemSlots = 10;
constexpr std::uintptr_t kSpellEntryStride = sizeof(std::uint64_t);
constexpr std::uintptr_t kSpellIdOffset = 0x0;
constexpr std::uintptr_t kFirstMagicSlotOffset = 0x10;
constexpr std::uintptr_t kSelectedSpellSlotOffset = 0x80;
constexpr std::uintptr_t kSelectedQuickItemSlotOffset = 0xA0;

template <typename T>
bool ReadMemory(std::uintptr_t address, T& value)
{
    if (!IsReadableMemory(reinterpret_cast<const void*>(address), sizeof(T))) return false;
    value = *reinterpret_cast<const T*>(address);
    return true;
}

std::uintptr_t ResolveGameDataManAddress();
std::uintptr_t ResolveEquipMagicData();
std::uintptr_t ResolveEquipItemData();
int ResolveCurrentSpellEntrySlot(std::uintptr_t equip_magic_data);
std::uint32_t ReadQuickItemId(std::uintptr_t equip_item_data, std::size_t slot);

}  // namespace radial_menu_mod::equip_access
