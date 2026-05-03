#include "radial_menu.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace radial_spell_menu::radial_menu {

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kStickSelectionDeadzone = 0.42f;
constexpr float kBaseViewportHeight = 1080.0f;
constexpr float kWheelInnerRadius = 118.0f;
constexpr float kWheelOuterRadius = 248.0f;
constexpr float kWheelIconRadius = 182.0f;
constexpr float kCenterPanelRadius = 102.0f;
constexpr float kSegmentGapRadians = 0.028f;
constexpr float kSelectedInset = 6.0f;

bool g_is_open = false;
int g_selected_slot = -1;
IconTextureInfo(*g_icon_texture_resolver)(std::uint32_t icon_id) = nullptr;

std::string FormatSlotLabel(const SpellSlot& slot)
{
    if (!slot.occupied) {
        return "Empty";
    }

    if (!slot.name.empty()) {
        return slot.name;
    }

    char buffer[64] = {};
    std::snprintf(buffer, sizeof(buffer), "Spell %u", slot.spell_id);
    return buffer;
}

const char* GetCategoryLabel(const SpellSlot& slot)
{
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

ImVec2 PolarPoint(const ImVec2& center, float angle, float radius)
{
    return {
        center.x + (std::cos(angle) * radius),
        center.y + (std::sin(angle) * radius),
    };
}

void AddRingSegment(
    ImDrawList* draw_list,
    const ImVec2& center,
    float inner_radius,
    float outer_radius,
    float start_angle,
    float end_angle,
    ImU32 fill,
    ImU32 border,
    float border_thickness)
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

void AddSegmentSeparator(
    ImDrawList* draw_list,
    const ImVec2& center,
    float angle,
    float inner_radius,
    float outer_radius,
    ImU32 color,
    float thickness)
{
    const ImVec2 p0 = PolarPoint(center, angle, inner_radius);
    const ImVec2 p1 = PolarPoint(center, angle, outer_radius);
    draw_list->AddLine(p0, p1, color, thickness);
}

void AddArcStroke(
    ImDrawList* draw_list,
    const ImVec2& center,
    float radius,
    float start_angle,
    float end_angle,
    ImU32 color,
    float thickness)
{
    const int segments = std::max(12, static_cast<int>((end_angle - start_angle) * 24.0f));
    draw_list->PathClear();
    draw_list->PathArcTo(center, radius, start_angle, end_angle, segments);
    draw_list->PathStroke(color, 0, thickness);
}

void AddCenteredText(
    ImDrawList* draw_list,
    ImFont* font,
    float font_size,
    const ImVec2& center,
    float y,
    ImU32 color,
    const char* text)
{
    const ImVec2 text_size = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, text);
    draw_list->AddText(font, font_size, {center.x - (text_size.x * 0.5f), y}, color, text);
}

void AddCenteredText(
    ImDrawList* draw_list,
    ImFont* font,
    float font_size,
    const ImVec2& center,
    float y,
    ImU32 color,
    const std::string& text)
{
    AddCenteredText(draw_list, font, font_size, center, y, color, text.c_str());
}

std::vector<std::string> WrapTextLines(ImFont* font, float font_size, const std::string& text, float wrap_width, std::size_t max_lines)
{
    std::vector<std::string> lines;
    if (text.empty() || max_lines == 0) {
        return lines;
    }

    auto measure = [&](const std::string& value) {
        return font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, value.c_str()).x;
    };

    auto fit_with_ellipsis = [&](std::string value) {
        while (!value.empty() && measure(value + "...") > wrap_width) {
            value.pop_back();
        }
        return value.empty() ? std::string("...") : (value + "...");
    };

    std::vector<std::string> words;
    std::size_t cursor = 0;
    while (cursor < text.size()) {
        while (cursor < text.size() && text[cursor] == ' ') {
            ++cursor;
        }
        if (cursor >= text.size()) {
            break;
        }

        std::size_t next_space = text.find(' ', cursor);
        if (next_space == std::string::npos) {
            next_space = text.size();
        }
        words.push_back(text.substr(cursor, next_space - cursor));
        cursor = next_space + 1;
    }

    std::size_t word_index = 0;
    while (word_index < words.size() && lines.size() < max_lines) {
        std::string line = words[word_index];
        if (measure(line) > wrap_width) {
            const bool last_allowed_line = (lines.size() + 1) == max_lines;
            lines.push_back(last_allowed_line ? fit_with_ellipsis(line) : fit_with_ellipsis(line));
            ++word_index;
            continue;
        }

        std::size_t next_index = word_index + 1;
        while (next_index < words.size()) {
            const std::string candidate = line + " " + words[next_index];
            if (measure(candidate) > wrap_width) {
                break;
            }
            line = candidate;
            ++next_index;
        }

        const bool last_allowed_line = (lines.size() + 1) == max_lines;
        if (last_allowed_line && next_index < words.size()) {
            line = fit_with_ellipsis(line);
            lines.push_back(line);
            break;
        }

        lines.push_back(line);
        word_index = next_index;
    }

    return lines;
}

