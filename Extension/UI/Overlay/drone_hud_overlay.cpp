#include "overlay_internal.h"
#include "Extension/Skater/drone_font.h"
#include "Extension/Skater/drone_osd.h"

// The FPV drone's goggle-style display and stick overlay. What to show comes from drone_osd.h;
// this draws it over the game (pixel font, drawn rectangle by rectangle, so no font file is needed).

namespace dingosdk::overlay::detail {
namespace {
dingosdk::drone::osd::Layout& hud() {
    static dingosdk::drone::osd::Layout value;
    return value;
}

ImU32 colour_of(const dingosdk::drone::osd::Layout& layout, bool warn, int alpha = 255) {
    if (warn) return IM_COL32(255, 60, 60, alpha);
    const auto rgb = dingosdk::drone::osd::preset_colour(layout.colour);
    return IM_COL32(rgb[0], rgb[1], rgb[2], alpha);
}

// Width of a pixel-font string in screen pixels, `px` being the size of one font pixel.
float pixel_width(const std::string& text, float px) {
    if (text.empty()) return 0.0f;
    return (static_cast<float>(text.size()) * (dingosdk::drone::font::columns + 1) - 1) * px;
}

void pixel_text(ImDrawList* draw, ImVec2 at, const std::string& text, float px, float tall, ImU32 colour, ImU32 shadow) {
    const float outline = std::max(1.0f, px * 0.5f);
    for (int pass = 0; pass < 2; ++pass) {
        const ImU32 c = pass == 0 ? shadow : colour;
        const float grow = pass == 0 ? outline : 0.0f;
        float x = at.x;
        for (const char ch : text) {
            const auto rows = dingosdk::drone::font::glyph(ch);
            for (int r = 0; r < dingosdk::drone::font::rows; ++r)
                for (int col = 0; col < dingosdk::drone::font::columns; ++col)
                    if (rows[static_cast<std::size_t>(r)] & (1 << (dingosdk::drone::font::columns - 1 - col))) {
                        const float x0 = x + static_cast<float>(col) * px, y0 = at.y + static_cast<float>(r) * px * tall;
                        draw->AddRectFilled(ImVec2(x0 - grow, y0 - grow), ImVec2(x0 + px + grow, y0 + px * tall + grow), c);
                    }
            x += (dingosdk::drone::font::columns + 1) * px;
        }
    }
}

void draw_stick(ImDrawList* draw, ImVec2 centre, float half, float x, float y, ImU32 colour, ImU32 shadow) {
    const float t = std::max(1.5f, half * 0.04f);
    const ImVec2 lo(centre.x - half, centre.y - half), hi(centre.x + half, centre.y + half);
    draw->AddRectFilled(lo, hi, IM_COL32(0, 0, 0, 70));
    draw->AddRect(lo, hi, shadow, 0.0f, 0, t * 2.4f);
    draw->AddRect(lo, hi, colour, 0.0f, 0, t);
    draw->AddLine(ImVec2(centre.x - half, centre.y), ImVec2(centre.x + half, centre.y), IM_COL32(255, 255, 255, 60), t * 0.6f);
    draw->AddLine(ImVec2(centre.x, centre.y - half), ImVec2(centre.x, centre.y + half), IM_COL32(255, 255, 255, 60), t * 0.6f);
    const float limit = half * 0.86f;
    const ImVec2 dot(centre.x + std::clamp(x, -1.0f, 1.0f) * limit, centre.y - std::clamp(y, -1.0f, 1.0f) * limit);
    const float radius = half * 0.12f;
    draw->AddCircleFilled(dot, radius + t, shadow);
    draw->AddCircleFilled(dot, radius, colour);
}
} // namespace

bool drone_hud_pending() {
    hud() = dingosdk::drone::osd::build();
    return hud().active;
}

void draw_drone_hud() {
    const auto& layout = hud();
    if (!layout.active) return;
    const auto display = ImGui::GetIO().DisplaySize;
    if (display.x <= 0 || display.y <= 0) return;
    auto* draw = ImGui::GetBackgroundDrawList();
    const float unit = display.y / 1080.0f;
    const float px = std::max(1.0f, layout.scale * 2.0f * unit);   // one font pixel
    constexpr float tall = 1.45f;                                   // goggle glyphs are taller than wide
    const float text_height = dingosdk::drone::font::rows * px * tall;
    const ImU32 shadow = IM_COL32(0, 0, 0, 220);
    const ImU32 colour_normal = colour_of(layout, false);
    auto* smooth_font = ImGui::GetFont();

    for (const auto& item : layout.items) {
        const ImU32 colour = colour_of(layout, item.warn);
        const float height = text_height;
        float width;
        if (layout.font == 0) width = pixel_width(item.text, px);
        else width = smooth_font->CalcTextSizeA(height, FLT_MAX, 0.0f, item.text.c_str()).x;
        ImVec2 at(item.x * display.x, item.y * display.y);
        if (item.align == dingosdk::drone::osd::Align::centre) at.x -= width * 0.5f;
        else if (item.align == dingosdk::drone::osd::Align::right) at.x -= width;
        if (item.bottom) at.y -= height;
        if (layout.font == 0) {
            pixel_text(draw, at, item.text, px, tall, colour, shadow);
        } else {
            const float offset = std::max(1.0f, height / 14.0f);
            draw->AddText(smooth_font, height, ImVec2(at.x + offset, at.y + offset), shadow, item.text.c_str());
            draw->AddText(smooth_font, height, at, colour, item.text.c_str());
        }
    }

    if (layout.crosshair) {
        const float arm = 14.0f * unit * layout.scale, gap = 4.0f * unit * layout.scale, t = std::max(2.0f, 2.0f * unit * layout.scale);
        const ImVec2 c(display.x * 0.5f, display.y * 0.5f);
        for (int pass = 0; pass < 2; ++pass) {
            const ImU32 col = pass == 0 ? shadow : colour_normal;
            const float w = pass == 0 ? t * 2.2f : t;
            draw->AddLine(ImVec2(c.x - gap - arm, c.y), ImVec2(c.x - gap, c.y), col, w);
            draw->AddLine(ImVec2(c.x + gap, c.y), ImVec2(c.x + gap + arm, c.y), col, w);
            draw->AddLine(ImVec2(c.x, c.y - gap - arm), ImVec2(c.x, c.y - gap), col, w);
            draw->AddLine(ImVec2(c.x, c.y + gap), ImVec2(c.x, c.y + gap + arm), col, w);
        }
    }

    if (layout.sticks.show) {
        // Mode 2: left stick = yaw (side to side) and throttle (up and down); right stick = roll and pitch.
        const float half = 0.5f * 0.17f * display.y * std::clamp(layout.sticks.size, 0.4f, 3.0f);
        const float margin = layout.margin * display.x;
        const float bottom = display.y * (1.0f - layout.margin) - text_height * 1.6f;
        const float cy = bottom - half;
        const ImVec2 left(margin + half, cy), right(display.x - margin - half, cy);
        draw_stick(draw, left, half, layout.sticks.yaw, layout.sticks.throttle, colour_normal, shadow);
        draw_stick(draw, right, half, layout.sticks.roll, layout.sticks.pitch, colour_normal, shadow);
    }
}
} // namespace dingosdk::overlay::detail
