#include "render/vfs/path_utils.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>

namespace radial_menu_mod::asset_reader {

std::wstring NormalizePath(const wchar_t* path)
{
    std::wstring normalized = path ? path : L"";
    for (wchar_t& ch : normalized) {
        if (ch == L'\\') {
            ch = L'/';
        } else {
            ch = static_cast<wchar_t>(std::towlower(ch));
        }
    }
    return normalized;
}

bool StartsWithPath(const std::wstring& path, const std::wstring& prefix)
{
    if (prefix.empty() || path.size() < prefix.size()) return false;
    return std::equal(prefix.begin(), prefix.end(), path.begin());
}

std::wstring JoinDiskPath(std::wstring base, std::wstring relative)
{
    while (!base.empty() && (base.back() == L'/' || base.back() == L'\\')) base.pop_back();
    while (!relative.empty() && (relative.front() == L'/' || relative.front() == L'\\')) relative.erase(relative.begin());
    for (wchar_t& ch : relative) {
        if (ch == L'/') ch = L'\\';
    }
    return base + L'\\' + relative;
}

bool ReadDiskFile(const std::wstring& path, std::vector<std::uint8_t>& bytes, std::uint64_t max_size)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || static_cast<std::uint64_t>(size.QuadPart) > max_size) {
        CloseHandle(file);
        return false;
    }

    bytes.resize(static_cast<std::size_t>(size.QuadPart));
    DWORD read = 0;
    const BOOL ok = ::ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr);
    CloseHandle(file);
    if (!ok || read != bytes.size()) {
        bytes.clear();
        return false;
    }
    return true;
}

}  // namespace radial_menu_mod::asset_reader
