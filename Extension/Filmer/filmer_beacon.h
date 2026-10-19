#pragma once
// Letting other players see that you are filming: a small `filmer` packet (Multiplayer/Net/protocol.h)
// about ten times a second while you film, and once when you stop. It says which hand holds the
// camera, which lens is on it and which way it points. Your arm holding it out travels in your pose
// like the rest of you; each receiver puts the camera in that hand (filmer_held.cpp).
// Header-only and free of game code, so the networking can use it without pulling in the Filmer.
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

namespace dingosdk::filmer::beacon {
using Clock = std::chrono::steady_clock;
using Rotation = std::array<float, 4>; // x, y, z, w: the camera's rows right, up, back
inline constexpr Rotation no_rotation{0.0f, 0.0f, 0.0f, 1.0f};
// The packet's flag byte.
// flag_worn: the camcorder is worn as hand wear (a cosmetic the game draws), so no model is placed for it.
inline constexpr std::uint8_t flag_filming = 1, flag_left_hand = 2, flag_long_lens = 4, flag_worn = 8, flag_mask = 15;

inline bool valid_rotation(const Rotation &q) {
    float length = 0.0f;
    for (const float v : q) {
        if (!(v >= -1.01f && v <= 1.01f)) return false; // also false for NaN
        length += v * v;
    }
    return length > 0.81f && length < 1.21f;
}
inline Rotation normalized(const Rotation &q) {
    const float length = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (!(length > 1e-6f) || !std::isfinite(length)) return no_rotation;
    return {q[0] / length, q[1] / length, q[2] / length, q[3] / length};
}
inline Rotation blend(const Rotation &a, Rotation b, float t) {
    if (a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3] < 0.0f)
        for (auto &v : b) v = -v;
    t = std::clamp(t, 0.0f, 1.0f);
    return normalized({a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t, a[3] + (b[3] - a[3]) * t});
}

// ---- Sending: what this player's camera is doing.
struct Local {
    std::mutex mutex;
    std::uint8_t flags{};         // flag_filming clear: not filming
    Rotation aim = no_rotation;
    Clock::time_point updated{};
    Clock::time_point last_sent{};
    bool announced = false;       // the others were last told "filming"
};
inline Local &local() {
    static Local value;
    return value;
}
// Called every Filmer tick while the camera is in hand (flags with flag_filming), and not at all
// otherwise: a few hundred milliseconds without one is "stopped".
inline void publish_local(std::uint8_t flags, const Rotation &aim) {
    auto &l = local();
    std::lock_guard lock(l.mutex);
    l.flags = static_cast<std::uint8_t>(flags & flag_mask);
    l.aim = valid_rotation(aim) ? normalized(aim) : no_rotation;
    l.updated = Clock::now();
}
struct Outgoing {
    std::uint8_t flags;
    Rotation aim;
};
inline constexpr auto send_interval = std::chrono::milliseconds(100);
// Asked by the network every tick: something to send now, or nothing.
inline std::optional<Outgoing> next_outgoing(Clock::time_point now = Clock::now()) {
    auto &l = local();
    std::lock_guard lock(l.mutex);
    const bool filming = (l.flags & flag_filming) && l.updated != Clock::time_point{} && now - l.updated < std::chrono::milliseconds(300);
    if (filming) {
        if (l.announced && now - l.last_sent < send_interval) return std::nullopt;
        l.announced = true;
        l.last_sent = now;
        return Outgoing{l.flags, l.aim};
    }
    if (l.announced) {
        l.announced = false;
        l.last_sent = now;
        return Outgoing{0, l.aim};
    }
    return std::nullopt;
}

// ---- Receiving: who else is filming.
struct Remote {
    std::uint64_t id{};
    std::uint8_t flags{};
    Rotation aim = no_rotation;  // in a snapshot: where the camera points now
    Rotation from = no_rotation, to = no_rotation;
    Clock::time_point seen{};
    Clock::duration glide{};
};
struct Remotes {
    std::mutex mutex;
    std::vector<Remote> list;
};
inline Remotes &remotes() {
    static Remotes value;
    return value;
}
inline constexpr std::size_t max_remote = 32;
inline constexpr auto stale_after = std::chrono::milliseconds(1500);
inline float glide_fraction(const Remote &r, Clock::time_point now) {
    if (r.glide <= Clock::duration::zero()) return 1.0f;
    return std::clamp(std::chrono::duration<float>(now - r.seen) / std::chrono::duration<float>(r.glide), 0.0f, 1.0f);
}
inline void receive(std::uint64_t id, std::uint8_t flags, const Rotation &aim, Clock::time_point now = Clock::now()) {
    if (!id) return;
    const Rotation pointing = valid_rotation(aim) ? normalized(aim) : no_rotation;
    const bool filming = (flags & flag_filming) != 0;
    auto &r = remotes();
    std::lock_guard lock(r.mutex);
    for (auto it = r.list.begin(); it != r.list.end(); ++it)
        if (it->id == id) {
            if (!filming) {
                r.list.erase(it);
                return;
            }
            // The camera turns from where it is shown to the new aim over the time this update took.
            it->from = blend(it->from, it->to, glide_fraction(*it, now));
            it->to = pointing;
            it->glide = std::clamp<Clock::duration>(now - it->seen, std::chrono::milliseconds(20), std::chrono::milliseconds(250));
            it->seen = now;
            it->flags = static_cast<std::uint8_t>(flags & flag_mask);
            return;
        }
    if (!filming || r.list.size() >= max_remote) return;
    r.list.push_back({id, static_cast<std::uint8_t>(flags & flag_mask), pointing, pointing, pointing, now, {}});
}
inline void forget(std::uint64_t id) {
    auto &r = remotes();
    std::lock_guard lock(r.mutex);
    std::erase_if(r.list, [id](const Remote &x) { return x.id == id; });
}
// Whether player `id` is filming now, and with what.
inline std::optional<Remote> filming(std::uint64_t id, Clock::time_point now = Clock::now()) {
    auto &r = remotes();
    std::lock_guard lock(r.mutex);
    for (const auto &x : r.list)
        if (x.id == id && now - x.seen <= stale_after) {
            auto out = x;
            out.aim = blend(x.from, x.to, glide_fraction(x, now));
            return out;
        }
    return std::nullopt;
}
inline std::vector<Remote> snapshot(Clock::time_point now = Clock::now()) {
    auto &r = remotes();
    std::lock_guard lock(r.mutex);
    std::erase_if(r.list, [now](const Remote &x) { return now - x.seen > stale_after; });
    auto out = r.list;
    for (auto &x : out) x.aim = blend(x.from, x.to, glide_fraction(x, now));
    return out;
}
} // namespace dingosdk::filmer::beacon
