#include "render/ui/radial_menu.h"

#include "render/ui/radial_menu_draw.h"

#include <cmath>

namespace radial_spell_menu::radial_menu {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kStickSelectionDeadzone = 0.42f;

bool g_is_open = false;
int g_selected_slot = -1;
IconTextureInfo(*g_icon_texture_resolver)(std::uint32_t icon_id) = nullptr;

}  // namespace

void SetIconTextureResolver(IconTextureInfo(*resolver)(std::uint32_t icon_id))
{
    g_icon_texture_resolver = resolver;
}

void Open(int initial_selection)
{
    g_is_open = true;
    g_selected_slot = initial_selection >= 0 ? initial_selection : 0;
}

void Close()
{
    g_is_open = false;
    g_selected_slot = -1;
}

bool IsOpen()
{
    return g_is_open;
}

int GetSelectedSlot()
{
    return g_selected_slot;
}

void UpdateSelectionFromStick(float stick_x, float stick_y, std::size_t slot_count)
{
    if (!g_is_open || slot_count == 0) return;

    const float magnitude = std::sqrt((stick_x * stick_x) + (stick_y * stick_y));
    if (magnitude < kStickSelectionDeadzone) return;

    float angle = std::atan2(-stick_y, stick_x) + (kPi * 0.5f);
    if (angle < 0.0f) angle += 2.0f * kPi;

    const float segment_size = (2.0f * kPi) / static_cast<float>(slot_count);
    g_selected_slot = static_cast<int>(std::floor(angle / segment_size)) % static_cast<int>(slot_count);
}

void Draw(const std::vector<SpellSlot>& slots, const char* title, const char* controls)
{
    if (!g_is_open) return;
    DrawMenuContents(slots, title, controls, g_selected_slot, g_icon_texture_resolver);
}

}  // namespace radial_spell_menu::radial_menu
