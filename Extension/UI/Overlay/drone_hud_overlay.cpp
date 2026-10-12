#include "overlay_internal.h"
#include "Extension/Skater/drone_font.h"
#include "Extension/Skater/drone_osd.h"
#include "Extension/Skater/drone_beacon.h"
#include "Engine/Game/UI/game_view.h"

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

std::vector<dingosdk::drone::beacon::Remote>& beacons() {
    static std::vector<dingosdk::drone::beacon::Remote> value;
    return value;
}

std::string metres_text(float metres) {
    char text[32]{};
    if (metres < 1000.0f) std::snprintf(text, sizeof text, "%.0f m", std::floor(metres));
    else std::snprintf(text, sizeof text, "%.1f km", metres / 1000.0f);
    return text;
}

// A little quadcopter: an X with a propeller circle at each end, drawn around `at`.
void drone_icon(ImDrawList* draw, ImVec2 at, float size, ImU32 colour, ImU32 shadow) {
    const float arm = size * 0.5f, prop = size * 0.2f, t = std::max(1.5f, size * 0.09f);
    const ImVec2 ends[4] = {{-arm, -arm}, {arm, -arm}, {arm, arm}, {-arm, arm}};
    for (int pass = 0; pass < 2; ++pass) {
        const ImU32 c = pass == 0 ? shadow : colour;
        const float w = pass == 0 ? t * 2.4f : t;
        draw->AddLine(ImVec2(at.x + ends[0].x, at.y + ends[0].y), ImVec2(at.x + ends[2].x, at.y + ends[2].y), c, w);
        draw->AddLine(ImVec2(at.x + ends[1].x, at.y + ends[1].y), ImVec2(at.x + ends[3].x, at.y + ends[3].y), c, w);
        for (const auto& e : ends) draw->AddCircle(ImVec2(at.x + e.x, at.y + e.y), prop, c, 14, w);
    }
    draw->AddCircleFilled(at, size * 0.1f, colour);
}

