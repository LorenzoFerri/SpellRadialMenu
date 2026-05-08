#pragma once

#include <d3d12.h>

#include <cstdint>
#include <vector>

namespace radial_spell_menu::d3d_texture_upload {

struct UploadedTexture {
    ID3D12Resource* resource = nullptr;
    float width = 1.0f;
    float height = 1.0f;
};

bool UploadBc7Texture(
    ID3D12Device* device,
    ID3D12CommandQueue* queue,
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_srv,
    const std::vector<std::uint8_t>& dds,
    UploadedTexture& texture);

}  // namespace radial_spell_menu::d3d_texture_upload
