#pragma once

#include "render/ui/radial_menu.h"

#include <d3d12.h>

#include <cstdint>
#include <vector>

namespace radial_menu_mod::icon_loader {

constexpr std::size_t kMaxAtlases = 32;

bool TryInitialize(
    ID3D12Device* device,
    ID3D12CommandQueue* queue,
    const D3D12_CPU_DESCRIPTOR_HANDLE* cpu_srvs,
    const D3D12_GPU_DESCRIPTOR_HANDLE* gpu_srvs,
    std::size_t srv_count);
void SetRequiredIcons(const std::vector<std::uint32_t>& icon_ids);
bool PreloadIcons(const std::vector<std::uint32_t>& icon_ids, std::size_t max_uploads);
radial_menu::IconTextureInfo Resolve(std::uint32_t icon_id);
void Shutdown();

}  // namespace radial_menu_mod::icon_loader
