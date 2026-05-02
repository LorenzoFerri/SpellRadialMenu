#pragma once

#include <cstdint>
#include <string>
#include <vector>
namespace radial_spell_menu {

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

bool InitializeSpellMetadata();
void BeginRuntimeMsgLookupTrace(const std::vector<std::uint32_t>& spell_ids, std::uint32_t focused_spell_id);
ResolvedSpellMetadata ResolveSpellMetadata(std::uint32_t spell_id);

}  // namespace radial_spell_menu
