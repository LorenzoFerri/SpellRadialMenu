#pragma once

#include <windows.h>

#include <array>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cwchar>

namespace radial_spell_menu {

inline void Log(const char* format, ...)
{
    char buffer[1024] = {};
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    OutputDebugStringA("[RadialSpellMenu] ");
    OutputDebugStringA(buffer);
    OutputDebugStringA("\n");

    // Write to RadialSpellMenu.log next to the DLL
    wchar_t log_path[MAX_PATH] = {};
    HMODULE module = nullptr;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&Log), &module);
    wchar_t module_path[MAX_PATH] = {};
    GetModuleFileNameW(module, module_path, MAX_PATH);
    wchar_t* sep = wcsrchr(module_path, L'\\');
    if (sep) {
        *(sep + 1) = L'\0';
        swprintf(log_path, MAX_PATH, L"%lsRadialSpellMenu.log", module_path);
        HANDLE file = CreateFileW(log_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            DWORD len = static_cast<DWORD>(strnlen(buffer, sizeof(buffer)));
            WriteFile(file, buffer, len, &written, nullptr);
            WriteFile(file, "\r\n", 2, &written, nullptr);
            CloseHandle(file);
        }
    }
}

// ── Pattern scanning helpers (used by spell_manager and spell_metadata) ───

inline std::uintptr_t GetModuleBase()
{
    return reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
}

inline std::size_t GetModuleSize()
{
    const auto base = GetModuleBase();
    if (!base) return 0;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    return nt->OptionalHeader.SizeOfImage;
}

template <std::size_t N>
inline std::uintptr_t FindPattern(const std::array<std::uint8_t, N>& pattern,
                                   const std::array<bool, N>& mask)
{
    const auto base = GetModuleBase();
    const auto size = GetModuleSize();
    if (!base || size < N) return 0;
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(base);
    for (std::size_t i = 0; i <= size - N; ++i) {
        bool match = true;
        for (std::size_t j = 0; j < N; ++j) {
            if (mask[j] && bytes[i + j] != pattern[j]) { match = false; break; }
        }
        if (match) return base + i;
    }
    return 0;
}

inline std::uintptr_t ResolveRipRelative(std::uintptr_t addr, std::uintptr_t disp_off, std::uintptr_t insn_size)
{
    const auto disp = *reinterpret_cast<const std::int32_t*>(addr + disp_off);
    return addr + insn_size + disp;
}

template <typename T>
inline void SafeRelease(T*& p)
{
    if (p) { p->Release(); p = nullptr; }
}

}  // namespace radial_spell_menu
