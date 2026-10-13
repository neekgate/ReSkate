#include "overlay_internal.h"
#include "skate_style.h"
#include "Extension/Skater/drone_flight.h"
#include "Extension/Skater/drone_beacon.h"
#include "Extension/Skater/drone_outfit.h"
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
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s.\n\nConsole: dronetune %s (default %g)", drone::knob_info[k].help, drone::knob_info[k].name,
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

// Presets (drone_flight.h): a whole drone in one choice. "Custom" once anything is changed.
void preset_picker() {
    auto& s = state();
    const int current = drone::matching_preset(s.model.debug.camera_speed);
    const char* shown = current >= 0 ? drone::presets[current].name : "Custom";
    ImGui::SetNextItemWidth(-ImGui::GetFontSize() * 9.0f);
    if (ImGui::BeginCombo("Drone", shown)) {
        for (std::size_t p = 0; p < drone::preset_count; ++p) {
            const auto& preset = drone::presets[p];
            if (ImGui::Selectable(preset.name, static_cast<int>(p) == current)) {
                for (const auto& v : preset.values) { set(v.knob, v.value); save(v.knob); }
                s.model.debug.camera_speed = preset.top_speed;
                run(std::format("flyspeed {}", preset.top_speed));
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", preset.description);
        }
        ImGui::EndCombo();
    }
    if (current >= 0) ImGui::TextDisabled("%s", drone::presets[current].description);
    else ImGui::TextDisabled("Your own settings. Pick a drone above to start from one of the presets.");
}

void tab_fly() {
    auto& s = state();
    const bool enabled = drone::is_enabled();
    const bool radio_on = drone::radio::config().enabled.load();
    const bool freecam = s.model.debug.free_camera;
    if (enabled && freecam) ImGui::TextDisabled("Flying with %s.", radio_on ? "the controller" : "the keyboard and mouse");
    else if (enabled) ImGui::TextDisabled("Drone mode is on, but freecam is off: press Start flying.");
    ImGui::BeginDisabled(!s.model.debug.camera_available);
    if (ImGui::Button(enabled && freecam ? "Stop flying" : "Start flying", ImVec2(-FLT_MIN, 0))) {
        // The game's interface is hidden for the flight (the `hideui` drone setting) and only
        // brought back if the drone was what hid it.
        static bool hid_ui = false;
        if (enabled && freecam) {
            run("dronemode 0");
            run("freecam 0"); // back to the skater
            if (hid_ui) run("hideui 0");
            hid_ui = false;
        } else {
            if (!freecam) run("freecam 1");
            if (!enabled) run("dronemode 1");
            if (get(Knob::kHideUi) >= 0.5f && !s.model.debug.game_ui_hidden) { run("hideui 1"); hid_ui = true; }
        }
    }
    ImGui::EndDisabled();
    if (!s.model.debug.camera_available) ImGui::TextDisabled("Load into a level to fly.");
    bool radio = radio_on;
    if (ImGui::Checkbox("Fly with a controller (RC radio or gamepad)", &radio)) run(radio ? "droneradio 1" : "droneradio 0");
    if (!radio_on && drone::radio::poll().valid)
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "A controller is connected. Tick the box above to fly with it, or the game "
                           "keeps using it for your skater.");
    ImGui::Separator();
    preset_picker();
    {   // What the pilot's skater wears over their eyes while flying.
        const int choice = std::clamp(static_cast<int>(std::lround(get(Knob::kGoggles))), 0, static_cast<int>(drone::outfit::eyewear_count));
        ImGui::SetNextItemWidth(-ImGui::GetFontSize() * 9.0f);
        if (ImGui::BeginCombo("Goggles", choice ? drone::outfit::eyewear[choice - 1].label : "None")) {
            if (ImGui::Selectable("None", choice == 0)) { set(Knob::kGoggles, 0.0f); save(Knob::kGoggles); }
            for (std::size_t i = 0; i < drone::outfit::eyewear_count; ++i)
                if (ImGui::Selectable(drone::outfit::eyewear[i].label, choice == static_cast<int>(i) + 1)) {
                    set(Knob::kGoggles, static_cast<float>(i + 1));
                    save(Knob::kGoggles);
                }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Your skater wears these while you fly, and other players see them. Your own eyewear comes back when you land.");
    }
    toggle(Knob::kHideUi, "Hide the game's interface while flying");
    ImGui::Separator();
    const bool armed = drone::armed();
    ImGui::Text("Motors: %s", armed ? "ARMED" : "DISARMED");
    const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2.0f) / 3.0f;
    if (ImGui::Button(armed ? "Disarm" : "Arm", ImVec2(half, 0))) drone::set_armed(!armed);
    ImGui::SameLine();
    if (ImGui::Button("Back to takeoff", ImVec2(half, 0))) drone::request_reset();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Puts the drone back where it took off, with a fresh battery and timer.");
    ImGui::SameLine();
    if (ImGui::Button("Reset settings", ImVec2(half, 0))) run("dronetune reset");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Puts every drone setting back to its default.");
    ImGui::Separator();
    if (drone::flying()) {
        const auto& t = drone::telemetry();
        ImGui::Text("Throttle %3.0f%%   Speed %.0f km/h   Altitude %.1f m   Time %.0f s",
                    static_cast<double>(t.throttle.load() * 100.0f), static_cast<double>(t.speed.load() * 3.6f),
                    static_cast<double>(t.altitude.load()), static_cast<double>(t.seconds.load()));
    } else {
        ImGui::TextDisabled("Not flying.");
    }
    // The game's value comes back every frame; hold the one being set until the game has it, or
    // the slider would snap back while it is dragged.
    static float pending = -1.0f;
    static double sent_at = 0.0;
    if (pending >= 0.0f && (std::abs(s.model.debug.camera_speed - pending) < 0.01f ||
                            (sent_at > 0.0 && ImGui::GetTime() - sent_at > 1.0))) { pending = -1.0f; sent_at = 0.0; }
    float top = pending >= 0.0f ? pending : s.model.debug.camera_speed;
    ImGui::SetNextItemWidth(-ImGui::GetFontSize() * 9.0f);
    if (ImGui::SliderFloat("Top speed", &top, 0.6f, 300.0f, "%.1f", ImGuiSliderFlags_Logarithmic)) pending = top;
    if (ImGui::IsItemDeactivatedAfterEdit()) { run(std::format("flyspeed {}", top)); sent_at = ImGui::GetTime(); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The same setting as freecam's fly speed.\n\nConsole: flyspeed");
    // The flight model caps speed at four times the fly speed (m/s), twice that with boost.
    ImGui::TextDisabled("Up to about %.0f km/h (twice that with boost).", static_cast<double>(top * 4.0f * 3.6f));
}

void tab_handling() {
    slider(Knob::kGravity, "Gravity", "%.1f");
    slider(Knob::kHover, "Hover throttle (lower = more power)");
    slider(Knob::kDrag, "Air drag");
    slider(Knob::kUptilt, "Camera tilt (degrees)", "%.0f");
    ImGui::Separator();
    toggle(Knob::kCollide, "Collide with the world");
    slider(Knob::kRadius, "Collision size");
    slider(Knob::kFloor, "Invisible floor (with collisions off)", "%.1f");
    toggle(Knob::kDisarmFreeze, "Hang in the air when disarmed");
    ImGui::Separator();
    ImGui::TextUnformatted("Range");
    bool limited = get(Knob::kRange) > 0.0f;
    if (ImGui::Checkbox("Limited range", &limited)) { set(Knob::kRange, limited ? 300.0f : 0.0f); save(Knob::kRange); }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Like a real radio and video link: the picture breaks up as you near the limit, and\n"
                          "further out the motors cut (failsafe) until you reset. Off = fly anywhere on the map.");
    if (limited) {
        float metres = get(Knob::kRange);
        ImGui::SetNextItemWidth(-ImGui::GetFontSize() * 9.0f);
        if (ImGui::SliderFloat("Range (metres)", &metres, 50.0f, 5000.0f, "%.0f", ImGuiSliderFlags_Logarithmic)) set(Knob::kRange, metres);
        if (ImGui::IsItemDeactivatedAfterEdit()) save(Knob::kRange);
        ImGui::TextWrapped("Measured from where you took off. The picture starts to break up at %.0f m and is lost at %.0f m. "
                           "Stay without a picture for %.0f seconds, or go past %.0f m, and the motors cut.",
                           static_cast<double>(get(Knob::kRange) * drone::range_clean), static_cast<double>(get(Knob::kRange)),
                           static_cast<double>(drone::failsafe_after_seconds),
                           static_cast<double>(get(Knob::kRange) * drone::range_failsafe));
    }
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
    ImGui::TextWrapped("How fast the drone turns, set like Betaflight's \"Actual\" rates. Centre: the speed near the middle of "
                       "the stick. Max: the speed at full stick. Expo: how much softer the middle is.");
    rate_row("Roll", Knob::kRollCenter, Knob::kRollMax, Knob::kRollExpo);
    ImGui::Separator();
    rate_row("Pitch", Knob::kPitchCenter, Knob::kPitchMax, Knob::kPitchExpo);
    ImGui::Separator();
    rate_row("Yaw", Knob::kYawCenter, Knob::kYawMax, Knob::kYawExpo);
}

