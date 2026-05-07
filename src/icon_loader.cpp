#include "icon_loader.h"

#include "asset_reader.h"
#include "common.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
#include <windows.h>
#include <bcrypt.h>

namespace radial_spell_menu::icon_loader {
namespace {

struct Rect {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
};

struct IconEntry {
    std::size_t atlas_index = 0;
    Rect rect{};
};

struct Atlas {
    const char* name = nullptr;
    ID3D12Resource* texture = nullptr;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_srv{};
    float width = 1.0f;
    float height = 1.0f;
};

using OodleDecompressFn = long long(__stdcall *)(
    const void*, long long, void*, long long, int, int, int, void*, long long, void*, void*, void*, long long, int);

std::array<Atlas, kMaxAtlases> g_atlases = {{
    {"SB_Icon_02_A"},
    {"SB_Icon_02_B"},
    {"SB_Icon_07_dlc"},
    {"SB_Icon_07_dlc_A"},
    {"SB_Icon_08_dlc"},
    {"SB_Icon_08_dlc_A"},
}};
std::unordered_map<std::uint32_t, IconEntry> g_icons;
bool g_initialized = false;
bool g_failed = false;

std::uint32_t ReadLe32(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    return std::uint32_t(bytes[offset]) |
           (std::uint32_t(bytes[offset + 1]) << 8) |
           (std::uint32_t(bytes[offset + 2]) << 16) |
           (std::uint32_t(bytes[offset + 3]) << 24);
}

std::uint32_t ReadBe32(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    return (std::uint32_t(bytes[offset]) << 24) |
           (std::uint32_t(bytes[offset + 1]) << 16) |
           (std::uint32_t(bytes[offset + 2]) << 8) |
           std::uint32_t(bytes[offset + 3]);
}

bool HasBytes(const std::vector<std::uint8_t>& bytes, std::size_t offset, const char* text)
{
    for (std::size_t i = 0; text[i] != '\0'; ++i) {
        if (offset + i >= bytes.size() || bytes[offset + i] != static_cast<std::uint8_t>(text[i])) return false;
    }
    return true;
}

bool DecompressDcxKrak(const std::vector<std::uint8_t>& dcx, std::vector<std::uint8_t>& out)
{
    out.clear();
    if (dcx.size() < 0x4c || !HasBytes(dcx, 0, "DCX\0") || !HasBytes(dcx, 0x28, "KRAK")) return false;

    const std::uint32_t raw_size = ReadBe32(dcx, 0x1c);
    const std::uint32_t comp_size = ReadBe32(dcx, 0x20);
    if (0x4cull + comp_size > dcx.size()) return false;

    HMODULE oodle = LoadLibraryA("oo2core_6_win64.dll");
    if (!oodle) return false;
    auto decompress = reinterpret_cast<OodleDecompressFn>(GetProcAddress(oodle, "OodleLZ_Decompress"));
    if (!decompress) return false;

    out.resize(raw_size);
    const long long written = decompress(dcx.data() + 0x4c, comp_size, out.data(), raw_size, 1, 0, 0, nullptr, 0, nullptr, nullptr, nullptr, 0, 3);
    if (written != raw_size) {
        out.clear();
        return false;
    }
    return true;
}

bool DecryptData0AesRanges(std::vector<std::uint8_t>& bytes)
{
    // Data0.bhd marks only selected byte ranges as AES-encrypted for this entry.
    static constexpr std::uint8_t kKey[16] = {
        0x01, 0xed, 0xbd, 0xd5, 0xd1, 0xae, 0x25, 0xcc,
        0xc5, 0x53, 0xc0, 0xcb, 0xff, 0x79, 0x76, 0x5f,
    };
    static constexpr std::pair<std::uint32_t, std::uint32_t> kRanges[] = {
        {0, 1024},
        {2048, 6144},
        {1048576, 1049600},
    };

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_AES_ALGORITHM, nullptr, 0) != 0) return false;
    if (BCryptSetProperty(algorithm, BCRYPT_CHAINING_MODE, reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_ECB)), sizeof(BCRYPT_CHAIN_MODE_ECB), 0) != 0 ||
        BCryptGenerateSymmetricKey(algorithm, &key, nullptr, 0, const_cast<PUCHAR>(kKey), sizeof(kKey), 0) != 0) {
        if (key) BCryptDestroyKey(key);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return false;
    }

    bool ok = true;
    for (const auto& [begin, end] : kRanges) {
        if (begin >= bytes.size()) continue;
        const std::uint32_t clamped_end = std::min<std::uint32_t>(end, static_cast<std::uint32_t>(bytes.size()));
        const std::uint32_t size = clamped_end - begin;
        if (size == 0 || (size % 16) != 0) {
            ok = false;
            break;
        }
        ULONG written = 0;
        if (BCryptDecrypt(key, bytes.data() + begin, size, nullptr, nullptr, 0, bytes.data() + begin, size, &written, 0) != 0 || written != size) {
            ok = false;
            break;
        }
    }

    BCryptDestroyKey(key);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return ok;
}

