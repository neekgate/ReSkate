#pragma once
// Betaflight-style on-screen display for the drone: battery, flight timer, crosshair, throttle,
// altitude, speed. Drawn through the overlay's game-text feed (text lines on a 1920x1080 screen).
#include "drone_flight.h"
#include "Extension/UI/Overlay/overlay.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <string>

namespace dingosdk::drone::osd {

// A pretend 4S pack: 16.8 V full, sagging a little with throttle and draining over the flight.
inline float battery_volts(float seconds, float throttle) {
    return std::max(13.2f, 16.8f - 0.0045f * seconds - 0.7f * throttle);
}

// Appends the OSD to `text` when the drone is flying and the OSD is on.
inline void append(overlay::GameText& text) {
    if (!flying() || knobs().value[12].load(std::memory_order_relaxed) < 0.5f) return;
    const auto& t = telemetry();
    const float throttle = t.throttle.load(std::memory_order_relaxed);
    const float speed = t.speed.load(std::memory_order_relaxed);
    const float altitude = t.altitude.load(std::memory_order_relaxed);
    const float seconds = t.seconds.load(std::memory_order_relaxed);
    if (text.width <= 0 || text.height <= 0) { text.width = 1920; text.height = 1080; }
    constexpr std::uint32_t white = 0xffffffffU, red = 0xff3030ffU;
    const float volts = battery_volts(seconds, throttle);
    const bool low = volts < 14.0f;
    const bool blink = (static_cast<long long>(seconds * 2.0f) & 1) != 0;
    auto line = [&](float x, float y, float scale, std::uint32_t colour, bool centered, std::string s) {
        overlay::GameTextLine l;
        l.x = x; l.y = y; l.scale = scale; l.color = colour; l.centered = centered; l.text = std::move(s);
        text.lines.push_back(std::move(l));
    };
    const int total = static_cast<int>(seconds);
    line(220, 90, 2.2f, low && blink ? red : white, true, std::format("{:.1f}V", volts));
    line(960, 90, 2.0f, white, true, "ARMED");
    line(1700, 90, 2.2f, white, true, std::format("{:02}:{:02}", total / 60, total % 60));
    line(960, 510, 3.0f, white, true, "-  +  -");
    line(220, 930, 2.2f, white, true, std::format("THR {:3.0f}%", throttle * 100.0f));
    line(960, 930, 2.2f, white, true, std::format("ALT {:.1f}M", altitude));
    line(1700, 930, 2.2f, white, true, std::format("{:.0f} KM/H", speed * 3.6f));
}
} // namespace dingosdk::drone::osd
