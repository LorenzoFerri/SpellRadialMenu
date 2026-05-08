#pragma once

#include <cstdint>
#include <vector>

namespace radial_menu_mod::asset_reader {

bool Install();
bool ReadFile(const wchar_t* path, std::vector<std::uint8_t>& bytes, std::uint64_t max_size);
bool IsHookInstalled();
bool HasGameReadContext();
void Shutdown();

}  // namespace radial_menu_mod::asset_reader