bool StartsWithDcx(const std::vector<std::uint8_t>& bytes)
{
    return bytes.size() >= 4 && HasBytes(bytes, 0, "DCX\0");
}

std::string ReadTpfName(const std::vector<std::uint8_t>& tpf, std::uint32_t offset, std::uint8_t encoding)
{
    std::string name;
    if (offset >= tpf.size()) return name;

    if (encoding == 1) {
        for (std::size_t i = offset; i + 1 < tpf.size(); i += 2) {
            const std::uint16_t c = std::uint16_t(tpf[i]) | (std::uint16_t(tpf[i + 1]) << 8);
            if (c == 0) break;
            name.push_back(c >= 32 && c <= 126 ? static_cast<char>(c) : '?');
        }
    } else {
        for (std::size_t i = offset; i < tpf.size() && tpf[i] != 0; ++i) {
            name.push_back(tpf[i] >= 32 && tpf[i] <= 126 ? static_cast<char>(tpf[i]) : '?');
        }
    }

    return name;
}

bool ExtractTpfTexture(const std::vector<std::uint8_t>& tpf, const char* target_name, std::vector<std::uint8_t>& dds)
{
    dds.clear();
    if (tpf.size() < 0x10 || !HasBytes(tpf, 0, "TPF\0")) return false;

    const std::uint32_t count = ReadLe32(tpf, 8);
    const std::uint8_t platform = tpf[0x0c];
    const std::uint8_t encoding = tpf[0x0e];
    constexpr std::size_t kPcTextureHeaderSize = 0x14;
    if (platform != 0 || count > 512 || 0x10ull + count * kPcTextureHeaderSize > tpf.size()) return false;

    for (std::uint32_t i = 0; i < count; ++i) {
        const std::size_t entry = 0x10ull + i * kPcTextureHeaderSize;
        const std::uint32_t file_offset = ReadLe32(tpf, entry);
        const std::uint32_t file_size = ReadLe32(tpf, entry + 4);
        const std::uint32_t name_offset = ReadLe32(tpf, entry + 12);
        if (file_offset > tpf.size() || file_size > tpf.size() - file_offset) continue;
        if (ReadTpfName(tpf, name_offset, encoding) != target_name) continue;

        dds.assign(tpf.begin() + file_offset, tpf.begin() + file_offset + file_size);
        return true;
    }

    return false;
}

int ReadXmlInt(const std::string& text, std::size_t line_start, const char* attr)
{
    const std::string needle = std::string(attr) + "=\"";
    const std::size_t pos = text.find(needle, line_start);
    if (pos == std::string::npos) return 0;
    return std::atoi(text.c_str() + pos + needle.size());
}

void ParseLayouts(const std::vector<std::uint8_t>& bnd)
{
    const std::string text(reinterpret_cast<const char*>(bnd.data()), bnd.size());
    for (std::size_t atlas_index = 0; atlas_index < g_atlases.size(); ++atlas_index) {
        const std::string marker = std::string("<TextureAtlas imagePath=\"") + g_atlases[atlas_index].name + ".png\">";
        const std::size_t atlas = text.find(marker);
        if (atlas == std::string::npos) continue;
        const std::size_t atlas_end = text.find("</TextureAtlas>", atlas);
        if (atlas_end == std::string::npos) continue;

        std::size_t pos = atlas;
        while ((pos = text.find("<SubTexture", pos)) != std::string::npos && pos < atlas_end) {
            const std::size_t name = text.find("MENU_ItemIcon_", pos);
            if (name == std::string::npos || name > atlas_end) break;
            const std::uint32_t id = static_cast<std::uint32_t>(std::atoi(text.c_str() + name + 14));
            Rect rect{};
            rect.x = static_cast<float>(ReadXmlInt(text, pos, "x"));
            rect.y = static_cast<float>(ReadXmlInt(text, pos, "y"));
            rect.w = static_cast<float>(ReadXmlInt(text, pos, "width"));
            rect.h = static_cast<float>(ReadXmlInt(text, pos, "height"));
            if (id != 0 && rect.w > 0.0f && rect.h > 0.0f) g_icons[id] = {atlas_index, rect};
            pos += 11;
        }
    }
}

