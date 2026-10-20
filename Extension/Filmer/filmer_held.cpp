#include "filmer_held.h"
#include "filmer_beacon.h"
#include "Engine/Core/Log/logging.h"
#include "Extension/Multiplayer/Net/protocol.h"
#include "Extension/Multiplayer/Remote/native_pose_layout.h"
#include "Extension/Multiplayer/Remote/native_prop.h"
#include "Extension/Multiplayer/Remote/native_skater.h"
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <format>
#include <mutex>
#include <vector>

namespace dingosdk::filmer::held {
namespace {
constexpr std::uintptr_t highest = 0x00007fffffffffffULL;
constexpr std::size_t bone_stride = 0x30; // scale.xyzw | rotation.xyzw | position.xyzw
constexpr ULONGLONG fresh_ms = 300;       // a request or a hand older than this is gone
constexpr float ease_seconds = 0.35f;     // how long the arm takes to come up, or go back down

bool seh_read(std::uintptr_t address, void *out, std::size_t size) noexcept {
    if (address < 0x10000 || size > 0x10000 || address > highest - size) return false;
    __try {
        std::memcpy(out, reinterpret_cast<const void *>(address), size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool seh_write(std::uintptr_t address, const void *in, std::size_t size) noexcept {
    if (address < 0x10000 || size > 0x10000 || address > highest - size) return false;
    __try {
        std::memcpy(reinterpret_cast<void *>(address), in, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---- Your arm (the animation thread) and what the Filmer asks of it (the game thread).
struct Arm {
    std::mutex mutex;
    Request request;              // under the mutex
    ULONGLONG requested{};        // when, under the mutex
    hold::Held palm;              // where your camera is, under the mutex
    ULONGLONG palm_at{};
    std::atomic<std::uintptr_t> component{}, base{};
    std::atomic<bool> hooked{};
    bool hook_tried{};            // game thread
    std::string issue;            // game thread: why the arm cannot be bent
    // The animation thread's own.
    Request seen;
    ULONGLONG seen_at{};
    float weight{};
    ULONGLONG last_update{};
    std::atomic<std::uint32_t> bent{}, unreadable{}, own_from_pose{};
};
Arm &arm() {
    static auto *value = new Arm;
    return *value;
}

// The skater's pose as it is handed to the renderer (the game has finished with it by then: bent any
// earlier, its own arm work puts the arm back), for your skater's.
// `animated`: called as the game finishes animating the skater: the request is taken and the arm
// eased; nothing is written, as the game works the arm again after this. Else called as the pose is
// handed to the renderer: written, so it is what you see (the others are sent it by on_outgoing).
void bend(std::uintptr_t component, std::uintptr_t holder, bool animated) noexcept {
    auto &a = arm();
    const auto now = GetTickCount64();
    if (animated) {
        {
            std::unique_lock lock(a.mutex, std::try_to_lock);
            if (lock) {
                a.seen = a.request;
                a.seen_at = a.requested;
            }
        }
        const float dt = a.last_update ? std::clamp(static_cast<float>(now - a.last_update) / 1000.0f, 0.0f, 0.1f) : 0.0f;
        a.last_update = now;
        const bool wanted = a.seen.hold && now - a.seen_at < fresh_ms;
        a.weight = std::clamp(a.weight + (wanted ? dt : -dt) / ease_seconds, 0.0f, 1.0f);
    }
    if (a.weight <= 0.0f) return;
    try {
        (void)component;
        const auto base = a.base.load(std::memory_order_acquire);
        const auto layout = multiplayer::read_native_pose_layout(seh_read, base, holder, 512);
        if (!layout.buffer || layout.count != hold::pose_joints) return;
        // Only the joints from the root to the hand are read: the rest stay as they are.
        std::array<hold::Bone, hold::pose_joints> pose{};
        const auto joints = hold::chain(a.seen.hand);
        for (const auto joint : joints) {
            std::array<float, 12> raw{};
            if (!seh_read(layout.buffer + joint * bone_stride, raw.data(), sizeof(raw))) {
                a.unreadable.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            pose[joint] = {{raw[0], raw[1], raw[2]}, {raw[4], raw[5], raw[6], raw[7]}, {raw[8], raw[9], raw[10]}};
        }
        hold::Goal goal;
        goal.hand = a.seen.hand;
        goal.aim = a.seen.aim;
        // Eased in and out: the arm comes up and goes back down instead of snapping.
        goal.weight = a.weight * a.weight * (3.0f - 2.0f * a.weight);
        goal.lens = a.seen.lens;
        goal.worn = a.seen.worn;
        if (a.seen.relative) {
            std::array<hold::Placed, hold::right_chain.size()> placed{};
            if (!hold::compose(pose, joints, placed)) return;
            const auto &shoulder = placed[8].position, &f = a.seen.facing;
            goal.lens = {shoulder[0] + f[0] * a.seen.lens[0], shoulder[1] + a.seen.lens[1], shoulder[2] + f[2] * a.seen.lens[0]};
        }
        const auto held = hold::hold(pose, goal);
        if (!held.valid) return;
        for (const std::size_t at : {std::size_t{8}, std::size_t{9}, std::size_t{10}}) {
            if (animated || (at == 10 && !goal.worn)) break;
            const auto joint = joints[at];
            if (!seh_write(layout.buffer + joint * bone_stride + 0x10, pose[joint].rotation.data(), sizeof(hold::Quat))) return;
        }
        a.bent.fetch_add(1, std::memory_order_relaxed);
        std::unique_lock lock(a.mutex, std::try_to_lock);
        if (lock) {
            a.palm = held;
            a.palm_at = now;
        }
    } catch (...) {
        a.unreadable.fetch_add(1, std::memory_order_relaxed);
    }
}

// The pose capture_local reads to send (client thread): the arm bent in it here, from the request
// itself, so the others see the camera held out whether or not the game drew or animated your skater
// in between (a camera filming someone else looks away from you).
struct Outgoing {
    float weight{};
    ULONGLONG last{};
    std::atomic<std::uint32_t> sent{}, bent{};
};
Outgoing &outgoing() {
    static auto *value = new Outgoing;
    return *value;
}
void on_outgoing(std::uintptr_t component, std::vector<multiplayer::Transform> &skater) noexcept {
    auto &a = arm();
    auto &o = outgoing();
    try {
        if (!component || component != a.component.load(std::memory_order_acquire)) return;
        o.sent.fetch_add(1, std::memory_order_relaxed);
        Request request;
        ULONGLONG requested{};
        {
            std::lock_guard lock(a.mutex);
            request = a.request;
            requested = a.requested;
        }
        const auto now = GetTickCount64();
        const float dt = o.last ? std::clamp(static_cast<float>(now - o.last) / 1000.0f, 0.0f, 0.2f) : 0.0f;
        o.last = now;
        const bool wanted = request.hold && now - requested < fresh_ms;
        o.weight = std::clamp(o.weight + (wanted ? dt : -dt) / ease_seconds, 0.0f, 1.0f);
        if (o.weight <= 0.0f || skater.size() < hold::pose_joints) return;
        thread_local std::vector<hold::Bone> pose;
        pose.resize(skater.size());
        for (std::size_t i = 0; i < skater.size(); ++i) pose[i] = {skater[i].scale, skater[i].rotation, skater[i].position};
        hold::Goal goal;
        goal.hand = request.hand;
        goal.aim = request.aim;
        goal.weight = o.weight * o.weight * (3.0f - 2.0f * o.weight);
        goal.lens = request.lens;
        goal.worn = request.worn;
        const auto joints = hold::chain(request.hand);
        if (request.relative) {
            std::array<hold::Placed, hold::right_chain.size()> placed{};
            if (!hold::compose(pose, joints, placed)) return;
            const auto &shoulder = placed[8].position, &f = request.facing;
            goal.lens = {shoulder[0] + f[0] * request.lens[0], shoulder[1] + request.lens[1], shoulder[2] + f[2] * request.lens[0]};
        }
        if (!hold::hold(pose, goal).valid) return;
        for (const std::size_t at : {std::size_t{8}, std::size_t{9}, std::size_t{10}})
            skater[joints[at]].rotation = pose[joints[at]].rotation;
        o.bent.fetch_add(1, std::memory_order_relaxed);
    } catch (...) {}
}

void on_render(std::uintptr_t animation_interface) noexcept {
    const auto component = arm().component.load(std::memory_order_acquire);
    if (!component) return;
    std::uintptr_t holder{};
    if (!seh_read(component + 0xa0, &holder, sizeof(holder)) || !holder || animation_interface != holder + 0xc0) return;
    const auto error = GetLastError();
    bend(component, holder, false);
    SetLastError(error);
}
void on_animation(std::uintptr_t component) noexcept {
    if (!component || component != arm().component.load(std::memory_order_acquire)) return;
    std::uintptr_t holder{};
    if (!seh_read(component + 0xa0, &holder, sizeof(holder)) || !holder) return;
    const auto error = GetLastError();
    bend(component, holder, true);
    SetLastError(error);
}

// ---- Other players' hands (the client thread).
struct RemoteHand {
    std::uint64_t id{};
    hold::Placed palm;
    ULONGLONG at{};
    bool worn{}; // they wear the camcorder: the game draws it, no model is placed
};
struct Remotes {
    std::mutex mutex;
    std::vector<RemoteHand> list;
};
Remotes &remotes() {
    static auto *value = new Remotes;
    return *value;
}
bool shown{}; // client thread: models were placed last tick
} // namespace

void set_local(const Request &request) noexcept {
    auto &a = arm();
    std::lock_guard lock(a.mutex);
    a.request = request;
    a.requested = GetTickCount64();
}

void note_remote_pose(std::uint64_t id, const multiplayer::Pose &pose) noexcept {
    try {
        const auto filming = beacon::filming(id);
        if (!filming || pose.skater.size() < hold::left_chain.back() + 1u) return;
        thread_local std::vector<hold::Bone> bones;
        bones.resize(pose.skater.size());
        for (std::size_t i = 0; i < bones.size(); ++i) {
            const auto &t = pose.skater[i];
            bones[i] = {t.scale, t.rotation, t.position};
        }
        const auto hand = (filming->flags & beacon::flag_left_hand) ? hold::Hand::left : hold::Hand::right;
        const bool worn = (filming->flags & beacon::flag_worn) != 0;
        const auto held = hold::held_in(bones, hand, filming->aim);
        if (!held.valid) return;
        auto &r = remotes();
        std::lock_guard lock(r.mutex);
        const auto now = GetTickCount64();
        for (auto &entry : r.list)
            if (entry.id == id) {
                entry.palm = held.palm;
                entry.at = now;
                entry.worn = worn;
                return;
            }
        if (r.list.size() < 32) r.list.push_back({id, held.palm, now, worn});
    } catch (...) {}
}

void tick(std::uintptr_t base, std::uintptr_t client, bool in_level, const Options &options) noexcept {
    auto &a = arm();
    try {
        if (!in_level) {
            a.component.store(0, std::memory_order_release);
            if (shown) multiplayer::tick_held_camera_models(base, client, {}, multiplayer::held_camera_blueprint, false);
            shown = false;
            return;
        }
        a.base.store(base, std::memory_order_release);
        // The pose the others are sent always goes through here (cheap while the camera is not held).
        multiplayer::set_local_pose_filter(&on_outgoing);
        a.component.store(multiplayer::local_skater_component(base, client), std::memory_order_release);
        Request request;
        {
            std::lock_guard lock(a.mutex);
            request = a.request;
        }
        // The animation hook, the first time the arm is wanted.
        if (request.hold && !a.hook_tried) {
            a.hook_tried = true;
            std::string detail;
            if (multiplayer::install_entity_hooks(base, detail)) {
                multiplayer::set_render_pose_listener(&on_render, 1);
                multiplayer::set_animation_evaluated_listener(&on_animation, 1);
                a.hooked.store(true, std::memory_order_release);
                logging::log(logging::Level::info, logging::Channel::skater, "Filmer: your arm holds the camera out while you film.");
            } else {
                a.issue = "Your arm cannot hold the camera out: " + detail;
                logging::log(logging::Level::warning, logging::Channel::skater, "Filmer: {}", a.issue);
            }
        }
        const auto now = GetTickCount64();
        std::vector<multiplayer::HeldCameraModel> cameras;
        if (options.show_others) {
            auto &r = remotes();
            std::lock_guard lock(r.mutex);
            std::erase_if(r.list, [&](const RemoteHand &h) { return now - h.at > 2000; });
            for (const auto &h : r.list)
                if (now - h.at < fresh_ms && !h.worn) cameras.push_back({h.id, h.palm.position, h.palm.rotation});
        }
        if (request.show_own && !request.worn) {
            bool placed = false;
            {
                std::lock_guard lock(a.mutex);
                if (a.palm.valid && now - a.palm_at < fresh_ms) {
                    cameras.push_back({0, a.palm.palm.position, a.palm.palm.rotation});
                    placed = true;
                }
            }
            // The arm is not being bent (no animation callback yet, or it failed): the camera still goes in
            // the hand as the animation has it, from your pose.
            if (!placed) {
                const auto frame = multiplayer::capture_local(base, client, true);
                if (frame.ready && frame.pose.skater.size() >= hold::pose_joints) {
                    std::vector<hold::Bone> bones(frame.pose.skater.size());
                    for (std::size_t i = 0; i < bones.size(); ++i) {
                        const auto &t = frame.pose.skater[i];
                        bones[i] = {t.scale, t.rotation, t.position};
                    }
                    if (const auto held = hold::held_in(bones, request.hand, request.aim); held.valid)
                        cameras.push_back({0, held.palm.position, held.palm.rotation});
                }
                a.own_from_pose.store(placed ? 0u : 1u, std::memory_order_relaxed);
            } else {
                a.own_from_pose.store(0, std::memory_order_relaxed);
            }
        }
        if (!cameras.empty() || shown)
            multiplayer::tick_held_camera_models(base, client, cameras, options.blueprint.empty() ? multiplayer::held_camera_blueprint : options.blueprint,
                                                 options.stand_in);
        shown = !cameras.empty();
    } catch (...) {}
}

std::string status() {
    auto &a = arm();
    std::string arm_text = !a.issue.empty() ? a.issue
                           : a.hooked.load() ? std::format("arm bent {} times", a.bent.load(std::memory_order_relaxed))
                                             : "arm not needed yet";
    if (const auto unreadable = a.unreadable.load(std::memory_order_relaxed)) arm_text += std::format(" ({} unreadable poses)", unreadable);
    if (a.own_from_pose.load(std::memory_order_relaxed)) arm_text += " (your camera placed from your pose: the arm is not bending)";
    arm_text += std::format(", sent bent {} of {}", outgoing().bent.load(std::memory_order_relaxed), outgoing().sent.load(std::memory_order_relaxed));
    return "Held camera: " + arm_text + "; " + multiplayer::held_camera_models_status();
}
} // namespace dingosdk::filmer::held
