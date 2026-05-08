#include "render/icons/icon_loader.h"

#include "core/common.h"
#include "render/assets/dcx.h"
#include "render/assets/icon_assets.h"
#include "render/d3d/d3d_texture_upload.h"
#include "render/vfs/asset_reader.h"

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace radial_menu_mod::icon_loader {
namespace {

struct IconEntry {
    std::size_t atlas_index = 0;
    icon_assets::Rect rect{};
};

struct Atlas {
    std::string name;
    ID3D12Resource* texture = nullptr;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_srv{};
    float width = 1.0f;
    float height = 1.0f;
};

std::array<Atlas, kMaxAtlases> g_atlases = {};
std::size_t g_atlas_count = 0;
std::unordered_map<std::uint32_t, IconEntry> g_icons;
bool g_initialized = false;
bool g_failed = false;

void ResetLoadedIconState()
{
    for (Atlas& atlas : g_atlases) {
        SafeRelease(atlas.texture);
        atlas.name.clear();
        atlas.gpu_srv = {};
        atlas.width = 1.0f;
        atlas.height = 1.0f;
    }
    g_icons.clear();
    g_atlas_count = 0;
}

std::size_t FindOrAddAtlas(std::string name)
{
    for (std::size_t i = 0; i < g_atlas_count; ++i) {
        if (g_atlases[i].name == name) return i;
    }
    if (g_atlas_count >= g_atlases.size()) return g_atlases.size();

    const std::size_t index = g_atlas_count++;
    g_atlases[index].name = std::move(name);
    return index;
}

void AddLayoutIcons(const std::vector<icon_assets::LayoutIcon>& icons)
{
    for (const auto& icon : icons) {
        if (g_icons.find(icon.id) != g_icons.end()) continue;
        const std::size_t atlas_index = FindOrAddAtlas(icon.atlas_name);
        if (atlas_index < g_atlases.size()) g_icons[icon.id] = {atlas_index, icon.rect};
    }
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
        const char* label;
    };
    constexpr Candidate candidates[] = {
        {L"data0:/menu/low/01_common.tpf.dcx", L"data0:/menu/low/01_common.sblytbnd.dcx", true, "low"},
        {L"data0:/menu/lo/01_common.tpf.dcx", L"data0:/menu/lo/01_common.sblytbnd.dcx", false, "lo"},
        {L"data0:/menu/hi/01_common.tpf.dcx", L"data0:/menu/hi/01_common.sblytbnd.dcx", true, "hi"},
    };

    bool saw_assets = false;
    bool uploaded_any = false;
    const char* selected_label = nullptr;
    std::size_t total_uploaded_count = 0;
    for (const Candidate& candidate : candidates) {
        std::vector<std::uint8_t> tpf_dcx;
        std::vector<std::uint8_t> sblyt_dcx;
        if (!asset_reader::ReadFile(candidate.tpf_path, tpf_dcx, 160ull * 1024ull * 1024ull) ||
            !asset_reader::ReadFile(candidate.layout_path, sblyt_dcx, 4ull * 1024ull * 1024ull)) {
            continue;
        }

        saw_assets = true;
        if (!dcx::StartsWithDcx(tpf_dcx)) {
            if (!candidate.decrypt_tpf || !icon_assets::DecryptData0AesRanges(tpf_dcx)) {
                Log("Icon loader: %s TPF is encrypted or invalid.", candidate.label);
                continue;
            }
        }

        std::vector<std::uint8_t> tpf;
        std::vector<std::uint8_t> sblyt;
        const char* dflt_error = "";
        if (!dcx::Decompress(tpf_dcx, tpf, &dflt_error) || !dcx::Decompress(sblyt_dcx, sblyt, &dflt_error)) {
            Log("Icon loader: failed to decompress %s icon assets (dflt=%s).",
                candidate.label,
                dflt_error);
            continue;
        }

        const std::size_t first_candidate_atlas = g_atlas_count;
        if (!uploaded_any) ResetLoadedIconState();
        AddLayoutIcons(icon_assets::ParseLayoutIcons(sblyt));
        std::size_t uploaded_count = 0;
        for (std::size_t i = first_candidate_atlas; i < g_atlas_count; ++i) {
            std::vector<std::uint8_t> dds;
            if (!icon_assets::ExtractTpfTexture(tpf, g_atlases[i].name, dds)) continue;
            d3d_texture_upload::UploadedTexture texture{};
            if (d3d_texture_upload::UploadBc7Texture(device, queue, cpu_srvs[i], dds, texture)) {
                g_atlases[i].texture = texture.resource;
                g_atlases[i].width = texture.width;
                g_atlases[i].height = texture.height;
                g_atlases[i].gpu_srv = gpu_srvs[i];
                uploaded_any = true;
                ++uploaded_count;
            }
        }

        if (uploaded_count == 0 && !uploaded_any) {
            Log("Icon loader: %s icon assets parsed but no atlases uploaded (icons=%zu atlases=%zu).",
                candidate.label,
                g_icons.size(),
                g_atlas_count);
            ResetLoadedIconState();
            continue;
        }

        if (uploaded_count != 0) {
            if (!selected_label) selected_label = candidate.label;
            total_uploaded_count += uploaded_count;
        }
    }

    if (!saw_assets) return false;
    if (g_icons.empty() || !uploaded_any) {
        Log("Icon loader: failed to upload atlases (icons=%zu uploaded=%d).", g_icons.size(), static_cast<int>(uploaded_any));
        ResetLoadedIconState();
        g_failed = true;
        return false;
    }

    g_initialized = true;
    Log("Icon loader ready: source=%s icons=%zu atlases=%zu uploaded_atlases=%zu.",
        selected_label ? selected_label : "unknown",
        g_icons.size(),
        g_atlas_count,
        total_uploaded_count);
    return true;
}

radial_menu::IconTextureInfo Resolve(std::uint32_t icon_id)
{
    if (!g_initialized) return {};

    auto it = g_icons.find(icon_id);
    if (it == g_icons.end()) {
        return {};
    }

    const IconEntry& entry = it->second;
    if (entry.atlas_index >= g_atlas_count) return {};
    const Atlas& atlas = g_atlases[entry.atlas_index];
    if (!atlas.gpu_srv.ptr) return {};
    const icon_assets::Rect& r = entry.rect;
    return {(ImTextureID)atlas.gpu_srv.ptr, {r.x / atlas.width, r.y / atlas.height}, {(r.x + r.w) / atlas.width, (r.y + r.h) / atlas.height}};
}

void Shutdown()
{
    ResetLoadedIconState();
    g_initialized = false;
    g_failed = false;
}

}  // namespace radial_menu_mod::icon_loader
