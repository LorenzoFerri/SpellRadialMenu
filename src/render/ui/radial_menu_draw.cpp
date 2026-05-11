#include "render/ui/radial_menu_draw.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace radial_menu_mod::radial_menu {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kBaseViewportHeight = 1080.0f;
constexpr float kWheelInnerRadius = 118.0f;
constexpr float kWheelOuterRadius = 248.0f;
constexpr float kWheelIconRadius = 182.0f;
constexpr float kCenterPanelRadius = 102.0f;
constexpr float kSegmentGapRadians = 0.028f;

struct RadialLayout {
    ImGuiViewport* viewport = nullptr;
    ImVec2 center{};
    ImVec2 bottom_right{};
    float ui_scale = 1.0f;
    float wheel_inner_radius = 0.0f;
    float wheel_outer_radius = 0.0f;
    float wheel_icon_radius = 0.0f;
    float center_panel_radius = 0.0f;
};

std::string FormatSlotLabel(const RadialSlot& slot)
{
    if (!slot.occupied) return "Empty";
    if (!slot.name.empty()) return slot.name;

    char buffer[64] = {};
    std::snprintf(buffer, sizeof(buffer), "%s %u", slot.is_item ? "Item" : "Spell", slot.id);
    return buffer;
}

const char* GetCategoryLabel(const RadialSlot& slot)
{
    if (slot.is_item) return "ITEM";

    switch (slot.category) {
    case SpellCategory::sorcery:
        return "SORCERY";
    case SpellCategory::incantation:
        return "INCANTATION";
    case SpellCategory::unknown:
    default:
        return "SPELL";
    }
}

ImU32 GetCategoryColor(const RadialSlot& slot, bool is_selected)
{
    switch (slot.category) {
    case SpellCategory::sorcery:
        return is_selected ? IM_COL32(155, 240, 255, 255) : IM_COL32(86, 184, 216, 255);
    case SpellCategory::incantation:
        return is_selected ? IM_COL32(255, 219, 170, 255) : IM_COL32(220, 186, 132, 255);
    case SpellCategory::unknown:
    default:
        return is_selected ? IM_COL32(220, 230, 235, 255) : IM_COL32(170, 182, 188, 255);
    }
}

ImVec2 PolarPoint(const ImVec2& center, float angle, float radius)
{
    return {center.x + (std::cos(angle) * radius), center.y + (std::sin(angle) * radius)};
}

void AddRingSegment(ImDrawList* draw_list, const ImVec2& center, float inner_radius, float outer_radius,
    float start_angle, float end_angle, ImU32 fill, ImU32 border, float border_thickness)
{
    const int segments = std::max(18, static_cast<int>((end_angle - start_angle) * 32.0f));
    draw_list->PathClear();
    draw_list->PathArcTo(center, outer_radius, start_angle, end_angle, segments);
    draw_list->PathArcTo(center, inner_radius, end_angle, start_angle, segments);
    draw_list->PathFillConvex(fill);

    draw_list->PathClear();
    draw_list->PathArcTo(center, outer_radius, start_angle, end_angle, segments);
    draw_list->PathArcTo(center, inner_radius, end_angle, start_angle, segments);
    draw_list->PathStroke(border, ImDrawFlags_Closed, border_thickness);
}

void AddSegmentSeparator(ImDrawList* draw_list, const ImVec2& center, float angle, float inner_radius,
    float outer_radius, ImU32 color, float thickness)
{
    draw_list->AddLine(PolarPoint(center, angle, inner_radius), PolarPoint(center, angle, outer_radius), color, thickness);
}

void AddArcStroke(ImDrawList* draw_list, const ImVec2& center, float radius, float start_angle, float end_angle,
    ImU32 color, float thickness)
{
    const int segments = std::max(12, static_cast<int>((end_angle - start_angle) * 24.0f));
    draw_list->PathClear();
    draw_list->PathArcTo(center, radius, start_angle, end_angle, segments);
    draw_list->PathStroke(color, 0, thickness);
}

