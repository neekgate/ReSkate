#pragma once
// USB radio-controller input for the FPV drone (RadioMaster, Jumper, FrSky... anything that
// shows up in Windows' "Set up USB game controllers" when its USB mode is Joystick).
//
// The pure maths (normalise / dead-zone / axis mapping) has no Windows dependency so it can be
// unit-tested anywhere. Only poll() and game_in_foreground() need Windows (winmm).
//
// Xbox and PlayStation (DualSense / DualShock 4) controllers are read through the overlay's own
// gamepad reader and presented as six axes too: 1 left X, 2 left Y, 3 right X, 4 right Y, 5 left
// trigger, 6 right trigger (sticks: right/up positive, triggers -1..1), and buttons 1 A/Cross, 2 B/Circle,
// 3 X/Square, 4 Y/Triangle, 5 LB, 6 RB, 7 Back, 8 Start, 9 L3, 10 R3, 11-14 d-pad up/down/left/right,
// 15 LT, 16 RT. So the axis mapping, switches and `droneradiocheck` work the same for every controller.
//
// Axis numbers 1..6 are winmm's X, Y, Z, R, U, V. `droneradiocheck` prints all six so you can see
// which one each stick moves; `droneaxes roll pitch throttle yaw` maps them (negative flips).
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>
#ifdef _WIN32
#include "Extension/UI/Overlay/overlay.h"
#include <Windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#endif

