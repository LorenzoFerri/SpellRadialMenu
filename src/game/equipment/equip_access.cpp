#include "equip_access.h"

#include <array>

namespace radial_menu_mod::equip_access {
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
CachedReadableRegion g_game_data_man_region = {};
CachedReadableRegion g_game_data_root_region = {};
CachedReadableRegion g_equip_magic_data_region = {};
CachedReadableRegion g_selected_spell_slot_region = {};
CachedReadableRegion g_selected_quick_item_slot_region = {};
CachedReadableRegion g_current_spell_id_region = {};

std::uintptr_t ResolveGameDataRoot()
{
    const auto gdm = ResolveGameDataManAddress();
    if (!gdm) return 0;
    std::uintptr_t manager = 0;
    if (!ReadCachedMemory(gdm, manager, g_game_data_man_region)) return 0;
    if (!manager) return 0;
    std::uintptr_t root = 0;
    return ReadCachedMemory(manager + kGameDataRootOffset, root, g_game_data_root_region) ? root : 0;
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

bool ReadQuickSlotInventoryIndex(std::uintptr_t equip_item_data, std::size_t slot, std::int32_t& inventory_index)
{
    if (!equip_item_data || slot >= kMaxQuickItemSlots) return false;

    const auto quick_slot = equip_item_data + kFirstQuickItemSlotOffset + slot * kQuickItemSlotStride;
    return ReadMemory(quick_slot + kQuickItemSlotInventoryIndexOffset, inventory_index);
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
    return ReadCachedMemory(root + kEquipMagicDataOffset, equip_magic_data, g_equip_magic_data_region) ?
        equip_magic_data : 0;
}

std::uintptr_t ResolveEquipItemData()
{
    const auto root = ResolveGameDataRoot();
    return root ? root + kEquipItemDataOffset : 0;
}

bool ReadSelectedSpellSlot(std::uintptr_t equip_magic_data, std::int32_t& slot)
{
    slot = -1;
    if (!equip_magic_data || !ReadCachedMemory(equip_magic_data + kSelectedSpellSlotOffset, slot,
        g_selected_spell_slot_region)) {
        return false;
    }
    return slot >= 0 && slot < static_cast<int>(kMaxSpellSlots);
}

bool ReadSelectedQuickItemSlot(std::uintptr_t equip_item_data, std::int32_t& slot)
{
    slot = -1;
    if (!equip_item_data || !ReadCachedMemory(equip_item_data + kSelectedQuickItemSlotOffset, slot,
        g_selected_quick_item_slot_region)) {
        return false;
    }
    return slot >= 0 && slot < static_cast<int>(kMaxQuickItemSlots);
}

int ResolveCurrentSpellEntrySlot(std::uintptr_t equip_magic_data)
{
    std::int32_t slot = -1;
    if (!ReadSelectedSpellSlot(equip_magic_data, slot)) return -1;

    const auto entry = equip_magic_data + kFirstMagicSlotOffset + static_cast<std::size_t>(slot) * kSpellEntryStride;
    std::uint32_t spell_id = 0;
    if (!ReadCachedMemory(entry + kSpellIdOffset, spell_id, g_current_spell_id_region)) return -1;
    return (spell_id == 0 || spell_id == 0xFFFFFFFFu) ? -1 : slot;
}

std::uint32_t ReadQuickItemId(std::uintptr_t equip_item_data, std::size_t slot)
{
    QuickItemInventorySnapshot snapshot{};
    return ReadQuickItemInventorySnapshot(equip_item_data, snapshot) ? ReadQuickItemId(snapshot, slot) : 0;
}

bool ReadQuickItemInventorySnapshot(std::uintptr_t equip_item_data, QuickItemInventorySnapshot& snapshot)
{
    snapshot = {};
    if (!equip_item_data) return false;

    snapshot.equip_item_data = equip_item_data;

    std::uintptr_t inventory = 0;
    if (!ReadMemory(equip_item_data + kQuickItemInventoryOffset, inventory)) return false;
    if (!inventory) return false;

    ReadMemory(inventory + kCurrentKeyItemsHeadOffset, snapshot.key_items_head);
    std::uintptr_t key_items_length_ptr = 0;
    ReadMemory(inventory + kCurrentKeyItemsLengthPtrOffset, key_items_length_ptr);
    ReadMemory(key_items_length_ptr, snapshot.key_items_length);
    ReadMemory(inventory + kNormalItemsHeadOffset, snapshot.normal_items_head);
    ReadMemory(inventory + kNormalItemsCapacityOffset, snapshot.normal_capacity);
    ReadMemory(inventory + kKeyItemsCapacityOffset, snapshot.key_capacity);
    return snapshot.key_capacity != 0 && snapshot.key_capacity <= 10000;
}

std::uint32_t ReadQuickItemId(const QuickItemInventorySnapshot& snapshot, std::size_t slot)
{
    if (!snapshot.equip_item_data || slot >= kMaxQuickItemSlots) return 0;

    std::int32_t inventory_index = -1;
    if (!ReadQuickSlotInventoryIndex(snapshot.equip_item_data, slot, inventory_index)) return 0;
    if (inventory_index < 0) return 0;

    const auto item_slot = static_cast<std::uint32_t>(inventory_index);
    if (item_slot < snapshot.key_capacity) {
        return ReadInventoryEntryGoodsId(snapshot.key_items_head, item_slot, snapshot.key_items_length);
    }

    const std::uint32_t normal_index = item_slot - snapshot.key_capacity;
    return ReadInventoryEntryGoodsId(snapshot.normal_items_head, normal_index, snapshot.normal_capacity);
}

bool ReadQuickItemIds(const QuickItemInventorySnapshot& snapshot, std::uint32_t* ids, std::size_t count)
{
    if (!ids || !snapshot.equip_item_data || count > kMaxQuickItemSlots) return false;
    for (std::size_t i = 0; i < count; ++i) ids[i] = 0;

    CachedReadableRegion quick_slot_region{};
    CachedReadableRegion key_item_region{};
    CachedReadableRegion normal_item_region{};
    const auto quick_slots = snapshot.equip_item_data + kFirstQuickItemSlotOffset;
    if (!EnsureCachedReadableMemory(quick_slots, count * kQuickItemSlotStride, quick_slot_region)) return false;

    for (std::size_t i = 0; i < count; ++i) {
        const auto quick_slot = quick_slots + i * kQuickItemSlotStride;
        const auto inventory_index = *reinterpret_cast<const std::int32_t*>(quick_slot + kQuickItemSlotInventoryIndexOffset);
        if (inventory_index < 0) continue;

        const auto item_slot = static_cast<std::uint32_t>(inventory_index);
        std::uintptr_t entry = 0;
        CachedReadableRegion* item_region = nullptr;
        if (item_slot < snapshot.key_capacity) {
            if (!snapshot.key_items_head || item_slot >= snapshot.key_items_length || snapshot.key_items_length > 10000) continue;
            entry = snapshot.key_items_head + static_cast<std::uintptr_t>(item_slot) * kInventoryEntryStride;
            item_region = &key_item_region;
        } else {
            const std::uint32_t normal_index = item_slot - snapshot.key_capacity;
            if (!snapshot.normal_items_head || normal_index >= snapshot.normal_capacity || snapshot.normal_capacity > 10000) continue;
            entry = snapshot.normal_items_head + static_cast<std::uintptr_t>(normal_index) * kInventoryEntryStride;
            item_region = &normal_item_region;
        }

        const auto item_id_address = entry + kInventoryEntryItemIdOffset;
        if (!EnsureCachedReadableMemory(item_id_address, sizeof(std::uint32_t), *item_region)) continue;
        ids[i] = UnpackGoodsItemId(*reinterpret_cast<const std::uint32_t*>(item_id_address));
    }

    return true;
}

}  // namespace radial_menu_mod::equip_access