void AddCenteredText(ImDrawList* draw_list, ImFont* font, float font_size, const ImVec2& center, float y,
    ImU32 color, const char* text)
{
    const ImVec2 text_size = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, text);
    draw_list->AddText(font, font_size, {center.x - (text_size.x * 0.5f), y}, color, text);
}

std::vector<std::string> WrapTextLines(ImFont* font, float font_size, const std::string& text, float wrap_width,
    std::size_t max_lines)
{
    std::vector<std::string> lines;
    if (text.empty() || max_lines == 0) return lines;

    auto measure = [&](const std::string& value) {
        return font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, value.c_str()).x;
    };
    auto fit_with_ellipsis = [&](std::string value) {
        while (!value.empty() && measure(value + "...") > wrap_width) value.pop_back();
        return value.empty() ? std::string("...") : (value + "...");
    };

    std::vector<std::string> words;
    for (std::size_t cursor = 0; cursor < text.size();) {
        while (cursor < text.size() && text[cursor] == ' ') ++cursor;
        if (cursor >= text.size()) break;
        std::size_t next_space = text.find(' ', cursor);
        if (next_space == std::string::npos) next_space = text.size();
        words.push_back(text.substr(cursor, next_space - cursor));
        cursor = next_space + 1;
    }

    for (std::size_t word_index = 0; word_index < words.size() && lines.size() < max_lines;) {
        std::string line = words[word_index];
        if (measure(line) > wrap_width) {
            lines.push_back(fit_with_ellipsis(line));
            ++word_index;
            continue;
        }

        std::size_t next_index = word_index + 1;
        while (next_index < words.size()) {
            const std::string candidate = line + " " + words[next_index];
            if (measure(candidate) > wrap_width) break;
            line = candidate;
            ++next_index;
        }

        if ((lines.size() + 1) == max_lines && next_index < words.size()) {
            lines.push_back(fit_with_ellipsis(line));
            break;
        }

        lines.push_back(line);
        word_index = next_index;
    }

    return lines;
}

RadialLayout BuildLayout()
{
    RadialLayout layout{};
    layout.viewport = ImGui::GetMainViewport();
    layout.center = {
        layout.viewport->Pos.x + (layout.viewport->Size.x * 0.5f),
        layout.viewport->Pos.y + (layout.viewport->Size.y * 0.5f),
    };
    layout.bottom_right = {
        layout.viewport->Pos.x + layout.viewport->Size.x,
        layout.viewport->Pos.y + layout.viewport->Size.y,
    };
    const float viewport_min = std::min(layout.viewport->Size.x, layout.viewport->Size.y);
    layout.ui_scale = std::clamp(viewport_min / kBaseViewportHeight, 0.9f, 1.85f);
    layout.wheel_inner_radius = kWheelInnerRadius * layout.ui_scale;
    layout.wheel_outer_radius = kWheelOuterRadius * layout.ui_scale;
    layout.wheel_icon_radius = kWheelIconRadius * layout.ui_scale;
    layout.center_panel_radius = kCenterPanelRadius * layout.ui_scale;
    return layout;
}

void BeginOverlayWindow(const RadialLayout& layout)
{
    ImGui::SetNextWindowPos(layout.viewport->Pos);
    ImGui::SetNextWindowSize(layout.viewport->Size);

    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBackground;
    ImGui::Begin("RadialMenuOverlay", nullptr, flags);
}

void DrawSlotIcon(ImDrawList* draw_list, const ImVec2& center, const RadialSlot& slot, float scale,
    IconTextureInfo(*icon_texture_resolver)(std::uint32_t icon_id))
{
    if (icon_texture_resolver == nullptr || slot.icon_id == 0) return;

    const IconTextureInfo icon = icon_texture_resolver(slot.icon_id);
    if (icon.texture == ImTextureID{}) return;

    const float half_extent = 40.0f * scale;
    const float vertical_offset = 4.0f * scale;
    const ImVec2 image_min = {center.x - half_extent, center.y - half_extent - vertical_offset};
    const ImVec2 image_max = {center.x + half_extent, center.y + half_extent - vertical_offset};
    draw_list->AddImageRounded(icon.texture, image_min, image_max, icon.uv_min, icon.uv_max, IM_COL32_WHITE, 10.0f * scale);
}

