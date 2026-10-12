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

namespace dingosdk::drone {

struct Settings {
    float pitch_rate_deg = 600.0f;  // deg/s at full deflection
    float roll_rate_deg = 600.0f;
    float yaw_look_scale = 0.0025f; // rad per unit of mouse delta (matches freecam look)
    float yaw_rate_deg = 250.0f;    // deg/s from stick yaw (unused by keyboard/mouse)
    float expo = 0.55f;             // 0 = linear, 1 = soft around centre
    float hover_throttle = 0.35f;   // throttle at which thrust equals gravity
    float gravity = 9.81f;
    float lin_drag = 0.15f;         // 1/s
    float quad_drag = 0.02f;        // 1/unit
    float camera_uptilt_deg = 25.0f;
    bool angle_mode = false;        // true = self-levelling (easier), false = acro (full flips)
    float max_angle_deg = 55.0f;
    float angle_response = 8.0f;
};

struct State {
    bool valid = false;
    std::array<float, 16> body{};   // body orientation (translation unused); camera = body tilted up
    std::array<float, 3> velocity{};
};

inline std::atomic<bool>& enabled() {
    static std::atomic<bool> value{false};
    return value;
}
inline bool is_enabled() { return enabled().load(std::memory_order_acquire); }
inline void set_enabled(bool on) { enabled().store(on, std::memory_order_release); }

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
inline float expo_curve(float s, float e) { return e * s * s * s + (1 - e) * s; }
inline float clamp1(float v) { return std::clamp(std::isfinite(v) ? v : 0.0f, -1.0f, 1.0f); }
inline std::array<float, 16> camera_from_body(const std::array<float, 16>& body, const std::array<float, 3>& position,
    float uptilt_rad) {
    auto cam = body;
    rotate_basis(cam, row(body, 0), uptilt_rad);  // + about the right axis = look up
    cam[12] = position[0]; cam[13] = position[1]; cam[14] = position[2]; cam[15] = body[15];
    return cam;
}
}

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
inline std::array<float, 16> step_drone_cmd(const std::array<float, 16>& camera, State& state, const Cmd& cmd,
    float seconds, float speed, const Settings& cfg = {}) {
    using namespace detail;
    if (!valid_flight_transform(camera) || !std::isfinite(seconds) || !std::isfinite(speed)) return camera;
    const float dt = std::clamp(seconds, 0.0f, 0.05f);
    const float uptilt = cfg.camera_uptilt_deg * kDegToRad;
    const Vec3 position{camera[12], camera[13], camera[14]};

    if (!state.valid) {
        // Start from the current view with the FPV tilt removed, at rest.
        state.body = camera;
        rotate_basis(state.body, row(camera, 0), -uptilt);
        state.velocity = {0, 0, 0};
        state.valid = true;
    }

    const float pitch_in = clamp1(cmd.pitch);
    const float roll_in = clamp1(cmd.roll);
    const float yaw_delta = std::isfinite(cmd.yaw_rad) ? cmd.yaw_rad : 0.0f;
    const float look_pitch = std::isfinite(cmd.look_pitch_rad) ? cmd.look_pitch_rad : 0.0f;

    // Attitude
    if (!cfg.angle_mode) {
        const float dp = (cfg.pitch_rate_deg * kDegToRad * expo_curve(pitch_in, cfg.expo)) * dt - look_pitch;
        const float dr = (cfg.roll_rate_deg * kDegToRad * expo_curve(roll_in, cfg.expo)) * dt;
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
    }

    const auto out = camera_from_body(state.body, next_pos, uptilt);
    if (!valid_flight_transform(out)) { state.valid = false; return camera; }
    return out;
}

// Keyboard / mouse / game-controller input (the Freecam's own FlightInput).
inline std::array<float, 16> step_drone_flight(const std::array<float, 16>& camera, State& state,
    const overlay::FlightInput& input, float seconds, float speed, const Settings& cfg = {}) {
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
    return step_drone_cmd(camera, state, cmd, seconds, speed, cfg);
}

// Radio controller: sticks drive rates directly, the throttle stick is absolute thrust, and a
// throttle at the bottom parks the drone (motors off) so it does not fall through the map.
inline std::array<float, 16> step_drone_radio(const std::array<float, 16>& camera, State& state,
    const radio::Command& sticks, float seconds, float speed, const Settings& cfg = {}) {
    using namespace detail;
    if (!sticks.valid) return camera;
    const float dt = std::clamp(std::isfinite(seconds) ? seconds : 0.0f, 0.0f, 0.05f);
    Cmd cmd;
    cmd.pitch = clamp1(-sticks.pitch);    // stick forward = nose down
    cmd.roll = clamp1(sticks.roll);
    cmd.yaw_rad = -expo_curve(clamp1(sticks.yaw), cfg.expo) * cfg.yaw_rate_deg * kDegToRad * dt;
    cmd.throttle = std::clamp(sticks.throttle, 0.0f, 1.0f);
    cmd.park = sticks.throttle <= 0.03f;
    return step_drone_cmd(camera, state, cmd, seconds, speed, cfg);
}
}
