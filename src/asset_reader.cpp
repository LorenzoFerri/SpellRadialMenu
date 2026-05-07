#include "asset_reader.h"

#include "common.h"

#include <MinHook.h>
#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace radial_spell_menu::asset_reader {
namespace {

template <typename T>
struct DlVector2015 {
    void* alloc = nullptr;
    T* first = nullptr;
    T* last = nullptr;
    T* end = nullptr;
};

struct DlUtf16String2015 {
    void* alloc = nullptr;
    union {
        wchar_t small[8];
        wchar_t* ptr;
    } storage = {};
    std::size_t len = 0;
    std::size_t cap = 7;
    std::uint8_t encoding = 1;
};

struct DlDevice;
using DlDeviceOpenFn = void* (*)(DlDevice*, DlUtf16String2015*, const wchar_t*, void*, void*, bool);

struct DlDeviceVtable {
    void* dtor = nullptr;
    DlDeviceOpenFn open_file = nullptr;
};

struct DlDevice {
    DlDeviceVtable* vtable = nullptr;
};

struct DlVirtualRoot {
    DlUtf16String2015 root;
    DlUtf16String2015 expanded;
};

struct DlVirtualMount {
    DlUtf16String2015 root;
    DlDevice* device = nullptr;
    std::size_t size = 0;
};

struct DlDeviceManager2015 {
    DlVector2015<DlDevice*> devices;
    DlVector2015<void*> spis;
    DlDevice* disk_device = nullptr;
    DlVector2015<DlVirtualRoot> virtual_roots;
    DlVector2015<DlVirtualMount> bnd3_mounts;
    DlVector2015<DlVirtualMount> bnd4_mounts;
    void* bnd3_spi = nullptr;
    void* bnd4_spi = nullptr;
    void* mutex_vtable = nullptr;
};

struct SectionRange {
    std::uintptr_t begin = 0;
    std::uintptr_t end = 0;