// Markers for other players' drones, placed over the world with the camera the client last used.
void draw_beacons(const std::vector<dingosdk::drone::beacon::Remote>& list) {
    if (list.empty()) return;
    const auto view = dingosdk::latest_game_view();
    if (!view || !(view->vertical_fov > 1 && view->vertical_fov < 175)) return;
    const auto display = ImGui::GetIO().DisplaySize;
    if (display.x <= 0 || display.y <= 0) return;
    const auto& m = view->world;
    const float k = display.y / 1080.0f;
    const float focal = display.y / (2.0f * std::tan(view->vertical_fov * 3.14159265f / 360.0f));
    const ImVec2 centre(display.x * 0.5f, display.y * 0.5f);
    const float margin = 40.0f * k;
    auto& s = state();
    auto* font = s.menu.bold ? s.menu.bold : ImGui::GetFont();
    auto* draw = ImGui::GetBackgroundDrawList();
    const float pulse = 0.78f + 0.22f * std::sin(static_cast<float>(ImGui::GetTime()) * 5.0f);
    const ImU32 colour = IM_COL32(90, 232, 255, static_cast<int>(255 * pulse));
    const ImU32 shadow = IM_COL32(0, 0, 0, 200);
    for (const auto& other : list) {
        const float dx = other.position[0] - m[12], dy = other.position[1] - m[13], dz = other.position[2] - m[14];
        const float depth = -(dx * m[8] + dy * m[9] + dz * m[10]);
        const float side = dx * m[0] + dy * m[1] + dz * m[2];
        const float height = dx * m[4] + dy * m[5] + dz * m[6];
        const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        const std::string label = (other.name.empty() ? std::string("Player") : other.name) + "'s drone";
        const std::string metres = metres_text(distance);
        const float size = std::clamp(14.0f * k, 10.0f, 30.0f);
        ImVec2 at;
        bool on_screen = false;
        if (depth > 0.1f) {
            at = ImVec2(centre.x + side * focal / depth, centre.y - height * focal / depth);
            on_screen = at.x >= margin && at.x <= display.x - margin && at.y >= margin && at.y <= display.y - margin;
        }
        if (on_screen) {
            drone_icon(draw, at, std::clamp(34.0f * k, 22.0f, 60.0f), colour, shadow);
            const auto text_size = font->CalcTextSizeA(size, FLT_MAX, 0.0f, label.c_str());
            const ImVec2 text_at(at.x - text_size.x * 0.5f, at.y - 34.0f * k - text_size.y);
            const float offset = std::max(1.0f, size / 14.0f);
            draw->AddText(font, size, ImVec2(text_at.x + offset, text_at.y + offset), shadow, label.c_str());
            draw->AddText(font, size, text_at, colour, label.c_str());
            const float fine_size = size * 0.78f;
            const auto metres_size = font->CalcTextSizeA(fine_size, FLT_MAX, 0.0f, metres.c_str());
            const ImVec2 metres_at(at.x - metres_size.x * 0.5f, at.y + 30.0f * k);
            draw->AddText(font, fine_size, ImVec2(metres_at.x + offset, metres_at.y + offset), shadow, metres.c_str());
            draw->AddText(font, fine_size, metres_at, IM_COL32(235, 235, 235, 230), metres.c_str());
        } else {
            // Off screen: an arrow at the screen's edge pointing to it, with the distance.
            float ex = depth > 0.1f ? at.x - centre.x : side, ey = depth > 0.1f ? at.y - centre.y : -height;
            if (depth <= 0.1f && std::abs(ex) < 1e-3f && std::abs(ey) < 1e-3f) ey = 1.0f; // straight behind
            const float hx = centre.x - margin, hy = centre.y - margin;
            const float scale = std::min(std::abs(ex) > 1e-6f ? hx / std::abs(ex) : FLT_MAX,
                                         std::abs(ey) > 1e-6f ? hy / std::abs(ey) : FLT_MAX);
            const ImVec2 edge(centre.x + ex * scale, centre.y + ey * scale);
            const float length = std::sqrt(ex * ex + ey * ey);
            const ImVec2 dir(length > 1e-6f ? ex / length : 0.0f, length > 1e-6f ? ey / length : 1.0f);
            const ImVec2 perp(-dir.y, dir.x);
            const float a = 13.0f * k;
            const ImVec2 tip(edge.x + dir.x * a, edge.y + dir.y * a);
            const ImVec2 b1(edge.x - dir.x * a * 0.5f + perp.x * a * 0.8f, edge.y - dir.y * a * 0.5f + perp.y * a * 0.8f);
            const ImVec2 b2(edge.x - dir.x * a * 0.5f - perp.x * a * 0.8f, edge.y - dir.y * a * 0.5f - perp.y * a * 0.8f);
            draw->AddTriangleFilled(ImVec2(tip.x + 2.0f * k, tip.y + 2.0f * k), ImVec2(b1.x + 2.0f * k, b1.y + 2.0f * k),
                                    ImVec2(b2.x + 2.0f * k, b2.y + 2.0f * k), shadow);
            draw->AddTriangleFilled(tip, b1, b2, colour);
            const float fine_size = size * 0.78f;
            const auto metres_size = font->CalcTextSizeA(fine_size, FLT_MAX, 0.0f, metres.c_str());
            ImVec2 text_at(edge.x - dir.x * a * 2.2f - metres_size.x * 0.5f, edge.y - dir.y * a * 2.2f - metres_size.y * 0.5f);
            text_at.x = std::clamp(text_at.x, 4.0f * k, display.x - metres_size.x - 4.0f * k);
            text_at.y = std::clamp(text_at.y, 4.0f * k, display.y - metres_size.y - 4.0f * k);
            const float offset = std::max(1.0f, fine_size / 14.0f);
            draw->AddText(font, fine_size, ImVec2(text_at.x + offset, text_at.y + offset), shadow, metres.c_str());
            draw->AddText(font, fine_size, text_at, colour, metres.c_str());
        }
    }
}
} // namespace

bool drone_hud_pending() {
    hud() = dingosdk::drone::osd::build();
    beacons().clear();
    if (dingosdk::drone::knobs().value[dingosdk::drone::kShowBeacons].load(std::memory_order_relaxed) >= 0.5f)
        beacons() = dingosdk::drone::beacon::snapshot();
    return hud().active || !beacons().empty();
}

void draw_drone_hud() {
    draw_beacons(beacons());
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
