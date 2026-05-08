#pragma once

#include <cstdint>
#include <vector>

namespace radial_spell_menu::asset_reader {

bool Install();
bool ReadFile(const wchar_t* path, std::vector<std::uint8_t>& bytes, std::uint64_t max_size);
void Shutdown();

}  // namespace radial_spell_menu::asset_reader