    bool Contains(std::uintptr_t value, std::size_t size = 1) const
    {
        return value >= begin && value + size >= value && value + size <= end;
    }
};

DlDeviceOpenFn g_orig_open_file = nullptr;
void* g_open_file_target = nullptr;
bool g_logged_install_failure = false;
DlDevice* g_disk_device = nullptr;
void* g_captured_container = nullptr;
void* g_captured_allocator = nullptr;
bool g_captured_temp_flag = false;

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

bool IsReadablePointer(const void* ptr, std::size_t bytes)
{
    MEMORY_BASIC_INFORMATION mbi{};
    if (!ptr || VirtualQuery(ptr, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD) || (mbi.Protect & PAGE_NOACCESS)) return false;

    const DWORD protect = mbi.Protect & 0xFFu;
    const bool readable = protect == PAGE_READONLY || protect == PAGE_READWRITE || protect == PAGE_WRITECOPY ||
                          protect == PAGE_EXECUTE_READ || protect == PAGE_EXECUTE_READWRITE || protect == PAGE_EXECUTE_WRITECOPY;
    if (!readable) return false;

    const auto begin = reinterpret_cast<std::uintptr_t>(ptr);
    const auto region_begin = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
    const auto region_end = region_begin + static_cast<std::uintptr_t>(mbi.RegionSize);
    return begin >= region_begin && begin + bytes >= begin && begin + bytes <= region_end;
}

bool IsExecutableAddress(std::uintptr_t value)
{
    MEMORY_BASIC_INFORMATION mbi{};
    if (value == 0 || VirtualQuery(reinterpret_cast<void*>(value), &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD) || (mbi.Protect & PAGE_NOACCESS)) return false;

    const DWORD protect = mbi.Protect & 0xFFu;
    return protect == PAGE_EXECUTE || protect == PAGE_EXECUTE_READ ||
           protect == PAGE_EXECUTE_READWRITE || protect == PAGE_EXECUTE_WRITECOPY;
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

template <typename T>
std::size_t VectorLen(const DlVector2015<T>& vector)
{
    if (!vector.first || !vector.last || vector.last < vector.first) return 0;
    const auto len = static_cast<std::size_t>(vector.last - vector.first);
    return len < 100000 ? len : 0;
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
    if (IsReadablePointer(manager->devices.first, VectorLen(manager->devices) * sizeof(DlDevice*))) {
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

DlUtf16String2015 MakeDlString(std::wstring& storage)
{
    DlUtf16String2015 value{};
    value.len = storage.size();
    value.encoding = 1;
    if (storage.size() <= 7) {
        value.cap = 7;
        std::fill(std::begin(value.storage.small), std::end(value.storage.small), L'\0');
        std::copy(storage.begin(), storage.end(), value.storage.small);
    } else {
        value.cap = storage.size();
        value.storage.ptr = storage.data();
    }
    return value;
}

void* HookedOpenFile(DlDevice* device, DlUtf16String2015* path, const wchar_t* path_cstr, void* container, void* allocator, bool is_temp_file)
{
    if (container && allocator && !g_captured_container) {
        g_disk_device = device;
        g_captured_container = container;
        g_captured_allocator = allocator;
        g_captured_temp_flag = is_temp_file;
    }

    return g_orig_open_file ? g_orig_open_file(device, path, path_cstr, container, allocator, is_temp_file) : nullptr;
}

}  // namespace

bool ReadFile(const wchar_t* path, std::vector<std::uint8_t>& bytes, std::uint64_t max_size)
{
    bytes.clear();
    if (!path || !g_disk_device || !g_orig_open_file || !g_captured_container || !g_captured_allocator) return false;

    std::wstring storage(path);
    DlUtf16String2015 dl_path = MakeDlString(storage);
    void* file_operator = g_orig_open_file(
        g_disk_device,
        &dl_path,
        storage.c_str(),
        g_captured_container,
        g_captured_allocator,
        g_captured_temp_flag);

    if (!file_operator || !IsReadablePointer(file_operator, sizeof(std::uintptr_t) * 14)) return false;

    const auto* words = reinterpret_cast<const std::uintptr_t*>(file_operator);
    const auto* vtable = reinterpret_cast<const std::uintptr_t*>(words[0]);
    const std::uint64_t size = words[13];
    if (size == 0 || size > max_size || !IsReadablePointer(vtable, sizeof(std::uintptr_t) * 26) || !IsExecutableAddress(vtable[25])) return false;

    using ReadFn = int (*)(void*, void*, std::uint64_t);
    bytes.resize(static_cast<std::size_t>(size));
    const int read = reinterpret_cast<ReadFn>(vtable[25])(file_operator, bytes.data(), bytes.size());
    if (read <= 0) {
        bytes.clear();
        return false;
    }

    bytes.resize(static_cast<std::size_t>(read));
    return true;
}

bool Install()
{
    if (g_open_file_target) return true;

    DlDeviceManager2015* manager = LocateDeviceManager();
    if (!manager || !manager->disk_device || !manager->disk_device->vtable || !manager->disk_device->vtable->open_file) {
        if (!g_logged_install_failure) {
            Log("Asset reader: failed to locate game VFS open function.");
            g_logged_install_failure = true;
        }
        return false;
    }

    g_disk_device = manager->disk_device;
    g_open_file_target = reinterpret_cast<void*>(manager->disk_device->vtable->open_file);
    const MH_STATUS create = MH_CreateHook(
        g_open_file_target,
        reinterpret_cast<void*>(&HookedOpenFile),
        reinterpret_cast<void**>(&g_orig_open_file));
    if (create != MH_OK) {
        Log("Asset reader: MH_CreateHook failed (%d).", static_cast<int>(create));
        g_open_file_target = nullptr;
        return false;
    }

    const MH_STATUS enable = MH_EnableHook(g_open_file_target);
    if (enable != MH_OK) {
        Log("Asset reader: MH_EnableHook failed (%d).", static_cast<int>(enable));
        MH_RemoveHook(g_open_file_target);
        g_open_file_target = nullptr;
        return false;
    }

    return true;
}

void Shutdown()
{
    if (!g_open_file_target) return;
    MH_DisableHook(g_open_file_target);
    MH_RemoveHook(g_open_file_target);
    g_open_file_target = nullptr;
    g_orig_open_file = nullptr;
    g_captured_container = nullptr;
    g_captured_allocator = nullptr;
}

}  // namespace radial_spell_menu::asset_reader