void tab_display() {
    toggle(Knob::kOsd, "Show the goggle display (OSD)");
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
    toggle(Knob::kSticks, "Show my sticks on screen");
    slider(Knob::kStickSize, "On-screen sticks size");
    ImGui::Separator();
    toggle(Knob::kAnalog, "Analogue video look");
    slider(Knob::kAnalogNoise, "Static");
    slider(Knob::kAnalogScan, "Scanlines");
    slider(Knob::kAnalogVignette, "Dark corners");
    slider(Knob::kAnalogGlitch, "Rolling glitches");
    ImGui::Separator();
    slider(Knob::kVolume, "Motor sound volume");
    slider(Knob::kMotorPitch, "Motor sound pitch");
}

const char* axis_text(int signed_axis, char* buffer, std::size_t size) {
    if (signed_axis == 0) std::snprintf(buffer, size, "none");
    else std::snprintf(buffer, size, "axis %d%s", std::abs(signed_axis), signed_axis < 0 ? " (reversed)" : "");
    return buffer;
}

// Live bars for the six axes; `highlight` (0-5) is drawn as the one being picked.
void axis_bars(const drone::radio::Sample& sample, int highlight) {
    for (int i = 0; i < 6; ++i) {
        const auto v = sample.axes[static_cast<std::size_t>(i)];
        char value[16];
        std::snprintf(value, sizeof value, "%+.2f", static_cast<double>(v));
        const bool lit = i == highlight;
        if (lit) ImGui::PushStyleColor(ImGuiCol_PlotHistogram, IM_COL32(120, 220, 120, 255));
        ImGui::ProgressBar((v + 1.0f) * 0.5f, ImVec2(ImGui::GetContentRegionAvail().x * 0.7f, 0), value);
        if (lit) ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::Text("Axis %d%s", i + 1, lit ? "  <" : "");
    }
}

