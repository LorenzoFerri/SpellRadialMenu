#pragma once

#include <cstdint>
#include <vector>

namespace radial_spell_menu::loose_asset_reader {

bool ReadFile(const wchar_t* path, std::vector<std::uint8_t>& bytes, std::uint64_t max_size);

}  // namespace radial_spell_menu::loose_asset_reader