namespace dingosdk::drone::radio {

// Mode 2 on an EdgeTX radio sends AETR: channel 1 aileron (roll), 2 elevator (pitch),
// 3 throttle, 4 rudder (yaw). Those are the defaults below: roll=1, pitch=2, throttle=3, yaw=4.
inline constexpr int auto_device = -1;
inline constexpr int gamepad_device = 100;   // Xbox / PlayStation controller; 0..15 are winmm joysticks
struct Config {
    std::atomic<bool> enabled{false};
    std::atomic<int> map[4];       // radio / joystick: roll, pitch, throttle, yaw -> signed axis number (1..6)
    std::atomic<int> pad_map[4];   // the same for an Xbox / PlayStation controller
    std::atomic<int> device{auto_device};
    Config() {
        for (int i = 0; i < 4; ++i) map[i] = i + 1;
        // Mode 2 on a gamepad: left stick = throttle (Y) and yaw (X), right stick = pitch (Y) and roll (X).
        pad_map[0] = 3; pad_map[1] = 4; pad_map[2] = 2; pad_map[3] = 1;
    }
};
inline Config& config() { static Config value; return value; }
inline std::array<int, 4> mapping_snapshot() {
    auto& c = config();
    return {c.map[0].load(), c.map[1].load(), c.map[2].load(), c.map[3].load()};
}
inline std::array<int, 4> pad_mapping_snapshot() {
    auto& c = config();
    return {c.pad_map[0].load(), c.pad_map[1].load(), c.pad_map[2].load(), c.pad_map[3].load()};
}
// The mapping that applies to a controller (a sample's device number, see Sample).
inline std::array<int, 4> mapping_for(int device) { return device == gamepad_device ? pad_mapping_snapshot() : mapping_snapshot(); }
// The mapping `droneaxes` / the menu edit: the one for the controller picked in the dropdown.
inline std::array<int, 4> selected_mapping() { return mapping_for(config().device.load()); }
inline void store_selected_mapping(const std::array<int, 4>& next) {
    auto& c = config();
    auto& target = c.device.load() == gamepad_device ? c.pad_map : c.map;
    for (std::size_t i = 0; i < 4; ++i) target[i] = next[i];
}
// Betaflight-style mode presets for the gamepad. Mode 2: left = throttle/yaw, right = pitch/roll.
// Mode 1: left = pitch/yaw, right = throttle/roll.
inline std::array<int, 4> gamepad_preset(int mode) {
    return mode == 1 ? std::array<int, 4>{3, 2, 4, 1} : std::array<int, 4>{3, 4, 2, 1};
}

// Sticks as the flight model wants them.
struct Command {
    bool valid = false;
    float roll = 0;      // + = right
    float pitch = 0;     // + = stick forward (nose down)
    float yaw = 0;       // + = right
    float throttle = 0;  // 0..1 (absolute; the throttle stick does not self-centre)
};

inline float normalize_axis(std::uint32_t value, std::uint32_t lo, std::uint32_t hi) {
    if (hi <= lo) return 0.0f;
    const float t = (static_cast<float>(value) - static_cast<float>(lo)) / (static_cast<float>(hi) - static_cast<float>(lo));
    return std::clamp(t * 2.0f - 1.0f, -1.0f, 1.0f);
}
inline float deadzone(float v, float dz) {
    const float a = std::abs(v);
    if (!(a > dz)) return 0.0f;
    return std::copysign(std::min(1.0f, (a - dz) / (1.0f - dz)), v);
}
inline float pick_axis(const std::array<float, 6>& axes, int signed_index) {
    const int index = std::abs(signed_index) - 1;
    if (index < 0 || index > 5) return 0.0f;
    return signed_index < 0 ? -axes[index] : axes[index];
}
// hover_centre < 0: the throttle axis is absolute (a radio: stick bottom = 0, top = 1).
// hover_centre >= 0: the throttle stick springs back to the middle (a gamepad), so the middle holds
// that throttle (the hover throttle), down goes to 0 and up goes to 1.
inline Command map_axes(const std::array<float, 6>& axes, const std::array<int, 4>& map, float dz = 0.03f,
                        float hover_centre = -1.0f) {
    Command out;
    out.valid = true;
    out.roll = deadzone(pick_axis(axes, map[0]), dz);
    out.pitch = deadzone(pick_axis(axes, map[1]), dz);
    if (hover_centre < 0.0f) {
        out.throttle = std::clamp((pick_axis(axes, map[2]) + 1.0f) * 0.5f, 0.0f, 1.0f);
    } else {
        const float h = std::clamp(hover_centre, 0.0f, 1.0f);
        const float t = deadzone(pick_axis(axes, map[2]), dz);
        out.throttle = t >= 0.0f ? h + t * (1.0f - h) : h + t * h;
    }
    out.yaw = deadzone(pick_axis(axes, map[3]), dz);
    return out;
}

// ---- Xbox / PlayStation controllers as six axes and 16 buttons (see the top of this file).
struct PadState {
    float left_x = 0, left_y = 0, right_x = 0, right_y = 0, left_trigger = 0, right_trigger = 0;   // sticks -1..1, triggers 0..1
    unsigned xinput_buttons = 0;                                                                    // XInput wButtons
};
inline std::array<float, 6> pad_axes(const PadState& pad) {
    const auto f = [](float v) { return std::isfinite(v) ? std::clamp(v, -1.0f, 1.0f) : 0.0f; };
    return {f(pad.left_x), f(pad.left_y), f(pad.right_x), f(pad.right_y),
            f(pad.left_trigger) * 2.0f - 1.0f, f(pad.right_trigger) * 2.0f - 1.0f};
}
inline std::uint32_t pad_buttons(const PadState& pad) {
    constexpr unsigned mask[14] = {0x1000, 0x2000, 0x4000, 0x8000, 0x0100, 0x0200, 0x0020, 0x0010, 0x0040, 0x0080,
                                   0x0001, 0x0002, 0x0004, 0x0008};
    std::uint32_t out = 0;
    for (unsigned i = 0; i < 14; ++i)
        if (pad.xinput_buttons & mask[i]) out |= 1u << i;
    if (pad.left_trigger > 0.5f) out |= 1u << 14;
    if (pad.right_trigger > 0.5f) out |= 1u << 15;
    return out;
}

struct Sample {
    bool valid = false;
    std::array<float, 6> axes{};
    std::uint32_t buttons = 0;   // bit n-1 set = button n pressed
    std::string name;
    int device = -1;       // the winmm id, or gamepad_device
    bool gamepad = false;  // an Xbox / PlayStation controller
};

// One line for the controller dropdown.
struct DeviceInfo { int id; std::string name; };

#ifdef _WIN32
namespace detail {
inline std::string narrow(const wchar_t* text) {
    std::string out;
    for (; *text; ++text) out += *text < 128 ? static_cast<char>(*text) : '?';
    return out;
}
inline std::string lowered(std::string s) {
    for (auto& ch : s) if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + 'a' - 'A');
    return s;
}
inline bool read_device(UINT id, Sample& out) {
    JOYCAPSW caps{};
    if (joyGetDevCapsW(id, &caps, sizeof(caps)) != JOYERR_NOERROR) return false;
    JOYINFOEX info{};
    info.dwSize = sizeof(info);
    info.dwFlags = JOY_RETURNX | JOY_RETURNY | JOY_RETURNZ | JOY_RETURNR | JOY_RETURNU | JOY_RETURNV | JOY_RETURNBUTTONS;
    if (joyGetPosEx(id, &info) != JOYERR_NOERROR) return false;
    const UINT n = caps.wNumAxes;
    out.axes = {};
    if (n > 0) out.axes[0] = normalize_axis(info.dwXpos, caps.wXmin, caps.wXmax);
    if (n > 1) out.axes[1] = normalize_axis(info.dwYpos, caps.wYmin, caps.wYmax);
    if (n > 2) out.axes[2] = normalize_axis(info.dwZpos, caps.wZmin, caps.wZmax);
    if (n > 3) out.axes[3] = normalize_axis(info.dwRpos, caps.wRmin, caps.wRmax);
    if (n > 4) out.axes[4] = normalize_axis(info.dwUpos, caps.wUmin, caps.wUmax);
    if (n > 5) out.axes[5] = normalize_axis(info.dwVpos, caps.wVmin, caps.wVmax);
    out.buttons = info.dwButtons;
    out.name = narrow(caps.szPname);
    out.device = static_cast<int>(id);
    out.valid = true;
    return true;
}

inline bool read_gamepad_sample(Sample& out) {
    const auto pad = overlay::read_gamepad();
    if (!pad.valid) return false;
    PadState state;
    state.left_x = pad.left_x; state.left_y = pad.left_y; state.right_x = pad.right_x; state.right_y = pad.right_y;
    state.left_trigger = pad.left_trigger; state.right_trigger = pad.right_trigger; state.xinput_buttons = pad.buttons;
    out.axes = pad_axes(state);
    out.buttons = pad_buttons(state);
    out.name = pad.playstation ? "PlayStation controller (DualSense / DualShock 4)" : "Xbox controller";
    out.device = gamepad_device;
    out.gamepad = true;
    out.valid = true;
    return true;
}
}

