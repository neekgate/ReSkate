#include "overlay_internal.h"
#include "skate_style.h"
#include "Extension/Skater/drone_flight.h"
#include <cstdio>
#include <format>

// The FPV drone's settings window: fly / arm / reset buttons, handling, Betaflight rates, the
// display, the radio and multiplayer. It shows while the menu or console holds the pointer
// (like the profiler window). Every change takes effect at once; it is also saved by running the
// same `dronetune` command you could type, so the console and this window always agree.

namespace dingosdk::overlay::detail {
namespace {
namespace skate = dingosdk::skate_theme;
namespace drone = dingosdk::drone;
using drone::Knob;

float scale() {
    return std::clamp(state().model.menu_scale, dingosdk::min_menu_scale, dingosdk::max_menu_scale);
}

// Runs a console command on the game's thread (what the console itself does).
bool run(const std::string& command) {
    auto& s = state();
    if (!s.callbacks.queue_console_command) return false;
    std::array<char, 256> result{};
    return s.callbacks.queue_console_command(s.callbacks.user, command.c_str(), result.data(), result.size());
}

float get(Knob k) { return drone::knobs().value[k].load(std::memory_order_relaxed); }
void set(Knob k, float v) { drone::knobs().value[k].store(v, std::memory_order_relaxed); }
// Saves a setting by running `dronetune`, which also clamps it and keeps it for next time.
void save(Knob k) { run(std::format("dronetune {} {}", drone::knob_info[k].name, get(k))); }

void tip(Knob k) {
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n(%s, default %g)", drone::knob_info[k].help, drone::knob_info[k].name,
                                                  static_cast<double>(drone::knob_info[k].def));
}

void slider(Knob k, const char* label, const char* format = "%.2f") {
    float v = get(k);
    ImGui::SetNextItemWidth(-ImGui::GetFontSize() * 9.0f);
    if (ImGui::SliderFloat(label, &v, drone::knob_info[k].lo, drone::knob_info[k].hi, format)) set(k, v);
    if (ImGui::IsItemDeactivatedAfterEdit()) save(k);
    tip(k);
}
void integer(Knob k, const char* label) {
    int v = static_cast<int>(std::lround(get(k)));
    ImGui::SetNextItemWidth(-ImGui::GetFontSize() * 9.0f);
    if (ImGui::SliderInt(label, &v, static_cast<int>(drone::knob_info[k].lo), static_cast<int>(drone::knob_info[k].hi)))
        set(k, static_cast<float>(v));
    if (ImGui::IsItemDeactivatedAfterEdit()) save(k);
    tip(k);
}
void toggle(Knob k, const char* label) {
    bool on = get(k) >= 0.5f;
    if (ImGui::Checkbox(label, &on)) { set(k, on ? 1.0f : 0.0f); save(k); }
    tip(k);
}
void choice(Knob k, const char* label, const char* const* items, int count) {
    int v = std::clamp(static_cast<int>(std::lround(get(k))), 0, count - 1);
    ImGui::SetNextItemWidth(-ImGui::GetFontSize() * 9.0f);
    if (ImGui::Combo(label, &v, items, count)) { set(k, static_cast<float>(v)); save(k); }
    tip(k);
}

void tab_fly() {
    auto& s = state();
    const bool enabled = drone::is_enabled();
    const bool radio_on = drone::radio::config().enabled.load();
    const bool freecam = s.model.debug.free_camera;
    ImGui::TextDisabled("Freecam: %s   Drone mode: %s   Radio: %s", freecam ? "on" : "off", enabled ? "on" : "off",
                        radio_on ? "on" : "off");
    ImGui::BeginDisabled(!s.model.debug.camera_available);
    if (ImGui::Button(enabled ? "Stop flying" : "Start flying", ImVec2(-FLT_MIN, 0))) {
        if (enabled) { run("dronemode 0"); }
        else { if (!freecam) run("freecam 1"); run("dronemode 1"); }
    }
    ImGui::EndDisabled();
    if (!s.model.debug.camera_available) ImGui::TextDisabled("Waiting for the player's controls (load into a level).");
    bool radio = radio_on;
    if (ImGui::Checkbox("Fly with the radio controller", &radio)) run(radio ? "droneradio 1" : "droneradio 0");
    ImGui::Separator();
    const bool armed = drone::armed();
    ImGui::Text("Motors: %s", armed ? "ARMED" : "DISARMED");
    const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2.0f) / 3.0f;
    if (ImGui::Button(armed ? "Disarm" : "Arm", ImVec2(half, 0))) drone::set_armed(!armed);
    ImGui::SameLine();
    if (ImGui::Button("Reset position", ImVec2(half, 0))) drone::request_reset();
    ImGui::SameLine();
    if (ImGui::Button("Defaults", ImVec2(half, 0))) run("dronetune reset");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Puts every drone setting back to its default.");
    ImGui::Separator();
    if (drone::flying()) {
        const auto& t = drone::telemetry();
        ImGui::Text("Throttle %3.0f%%   Speed %.0f km/h   Altitude %.1f m   Time %.0f s",
                    static_cast<double>(t.throttle.load() * 100.0f), static_cast<double>(t.speed.load() * 3.6f),
                    static_cast<double>(t.altitude.load()), static_cast<double>(t.seconds.load()));
    } else {
        ImGui::TextDisabled("Not flying right now.");
    }
    float top = s.model.debug.camera_speed;
    ImGui::SetNextItemWidth(-ImGui::GetFontSize() * 9.0f);
    if (ImGui::SliderFloat("Top speed (fly speed)", &top, 0.6f, 300.0f, "%.0f", ImGuiSliderFlags_Logarithmic) && s.callbacks.queue_console_command)
        s.model.debug.camera_speed = top;
    if (ImGui::IsItemDeactivatedAfterEdit()) run(std::format("flyspeed {}", top));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The drone's speed limit is four times this (eight with boost).");
}

