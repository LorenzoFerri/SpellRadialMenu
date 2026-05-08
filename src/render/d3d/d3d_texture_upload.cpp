#include "render/d3d/d3d_texture_upload.h"

#include "core/common.h"

#include <windows.h>

#include <cstring>

namespace radial_menu_mod::d3d_texture_upload {
namespace {

std::uint32_t ReadLe32(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    return std::uint32_t(bytes[offset]) |
           (std::uint32_t(bytes[offset + 1]) << 8) |
           (std::uint32_t(bytes[offset + 2]) << 16) |
           (std::uint32_t(bytes[offset + 3]) << 24);
}

bool HasBytes(const std::vector<std::uint8_t>& bytes, std::size_t offset, const char* text)
{
    for (std::size_t i = 0; text[i] != '\0'; ++i) {
        if (offset + i >= bytes.size() || bytes[offset + i] != static_cast<std::uint8_t>(text[i])) return false;
    }
    return true;
}

void ReleaseUploadResources(
    ID3D12Resource*& upload,
    ID3D12Fence*& fence,
    ID3D12GraphicsCommandList*& list,
    ID3D12CommandAllocator*& allocator)
{
    SafeRelease(fence);
    SafeRelease(list);
    SafeRelease(allocator);
    SafeRelease(upload);
}

}  // namespace

bool UploadBc7Texture(
    ID3D12Device* device,
    ID3D12CommandQueue* queue,
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_srv,
    const std::vector<std::uint8_t>& dds,
    UploadedTexture& texture)
{
    texture = {};
    if (dds.size() < 148 || !HasBytes(dds, 0, "DDS ") || !HasBytes(dds, 84, "DX10")) return false;

    const std::uint32_t height = ReadLe32(dds, 12);
    const std::uint32_t width = ReadLe32(dds, 16);
    const std::uint32_t format = ReadLe32(dds, 128);
    if (width == 0 || height == 0 || format != DXGI_FORMAT_BC7_UNORM) return false;

    constexpr std::size_t kDataOffset = 148;
    if (dds.size() <= kDataOffset) return false;

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
    if (FAILED(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture.resource)))) {
        return false;
    }

    const std::uint64_t row_pitch = ((width + 3ull) / 4ull) * 16ull;
    const std::uint64_t upload_size = row_pitch * ((height + 3ull) / 4ull);
    if (kDataOffset + upload_size > dds.size()) {
        SafeRelease(texture.resource);
        return false;
    }

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
    if (FAILED(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &upload_desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)))) {
        SafeRelease(texture.resource);
        return false;
    }

    void* mapped = nullptr;
    upload->Map(0, nullptr, &mapped);
    std::memcpy(mapped, dds.data() + kDataOffset, static_cast<std::size_t>(upload_size));
    upload->Unmap(0, nullptr);

    ID3D12Fence* fence = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&list)))) {
        ReleaseUploadResources(upload, fence, list, allocator);
        SafeRelease(texture.resource);
        return false;
    }

    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = texture.resource;
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
    barrier.Transition.pResource = texture.resource;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &barrier);
    list->Close();

    ID3D12CommandList* lists[] = {list};
    queue->ExecuteCommandLists(1, lists);

    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    queue->Signal(fence, 1);
    if (fence->GetCompletedValue() < 1) {
        fence->SetEventOnCompletion(1, event);
        const DWORD wait = WaitForSingleObject(event, 2000);
        if (wait != WAIT_OBJECT_0) {
            Log("Icon loader: timed out waiting for texture upload fence.");
            CloseHandle(event);
            ReleaseUploadResources(upload, fence, list, allocator);
            SafeRelease(texture.resource);
            return false;
        }
    }
    CloseHandle(event);
    ReleaseUploadResources(upload, fence, list, allocator);

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_BC7_UNORM;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(texture.resource, &srv, cpu_srv);

    texture.width = static_cast<float>(width);
    texture.height = static_cast<float>(height);
    return true;
}

}  // namespace radial_menu_mod::d3d_texture_upload
