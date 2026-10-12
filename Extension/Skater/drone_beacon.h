#pragma once
// Letting other players in a lobby see where your FPV drone is (they need this build too).
// The flight tick publishes the drone's position; the session sends it a few times a second as a
// small `drone` packet (Multiplayer/Net/protocol.h); the receiver keeps the latest position of
// each player's drone and the overlay draws a marker for it (UI/Overlay/drone_hud_overlay.cpp).
// Header-only and free of game code, so the networking can use it without pulling in the flight model.
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dingosdk::drone::beacon {
using Clock = std::chrono::steady_clock;
using Position = std::array<float, 3>;

inline bool valid_position(const Position& p) {
    for (const float v : p) if (!std::isfinite(v) || std::abs(v) > 1.0e6f) return false;
    return true;
}

// ---- Sending: what this player's drone is doing.
struct Local {
    std::mutex mutex;
    bool share = false;           // the `beacon` setting
    Position position{};
    Clock::time_point updated{};
    Clock::time_point last_sent{};
    bool announced = false;       // the others were last told "flying"
};
inline Local& local() { static Local value; return value; }

// Called every flight tick.
inline void publish_local(const Position& position, bool share) {
    auto& l = local();
    std::lock_guard lock(l.mutex);
    l.share = share;
    l.position = position;
    l.updated = Clock::now();
}

struct Outgoing { bool active; Position position; };
// Asked by the network every tick: a position to send now (about five times a second while
// flying, and once to say it stopped), or nothing.
inline std::optional<Outgoing> next_outgoing(Clock::time_point now = Clock::now()) {
    auto& l = local();
    std::lock_guard lock(l.mutex);
    const bool flying = l.share && l.updated != Clock::time_point{} && now - l.updated < std::chrono::milliseconds(300) &&
                        valid_position(l.position);
    if (flying) {
        if (l.announced && now - l.last_sent < std::chrono::milliseconds(200)) return std::nullopt;
        l.announced = true;
        l.last_sent = now;
        return Outgoing{true, l.position};
    }
    if (l.announced) {
        l.announced = false;
        l.last_sent = now;
        return Outgoing{false, l.position};
    }
    return std::nullopt;
}

// ---- Receiving: other players' drones.
struct Remote {
    std::uint64_t id{};
    std::string name;
    Position position{};
    Clock::time_point seen{};
};
struct Remotes {
    std::mutex mutex;
    std::vector<Remote> list;
};
inline Remotes& remotes() { static Remotes value; return value; }
constexpr std::size_t max_remote = 32;
inline constexpr auto stale_after = std::chrono::milliseconds(1500);

inline void receive(std::uint64_t id, std::string name, const Position& position, bool active,
                    Clock::time_point now = Clock::now()) {
    if (!id || !valid_position(position)) return;
    auto& r = remotes();
    std::lock_guard lock(r.mutex);
    for (auto it = r.list.begin(); it != r.list.end(); ++it)
        if (it->id == id) {
            if (!active) { r.list.erase(it); return; }
            if (!name.empty()) it->name = std::move(name);
            it->position = position;
            it->seen = now;
            return;
        }
    if (!active || r.list.size() >= max_remote) return;
    r.list.push_back({id, std::move(name), position, now});
}
// Someone left the session.
inline void forget(std::uint64_t id) {
    auto& r = remotes();
    std::lock_guard lock(r.mutex);
    std::erase_if(r.list, [id](const Remote& x) { return x.id == id; });
}
// The drones to show now (stale ones are dropped).
inline std::vector<Remote> snapshot(Clock::time_point now = Clock::now()) {
    auto& r = remotes();
    std::lock_guard lock(r.mutex);
    std::erase_if(r.list, [now](const Remote& x) { return now - x.seen > stale_after; });
    return r.list;
}
} // namespace dingosdk::drone::beacon
