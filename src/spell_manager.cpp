#include "spell_manager.h"

#include "common.h"

#include <windows.h>

namespace radial_spell_menu {

namespace {

constexpr std::size_t    kMaxSpellSlots   = 14;
constexpr std::size_t    kMaxQuickItemSlots = 10;
constexpr std::uintptr_t kSpellEntryStride = sizeof(std::uint64_t);
constexpr std::uintptr_t kSpellIdOffset    = 0x0;

constexpr std::array<std::uint8_t, 18> kGameDataManPattern = {
    0x48, 0x8B, 0x05, 0x00, 0x00, 0x00, 0x00, 0x48, 0x85, 0xC0, 0x74, 0x05, 0x48, 0x8B, 0x40, 0x58, 0xC3, 0xC3,
};
constexpr std::array<bool, 18> kGameDataManMask = {
    true, true, true, false, false, false, false, true, true, true, true, true, true, true, true, true, true, true,
};

constexpr std::uintptr_t kGameDataRootOffset  = 0x08;
constexpr std::uintptr_t kEquipMagicDataOffset = 0x530;
constexpr std::uintptr_t kEquipItemDataOffset  = 0x538;
constexpr std::uintptr_t kFirstMagicSlotOffset = 0x10;
constexpr std::uintptr_t kSelectedSlotOffset   = 0x80;
constexpr std::uintptr_t kFirstQuickItemSlotOffset = 0x08;
constexpr std::uintptr_t kQuickItemSlotStride = sizeof(std::uint64_t);
constexpr std::uintptr_t kQuickItemSlotInventoryIndexOffset = 0x04;
constexpr std::uintptr_t kQuickItemInventoryOffset = 0x98;
constexpr std::uintptr_t kSelectedQuickItemSlotOffset = 0xA0;
constexpr std::uintptr_t kInventoryItemsDataOffset = 0x08;
constexpr std::uintptr_t kNormalItemsCapacityOffset = kInventoryItemsDataOffset + 0x04;
constexpr std::uintptr_t kNormalItemsHeadOffset = kInventoryItemsDataOffset + 0x08;
constexpr std::uintptr_t kKeyItemsCapacityOffset = kInventoryItemsDataOffset + 0x14;
constexpr std::uintptr_t kCurrentKeyItemsHeadOffset = kInventoryItemsDataOffset + 0x48;
constexpr std::uintptr_t kCurrentKeyItemsLengthPtrOffset = kInventoryItemsDataOffset + 0x50;
constexpr std::uintptr_t kInventoryEntryStride = 0x18;
constexpr std::uintptr_t kInventoryEntryItemIdOffset = 0x04;

std::uintptr_t g_game_data_man_address  = 0;
bool           g_logged_no_equip_data   = false;

bool IsReadablePointer(const void* ptr, std::size_t bytes)
{
    MEMORY_BASIC_INFORMATION mbi{};
    if (!ptr || VirtualQuery(ptr, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD) || (mbi.Protect & PAGE_NOACCESS)) return false;

    const DWORD protect = mbi.Protect & 0xFFu;
    const bool readable = protect == PAGE_READONLY || protect == PAGE_READWRITE || protect == PAGE_WRITECOPY ||
                          protect == PAGE_EXECUTE_READ || protect == PAGE_EXECUTE_READWRITE || protect == PAGE_EXECUTE_WRITECOPY;
    if (!readable) return false;

    const auto begin = reinterpret_cast<std::uintptr_t>(ptr);
    const auto region_begin = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
    const auto region_end = region_begin + static_cast<std::uintptr_t>(mbi.RegionSize);
    return begin >= region_begin && begin + bytes >= begin && begin + bytes <= region_end;
}

template <typename T>
bool ReadMemory(std::uintptr_t address, T& value)
{
    if (!IsReadablePointer(reinterpret_cast<const void*>(address), sizeof(T))) return false;
    value = *reinterpret_cast<const T*>(address);
    return true;
}

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
    const auto gdm = ResolveGameDataManAddress();
    if (!gdm) return 0;
    std::uintptr_t manager = 0;
    if (!ReadMemory(gdm, manager)) return 0;
    if (!manager) return 0;
    std::uintptr_t root = 0;
    if (!ReadMemory(manager + kGameDataRootOffset, root)) return 0;
    if (!root) return 0;
    std::uintptr_t equip_magic_data = 0;
    return ReadMemory(root + kEquipMagicDataOffset, equip_magic_data) ? equip_magic_data : 0;
}

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

std::uintptr_t ResolveEquipItemData()
{
    const auto root = ResolveGameDataRoot();
    return root ? root + kEquipItemDataOffset : 0;
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

std::uint32_t ReadQuickItemId(std::uintptr_t eid, std::size_t slot)
{
    if (!eid || slot >= kMaxQuickItemSlots) return 0;

    const auto quick_slot = eid + kFirstQuickItemSlotOffset + slot * kQuickItemSlotStride;
    std::int32_t inventory_index = -1;
    if (!ReadMemory(quick_slot + kQuickItemSlotInventoryIndexOffset, inventory_index)) return 0;
    if (inventory_index < 0) return 0;

    std::uintptr_t inventory = 0;
    if (!ReadMemory(eid + kQuickItemInventoryOffset, inventory)) return 0;
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

int ResolveCurrentSpellEntrySlot(std::uintptr_t emd)
{
    std::int32_t slot = -1;
    if (!ReadMemory(emd + kSelectedSlotOffset, slot)) return -1;
    if (slot < 0 || slot >= static_cast<int>(kMaxSpellSlots)) return -1;
    const auto entry    = emd + kFirstMagicSlotOffset + static_cast<std::size_t>(slot) * kSpellEntryStride;
    std::uint32_t spell_id = 0;
    if (!ReadMemory(entry + kSpellIdOffset, spell_id)) return -1;
    return (spell_id == 0 || spell_id == 0xFFFFFFFFu) ? -1 : slot;
}

}  // namespace

bool InitializeSpellManager()
{
    InitializeSpellMetadata();
    const auto gdm = ResolveGameDataManAddress();
    if (!gdm) {
        Log("Failed to resolve GameDataMan; memorized spells will be unavailable.");
        return false;
    }
    Log("Spell manager initialized (GameDataMan at 0x%p).", reinterpret_cast<void*>(gdm));
    return true;
}

int GetCurrentSpellSlot()
{
    const auto emd = ResolveEquipMagicData();
    if (!emd) return -1;
    const int entry_slot = ResolveCurrentSpellEntrySlot(emd);
    if (entry_slot < 0) return -1;

    int display = 0;
    for (std::size_t i = 0; i < kMaxSpellSlots; ++i) {
        std::uint32_t spell_id = 0;
        if (!ReadMemory(emd + kFirstMagicSlotOffset + i * kSpellEntryStride + kSpellIdOffset, spell_id)) continue;
        if (spell_id == 0 || spell_id == 0xFFFFFFFFu) continue;
        if (static_cast<int>(i) == entry_slot) return display;
        ++display;
    }
    return -1;
}

std::vector<SpellSlot> GetMemorizedSpells()
{
    std::vector<SpellSlot> spells;
    const auto emd = ResolveEquipMagicData();
    if (!emd) {
        if (!g_logged_no_equip_data) {
            Log("EquipMagicData unresolved; radial menu will be empty.");
            g_logged_no_equip_data = true;
        }
        return spells;
    }
    g_logged_no_equip_data = false;

    const int current_entry = ResolveCurrentSpellEntrySlot(emd);
    for (std::size_t i = 0; i < kMaxSpellSlots; ++i) {
        std::uint32_t spell_id = 0;
        if (!ReadMemory(emd + kFirstMagicSlotOffset + i * kSpellEntryStride + kSpellIdOffset, spell_id)) continue;
        if (spell_id == 0 || spell_id == 0xFFFFFFFFu) continue;
        const auto meta = ResolveSpellMetadata(spell_id);
        spells.push_back({
            .slot_index = i,
            .spell_id   = spell_id,
            .name       = meta.name,
            .icon_id    = meta.icon_id,
            .category   = meta.category,
            .is_item    = false,
            .occupied   = true,
            .is_current = (static_cast<int>(i) == current_entry),
        });
    }
    return spells;
}

bool SwitchToSpellSlot(std::size_t slot_index)
{
    const auto emd = ResolveEquipMagicData();
    if (!emd) { Log("Spell switch skipped: EquipMagicData unresolved."); return false; }
    if (slot_index >= kMaxSpellSlots) { Log("Spell switch skipped: slot %zu out of range.", slot_index); return false; }

    std::uint32_t spell_id = 0;
    if (!ReadMemory(emd + kFirstMagicSlotOffset + slot_index * kSpellEntryStride + kSpellIdOffset, spell_id)) {
        Log("Spell switch skipped: slot %zu could not be read.", slot_index);
        return false;
    }
    if (spell_id == 0 || spell_id == 0xFFFFFFFFu) {
        Log("Spell switch skipped: slot %zu is empty.", slot_index);
        return false;
    }

    auto* selected = reinterpret_cast<std::int32_t*>(emd + kSelectedSlotOffset);
    *selected = static_cast<std::int32_t>(slot_index);
    if (*selected == static_cast<std::int32_t>(slot_index)) return true;

    Log("Spell switch write did not persist for slot %zu.", slot_index);
    return false;
}

int GetCurrentQuickItemSlot()
{
    const auto eid = ResolveEquipItemData();
    if (!eid) return -1;
    std::int32_t slot = -1;
    if (!ReadMemory(eid + kSelectedQuickItemSlotOffset, slot)) return -1;
    if (slot < 0 || slot >= static_cast<int>(kMaxQuickItemSlots)) return -1;

    if (!ReadQuickItemId(eid, static_cast<std::size_t>(slot))) return -1;

    int display = 0;
    for (std::size_t i = 0; i < kMaxQuickItemSlots; ++i) {
        if (!ReadQuickItemId(eid, i)) continue;
        if (static_cast<int>(i) == slot) return display;
        ++display;
    }
    return -1;
}

std::vector<SpellSlot> GetQuickItems()
{
    std::vector<SpellSlot> items;
    const auto eid = ResolveEquipItemData();
    if (!eid) return items;

    std::int32_t current_slot = -1;
    ReadMemory(eid + kSelectedQuickItemSlotOffset, current_slot);
    for (std::size_t i = 0; i < kMaxQuickItemSlots; ++i) {
        const auto item_id = ReadQuickItemId(eid, i);
        if (!item_id) continue;

        const auto meta = ResolveItemMetadata(item_id);
        items.push_back({
            .slot_index = i,
            .spell_id = item_id,
            .name = meta.name,
            .icon_id = meta.icon_id,
            .category = SpellCategory::unknown,
            .is_item = true,
            .occupied = true,
            .is_current = (current_slot == static_cast<std::int32_t>(i)),
        });
    }
    return items;
}

bool SwitchToQuickItemSlot(std::size_t slot_index)
{
    const auto eid = ResolveEquipItemData();
    if (!eid || slot_index >= kMaxQuickItemSlots || !ReadQuickItemId(eid, slot_index)) return false;

    auto* selected = reinterpret_cast<std::int32_t*>(eid + kSelectedQuickItemSlotOffset);
    *selected = static_cast<std::int32_t>(slot_index);
    if (*selected == static_cast<std::int32_t>(slot_index)) return true;
    return false;
}

}  // namespace radial_spell_menu
