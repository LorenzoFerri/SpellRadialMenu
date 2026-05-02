#include "spell_manager.h"

#include "common.h"
#include "spell_metadata.h"

#include <array>
#include <cstring>
#include <cstdint>
#include <windows.h>

namespace radial_spell_menu {

namespace {

SpellOffsets g_offsets = {
    .local_player_rva = 0x00000000,
    .current_spell_slot_offset = 0x00000000,
    .memorized_spells_offset = 0x00000000,
    .spell_entry_stride = sizeof(std::uint64_t),
    .spell_id_offset = 0x0,
    .equip_spell_function_rva = 0x00000000,
    .max_slots = 14,
};

constexpr std::array<std::uint8_t, 18> kGameDataManPattern = {
    0x48, 0x8B, 0x05, 0x00, 0x00, 0x00, 0x00, 0x48, 0x85, 0xC0, 0x74, 0x05, 0x48, 0x8B, 0x40, 0x58, 0xC3, 0xC3,
};
constexpr std::array<bool, 18> kGameDataManMask = {
    true, true, true, false, false, false, false, true, true, true, true, true, true, true, true, true, true, true,
};
constexpr std::uintptr_t kGameDataRootOffset = 0x08;
constexpr std::uintptr_t kEquipMagicDataOffset = 0x530;
constexpr std::uintptr_t kFirstMagicSlotOffset = 0x10;
constexpr std::uintptr_t kSelectedSlotOffset = 0x80;

std::uintptr_t g_game_data_man_address = 0;
bool g_logged_game_data_man = false;

std::uintptr_t GetModuleBase()
{
    return reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
}

std::size_t GetModuleSize()
{
    const auto base = GetModuleBase();
    if (base == 0) {
        return 0;
    }

    auto* const dos_header = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos_header->e_magic != IMAGE_DOS_SIGNATURE) {
        return 0;
    }

    auto* const nt_headers =
        reinterpret_cast<const IMAGE_NT_HEADERS*>(base + static_cast<std::uintptr_t>(dos_header->e_lfanew));
    if (nt_headers->Signature != IMAGE_NT_SIGNATURE) {
        return 0;
    }

    return nt_headers->OptionalHeader.SizeOfImage;
}

std::uintptr_t FindPattern(
    const std::array<std::uint8_t, kGameDataManPattern.size()>& pattern,
    const std::array<bool, kGameDataManMask.size()>& mask)
{
    const auto module_base = GetModuleBase();
    const auto module_size = GetModuleSize();

    if (module_base == 0 || module_size < pattern.size()) {
        return 0;
    }

    auto* const bytes = reinterpret_cast<const std::uint8_t*>(module_base);
    for (std::size_t i = 0; i <= (module_size - pattern.size()); ++i) {
        bool matches = true;
        for (std::size_t j = 0; j < pattern.size(); ++j) {
            if (mask[j] && bytes[i + j] != pattern[j]) {
                matches = false;
                break;
            }
        }

        if (matches) {
            return module_base + i;
        }
    }

    return 0;
}

std::uintptr_t ResolveRipRelativeTarget(std::uintptr_t instruction_address, std::uintptr_t displacement_offset, std::uintptr_t instruction_size)
{
    const auto displacement =
        *reinterpret_cast<const std::int32_t*>(instruction_address + displacement_offset);
    return instruction_address + instruction_size + displacement;
}

std::uintptr_t ResolveGameDataManAddress()
{
    if (g_game_data_man_address != 0) {
        return g_game_data_man_address;
    }

    const auto pattern_address = FindPattern(kGameDataManPattern, kGameDataManMask);
    if (pattern_address == 0) {
        return 0;
    }

    g_game_data_man_address = ResolveRipRelativeTarget(pattern_address, 3, 7);
    return g_game_data_man_address;
}

std::uintptr_t ResolveGameDataRoot()
{
    const auto game_data_man = ResolveGameDataManAddress();
    if (game_data_man == 0) {
        return 0;
    }

    const auto manager = *reinterpret_cast<std::uintptr_t*>(game_data_man);
    if (manager == 0) {
        return 0;
    }

    return *reinterpret_cast<std::uintptr_t*>(manager + kGameDataRootOffset);
}

std::uintptr_t ResolveEquipMagicData()
{
    const auto game_data_root = ResolveGameDataRoot();
    if (game_data_root == 0) {
        return 0;
    }

    return *reinterpret_cast<std::uintptr_t*>(game_data_root + kEquipMagicDataOffset);
}

int ResolveCurrentSpellEntrySlot(std::uintptr_t equip_magic_data)
{
    const auto selected_slot = *reinterpret_cast<const std::int32_t*>(equip_magic_data + kSelectedSlotOffset);
    if (selected_slot < 0 || selected_slot >= static_cast<int>(g_offsets.max_slots)) {
        return -1;
    }

    const auto selected_entry = equip_magic_data + kFirstMagicSlotOffset +
        (static_cast<std::size_t>(selected_slot) * g_offsets.spell_entry_stride);
    const auto selected_spell_id = *reinterpret_cast<const std::uint32_t*>(selected_entry + g_offsets.spell_id_offset);
    if (selected_spell_id == 0 || selected_spell_id == 0xFFFFFFFFu) {
        return -1;
    }

    return selected_slot;
}

}  // namespace

