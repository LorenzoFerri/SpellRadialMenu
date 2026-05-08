#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace radial_menu_mod::asset_reader {

std::wstring NormalizePath(const wchar_t* path);
bool StartsWithPath(const std::wstring& path, const std::wstring& prefix);
std::wstring JoinDiskPath(std::wstring base, std::wstring relative);
bool ReadDiskFile(const std::wstring& path, std::vector<std::uint8_t>& bytes, std::uint64_t max_size);

}  // namespace radial_menu_mod::asset_reader