bool UploadBc7Texture(
    ID3D12Device* device,
    ID3D12CommandQueue* queue,
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_srv,
    Atlas& atlas,
    const std::vector<std::uint8_t>& dds)
{
    if (dds.size() < 148 || !HasBytes(dds, 0, "DDS ") || !HasBytes(dds, 84, "DX10")) return false;

    const std::uint32_t height = ReadLe32(dds, 12);
    const std::uint32_t width = ReadLe32(dds, 16);
    const std::uint32_t format = ReadLe32(dds, 128);
    if (width == 0 || height == 0 || format != DXGI_FORMAT_BC7_UNORM) return false;

    const std::size_t data_offset = 148;
    if (dds.size() <= data_offset) return false;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_BC7_UNORM;
    desc.SampleDesc.Count = 1;

    D3D12_HEAP_PROPERTIES default_heap{};
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    if (FAILED(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&atlas.texture)))) {
        return false;
    }

    const std::uint64_t row_pitch = ((width + 3ull) / 4ull) * 16ull;
    const std::uint64_t upload_size = row_pitch * ((height + 3ull) / 4ull);
    if (data_offset + upload_size > dds.size()) return false;

    D3D12_HEAP_PROPERTIES upload_heap{};
    upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC upload_desc{};
    upload_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    upload_desc.Width = upload_size;
    upload_desc.Height = 1;
    upload_desc.DepthOrArraySize = 1;
    upload_desc.MipLevels = 1;
    upload_desc.SampleDesc.Count = 1;
    upload_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ID3D12Resource* upload = nullptr;
    if (FAILED(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &upload_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)))) return false;

    void* mapped = nullptr;
    upload->Map(0, nullptr, &mapped);
    std::memcpy(mapped, dds.data() + data_offset, static_cast<std::size_t>(upload_size));
    upload->Unmap(0, nullptr);

    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&list)))) {
        SafeRelease(upload);
        return false;
    }

    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = atlas.texture;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = upload;
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_BC7_UNORM;
    src.PlacedFootprint.Footprint.Width = width;
    src.PlacedFootprint.Footprint.Height = height;
    src.PlacedFootprint.Footprint.Depth = 1;
    src.PlacedFootprint.Footprint.RowPitch = static_cast<UINT>(row_pitch);
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = atlas.texture;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &barrier);
    list->Close();

    ID3D12CommandList* lists[] = {list};
    queue->ExecuteCommandLists(1, lists);

    ID3D12Fence* fence = nullptr;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    queue->Signal(fence, 1);
    if (fence->GetCompletedValue() < 1) {
        fence->SetEventOnCompletion(1, event);
        WaitForSingleObject(event, INFINITE);
    }
    CloseHandle(event);
    SafeRelease(fence);
    SafeRelease(list);
    SafeRelease(allocator);
    SafeRelease(upload);

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_BC7_UNORM;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(atlas.texture, &srv, cpu_srv);

    atlas.width = static_cast<float>(width);
    atlas.height = static_cast<float>(height);
    return true;
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
    for (const Candidate& candidate : candidates) {
        std::vector<std::uint8_t> tpf_dcx;
        std::vector<std::uint8_t> sblyt_dcx;
        if (!asset_reader::ReadFile(candidate.tpf_path, tpf_dcx, 96ull * 1024ull * 1024ull) ||
            !asset_reader::ReadFile(candidate.layout_path, sblyt_dcx, 4ull * 1024ull * 1024ull)) {
            continue;
        }

        saw_assets = true;
        if (!StartsWithDcx(tpf_dcx)) {
            if (!candidate.decrypt_tpf || !DecryptData0AesRanges(tpf_dcx)) {
                Log("Icon loader: %s TPF is encrypted or invalid.", candidate.label);
                continue;
            }
        }

        std::vector<std::uint8_t> tpf;
        std::vector<std::uint8_t> sblyt;
        if (!DecompressDcxKrak(tpf_dcx, tpf) || !DecompressDcxKrak(sblyt_dcx, sblyt)) {
            Log("Icon loader: failed to decompress %s icon assets.", candidate.label);
            continue;
        }

        g_icons.clear();
        ParseLayouts(sblyt);
        for (std::size_t i = 0; i < g_atlases.size(); ++i) {
            std::vector<std::uint8_t> dds;
            if (!ExtractTpfTexture(tpf, g_atlases[i].name, dds)) continue;
            if (UploadBc7Texture(device, queue, cpu_srvs[i], g_atlases[i], dds)) {
                g_atlases[i].gpu_srv = gpu_srvs[i];
                uploaded_any = true;
            }
        }
        break;
    }

    if (!saw_assets) return false;
    if (g_icons.empty() || !uploaded_any) {
        Log("Icon loader: failed to upload atlases (icons=%zu uploaded=%d).", g_icons.size(), static_cast<int>(uploaded_any));
        g_failed = true;
        return false;
    }

    g_initialized = true;
    return true;
}

radial_menu::IconTextureInfo Resolve(std::uint32_t icon_id)
{
    if (!g_initialized) return {};

    auto it = g_icons.find(icon_id);
    if (it == g_icons.end()) it = g_icons.find(icon_id + 2000u);
    if (it == g_icons.end()) return {};

    const IconEntry& entry = it->second;
    if (entry.atlas_index >= g_atlases.size()) return {};
    const Atlas& atlas = g_atlases[entry.atlas_index];
    if (!atlas.gpu_srv.ptr) return {};
    const Rect& r = entry.rect;
    return {(ImTextureID)atlas.gpu_srv.ptr, {r.x / atlas.width, r.y / atlas.height}, {(r.x + r.w) / atlas.width, (r.y + r.h) / atlas.height}};
}

void Shutdown()
{
    for (Atlas& atlas : g_atlases) {
        SafeRelease(atlas.texture);
        atlas.gpu_srv = {};
        atlas.width = 1.0f;
        atlas.height = 1.0f;
    }
    g_icons.clear();
    g_initialized = false;
    g_failed = false;
}

}  // namespace radial_spell_menu::icon_loader