ImU32 GetCategoryColor(const SpellSlot& slot, bool is_selected)
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

void DrawSpellBadge(ImDrawList* draw_list, ImFont* font, float base_font_size, const ImVec2& center, const SpellSlot& slot, bool is_selected, float scale)
{
    if (g_icon_texture_resolver != nullptr && slot.icon_id != 0) {
        const IconTextureInfo icon = g_icon_texture_resolver(slot.icon_id);
        if (icon.texture != ImTextureID{}) {
            const float half_extent = (is_selected ? 44.0f : 40.0f) * scale;
            const float vertical_offset = 4.0f * scale;
            const ImVec2 image_min = {center.x - half_extent, center.y - half_extent - vertical_offset};
            const ImVec2 image_max = {center.x + half_extent, center.y + half_extent - vertical_offset};
            draw_list->AddImageRounded(icon.texture, image_min, image_max, icon.uv_min, icon.uv_max, IM_COL32_WHITE, 10.0f * scale);
            return;
        }
    }

    // No game icon yet: tiny category dot only (no frame / crosshair-style placeholder art).
    const ImU32 rgb = GetCategoryColor(slot, is_selected);
    const int a = is_selected ? 200 : 120;
    const ImU32 dot_color = (rgb & 0x00FFFFFFu) | (static_cast<ImU32>(a) << 24);
    const ImVec2 dot_center = {center.x, center.y - (2.0f * scale)};
    const float dot_radius = (is_selected ? 5.5f : 4.5f) * scale;
    draw_list->AddCircleFilled(dot_center, dot_radius, dot_color, 20);

    (void)font;
    (void)base_font_size;
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
    if (!g_is_open || slot_count == 0) {
        return;
    }

    const float magnitude = std::sqrt((stick_x * stick_x) + (stick_y * stick_y));
    if (magnitude < kStickSelectionDeadzone) {
        return;
    }

    float angle = std::atan2(-stick_y, stick_x) + (kPi * 0.5f);
    if (angle < 0.0f) {
        angle += 2.0f * kPi;
    }

    const float segment_size = (2.0f * kPi) / static_cast<float>(slot_count);
    g_selected_slot = static_cast<int>(std::floor(angle / segment_size)) % static_cast<int>(slot_count);
}