void DrawBackdrop(ImDrawList* draw_list, const RadialLayout& layout)
{
    const float overlay_shadow_radius = layout.wheel_outer_radius + (22.0f * layout.ui_scale);
    const float outer_frame_radius = layout.wheel_outer_radius + (8.0f * layout.ui_scale);

    draw_list->AddRectFilled(layout.viewport->Pos, layout.bottom_right, IM_COL32(0, 0, 0, 72));
    draw_list->AddCircleFilled(layout.center, overlay_shadow_radius, IM_COL32(0, 0, 0, 48), 128);
    draw_list->AddCircleFilled(layout.center, outer_frame_radius, IM_COL32(12, 11, 10, 210), 128);
    draw_list->AddCircle(layout.center, outer_frame_radius, IM_COL32(110, 94, 64, 180), 128, 2.0f * layout.ui_scale);
    draw_list->AddCircle(layout.center, layout.wheel_outer_radius - (4.0f * layout.ui_scale), IM_COL32(171, 148, 102, 170), 128, 1.5f * layout.ui_scale);
}

void DrawWheel(ImDrawList* draw_list, const RadialLayout& layout, const std::vector<RadialSlot>& slots,
    int selected_slot, IconTextureInfo(*icon_texture_resolver)(std::uint32_t icon_id))
{
    const std::size_t slot_count = std::max<std::size_t>(slots.size(), 1);
    const float step = (2.0f * kPi) / static_cast<float>(slot_count);

    for (std::size_t i = 0; i < slots.size(); ++i) {
        const bool is_selected = static_cast<int>(i) == selected_slot;
        const float start_angle = (-kPi * 0.5f) + (step * static_cast<float>(i)) + kSegmentGapRadians;
        const float end_angle = (-kPi * 0.5f) + (step * static_cast<float>(i + 1)) - kSegmentGapRadians;
        const float mid_angle = (start_angle + end_angle) * 0.5f;
        const ImVec2 icon_center = PolarPoint(layout.center, mid_angle, layout.wheel_icon_radius);

        const ImU32 fill = is_selected ? IM_COL32(40, 36, 30, 232) : IM_COL32(24, 22, 19, 224);
        const ImU32 border = is_selected ? GetCategoryColor(slots[i], true) : IM_COL32(110, 95, 65, 170);
        const float border_thickness = (is_selected ? 3.5f : 1.25f) * layout.ui_scale;
        AddRingSegment(draw_list, layout.center, layout.wheel_inner_radius, layout.wheel_outer_radius, start_angle, end_angle, fill, border, border_thickness);

        const ImU32 inner_trim = is_selected ? GetCategoryColor(slots[i], false) : IM_COL32(146, 124, 84, 120);
        const ImU32 outer_trim = is_selected ? GetCategoryColor(slots[i], false) : IM_COL32(120, 102, 72, 110);
        AddArcStroke(draw_list, layout.center, layout.wheel_inner_radius + (8.0f * layout.ui_scale), start_angle + 0.03f, end_angle - 0.03f, inner_trim, 1.0f * layout.ui_scale);
        AddArcStroke(draw_list, layout.center, layout.wheel_outer_radius - (16.0f * layout.ui_scale), start_angle + 0.05f, end_angle - 0.05f, outer_trim, 1.0f * layout.ui_scale);

        DrawSlotIcon(draw_list, icon_center, slots[i], layout.ui_scale, icon_texture_resolver);
    }

    for (std::size_t i = 0; i < slot_count; ++i) {
        const float separator_angle = (-kPi * 0.5f) + (step * static_cast<float>(i));
        AddSegmentSeparator(draw_list, layout.center, separator_angle, layout.wheel_inner_radius + (2.0f * layout.ui_scale), layout.wheel_outer_radius - (10.0f * layout.ui_scale), IM_COL32(97, 82, 56, 160), 1.0f * layout.ui_scale);
    }
}

