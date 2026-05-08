#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace radial_menu_mod::asset_reader {

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
using DlFileReadFn = int (*)(void*, void*, std::uint64_t);

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

template <typename T>
std::size_t VectorLen(const DlVector2015<T>& vector)
{
    if (!vector.first || !vector.last || vector.last < vector.first) return 0;
    const auto len = static_cast<std::size_t>(vector.last - vector.first);
    return len < 100000 ? len : 0;
}

bool IsExecutableAddress(std::uintptr_t value);
std::wstring ReadDlString(const DlUtf16String2015& value);
DlDeviceManager2015* LocateDeviceManager();

}  // namespace radial_menu_mod::asset_reader