void tab_handling() {
    slider(Knob::kGravity, "Gravity", "%.1f");
    slider(Knob::kHover, "Hover point (lower = more thrust)");
    slider(Knob::kDrag, "Air drag");
    slider(Knob::kUptilt, "Camera tilt (degrees)", "%.0f");
    ImGui::Separator();
    toggle(Knob::kCollide, "Collide with the world");
    slider(Knob::kRadius, "Collision size");
    slider(Knob::kFloor, "Invisible floor (when not colliding)", "%.1f");
    toggle(Knob::kPark, "Radio: throttle at the bottom freezes the drone");
    toggle(Knob::kDisarmFreeze, "A disarmed drone hangs in place");
}

void rate_row(const char* axis, Knob centre, Knob max, Knob expo) {
    ImGui::PushID(axis);
    ImGui::TextUnformatted(axis);
    slider(centre, "Centre sensitivity", "%.0f");
    slider(max, "Max rate", "%.0f");
    slider(expo, "Expo");
    ImGui::PopID();
}
void tab_rates() {
    ImGui::TextDisabled("Betaflight \"Actual\" rates: centre sensitivity, max rate and expo, in degrees per second.");
    rate_row("Roll", Knob::kRollCenter, Knob::kRollMax, Knob::kRollExpo);
    ImGui::Separator();
    rate_row("Pitch", Knob::kPitchCenter, Knob::kPitchMax, Knob::kPitchExpo);
    ImGui::Separator();
    rate_row("Yaw", Knob::kYawCenter, Knob::kYawMax, Knob::kYawExpo);
}

void tab_display() {
    toggle(Knob::kOsd, "Show the flight display (OSD)");
    static const char* const colours[] = {"White", "Green", "Yellow", "Cyan", "Orange", "Pink"};
    static const char* const fonts[] = {"Pixel (goggles)", "Smooth"};
    slider(Knob::kOsdScale, "Text size");
    choice(Knob::kOsdColor, "Colour", colours, 6);
    choice(Knob::kOsdFont, "Font", fonts, 2);
    slider(Knob::kOsdMargin, "Distance from the edge");
    toggle(Knob::kCrosshair, "Crosshair");
    ImGui::TextDisabled("Show:");
    toggle(Knob::kShowArmed, "Armed / disarmed"); ImGui::SameLine();
    toggle(Knob::kShowBattery, "Battery");
    toggle(Knob::kShowTimer, "Timer"); ImGui::SameLine();
    toggle(Knob::kShowThrottle, "Throttle"); ImGui::SameLine();
    toggle(Knob::kShowAlt, "Altitude"); ImGui::SameLine();
    toggle(Knob::kShowSpeed, "Speed");
    ImGui::Separator();
    toggle(Knob::kSticks, "Stick overlay");
    slider(Knob::kStickSize, "Stick overlay size");
    ImGui::Separator();
    slider(Knob::kVolume, "Motor sound volume");
}