// The guided setup (drone_radio.h: Calibration). Saves through the same commands as the manual controls.
void setup_wizard(const drone::radio::Sample& sample) {
    using Step = drone::radio::Calibration::Step;
    auto& c = drone::radio::calibration();
    if (sample.valid) c.update(sample.axes, sample.buttons, ImGui::GetIO().DeltaTime);
    const auto step = c.step();
    if (c.active()) drone::radio::mark_calibrating();
    const char* title = "";
    const char* body = "";
    switch (step) {
    case Step::rest:
        title = "Step 1 of 7: sticks at rest";
        body = "Let go of both sticks so they centre, and put the throttle all the way down (a gamepad's sticks just "
               "centre). Then press Ready.";
        break;
    case Step::throttle:
        title = "Step 2 of 7: throttle";
        body = c.waiting_for_return() ? "Got it. Now let the throttle back down."
                                      : "Push the throttle all the way up and hold it there. On a gamepad, that's the left stick, up.";
        break;
    case Step::yaw:
        title = "Step 3 of 7: yaw";
        body = c.waiting_for_return() ? "Got it. Let the stick go." : "Push the yaw stick (left stick, sideways) fully right and hold it.";
        break;
    case Step::pitch:
        title = "Step 4 of 7: pitch";
        body = c.waiting_for_return() ? "Got it. Let the stick go." : "Push the right stick fully forward (away from you) and hold it.";
        break;
    case Step::roll:
        title = "Step 5 of 7: roll";
        body = c.waiting_for_return() ? "Got it. Let the stick go." : "Push the right stick fully right and hold it.";
        break;
    case Step::arm:
        title = "Step 6 of 7: arm switch (optional)";
        body = "Flip the switch you want to arm the motors with to its ARMED position. No arm switch? Press Skip.";
        break;
    case Step::reset:
        title = "Step 7 of 7: reset switch (optional)";
        body = c.waiting_for_return() ? "Got it. Let the switch go back."
                                      : "Flick the switch or press the button you want to put the drone back at its start. No reset switch? Press Skip.";
        break;
    default: break;
    }
    if (step == Step::done) {
        // Save once, then show what was found.
        static bool saved = false;
        if (!saved) {
            const auto m = c.mapping();
            run(std::format("droneaxes {} {} {} {}", m[0], m[1], m[2], m[3]));
            set(Knob::kArmAxis, static_cast<float>(c.arm_axis())); save(Knob::kArmAxis);
            set(Knob::kArmButton, static_cast<float>(c.arm_button())); save(Knob::kArmButton);
            set(Knob::kResetAxis, static_cast<float>(c.reset_axis())); save(Knob::kResetAxis);
            set(Knob::kResetButton, static_cast<float>(c.reset_button())); save(Knob::kResetButton);
            set(Knob::kThrottleCentre, c.throttle_centred() ? 1.0f : 0.0f); save(Knob::kThrottleCentre);
            // Gamepad sticks drift more than a radio's gimbals.
            if (c.throttle_centred() && get(Knob::kDeadzone) < 0.12f) { set(Knob::kDeadzone, 0.12f); save(Knob::kDeadzone); }
            // Setting a controller up means flying with it.
            if (!drone::radio::config().enabled.load()) run("droneradio 1");
            saved = true;
        }
        ImGui::TextUnformatted("Setup saved. The drone now flies with this controller.");
        const auto m = c.mapping();
        static const char* const names[4] = {"Roll", "Pitch", "Throttle", "Yaw"};
        char buffer[32];
        for (int i = 0; i < 4; ++i) ImGui::BulletText("%s: %s", names[i], axis_text(m[static_cast<std::size_t>(i)], buffer, sizeof buffer));
        const auto switch_text = [&buffer](int axis, int button) -> std::string {
            if (button > 0) return std::format("button {}", button);
            return axis_text(axis, buffer, sizeof buffer);
        };
        ImGui::BulletText("Arm switch: %s", switch_text(c.arm_axis(), c.arm_button()).c_str());
        ImGui::BulletText("Reset switch: %s", switch_text(c.reset_axis(), c.reset_button()).c_str());
        if (c.throttle_centred())
            ImGui::TextWrapped("Your throttle springs back to the middle, so the middle now holds the drone up: push up to "
                               "climb, down to descend.");
        if (ImGui::Button("Done", ImVec2(-FLT_MIN, 0))) { c.cancel(); saved = false; }
        return;
    }
    ImGui::TextUnformatted(title);
    ImGui::TextWrapped("%s", body);
    if (!sample.valid) ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "The controller is not connected.");
    else axis_bars(sample, c.waiting_for_return() ? -1 : c.candidate());
    const float third = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2.0f) / 3.0f;
    if (step == Step::rest) {
        ImGui::BeginDisabled(!sample.valid);
        if (ImGui::Button("Ready", ImVec2(third, 0))) c.capture_rest(sample.axes, sample.buttons);
        ImGui::EndDisabled();
        ImGui::SameLine();
    } else if ((step == Step::arm || step == Step::reset) && !c.waiting_for_return()) {
        if (ImGui::Button("Skip", ImVec2(third, 0))) c.skip();
        ImGui::SameLine();
    }
    if (ImGui::Button("Cancel", ImVec2(third, 0))) c.cancel();
}

