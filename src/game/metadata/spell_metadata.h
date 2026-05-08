#pragma once

#include <cstdint>
#include <string>

namespace radial_menu_mod {

enum class SpellCategory : std::uint8_t {
    unknown = 0,
    sorcery,
    incantation,
};

struct ResolvedSpellMetadata {
    std::string name;
    std::uint32_t icon_id = 0;
    SpellCategory category = SpellCategory::unknown;
};

struct ResolvedItemMetadata {
    std::string name;
    std::uint32_t icon_id = 0;
};

bool InitializeSpellMetadata();
ResolvedSpellMetadata ResolveSpellMetadata(std::uint32_t spell_id);
ResolvedItemMetadata ResolveItemMetadata(std::uint32_t item_id);

}  // namespace radial_menu_mod
