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
bool g_logged_virtual_mounts = false;
bool g_logged_memory_root_read = false;
bool g_logged_system_root_read = false;
bool g_logged_memory_root_miss = false;
bool g_logged_loader_filesystem_read = false;
bool g_logged_loader_filesystem_miss = false;
bool g_logged_mounted_vfs_attempt = false;
bool g_logged_mounted_vfs_read = false;
bool g_logged_game_read_context = false;
bool g_logged_icon_resolution_diagnostics = false;

bool IsIconAssetPath(const std::wstring& path);

std::string NarrowPath(const std::wstring& path)
{
    if (path.empty()) return {};
    const int required = WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (required <= 1) return {};

    std::string result(static_cast<std::size_t>(required), '\0');
    WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, result.data(), required, nullptr, nullptr);
    result.pop_back();
    return result;
}

std::wstring CanonicalVirtualRoot(std::wstring root)
{
    root = NormalizePath(root.c_str());
    while (!root.empty() && root.back() == L'/') root.pop_back();
    if (!root.empty() && root.back() == L':') root.pop_back();
    return root;
}

bool SplitVirtualPath(const std::wstring& path, std::wstring& root, std::wstring& relative)
{
    const std::size_t root_end = path.find(L":/");
    if (root_end == std::wstring::npos) return false;

    root = CanonicalVirtualRoot(path.substr(0, root_end));
    relative = path.substr(root_end + 2);
    return !root.empty() && !relative.empty();
}

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
        if (!g_logged_loader_filesystem_read) Log("Asset reader: icon asset resolved through loader filesystem override.");
        g_logged_loader_filesystem_read = true;
        return true;
    }

    wchar_t cwd[MAX_PATH] = {};
    if (GetCurrentDirectoryW(MAX_PATH, cwd)) {
        const std::wstring cwd_relative = JoinDiskPath(cwd, relative);
        if (ReadDiskFile(cwd_relative, bytes, max_size)) {
            if (!g_logged_loader_filesystem_read) Log("Asset reader: icon asset resolved through loader filesystem override.");
            g_logged_loader_filesystem_read = true;
            return true;
        }
    }
    g_logged_loader_filesystem_miss = true;
    return false;
}

void CaptureVirtualRoots(const DlDeviceManager2015& manager)
{
    const std::size_t count = VectorLen(manager.virtual_roots);
    if (count == 0 || !IsReadableMemory(manager.virtual_roots.first, count * sizeof(DlVirtualRoot))) return;

    std::vector<VirtualRootPath> roots;
    roots.reserve(count);
    for (const DlVirtualRoot* it = manager.virtual_roots.first; it != manager.virtual_roots.last; ++it) {
        std::wstring root = NormalizePath(ReadDlString(it->root).c_str());
        std::wstring expanded = ReadDlString(it->expanded);
        if (root.empty() || expanded.empty()) continue;
        roots.push_back({std::move(root), std::move(expanded)});
    }

    if (roots.empty()) return;

    const bool changed = roots.size() != g_virtual_roots.size() ||
        !std::equal(roots.begin(), roots.end(), g_virtual_roots.begin(),
            [](const VirtualRootPath& lhs, const VirtualRootPath& rhs) {
                return lhs.root == rhs.root && lhs.expanded == rhs.expanded;
            });
    if (!changed) return;

    g_virtual_roots = std::move(roots);
    Log("Asset reader: captured %zu game VFS virtual roots.", g_virtual_roots.size());
}

void RefreshVirtualRoots()
{
    DlDeviceManager2015* manager = LocateDeviceManager();
    if (!manager) return;
    CaptureVirtualRoots(*manager);
}

void LogVirtualMounts(const char* label, const DlVector2015<DlVirtualMount>& mounts)
{
    (void)label;
    (void)mounts;
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
    std::wstring path_root;
    std::wstring relative;
    if (!SplitVirtualPath(normalized_path, path_root, relative)) return false;

    for (const VirtualRootPath& root : g_virtual_roots) {
        if (CanonicalVirtualRoot(root.root) != path_root) continue;

        const std::wstring disk_path = JoinDiskPath(root.expanded, relative);
        if (ReadDiskFile(disk_path, bytes, max_size)) {
            if (!g_logged_memory_root_read) Log("Asset reader: icon asset resolved through game VFS root mapping.");
            g_logged_memory_root_read = true;
            return true;
        }
    }

    if (!g_virtual_roots.empty()) g_logged_memory_root_miss = true;
    return false;
}

bool ReadFromMemorySystemRoot(const wchar_t* path, std::vector<std::uint8_t>& bytes, std::uint64_t max_size)
{
    std::wstring relative;
    if (!Data0IconPathToRelativePath(path, relative)) return false;

    for (const VirtualRootPath& root : g_virtual_roots) {
        if (root.root != L"system") continue;

        const std::wstring disk_path = JoinDiskPath(root.expanded, relative);
        if (ReadDiskFile(disk_path, bytes, max_size)) {
            if (!g_logged_system_root_read) Log("Asset reader: icon asset resolved through game system root mapping.");
            g_logged_system_root_read = true;
            return true;
        }
    }
    return false;
}

bool IsIconAssetPath(const std::wstring& path)
{
    return path == L"data0:/menu/low/01_common.tpf.dcx" ||
        path == L"data0:/menu/low/01_common.sblytbnd.dcx" ||
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
    g_logged_direct_read_result = true;
    if (read <= 0) {
        bytes.clear();
        return false;
    }

    bytes.resize(static_cast<std::size_t>(read));
    return true;
}

