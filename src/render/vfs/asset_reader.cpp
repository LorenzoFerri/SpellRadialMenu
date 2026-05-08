#include "render/vfs/asset_reader.h"

#include "core/common.h"
#include "render/vfs/game_vfs.h"
#include "render/vfs/path_utils.h"
#include <MinHook.h>
#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace radial_menu_mod::asset_reader {
namespace {

struct VirtualRootPath {
    std::wstring root;
    std::wstring expanded;
};

DlDeviceOpenFn g_orig_open_file = nullptr;
void* g_open_file_target = nullptr;
bool g_logged_install_failure = false;
DlDevice* g_disk_device = nullptr;
void* g_captured_container = nullptr;
void* g_captured_allocator = nullptr;
bool g_captured_temp_flag = false;
bool g_logged_waiting_for_cache = false;
bool g_logged_direct_read_attempt = false;
bool g_logged_direct_read_result = false;
void* g_read_file_target = nullptr;
DlFileReadFn g_orig_read_file = nullptr;
std::mutex g_cache_mutex;
std::unordered_map<void*, std::wstring> g_pending_reads;
std::unordered_map<std::wstring, std::vector<std::uint8_t>> g_cached_files;
std::vector<VirtualRootPath> g_virtual_roots;
bool g_logged_virtual_roots = false;
int g_logged_virtual_root_details = 0;
bool g_logged_virtual_mounts = false;
bool g_logged_memory_root_read = false;
bool g_logged_memory_root_miss = false;
bool g_logged_loader_filesystem_read = false;
bool g_logged_loader_filesystem_miss = false;
bool g_logged_mounted_vfs_attempt = false;
bool g_logged_mounted_vfs_read = false;

bool IsIconAssetPath(const std::wstring& path);

bool Data0IconPathToRelativePath(const wchar_t* path, std::wstring& relative)
{
    const std::wstring normalized = NormalizePath(path);
    constexpr const wchar_t* prefix = L"data0:/";
    if (!StartsWithPath(normalized, prefix) || !IsIconAssetPath(normalized)) return false;

    relative = normalized.substr(7);
    for (wchar_t& ch : relative) {
        if (ch == L'/') ch = L'\\';
    }
    return true;
}

bool ReadThroughLoaderFilesystem(const wchar_t* path, std::vector<std::uint8_t>& bytes, std::uint64_t max_size)
{
    std::wstring relative;
    if (!Data0IconPathToRelativePath(path, relative)) return false;

    if (ReadDiskFile(relative, bytes, max_size)) {
        if (!g_logged_loader_filesystem_read) {
            Log("Asset reader: read icon asset through loader filesystem override.");
            g_logged_loader_filesystem_read = true;
        }
        return true;
    }

    wchar_t cwd[MAX_PATH] = {};
    if (GetCurrentDirectoryW(MAX_PATH, cwd)) {
        const std::wstring cwd_relative = JoinDiskPath(cwd, relative);
        if (ReadDiskFile(cwd_relative, bytes, max_size)) {
            if (!g_logged_loader_filesystem_read) {
                Log("Asset reader: read icon asset through loader filesystem override.");
                g_logged_loader_filesystem_read = true;
            }
            return true;
        }
    }

    if (!g_logged_loader_filesystem_miss) {
        Log("Asset reader: loader filesystem override did not resolve requested icon asset.");
        g_logged_loader_filesystem_miss = true;
    }
    return false;
}

void CaptureVirtualRoots(const DlDeviceManager2015& manager)
{
    if (!g_virtual_roots.empty()) return;
    const std::size_t count = VectorLen(manager.virtual_roots);
    if (count == 0 || !IsReadableMemory(manager.virtual_roots.first, count * sizeof(DlVirtualRoot))) return;

    for (const DlVirtualRoot* it = manager.virtual_roots.first; it != manager.virtual_roots.last; ++it) {
        std::wstring root = NormalizePath(ReadDlString(it->root).c_str());
        std::wstring expanded = ReadDlString(it->expanded);
        if (root.empty() || expanded.empty()) continue;
        g_virtual_roots.push_back({std::move(root), std::move(expanded)});
    }

    if (!g_logged_virtual_roots) {
        Log("Asset reader: captured %zu game VFS virtual roots.", g_virtual_roots.size());
        for (const VirtualRootPath& entry : g_virtual_roots) {
            if (g_logged_virtual_root_details >= 24) break;
            if (entry.root.find(L"data") == std::wstring::npos &&
                entry.root.find(L"menu") == std::wstring::npos &&
                entry.expanded.find(L"menu") == std::wstring::npos &&
                entry.expanded.find(L"mod") == std::wstring::npos) {
                continue;
            }
            Log("Asset reader: VFS root '%ls' -> '%ls'.", entry.root.c_str(), entry.expanded.c_str());
            ++g_logged_virtual_root_details;
        }
        g_logged_virtual_roots = true;
    }
}

void LogVirtualMounts(const char* label, const DlVector2015<DlVirtualMount>& mounts)
{
    const std::size_t count = VectorLen(mounts);
    Log("Asset reader: %s mount count=%zu readable=%d.",
        label,
        count,
        static_cast<int>(count != 0 && IsReadableMemory(mounts.first, count * sizeof(DlVirtualMount))));
    if (count == 0 || !IsReadableMemory(mounts.first, count * sizeof(DlVirtualMount))) return;

    int logged = 0;
    for (const DlVirtualMount* it = mounts.first; it != mounts.last && logged < 32; ++it) {
        const std::wstring root = NormalizePath(ReadDlString(it->root).c_str());
        Log("Asset reader: %s mount root='%ls' device=%p size=%zu.", label, root.c_str(), it->device, it->size);
        ++logged;
    }
}

void LogMountedRoots(const DlDeviceManager2015& manager)
{
    if (g_logged_virtual_mounts) return;
    LogVirtualMounts("bnd3", manager.bnd3_mounts);
    LogVirtualMounts("bnd4", manager.bnd4_mounts);
    g_logged_virtual_mounts = true;
}

bool ReadFromMemoryVirtualRoot(const wchar_t* path, std::vector<std::uint8_t>& bytes, std::uint64_t max_size)
{
    const std::wstring normalized_path = NormalizePath(path);
    for (const VirtualRootPath& root : g_virtual_roots) {
        if (!StartsWithPath(normalized_path, root.root)) continue;

        std::wstring relative = normalized_path.substr(root.root.size());
        const std::wstring disk_path = JoinDiskPath(root.expanded, std::move(relative));
        if (ReadDiskFile(disk_path, bytes, max_size)) {
            if (!g_logged_memory_root_read) {
                Log("Asset reader: read Data0 asset through game VFS root mapping.");
                g_logged_memory_root_read = true;
            }
            return true;
        }
    }

    if (!g_logged_memory_root_miss && !g_virtual_roots.empty()) {
        Log("Asset reader: game VFS root mappings did not contain the requested Data0 asset on disk.");
        g_logged_memory_root_miss = true;
    }
    return false;
}

bool IsIconAssetPath(const std::wstring& path)
{
    return path == L"data0:/menu/low/01_common.tpf.dcx" ||
        path == L"data0:/menu/low/01_common.sblytbnd.dcx" ||
        path == L"data0:/menu/lo/01_common.tpf.dcx" ||
        path == L"data0:/menu/lo/01_common.sblytbnd.dcx" ||
        path == L"data0:/menu/hi/01_common.tpf.dcx" ||
        path == L"data0:/menu/hi/01_common.sblytbnd.dcx";
}

bool AllowsDirectRead(const wchar_t* path)
{
    const std::wstring normalized = NormalizePath(path);
    return normalized == L"data0:/menu/low/01_common.tpf.dcx" ||
        normalized == L"data0:/menu/low/01_common.sblytbnd.dcx";
}

bool TryGetCachedFile(const wchar_t* path, std::vector<std::uint8_t>& bytes)
{
    std::lock_guard lock(g_cache_mutex);
    const auto it = g_cached_files.find(NormalizePath(path));
    if (it == g_cached_files.end()) return false;
    bytes = it->second;
    return true;
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

int HookedReadFile(void* file_operator, void* buffer, std::uint64_t size)
{
    const int read = g_orig_read_file ? g_orig_read_file(file_operator, buffer, size) : 0;
    if (read <= 0 || !buffer) return read;

    std::wstring path;
    {
        std::lock_guard lock(g_cache_mutex);
        const auto it = g_pending_reads.find(file_operator);
        if (it == g_pending_reads.end()) return read;
        path = it->second;
        g_pending_reads.erase(it);
    }

    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(read));
    std::memcpy(bytes.data(), buffer, bytes.size());
    {
        std::lock_guard lock(g_cache_mutex);
        g_cached_files[path] = std::move(bytes);
    }
    Log("Asset reader: cached game VFS read for %ls (%d bytes).", path.c_str(), read);
    return read;
}

void TryInstallReadHook(void* file_operator)
{
    if (g_read_file_target || !file_operator || !IsReadableMemory(file_operator, sizeof(std::uintptr_t) * 14)) return;

    const auto* words = reinterpret_cast<const std::uintptr_t*>(file_operator);
    const auto* vtable = reinterpret_cast<const std::uintptr_t*>(words[0]);
    if (!IsReadableMemory(vtable, sizeof(std::uintptr_t) * 26) || !IsExecutableAddress(vtable[25])) return;

    g_read_file_target = reinterpret_cast<void*>(vtable[25]);
    const MH_STATUS create = MH_CreateHook(g_read_file_target, reinterpret_cast<void*>(&HookedReadFile),
        reinterpret_cast<void**>(&g_orig_read_file));
    if (create != MH_OK) {
        Log("Asset reader: read hook creation failed (%d).", static_cast<int>(create));
        g_read_file_target = nullptr;
        return;
    }

    const MH_STATUS enable = MH_EnableHook(g_read_file_target);
    if (enable != MH_OK) {
        Log("Asset reader: read hook enable failed (%d).", static_cast<int>(enable));
        MH_RemoveHook(g_read_file_target);
        g_read_file_target = nullptr;
        g_orig_read_file = nullptr;
        return;
    }
    Log("Asset reader: installed game VFS read cache hook.");
}

bool ReadOpenedFile(void* file_operator, std::vector<std::uint8_t>& bytes, std::uint64_t max_size)
{
    if (!file_operator || !IsReadableMemory(file_operator, sizeof(std::uintptr_t) * 14)) return false;

    const auto* words = reinterpret_cast<const std::uintptr_t*>(file_operator);
    const auto* vtable = reinterpret_cast<const std::uintptr_t*>(words[0]);
    const std::uint64_t size = words[13];
    if (size == 0 || size > max_size || !IsReadableMemory(vtable, sizeof(std::uintptr_t) * 26) || !IsExecutableAddress(vtable[25])) {
        return false;
    }

    bytes.resize(static_cast<std::size_t>(size));
    const auto read_file = g_orig_read_file && g_read_file_target == reinterpret_cast<void*>(vtable[25])
        ? g_orig_read_file
        : reinterpret_cast<DlFileReadFn>(vtable[25]);
    const int read = read_file(file_operator, bytes.data(), bytes.size());
    if (!g_logged_direct_read_result) {
        Log("Asset reader: direct game VFS read returned %d for %llu bytes.",
            read,
            static_cast<unsigned long long>(size));
        g_logged_direct_read_result = true;
    }
    if (read <= 0) {
        bytes.clear();
        return false;
    }

    bytes.resize(static_cast<std::size_t>(read));
    return true;
}

DlDevice* FindMountedVfsDevice(const std::wstring& path)
{
    const std::size_t root_end = path.find(L":/");
    if (root_end == std::wstring::npos) return nullptr;
    const std::wstring root = path.substr(0, root_end);

    const DlDeviceManager2015* manager = LocateDeviceManager();
    if (!manager) return nullptr;

    const std::size_t count = VectorLen(manager->bnd4_mounts);
    if (count == 0 || !IsReadableMemory(manager->bnd4_mounts.first, count * sizeof(DlVirtualMount))) return nullptr;

    for (const DlVirtualMount* it = manager->bnd4_mounts.first; it != manager->bnd4_mounts.last; ++it) {
        if (NormalizePath(ReadDlString(it->root).c_str()) != root) continue;
        if (!it->device || !IsReadableMemory(it->device, sizeof(DlDevice))) return nullptr;
        if (!it->device->vtable || !IsReadableMemory(it->device->vtable, sizeof(DlDeviceVtable))) return nullptr;
        if (!IsExecutableAddress(reinterpret_cast<std::uintptr_t>(it->device->vtable->open_file))) return nullptr;
        return it->device;
    }

    return nullptr;
}

bool ReadFileFromMountedVfs(const wchar_t* path, std::vector<std::uint8_t>& bytes, std::uint64_t max_size)
{
    if (!g_captured_container || !g_captured_allocator) return false;

    const std::wstring normalized = NormalizePath(path);
    if (!IsIconAssetPath(normalized)) return false;

    DlDevice* device = FindMountedVfsDevice(normalized);
    if (!device) return false;

    std::wstring storage(path);
    DlUtf16String2015 dl_path = MakeDlString(storage);
    if (!g_logged_mounted_vfs_attempt) {
        Log("Asset reader: attempting mounted game VFS read for %ls.", path);
        g_logged_mounted_vfs_attempt = true;
    }

    void* file_operator = device->vtable->open_file(
        device,
        &dl_path,
        storage.c_str(),
        g_captured_container,
        g_captured_allocator,
        g_captured_temp_flag);
    if (!file_operator) return false;

    TryInstallReadHook(file_operator);
    if (!ReadOpenedFile(file_operator, bytes, max_size)) return false;

    if (!g_logged_mounted_vfs_read) {
        Log("Asset reader: read icon asset through mounted game VFS.");
        g_logged_mounted_vfs_read = true;
    }
    return true;
}

bool ReadFileDirect(const wchar_t* path, std::vector<std::uint8_t>& bytes, std::uint64_t max_size)
{
    if (!g_disk_device || !g_orig_open_file || !g_captured_container || !g_captured_allocator) return false;

    std::wstring storage(path);
    DlUtf16String2015 dl_path = MakeDlString(storage);
    if (!g_logged_direct_read_attempt) {
        Log("Asset reader: attempting direct game VFS read for %ls.", path);
        g_logged_direct_read_attempt = true;
    }

    void* file_operator = g_orig_open_file(
        g_disk_device,
        &dl_path,
        storage.c_str(),
        g_captured_container,
        g_captured_allocator,
        g_captured_temp_flag);
    if (!file_operator) return false;
    TryInstallReadHook(file_operator);
    return ReadOpenedFile(file_operator, bytes, max_size);
}

void* HookedOpenFile(DlDevice* device, DlUtf16String2015* path, const wchar_t* path_cstr, void* container, void* allocator, bool is_temp_file)
{
    if (container && allocator && !g_captured_container) {
        g_disk_device = device;
        g_captured_container = container;
        g_captured_allocator = allocator;
        g_captured_temp_flag = is_temp_file;
        Log("Asset reader: captured game VFS read context.");
    }

    void* file_operator = g_orig_open_file ? g_orig_open_file(device, path, path_cstr, container, allocator, is_temp_file) : nullptr;
    const std::wstring normalized_path = NormalizePath(path_cstr);
    if (file_operator && IsIconAssetPath(normalized_path)) {
        {
            std::lock_guard lock(g_cache_mutex);
            g_pending_reads[file_operator] = normalized_path;
        }
        TryInstallReadHook(file_operator);
        Log("Asset reader: tracking game VFS icon asset %ls.", normalized_path.c_str());
    }
    return file_operator;
}

}  // namespace

