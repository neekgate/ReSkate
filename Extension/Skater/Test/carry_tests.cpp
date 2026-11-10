// Something carried apart from the game: held out in front of the chest when it points level, higher
// when it points up, turned the way it is aimed when worn on the hand, the other hand under it, the
// hips lowered with the feet kept where they were, the body turned, and a broken pose left alone.
#include "Extension/Skater/carry.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <vector>

using namespace dingosdk;
using namespace dingosdk::carry;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
float distance(const Vec3& a, const Vec3& b) {
    return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
}
bool same_turn(const Quat& a, const Quat& b) { return std::abs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]) > 0.999f; }

// A skater standing, facing +Z (its right at -X), arms hanging out to the sides: upper arms 0.3 m,
// forearms 0.28 m, thighs and shins 0.45 m, the knees a little forward.
constexpr float upper_length = 0.3f, fore_length = 0.28f;
std::vector<Joint> standing() {
    std::vector<Joint> pose(395);
    pose[0].position = {4, 0, -2};
    pose[1].position = {0, 0.95f, 0};
    for (const std::size_t j : {42u, 43u, 44u, 45u}) pose[j].position = {0, 0.1f, 0};
    pose[101].position = {0, 0.1f, 0};
    pose[102].position = {0, 0.05f, 0};
    pose[103].position = {0, 0.1f, 0};
    for (const auto hand : {Hand::right, Hand::left}) {
        const auto joints = arm_reach::chain(hand);
        const float x = hand == Hand::right ? -1.0f : 1.0f;
        pose[joints[7]].position = {0.15f * x, 0, 0};
        pose[joints[8]].position = {0.05f * x, 0, 0};
        pose[joints[9]].position = {0, -upper_length, 0};
        pose[joints[10]].position = {0, -fore_length, 0};
    }
    for (const auto& leg : {right_leg, left_leg}) {
        const float x = leg[3] == 8 ? -1.0f : 1.0f;
        pose[leg[3]].position = {0.1f * x, 0, 0};
        pose[leg[4]].position = {0, -0.45f, 0.03f};
        pose[leg[5]].position = {0, -0.45f, -0.03f};
    }
    return pose;
}
std::array<Placed, 11> arm(const std::vector<Joint>& pose, Hand hand) {
    std::array<Placed, 11> w{};
    check(arm_reach::compose(pose, arm_reach::chain(hand), w), "The arm composes");
    return w;
}
Placed placed(const std::vector<Joint>& pose, std::span<const std::uint16_t> path) {
    std::array<Placed, 11> w{};
    check(arm_reach::compose(pose, path, w), "The joints compose");
    return w[path.size() - 1];
}
// Pointing at +Z, tipped up by `up` radians.
Quat aimed(float up) { return anim_studio::multiply(anim_studio::axis_angle({0, 1, 0}, std::numbers::pi_v<float>), anim_studio::axis_angle({1, 0, 0}, up)); }

void held_level_in_front_of_the_chest() {
    auto pose = standing();
    const Vec3 shoulder = arm(pose, Hand::right)[8].position;
    Goal goal;
    goal.front = {shoulder[0], shoulder[1], shoulder[2] + 2.0f}; // asked for far off: held where a person holds it
    goal.aim = aimed(0);
    goal.front_from_grip = {0, -0.143f, -0.248f};
    goal.worn = Grip{};
    const auto held = carry::carry(pose, goal);
    check(held.valid, "Carried");
    const auto w = arm(pose, Hand::right);
    check(held.front[2] - w[8].position[2] > 0.25f, "out in front of the shoulder");
    check(held.front[1] < w[8].position[1], "and below it");
    check(std::abs(distance(w[8].position, w[9].position) - upper_length) < 1e-4f, "The upper arm keeps its length");
    check(std::abs(distance(w[9].position, w[10].position) - fore_length) < 1e-4f, "and the forearm its length");
    check(same_turn(held.item.rotation, goal.aim), "Worn, it points the way it is aimed");
}

void aimed_up_it_comes_up() {
    auto level = standing(), up = standing();
    Goal goal;
    goal.front = {4, 1.4f, 0};
    goal.front_from_grip = {0, -0.143f, -0.248f};
    goal.aim = aimed(0);
    const auto low = carry::carry(level, goal);
    goal.aim = aimed(1.2f);
    const auto high = carry::carry(up, goal);
    check(low.valid && high.valid, "Carried both ways");
    check(high.front[1] > low.front[1] + 0.2f, "Aimed up, it is held higher");
}

