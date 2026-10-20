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
// The world turn `world` as the local rotation of a joint whose parent is placed at `parent`.
Quat local_of(const Placed &parent, const Quat &world) { return normalized(multiply(conjugate(parent.rotation), world)); }
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

bool crouch(std::span<Bone> pose, float depth) noexcept {
    if (!(depth > 0.001f) || !std::isfinite(depth)) return true;
    depth = std::min(depth, 0.6f);
    struct Leg {
        std::span<const std::uint16_t> path;
        std::array<Placed, 6> before{};
    };
    std::array<Leg, 2> legs{{{right_leg}, {left_leg}}};
    for (auto &leg : legs)
        if (!compose(pose, leg.path, leg.before)) return false;
    // The hips down, in their parent's frame (the skater's placement).
    const auto &parent = legs[0].before[1];
    auto &hips = pose[hips_joint];
    const Vec3 drop = rotate(conjugate(parent.rotation), Vec3{0.0f, -depth, 0.0f});
    const float s = std::max(parent.scale, 0.1f);
    hips.position = add(hips.position, scaled(drop, 1.0f / s));
    for (auto &leg : legs) {
        std::array<Placed, 6> now{};
        if (!compose(pose, leg.path, now)) return false;
        const Vec3 hip = now[3].position, knee = leg.before[4].position, ankle = leg.before[5].position;
        const float a = length(sub(leg.before[4].position, leg.before[3].position)), b = length(sub(ankle, knee));
        if (!(a > 0.1f && b > 0.1f)) continue;
        const float d = std::clamp(length(sub(ankle, hip)), std::abs(a - b) + 0.01f, (a + b) * 0.999f);
        const Vec3 along = unit(sub(ankle, hip), {0, -1, 0});
        // The knee bends the way it already does (forward of the hip-to-ankle line).
        const Vec3 old_mid = add(leg.before[3].position, scaled(unit(sub(ankle, leg.before[3].position), along),
                                                                 dot(sub(knee, leg.before[3].position), unit(sub(ankle, leg.before[3].position), along))));
        Vec3 pole = sub(knee, old_mid);
        pole = sub(pole, scaled(along, dot(pole, along)));
        pole = unit(pole, Vec3{0.0f, 0.0f, 1.0f});
        const float cos_a = std::clamp((a * a + d * d - b * b) / (2.0f * a * d), -1.0f, 1.0f);
        const float sin_a = std::sqrt(std::max(0.0f, 1.0f - cos_a * cos_a));
        const Vec3 new_knee = add(hip, add(scaled(along, a * cos_a), scaled(pole, a * sin_a)));
        // Thigh, then shin, turned onto the new knee and the ankle; the foot keeps its turn in the world.
        const Quat thigh_world = normalized(multiply(arc(sub(now[4].position, hip), sub(new_knee, hip)), now[3].rotation));
        auto &thigh = pose[leg.path[3]];
        thigh.rotation = local_of(now[2], thigh_world);
        if (!compose(pose, leg.path, now)) return false;
        const Quat shin_world = normalized(multiply(arc(sub(now[5].position, now[4].position), sub(ankle, now[4].position)), now[4].rotation));
        pose[leg.path[4]].rotation = local_of(now[3], shin_world);
        if (!compose(pose, leg.path, now)) return false;
        pose[leg.path[5]].rotation = local_of(now[4], leg.before[5].rotation);
    }
    return true;
}

bool hunch(std::span<Bone> pose, const Vec3 &toward, float angle, float look, float twist) noexcept {
    if (!(std::abs(angle) + std::abs(look) + std::abs(twist) > 1e-4f) || !finite(toward)) return true;
    Vec3 level{toward[0], 0.0f, toward[2]};
    if (length(level) < 1e-4f) return true;
    level = unit(level, {0, 0, 1});
    // Turning about this bends the top of the body over toward `level`.
    const Vec3 axis = unit(cross(Vec3{0.0f, 1.0f, 0.0f}, level), {1, 0, 0});
    // Spine, Spine1, Spine2, Spine3 share the back's angle; Neck, Neck1 and Head the look.
    constexpr std::array<float, 7> share{0.15f, 0.25f, 0.3f, 0.3f, 0.35f, 0.25f, 0.4f};
    constexpr std::array<float, 7> twist_share{0.05f, 0.05f, 0.1f, 0.1f, 0.2f, 0.2f, 0.3f};
    for (std::size_t i = 0; i < hunched_joints.size(); ++i) {
        std::array<Placed, head_chain.size()> w{};
        if (!compose(pose, head_chain, w)) return false;
        const std::size_t at = 3 + i; // its place in head_chain
        const float turn = (i < 4 ? angle : look) * share[i];
        const Quat bend{axis[0] * std::sin(turn / 2), axis[1] * std::sin(turn / 2), axis[2] * std::sin(turn / 2), std::cos(turn / 2)};
        const float round = twist * twist_share[i];
        const Quat about_up{0.0f, std::sin(round / 2), 0.0f, std::cos(round / 2)};
        const Quat world = normalized(multiply(bend, multiply(about_up, w[at].rotation)));
        pose[hunched_joints[i]].rotation = local_of(w[at - 1], world);
    }
    return true;
}

