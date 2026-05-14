#include "game/equipment/radial_slots.h"

#include "core/common.h"
#include "game/equipment/equip_access.h"

#include <windows.h>

#include <array>
#include <cstdio>
#include <string>
#include <utility>

namespace radial_menu_mod {

namespace {

using namespace equip_access;

using SpellSlotIds = std::array<std::uint32_t, kMaxSpellSlots>;
using QuickItemIds = std::array<std::uint32_t, kMaxQuickItemSlots>;

struct SpellCacheSignature {
    std::uintptr_t equip_magic_data = 0;
    SpellSlotIds ids = {};
};

struct QuickItemCacheSignature {
    std::uintptr_t equip_item_data = 0;
    QuickItemIds ids = {};
};

bool g_logged_no_equip_data = false;
std::vector<std::uint32_t> g_last_spell_log_signature;
std::vector<std::uint32_t> g_last_item_log_signature;
std::vector<RadialSlot> g_cached_spells;
std::vector<RadialSlot> g_cached_quick_items;
bool g_cached_spells_valid = false;
bool g_cached_quick_items_valid = false;
SpellCacheSignature g_cached_spells_signature = {};
QuickItemCacheSignature g_cached_quick_items_signature = {};
ULONGLONG g_last_slow_spell_slots_log_ms = 0;
ULONGLONG g_last_slow_item_slots_log_ms = 0;

void LogSlowSlotDuration(const char* label, ULONGLONG start_ms, ULONGLONG& last_log_ms)
{
    const ULONGLONG now = GetTickCount64();
    const ULONGLONG elapsed = now - start_ms;
    if (elapsed < 16) return;
    if (last_log_ms != 0 && now - last_log_ms < 2000) return;

    last_log_ms = now;
    Log("Timing: %s took %llums.", label, static_cast<unsigned long long>(elapsed));
}

void LogSlotSummaryIfChanged(const char* label, const std::vector<RadialSlot>& slots, std::vector<std::uint32_t>& last_signature)
{
    std::vector<std::uint32_t> signature;
    signature.reserve(slots.size() * 3);
    for (const RadialSlot& slot : slots) {
        signature.push_back(static_cast<std::uint32_t>(slot.slot_index));
        signature.push_back(slot.id);
        signature.push_back(slot.icon_id);
    }

    if (signature == last_signature) return;
    last_signature = std::move(signature);

    std::string summary;
    summary.reserve(slots.size() * 24);
    for (const RadialSlot& slot : slots) {
        char entry[48] = {};
        std::snprintf(entry, sizeof(entry), "%s%u:%u", summary.empty() ? "" : ",", slot.id, slot.icon_id);
        summary += entry;
    }

    Log("%s radial entries changed (count=%zu ids_icons=[%s]).", label, slots.size(), summary.c_str());
}

void UpdateCurrentFlags(std::vector<RadialSlot>& slots, std::size_t current_slot)
{
    for (RadialSlot& slot : slots) {
        slot.is_current = slot.slot_index == current_slot;
    }
}

int DisplayIndexForSlot(const std::vector<RadialSlot>& slots, std::size_t current_slot)
{
    for (std::size_t i = 0; i < slots.size(); ++i) {
        if (slots[i].slot_index == current_slot) return static_cast<int>(i);
    }
    return -1;
}

bool ReadSpellSignature(std::uintptr_t equip_magic_data, SpellCacheSignature& signature)
{
    signature = {};
    signature.equip_magic_data = equip_magic_data;

    const auto spell_slots = equip_magic_data + kFirstMagicSlotOffset;
    if (!IsReadableMemory(reinterpret_cast<const void*>(spell_slots), kMaxSpellSlots * kSpellEntryStride)) {
        return false;
    }

    for (std::size_t i = 0; i < kMaxSpellSlots; ++i) {
        signature.ids[i] = *reinterpret_cast<const std::uint32_t*>(spell_slots + i * kSpellEntryStride + kSpellIdOffset);
    }
    return true;
}

bool ReadQuickItemSignature(std::uintptr_t equip_item_data, const QuickItemInventorySnapshot& quick_items,
    QuickItemCacheSignature& signature)
{
    signature = {};
    signature.equip_item_data = equip_item_data;
    return ReadQuickItemIds(quick_items, signature.ids.data(), signature.ids.size());
}

}  // namespace

bool InitializeRadialSlots()
{
    InitializeSpellMetadata();
    InitializeItemMetadata();
    const auto game_data_man = ResolveGameDataManAddress();
    if (!game_data_man) {
        Log("Failed to resolve GameDataMan; memorized spells will be unavailable.");
        return false;
    }
    return true;
}

void InvalidateRadialSlotCaches()
{
    g_cached_spells.clear();
    g_cached_quick_items.clear();
    g_cached_spells_valid = false;
    g_cached_quick_items_valid = false;
    g_cached_spells_signature = {};
    g_cached_quick_items_signature = {};
}

int GetCurrentSpellSlot()
{
    const auto equip_magic_data = ResolveEquipMagicData();
    if (!equip_magic_data) return -1;
    std::int32_t entry_slot = -1;
    if (!ReadSelectedSpellSlot(equip_magic_data, entry_slot)) return -1;

    if (g_cached_spells_valid && equip_magic_data == g_cached_spells_signature.equip_magic_data) {
        return DisplayIndexForSlot(g_cached_spells, static_cast<std::size_t>(entry_slot));
    }

    const auto spell_slots = equip_magic_data + kFirstMagicSlotOffset;
    if (!IsReadableMemory(reinterpret_cast<const void*>(spell_slots), kMaxSpellSlots * kSpellEntryStride)) return -1;

    int display = 0;
    for (std::size_t i = 0; i < kMaxSpellSlots; ++i) {
        const auto spell_id = *reinterpret_cast<const std::uint32_t*>(spell_slots + i * kSpellEntryStride + kSpellIdOffset);
        if (spell_id == 0 || spell_id == 0xFFFFFFFFu) continue;
        if (static_cast<int>(i) == entry_slot) return display;
        ++display;
    }
    return -1;
}

std::vector<RadialSlot> GetMemorizedSpells()
{
    const ULONGLONG start = GetTickCount64();
    std::vector<RadialSlot> spells;
    const auto equip_magic_data = ResolveEquipMagicData();
    if (!equip_magic_data) {
        if (!g_logged_no_equip_data) {
            Log("EquipMagicData unresolved; radial menu will be empty.");
            g_logged_no_equip_data = true;
        }
        LogSlowSlotDuration("GetMemorizedSpells", start, g_last_slow_spell_slots_log_ms);
        return spells;
    }
    g_logged_no_equip_data = false;

    std::int32_t current_entry = -1;
    ReadSelectedSpellSlot(equip_magic_data, current_entry);
    if (g_cached_spells_valid && equip_magic_data == g_cached_spells_signature.equip_magic_data) {
        spells = g_cached_spells;
        if (current_entry >= 0) UpdateCurrentFlags(spells, static_cast<std::size_t>(current_entry));
        LogSlowSlotDuration("GetMemorizedSpells", start, g_last_slow_spell_slots_log_ms);
        return spells;
    }

    SpellCacheSignature signature{};
    if (!ReadSpellSignature(equip_magic_data, signature)) {
        LogSlowSlotDuration("GetMemorizedSpells", start, g_last_slow_spell_slots_log_ms);
        return spells;
    }

    for (std::size_t i = 0; i < kMaxSpellSlots; ++i) {
        const auto spell_id = signature.ids[i];
        if (spell_id == 0 || spell_id == 0xFFFFFFFFu) continue;
        const auto meta = ResolveSpellMetadata(spell_id);
        spells.push_back({
            .slot_index = i,
            .id         = spell_id,
            .name       = meta.name,
            .icon_id    = meta.icon_id,
            .category   = meta.category,
            .is_item    = false,
            .occupied   = true,
            .is_current = (static_cast<int>(i) == current_entry),
        });
    }
    LogSlotSummaryIfChanged("Spell", spells, g_last_spell_log_signature);
    g_cached_spells = spells;
    g_cached_spells_signature = signature;
    g_cached_spells_valid = true;
    LogSlowSlotDuration("GetMemorizedSpells", start, g_last_slow_spell_slots_log_ms);
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
    if (!ReadSelectedQuickItemSlot(equip_item_data, slot)) return -1;

    if (g_cached_quick_items_valid && equip_item_data == g_cached_quick_items_signature.equip_item_data) {
        return DisplayIndexForSlot(g_cached_quick_items, static_cast<std::size_t>(slot));
    }

    QuickItemInventorySnapshot quick_items{};
    if (!ReadQuickItemInventorySnapshot(equip_item_data, quick_items)) return -1;

    if (!ReadQuickItemId(quick_items, static_cast<std::size_t>(slot))) return -1;

    int display = 0;
    for (std::size_t i = 0; i < kMaxQuickItemSlots; ++i) {
        if (!ReadQuickItemId(quick_items, i)) continue;
        if (static_cast<int>(i) == slot) return display;
        ++display;
    }
    return -1;
}

std::vector<RadialSlot> GetQuickItems()
{
    const ULONGLONG start = GetTickCount64();
    std::vector<RadialSlot> items;
    const auto equip_item_data = ResolveEquipItemData();
    if (!equip_item_data) {
        LogSlowSlotDuration("GetQuickItems", start, g_last_slow_item_slots_log_ms);
        return items;
    }

    std::int32_t current_slot = -1;
    ReadSelectedQuickItemSlot(equip_item_data, current_slot);
    if (g_cached_quick_items_valid && equip_item_data == g_cached_quick_items_signature.equip_item_data) {
        items = g_cached_quick_items;
        if (current_slot >= 0) UpdateCurrentFlags(items, static_cast<std::size_t>(current_slot));
        LogSlowSlotDuration("GetQuickItems", start, g_last_slow_item_slots_log_ms);
        return items;
    }

    QuickItemInventorySnapshot quick_items{};
    if (!ReadQuickItemInventorySnapshot(equip_item_data, quick_items)) {
        LogSlowSlotDuration("GetQuickItems", start, g_last_slow_item_slots_log_ms);
        return items;
    }

    QuickItemCacheSignature signature{};
    ReadQuickItemSignature(equip_item_data, quick_items, signature);

    for (std::size_t i = 0; i < kMaxQuickItemSlots; ++i) {
        const auto item_id = signature.ids[i];
        if (!item_id) continue;

        const auto meta = ResolveItemMetadata(item_id);
        items.push_back({
            .slot_index = i,
            .id = item_id,
            .name = meta.name,
            .icon_id = meta.icon_id,
            .category = SpellCategory::unknown,
            .is_item = true,
            .occupied = true,
            .is_current = (current_slot == static_cast<std::int32_t>(i)),
        });
    }
    LogSlotSummaryIfChanged("Quick item", items, g_last_item_log_signature);
    g_cached_quick_items = items;
    g_cached_quick_items_signature = signature;
    g_cached_quick_items_valid = true;
    LogSlowSlotDuration("GetQuickItems", start, g_last_slow_item_slots_log_ms);
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
