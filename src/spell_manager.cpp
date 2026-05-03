#include "spell_manager.h"

#include "common.h"

#include <windows.h>

namespace radial_spell_menu {

namespace {

constexpr std::size_t    kMaxSpellSlots   = 14;
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
constexpr std::uintptr_t kFirstMagicSlotOffset = 0x10;
constexpr std::uintptr_t kSelectedSlotOffset   = 0x80;

std::uintptr_t g_game_data_man_address  = 0;
bool           g_logged_no_equip_data   = false;

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
    const auto manager = *reinterpret_cast<std::uintptr_t*>(gdm);
    if (!manager) return 0;
    const auto root = *reinterpret_cast<std::uintptr_t*>(manager + kGameDataRootOffset);
    if (!root) return 0;
    return *reinterpret_cast<std::uintptr_t*>(root + kEquipMagicDataOffset);
}

int ResolveCurrentSpellEntrySlot(std::uintptr_t emd)
{
    const auto slot = *reinterpret_cast<const std::int32_t*>(emd + kSelectedSlotOffset);
    if (slot < 0 || slot >= static_cast<int>(kMaxSpellSlots)) return -1;
    const auto entry    = emd + kFirstMagicSlotOffset + static_cast<std::size_t>(slot) * kSpellEntryStride;
    const auto spell_id = *reinterpret_cast<const std::uint32_t*>(entry + kSpellIdOffset);
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
        const auto spell_id = *reinterpret_cast<const std::uint32_t*>(
            emd + kFirstMagicSlotOffset + i * kSpellEntryStride + kSpellIdOffset);
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
        const auto spell_id = *reinterpret_cast<const std::uint32_t*>(
            emd + kFirstMagicSlotOffset + i * kSpellEntryStride + kSpellIdOffset);
        if (spell_id == 0 || spell_id == 0xFFFFFFFFu) continue;
        const auto meta = ResolveSpellMetadata(spell_id);
        spells.push_back({
            .slot_index = i,
            .spell_id   = spell_id,
            .name       = meta.name,
            .icon_id    = meta.icon_id,
            .category   = meta.category,
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

    const auto spell_id = *reinterpret_cast<const std::uint32_t*>(
        emd + kFirstMagicSlotOffset + slot_index * kSpellEntryStride + kSpellIdOffset);
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

}  // namespace radial_spell_menu