DlDevice* FindMountedVfsDevice(const std::wstring& path)
{
    std::wstring root;
    std::wstring relative;
    if (!SplitVirtualPath(path, root, relative)) return nullptr;

    const DlDeviceManager2015* manager = LocateDeviceManager();
    if (!manager) return nullptr;

    const std::size_t count = VectorLen(manager->bnd4_mounts);
    if (count == 0 || !IsReadableMemory(manager->bnd4_mounts.first, count * sizeof(DlVirtualMount))) return nullptr;

    for (const DlVirtualMount* it = manager->bnd4_mounts.first; it != manager->bnd4_mounts.last; ++it) {
        if (CanonicalVirtualRoot(ReadDlString(it->root)) != root) continue;
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
    g_logged_mounted_vfs_attempt = true;

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

    if (!g_logged_mounted_vfs_read) Log("Asset reader: icon asset resolved through mounted game VFS.");
    g_logged_mounted_vfs_read = true;
    return true;
}

void LogIconResolutionDiagnostics(const std::wstring& normalized_path)
{
    if (g_logged_icon_resolution_diagnostics || g_virtual_roots.empty()) return;
    g_logged_icon_resolution_diagnostics = true;

    const DlDeviceManager2015* manager = LocateDeviceManager();
    const std::size_t mount_count = manager ? VectorLen(manager->bnd4_mounts) : 0;
    Log("Asset reader: icon read unresolved for '%s' (virtual_roots=%zu bnd4_mounts=%zu read_context=%d).",
        NarrowPath(normalized_path).c_str(),
        g_virtual_roots.size(),
        mount_count,
        (g_captured_container && g_captured_allocator) ? 1 : 0);

    bool logged_relevant_root = false;
    for (const VirtualRootPath& root : g_virtual_roots) {
        const std::wstring canonical = CanonicalVirtualRoot(root.root);
        if (canonical != L"data0" && canonical != L"system") continue;

        Log("Asset reader: virtual root '%s' expands to '%s'.",
            NarrowPath(root.root).c_str(),
            NarrowPath(root.expanded).c_str());
        logged_relevant_root = true;
    }
    if (!logged_relevant_root) Log("Asset reader: no data0/system virtual roots were captured.");

    if (!manager || mount_count == 0) return;
    if (!IsReadableMemory(manager->bnd4_mounts.first, mount_count * sizeof(DlVirtualMount))) {
        Log("Asset reader: bnd4 mount table is not readable for icon diagnostics.");
        return;
    }

    bool logged_data_mount = false;
    for (const DlVirtualMount* it = manager->bnd4_mounts.first; it != manager->bnd4_mounts.last; ++it) {
        const std::wstring root = ReadDlString(it->root);
        if (CanonicalVirtualRoot(root) != L"data0") continue;

        Log("Asset reader: bnd4 data0 mount root='%s' device=%p.",
            NarrowPath(root).c_str(),
            static_cast<void*>(it->device));
        logged_data_mount = true;
    }
    if (!logged_data_mount) Log("Asset reader: no bnd4 data0 mount was found.");
}

bool ReadFileDirect(const wchar_t* path, std::vector<std::uint8_t>& bytes, std::uint64_t max_size)
{
    if (!g_disk_device || !g_orig_open_file || !g_captured_container || !g_captured_allocator) return false;

    std::wstring storage(path);
    DlUtf16String2015 dl_path = MakeDlString(storage);
    g_logged_direct_read_attempt = true;

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
    }

    void* file_operator = g_orig_open_file ? g_orig_open_file(device, path, path_cstr, container, allocator, is_temp_file) : nullptr;
    if (g_captured_container && !g_logged_game_read_context) {
        Log("Asset reader: captured game VFS read context.");
        g_logged_game_read_context = true;
    }
    const std::wstring normalized_path = NormalizePath(path_cstr);
    if (file_operator && IsIconAssetPath(normalized_path)) {
        {
            std::lock_guard lock(g_cache_mutex);
            g_pending_reads[file_operator] = normalized_path;
        }
        TryInstallReadHook(file_operator);
    }
    return file_operator;
}

}  // namespace

bool ReadFile(const wchar_t* path, std::vector<std::uint8_t>& bytes, std::uint64_t max_size)
{
    bytes.clear();
    if (!path) return false;
    const std::wstring normalized_path = NormalizePath(path);
    if (TryGetCachedFile(path, bytes)) return true;
    RefreshVirtualRoots();
    if (ReadFromMemoryVirtualRoot(path, bytes, max_size)) return true;
    if (ReadFromMemorySystemRoot(path, bytes, max_size)) return true;
    if (ReadThroughLoaderFilesystem(path, bytes, max_size)) return true;
    if (ReadFileFromMountedVfs(path, bytes, max_size)) return true;
    if (!AllowsDirectRead(path)) return false;
    if (ReadFileDirect(path, bytes, max_size)) return true;

    if (IsIconAssetPath(normalized_path)) LogIconResolutionDiagnostics(normalized_path);
    if (g_open_file_target) g_logged_waiting_for_cache = true;
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
    g_logged_system_root_read = false;
    g_logged_memory_root_miss = false;
    g_logged_icon_resolution_diagnostics = false;
    std::lock_guard lock(g_cache_mutex);
    g_pending_reads.clear();
    g_cached_files.clear();
    g_virtual_roots.clear();
}

}  // namespace radial_menu_mod::asset_reader
