#pragma once

#include <cstdint>
#include <vector>

namespace radial_menu_mod::loose_asset_reader {

bool ReadFile(const wchar_t* path, std::vector<std::uint8_t>& bytes, std::uint64_t max_size);

}  // namespace radial_menu_mod::loose_asset_reader
