#pragma once

#include <windows.h>

#include <array>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cwchar>

namespace radial_menu_mod {

inline void Log(const char* format, ...)
{
    static bool log_initialized = false;

    char buffer[1024] = {};
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    OutputDebugStringA("[RadialMenu] ");
    OutputDebugStringA(buffer);
    OutputDebugStringA("\n");

    // Write to RadialMenu.log next to the DLL
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
        swprintf(log_path, MAX_PATH, L"%lsRadialMenu.log", module_path);
        const DWORD disposition = log_initialized ? OPEN_ALWAYS : CREATE_ALWAYS;
        HANDLE file = CreateFileW(log_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  nullptr, disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            log_initialized = true;
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

inline bool IsReadableMemory(const void* ptr, std::size_t bytes)
{
    MEMORY_BASIC_INFORMATION mbi{};
    if (!ptr || VirtualQuery(ptr, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD) || (mbi.Protect & PAGE_NOACCESS)) return false;

    const DWORD protect = mbi.Protect & 0xFFu;
    const bool readable = protect == PAGE_READONLY || protect == PAGE_READWRITE || protect == PAGE_WRITECOPY ||
                          protect == PAGE_EXECUTE_READ || protect == PAGE_EXECUTE_READWRITE ||
                          protect == PAGE_EXECUTE_WRITECOPY;
    if (!readable) return false;

    const auto begin = reinterpret_cast<std::uintptr_t>(ptr);
    const auto region_begin = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
    const auto region_end = region_begin + static_cast<std::uintptr_t>(mbi.RegionSize);
    return begin >= region_begin && begin + bytes >= begin && begin + bytes <= region_end;
}

template <typename T>
inline void SafeRelease(T*& p)
{
    if (p) { p->Release(); p = nullptr; }
}

}  // namespace radial_menu_mod