// Every controller that could be picked: Auto, each joystick / radio winmm sees, and the gamepad entry.
inline std::vector<DeviceInfo> list_devices() {
    std::vector<DeviceInfo> out{{auto_device, "Auto (a radio if plugged in, else the first controller)"}};
    const UINT count = std::min<UINT>(joyGetNumDevs(), 16);
    for (UINT i = 0; i < count; ++i) {
        Sample candidate;
        if (detail::read_device(i, candidate)) out.push_back({static_cast<int>(i), candidate.name});
    }
    out.push_back({gamepad_device, "Xbox / PlayStation controller"});
    return out;
}

// Reads the selected controller. Safe to call from any thread; rescans for a controller at most once a second.
inline Sample poll() {
    static std::atomic<int> cached{-1};
    static std::atomic<ULONGLONG> next_scan{0};
    Sample sample;
    const int forced = config().device.load();
    if (forced == gamepad_device) { detail::read_gamepad_sample(sample); return sample; }
    if (forced >= 0) { detail::read_device(static_cast<UINT>(forced), sample); return sample; }
    int id = cached.load();
    if (id >= 0 && detail::read_device(static_cast<UINT>(id), sample)) return sample;
    cached.store(-1);
    const auto now = GetTickCount64();
    if (now < next_scan.load()) { if (!sample.valid) detail::read_gamepad_sample(sample); return sample; }
    next_scan.store(now + 1000);
    const UINT count = std::min<UINT>(joyGetNumDevs(), 16);
    int first = -1;
    for (UINT i = 0; i < count; ++i) {
        Sample candidate;
        if (!detail::read_device(i, candidate)) continue;
        if (detail::lowered(candidate.name).find("radio") != std::string::npos) { cached.store(static_cast<int>(i)); return candidate; }
        // An Xbox pad also shows up here, with its triggers fused on one axis; the gamepad entry reads it properly.
        if (first < 0 && detail::lowered(candidate.name).find("xbox") == std::string::npos &&
            detail::lowered(candidate.name).find("xinput") == std::string::npos) first = static_cast<int>(i);
    }
    if (first >= 0) { detail::read_device(static_cast<UINT>(first), sample); cached.store(first); }
    else detail::read_gamepad_sample(sample);
    return sample;
}

// True while this game's window has focus, so the radio does not fly the drone from the background.
inline bool game_in_foreground() {
    DWORD pid{};
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}
#endif
}
