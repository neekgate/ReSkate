#pragma once
// The filmer's camera in their hand: the arm that holds it out and where the camera sits.
//
// Pure maths, no game code, so it is tested on its own. The skater's animation pose is the 395-joint
// AnimBase skeleton, each joint {scale, Hamilton quaternion, translation} relative to its parent, and
// composing from joint 0 lands in the world (joint 1 carries the skater's placement); see
// Extension/Skater/client_first_person.cpp. The arms hang off Spine3 (45): the right arm is 46
// RightShoulder, 47 RightArm, 48 RightForeArm, 49 RightHand; the left 275, 276, 277, 278
// (Extension/Multiplayer/Net/skater_bone_parents.h, Extension/HallOfMeat/hall_of_meat_skeleton.h).
//
// The camera is held by its top handle. The model's origin is the middle of that handle and its lens
// looks down its -Z (assets/models/vx1000/prepare_camcorder.py), so the model is placed at the palm and
// turned to where the filmer aims.
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace dingosdk::filmer::hold {
using Vec3 = std::array<float, 3>;
using Quat = std::array<float, 4>; // x, y, z, w

struct Bone {
    Vec3 scale{1, 1, 1};
    Quat rotation{0, 0, 0, 1};
    Vec3 position{};
};
struct Placed {
    Vec3 position{};
    Quat rotation{0, 0, 0, 1};
    float scale = 1.0f;
};

enum class Hand : std::uint8_t { right, left };
// Joints from the pose's root to the hand: the arm's last four are shoulder, upper arm, forearm, hand.
inline constexpr std::array<std::uint16_t, 11> right_chain{0, 1, 7, 42, 43, 44, 45, 46, 47, 48, 49};
inline constexpr std::array<std::uint16_t, 11> left_chain{0, 1, 7, 42, 43, 44, 45, 275, 276, 277, 278};
inline constexpr std::uint16_t chest_joint = 45;
inline constexpr std::size_t pose_joints = 395;
inline constexpr std::span<const std::uint16_t> chain(Hand hand) noexcept { return hand == Hand::left ? left_chain : right_chain; }

// The camera as the model has it: from the handle (the origin) to the front of the lens, and from the
// wrist joint to the middle of the palm along the hand.
inline constexpr Vec3 lens_from_handle{0.0f, -0.143f, -0.248f};
inline constexpr float palm_from_wrist = 0.075f;

// The camcorder worn as hand wear (a cosmetic, assets/models/vx1000/make_cosmetic.py): its mesh is
// weighted wholly to the hand, its handle bar through the fist at <Hand>_Attach. `camera` is the
// camera's rows (right, up, back) in the hand joint's own frame and `from_wrist` where its grip is
// from the hand joint, in that frame, both from the skeleton's bind pose.
struct Grip {
    Quat camera{0, 0, 0, 1};
    Vec3 from_wrist{};
};
inline constexpr Grip right_grip{{0.5398f, -0.5523f, 0.4688f, 0.4287f}, {0.1191f, 0.0435f, -0.0251f}};
inline constexpr Grip left_grip{{0.4688f, -0.4287f, 0.5398f, 0.5523f}, {0.1191f, 0.0435f, 0.0251f}};
inline constexpr const Grip &grip(Hand hand) noexcept { return hand == Hand::left ? left_grip : right_grip; }

// ---- Vector and quaternion helpers.
Quat multiply(const Quat &a, const Quat &b) noexcept;
Quat conjugate(const Quat &q) noexcept;
Vec3 rotate(const Quat &q, const Vec3 &v) noexcept;
Quat normalized(const Quat &q) noexcept;
// The shortest turn taking direction `from` onto direction `to`.
Quat arc(const Vec3 &from, const Vec3 &to) noexcept;
// Part way from a to b, the short way round.
Quat blend(const Quat &a, Quat b, float t) noexcept;
// From a matrix whose rows are right, up, back (the camera's and the flight model's), and back.
Quat from_rows(const std::array<float, 16> &m) noexcept;
// A turn of `yaw` about up then `pitch` about the result's right (radians): the camera's rows for a
// heading and a tilt.
Quat from_yaw_pitch(float yaw, float pitch) noexcept;

// Every joint along `joints` (each the parent of the next, the first hanging from nothing) composed
// into the world. `out` gets one per joint; false when a joint is missing or not a number.
bool compose(std::span<const Bone> pose, std::span<const std::uint16_t> joints, std::span<Placed> out) noexcept;

struct Goal {
    Hand hand = Hand::right;
    Vec3 lens{};          // where the front of the lens should be (the camera's eye)
    Quat aim{0, 0, 0, 1}; // which way the camera points (rows right, up, back)
    float weight = 1.0f;  // 0: the arm as the animation has it, 1: holding the camera out
    // The camcorder is worn on the hand (Grip): the hand turns as well, so its lens points along `aim`.
    bool worn{};
};
struct Held {
    bool valid{};
    Placed palm; // where the camera's handle is, turned the way it points
    Vec3 lens{}; // where the front of its lens ended up (short of the goal when out of reach)
    bool reached{};
};
// Bends the arm in `pose` (its upper arm and forearm, in place, and the hand when the camera is worn) so the hand holds the camera with its
// lens as near the goal as the arm reaches, the elbow out and down, and says where the camera is.
// Leaves the pose alone and returns invalid when the joints are not there.
Held hold(std::span<Bone> pose, const Goal &goal) noexcept;
// Where the camera is in a pose that already holds it (another player's, as they sent it): the palm
// of the hand, turned by the aim they sent.
Held held_in(std::span<const Bone> pose, Hand hand, const Quat &aim) noexcept;
} // namespace dingosdk::filmer::hold