void tab_radio() {
    const auto sample = drone::radio::poll();
    auto& c = drone::radio::calibration();
    if (c.step() != drone::radio::Calibration::Step::idle) {
        setup_wizard(sample);
        return;
    }
    if (!sample.valid) {
        ImGui::TextWrapped("No controller found. Plug in a gamepad, or an RC radio with its USB mode set to Joystick (on "
                           "EdgeTX radios, pick \"USB Joystick (HID)\" when you plug it in).");
    } else {
        ImGui::Text("Controller: %s", sample.name.c_str());
        ImGui::BeginDisabled(!sample.valid);
        if (ImGui::Button("Set up controller", ImVec2(-FLT_MIN, 0))) c.start();
        ImGui::EndDisabled();
        ImGui::TextDisabled("Moves you through each stick and switch and saves which is which. The drone holds still meanwhile.");
    }
    if (!ImGui::CollapsingHeader("Manual setup")) return;
    if (sample.valid) {
        axis_bars(sample, -1);
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
        if (ImGui::Checkbox("Reverse", &flip)) changed = true;
        map[static_cast<std::size_t>(i)] = (axis + 1) * (flip ? -1 : 1);
        ImGui::PopID();
    }
    if (changed) run(std::format("droneaxes {} {} {} {}", map[0], map[1], map[2], map[3]));
    toggle(Knob::kThrottleCentre, "Throttle springs back to the middle (gamepads)");
    slider(Knob::kDeadzone, "Stick dead zone");
    ImGui::Separator();
    ImGui::TextUnformatted("Switches (use an axis or button no stick uses; 0 = none)");
    integer(Knob::kArmAxis, "Arm switch axis");
    integer(Knob::kArmButton, "Arm button");
    integer(Knob::kResetAxis, "Reset switch axis");
    integer(Knob::kResetButton, "Reset button");
}

void tab_lobby() {
    if (!drone::beacon::server_allows().load()) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "This server has drones turned off.");
        ImGui::TextWrapped("You can still fly, but nobody here sees your drone and you don't see theirs.");
        ImGui::Separator();
    }
    ImGui::TextWrapped("Other players only see drones if they are running this version of ReSkate.");
    toggle(Knob::kBeacon, "Let other players see my drone");
    toggle(Knob::kShowBeacons, "Show other players' drones");
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
            if (ImGui::BeginTabItem("Controller")) { tab_radio(); ImGui::EndTabItem(); }
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
