#include "game/equipment/spell_manager.h"

#include "core/common.h"
#include "game/equipment/equip_access.h"

#include <cstdio>
#include <string>
#include <utility>

namespace radial_menu_mod {

namespace {

using namespace equip_access;

bool g_logged_no_equip_data = false;
std::vector<std::uint32_t> g_last_spell_log_signature;
std::vector<std::uint32_t> g_last_item_log_signature;

void LogSlotSummaryIfChanged(const char* label, const std::vector<SpellSlot>& slots, std::vector<std::uint32_t>& last_signature)
{
    std::vector<std::uint32_t> signature;
    signature.reserve(slots.size() * 3);
    for (const SpellSlot& slot : slots) {
        signature.push_back(static_cast<std::uint32_t>(slot.slot_index));
        signature.push_back(slot.spell_id);
        signature.push_back(slot.icon_id);
    }

    if (signature == last_signature) return;
    last_signature = std::move(signature);

    std::string summary;
    summary.reserve(slots.size() * 24);
    for (const SpellSlot& slot : slots) {
        char entry[48] = {};
        std::snprintf(entry, sizeof(entry), "%s%u:%u", summary.empty() ? "" : ",", slot.spell_id, slot.icon_id);
        summary += entry;
    }

    Log("%s radial entries changed (count=%zu ids_icons=[%s]).", label, slots.size(), summary.c_str());
}

}  // namespace

bool InitializeSpellManager()
{
    InitializeSpellMetadata();
    const auto game_data_man = ResolveGameDataManAddress();
    if (!game_data_man) {
        Log("Failed to resolve GameDataMan; memorized spells will be unavailable.");
        return false;
    }
    return true;
}

int GetCurrentSpellSlot()
{
    const auto equip_magic_data = ResolveEquipMagicData();
    if (!equip_magic_data) return -1;
    const int entry_slot = ResolveCurrentSpellEntrySlot(equip_magic_data);
    if (entry_slot < 0) return -1;

    int display = 0;
    for (std::size_t i = 0; i < kMaxSpellSlots; ++i) {
        std::uint32_t spell_id = 0;
        if (!ReadMemory(equip_magic_data + kFirstMagicSlotOffset + i * kSpellEntryStride + kSpellIdOffset, spell_id)) continue;
        if (spell_id == 0 || spell_id == 0xFFFFFFFFu) continue;
        if (static_cast<int>(i) == entry_slot) return display;
        ++display;
    }
    return -1;
}

std::vector<SpellSlot> GetMemorizedSpells()
{
    std::vector<SpellSlot> spells;
    const auto equip_magic_data = ResolveEquipMagicData();
    if (!equip_magic_data) {
        if (!g_logged_no_equip_data) {
            Log("EquipMagicData unresolved; radial menu will be empty.");
            g_logged_no_equip_data = true;
        }
        return spells;
    }
    g_logged_no_equip_data = false;

    const int current_entry = ResolveCurrentSpellEntrySlot(equip_magic_data);
    for (std::size_t i = 0; i < kMaxSpellSlots; ++i) {
        std::uint32_t spell_id = 0;
        if (!ReadMemory(equip_magic_data + kFirstMagicSlotOffset + i * kSpellEntryStride + kSpellIdOffset, spell_id)) continue;
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
    LogSlotSummaryIfChanged("Spell", spells, g_last_spell_log_signature);
    return spells;
}

bool SwitchToSpellSlot(std::size_t slot_index)
{
    const auto equip_magic_data = ResolveEquipMagicData();
    if (!equip_magic_data) { Log("Spell switch skipped: EquipMagicData unresolved."); return false; }
    if (slot_index >= kMaxSpellSlots) { Log("Spell switch skipped: slot %zu out of range.", slot_index); return false; }

    std::uint32_t spell_id = 0;
    if (!ReadMemory(equip_magic_data + kFirstMagicSlotOffset + slot_index * kSpellEntryStride + kSpellIdOffset, spell_id)) {
        Log("Spell switch skipped: slot %zu could not be read.", slot_index);
        return false;
    }
    if (spell_id == 0 || spell_id == 0xFFFFFFFFu) {
        Log("Spell switch skipped: slot %zu is empty.", slot_index);
        return false;
    }

    auto* selected = reinterpret_cast<std::int32_t*>(equip_magic_data + kSelectedSpellSlotOffset);
    *selected = static_cast<std::int32_t>(slot_index);
    if (*selected == static_cast<std::int32_t>(slot_index)) return true;

    Log("Spell switch write did not persist for slot %zu.", slot_index);
    return false;
}

int GetCurrentQuickItemSlot()
{
    const auto equip_item_data = ResolveEquipItemData();
    if (!equip_item_data) return -1;
    std::int32_t slot = -1;
    if (!ReadMemory(equip_item_data + kSelectedQuickItemSlotOffset, slot)) return -1;
    if (slot < 0 || slot >= static_cast<int>(kMaxQuickItemSlots)) return -1;

    if (!ReadQuickItemId(equip_item_data, static_cast<std::size_t>(slot))) return -1;

    int display = 0;
    for (std::size_t i = 0; i < kMaxQuickItemSlots; ++i) {
        if (!ReadQuickItemId(equip_item_data, i)) continue;
        if (static_cast<int>(i) == slot) return display;
        ++display;
    }
    return -1;
}

std::vector<SpellSlot> GetQuickItems()
{
    std::vector<SpellSlot> items;
    const auto equip_item_data = ResolveEquipItemData();
    if (!equip_item_data) return items;

    std::int32_t current_slot = -1;
    ReadMemory(equip_item_data + kSelectedQuickItemSlotOffset, current_slot);
    for (std::size_t i = 0; i < kMaxQuickItemSlots; ++i) {
        const auto item_id = ReadQuickItemId(equip_item_data, i);
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
    LogSlotSummaryIfChanged("Quick item", items, g_last_item_log_signature);
    return items;
}

bool SwitchToQuickItemSlot(std::size_t slot_index)
{
    const auto equip_item_data = ResolveEquipItemData();
    if (!equip_item_data || slot_index >= kMaxQuickItemSlots || !ReadQuickItemId(equip_item_data, slot_index)) return false;

    auto* selected = reinterpret_cast<std::int32_t*>(equip_item_data + kSelectedQuickItemSlotOffset);
    *selected = static_cast<std::int32_t>(slot_index);
    if (*selected == static_cast<std::int32_t>(slot_index)) return true;
    return false;
}

}  // namespace radial_menu_mod
