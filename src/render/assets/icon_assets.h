#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace radial_menu_mod::icon_assets {

struct Rect {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
};

struct LayoutIcon {
    std::uint32_t id = 0;
    std::string atlas_name;
    Rect rect{};
};

bool DecryptData0AesRanges(std::vector<std::uint8_t>& bytes);
bool ExtractTpfTexture(const std::vector<std::uint8_t>& tpf, const std::string& target_name, std::vector<std::uint8_t>& dds);
std::vector<LayoutIcon> ParseLayoutIcons(const std::vector<std::uint8_t>& bnd);

}  // namespace radial_menu_mod::icon_assets
