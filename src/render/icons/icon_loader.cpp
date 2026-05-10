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
};

struct IconSource {
    explicit IconSource(const char* source_label) : label(source_label) {}

    const char* label = "";
    std::unordered_map<std::uint32_t, IconEntry> icons;
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
bool g_initialized = false;
bool g_failed = false;
bool g_logged_begin = false;
bool g_logged_missing_assets = false;
bool g_logged_waiting_for_vfs_context = false;
std::unordered_set<std::uint32_t> g_logged_missing_icon_ids;

void ResetLoadedIconState()
{
    for (Atlas& atlas : g_atlases) {
        SafeRelease(atlas.texture);
        atlas.source = "";
        atlas.name.clear();
        atlas.gpu_srv = {};
        atlas.width = 1.0f;
        atlas.height = 1.0f;
    }
    g_hi_source.icons.clear();
    g_hi_source.saw_assets = false;
    g_hi_source.uploaded_any = false;
    g_hi_source.uploaded_count = 0;
    g_hi_source.missing_texture_count = 0;
    g_hi_source.upload_failure_count = 0;
    g_low_source.icons.clear();
    g_low_source.saw_assets = false;
    g_low_source.uploaded_any = false;
    g_low_source.uploaded_count = 0;
    g_low_source.missing_texture_count = 0;
    g_low_source.upload_failure_count = 0;
    g_atlas_count = 0;
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

void LogLoadedAtlases()
{
    for (std::size_t i = 0; i < g_atlas_count; ++i) {
        const Atlas& atlas = g_atlases[i];
        Log("Icon loader atlas[%zu]: source=%s name='%s' uploaded=%d size=%.0fx%.0f.",
            i,
            atlas.source,
            atlas.name.c_str(),
            static_cast<int>(atlas.gpu_srv.ptr != 0),
            atlas.width,
            atlas.height);
    }
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

    const char* selected_label = nullptr;
    std::size_t total_uploaded_count = 0;
    std::size_t total_missing_texture_count = 0;
    std::size_t total_upload_failure_count = 0;
    g_logged_begin = true;
    if (asset_reader::HasGameReadContext()) g_logged_waiting_for_vfs_context = false;
    ResetLoadedIconState();

    for (const Candidate& candidate : candidates) {
        IconSource& source = *candidate.source;
        std::vector<std::uint8_t> tpf_dcx;
        std::vector<std::uint8_t> sblyt_dcx;
        const bool read_tpf = asset_reader::ReadFile(candidate.tpf_path, tpf_dcx, 160ull * 1024ull * 1024ull);
        const bool read_layout = asset_reader::ReadFile(candidate.layout_path, sblyt_dcx, 4ull * 1024ull * 1024ull);
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
        std::size_t uploaded_count = 0;
        std::size_t missing_texture_count = 0;
        std::size_t upload_failure_count = 0;
        for (const std::size_t i : candidate_atlases) {
            std::vector<std::uint8_t> dds;
            if (!icon_assets::ExtractTpfTexture(tpf, g_atlases[i].name, dds)) {
                ++missing_texture_count;
                if (missing_texture_count <= 4) {
                    Log("Icon loader: %s missing atlas texture '%s' in TPF.", source.label, g_atlases[i].name.c_str());
                }
                continue;
            }

            d3d_texture_upload::UploadedTexture texture{};
            if (d3d_texture_upload::UploadBc7Texture(device, queue, cpu_srvs[i], dds, texture)) {
                g_atlases[i].texture = texture.resource;
                g_atlases[i].width = texture.width;
                g_atlases[i].height = texture.height;
                g_atlases[i].gpu_srv = gpu_srvs[i];
                ++uploaded_count;
            } else {
                ++upload_failure_count;
                if (upload_failure_count <= 4) {
                    Log("Icon loader: %s failed to upload atlas '%s' (%zu DDS bytes).",
                        source.label,
                        g_atlases[i].name.c_str(),
                        dds.size());
                }
            }
        }
        source.uploaded_any = uploaded_count != 0;
        source.uploaded_count = uploaded_count;
        source.missing_texture_count = missing_texture_count;
        source.upload_failure_count = upload_failure_count;

        if (uploaded_count == 0) {
            Log("Icon loader: %s icon assets parsed but no atlases uploaded (icons=%zu atlases=%zu).",
                source.label,
                source.icons.size(),
                g_atlas_count);
            continue;
        }
        total_missing_texture_count += missing_texture_count;
        total_upload_failure_count += upload_failure_count;

        if (uploaded_count != 0) {
            if (!selected_label) selected_label = source.label;
            total_uploaded_count += uploaded_count;
        }
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
    if ((g_hi_source.icons.empty() && g_low_source.icons.empty()) || total_uploaded_count == 0) {
        Log("Icon loader: failed to upload atlases (hi_icons=%zu low_icons=%zu uploaded=%zu).",
            g_hi_source.icons.size(),
            g_low_source.icons.size(),
            total_uploaded_count);
        ResetLoadedIconState();
        g_failed = true;
        return false;
    }

    g_initialized = true;
    Log("Icon loader ready (primary=%s hi_icons=%zu low_icons=%zu atlases=%zu uploaded=%zu missing_textures=%zu upload_failures=%zu).",
        selected_label ? selected_label : "unknown",
        g_hi_source.icons.size(),
        g_low_source.icons.size(),
        g_atlas_count,
        total_uploaded_count,
        total_missing_texture_count,
        total_upload_failure_count);
    LogLoadedAtlases();
    return true;
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
    g_logged_missing_icon_ids.clear();
    g_initialized = false;
    g_failed = false;
}

}  // namespace radial_menu_mod::icon_loader
