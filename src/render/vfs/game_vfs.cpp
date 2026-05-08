#include "render/vfs/game_vfs.h"

#include "core/common.h"

#include <windows.h>

#include <algorithm>
#include <cstring>

namespace radial_menu_mod::asset_reader {
namespace {

struct SectionRange {
    std::uintptr_t begin = 0;
    std::uintptr_t end = 0;

    bool Contains(std::uintptr_t value, std::size_t size = 1) const
    {
        return value >= begin && value + size >= value && value + size <= end;
    }
};

bool GetSectionRange(const char* name, SectionRange& range)
{
    const auto base = GetModuleBase();
    if (!base) return false;

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

    const auto* section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        char section_name[9] = {};
        std::copy_n(reinterpret_cast<const char*>(section->Name), 8, section_name);
        if (std::strcmp(section_name, name) != 0) continue;

        range.begin = base + section->VirtualAddress;
        range.end = range.begin + section->Misc.VirtualSize;
        return range.end > range.begin;
    }

    return false;
}

template <typename T>
bool LooksLikeVector(const DlVector2015<T>& vector, const SectionRange& data)
{
    const auto first = reinterpret_cast<std::uintptr_t>(vector.first);
    const auto last = reinterpret_cast<std::uintptr_t>(vector.last);
    const auto end = reinterpret_cast<std::uintptr_t>(vector.end);

    if (reinterpret_cast<std::uintptr_t>(vector.alloc) % alignof(void*) != 0) return false;
    if (first % alignof(T*) != 0 || last % alignof(T*) != 0 || end % alignof(T*) != 0) return false;
    if (first > last || last > end) return false;
    if (first == 0 && last == 0 && end == 0) return true;
    if (first == 0 || last == 0 || end == 0) return false;

    return data.Contains(first) || (end - first) < (64ull * 1024ull * 1024ull);
}

bool VerifyDeviceManager(const DlDeviceManager2015* manager, const SectionRange& data, const SectionRange& rdata)
{
    if (!manager || reinterpret_cast<std::uintptr_t>(manager) % alignof(DlDeviceManager2015) != 0) return false;
    if (!data.Contains(reinterpret_cast<std::uintptr_t>(manager), sizeof(DlDeviceManager2015))) return false;
    if (!LooksLikeVector(manager->devices, data)) return false;
    if (!LooksLikeVector(manager->spis, data)) return false;
    if (!LooksLikeVector(manager->virtual_roots, data)) return false;
    if (!LooksLikeVector(manager->bnd3_mounts, data)) return false;
    if (!LooksLikeVector(manager->bnd4_mounts, data)) return false;
    if (VectorLen(manager->devices) == 0 || VectorLen(manager->devices) > 512) return false;
    if (VectorLen(manager->virtual_roots) == 0 || VectorLen(manager->virtual_roots) > 128) return false;

    if (!manager->bnd3_spi || !manager->bnd4_spi) return false;
    const auto mutex_vtable = reinterpret_cast<std::uintptr_t>(manager->mutex_vtable);
    if (mutex_vtable % alignof(void*) != 0 || !rdata.Contains(mutex_vtable, sizeof(void*))) return false;

    const auto disk = reinterpret_cast<std::uintptr_t>(manager->disk_device);
    if (disk % alignof(DlDevice) != 0 || !data.Contains(disk, sizeof(DlDevice))) return false;

    bool disk_in_devices = false;
    if (IsReadableMemory(manager->devices.first, VectorLen(manager->devices) * sizeof(DlDevice*))) {
        for (auto* it = manager->devices.first; it != manager->devices.last; ++it) {
            if (*it == manager->disk_device) {
                disk_in_devices = true;
                break;
            }
        }
    }
    if (!disk_in_devices) return false;

    const auto vtable = reinterpret_cast<std::uintptr_t>(manager->disk_device->vtable);
    if (vtable % alignof(void*) != 0 || !rdata.Contains(vtable, sizeof(DlDeviceVtable))) return false;

    const auto open = reinterpret_cast<std::uintptr_t>(manager->disk_device->vtable->open_file);
    return open >= GetModuleBase() && open < GetModuleBase() + GetModuleSize();
}

}  // namespace

bool IsExecutableAddress(std::uintptr_t value)
{
    MEMORY_BASIC_INFORMATION mbi{};
    if (value == 0 || VirtualQuery(reinterpret_cast<void*>(value), &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD) || (mbi.Protect & PAGE_NOACCESS)) return false;

    const DWORD protect = mbi.Protect & 0xFFu;
    return protect == PAGE_EXECUTE || protect == PAGE_EXECUTE_READ ||
           protect == PAGE_EXECUTE_READWRITE || protect == PAGE_EXECUTE_WRITECOPY;
}

std::wstring ReadDlString(const DlUtf16String2015& value)
{
    if (value.len == 0 || value.len > 1024) return {};
    const wchar_t* text = value.cap <= 7 ? value.storage.small : value.storage.ptr;
    if (!IsReadableMemory(text, value.len * sizeof(wchar_t))) return {};
    return std::wstring(text, text + value.len);
}

DlDeviceManager2015* LocateDeviceManager()
{
    SectionRange data{};
    SectionRange rdata{};
    if (!GetSectionRange(".data", data) || !GetSectionRange(".rdata", rdata)) return nullptr;

    for (auto cursor = data.begin; cursor + sizeof(void*) <= data.end; cursor += sizeof(void*)) {
        const auto candidate = *reinterpret_cast<DlDeviceManager2015**>(cursor);
        if (VerifyDeviceManager(candidate, data, rdata)) return candidate;
    }

    return nullptr;
}

}  // namespace radial_menu_mod::asset_reader
