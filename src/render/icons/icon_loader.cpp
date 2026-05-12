#include "render/icons/icon_loader.h"

#include "core/common.h"
#include "render/assets/dcx.h"
#include "render/assets/icon_assets.h"
#include "render/d3d/d3d_texture_upload.h"
#include "render/vfs/asset_reader.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace radial_menu_mod::icon_loader {
namespace {

struct IconEntry {
    std::size_t atlas_index = 0;
    icon_assets::Rect rect{};
};

struct Atlas {
    const char* source = "";
    std::string name;
    ID3D12Resource* texture = nullptr;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_srv{};
    float width = 1.0f;
    float height = 1.0f;
    bool upload_failed = false;
};

struct IconSource {
    explicit IconSource(const char* source_label) : label(source_label) {}

    const char* label = "";
    std::unordered_map<std::uint32_t, IconEntry> icons;
    std::vector<std::uint8_t> tpf;
    bool saw_assets = false;
    bool uploaded_any = false;
    std::size_t uploaded_count = 0;
    std::size_t missing_texture_count = 0;
    std::size_t upload_failure_count = 0;
};

std::array<Atlas, kMaxAtlases> g_atlases = {};
std::size_t g_atlas_count = 0;
IconSource g_hi_source{"hi"};
IconSource g_low_source{"low"};
ID3D12Device* g_device = nullptr;
ID3D12CommandQueue* g_queue = nullptr;
std::array<D3D12_CPU_DESCRIPTOR_HANDLE, kMaxAtlases> g_cpu_srvs = {};
std::array<D3D12_GPU_DESCRIPTOR_HANDLE, kMaxAtlases> g_gpu_srvs = {};
std::size_t g_srv_count = 0;
bool g_initialized = false;
bool g_failed = false;
bool g_logged_begin = false;
bool g_logged_missing_assets = false;
bool g_logged_waiting_for_vfs_context = false;
std::unordered_set<std::uint32_t> g_logged_missing_icon_ids;
std::vector<std::size_t> g_required_atlases;
bool g_logged_hi_read_result = false;
bool g_logged_low_read_result = false;
bool g_last_hi_tpf_read = false;
bool g_last_hi_layout_read = false;
bool g_last_low_tpf_read = false;
bool g_last_low_layout_read = false;

void ResetLoadedIconState()
{
    for (Atlas& atlas : g_atlases) {
        SafeRelease(atlas.texture);
        atlas.source = "";
        atlas.name.clear();
        atlas.gpu_srv = {};
        atlas.width = 1.0f;
        atlas.height = 1.0f;
        atlas.upload_failed = false;
    }
    g_hi_source.icons.clear();
    g_hi_source.tpf.clear();
    g_hi_source.saw_assets = false;
    g_hi_source.uploaded_any = false;
    g_hi_source.uploaded_count = 0;
    g_hi_source.missing_texture_count = 0;
    g_hi_source.upload_failure_count = 0;
    g_low_source.icons.clear();
    g_low_source.tpf.clear();
    g_low_source.saw_assets = false;
    g_low_source.uploaded_any = false;
    g_low_source.uploaded_count = 0;
    g_low_source.missing_texture_count = 0;
    g_low_source.upload_failure_count = 0;
    g_atlas_count = 0;
    g_required_atlases.clear();
}

std::size_t FindOrAddAtlas(const char* source, std::string name)
{
    for (std::size_t i = 0; i < g_atlas_count; ++i) {
        if (g_atlases[i].source == source && g_atlases[i].name == name) return i;
    }
    if (g_atlas_count >= g_atlases.size()) return g_atlases.size();

    const std::size_t index = g_atlas_count++;
    g_atlases[index].source = source;
    g_atlases[index].name = std::move(name);
    return index;
}

std::vector<std::size_t> AddLayoutIcons(IconSource& source, const std::vector<icon_assets::LayoutIcon>& icons)
{
    std::vector<std::size_t> referenced_atlases;
    for (const auto& icon : icons) {
        const std::size_t atlas_index = FindOrAddAtlas(source.label, icon.atlas_name);
        if (atlas_index >= g_atlases.size()) continue;

        if (std::find(referenced_atlases.begin(), referenced_atlases.end(), atlas_index) == referenced_atlases.end()) {
            referenced_atlases.push_back(atlas_index);
        }
        source.icons[icon.id] = {atlas_index, icon.rect};
    }
    return referenced_atlases;
}

const IconEntry* FindUsableIcon(const IconSource& source, std::uint32_t icon_id)
{
    const auto it = source.icons.find(icon_id);
    if (it == source.icons.end()) return nullptr;

    const IconEntry& entry = it->second;
    if (entry.atlas_index >= g_atlas_count) return nullptr;

    const Atlas& atlas = g_atlases[entry.atlas_index];
    return atlas.gpu_srv.ptr ? &entry : nullptr;
}

const IconEntry* FindLayoutIcon(const IconSource& source, std::uint32_t icon_id)
{
    const auto it = source.icons.find(icon_id);
    if (it == source.icons.end()) return nullptr;
    if (it->second.atlas_index >= g_atlas_count) return nullptr;
    return &it->second;
}

IconSource* SourceForAtlas(const Atlas& atlas)
{
    if (atlas.source == g_hi_source.label) return &g_hi_source;
    if (atlas.source == g_low_source.label) return &g_low_source;
    return nullptr;
}

bool UploadAtlas(std::size_t atlas_index)
{
    if (atlas_index >= g_atlas_count || atlas_index >= g_srv_count) return false;

    Atlas& atlas = g_atlases[atlas_index];
    if (atlas.texture && atlas.gpu_srv.ptr) return true;
    if (atlas.upload_failed) return false;

    IconSource* source = SourceForAtlas(atlas);
    if (!source || source->tpf.empty()) {
        atlas.upload_failed = true;
        return false;
    }

    std::vector<std::uint8_t> dds;
    if (!icon_assets::ExtractTpfTexture(source->tpf, atlas.name, dds)) {
        ++source->missing_texture_count;
        if (source->missing_texture_count <= 4) {
            Log("Icon loader: %s missing atlas texture '%s' in TPF.", source->label, atlas.name.c_str());
        }
        atlas.upload_failed = true;
        return false;
    }

    d3d_texture_upload::UploadedTexture texture{};
    if (!d3d_texture_upload::UploadBc7Texture(g_device, g_queue, g_cpu_srvs[atlas_index], dds, texture)) {
        ++source->upload_failure_count;
        if (source->upload_failure_count <= 4) {
            Log("Icon loader: %s failed to upload atlas '%s' (%zu DDS bytes).",
                source->label,
                atlas.name.c_str(),
                dds.size());
        }
        atlas.upload_failed = true;
        return false;
    }

    atlas.texture = texture.resource;
    atlas.width = texture.width;
    atlas.height = texture.height;
    atlas.gpu_srv = g_gpu_srvs[atlas_index];
    source->uploaded_any = true;
    ++source->uploaded_count;
    Log("Icon loader uploaded atlas[%zu]: source=%s name='%s' size=%.0fx%.0f.",
        atlas_index,
        source->label,
        atlas.name.c_str(),
        atlas.width,
        atlas.height);
    return true;
}

void LogSourceReadResult(const IconSource& source, bool read_tpf, bool read_layout)
{
    bool* logged = source.label == g_hi_source.label ? &g_logged_hi_read_result : &g_logged_low_read_result;
    bool* last_tpf = source.label == g_hi_source.label ? &g_last_hi_tpf_read : &g_last_low_tpf_read;
    bool* last_layout = source.label == g_hi_source.label ? &g_last_hi_layout_read : &g_last_low_layout_read;
    if (*logged && *last_tpf == read_tpf && *last_layout == read_layout) return;

    *logged = true;
    *last_tpf = read_tpf;
    *last_layout = read_layout;
    Log("Icon loader source read result: source=%s tpf=%d layout=%d.",
        source.label,
        read_tpf ? 1 : 0,
        read_layout ? 1 : 0);
}

}  // namespace