void tab_radio() {
    const auto sample = drone::radio::poll();
    if (!sample.valid) {
        ImGui::TextWrapped("No radio controller found. Plug it in and set its USB mode to Joystick.");
    } else {
        ImGui::Text("Controller: %s", sample.name.c_str());
        for (int i = 0; i < 6; ++i) {
            char label[32];
            std::snprintf(label, sizeof label, "Axis %d", i + 1);
            char value[16];
            std::snprintf(value, sizeof value, "%+.2f", static_cast<double>(sample.axes[static_cast<std::size_t>(i)]));
            ImGui::ProgressBar((sample.axes[static_cast<std::size_t>(i)] + 1.0f) * 0.5f, ImVec2(ImGui::GetContentRegionAvail().x * 0.7f, 0), value);
            ImGui::SameLine();
            ImGui::TextUnformatted(label);
        }
        std::string pressed;
        for (int n = 1; n <= 32; ++n)
            if (sample.buttons & (1u << (n - 1))) pressed += (pressed.empty() ? "" : " ") + std::to_string(n);
        ImGui::Text("Buttons pressed: %s", pressed.empty() ? "none" : pressed.c_str());
        ImGui::TextDisabled("Move a stick or flip a switch to see which axis or button it is.");
    }
    ImGui::Separator();
    ImGui::TextUnformatted("Which axis each stick uses");
    static const char* const names[4] = {"Roll", "Pitch", "Throttle", "Yaw"};
    static const char* const axes[] = {"Axis 1", "Axis 2", "Axis 3", "Axis 4", "Axis 5", "Axis 6"};
    auto map = drone::radio::mapping_snapshot();
    bool changed = false;
    for (int i = 0; i < 4; ++i) {
        ImGui::PushID(i);
        int axis = std::clamp(std::abs(map[static_cast<std::size_t>(i)]), 1, 6) - 1;
        bool flip = map[static_cast<std::size_t>(i)] < 0;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.0f);
        if (ImGui::Combo(names[i], &axis, axes, 6)) changed = true;
        ImGui::SameLine();
        if (ImGui::Checkbox("Flip", &flip)) changed = true;
        map[static_cast<std::size_t>(i)] = (axis + 1) * (flip ? -1 : 1);
        ImGui::PopID();
    }
    if (changed) run(std::format("droneaxes {} {} {} {}", map[0], map[1], map[2], map[3]));
    ImGui::Separator();
    ImGui::TextUnformatted("Switches (use an axis or button no stick uses; 0 = none)");
    integer(Knob::kArmAxis, "Arm switch axis");
    integer(Knob::kArmButton, "Arm button");
    integer(Knob::kResetAxis, "Reset switch axis");
    integer(Knob::kResetButton, "Reset button");
}

void tab_lobby() {
    ImGui::TextWrapped("Let other players in your lobby see where your drone is. They need this build of ReSkate too.");
    toggle(Knob::kBeacon, "Share my drone's position");
    toggle(Knob::kShowBeacons, "Show other players' drone markers");
}
} // namespace

void draw_drone_window() {
    if (!drone::menu_flag().load()) return;
    auto& s = state();
    const float k = scale();
    const auto P = [k](float value) { return value * k; };
    auto& io = ImGui::GetIO();
    const auto restore_font_scale = io.FontGlobalScale;
    io.FontGlobalScale = k;
    const auto* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + 16, viewport->WorkPos.y + 16), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(P(520), std::min(P(640), viewport->WorkSize.y - 32)), ImGuiCond_FirstUseEver);
    ImGui::PushFont(s.menu.body);
    const int colours = skate::push_widget_colours();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
    bool open = true;
    if (ImGui::Begin("FPV drone###ReSkateDrone", &open, ImGuiWindowFlags_NoCollapse)) {
        if (ImGui::BeginTabBar("##drone_tabs")) {
            if (ImGui::BeginTabItem("Fly")) { tab_fly(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Handling")) { tab_handling(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Rates")) { tab_rates(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Display")) { tab_display(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Radio")) { tab_radio(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Lobby")) { tab_lobby(); ImGui::EndTabItem(); }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(colours);
    ImGui::PopFont();
    io.FontGlobalScale = restore_font_scale;
    if (!open) drone::menu_flag().store(false);
}
} // namespace dingosdk::overlay::detail