void the_other_hand_holds_it_too() {
    auto pose = standing();
    const auto before = arm(pose, Hand::left)[10].position;
    Goal goal;
    goal.front = {4, 1.3f, 0.5f};
    goal.aim = aimed(0);
    goal.both = true;
    const auto held = carry::carry(pose, goal);
    check(held.valid, "Carried in both hands");
    const auto left = arm(pose, Hand::left)[10].position;
    check(distance(left, before) > 0.2f, "The other hand comes up to it");
    check(distance(left, held.item.position) < 0.3f, "and holds it");
}

void the_feet_stay_as_the_hips_go_down() {
    auto pose = standing();
    const auto right_foot = placed(pose, right_leg).position, left_foot = placed(pose, left_leg).position;
    const float hips = placed(pose, std::span<const std::uint16_t>(right_leg).first(3)).position[1];
    Goal goal;
    goal.front = {4, 1.2f, 0.5f};
    goal.aim = aimed(-0.3f);
    goal.crouch = 0.2f;
    goal.hunch = 0.8f;
    check(carry::carry(pose, goal).valid, "Carried, crouched");
    check(std::abs(placed(pose, std::span<const std::uint16_t>(right_leg).first(3)).position[1] - (hips - 0.2f)) < 1e-3f, "The hips go down");
    check(distance(placed(pose, right_leg).position, right_foot) < 2e-3f, "the right foot stays where it was");
    check(distance(placed(pose, left_leg).position, left_foot) < 2e-3f, "and the left");
    const Vec3 head = placed(pose, head_chain).position;
    check(head[2] > -2.0f + 0.05f, "The back curls over toward it");
}

void the_body_turns() {
    auto pose = standing();
    Goal goal;
    goal.front = {4, 1.3f, 0.5f};
    goal.aim = aimed(0);
    goal.turn = std::numbers::pi_v<float> / 2;
    check(carry::carry(pose, goal).valid, "Carried, turned");
    const auto facing = body_facing(pose);
    check(std::abs(facing[0]) > 0.99f, "The body turns a quarter round");
}

void the_face_looks_at_a_point() {
    // A face in front of the head (+Z, the way the skater faces), its joints under joint 107.
    auto pose = standing();
    pose[107].position = {0, 0.05f, 0};
    for (std::size_t j = 108; j <= 254; ++j) pose[j].position = {0.01f * static_cast<float>(j % 5) - 0.02f, 0.03f, 0.09f};
    const std::array<std::uint16_t, 10> chain{0, 1, 7, 42, 43, 44, 45, 101, 102, 103};
    const Placed head = placed(pose, chain);
    Goal goal;
    goal.front = {4, 1.3f, -1.5f};
    goal.aim = aimed(0);
    // Off to its right (-X) and a little ahead: the head turns that way.
    goal.look_at = Vec3{head.position[0] - 1.0f, head.position[1], head.position[2] + 0.4f};
    check(carry::carry(pose, goal).valid, "Carried, looking");
    const Placed turned = placed(pose, chain);
    const Vec3 face = anim_studio::rotate(turned.rotation, {0, 0, 1});
    check(face[0] < -0.5f, "The face turns to the point on its right");

    // Then a pose whose face joints are all at one point (not posed, as riding): the face's way as measured
    // is kept, and the head still turns to the point.
    auto riding = standing();
    for (std::size_t j = 107; j <= 254; ++j) riding[j].position = {0, 0, 0};
    check(carry::carry(riding, goal).valid, "Carried, looking, the face not posed");
    const Placed again = placed(riding, chain);
    check(anim_studio::rotate(again.rotation, {0, 0, 1})[0] < -0.5f, "The face still turns to the point, by the way measured before");
}