bool ReadFile(const wchar_t* path, std::vector<std::uint8_t>& bytes, std::uint64_t max_size)
{
    bytes.clear();
    if (!path) return false;
    if (TryGetCachedFile(path, bytes)) return true;
    if (ReadFromMemoryVirtualRoot(path, bytes, max_size)) return true;
    if (ReadThroughLoaderFilesystem(path, bytes, max_size)) return true;
    if (ReadFileFromMountedVfs(path, bytes, max_size)) return true;
    if (!AllowsDirectRead(path)) return false;
    if (ReadFileDirect(path, bytes, max_size)) return true;

    if (g_open_file_target && !g_logged_waiting_for_cache) {
        Log("Asset reader: waiting for game VFS context or cached Data0 paths.");
        g_logged_waiting_for_cache = true;
    }
    return false;
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

    CaptureVirtualRoots(*manager);
    LogMountedRoots(*manager);

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

    Log("Asset reader: installed game VFS open hook.");
    return true;
}

bool IsHookInstalled()
{
    return g_open_file_target != nullptr;
}

bool HasGameReadContext()
{
    return g_disk_device && g_orig_open_file && g_captured_container && g_captured_allocator;
}

void Shutdown()
{
    if (g_read_file_target) {
        MH_DisableHook(g_read_file_target);
        MH_RemoveHook(g_read_file_target);
        g_read_file_target = nullptr;
        g_orig_read_file = nullptr;
    }
    if (!g_open_file_target) return;
    MH_DisableHook(g_open_file_target);
    MH_RemoveHook(g_open_file_target);
    g_open_file_target = nullptr;
    g_orig_open_file = nullptr;
    g_captured_container = nullptr;
    g_captured_allocator = nullptr;
    g_captured_temp_flag = false;
    g_logged_waiting_for_cache = false;
    g_logged_memory_root_read = false;
    g_logged_memory_root_miss = false;
    std::lock_guard lock(g_cache_mutex);
    g_pending_reads.clear();
    g_cached_files.clear();
    g_virtual_roots.clear();
}

}  // namespace radial_menu_mod::asset_reader
