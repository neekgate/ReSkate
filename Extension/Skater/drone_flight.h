#pragma once
// FPV drone flight model for the Freecam. Replaces step_free_flight() while drone mode is on.
//
// Matrix convention (same as free_flight.h): row-major 4x4, rows are right / up / backward,
// translation in elements 12..14. The camera looks along -backward. World up is +Y.
//
// Controls (all come from the existing FlightInput, so keyboard, mouse and controller work):
//   input.forward  -> pitch   (forward = nose down)
//   input.right    -> roll    (right = roll right)
//   input.up       -> throttle (0 = hover, + = climb, - = descend)
//   input.look_x   -> yaw     (mouse look delta, right = turn right)
//   input.look_y   -> extra pitch (mouse look delta)
//   input.boost    -> doubles the speed cap
#include "free_flight.h"
#include "drone_radio.h"
#include <atomic>
#include <cstdlib>
#include <chrono>
#include <functional>
#include <iterator>
#include <string_view>

namespace dingosdk::drone {

// Betaflight "Actual" rates for one axis: deg/s near the stick centre, deg/s at full stick, expo 0-1.
struct Rate { float center, max, expo; };

struct Settings {
    Rate roll{80.0f, 700.0f, 0.15f};
    Rate pitch{80.0f, 700.0f, 0.15f};
    Rate yaw{130.0f, 780.0f, 0.30f};
    float yaw_look_scale = 0.0025f; // rad per unit of mouse delta (matches freecam look)
    float hover_throttle = 0.35f;   // throttle at which thrust equals gravity
    float gravity = 9.81f;
    float lin_drag = 0.15f;         // 1/s
    float quad_drag = 0.02f;        // 1/unit
    float camera_uptilt_deg = 25.0f;
    bool angle_mode = false;        // true = self-levelling (easier), false = acro (full flips)
    float max_angle_deg = 55.0f;
    float angle_response = 8.0f;
    bool collide = true;            // collide with the world (needs the game's collision query)
    float body_radius = 0.3f;       // how close it gets to a surface before stopping
    float floor_depth = 1.5f;       // invisible floor this far below the start height (only without collisions); 0 = none
    bool park_at_zero = false;      // radio: throttle at the bottom freezes the drone instead of falling
    bool armed = true;              // false = motors off: no control, no thrust (the drone falls)
    bool disarm_freeze = false;     // true = a disarmed drone hangs in place instead of falling
};

struct State {
    bool valid = false;
    std::array<float, 16> body{};   // body orientation (translation unused); camera = body tilted up
    std::array<float, 3> velocity{};
    float start_y = 0;              // height the flight began at; the invisible floor hangs below it
    float flight_time = 0;          // seconds the motors have been armed since the flight began (the OSD timer)
    std::array<float, 3> start_pos{};   // where the flight began (`dronereset` returns here)
    std::array<float, 16> start_body{};
};

// What the OSD and the motor sound read (written by the flight tick, read from other threads).
struct Telemetry {
    std::atomic<float> throttle{0}, speed{0}, altitude{0}, seconds{0};
    std::atomic<bool> armed{true};
    std::atomic<float> stick_roll{0}, stick_pitch{0}, stick_yaw{0};   // -1..1, + = right / forward / right
    std::atomic<long long> last_ms{0};   // steady-clock milliseconds of the last flight tick
};
inline Telemetry& telemetry() { static Telemetry value; return value; }
inline long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
// True while the drone is being flown (a flight tick ran in the last quarter second).
inline bool flying() { return now_ms() - telemetry().last_ms.load(std::memory_order_relaxed) < 250; }

inline std::atomic<bool>& enabled() {
    static std::atomic<bool> value{false};
    return value;
}
inline bool is_enabled() { return enabled().load(std::memory_order_acquire); }
inline std::atomic<bool>& armed_flag() {
    static std::atomic<bool> value{true};
    return value;
}
inline bool armed() { return armed_flag().load(std::memory_order_acquire); }
inline void set_armed(bool on) { armed_flag().store(on, std::memory_order_release); }
// Asks the flight tick to put the drone back at its starting point and restart the timer and battery.
inline std::atomic<bool>& reset_flag() {
    static std::atomic<bool> value{false};
    return value;
}
inline void request_reset() { reset_flag().store(true, std::memory_order_release); }
inline void set_enabled(bool on) { enabled().store(on, std::memory_order_release); }

// Settings you can change while flying (`dronetune`). The index order matters: live_settings() uses it.
struct KnobInfo { const char* name; const char* help; float lo, hi, def; };
enum Knob : std::size_t { kGravity, kDrag, kHover, kRollCenter, kRollMax, kRollExpo, kPitchCenter, kPitchMax, kPitchExpo,
    kYawCenter, kYawMax, kYawExpo, kUptilt, kFloor, kPark, kCollide, kRadius, kOsd, kVolume,
    kOsdScale, kOsdColor, kOsdFont, kOsdMargin, kCrosshair, kShowArmed, kShowBattery, kShowTimer, kShowThrottle, kShowAlt,
    kShowSpeed, kSticks, kStickSize, kArmAxis, kResetAxis, kDisarmFreeze };
inline constexpr KnobInfo knob_info[] = {
    {"gravity", "downward pull", 0.0f, 50.0f, 9.81f},
    {"drag", "air drag: higher stops you sooner", 0.0f, 3.0f, 0.15f},
    {"hover", "throttle (0.1-0.9) that holds altitude; lower = more thrust", 0.1f, 0.9f, 0.35f},
    {"rollcenter", "Betaflight actual rates: roll centre sensitivity, deg/s", 1.0f, 1000.0f, 80.0f},
    {"rollmax", "roll max rate, deg/s", 30.0f, 2000.0f, 700.0f},
    {"rollexpo", "roll expo, 0-1", 0.0f, 1.0f, 0.15f},
    {"pitchcenter", "pitch centre sensitivity, deg/s", 1.0f, 1000.0f, 80.0f},
    {"pitchmax", "pitch max rate, deg/s", 30.0f, 2000.0f, 700.0f},
    {"pitchexpo", "pitch expo, 0-1", 0.0f, 1.0f, 0.15f},
    {"yawcenter", "yaw centre sensitivity, deg/s", 1.0f, 1000.0f, 130.0f},
    {"yawmax", "yaw max rate, deg/s", 30.0f, 2000.0f, 780.0f},
    {"yawexpo", "yaw expo, 0-1", 0.0f, 1.0f, 0.30f},
    {"uptilt", "camera tilt up in degrees (FPV cameras are about 25-40)", -30.0f, 70.0f, 25.0f},
    {"floor", "invisible floor, units below your start height; 0 = none", 0.0f, 1000.0f, 1.5f},
    {"park", "1 = radio throttle at the bottom freezes the drone, 0 = it falls", 0.0f, 1.0f, 0.0f},
    {"collide", "1 = hit the ground, walls and ramps, 0 = fly through everything", 0.0f, 1.0f, 1.0f},
    {"radius", "how close it gets to a surface before stopping", 0.05f, 3.0f, 0.3f},
    {"osd", "1 = show the flight display (battery, timer, throttle, altitude, speed)", 0.0f, 1.0f, 1.0f},
    {"volume", "motor sound volume, 0 = silent", 0.0f, 1.0f, 0.5f},
    {"osdscale", "OSD text size (1 = normal, 2 = double)", 0.4f, 5.0f, 1.5f},
    {"osdcolor", "OSD colour: 0 white, 1 green, 2 yellow, 3 cyan, 4 orange, 5 pink", 0.0f, 5.0f, 0.0f},
    {"osdfont", "OSD font: 0 = pixel (goggles style), 1 = smooth", 0.0f, 1.0f, 0.0f},
    {"osdmargin", "OSD distance from the screen edge, 0-0.3 of the screen", 0.0f, 0.3f, 0.06f},
    {"crosshair", "1 = centre crosshair", 0.0f, 1.0f, 0.0f},
    {"osdarmed", "1 = show ARMED", 0.0f, 1.0f, 1.0f},
    {"osdbattery", "1 = show battery voltage", 0.0f, 1.0f, 1.0f},
    {"osdtimer", "1 = show flight timer", 0.0f, 1.0f, 1.0f},
    {"osdthrottle", "1 = show throttle", 0.0f, 1.0f, 1.0f},
    {"osdalt", "1 = show altitude", 0.0f, 1.0f, 1.0f},
    {"osdspeed", "1 = show speed", 0.0f, 1.0f, 1.0f},
    {"sticks", "1 = show the stick overlay (mode 2 layout)", 0.0f, 1.0f, 1.0f},
    {"sticksize", "stick overlay size (1 = normal)", 0.4f, 3.0f, 1.0f},
    {"armaxis", "radio axis 1-6 for an arm switch (high = armed; minus flips it; 0 = none, use dronearm)", -6.0f, 6.0f, 0.0f},
    {"resetaxis", "radio axis 1-6 for a reset switch/button (flick it high to reset; 0 = none)", -6.0f, 6.0f, 0.0f},
    {"disarmfreeze", "1 = a disarmed drone hangs in place, 0 = it falls", 0.0f, 1.0f, 0.0f},
};
inline constexpr std::size_t knob_count = std::size(knob_info);
static_assert(knob_count == kDisarmFreeze + 1, "knob enum and table must match");
struct KnobValues {
    std::atomic<float> value[knob_count];
    KnobValues() { for (std::size_t i = 0; i < knob_count; ++i) value[i] = knob_info[i].def; }
};
inline KnobValues& knobs() { static KnobValues values; return values; }
inline int find_knob(std::string_view name) {
    for (std::size_t i = 0; i < knob_count; ++i) if (name == knob_info[i].name) return static_cast<int>(i);
    return -1;
}
inline Settings live_settings() {
    Settings s;
    auto get = [](Knob k) { return knobs().value[k].load(std::memory_order_relaxed); };
    s.gravity = get(kGravity); s.lin_drag = get(kDrag); s.hover_throttle = get(kHover);
    s.roll = {get(kRollCenter), get(kRollMax), get(kRollExpo)};
    s.pitch = {get(kPitchCenter), get(kPitchMax), get(kPitchExpo)};
    s.yaw = {get(kYawCenter), get(kYawMax), get(kYawExpo)};
    s.camera_uptilt_deg = get(kUptilt); s.floor_depth = get(kFloor); s.park_at_zero = get(kPark) >= 0.5f;
    s.collide = get(kCollide) >= 0.5f; s.body_radius = get(kRadius);
    s.armed = armed(); s.disarm_freeze = get(kDisarmFreeze) >= 0.5f;
    return s;
}

namespace detail {
using Vec3 = std::array<float, 3>;
constexpr float kDegToRad = 0.017453292519943295f;

inline Vec3 row(const std::array<float, 16>& m, int r) { return {m[r * 4], m[r * 4 + 1], m[r * 4 + 2]}; }
inline void set_row(std::array<float, 16>& m, int r, const Vec3& v) {
    m[r * 4] = v[0]; m[r * 4 + 1] = v[1]; m[r * 4 + 2] = v[2];
}
inline float dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
inline Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
inline Vec3 add(const Vec3& a, const Vec3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
inline Vec3 scale(const Vec3& a, float s) { return {a[0] * s, a[1] * s, a[2] * s}; }
inline Vec3 normalize(const Vec3& a) {
    const float len = std::sqrt(dot(a, a));
    return len > 1e-8f ? scale(a, 1.0f / len) : Vec3{0, 0, 0};
}
// Rodrigues rotation of v about unit axis by angle (right-handed).
inline Vec3 rotate(const Vec3& v, const Vec3& axis, float angle) {
    const float c = std::cos(angle), s = std::sin(angle);
    return add(add(scale(v, c), scale(cross(axis, v), s)), scale(axis, dot(axis, v) * (1 - c)));
}
// Rotate the whole basis about a world-space axis, then re-orthonormalise.
inline void rotate_basis(std::array<float, 16>& m, const Vec3& axis, float angle) {
    for (int r = 0; r < 3; ++r) set_row(m, r, rotate(row(m, r), axis, angle));
    Vec3 right = normalize(row(m, 0));
    Vec3 back = normalize(cross(right, row(m, 1)));   // right x up = backward
    Vec3 up = cross(back, right);
    set_row(m, 0, right); set_row(m, 1, up); set_row(m, 2, back);
}
// Betaflight's actual-rates curve: deg/s for a stick position -1..1.
inline float actual_rate(float stick, const Rate& r) {
    const float a = std::min(std::abs(stick), 1.0f);
    const float expo = std::clamp(r.expo, 0.0f, 1.0f);
    const float curve = a * (a * a * a * a * a * expo + a * (1.0f - expo));
    const float movement = std::max(0.0f, r.max - r.center);
    return std::copysign(r.center * a + movement * curve, stick);
}
inline float clamp1(float v) { return std::clamp(std::isfinite(v) ? v : 0.0f, -1.0f, 1.0f); }
inline std::array<float, 16> camera_from_body(const std::array<float, 16>& body, const std::array<float, 3>& position,
    float uptilt_rad) {
    auto cam = body;
    rotate_basis(cam, row(body, 0), uptilt_rad);  // + about the right axis = look up
    cam[12] = position[0]; cam[13] = position[1]; cam[14] = position[2]; cam[15] = body[15];
    return cam;
}
}

// Radio switches: an arm switch (high = armed) and a reset switch (flick high to reset), if mapped.
// Call once per tick with the raw axes (-1..1).
inline void apply_switches(const std::array<float, 6>& axes) {
    static bool reset_was_high = false;
    const auto axis = [&axes](Knob k) {
        const int a = static_cast<int>(std::lround(knobs().value[k].load(std::memory_order_relaxed)));
        return (a == 0 || std::abs(a) > 6) ? 0 : a;
    };
    if (const int a = axis(kArmAxis)) set_armed(radio::pick_axis(axes, a) > 0.0f);
    if (const int a = axis(kResetAxis)) {
        const bool high = radio::pick_axis(axes, a) > 0.0f;
        if (high && !reset_was_high) request_reset();
        reset_was_high = high;
    }
}

// The world the drone can bump into. `cast(from, to, hit)` returns true and the hit point when
// something solid lies on the line. Without it (or with collide off) the drone flies through everything.
struct World {
    std::function<bool(const std::array<float, 3>& from, const std::array<float, 3>& to, std::array<float, 3>& hit)> cast;
};

// What the pilot is asking for, whatever the input device.
struct Cmd {
    float pitch = 0;          // + = nose up
    float roll = 0;           // + = roll right
    float yaw_rad = 0;        // radians this step; negative = turn right
    float look_pitch_rad = 0; // extra pitch from the mouse (radians, + = look down)
    float throttle = 0.35f;   // 0..1 absolute
    bool boost = false;
    bool park = false;        // motors off: hold position
};

// One step of the flight model. `camera` is the pose published last tick (its translation is
// the drone position).
inline std::array<float, 16> step_drone_cmd(const std::array<float, 16>& camera, State& state, const Cmd& cmd_in,
    float seconds, float speed, const Settings& cfg = {}, const World* world = nullptr) {
    using namespace detail;
    if (!valid_flight_transform(camera) || !std::isfinite(seconds) || !std::isfinite(speed)) return camera;
    const float dt = std::clamp(seconds, 0.0f, 0.05f);
    const float uptilt = cfg.camera_uptilt_deg * kDegToRad;
    Vec3 position{camera[12], camera[13], camera[14]};

    if (!state.valid) {
        // Start from the current view with the FPV tilt removed, at rest.
        state.body = camera;
        rotate_basis(state.body, row(camera, 0), -uptilt);
        state.velocity = {0, 0, 0};
        state.start_y = position[1];
        state.flight_time = 0;
        state.start_pos = position;
        state.start_body = state.body;
        state.valid = true;
        reset_flag().store(false, std::memory_order_relaxed);
    }
    if (reset_flag().exchange(false, std::memory_order_acq_rel)) {
        state.body = state.start_body;
        state.velocity = {0, 0, 0};
        state.flight_time = 0;
        position = state.start_pos;
    }
    // Disarmed: the pilot's inputs and the motors are off.
    Cmd cmd = cmd_in;
    if (!cfg.armed) {
        cmd = Cmd{};
        cmd.throttle = 0.0f;
        cmd.park = cfg.disarm_freeze;
    }

    const float pitch_in = clamp1(cmd.pitch);
    const float roll_in = clamp1(cmd.roll);
    const float yaw_delta = std::isfinite(cmd.yaw_rad) ? cmd.yaw_rad : 0.0f;
    const float look_pitch = std::isfinite(cmd.look_pitch_rad) ? cmd.look_pitch_rad : 0.0f;

    // Attitude
    if (!cfg.angle_mode) {
        const float dp = (actual_rate(pitch_in, cfg.pitch) * kDegToRad) * dt - look_pitch;
        const float dr = (actual_rate(roll_in, cfg.roll) * kDegToRad) * dt;
        rotate_basis(state.body, row(state.body, 1), yaw_delta);        // yaw about body up
        rotate_basis(state.body, row(state.body, 0), dp);               // pitch about body right
        rotate_basis(state.body, scale(row(state.body, 2), -1.0f), dr); // roll about body forward
    } else {
        // Heading from yaw input; tilt eases toward the commanded angle.
        const Vec3 fwd = scale(row(state.body, 2), -1.0f);
        const float heading = std::atan2(fwd[0], -fwd[2]);
        const float new_heading = heading - yaw_delta;
        const float max_a = cfg.max_angle_deg * kDegToRad;
        const float k = 1 - std::exp(-cfg.angle_response * dt);
        const float cur_pitch = std::asin(std::clamp(fwd[1], -1.0f, 1.0f));
        const float cur_roll = std::asin(std::clamp(row(state.body, 0)[1], -1.0f, 1.0f)) * -1.0f;
        const float want_pitch = pitch_in * max_a, want_roll = roll_in * max_a;
        const float p = cur_pitch + (want_pitch - cur_pitch) * k - look_pitch;
        const float r = cur_roll + (want_roll - cur_roll) * k;
        Vec3 f{std::sin(new_heading), 0, -std::cos(new_heading)};
        Vec3 right{-f[2], 0, f[0]};
        Vec3 up{0, 1, 0};
        std::array<float, 16> m = state.body;
        set_row(m, 0, right); set_row(m, 1, up); set_row(m, 2, scale(f, -1.0f));
        rotate_basis(m, right, p);
        rotate_basis(m, scale(row(m, 2), -1.0f), r);
        state.body = m;
    }

    // Translation
    Vec3 next_pos = position;
    if (cmd.park) {
        state.velocity = {0, 0, 0};
    } else {
        const Vec3 up = row(state.body, 1);
        const float thrust = std::clamp(cmd.throttle, 0.0f, 1.0f) * (cfg.gravity / cfg.hover_throttle);
        const float spd = std::sqrt(dot(state.velocity, state.velocity));
        const float drag = cfg.lin_drag + cfg.quad_drag * spd;
        Vec3 accel{up[0] * thrust - state.velocity[0] * drag,
                   up[1] * thrust - cfg.gravity - state.velocity[1] * drag,
                   up[2] * thrust - state.velocity[2] * drag};
        for (int i = 0; i < 3; ++i) state.velocity[i] += accel[i] * dt;
        const float cap = std::clamp(speed, 0.6f, 1500.0f) * 4.0f * (cmd.boost ? 2.0f : 1.0f);
        const float now = std::sqrt(dot(state.velocity, state.velocity));
        if (now > cap) for (auto& v : state.velocity) v *= cap / now;
        next_pos = {position[0] + state.velocity[0] * dt, position[1] + state.velocity[1] * dt,
                    position[2] + state.velocity[2] * dt};
        if (world && world->cast && cfg.collide) {
            // Swept move, one axis at a time (up/down first so landing works), so it slides along
            // walls and rests on the ground. The game's query gives hit points, not surface normals.
            Vec3 p = position;
            const Vec3 d{next_pos[0] - position[0], next_pos[1] - position[1], next_pos[2] - position[2]};
            const float r = std::max(cfg.body_radius, 0.01f);
            for (const int axis : {1, 0, 2}) {
                const float move = d[static_cast<std::size_t>(axis)];
                if (move == 0.0f) continue;
                const auto a = static_cast<std::size_t>(axis);
                const float sign = move > 0 ? 1.0f : -1.0f;
                Vec3 to = p;
                to[a] += move + sign * r;
                Vec3 hit{};
                if (world->cast(p, to, hit)) {
                    const float allowed = (hit[a] - p[a]) - sign * r;
                    p[a] += sign > 0 ? std::clamp(allowed, 0.0f, move) : std::clamp(allowed, move, 0.0f);
                    state.velocity[a] = 0;
                    if (axis == 1 && sign < 0) {   // landed: skid to a stop
                        const float grip = std::exp(-6.0f * dt);
                        state.velocity[0] *= grip;
                        state.velocity[2] *= grip;
                    }
                } else {
                    p[a] += move;
                }
            }
            next_pos = p;
            // Safety net if the collision world is not streamed in yet: do not fall forever.
            const float deep = state.start_y - 300.0f;
            if (next_pos[1] < deep) { next_pos[1] = deep; if (state.velocity[1] < 0) state.velocity[1] = 0; }
        } else if (cfg.floor_depth > 0.0f) {
            // No collisions: an invisible floor so it lands instead of falling through the map.
            const float floor_y = state.start_y - cfg.floor_depth;
            if (next_pos[1] < floor_y) {
                next_pos[1] = floor_y;
                if (state.velocity[1] < 0) state.velocity[1] = 0;
                const float grip = std::exp(-6.0f * dt);
                state.velocity[0] *= grip;
                state.velocity[2] *= grip;
            }
        }
    }

    const auto out = camera_from_body(state.body, next_pos, uptilt);
    if (!valid_flight_transform(out)) { state.valid = false; return camera; }
    if (cfg.armed) state.flight_time += dt;
    auto& t = telemetry();
    t.throttle.store(cmd.park ? 0.0f : std::clamp(cmd.throttle, 0.0f, 1.0f), std::memory_order_relaxed);
    t.speed.store(std::sqrt(dot(state.velocity, state.velocity)), std::memory_order_relaxed);
    t.altitude.store(next_pos[1] - state.start_y, std::memory_order_relaxed);
    t.seconds.store(state.flight_time, std::memory_order_relaxed);
    t.armed.store(cfg.armed, std::memory_order_relaxed);
    t.last_ms.store(now_ms(), std::memory_order_relaxed);
    return out;
}

// Keyboard / mouse / game-controller input (the Freecam's own FlightInput).
inline std::array<float, 16> step_drone_flight(const std::array<float, 16>& camera, State& state,
    const overlay::FlightInput& input, float seconds, float speed, const Settings& cfg = {},
    const World* world = nullptr) {
    using namespace detail;
    if (!input.active || !std::isfinite(input.look_x) || !std::isfinite(input.look_y)) return camera;
    Cmd cmd;
    cmd.pitch = clamp1(-input.forward);   // forward stick = nose down
    cmd.roll = clamp1(input.right);
    cmd.yaw_rad = -std::clamp(input.look_x, -250.0f, 250.0f) * cfg.yaw_look_scale;  // right = negative about up
    cmd.look_pitch_rad = std::clamp(input.look_y, -250.0f, 250.0f) * cfg.yaw_look_scale;
    // Keyboard has no absolute throttle: neutral hovers, Q/E climb or descend.
    const float up_in = clamp1(input.up);
    cmd.throttle = up_in >= 0 ? cfg.hover_throttle + up_in * (1.0f - cfg.hover_throttle)
                              : cfg.hover_throttle + up_in * cfg.hover_throttle;
    cmd.boost = input.boost;
    {
        auto& t = telemetry();
        t.stick_roll.store(clamp1(input.right), std::memory_order_relaxed);
        t.stick_pitch.store(clamp1(input.forward), std::memory_order_relaxed);
        t.stick_yaw.store(clamp1(input.look_x / 40.0f), std::memory_order_relaxed);
    }
    return step_drone_cmd(camera, state, cmd, seconds, speed, cfg, world);
}

// Radio controller: sticks drive rates directly and the throttle stick is absolute thrust. With the
// `park` setting on, a throttle at the bottom freezes the drone; otherwise it falls and lands on the
// invisible floor.
inline std::array<float, 16> step_drone_radio(const std::array<float, 16>& camera, State& state,
    const radio::Command& sticks, float seconds, float speed, const Settings& cfg = {}, const World* world = nullptr) {
    using namespace detail;
    if (!sticks.valid) return camera;
    const float dt = std::clamp(std::isfinite(seconds) ? seconds : 0.0f, 0.0f, 0.05f);
    Cmd cmd;
    cmd.pitch = clamp1(-sticks.pitch);    // stick forward = nose down
    cmd.roll = clamp1(sticks.roll);
    cmd.yaw_rad = -actual_rate(clamp1(sticks.yaw), cfg.yaw) * kDegToRad * dt;
    cmd.throttle = std::clamp(sticks.throttle, 0.0f, 1.0f);
    cmd.park = cfg.park_at_zero && sticks.throttle <= 0.03f;
    {
        auto& t = telemetry();
        t.stick_roll.store(clamp1(sticks.roll), std::memory_order_relaxed);
        t.stick_pitch.store(clamp1(sticks.pitch), std::memory_order_relaxed);
        t.stick_yaw.store(clamp1(sticks.yaw), std::memory_order_relaxed);
    }
    return step_drone_cmd(camera, state, cmd, seconds, speed, cfg, world);
}
}