float degrees_between(const Vec3& a, const Vec3& b) {
    const float d = (a[0] * b[0] + a[1] * b[1] + a[2] * b[2]) /
                    std::sqrt((a[0] * a[0] + a[1] * a[1] + a[2] * a[2]) * (b[0] * b[0] + b[1] * b[1] + b[2] * b[2]));
    return std::acos(std::clamp(d, -1.0f, 1.0f)) * 180.0f / std::numbers::pi_v<float>;
}
std::vector<Joint> with_a_face() {
    auto pose = standing();
    pose[107].position = {0, 0.05f, 0};
    for (std::size_t j = 108; j <= 254; ++j) pose[j].position = {0.01f * static_cast<float>(j % 5) - 0.02f, 0.03f, 0.09f};
    return pose;
}
void the_face_turns_all_the_way_and_no_further() {
    const std::array<std::uint16_t, 10> chain{0, 1, 7, 42, 43, 44, 45, 101, 102, 103};
    // A quarter turn round to its right, level: the face ends on the point.
    {
        auto pose = with_a_face();
        const Placed head = placed(pose, chain);
        Goal goal;
        goal.front = {4, 1.3f, -1.5f};
        goal.aim = aimed(0);
        goal.look_at = Vec3{head.position[0] - 1.0f, head.position[1], head.position[2]};
        check(carry::carry(pose, goal).valid, "Carried, looking right round");
        const Placed turned = placed(pose, chain);
        const Vec3 to{goal.look_at->at(0) - turned.position[0], goal.look_at->at(1) - turned.position[1], goal.look_at->at(2) - turned.position[2]};
        check(degrees_between(anim_studio::rotate(turned.rotation, {0, 0, 1}), to) < 3.0f, "A quarter turn lands on the point");
    }
    // Straight behind it: no further round than a neck turns (about 100 degrees off the way the hips face).
    {
        auto pose = with_a_face();
        const Placed head = placed(pose, chain);
        Goal goal;
        goal.front = {4, 1.3f, -1.5f};
        goal.aim = aimed(0);
        goal.look_at = Vec3{head.position[0] - 0.05f, head.position[1], head.position[2] - 1.0f};
        check(carry::carry(pose, goal).valid, "Carried, looking behind");
        const Placed turned = placed(pose, chain);
        const float off = degrees_between(anim_studio::rotate(turned.rotation, {0, 0, 1}), {0, 0, 1});
        check(off > 90.0f && off < 110.0f, "Behind: the head turns as far as a neck does, no further");
    }
    // Held out of reach (exact, the front far off): the face looks where the item ended up.
    {
        auto pose = with_a_face();
        Goal goal;
        goal.front = {4, 1.3f, 3.0f}; // 5 m out in front: well past the arm
        goal.aim = aimed(0);
        goal.exact = true;
        goal.look_at = goal.front;
        const auto held = carry::carry(pose, goal);
        check(held.valid, "Carried, out of reach");
        const Placed turned = placed(pose, chain);
        const Vec3 to{held.front[0] - turned.position[0], held.front[1] - turned.position[1], held.front[2] - turned.position[2]};
        check(degrees_between(anim_studio::rotate(turned.rotation, {0, 0, 1}), to) < 5.0f, "The face looks at the item where it is held");
    }
}

void held_where_asked_up_in_front_of_the_face() {
    // Aimed straight up and asked to hold it over the face (exact): the hand goes up there, well over the
    // shoulder, rather than kept down by it as the aim's own sweep does.
    auto pose = standing();
    const auto shoulder = arm(pose, Hand::right)[8].position;
    Goal goal;
    goal.aim = aimed(1.5f);
    goal.front = {shoulder[0] + 0.15f, shoulder[1] + 0.45f, shoulder[2] + 0.25f};
    goal.exact = true;
    const auto held = carry::carry(pose, goal);
    check(held.valid, "Carried up in front of the face");
    check(held.front[1] > shoulder[1] + 0.35f, "Held high, in front of the face");
    Goal sweep = goal;
    sweep.exact = false;
    auto pose2 = standing();
    const auto swept = carry::carry(pose2, sweep);
    check(swept.valid && swept.front[1] < held.front[1], "The aim's own sweep holds it lower");
}

void no_weight_and_a_broken_pose() {
    auto pose = standing();
    const auto before = pose;
    Goal goal;
    goal.front = {4, 1.3f, 0.5f};
    goal.aim = aimed(0);
    goal.crouch = 0.3f;
    goal.weight = 0;
    check(carry::carry(pose, goal).valid, "Carried with no weight");
    for (std::size_t i = 0; i < pose.size(); ++i)
        check(same_turn(pose[i].rotation, before[i].rotation) && distance(pose[i].position, before[i].position) < 1e-5f, "leaves the pose as it was");
    auto again = standing();
    goal.weight = 1;
    goal.both = true;
    goal.hunch = 0.6f;
    goal.turn = 0.5f;
    check(carry::carry(again, goal).valid, "Carried with everything");
    for (std::size_t i = 0; i < again.size(); ++i)
        if (std::find(written.begin(), written.end(), i) == written.end())
            check(same_turn(again[i].rotation, before[i].rotation) && distance(again[i].position, before[i].position) < 1e-5f,
                  "A carry writes only the joints it says it writes");
    auto broken = standing();
    broken[48].rotation[0] = NAN;
    check(!carry::carry(broken, goal).valid, "A broken arm carries nothing");
    goal.front = {NAN, 0, 0};
    auto fine = standing();
    check(!carry::carry(fine, goal).valid, "A point that is not a number is refused");
}
} // namespace

int main() {
    try {
        held_level_in_front_of_the_chest();
        aimed_up_it_comes_up();
        the_other_hand_holds_it_too();
        the_feet_stay_as_the_hips_go_down();
        the_body_turns();
        the_face_looks_at_a_point();
        the_face_turns_all_the_way_and_no_further();
        held_where_asked_up_in_front_of_the_face();
        no_weight_and_a_broken_pose();
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
    std::cout << "carry: all passed\n";
    return 0;
}