bool InitializeSpellManager()
{
    InitializeSpellMetadata();

    const auto game_data_man = ResolveGameDataManAddress();
    if (game_data_man == 0) {
        Log("Failed to resolve GameDataMan; memorized spells will be unavailable.");
        return false;
    }

    Log("Spell manager initialized via GameDataMan signature at 0x%p.", reinterpret_cast<void*>(game_data_man));
    return true;
}

void SetOffsets(const SpellOffsets& offsets)
{
    g_offsets = offsets;
}

const SpellOffsets& GetOffsets()
{
    return g_offsets;
}

int GetCurrentSpellSlot()
{
    const auto equip_magic_data = ResolveEquipMagicData();
    if (equip_magic_data == 0) {
        return -1;
    }

    const int current_entry_slot = ResolveCurrentSpellEntrySlot(equip_magic_data);
    if (current_entry_slot < 0) {
        return -1;
    }

    int display_slot = 0;
    for (std::size_t i = 0; i < g_offsets.max_slots; ++i) {
        const auto entry = equip_magic_data + kFirstMagicSlotOffset + (i * g_offsets.spell_entry_stride);
        const auto spell_id = *reinterpret_cast<const std::uint32_t*>(entry + g_offsets.spell_id_offset);
        if (spell_id == 0 || spell_id == 0xFFFFFFFFu) {
            continue;
        }

        if (static_cast<int>(i) == current_entry_slot) {
            return display_slot;
        }

        ++display_slot;
    }

    return -1;
}

std::vector<SpellSlot> GetMemorizedSpells()
{
    std::vector<SpellSlot> spells;
    spells.reserve(g_offsets.max_slots);

    const auto equip_magic_data = ResolveEquipMagicData();
    if (equip_magic_data == 0) {
        if (!g_logged_game_data_man) {
            Log("EquipMagicData is unresolved; the radial menu cannot list memorized spells yet.");
            g_logged_game_data_man = true;
        }
        return spells;
    }

    const int current_entry_slot = ResolveCurrentSpellEntrySlot(equip_magic_data);
    for (std::size_t i = 0; i < g_offsets.max_slots; ++i) {
        const auto entry = equip_magic_data + kFirstMagicSlotOffset + (i * g_offsets.spell_entry_stride);
        const auto spell_id = *reinterpret_cast<const std::uint32_t*>(entry + g_offsets.spell_id_offset);
        if (spell_id == 0 || spell_id == 0xFFFFFFFFu) {
            continue;
        }

        const ResolvedSpellMetadata metadata = ResolveSpellMetadata(spell_id);

        spells.push_back({
            .slot_index = i,
            .spell_id = spell_id,
            .name = metadata.name,
            .icon_id = metadata.icon_id,
            .category = metadata.category,
            .occupied = true,
            .is_current = (static_cast<int>(i) == current_entry_slot),
        });
    }

    g_logged_game_data_man = false;
    return spells;
}

bool SwitchToSpellSlot(std::size_t slot_index)
{
    const auto equip_magic_data = ResolveEquipMagicData();
    if (equip_magic_data == 0) {
        Log("Spell switch skipped because EquipMagicData is unresolved.");
        return false;
    }

    if (slot_index >= g_offsets.max_slots) {
        Log("Spell switch skipped because slot %zu is outside the supported range.", slot_index);
        return false;
    }

    const auto entry = equip_magic_data + kFirstMagicSlotOffset + (slot_index * g_offsets.spell_entry_stride);
    const auto spell_id = *reinterpret_cast<const std::uint32_t*>(entry + g_offsets.spell_id_offset);
    if (spell_id == 0 || spell_id == 0xFFFFFFFFu) {
        Log("Spell switch skipped because slot %zu is empty.", slot_index);
        return false;
    }

    auto* const selected_slot = reinterpret_cast<std::int32_t*>(equip_magic_data + kSelectedSlotOffset);
    Log("Selecting spell entry slot %zu (spell_id=%u) via EquipMagicData.selected_slot.", slot_index, spell_id);
    *selected_slot = static_cast<std::int32_t>(slot_index);

    if (*selected_slot == static_cast<std::int32_t>(slot_index)) {
        return true;
    }

    Log("Spell switch write did not persist for slot %zu.", slot_index);
    return false;
}
}  // namespace radial_spell_menu
