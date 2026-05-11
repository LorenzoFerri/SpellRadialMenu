#pragma once

#include <cstdint>
#include <string>

namespace radial_menu_mod {

struct ResolvedItemMetadata {
    std::string name;
    std::uint32_t icon_id = 0;
};

bool InitializeItemMetadata();
ResolvedItemMetadata ResolveItemMetadata(std::uint32_t item_id);

}  // namespace radial_menu_mod