bool TryInitialize(
    ID3D12Device* device,
    ID3D12CommandQueue* queue,
    const D3D12_CPU_DESCRIPTOR_HANDLE* cpu_srvs,
    const D3D12_GPU_DESCRIPTOR_HANDLE* gpu_srvs,
    std::size_t srv_count)
{
    if (g_initialized) return true;
    if (g_failed || !device || !queue || !cpu_srvs || !gpu_srvs || srv_count < g_atlases.size()) return false;

    struct Candidate {
        const wchar_t* tpf_path;
        const wchar_t* layout_path;
        bool decrypt_tpf;
        IconSource* source;
    };
    constexpr Candidate candidates[] = {
        {L"data0:/menu/hi/01_common.tpf.dcx", L"data0:/menu/hi/01_common.sblytbnd.dcx", true, &g_hi_source},
        {L"data0:/menu/low/01_common.tpf.dcx", L"data0:/menu/low/01_common.sblytbnd.dcx", true, &g_low_source},
    };

    g_logged_begin = true;
    if (asset_reader::HasGameReadContext()) g_logged_waiting_for_vfs_context = false;
    ResetLoadedIconState();
    g_device = device;
    g_queue = queue;
    g_srv_count = srv_count;
    for (std::size_t i = 0; i < g_atlases.size(); ++i) {
        g_cpu_srvs[i] = cpu_srvs[i];
        g_gpu_srvs[i] = gpu_srvs[i];
    }

    const char* selected_label = nullptr;

    for (const Candidate& candidate : candidates) {
        IconSource& source = *candidate.source;
        std::vector<std::uint8_t> tpf_dcx;
        std::vector<std::uint8_t> sblyt_dcx;
        const bool read_tpf = asset_reader::ReadFile(candidate.tpf_path, tpf_dcx, 160ull * 1024ull * 1024ull);
        const bool read_layout = asset_reader::ReadFile(candidate.layout_path, sblyt_dcx, 4ull * 1024ull * 1024ull);
        LogSourceReadResult(source, read_tpf, read_layout);
        if (!read_tpf || !read_layout) {
            continue;
        }

        source.saw_assets = true;

        if (!dcx::StartsWithDcx(tpf_dcx)) {
            if (!candidate.decrypt_tpf || !icon_assets::DecryptData0AesRanges(tpf_dcx)) {
                Log("Icon loader: %s TPF is encrypted or invalid.", source.label);
                continue;
            }
        }

        std::vector<std::uint8_t> tpf;
        std::vector<std::uint8_t> sblyt;
        const char* dflt_error = "";
        if (!dcx::Decompress(tpf_dcx, tpf, &dflt_error) || !dcx::Decompress(sblyt_dcx, sblyt, &dflt_error)) {
            Log("Icon loader: failed to decompress %s icon assets (dflt=%s).",
                source.label,
                dflt_error);
            continue;
        }

        const auto layout_icons = icon_assets::ParseLayoutIcons(sblyt);
        const auto candidate_atlases = AddLayoutIcons(source, layout_icons);
        if (source.icons.empty() || candidate_atlases.empty()) {
            Log("Icon loader: %s icon layout parsed but no icons were found.", source.label);
            continue;
        }

        source.tpf = std::move(tpf);
        if (!selected_label) selected_label = source.label;
    }

    if (!g_hi_source.saw_assets && !g_low_source.saw_assets) {
        if (asset_reader::IsHookInstalled() && !asset_reader::HasGameReadContext()) {
            if (!g_logged_waiting_for_vfs_context) {
                Log("Icon loader: waiting for game VFS read context and cached icon archives.");
                g_logged_waiting_for_vfs_context = true;
            }
            return false;
        }

        if (!g_logged_missing_assets) {
            Log("Icon loader: no cached game VFS icon archives available yet.");
            g_logged_missing_assets = true;
        }
        return false;
    }
    if (g_hi_source.icons.empty() && g_low_source.icons.empty()) {
        Log("Icon loader: failed to parse icon layouts (hi_icons=%zu low_icons=%zu).",
            g_hi_source.icons.size(),
            g_low_source.icons.size());
        ResetLoadedIconState();
        g_failed = true;
        return false;
    }

    g_initialized = true;
    Log("Icon loader metadata ready (primary=%s hi_icons=%zu low_icons=%zu atlases=%zu).",
        selected_label ? selected_label : "unknown",
        g_hi_source.icons.size(),
        g_low_source.icons.size(),
        g_atlas_count);
    return true;
}

