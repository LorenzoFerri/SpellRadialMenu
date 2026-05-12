#pragma once

#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace radial_menu_mod {

void Log(const char* format, ...);
void ShutdownLog();

// ── Pattern scanning helpers (used by metadata and singleton resolution) ───

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

struct CachedReadableRegion {
    std::uintptr_t begin = 0;
    std::uintptr_t end = 0;

    void Reset()
    {
        begin = 0;
        end = 0;
    }
};

inline bool IsReadableProtection(DWORD protect)
{
    protect &= 0xFFu;
    return protect == PAGE_READONLY || protect == PAGE_READWRITE || protect == PAGE_WRITECOPY ||
        protect == PAGE_EXECUTE_READ || protect == PAGE_EXECUTE_READWRITE || protect == PAGE_EXECUTE_WRITECOPY;
}

inline bool IsInCachedReadableRegion(const CachedReadableRegion& region, std::uintptr_t address, std::size_t bytes)
{
    return region.begin != 0 && address >= region.begin && address + bytes >= address && address + bytes <= region.end;
}

inline bool RefreshCachedReadableRegion(std::uintptr_t address, std::size_t bytes, CachedReadableRegion& region)
{
    MEMORY_BASIC_INFORMATION mbi{};
    if (!address || VirtualQuery(reinterpret_cast<const void*>(address), &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD) || (mbi.Protect & PAGE_NOACCESS)) return false;
    if (!IsReadableProtection(mbi.Protect)) return false;

    const auto begin = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
    const auto end = begin + static_cast<std::uintptr_t>(mbi.RegionSize);
    if (address < begin || address + bytes < address || address + bytes > end) return false;

    region.begin = begin;
    region.end = end;
    return true;
}

inline bool EnsureCachedReadableMemory(std::uintptr_t address, std::size_t bytes, CachedReadableRegion& region)
{
    return IsInCachedReadableRegion(region, address, bytes) || RefreshCachedReadableRegion(address, bytes, region);
}

template <typename T>
inline bool ReadCachedMemory(std::uintptr_t address, T& value, CachedReadableRegion& region)
{
    if (!EnsureCachedReadableMemory(address, sizeof(T), region)) return false;
    value = *reinterpret_cast<const T*>(address);
    return true;
}

inline bool IsReadableMemory(const void* ptr, std::size_t bytes)
{
    CachedReadableRegion region{};
    return RefreshCachedReadableRegion(reinterpret_cast<std::uintptr_t>(ptr), bytes, region);
}

template <typename T>
inline void SafeRelease(T*& p)
{
    if (p) { p->Release(); p = nullptr; }
}

}  // namespace radial_menu_mod
