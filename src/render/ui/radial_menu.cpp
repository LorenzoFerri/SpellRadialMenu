#include "render/ui/radial_menu.h"

#include "render/ui/radial_menu_draw.h"

#include <cmath>

namespace radial_menu_mod::radial_menu {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kSelectionDirectionDeadzone = 0.42f;

bool g_is_open = false;
int g_selected_slot = -1;
IconTextureInfo(*g_icon_texture_resolver)(std::uint32_t icon_id) = nullptr;
std::vector<std::uint32_t> g_cached_icon_ids;
std::vector<IconTextureInfo> g_cached_icon_textures;

void ClearIconCache()
{
    g_cached_icon_ids.clear();
    g_cached_icon_textures.clear();
}

const std::vector<IconTextureInfo>& ResolveIconTextures(const std::vector<RadialSlot>& slots)
{
    if (g_icon_texture_resolver == nullptr) {
        ClearIconCache();
        return g_cached_icon_textures;
    }

    bool cache_matches_slots = g_cached_icon_ids.size() == slots.size() &&
        g_cached_icon_textures.size() == slots.size();
    for (std::size_t i = 0; cache_matches_slots && i < slots.size(); ++i) {
        cache_matches_slots = g_cached_icon_ids[i] == slots[i].icon_id;
    }

    if (!cache_matches_slots) {
        g_cached_icon_ids.clear();
        g_cached_icon_textures.clear();
        g_cached_icon_ids.reserve(slots.size());
        g_cached_icon_textures.reserve(slots.size());
        for (const RadialSlot& slot : slots) {
            g_cached_icon_ids.push_back(slot.icon_id);
            g_cached_icon_textures.push_back(slot.icon_id ? g_icon_texture_resolver(slot.icon_id) : IconTextureInfo{});
        }
        return g_cached_icon_textures;
    }

    for (std::size_t i = 0; i < slots.size(); ++i) {
        if (slots[i].icon_id != 0 && g_cached_icon_textures[i].texture == ImTextureID{}) {
            g_cached_icon_textures[i] = g_icon_texture_resolver(slots[i].icon_id);
        }
    }
    return g_cached_icon_textures;
}

}  // namespace

void SetIconTextureResolver(IconTextureInfo(*resolver)(std::uint32_t icon_id))
{
    g_icon_texture_resolver = resolver;
}

void Open(int initial_selection)
{
    g_is_open = true;
    g_selected_slot = initial_selection >= 0 ? initial_selection : 0;
    InvalidateDrawCache();
}

void Close()
{
    g_is_open = false;
    g_selected_slot = -1;
    InvalidateDrawCache();
}

void InvalidateDrawCache()
{
    ClearIconCache();
    InvalidateMenuDrawCache();
}

bool IsOpen()
{
    return g_is_open;
}

int GetSelectedSlot()
{
    return g_selected_slot;
}

void UpdateSelectionFromDirection(float selection_x, float selection_y, std::size_t slot_count)
{
    if (!g_is_open || slot_count == 0) return;

    const float magnitude = std::sqrt((selection_x * selection_x) + (selection_y * selection_y));
    if (magnitude < kSelectionDirectionDeadzone) return;

    float angle = std::atan2(-selection_y, selection_x) + (kPi * 0.5f);
    if (angle < 0.0f) angle += 2.0f * kPi;

    const float segment_size = (2.0f * kPi) / static_cast<float>(slot_count);
    g_selected_slot = static_cast<int>(std::floor(angle / segment_size)) % static_cast<int>(slot_count);
}

void Draw(const std::vector<RadialSlot>& slots, const char* title, const char* controls)
{
    if (!g_is_open) return;
    DrawMenuContents(slots, title, controls, g_selected_slot, ResolveIconTextures(slots));
}

}  // namespace radial_menu_mod::radial_menu