void Draw(const std::vector<SpellSlot>& slots)
{
    if (!g_is_open) {
        return;
    }

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);

    constexpr ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_NoBackground;

    ImGui::Begin("RadialSpellMenuOverlay", nullptr, flags);

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImFont* font = ImGui::GetFont();
    const float base_font_size = ImGui::GetFontSize();
    const ImVec2 center = {
        viewport->Pos.x + (viewport->Size.x * 0.5f),
        viewport->Pos.y + (viewport->Size.y * 0.5f),
    };
    const ImVec2 bottom_right = {
        viewport->Pos.x + viewport->Size.x,
        viewport->Pos.y + viewport->Size.y,
    };
    const float viewport_min = std::min(viewport->Size.x, viewport->Size.y);
    const float ui_scale = std::clamp(viewport_min / kBaseViewportHeight, 0.9f, 1.85f);
    const float wheel_inner_radius = kWheelInnerRadius * ui_scale;
    const float wheel_outer_radius = kWheelOuterRadius * ui_scale;
    const float wheel_icon_radius = kWheelIconRadius * ui_scale;
    const float center_panel_radius = kCenterPanelRadius * ui_scale;
    const float selected_inset = kSelectedInset * ui_scale;
    const float overlay_shadow_radius = wheel_outer_radius + (22.0f * ui_scale);
    const float outer_frame_radius = wheel_outer_radius + (8.0f * ui_scale);

    draw_list->AddRectFilled(viewport->Pos, bottom_right, IM_COL32(0, 0, 0, 72));
    draw_list->AddCircleFilled(center, overlay_shadow_radius, IM_COL32(0, 0, 0, 48), 128);

    const std::size_t slot_count = std::max<std::size_t>(slots.size(), 1);
    const float step = (2.0f * kPi) / static_cast<float>(slot_count);

    draw_list->AddCircleFilled(center, outer_frame_radius, IM_COL32(12, 11, 10, 210), 128);
    draw_list->AddCircle(center, outer_frame_radius, IM_COL32(110, 94, 64, 180), 128, 2.0f * ui_scale);
    draw_list->AddCircle(center, wheel_outer_radius - (4.0f * ui_scale), IM_COL32(171, 148, 102, 170), 128, 1.5f * ui_scale);

    for (std::size_t i = 0; i < slots.size(); ++i) {
        const bool is_selected = static_cast<int>(i) == g_selected_slot;
        const float start_angle = (-kPi * 0.5f) + (step * static_cast<float>(i)) + kSegmentGapRadians;
        const float end_angle = (-kPi * 0.5f) + (step * static_cast<float>(i + 1)) - kSegmentGapRadians;
        const float inner_radius = is_selected ? (wheel_inner_radius - (2.0f * ui_scale)) : wheel_inner_radius;
        const float outer_radius = is_selected ? (wheel_outer_radius - selected_inset) : wheel_outer_radius;
        const float mid_angle = (start_angle + end_angle) * 0.5f;
        const ImVec2 icon_center = PolarPoint(center, mid_angle, wheel_icon_radius - (is_selected ? (4.0f * ui_scale) : 0.0f));

        const ImU32 fill = is_selected ? IM_COL32(150, 240, 255, 84) : IM_COL32(24, 22, 19, 224);
        const ImU32 border = is_selected ? IM_COL32(196, 237, 242, 230) : IM_COL32(110, 95, 65, 170);
        AddRingSegment(draw_list, center, inner_radius, outer_radius, start_angle, end_angle, fill, border, (is_selected ? 2.5f : 1.25f) * ui_scale);

        const ImU32 inner_trim = is_selected ? IM_COL32(205, 244, 248, 180) : IM_COL32(146, 124, 84, 120);
        const ImU32 outer_trim = is_selected ? IM_COL32(180, 225, 230, 130) : IM_COL32(120, 102, 72, 110);
        AddArcStroke(draw_list, center, wheel_inner_radius + (8.0f * ui_scale), start_angle + 0.03f, end_angle - 0.03f, inner_trim, 1.0f * ui_scale);
        AddArcStroke(draw_list, center, wheel_outer_radius - (16.0f * ui_scale), start_angle + 0.05f, end_angle - 0.05f, outer_trim, 1.0f * ui_scale);

        DrawSpellBadge(draw_list, font, base_font_size, icon_center, slots[i], is_selected, ui_scale);

        if (slots[i].is_current) {
            const ImVec2 pip_center = PolarPoint(center, mid_angle, wheel_inner_radius + (18.0f * ui_scale));
            draw_list->AddCircleFilled(pip_center, 5.0f * ui_scale, IM_COL32(122, 214, 255, 230), 16);
            draw_list->AddCircle(pip_center, 8.0f * ui_scale, IM_COL32(122, 214, 255, 120), 16, 1.0f * ui_scale);
        }
    }

    for (std::size_t i = 0; i < slot_count; ++i) {
        const float separator_angle = (-kPi * 0.5f) + (step * static_cast<float>(i));
        AddSegmentSeparator(draw_list, center, separator_angle, wheel_inner_radius + (2.0f * ui_scale), wheel_outer_radius - (10.0f * ui_scale), IM_COL32(97, 82, 56, 160), 1.0f * ui_scale);
    }

    draw_list->AddCircleFilled(center, center_panel_radius + (14.0f * ui_scale), IM_COL32(8, 8, 8, 190), 96);
    draw_list->AddCircleFilled(center, center_panel_radius, IM_COL32(18, 17, 15, 224), 96);
    draw_list->AddCircle(center, center_panel_radius + (14.0f * ui_scale), IM_COL32(136, 115, 78, 180), 96, 1.5f * ui_scale);
    draw_list->AddCircle(center, center_panel_radius, IM_COL32(176, 151, 106, 190), 96, 1.5f * ui_scale);
    draw_list->AddCircle(center, center_panel_radius - (12.0f * ui_scale), IM_COL32(94, 79, 54, 120), 96, 1.0f * ui_scale);

    const char* title = "Quick Spell";
    AddCenteredText(draw_list, font, base_font_size * 0.94f * ui_scale, center, center.y - (48.0f * ui_scale), IM_COL32(219, 206, 174, 220), title);

    if (g_selected_slot >= 0 && g_selected_slot < static_cast<int>(slots.size())) {
        const SpellSlot& selected_slot = slots[static_cast<std::size_t>(g_selected_slot)];
        const char* category = GetCategoryLabel(selected_slot);
        AddCenteredText(draw_list, font, base_font_size * 0.84f * ui_scale, center, center.y - (28.0f * ui_scale), GetCategoryColor(selected_slot, true), category);

        const std::string selected_label = FormatSlotLabel(selected_slot);
        const float selected_label_font_size = base_font_size * 0.96f * ui_scale;
        const float selected_label_wrap_width = center_panel_radius * 1.52f;
        const std::vector<std::string> label_lines = WrapTextLines(font, selected_label_font_size, selected_label, selected_label_wrap_width, 2);
        const float line_height = selected_label_font_size + (2.0f * ui_scale);
        float line_y = center.y + (14.0f * ui_scale);
        if (label_lines.size() > 1) {
            line_y -= (line_height * 0.35f);
        }
        for (const std::string& line : label_lines) {
            AddCenteredText(draw_list, font, selected_label_font_size, center, line_y, IM_COL32(244, 238, 223, 255), line);
            line_y += line_height;
        }
    }

    const char* controls = "Right Stick Rotate   Release D-pad Up Confirm";
    AddCenteredText(draw_list, font, base_font_size * 0.84f * ui_scale, center, center.y + wheel_outer_radius + (28.0f * ui_scale), IM_COL32(220, 213, 197, 220), controls);

    ImGui::End();
}

}  // namespace radial_spell_menu::radial_menu