void SetRequiredIcons(const std::vector<std::uint32_t>& icon_ids)
{
    if (!g_initialized || !g_device || !g_queue) return;

    std::vector<std::size_t> required_atlases;
    required_atlases.reserve(icon_ids.size());
    std::size_t uploaded_count = 0;

    const auto add_required = [&required_atlases](std::size_t atlas_index) {
        if (std::find(required_atlases.begin(), required_atlases.end(), atlas_index) == required_atlases.end()) {
            required_atlases.push_back(atlas_index);
        }
    };

    for (const std::uint32_t icon_id : icon_ids) {
        if (icon_id == 0) continue;

        bool uploaded = false;
        if (const IconEntry* entry = FindLayoutIcon(g_hi_source, icon_id)) {
            const bool was_uploaded = g_atlases[entry->atlas_index].texture != nullptr;
            uploaded = UploadAtlas(entry->atlas_index);
            if (uploaded) {
                add_required(entry->atlas_index);
                if (!was_uploaded) ++uploaded_count;
            }
        }

        if (!uploaded) {
            if (const IconEntry* entry = FindLayoutIcon(g_low_source, icon_id)) {
                const bool was_uploaded = g_atlases[entry->atlas_index].texture != nullptr;
                uploaded = UploadAtlas(entry->atlas_index);
                if (uploaded) {
                    add_required(entry->atlas_index);
                    if (!was_uploaded) ++uploaded_count;
                }
            }
        }
    }

    std::sort(required_atlases.begin(), required_atlases.end());

    const bool changed = required_atlases != g_required_atlases;
    g_required_atlases = std::move(required_atlases);
    if (changed || uploaded_count != 0) {
        Log("Icon loader required atlases updated (icons=%zu atlases=%zu uploaded=%zu).",
            icon_ids.size(),
            g_required_atlases.size(),
            uploaded_count);
    }
}