void DrawCenterPanel(ImDrawList* draw_list, const RadialLayout& layout)
{
    draw_list->AddCircleFilled(layout.center, layout.center_panel_radius + (14.0f * layout.ui_scale), IM_COL32(8, 8, 8, 190), 96);
    draw_list->AddCircleFilled(layout.center, layout.center_panel_radius, IM_COL32(18, 17, 15, 224), 96);
    draw_list->AddCircle(layout.center, layout.center_panel_radius + (14.0f * layout.ui_scale), IM_COL32(136, 115, 78, 180), 96, 1.5f * layout.ui_scale);
    draw_list->AddCircle(layout.center, layout.center_panel_radius, IM_COL32(176, 151, 106, 190), 96, 1.5f * layout.ui_scale);
    draw_list->AddCircle(layout.center, layout.center_panel_radius - (12.0f * layout.ui_scale), IM_COL32(94, 79, 54, 120), 96, 1.0f * layout.ui_scale);
}

void DrawSelectedDetails(ImDrawList* draw_list, ImFont* font, float base_font_size, const RadialLayout& layout,
    const std::vector<RadialSlot>& slots, const char* title, int selected_slot)
{
    AddCenteredText(draw_list, font, base_font_size * 0.94f * layout.ui_scale, layout.center, layout.center.y - (48.0f * layout.ui_scale), IM_COL32(219, 206, 174, 220), title);
    if (selected_slot < 0 || selected_slot >= static_cast<int>(slots.size())) return;

    const RadialSlot& slot = slots[static_cast<std::size_t>(selected_slot)];
    AddCenteredText(draw_list, font, base_font_size * 0.84f * layout.ui_scale, layout.center, layout.center.y - (28.0f * layout.ui_scale), GetCategoryColor(slot, true), GetCategoryLabel(slot));

    const float font_size = base_font_size * 0.96f * layout.ui_scale;
    const std::vector<std::string> label_lines = WrapTextLines(font, font_size, FormatSlotLabel(slot), layout.center_panel_radius * 1.52f, 2);
    const float line_height = font_size + (2.0f * layout.ui_scale);
    float line_y = layout.center.y + (14.0f * layout.ui_scale);
    if (label_lines.size() > 1) line_y -= (line_height * 0.35f);
    for (const std::string& line : label_lines) {
        AddCenteredText(draw_list, font, font_size, layout.center, line_y, IM_COL32(244, 238, 223, 255), line.c_str());
        line_y += line_height;
    }
}

}  // namespace

void DrawMenuContents(const std::vector<RadialSlot>& slots, const char* title, const char* controls,
    int selected_slot, IconTextureInfo(*icon_texture_resolver)(std::uint32_t icon_id))
{
    const RadialLayout layout = BuildLayout();
    BeginOverlayWindow(layout);

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImFont* font = ImGui::GetFont();
    const float base_font_size = ImGui::GetFontSize();

    DrawBackdrop(draw_list, layout);
    DrawWheel(draw_list, layout, slots, selected_slot, icon_texture_resolver);
    DrawCenterPanel(draw_list, layout);
    DrawSelectedDetails(draw_list, font, base_font_size, layout, slots, title, selected_slot);
    AddCenteredText(draw_list, font, base_font_size * 0.84f * layout.ui_scale, layout.center,
        layout.center.y + layout.wheel_outer_radius + (28.0f * layout.ui_scale), IM_COL32(220, 213, 197, 220), controls);

    ImGui::End();
}

}  // namespace radial_menu_mod::radial_menu
