#include "render/assets/loose_asset_reader.h"

#include "core/common.h"

#include <windows.h>

#include <string>
#include <utility>

namespace radial_spell_menu::loose_asset_reader {
namespace {

bool g_logged_disk_fallback = false;

std::wstring DllDirectory()
{
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&DllDirectory),
            &module)) {
        return L".";
    }

    wchar_t path[MAX_PATH] = {};
    const DWORD len = GetModuleFileNameW(module, path, static_cast<DWORD>(std::size(path)));
    if (len == 0 || len >= std::size(path)) return L".";

    std::wstring result(path, path + len);
    const std::size_t slash = result.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : result.substr(0, slash);
}

std::wstring ParentPath(std::wstring path)
{
    while (!path.empty() && (path.back() == L'\\' || path.back() == L'/')) path.pop_back();
    const std::size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring{} : path.substr(0, slash);
}

std::wstring RelativeDataPath(const wchar_t* path)
{
    constexpr const wchar_t* prefix = L"data0:/";
    constexpr std::size_t prefix_len = 7;
    if (!path || _wcsnicmp(path, prefix, prefix_len) != 0) return {};

    std::wstring relative(path + prefix_len);
    for (wchar_t& c : relative) {
        if (c == L'/') c = L'\\';
    }
    return relative;
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

}  // namespace

bool ReadFile(const wchar_t* path, std::vector<std::uint8_t>& bytes, std::uint64_t max_size)
{
    const std::wstring relative = RelativeDataPath(path);
    if (relative.empty()) return false;

    std::wstring dir = DllDirectory();
    for (int depth = 0; depth < 6 && !dir.empty(); ++depth) {
        const std::wstring candidate = dir + L"\\mod\\" + relative;
        if (ReadDiskFile(candidate, bytes, max_size)) {
            if (!g_logged_disk_fallback) {
                Log("Asset reader: using disk mod asset fallback.");
                g_logged_disk_fallback = true;
            }
            return true;
        }
        dir = ParentPath(std::move(dir));
    }

    return false;
}

}  // namespace radial_spell_menu::loose_asset_reader