namespace {
Held hold_one(std::span<Bone> pose, const Goal &goal) noexcept;
}

Held hold(std::span<Bone> pose, const Goal &goal) noexcept {
    auto held = hold_one(pose, goal);
    if (!held.valid || !goal.two_hands) return held;
    // The other hand to the camera's side, underneath, where the first one put it.
    Vec3 support = support_from_grip;
    if (goal.hand == Hand::left) support[0] = -support[0];
    const Vec3 point = add(held.palm.position, rotate(held.palm.rotation, support));
    Goal other;
    other.hand = goal.hand == Hand::left ? Hand::right : Hand::left;
    other.aim = normalized(goal.aim);
    other.weight = goal.weight;
    other.free_reach = true;
    other.lens = add(point, rotate(other.aim, lens_from_handle)); // so its palm goes to `point`
    (void)hold_one(pose, other);
    return held;
}

namespace {
Held hold_one(std::span<Bone> pose, const Goal &goal) noexcept {
    const auto joints = chain(goal.hand);
    std::array<Placed, right_chain.size()> w{};
    if (!finite(goal.lens) || !finite(goal.aim)) return {};
    const float body_weight = std::clamp(goal.weight, 0.0f, 1.0f);
    if (goal.crouch > 0.0f && !crouch(pose, goal.crouch * body_weight)) return {};
    if (goal.hunch > 0.0f) {
        // Over the camera in the hand, the way the body faces: toward the aim, but no more than 60 degrees
        // off the body's own facing (never folded backwards), and further the lower it points.
        std::array<Placed, 6> right_thigh{}, left_thigh{};
        if (!compose(pose, right_leg, right_thigh) || !compose(pose, left_leg, left_thigh)) return {};
        Vec3 across = sub(left_thigh[3].position, right_thigh[3].position);
        across[1] = 0.0f;
        const Vec3 facing = unit(cross(across, {0.0f, 1.0f, 0.0f}), {0, 0, 1});
        const Vec3 pointing = rotate(normalized(goal.aim), {0.0f, 0.0f, -1.0f});
        const Vec3 level = unit(Vec3{pointing[0], 0.0f, pointing[2]}, facing);
        const float full = std::atan2(dot(cross(facing, level), {0.0f, 1.0f, 0.0f}), dot(facing, level));
        const float off = std::clamp(full, -1.05f, 1.05f);
        const Quat turn{0.0f, std::sin(off / 2), 0.0f, std::cos(off / 2)};
        const Vec3 toward = rotate(turn, facing);
        const float pitch = std::asin(std::clamp(pointing[1], -1.0f, 1.0f)); // down is negative
        // The back no more than about 57 degrees over, the head down at most about 34 more: it stays a
        // filmer looking down at the camera, never folded past it.
        const float amount = std::min(goal.hunch * std::clamp(1.0f - pitch * 1.2f, 0.3f, 1.6f), 1.0f) * body_weight;
        const float look = std::min(amount * 0.8f, 0.6f * body_weight);
        // The head turns to look at the camera off to the side (the back a little with it).
        if (!hunch(pose, toward, amount, look, std::clamp(full, -1.6f, 1.6f) * body_weight)) return {};
    }
    if (!compose(pose, joints, w)) return {};
    const Quat aim = normalized(goal.aim);
    const Vec3 s = w[upper].position, e = w[fore].position, h = w[hand_at].position;
    const float a = length(sub(e, s)), b = length(sub(h, e));
    if (!(a > 0.05f && b > 0.05f && a < 1.0f && b < 1.0f)) return {};
    const float scale = std::max(w[hand_at].scale, 0.1f);
    // Worn: the hand turned so the camera points along the aim, and the wrist where that puts the lens
    // at the goal. Held: the palm lies on the forearm's line, past the wrist, so the forearm and hand are
    // one straight reach of b + palm to where the camera's handle has to be.
    // Where the lens is shown held: the camera's own eye, but out in front of the shoulder along the aim
    // and no higher than the shoulder, as a filmer holds it (an eye up by the head or tucked against the
    // body would fold the arm up into the face).
    Vec3 lens = goal.lens;
    {
        Vec3 ahead = rotate(aim, {0.0f, 0.0f, -1.0f});
        ahead[1] = 0.0f;
        ahead = unit(ahead, unit(Vec3{lens[0] - s[0], 0.0f, lens[2] - s[2]}, {0, 0, 1}));
        const float out = dot(sub(lens, s), ahead);
        if (!goal.free_reach) {
            if (out < min_reach_out) lens = add(lens, scaled(ahead, min_reach_out - out));
            lens[1] = std::min(lens[1], s[1] - below_shoulder);
        }
    }
    const auto &worn = grip(goal.hand);
    const Quat hand_world = normalized(multiply(aim, conjugate(worn.camera)));
    const Vec3 grip_offset = scaled(rotate(hand_world, worn.from_wrist), scale);
    Vec3 palm_goal = goal.worn ? sub(sub(lens, rotate(aim, lens_from_handle)), grip_offset)
                               : sub(lens, rotate(aim, lens_from_handle));
    if (goal.around_body) {
        // The body as a column through the hips and chest: the hand's target, and the straight reach to
        // it from the shoulder, kept out of it (pushed out along the way they came closest).
        const Vec3 low = w[2].position, high = w[chest].position;
        const float bottom = low[1] - 0.35f, top = high[1] + 0.15f;
        const auto axis_at = [&](float y) {
            const float t = std::clamp((y - low[1]) / std::max(high[1] - low[1], 0.05f), 0.0f, 1.0f);
            return Vec3{low[0] + (high[0] - low[0]) * t, y, low[2] + (high[2] - low[2]) * t};
        };
        // As wide as the body is: inside the shoulders (the arm hangs from the column's edge).
        Vec3 shoulder_out = sub(s, axis_at(s[1]));
        shoulder_out[1] = 0.0f;
        const float radius = std::min(body_radius, length(shoulder_out) * 0.85f);
        for (int round = 0; round < 3; ++round) {
            float nearest = radius;
            Vec3 out_dir{}, at{};
            for (int k = 3; k <= 8; ++k) {
                const Vec3 p = add(s, scaled(sub(palm_goal, s), k / 8.0f));
                if (p[1] < bottom || p[1] > top) continue;
                Vec3 radial = sub(p, axis_at(p[1]));
                radial[1] = 0.0f;
                const float r = length(radial);
                if (r < nearest) {
                    nearest = r;
                    at = p;
                    Vec3 side_out = sub(s, w[chest].position);
                    side_out[1] = 0.0f;
                    out_dir = unit(radial, unit(side_out, {goal.hand == Hand::left ? -1.0f : 1.0f, 0, 0}));
                }
            }
            if (nearest >= radius) break;
            palm_goal = add(palm_goal, scaled(out_dir, radius - nearest + 0.01f));
        }
    }
    const float palm = goal.worn ? 0.0f : palm_from_wrist * scale, b_palm = b + palm;
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
    if (goal.worn) {
        // The hand, from the forearm as it now is, turned so the worn camera points along the aim.
        if (!compose(pose, joints, w)) return {};
        auto &hand_bone = pose[joints[hand_at]];
        hand_bone.rotation = blend(hand_bone.rotation, normalized(multiply(conjugate(w[fore].rotation), hand_world)), weight);
    }
    // Where the hand ended up, from the pose as it now is.
    if (!compose(pose, joints, w)) return {};
    if (goal.worn) {
        Held held;
        held.palm.rotation = normalized(multiply(w[hand_at].rotation, worn.camera));
        held.palm.position = add(w[hand_at].position, scaled(rotate(w[hand_at].rotation, worn.from_wrist), scale));
        held.lens = add(held.palm.position, rotate(held.palm.rotation, lens_from_handle));
        held.valid = finite(held.palm.position) && finite(held.lens);
        held.reached = wanted <= reach + 0.01f;
        return held;
    }
    auto held = place_camera(w[fore].position, w[hand_at].position, aim, scale);
    held.reached = wanted <= reach + 0.01f;
    return held;
}

} // namespace

Held held_in(std::span<const Bone> pose, Hand hand, const Quat &aim) noexcept {
    std::array<Placed, right_chain.size()> w{};
    if (!compose(pose, chain(hand), w) || !finite(aim)) return {};
    return place_camera(w[fore].position, w[hand_at].position, aim, std::max(w[hand_at].scale, 0.1f));
}
} // namespace dingosdk::filmer::hold
