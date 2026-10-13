#pragma once
// USB radio-controller input for the FPV drone (RadioMaster, Jumper, FrSky... anything that
// shows up in Windows' "Set up USB game controllers" when its USB mode is Joystick).
//
// The pure maths (normalise / dead-zone / axis mapping) has no Windows dependency so it can be
// unit-tested anywhere. Only poll() and game_in_foreground() need Windows (winmm).
//
// Axis numbers 1..6 are winmm's X, Y, Z, R, U, V. `droneradiocheck` prints all six so you can see
// which one each stick moves; `droneaxes roll pitch throttle yaw` maps them (negative flips).
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <string>
#ifdef _WIN32
#include <Windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#endif

namespace dingosdk::drone::radio {

// Mode 2 on an EdgeTX radio sends AETR: channel 1 aileron (roll), 2 elevator (pitch),
// 3 throttle, 4 rudder (yaw). Those are the defaults below: roll=1, pitch=2, throttle=3, yaw=4.
struct Config {
    std::atomic<bool> enabled{false};
    std::atomic<int> map[4];   // roll, pitch, throttle, yaw -> signed axis number (1..6)
    std::atomic<int> device{-1}; // -1 = pick automatically
    Config() { for (int i = 0; i < 4; ++i) map[i] = i + 1; }
};
inline Config& config() { static Config value; return value; }
inline std::array<int, 4> mapping_snapshot() {
    auto& c = config();
    return {c.map[0].load(), c.map[1].load(), c.map[2].load(), c.map[3].load()};
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
inline Command map_axes(const std::array<float, 6>& axes, const std::array<int, 4>& map, float dz = 0.03f) {
    Command out;
    out.valid = true;
    out.roll = deadzone(pick_axis(axes, map[0]), dz);
    out.pitch = deadzone(pick_axis(axes, map[1]), dz);
    out.throttle = std::clamp((pick_axis(axes, map[2]) + 1.0f) * 0.5f, 0.0f, 1.0f);
    out.yaw = deadzone(pick_axis(axes, map[3]), dz);
    return out;
}

struct Sample {
    bool valid = false;
    std::array<float, 6> axes{};
    std::uint32_t buttons = 0;   // bit n-1 set = button n pressed
    std::string name;
    int device = -1;
};

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
}

// Reads the radio. Safe to call from any thread; rescans for a controller at most once a second.
inline Sample poll() {
    static std::atomic<int> cached{-1};
    static std::atomic<ULONGLONG> next_scan{0};
    Sample sample;
    const int forced = config().device.load();
    if (forced >= 0) { detail::read_device(static_cast<UINT>(forced), sample); return sample; }
    int id = cached.load();
    if (id >= 0 && detail::read_device(static_cast<UINT>(id), sample)) return sample;
    cached.store(-1);
    const auto now = GetTickCount64();
    if (now < next_scan.load()) return sample;
    next_scan.store(now + 1000);
    const UINT count = std::min<UINT>(joyGetNumDevs(), 16);
    int first = -1;
    for (UINT i = 0; i < count; ++i) {
        Sample candidate;
        if (!detail::read_device(i, candidate)) continue;
        if (first < 0) first = static_cast<int>(i);
        if (detail::lowered(candidate.name).find("radio") != std::string::npos) { cached.store(static_cast<int>(i)); return candidate; }
    }
    if (first >= 0) { detail::read_device(static_cast<UINT>(first), sample); cached.store(first); }
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
