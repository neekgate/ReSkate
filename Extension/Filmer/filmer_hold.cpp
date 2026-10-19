#include "filmer_hold.h"
#include <algorithm>
#include <cmath>

namespace dingosdk::filmer::hold {
namespace {
Vec3 add(const Vec3 &a, const Vec3 &b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
Vec3 sub(const Vec3 &a, const Vec3 &b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
Vec3 scaled(const Vec3 &a, float k) { return {a[0] * k, a[1] * k, a[2] * k}; }
float dot(const Vec3 &a, const Vec3 &b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Vec3 cross(const Vec3 &a, const Vec3 &b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
float length(const Vec3 &a) { return std::sqrt(dot(a, a)); }
Vec3 unit(const Vec3 &a, const Vec3 &fallback) {
    const float l = length(a);
    return l > 1e-6f && std::isfinite(l) ? scaled(a, 1.0f / l) : fallback;
}
bool finite(const Vec3 &v) { return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]); }
bool finite(const Quat &q) { return std::isfinite(q[0]) && std::isfinite(q[1]) && std::isfinite(q[2]) && std::isfinite(q[3]); }

// The arm's joints in a chain: chest, shoulder, upper arm, forearm, hand.
constexpr std::size_t chest = 6, shoulder = 7, upper = 8, fore = 9, hand_at = 10;
static_assert(right_chain.size() == hand_at + 1 && right_chain[chest] == chest_joint && left_chain[chest] == chest_joint);

Held place_camera(const Vec3 &elbow, const Vec3 &wrist, const Quat &aim, float scale) {
    Held held;
    const Vec3 along = unit(sub(wrist, elbow), {0, -1, 0});
    held.palm.position = add(wrist, scaled(along, palm_from_wrist * scale));
    held.palm.rotation = normalized(aim);
    held.palm.scale = 1.0f;
    held.lens = add(held.palm.position, rotate(held.palm.rotation, lens_from_handle));
    held.valid = finite(held.palm.position) && finite(held.lens);
    return held;
}
} // namespace

Quat multiply(const Quat &a, const Quat &b) noexcept {
    return {a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
            a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
            a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
            a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
}
Quat conjugate(const Quat &q) noexcept { return {-q[0], -q[1], -q[2], q[3]}; }
Vec3 rotate(const Quat &q, const Vec3 &v) noexcept {
    const Vec3 u{q[0], q[1], q[2]};
    const Vec3 t = scaled(cross(u, v), 2.0f);
    return add(add(v, scaled(t, q[3])), cross(u, t));
}
Quat normalized(const Quat &q) noexcept {
    const float l = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (!(l > 1e-6f) || !std::isfinite(l)) return {0, 0, 0, 1};
    return {q[0] / l, q[1] / l, q[2] / l, q[3] / l};
}
Quat arc(const Vec3 &from, const Vec3 &to) noexcept {
    const Vec3 a = unit(from, {0, 0, 1}), b = unit(to, {0, 0, 1});
    const float d = dot(a, b);
    if (d < -0.9999f) {
        // Half a turn: about any axis square to `from`.
        Vec3 axis = cross({1, 0, 0}, a);
        if (length(axis) < 1e-3f) axis = cross({0, 1, 0}, a);
        axis = unit(axis, {0, 1, 0});
        return {axis[0], axis[1], axis[2], 0.0f};
    }
    const Vec3 c = cross(a, b);
    return normalized({c[0], c[1], c[2], 1.0f + d});
}
Quat blend(const Quat &a, Quat b, float t) noexcept {
    t = std::clamp(t, 0.0f, 1.0f);
    if (a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3] < 0.0f)
        for (auto &v : b) v = -v;
    return normalized({a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t, a[3] + (b[3] - a[3]) * t});
}
Quat from_rows(const std::array<float, 16> &m) noexcept {
    const float m00 = m[0], m01 = m[1], m02 = m[2], m10 = m[4], m11 = m[5], m12 = m[6], m20 = m[8], m21 = m[9], m22 = m[10];
    Quat q{};
    const float trace = m00 + m11 + m22;
    if (trace > 0.0f) {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        q = {(m12 - m21) / s, (m20 - m02) / s, (m01 - m10) / s, 0.25f * s};
    } else if (m00 > m11 && m00 > m22) {
        const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
        q = {0.25f * s, (m01 + m10) / s, (m20 + m02) / s, (m12 - m21) / s};
    } else if (m11 > m22) {
        const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
        q = {(m01 + m10) / s, 0.25f * s, (m12 + m21) / s, (m20 - m02) / s};
    } else {
        const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
        q = {(m20 + m02) / s, (m12 + m21) / s, 0.25f * s, (m01 - m10) / s};
    }
    return normalized(q);
}
Quat from_yaw_pitch(float yaw, float pitch) noexcept {
    const Vec3 forward{std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
    const Vec3 back = scaled(forward, -1.0f);
    const Vec3 right = unit(cross({0, 1, 0}, back), {1, 0, 0});
    const Vec3 up = cross(back, right);
    std::array<float, 16> m{};
    for (int i = 0; i < 3; ++i) {
        m[i] = right[i];
        m[4 + i] = up[i];
        m[8 + i] = back[i];
    }
    m[15] = 1.0f;
    return from_rows(m);
}

bool compose(std::span<const Bone> pose, std::span<const std::uint16_t> joints, std::span<Placed> out) noexcept {
    if (out.size() < joints.size()) return false;
    Placed at;
    for (std::size_t i = 0; i < joints.size(); ++i) {
        if (joints[i] >= pose.size()) return false;
        const auto &bone = pose[joints[i]];
        if (!finite(bone.position) || !finite(bone.rotation) || !finite(bone.scale)) return false;
        Placed next;
        next.position = add(at.position, rotate(at.rotation, scaled(bone.position, at.scale)));
        next.rotation = normalized(multiply(at.rotation, bone.rotation));
        next.scale = at.scale * (bone.scale[0] + bone.scale[1] + bone.scale[2]) / 3.0f;
        if (!finite(next.position) || !(std::abs(next.position[0]) < 1e6f && std::abs(next.position[1]) < 1e6f && std::abs(next.position[2]) < 1e6f))
            return false;
        out[i] = next;
        at = next;
    }
    return true;
}

Held hold(std::span<Bone> pose, const Goal &goal) noexcept {
    const auto joints = chain(goal.hand);
    std::array<Placed, right_chain.size()> w{};
    if (!compose(pose, joints, w) || !finite(goal.lens) || !finite(goal.aim)) return {};
    const Quat aim = normalized(goal.aim);
    const Vec3 s = w[upper].position, e = w[fore].position, h = w[hand_at].position;
    const float a = length(sub(e, s)), b = length(sub(h, e));
    if (!(a > 0.05f && b > 0.05f && a < 1.0f && b < 1.0f)) return {};
    const float scale = std::max(w[hand_at].scale, 0.1f);
    // Where the palm has to be for the lens to be at the goal. The palm lies on the forearm's line, past
    // the wrist, so the forearm and hand are one straight reach of b + palm.
    const Vec3 palm_goal = sub(goal.lens, rotate(aim, lens_from_handle));
    const float palm = palm_from_wrist * scale, b_palm = b + palm;
    const float reach = (a + b_palm) * 0.995f;
    const float wanted = length(sub(palm_goal, s));
    const float d = std::clamp(wanted, std::abs(a - b_palm) + 0.02f, reach);
    const Vec3 direction = unit(sub(palm_goal, s), {0, -1, 0});
    const Vec3 target = add(s, scaled(direction, d));
    // The elbow out from the body and a little down and back: the shoulder's side of the chest.
    Vec3 side = sub(s, w[chest].position);
    side[1] = 0.0f;
    side = unit(side, {goal.hand == Hand::left ? -1.0f : 1.0f, 0, 0});
    Vec3 ahead{direction[0], 0.0f, direction[2]};
    ahead = unit(ahead, cross(side, {0, 1, 0}));
    Vec3 pole = add(add(scaled(side, 0.8f), Vec3{0.0f, -0.45f, 0.0f}), scaled(ahead, -0.35f));
    pole = sub(pole, scaled(direction, dot(pole, direction)));
    pole = unit(pole, side);
    const float cos_a = std::clamp((a * a + d * d - b_palm * b_palm) / (2.0f * a * d), -1.0f, 1.0f);
    const float sin_a = std::sqrt(std::max(0.0f, 1.0f - cos_a * cos_a));
    const Vec3 elbow = add(s, add(scaled(direction, a * cos_a), scaled(pole, a * sin_a)));
    const Vec3 wrist = add(elbow, scaled(unit(sub(target, elbow), direction), b));
    // The upper arm turned onto the new elbow, then the forearm (as the upper arm left it) onto the wrist.
    const Quat turn_upper = arc(sub(e, s), sub(elbow, s));
    const Quat upper_world = normalized(multiply(turn_upper, w[upper].rotation));
    const Quat turn_fore = arc(rotate(turn_upper, sub(h, e)), sub(wrist, elbow));
    const Quat fore_world = normalized(multiply(turn_fore, multiply(turn_upper, w[fore].rotation)));
    // Back to each joint's own rotation, part way by the weight.
    const float weight = std::clamp(goal.weight, 0.0f, 1.0f);
    auto &upper_bone = pose[joints[upper]];
    auto &fore_bone = pose[joints[fore]];
    const Quat upper_local = normalized(multiply(conjugate(w[shoulder].rotation), upper_world));
    const Quat fore_local = normalized(multiply(conjugate(upper_world), fore_world));
    upper_bone.rotation = blend(upper_bone.rotation, upper_local, weight);
    fore_bone.rotation = blend(fore_bone.rotation, fore_local, weight);
    // Where the hand ended up, from the pose as it now is.
    if (!compose(pose, joints, w)) return {};
    auto held = place_camera(w[fore].position, w[hand_at].position, aim, scale);
    held.reached = wanted <= reach + 0.01f;
    return held;
}

Held held_in(std::span<const Bone> pose, Hand hand, const Quat &aim) noexcept {
    std::array<Placed, right_chain.size()> w{};
    if (!compose(pose, chain(hand), w) || !finite(aim)) return {};
    return place_camera(w[fore].position, w[hand_at].position, aim, std::max(w[hand_at].scale, 0.1f));
}
} // namespace dingosdk::filmer::hold