bool PreloadIcons(const std::vector<std::uint32_t>& icon_ids, std::size_t max_uploads)
{
    if (!g_initialized || !g_device || !g_queue) return false;

    std::size_t uploaded_count = 0;
    bool pending_upload = false;

    const auto try_upload = [&](const IconEntry* entry) -> bool {
        if (!entry || entry->atlas_index >= g_atlas_count) return false;

        Atlas& atlas = g_atlases[entry->atlas_index];
        if (atlas.texture && atlas.gpu_srv.ptr) return true;
        if (atlas.upload_failed) return false;

        if (uploaded_count >= max_uploads) {
            pending_upload = true;
            return false;
        }

        const bool uploaded = UploadAtlas(entry->atlas_index);
        if (uploaded) ++uploaded_count;
        return uploaded;
    };

    for (const std::uint32_t icon_id : icon_ids) {
        if (icon_id == 0) continue;
        if (try_upload(FindLayoutIcon(g_hi_source, icon_id))) continue;
        (void)try_upload(FindLayoutIcon(g_low_source, icon_id));
    }

    return !pending_upload;
}

radial_menu::IconTextureInfo Resolve(std::uint32_t icon_id)
{
    if (!g_initialized) return {};

    const IconEntry* resolved = FindUsableIcon(g_hi_source, icon_id);
    if (!resolved) {
        resolved = FindUsableIcon(g_low_source, icon_id);
    }

    if (!resolved) {
        if (g_logged_missing_icon_ids.insert(icon_id).second) {
            Log("Icon loader: icon id %u unavailable in hi/low layouts or uploaded atlases (hi_icons=%zu low_icons=%zu atlases=%zu).",
                icon_id,
                g_hi_source.icons.size(),
                g_low_source.icons.size(),
                g_atlas_count);
        }
        return {};
    }

    const IconEntry& entry = *resolved;
    const Atlas& atlas = g_atlases[entry.atlas_index];
    const icon_assets::Rect& r = entry.rect;
    return {(ImTextureID)atlas.gpu_srv.ptr, {r.x / atlas.width, r.y / atlas.height}, {(r.x + r.w) / atlas.width, (r.y + r.h) / atlas.height}};
}

void Shutdown()
{
    ResetLoadedIconState();
    g_device = nullptr;
    g_queue = nullptr;
    g_srv_count = 0;
    g_cpu_srvs = {};
    g_gpu_srvs = {};
    g_logged_missing_icon_ids.clear();
    g_logged_hi_read_result = false;
    g_logged_low_read_result = false;
    g_last_hi_tpf_read = false;
    g_last_hi_layout_read = false;
    g_last_low_tpf_read = false;
    g_last_low_layout_read = false;
    g_initialized = false;
    g_failed = false;
}

}  // namespace radial_menu_mod::icon_loader
