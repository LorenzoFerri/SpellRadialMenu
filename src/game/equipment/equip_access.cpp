#include "equip_access.h"

#include <array>

namespace radial_spell_menu::equip_access {
namespace {

constexpr std::array<std::uint8_t, 18> kGameDataManPattern = {
    0x48, 0x8B, 0x05, 0x00, 0x00, 0x00, 0x00, 0x48, 0x85, 0xC0, 0x74, 0x05, 0x48, 0x8B, 0x40, 0x58, 0xC3, 0xC3,
};
constexpr std::array<bool, 18> kGameDataManMask = {
    true, true, true, false, false, false, false, true, true, true, true, true, true, true, true, true, true, true,
};

constexpr std::uintptr_t kGameDataRootOffset = 0x08;
constexpr std::uintptr_t kEquipMagicDataOffset = 0x530;
constexpr std::uintptr_t kEquipItemDataOffset = 0x538;
constexpr std::uintptr_t kFirstQuickItemSlotOffset = 0x08;
constexpr std::uintptr_t kQuickItemSlotStride = sizeof(std::uint64_t);
constexpr std::uintptr_t kQuickItemSlotInventoryIndexOffset = 0x04;
constexpr std::uintptr_t kQuickItemInventoryOffset = 0x98;
constexpr std::uintptr_t kInventoryItemsDataOffset = 0x08;
constexpr std::uintptr_t kNormalItemsCapacityOffset = kInventoryItemsDataOffset + 0x04;
constexpr std::uintptr_t kNormalItemsHeadOffset = kInventoryItemsDataOffset + 0x08;
constexpr std::uintptr_t kKeyItemsCapacityOffset = kInventoryItemsDataOffset + 0x14;
constexpr std::uintptr_t kCurrentKeyItemsHeadOffset = kInventoryItemsDataOffset + 0x48;
constexpr std::uintptr_t kCurrentKeyItemsLengthPtrOffset = kInventoryItemsDataOffset + 0x50;
constexpr std::uintptr_t kInventoryEntryStride = 0x18;
constexpr std::uintptr_t kInventoryEntryItemIdOffset = 0x04;

std::uintptr_t g_game_data_man_address = 0;

std::uintptr_t ResolveGameDataRoot()
{
    const auto gdm = ResolveGameDataManAddress();
    if (!gdm) return 0;
    std::uintptr_t manager = 0;
    if (!ReadMemory(gdm, manager)) return 0;
    if (!manager) return 0;
    std::uintptr_t root = 0;
    return ReadMemory(manager + kGameDataRootOffset, root) ? root : 0;
}

std::uint32_t UnpackGoodsItemId(std::uint32_t packed_item_id)
{
    if (packed_item_id == 0xFFFFFFFFu || ((packed_item_id >> 28) & 0xFu) != 4u) return 0;
    return packed_item_id & 0x0FFFFFFFu;
}

std::uint32_t ReadInventoryEntryGoodsId(std::uintptr_t head, std::uint32_t index, std::uint32_t capacity)
{
    if (!head || index >= capacity || capacity > 10000) return 0;

    const auto entry = head + static_cast<std::uintptr_t>(index) * kInventoryEntryStride;
    std::uint32_t packed_item_id = 0;
    if (!ReadMemory(entry + kInventoryEntryItemIdOffset, packed_item_id)) return 0;
    return UnpackGoodsItemId(packed_item_id);
}

}  // namespace

std::uintptr_t ResolveGameDataManAddress()
{
    if (g_game_data_man_address) return g_game_data_man_address;
    const auto addr = FindPattern(kGameDataManPattern, kGameDataManMask);
    if (!addr) return 0;
    g_game_data_man_address = ResolveRipRelative(addr, 3, 7);
    return g_game_data_man_address;
}

std::uintptr_t ResolveEquipMagicData()
{
    const auto root = ResolveGameDataRoot();
    if (!root) return 0;
    std::uintptr_t equip_magic_data = 0;
    return ReadMemory(root + kEquipMagicDataOffset, equip_magic_data) ? equip_magic_data : 0;
}

std::uintptr_t ResolveEquipItemData()
{
    const auto root = ResolveGameDataRoot();
    return root ? root + kEquipItemDataOffset : 0;
}

int ResolveCurrentSpellEntrySlot(std::uintptr_t equip_magic_data)
{
    std::int32_t slot = -1;
    if (!ReadMemory(equip_magic_data + kSelectedSpellSlotOffset, slot)) return -1;
    if (slot < 0 || slot >= static_cast<int>(kMaxSpellSlots)) return -1;

    const auto entry = equip_magic_data + kFirstMagicSlotOffset + static_cast<std::size_t>(slot) * kSpellEntryStride;
    std::uint32_t spell_id = 0;
    if (!ReadMemory(entry + kSpellIdOffset, spell_id)) return -1;
    return (spell_id == 0 || spell_id == 0xFFFFFFFFu) ? -1 : slot;
}

std::uint32_t ReadQuickItemId(std::uintptr_t equip_item_data, std::size_t slot)
{
    if (!equip_item_data || slot >= kMaxQuickItemSlots) return 0;

    const auto quick_slot = equip_item_data + kFirstQuickItemSlotOffset + slot * kQuickItemSlotStride;
    std::int32_t inventory_index = -1;
    if (!ReadMemory(quick_slot + kQuickItemSlotInventoryIndexOffset, inventory_index)) return 0;
    if (inventory_index < 0) return 0;

    std::uintptr_t inventory = 0;
    if (!ReadMemory(equip_item_data + kQuickItemInventoryOffset, inventory)) return 0;
    if (!inventory) return 0;

    std::uintptr_t key_items_head = 0;
    std::uint32_t key_items_length = 0;
    std::uintptr_t normal_items_head = 0;
    std::uint32_t normal_capacity = 0;
    std::uint32_t key_capacity = 0;

    ReadMemory(inventory + kCurrentKeyItemsHeadOffset, key_items_head);
    std::uintptr_t key_items_length_ptr = 0;
    ReadMemory(inventory + kCurrentKeyItemsLengthPtrOffset, key_items_length_ptr);
    ReadMemory(key_items_length_ptr, key_items_length);
    ReadMemory(inventory + kNormalItemsHeadOffset, normal_items_head);
    ReadMemory(inventory + kNormalItemsCapacityOffset, normal_capacity);
    ReadMemory(inventory + kKeyItemsCapacityOffset, key_capacity);
    if (key_capacity == 0 || key_capacity > 10000) return 0;

    const auto item_slot = static_cast<std::uint32_t>(inventory_index);
    if (item_slot < key_capacity) {
        return ReadInventoryEntryGoodsId(key_items_head, item_slot, key_items_length);
    }

    const std::uint32_t normal_index = item_slot - key_capacity;
    return ReadInventoryEntryGoodsId(normal_items_head, normal_index, normal_capacity);
}

}  // namespace radial_spell_menu::equip_access
