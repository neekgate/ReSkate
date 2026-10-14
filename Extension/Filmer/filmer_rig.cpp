#include "filmer_rig.h"
#include <algorithm>
#include <cmath>
#include <numbers>

namespace dingosdk::filmer {
namespace {
constexpr float pi = std::numbers::pi_v<float>;
constexpr float degrees = pi / 180.0f;

float length(const Vec3 &v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }
float flat_length(const Vec3 &v) { return std::sqrt(v[0] * v[0] + v[2] * v[2]); }
Vec3 add(const Vec3 &a, const Vec3 &b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
Vec3 sub(const Vec3 &a, const Vec3 &b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
Vec3 scale(const Vec3 &a, float s) { return {a[0] * s, a[1] * s, a[2] * s}; }
Vec3 lerp(const Vec3 &a, const Vec3 &b, float t) { return add(a, scale(sub(b, a), t)); }
bool finite(const Vec3 &v) { return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]); }
// The share of the way an eased value moves in `dt` towards its target, for a time constant `tau`.
float ease(float dt, float tau) { return tau <= 0 ? 1.0f : 1.0f - std::exp(-dt / tau); }
float mix(float a, float b, float t) { return a + (b - a) * t; }
// A second-order spring towards a moving target: no lag behind a target moving at constant speed.
void spring(float &x, float &v, float target, float target_rate, float omega, float damping, float dt) {
    // Substeps keep a stiff spring stable through a long frame.
    const int steps = std::clamp(static_cast<int>(std::ceil(dt * omega / 0.25f)), 1, 16);
    const float h = dt / static_cast<float>(steps);
    for (int i = 0; i < steps; ++i) {
        const float a = omega * omega * (target - x) + 2.0f * damping * omega * (target_rate - v);
        v += a * h;
        x += v * h;
    }
}
// The same, for an angle: the target is reached the short way round.
void angle_spring(float &x, float &v, float target, float omega, float damping, float dt) {
    const float goal = x + wrap_angle(target - x);
    spring(x, v, goal, 0.0f, omega, damping, dt);
    x = wrap_angle(x);
}
float yaw_of(float x, float z) { return std::atan2(x, z); }
} // namespace

float wrap_angle(float radians) noexcept {
    if (!std::isfinite(radians)) return 0;
    radians = std::fmod(radians + pi, 2.0f * pi);
    if (radians < 0) radians += 2.0f * pi;
    return radians - pi;
}

bool valid_camera(const Matrix &m) noexcept {
    for (float value : m)
        if (!std::isfinite(value) || std::abs(value) > 1000000.0f) return false;
    for (std::size_t row = 0; row < 3; ++row) {
        const float l = std::sqrt(m[row * 4] * m[row * 4] + m[row * 4 + 1] * m[row * 4 + 1] + m[row * 4 + 2] * m[row * 4 + 2]);
        if (std::abs(l - 1.0f) > 0.02f) return false;
        for (std::size_t other = row + 1; other < 3; ++other) {
            const float dot = m[row * 4] * m[other * 4] + m[row * 4 + 1] * m[other * 4 + 1] + m[row * 4 + 2] * m[other * 4 + 2];
            if (std::abs(dot) > 0.02f) return false;
        }
    }
    return true;
}

float Rig::Wobble::sample(double t) const noexcept {
    float total = 0;
    for (std::size_t i = 0; i < 3; ++i)
        total += std::sin(static_cast<float>(std::fmod(t * frequency[i], 1.0)) * 2.0f * pi + phase[i]) * (i == 0 ? 0.6f : i == 1 ? 0.3f : 0.15f);
    return total; // about -1..1
}

Rig::Rig() : random_(0x5658u) {
    // Each wobble sums three sines at frequencies that never line up, so the motion never repeats.
    // Rows: the slow sway of a body on a board, the hands, and the board's rattle over the ground.
    const std::array<float, 9> base{0.23f, 0.31f, 0.27f, 1.1f, 1.37f, 0.9f, 9.0f, 11.3f, 7.7f};
    std::uniform_real_distribution<float> phase(0.0f, 2.0f * pi), spread(0.85f, 1.2f);
    for (std::size_t i = 0; i < wobble_.size(); ++i)
        for (std::size_t k = 0; k < 3; ++k) {
            wobble_[i].phase[k] = phase(random_);
            wobble_[i].frequency[k] = base[i] * spread(random_) * (k == 0 ? 1.0f : k == 1 ? 1.618f : 2.414f);
        }
    reset();
}

void Rig::reset() noexcept {
    subject_ = {};
    self_ = {};
    trail_.clear();
    history_.clear();
    aim_ = {};
    operator_have_ = false;
    eye_have_ = false;
    operator_velocity_ = {};
    operator_height_rate_ = 0;
    lean_ = 0;
    angle_ = angle_target_ = 22.0f * degrees;
    angle_rate_ = 0;
    angle_until_ = 0;
    offset_yaw_ = offset_pitch_ = 0;
    pushing_ = false;
    planted_ = edge_ = holding_ = false;
    flat_air_ = false;
    unseen_since_ = -1.0;
}

void Rig::set_planted(bool planted) noexcept {
    if (planted_ && !planted && operator_have_) {
        // Riding again: from far off the filmer cuts to a new spot behind you instead of a long chase.
        const float away = flat_length(sub(subject_.position, operator_));
        if (away > 12.0f) operator_have_ = false;
    }
    planted_ = planted;
    edge_ = false;
}

void Rig::begin_tick(double now) noexcept {
    now_ = now;
    probe_budget_ = 8;
    ray_budget_ = 4;
    if (probes_.size() > 4096) probes_.clear();
}

std::optional<float> Rig::ground_at(float x, float z, float near_y, bool always) const {
    if (!cast_ || !std::isfinite(x) || !std::isfinite(z) || !std::isfinite(near_y)) return std::nullopt;
    const auto cell = [](float v, float size) { return static_cast<std::uint64_t>(static_cast<std::int64_t>(std::floor(v / size)) & 0x1fffff); };
    const std::uint64_t key = cell(x, 0.25f) | cell(z, 0.25f) << 21 | cell(near_y, 0.5f) << 42;
    const auto found = probes_.find(key);
    // The ground under the lens is asked fresh each tick, at its own spot: a cell's answer is from
    // somewhere else in the cell, up to a hand's height off on a bank.
    const bool fresh = !always && found != probes_.end() && now_ - found->second.time < 2.0;
    if (!fresh && (probe_budget_ > 0 || always)) {
        if (probe_budget_ > 0) --probe_budget_;
        Vec3 hit{};
        Probe probe{false, 0.0f, now_};
        if (cast_(Vec3{x, near_y + 0.9f, z}, Vec3{x, near_y - 6.0f, z}, hit) && std::isfinite(hit[1])) probe = {true, hit[1], now_};
        probes_[key] = probe;
        ++ground_asked_;
        ground_answers_ += ((probe.hit ? 1.0f : 0.0f) - ground_answers_) * 0.03f;
        if (!world_answers()) return near_y;
        return probe.hit ? std::optional<float>(probe.y) : std::nullopt;
    }
    if (!world_answers()) return near_y; // the game is not answering: level, the trail decides
    if (found != probes_.end()) return found->second.hit ? std::optional<float>(found->second.y) : std::nullopt;
    return near_y; // out of questions this tick and never asked here: assume it is level
}

bool Rig::blocked(const Vec3 &from, const Vec3 &to, Vec3 *hit) const {
    if (!cast_ || ray_budget_ == 0) return false;
    --ray_budget_;
    Vec3 found{};
    if (!cast_(from, to, found)) return false;
    if (hit) *hit = found;
    return true;
}

const Rig::TrailPoint *Rig::nearest_trail(float x, float z, float within) const {
    const TrailPoint *best{};
    float best_distance = within * within;
    for (const auto &point : trail_) {
        const float dx = point.at[0] - x, dz = point.at[2] - z, d = dx * dx + dz * dz;
        if (d < best_distance) {
            best_distance = d;
            best = &point;
        }
    }
    return best;
}

// Whether a filmer on a board could roll from where they are to `to` (with ground at `to_ground`):
// not up onto anything a board will not roll up, and, with stop_at_drops, not off a drop or over a
// stretch you flew over (a set, a gap): there they stop at the edge and film.
bool Rig::step_allowed(const Vec3 &, float to_ground, bool stop_at_drops) const {
    const float rise = to_ground - operator_ground_;
    if (rise > 0.3f) return false;
    if (!stop_at_drops) return true;
    return rise >= -0.85f; // a curb, a planter's edge: rolled off; a set or a big drop: not
}

// The vertical half of the view (radians): the fisheye's frame edge sees half the game camera's FOV.
float Rig::half_view(const RigSettings &s) const { return render_fov(s.lens) * 0.5f * degrees; }

// The pitch that frames a skater standing at `root` (their feet) from `eye`: the board at a fixed
// place below the centre (tilt), raised if their head would be cut, but never so far the board leaves.
float Rig::frame_pitch(const Vec3 &eye, const Vec3 &root, bool, float, const RigSettings &s) const {
    const float horizontal = std::max(flat_length(sub(root, eye)), 0.3f);
    const float feet = std::atan2(root[1] + 0.05f - eye[1], horizontal);
    const float head = std::atan2(root[1] + 1.7f - eye[1], horizontal);
    const float view = half_view(s);
    float pitch = feet + mix(0.15f, 0.75f, std::clamp(s.tilt, 0.0f, 1.0f)) * view;
    pitch = std::max(pitch, head - 0.88f * view);
    return std::min(pitch, feet + 0.82f * view);
}

void Rig::slap(double now) noexcept {
    slap_at_ = now;
    std::uniform_real_distribution<float> side(-1.0f, 1.0f);
    slap_yaw_ = side(random_) > 0 ? 1.0f : -1.0f;
    slap_pitch_ = -0.6f - 0.4f * std::abs(side(random_));
}

float Rig::angle_degrees() const noexcept { return angle_ / degrees; }

// How far the skater's root (where the game says they are) is above the ground under their board.
// The game's root can sit at the hips: everything that compares the trail (roots) with the ground the
// game reports (the level, the ledges, the gaps, the framing of the feet) goes through this.
void Rig::learn_root(float dt, const Vec3 &root) {
    if (!cast_ || !world_answers()) return;
    const auto under = ground_at(root[0], root[2], root[1]);
    if (!under) return;
    const float d = root[1] - *under;
    if (d < -0.3f || d > 1.6f) return;
    if (!root_known_) {
        root_samples_.push_back(d);
        if (root_samples_.size() >= 15) {
            std::vector<float> sorted = root_samples_;
            std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(sorted.size() / 2), sorted.end());
            root_above_ground_ = sorted[sorted.size() / 2];
            root_known_ = true;
            root_samples_.clear();
        }
    } else if (std::abs(d - root_above_ground_) < 0.15f) {
        root_above_ground_ = mix(root_above_ground_, d, ease(dt, 3.0f));
    }
}

// The least the lens is ever above the ground: closer, the game's near clipping plane cuts into the
// ground at the corners of the wide lens and the picture shows under the floor.
constexpr float lens_clearance = 0.12f;

// Follows a skater from tick to tick: speed, the way they travel, whether they are in the air and
// the ground under them. A jump of more than 8 m is a teleport or a respawn: everything restarts.
void Rig::track(double now, float dt, const Subject &subject, Tracked &t, bool own_trail, bool learn) {
    Vec3 p = subject.position;
    if (!t.have || length(sub(p, t.position)) > 8.0f || now - t.last_seen > 1.0) {
        t = {};
        t.have = true;
        t.position = p;
        t.samples.push_back({now, p});
        const float fl = std::sqrt(subject.facing[0] * subject.facing[0] + subject.facing[2] * subject.facing[2]);
        if (fl > 0.01f) { t.heading_x = subject.facing[0] / fl; t.heading_z = subject.facing[2] / fl; }
        t.ground = p[1];
        t.last_seen = now;
        operator_have_ = false;
        if (own_trail) {
            trail_.clear();
            history_.clear();
            aim_.have = false;
        }
        return;
    }
    t.last_seen = now;
    // A new sample from the game, or the same one again (its physics has not stepped since)?
    if (t.samples.empty() || length(sub(p, t.samples.back().second)) > 1e-5f) {
        if (!t.samples.empty()) {
            const double gap = now - t.samples.back().first;
            if (gap > 0.002 && gap < 0.1) t.interval = mix(static_cast<float>(t.interval), static_cast<float>(gap), 0.1f);
        }
        t.samples.push_back({now, p});
        while (t.samples.size() > 8) t.samples.pop_front();
    }
    // Where the game draws them now: between its last two samples, as its picture does.
    {
        const double when = now - t.interval;
        const auto &first = t.samples.front();
        if (when <= first.first) p = first.second;
        else {
            p = t.samples.back().second;
            for (std::size_t i = t.samples.size(); i-- > 1;) {
                const auto &a = t.samples[i - 1], &b = t.samples[i];
                if (a.first <= when && when <= b.first) {
                    const double span = b.first - a.first;
                    p = lerp(a.second, b.second, span > 1e-6 ? static_cast<float>((when - a.first) / span) : 1.0f);
                    break;
                }
            }
        }
    }
    const float step = std::max(dt, 0.001f);
    const Vec3 raw = scale(sub(p, t.position), 1.0f / step);
    t.velocity = lerp(t.velocity, raw, ease(dt, 0.07f));
    t.position = p;
    const float speed = flat_length(t.velocity);
    // The heading follows the travel a little behind, so the filmer swings round smoothly; slowly
    // towards the way the skater faces when they stop.
    float hx = t.heading_x, hz = t.heading_z;
    if (speed > 0.8f) {
        const float k = ease(dt, 0.3f);
        hx = mix(hx, t.velocity[0] / speed, k);
        hz = mix(hz, t.velocity[2] / speed, k);
    } else {
        const float fl = std::sqrt(subject.facing[0] * subject.facing[0] + subject.facing[2] * subject.facing[2]);
        if (fl > 0.01f) {
            const float k = ease(dt, 1.5f);
            hx = mix(hx, subject.facing[0] / fl, k);
            hz = mix(hz, subject.facing[2] / fl, k);
        }
    }
    const float hl = std::sqrt(hx * hx + hz * hz);
    if (hl > 0.01f) { t.heading_x = hx / hl; t.heading_z = hz / hl; }
    // In the air: told for your own skater; for others, rising fast or well above the last ground.
    bool air = false;
    if (subject.airborne) air = *subject.airborne;
    else air = (t.airborne && p[1] > t.ground + 0.15f) || t.velocity[1] > 2.2f || p[1] > t.ground + 0.45f;
    if (subject.bailing) air = false;
    if (air && !t.airborne) {
        t.air_since = now;
        if (own_trail) {
            // A flatground trick, for the lunge: popped off level ground (not a ramp or a bank: the last
            // metre of trail was flat, and the pop not too steep), towards level ground.
            bool flat = t.velocity[1] < 4.0f;
            float seen = 0.0f;
            Vec3 previous{p[0], t.ground, p[2]};
            for (auto it = trail_.rbegin(); it != trail_.rend() && seen < 1.2f; ++it) {
                if (std::abs(it->at[1] - t.ground) > 0.12f) flat = false;
                seen += flat_length(sub(it->at, previous));
                previous = it->at;
            }
            if (flat && cast_) {
                const float speed_now = flat_length(t.velocity);
                if (speed_now > 0.5f)
                    for (const float ahead : {1.0f, 2.0f}) {
                        const auto g = ground_at(p[0] + t.velocity[0] / speed_now * ahead, p[2] + t.velocity[2] / speed_now * ahead, t.ground);
                        if (!g || std::abs(*g - world(t.ground)) > 0.15f) flat = false;
                    }
            }
            flat_air_ = flat;
            takeoff_height_ = t.ground;
        }
    }
    // Not flatground after all: a big air, or going down a set.
    if (air && own_trail && (p[1] > takeoff_height_ + 1.0f || p[1] < takeoff_height_ - 0.3f)) flat_air_ = false;
    // Nobody is in the air for four seconds: the ground was guessed wrong (a ramp, a lift).
    if (air && now - t.air_since > 4.0) air = false;
    const bool landed = t.airborne && !air;
    t.airborne = air;
    // On the ground, the ground is where the skater is: a step (onto a ledge, off one) is a step, not a
    // slope, so it is not smoothed over; the root's bob is.
    if (!air) t.ground = landed || std::abs(p[1] - t.ground) > 0.2f ? p[1] : mix(t.ground, p[1], ease(dt, 0.15f));
    else if (p[1] < t.ground) t.ground = p[1]; // dropped below where they left: a drop-in or a stair set
    if (learn && !air) learn_root(dt, p);
    // The trail: where the skater has been, at their feet. That ground is known to be clear, so the
    // filmer rides it.
    if (own_trail) {
        const Vec3 foot{p[0], t.ground, p[2]};
        if (trail_.empty() || flat_length(sub(foot, trail_.back().at)) > 0.12f) {
            trail_.push_back({foot, t.airborne, t.ground, next_trail_id_++});
            while (trail_.size() > 600) trail_.pop_front();
        }
        if (t.airborne) landed_since_ = now;
    }
}

// The point `arc` metres back along the trail from the skater; `fallback` when it is shorter.
Vec3 Rig::trail_point(float arc, Vec3 fallback) const {
    if (trail_.size() < 2) return fallback;
    float left = arc;
    Vec3 previous = subject_.position;
    previous[1] = subject_.ground;
    for (auto it = trail_.rbegin(); it != trail_.rend(); ++it) {
        const Vec3 &at = it->at;
        const float piece = flat_length(sub(at, previous));
        if (piece >= left && piece > 0.0001f) return lerp(previous, at, left / piece);
        left -= piece;
        previous = at;
    }
    // Not that much trail yet: carry on straight back from its end.
    const Vec3 end = trail_.front().at;
    Vec3 back{-subject_.heading_x, 0, -subject_.heading_z};
    return add(end, scale(back, left));
}

std::optional<std::size_t> Rig::trail_index_near(float x, float z, float within) const {
    std::optional<std::size_t> best;
    float best_distance = within * within;
    for (std::size_t i = 0; i < trail_.size(); ++i) {
        const float dx = trail_[i].at[0] - x, dz = trail_[i].at[2] - z, d = dx * dx + dz * dz;
        if (d < best_distance) { best_distance = d; best = i; }
    }
    return best;
}

// The trail point `arc` metres back from the skater.
std::size_t Rig::trail_index_back(float arc) const {
    if (trail_.empty()) return 0;
    float left = arc;
    Vec3 previous{subject_.position[0], subject_.ground, subject_.position[2]};
    for (std::size_t i = trail_.size(); i-- > 0;) {
        left -= flat_length(sub(trail_[i].at, previous));
        previous = trail_[i].at;
        if (left <= 0.0f) return i;
    }
    return 0;
}

// From trail point `from` (where the filmer is, at `level`) towards `to`. The filmer follows the
// ground you rolled on up and down banks and slopes, step by step. Where you went up onto something
// (a ledge, a rail) they stay down at their level and ride alongside. Where you came down from their
// level all at once (a set, a drop, off the end of a ledge they were not on) they stop at the top;
// and where you jumped a gap at the same level (the ground under the jump missing or far below) too.
// A jump you are still in the air for: they wait at the takeoff.
Rig::Walk Rig::walk(std::size_t from, std::size_t to, float level, bool stop_at_drops) const {
    Walk result{from, level, false, false};
    constexpr std::size_t none = static_cast<std::size_t>(-1);
    std::size_t air_start = none;
    for (std::size_t k = from + 1; k <= to && k < trail_.size(); ++k) {
        const auto &point = trail_[k];
        if (point.air) {
            if (air_start == none) air_start = k;
            continue;
        }
        const float ground = point.at[1];
        const std::size_t before = air_start != none ? (air_start > 0 ? air_start - 1 : 0) : k - 1;
        if (ground < result.level - 0.8f) {
            // Down from the filmer's level in one go, further than a curb or a planter: a set, a drop.
            if (stop_at_drops) { result.edge = true; result.reach = before; return result; }
            result.level = ground;
        } else if (ground < result.level - 0.35f) {
            result.level = ground; // a curb, off a planter: rolled off after you
        } else if (air_start != none && std::abs(ground - result.level) <= 0.35f && cast_) {
            // Over a jump at the same level: is there ground under it (an ollie), or nothing (a gap)?
            const Vec3 &a = trail_[before].at;
            const float span = flat_length(sub(point.at, a));
            if (span > 1.2f) {
                for (const float u : {0.33f, 0.5f, 0.67f}) {
                    const auto g = ground_at(a[0] + (point.at[0] - a[0]) * u, a[2] + (point.at[2] - a[2]) * u, result.level);
                    if (!g || *g < world(result.level) - 0.8f) {
                        if (stop_at_drops) { result.edge = true; result.reach = before; return result; }
                        break;
                    }
                }
            }
            result.level = ground;
        } else if (std::abs(ground - result.level) <= 0.35f) {
            result.level = ground; // rolling along, up and down banks and slopes
        }
        // (Higher up, after a jump: on a ledge or a rail. The filmer stays at their level.)
        air_start = none;
        result.reach = k;
    }
    // Still in the air at the end: if the ground under the jump so far is the filmer's level (an
    // ollie on flat), they ride on; otherwise (or with no way to tell) they wait at the takeoff until
    // you land.
    if (air_start != none) {
        bool level_ground = static_cast<bool>(cast_);
        for (std::size_t k = air_start; level_ground && k <= to && k < trail_.size(); k += 3) {
            const auto g = ground_at(trail_[k].at[0], trail_[k].at[2], result.level);
            if (!g || std::abs(*g - world(result.level)) > 0.3f) level_ground = false;
        }
        if (!level_ground) {
            result.hold = true;
            result.reach = air_start > 0 ? air_start - 1 : 0;
        } else {
            result.reach = std::min(to, trail_.size() - 1);
        }
    }
    return result;
}

// Auto: a filmer picks angles. Mostly a little off to one side behind, now and then wide, rarely out
// in front; never for less than a few seconds.
void Rig::choose_angle(double now, float speed) {
    if (now < angle_until_) return;
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    const float roll = unit(random_);
    const float side = angle_target_ >= 0 ? 1.0f : -1.0f;
    float keep_side = unit(random_) < 0.65f ? side : -side;
    if (side_lock_ != 0.0f) keep_side = side_lock_;
    float pick;
    double hold;
    if (roll < 0.5f) { pick = (18.0f + 22.0f * unit(random_)) * keep_side; hold = 5.0 + 6.0 * unit(random_); }
    else if (roll < 0.85f) { pick = (55.0f + 25.0f * unit(random_)) * keep_side; hold = 3.5 + 4.0 * unit(random_); }
    else if (speed > 3.5f) { pick = (160.0f + 15.0f * unit(random_)) * keep_side; hold = 3.0 + 2.5 * unit(random_); }
    else { pick = 28.0f * keep_side; hold = 4.0; }
    angle_target_ = pick * degrees;
    angle_until_ = now + hold;
}

// Where in the skater the filmer points: low on the body with the fisheye (feet and board in frame),
// a little ahead of where they are going; higher and further ahead through an air.
// Where the filmer is looking: the skater's feet, a little ahead of where they are going (further ahead
// through an air). The height of the frame comes from frame_pitch.
Vec3 Rig::aim_point(const Tracked &t, const RigSettings &) const {
    const float lead = t.airborne ? 0.16f : 0.12f;
    Vec3 point = t.position;
    point[1] -= root_above_ground_; // the board, not the root
    point[0] += t.velocity[0] * lead;
    point[2] += t.velocity[2] * lead;
    if (t.airborne) point[1] += std::clamp(t.velocity[1] * 0.08f, -0.4f, 0.4f);
    return point;
}

// Where the aim point was `delay` seconds ago: a person sees the move, then turns.
Vec3 Rig::delayed_point(double now, float delay) const {
    if (history_.empty()) return {};
    const double when = now - delay;
    for (std::size_t i = history_.size(); i-- > 0;) {
        if (history_[i].time <= when) {
            if (i + 1 < history_.size()) {
                const auto &a = history_[i], &b = history_[i + 1];
                const double span = b.time - a.time;
                const float t = span > 1e-6 ? static_cast<float>((when - a.time) / span) : 0.0f;
                return lerp(a.point, b.point, std::clamp(t, 0.0f, 1.0f));
            }
            return history_[i].point;
        }
    }
    return history_.front().point;
}

void Rig::steer_aim(float dt, float want_yaw, float want_pitch, float omega, float damping) {
    if (!aim_.have) {
        aim_ = {want_yaw, want_pitch, 0, 0, true};
        return;
    }
    angle_spring(aim_.yaw, aim_.yaw_rate, want_yaw, omega, damping, dt);
    spring(aim_.pitch, aim_.pitch_rate, std::clamp(want_pitch, -1.35f, 1.35f), 0.0f, omega, damping, dt);
    aim_.pitch = std::clamp(aim_.pitch, -1.4f, 1.4f);
}

// The camera from its position and angles. Right, up and back are built as the game's own follow
// camera builds them (Extension/Multiplayer/Hud/follow_camera.cpp), then turned about the view
// by `roll` (positive drops the right side).
Matrix Rig::build(const Vec3 &eye, float yaw, float pitch, float roll) const {
    const float cp = std::cos(pitch), sp = std::sin(pitch);
    const Vec3 forward{std::sin(yaw) * cp, sp, std::cos(yaw) * cp};
    const Vec3 back = scale(forward, -1.0f);
    Vec3 right{back[2], 0, -back[0]};
    const float fl = std::sqrt(right[0] * right[0] + right[2] * right[2]);
    if (fl > 0.0001f) { right[0] /= fl; right[2] /= fl; } else right = {1, 0, 0};
    Vec3 up{back[1] * right[2] - back[2] * right[1], back[2] * right[0] - back[0] * right[2], back[0] * right[1] - back[1] * right[0]};
    const float cr = std::cos(roll), sr = std::sin(roll);
    const Vec3 rolled_right = sub(scale(right, cr), scale(up, sr));
    const Vec3 rolled_up = add(scale(up, cr), scale(right, sr));
    return {rolled_right[0], rolled_right[1], rolled_right[2], 0, rolled_up[0], rolled_up[1], rolled_up[2], 0,
            back[0], back[1], back[2], 0, eye[0], eye[1], eye[2], 1};
}

// Handheld: the body swaying on the board, the hands, the board rattling over the ground faster the
// faster it rolls, and a hand knocking the lens.
void Rig::shake(double now, float speed, float human, float &yaw, float &pitch, float &roll, Vec3 &eye) const {
    const float h = std::clamp(human, 0.0f, 1.0f);
    const float rattle = std::clamp(speed / 8.0f, 0.0f, 1.4f) * (0.25f + 0.75f * h);
    yaw += (wobble_[0].sample(now) * 1.3f * h + wobble_[3].sample(now) * 0.45f * h + wobble_[6].sample(now) * 0.22f * rattle) * degrees;
    pitch += (wobble_[1].sample(now) * 0.9f * h + wobble_[4].sample(now) * 0.4f * h + wobble_[7].sample(now) * 0.3f * rattle) * degrees;
    roll += (wobble_[2].sample(now) * 1.6f * h + wobble_[5].sample(now) * 0.5f * h + wobble_[8].sample(now) * 0.25f * rattle) * degrees;
    eye[1] += wobble_[7].sample(now * 0.5) * 0.008f * rattle + wobble_[1].sample(now) * 0.02f * h;
    // The smack: the palm hits the glass 0.1 s after the slap starts and knocks the camera back hard,
    // it shakes while the hand is held there, and recovers as the hand comes off.
    const double since = now - slap_at_ - slap_contact;
    if (since >= 0 && since < 0.6) {
        const float t = static_cast<float>(since);
        const float kick = (1.0f - std::exp(-t * 60.0f)) * std::exp(-t * 7.0f);
        const float buzz = std::sin(t * 70.0f) * std::exp(-t * 10.0f);
        yaw += slap_yaw_ * (kick * 5.0f + buzz * 0.8f) * degrees;
        pitch += slap_pitch_ * (kick * 6.0f + buzz * 0.6f) * degrees;
        roll += slap_yaw_ * kick * 4.0f * degrees;
        eye[1] -= kick * 0.04f;
    }
}

Frame Rig::follow(double now, float dt, const Subject &subject, const RigSettings &s) {
    if (!subject.valid || !finite(subject.position)) return {};
    dt = std::clamp(dt, 0.0f, 0.1f);
    begin_tick(now);
    track(now, dt, subject, subject_, true, true);
    const auto &t = subject_;
    const float speed = flat_length(t.velocity);
    const float human = std::clamp(s.human, 0.0f, 1.0f), reaction = std::clamp(s.reaction, 0.0f, 1.0f);
    const bool fish = s.lens == Lens::fisheye;
    float distance = std::clamp(s.distance, 0.6f, 6.0f) * (fish ? 1.0f : 3.2f);
    // The lunge: with the fisheye, on a flatground trick the filmer pushes in closer and lower under
    // it, and eases back out as you land and ride away.
    const bool lunging = s.lunge && fish && t.airborne && flat_air_ && !planted_ && !edge_ && !holding_;
    spring(lunge_, lunge_rate_, lunging ? 1.0f : 0.0f, 0.0f, lunging ? 6.0f : 2.5f, 1.0f, dt);
    lunge_ = std::clamp(lunge_, 0.0f, 1.0f);
    // Pull back a little at speed and in the air, so a trick stays in the frame (unless lunging in).
    distance += std::min(speed * 0.04f, 0.5f);
    if (t.airborne) distance += (std::clamp(t.position[1] - t.ground, 0.0f, 3.0f) * 0.35f + 0.3f) * (1.0f - lunge_);
    distance = std::max(distance * mix(1.0f, 0.65f, lunge_), std::min(distance, 0.9f));
    // The side they keep to.
    side_lock_ = s.side == Side::left ? 1.0f : s.side == Side::right ? -1.0f : 0.0f;
    const float sign = side_lock_ != 0.0f ? side_lock_ : (angle_target_ >= 0 ? 1.0f : -1.0f);
    // The angle round the skater.
    switch (s.style) {
    case Style::automatic:
        choose_angle(now, speed);
        if (side_lock_ != 0.0f) angle_target_ = std::abs(angle_target_) * side_lock_;
        break;
    case Style::behind: angle_target_ = 14.0f * degrees * sign; break;
    case Style::side: angle_target_ = 72.0f * degrees * sign; break;
    case Style::ahead: angle_target_ = 172.0f * degrees * sign; break;
    }
    // Moving round to a new angle takes the time a filmer needs to ride there.
    spring(angle_, angle_rate_, angle_target_, 0.0f, 0.9f, 1.0f, dt);
    // A filmer never holds exactly the same spot.
    const float wander_angle = wobble_[0].sample(now * 0.31) * 9.0f * degrees * human;
    const float wander_distance = wobble_[1].sample(now * 0.27) * 0.35f * human;
    const float angle = angle_ + wander_angle;
    const float d = std::max(0.5f, distance + wander_distance);
    const Vec3 heading{t.heading_x, 0, t.heading_z};
    const Vec3 side{t.heading_z, 0, -t.heading_x};
    const float height = std::max(0.08f, std::clamp(s.height, 0.08f, 2.5f) * mix(1.0f, 0.75f, lunge_));
    // Stopped at a drop and you rode on: once you are well away on the ground (or long out of sight),
    // the filmer cuts to a new spot behind you, as if the next clip. Stopped by you (the left stick),
    // they stay until you send them riding again.
    if (operator_have_ && edge_ && !t.airborne && now - landed_since_ > 1.5 &&
        (flat_length(sub(t.position, operator_)) > 25.0f || (unseen_since_ >= 0 && now - unseen_since_ > 2.0))) {
        operator_have_ = false;
        edge_ = false;
    }
    // Lost sight of you for a while when riding along: cut too.
    if (!planted_ && !edge_ && !holding_ && unseen_since_ >= 0 && now - unseen_since_ > 2.5) operator_have_ = false;
    // Where on the trail the filmer is, and the level they ride at.
    std::optional<std::size_t> at_index;
    if (operator_have_) {
        at_index = trail_index_near(operator_[0], operator_[2], 4.0f);
        if (at_index && *at_index > last_index_) {
            // Ridden on along the trail since the last tick: follow its slopes to here.
            level_ = walk(last_index_, *at_index, level_, false).level;
        }
        if (at_index) last_index_ = *at_index;
    }
    Vec3 desired;
    float ground = level_;
    bool edge = false;
    holding_ = false;
    Vec3 base{};
    float offset = 0.0f;
    bool raised_ahead = false;
    if (std::abs(angle) <= 100.0f * degrees && trail_.size() >= 2) {
        // Behind and beside: on the trail the skater left, out to the side; never past a drop, a gap
        // or a jump you are still in the air for.
        const float arc = std::max(0.25f, d * std::cos(angle));
        std::size_t index = trail_index_back(arc);
        if (operator_have_ && at_index && *at_index < index) {
            const auto w = walk(*at_index, index, level_, s.stop_at_drops);
            ground = w.level;
            edge = w.edge;
            holding_ = w.hold;
            if (w.edge || w.hold) {
                index = w.reach;
                // Which way is over the edge: along the trail from its last point.
                const auto &here = trail_[index].at;
                const auto &next = trail_[std::min(index + 1, trail_.size() - 1)].at;
                const Vec3 along_trail = sub(next, here);
                const float l = flat_length(along_trail);
                edge_dir_ = l > 0.01f ? Vec3{along_trail[0] / l, 0, along_trail[2] / l} : heading;
                edge_point_ = here;
            }
        } else if (!operator_have_) {
            ground = trail_[index].at[1];
        }
        base = trail_[index].at;
        offset = d * std::sin(angle);
        // Something raised just ahead on your line (the planter you ollied onto): the filmer starts
        // out round it before they reach it, not once they are nosed up against it.
        const std::size_t ahead = trail_index_back(std::max(0.25f, arc - 2.5f));
        for (std::size_t i = index; i <= ahead && i < trail_.size(); ++i)
            if (!trail_[i].air && trail_[i].at[1] > ground + 0.3f) { raised_ahead = true; break; }
    } else {
        // Ahead, riding backwards up the line in front of them (or no trail yet).
        base = sub(Vec3{t.position[0], t.ground, t.position[2]}, scale(heading, d * std::cos(angle)));
        offset = d * std::sin(angle);
        if (!operator_have_) ground = t.ground;
    }
    // Out to the side only where the ground is the filmer's level: not over a gap or off the end of a
    // ledge. Where you are up on something (a ledge, a rail, a planter) they keep at least a metre off
    // your line, beside it, never on it. The share of the offset used eases, so the spot never jumps.
    const bool raised = raised_ahead || base[1] > ground + 0.3f;
    const float lean = offset == 0.0f ? sign : (offset > 0.0f ? 1.0f : -1.0f);
    if (raised) {
        // Beside whatever you are up on: the nearest spot out to the side (yours first, then the other)
        // whose ground is the filmer's level, so they ride round it, never over it.
        float wanted = std::max(std::abs(offset), 1.0f) * lean;
        if (cast_) {
            bool found = false;
            for (const float flip : {1.0f, -1.0f}) {
                for (const float out : {std::max(std::abs(offset), 1.0f), 1.6f, 2.2f, 2.8f, 3.5f}) {
                    // Level there and half a metre further in towards your line: room to ride by.
                    const Vec3 spot = add(base, scale(side, out * lean * flip));
                    const Vec3 inner = add(base, scale(side, (out - 0.5f) * lean * flip));
                    const auto g = ground_at(spot[0], spot[2], ground);
                    const auto gi = ground_at(inner[0], inner[2], ground);
                    if (g && gi && std::abs(*g - world(ground)) < 0.3f && std::abs(*gi - world(ground)) < 0.3f) {
                        wanted = out * lean * flip;
                        found = true;
                        break;
                    }
                }
                if (found) break;
            }
        }
        raised_offset_ += (wanted - raised_offset_) * ease(dt, 0.25f);
        side_keep_ = 1.0f;
        desired = add(Vec3{base[0], ground, base[2]}, scale(side, raised_offset_));
    } else {
        // Out to the side only where the ground is the filmer's level (not over a gap or off the end of
        // a ledge): otherwise closer in. The share of the offset used eases, so the spot never jumps.
        float keep_target = 1.0f;
        if (cast_ && std::abs(offset) > 0.05f) {
            keep_target = 0.0f;
            for (const float keep : {1.0f, 0.5f}) {
                const Vec3 spot = add(base, scale(side, offset * keep));
                const auto g = ground_at(spot[0], spot[2], ground);
                if (g && std::abs(*g - world(ground)) < 0.3f) { keep_target = keep; break; }
            }
        }
        side_keep_ += (keep_target - side_keep_) * ease(dt, 0.5f);
        raised_offset_ = offset * side_keep_;
        desired = add(Vec3{base[0], ground, base[2]}, scale(side, raised_offset_));
    }
    const Vec3 centre{t.position[0], world(t.position[1]) + 0.9f, t.position[2]};
    // Never behind a wall: where the spot is out of sight of the skater, it comes in towards them.
    if (cast_ && !edge && !holding_) {
        const Vec3 spot{desired[0], ground + height + 0.1f, desired[2]};
        Vec3 hit{};
        if (blocked(centre, spot, &hit)) {
            const Vec3 to = sub(spot, centre);
            const float full = flat_length(to);
            if (full > 0.01f) {
                const float keep = std::max(0.5f, flat_length(sub(hit, centre)) - 0.4f);
                desired = add(Vec3{centre[0], ground, centre[2]}, scale(Vec3{to[0], 0, to[2]}, keep / full));
            }
        }
    }
    if (edge && !edge_) edge_since_ = now;
    edge_ = edge;
    // A new spot: far off the one wanted (a teleport, a respawn), or after a cut. Never while stopped.
    if (!operator_have_ || (!planted_ && !edge_ && !holding_ && length(sub(operator_, desired)) > 15.0f)) {
        operator_ = desired;
        operator_velocity_ = t.velocity;
        operator_velocity_[1] = 0;
        level_ = ground;
        operator_ground_ = cast_ ? ground_at(desired[0], desired[2], ground).value_or(world(ground)) : ground;
        operator_floor_ = operator_ground_;
        operator_height_ = operator_ground_ + height;
        operator_height_rate_ = 0;
        last_desired_ = desired;
        last_index_ = trail_index_near(desired[0], desired[2], 4.0f).value_or(trail_.empty() ? 0 : trail_.size() - 1);
        operator_have_ = true;
        edge_ = holding_ = false;
        unseen_since_ = -1.0;
    }
    // Riding with you: at your speed (not the spot's jumps from tick to tick).
    const Vec3 desired_rate{t.velocity[0], 0.0f, t.velocity[2]};
    last_desired_ = desired;
    const Vec3 before = operator_;
    if (planted_) {
        operator_velocity_ = {};
    } else {
        const float omega = mix(mix(3.2f, 2.0f, reaction), 4.5f, lunge_);
        const bool stopping = edge_ || holding_;
        for (std::size_t axis : {std::size_t{0}, std::size_t{2}})
            spring(operator_[axis], operator_velocity_[axis], desired[axis],
                   stopping ? 0.0f : std::clamp(desired_rate[axis], -25.0f, 25.0f), omega, 1.0f, dt);
    }
    // Stopping at an edge: never past it, however fast they were riding (they brake to it).
    if (edge_ || holding_) {
        const float past = (operator_[0] - edge_point_[0]) * edge_dir_[0] + (operator_[2] - edge_point_[2]) * edge_dir_[2];
        if (past > 0.0f) {
            operator_[0] -= edge_dir_[0] * past;
            operator_[2] -= edge_dir_[2] * past;
            const float v = operator_velocity_[0] * edge_dir_[0] + operator_velocity_[2] * edge_dir_[2];
            if (v > 0.0f) { operator_velocity_[0] -= edge_dir_[0] * v; operator_velocity_[2] -= edge_dir_[2] * v; }
        }
    }
    if (cast_ && !planted_) {
        // Each step is checked against the world: real ground under it, no wall in the way, nothing a
        // board would not roll up or off. Blocked, the filmer slides along what stopped them, or stays.
        const auto try_step = [&](const Vec3 &to) -> bool {
            const auto g = ground_at(to[0], to[2], operator_ground_ + 0.5f, true); // from well above: a rise is found, never missed
            if (!g || !step_allowed(to, *g, s.stop_at_drops)) return false;
            if (blocked(Vec3{before[0], operator_ground_ + 0.45f, before[2]}, Vec3{to[0], *g + 0.45f, to[2]})) return false;
            operator_ground_ = *g;
            return true;
        };
        if (flat_length(sub(operator_, before)) > 0.0005f && !try_step(operator_)) {
            // Nosed up against something (a planter, a ledge's end): step round it, out to the side
            // of the line they want, at a walking pace. Failing that, slide along it, or stay.
            const Vec3 moved = sub(operator_, before);
            const float l = flat_length(moved);
            const Vec3 across{-moved[2] / l, 0.0f, moved[0] / l};
            const float which = (desired[0] - before[0]) * across[0] + (desired[2] - before[2]) * across[2] >= 0.0f ? 1.0f : -1.0f;
            const float sidestep = std::max(l, 2.5f * dt);
            const Vec3 round = add(before, scale(across, which * sidestep));
            const Vec3 along_x{operator_[0], 0, before[2]}, along_z{before[0], 0, operator_[2]};
            if (try_step(round)) { operator_ = round; operator_velocity_ = scale(across, which * std::min(sidestep / std::max(dt, 1e-3f), 4.0f)); }
            else if (try_step(along_x)) { operator_ = along_x; operator_velocity_[2] = 0; }
            else if (try_step(along_z)) { operator_ = along_z; operator_velocity_[0] = 0; }
            else { operator_ = before; operator_velocity_ = {}; }
        }
        operator_[1] = 0;
    } else if (!cast_) {
        operator_ground_ = ground; // no world to ask: the level from the trail
    }
    // The camera's height: over the ground under the filmer, smoothed (rolling down a bank, over a
    // crack), never below it.
    // Ground rising under them (a bank, a ramp, a kerb they rode up): taken at once, so the lens never
    // sinks into it. Falling away: eased, so an odd answer never drops the camera with a jolt.
    if (operator_ground_ > operator_floor_) operator_floor_ = operator_ground_;
    else operator_floor_ += (operator_ground_ - operator_floor_) * ease(dt, 0.12f);
    spring(operator_height_, operator_height_rate_, operator_floor_ + height, 0.0f, 5.5f, 0.8f, dt);
    operator_height_ = std::max(operator_height_, operator_floor_ + lens_clearance);
    const float operator_speed = flat_length(operator_velocity_);
    // Pushing to keep up: a dip and a nod every stride.
    const float behind = flat_length(sub(desired, operator_));
    pushing_ = !planted_ && !edge_ && speed > 1.5f && (behind > 0.6f || operator_speed < speed - 0.6f);
    if (pushing_ || std::fmod(push_phase_, 1.0f) > 0.02f) push_phase_ += dt / 0.95f;
    const float stride = std::fmod(push_phase_, 1.0f);
    const float dip = stride < 0.4f ? std::sin(stride / 0.4f * pi) : 0.0f;
    Vec3 eye{operator_[0], std::max(operator_height_ - dip * 0.03f * (0.4f + human), operator_floor_ + lens_clearance), operator_[2]};
    // The lens travels along the path the filmer rode, at their eased speed: a step the world refused
    // for a tick (and the catch-up after it) is never a jolt in the picture, and as the path is ground
    // they rode, it never cuts a corner over something.
    {
        const Vec3 here{operator_[0], operator_floor_, operator_[2]};
        if (!eye_have_ || lens_path_.empty() || flat_length(sub(here, lens_path_.back().at)) > 2.0f) {
            lens_path_.clear();
            lens_path_.push_back({here, 0.0f});
            lens_at_ = 0.0f;
            lens_speed_ = 0.0f;
            eye_have_ = true;
        } else if (flat_length(sub(here, lens_path_.back().at)) > 0.002f) {
            lens_path_.push_back({here, lens_path_.back().along + flat_length(sub(here, lens_path_.back().at))});
        } else {
            lens_path_.back().at[1] = operator_floor_;
        }
        const float end = lens_path_.back().along;
        const float body_speed = dt > 0.0f ? flat_length(sub(here, eye_last_)) / dt : 0.0f;
        eye_last_ = here;
        // Eased speed, a hand's length behind at a roll (so a stalled tick has slack to ride through),
        // and a pull on the gap, so they never fall further behind and close it when stopped.
        lens_speed_ += (std::min(body_speed, 25.0f) - lens_speed_) * ease(dt, 0.3f);
        const float slack = std::min(0.25f, lens_speed_ * 0.05f);
        lens_at_ = std::min(end, lens_at_ + std::max(0.0f, lens_speed_ + (end - lens_at_ - slack) * 2.0f) * dt);
        if (end - lens_at_ > 0.6f) lens_at_ = end - 0.6f; // never far off the body
        while (lens_path_.size() > 2 && lens_path_[1].along <= lens_at_) lens_path_.pop_front();
        Vec3 at = lens_path_.back().at;
        for (std::size_t i = 1; i < lens_path_.size(); ++i)
            if (lens_path_[i].along >= lens_at_) {
                const auto &p0 = lens_path_[i - 1], &p1 = lens_path_[i];
                const float span = p1.along - p0.along;
                at = lerp(p0.at, p1.at, span > 1e-5f ? std::clamp((lens_at_ - p0.along) / span, 0.0f, 1.0f) : 1.0f);
                break;
            }
        eye[0] = at[0];
        eye[2] = at[2];
        lens_ground_ = std::max(at[1], operator_floor_ - 0.3f);
    }
    eye[1] = std::max(eye[1], lens_ground_ + lens_clearance);
    // Can the filmer see you? (For the cut when they lose you.)
    if (cast_) {
        const bool hidden = blocked(eye, centre);
        if (!hidden) unseen_since_ = -1.0;
        else if (unseen_since_ < 0) unseen_since_ = now;
    }
    // Aim: at where the skater was a moment ago (reaction), a little off-centre (a person framing),
    // caught up hard when the skater is about to leave the frame (a filmer never loses a trick).
    const Vec3 now_point = aim_point(t, s);
    history_.push_back({now, now_point});
    while (!history_.empty() && now - history_.front().time > 0.6) history_.pop_front();
    const Vec3 late = delayed_point(now, 0.04f + 0.16f * reaction * (0.5f + human));
    // Stopped (the left stick, or braked to a stop at the top of a drop): the camera stays on the
    // framing it had, held by hand (it does not follow you round). Otherwise it tracks you.
    if (!edge_) edge_locked_ = false;
    else if (operator_speed < 0.5f) edge_locked_ = true; // braked to a stop at the lip
    if (!(planted_ || edge_locked_) || !aim_.have) {
        const Vec3 to_late = sub(late, eye), to_now = sub(now_point, eye);
        float want_yaw = yaw_of(to_late[0], to_late[2]);
        float want_pitch = frame_pitch(eye, late, t.airborne, 0.0f, s);
        want_yaw += wobble_[2].sample(now * 0.4) * 3.5f * degrees * human;
        want_pitch += wobble_[3].sample(now * 0.17) * 2.5f * degrees * human;
        const float true_yaw = yaw_of(to_now[0], to_now[2]);
        const float true_pitch = frame_pitch(eye, now_point, t.airborne, 0.0f, s);
        // How far off the skater is, against how much of the frame there is to the edge.
        const float view = half_view(s) * (fish ? 1.0f : 0.95f);
        const float off = aim_.have ? std::max(std::abs(wrap_angle(true_yaw - aim_.yaw)), std::abs(true_pitch - aim_.pitch)) : 0.0f;
        const float slack = view * mix(0.3f, 0.55f, human);
        const float catch_up = off > slack ? std::clamp((off - slack) / slack * 3.0f, 0.0f, 5.0f) : 0.0f;
        const float aim_omega = mix(9.0f, 4.2f, reaction) * (1.0f + catch_up);
        const float aim_damping = mix(0.95f, 0.62f, human);
        steer_aim(dt, want_yaw, want_pitch, aim_omega, aim_damping);
        // Never let them out of the frame: past 85% of the way to the edge sideways, or the board about to
        // leave the bottom or the top, the aim is dragged back in.
        if (aim_.have) {
            const float limit = view * 0.85f;
            const float dy = wrap_angle(true_yaw - aim_.yaw);
            if (std::abs(dy) > limit) aim_.yaw = wrap_angle(true_yaw - std::copysign(limit, dy));
            const float horizontal = std::max(flat_length(to_now), 0.3f);
            const float feet = std::atan2(now_point[1] + 0.05f - eye[1], horizontal);
            aim_.pitch = std::clamp(aim_.pitch, feet - 0.85f * half_view(s), feet + 0.85f * half_view(s));
        }
    }
    // Leaning into the turns the filmer rides.
    const float operator_yaw = operator_speed > 0.5f ? yaw_of(operator_velocity_[0], operator_velocity_[2]) : last_operator_yaw_;
    const float turn = dt > 0 ? wrap_angle(operator_yaw - last_operator_yaw_) / std::max(dt, 0.001f) : 0.0f;
    last_operator_yaw_ = operator_yaw;
    lean_ = mix(lean_, std::clamp(-turn * operator_speed * 0.012f, -0.12f, 0.12f), ease(dt, 0.25f));
    float yaw = aim_.yaw, pitch = aim_.pitch, roll = lean_;
    shake(now, operator_speed, human, yaw, pitch, roll, eye);
    eye[1] = std::max(eye[1], lens_ground_ + lens_clearance); // the hand's shake never takes it under
    Frame frame;
    frame.camera = build(eye, yaw, std::clamp(pitch, -1.45f, 1.45f), roll);
    frame.fov = render_fov(s.lens);
    frame.valid = valid_camera(frame.camera);
    return frame;
}

Frame Rig::film(double now, float dt, const Subject &self, const Subject *target, const StickInput &stick,
                const RigSettings &s) {
    if (!self.valid || !finite(self.position)) return {};
    dt = std::clamp(dt, 0.0f, 0.1f);
    begin_tick(now);
    track(now, dt, self, self_, false, true);
    const bool aiming_at = target && target->valid && finite(target->position);
    if (aiming_at) track(now, dt, *target, subject_, true, false);
    const float human = std::clamp(s.human, 0.0f, 1.0f) * 0.6f; // your own hands add the rest
    const auto &me = self_;
    const float speed = flat_length(me.velocity);
    const float heading = yaw_of(me.heading_x, me.heading_z);
    // The stick, with a curve: fine framing near the centre, a whip at the edge.
    const auto curve = [](float v) { return std::copysign(std::pow(std::abs(std::clamp(v, -1.0f, 1.0f)), 1.6f), v); };
    const float rate = 160.0f * degrees * std::clamp(s.sensitivity, 0.2f, 3.0f);
    const float sx = curve(stick.x), sy = curve(stick.y) * (s.invert_y ? -1.0f : 1.0f);
    const bool touched = std::abs(stick.x) > 0.01f || std::abs(stick.y) > 0.01f;
    if (touched) stick_idle_since_ = now;
    offset_yaw_ = wrap_angle(offset_yaw_ - sx * rate * dt);
    offset_pitch_ = std::clamp(offset_pitch_ + sy * rate * 0.7f * dt, -1.2f, 1.2f);
    float want_yaw, want_pitch;
    float omega = 16.0f, damping = 1.0f;
    // The camera is held out towards where it points, so turning it round never looks through you.
    // Held at chest height: the eye rises with you through an ollie, smoothed so a landing is a bump.
    if (!operator_have_) {
        operator_height_ = world(me.position[1]);
        operator_height_rate_ = 0;
        operator_have_ = true;
    }
    spring(operator_height_, operator_height_rate_, world(me.position[1]), 0.0f, 9.0f, 0.75f, dt);
    // Held low and out at arm's length, towards where it points: never inside your own head, arms or
    // board, whichever way you turn it. A wall in the way brings it in; it never goes under the ground.
    const float hold = std::clamp(s.hold_height, 0.05f, 1.8f);
    const Vec3 body{me.position[0], operator_height_ + hold, me.position[2]};
    const float reach_yaw = aim_.have ? aim_.yaw : heading;
    const float reach = 0.62f;
    Vec3 eye = add(body, Vec3{std::sin(reach_yaw) * reach, 0, std::cos(reach_yaw) * reach});
    if (cast_) {
        const Vec3 chest{me.position[0], operator_height_ + std::max(hold, 0.9f), me.position[2]};
        Vec3 hit{};
        if (blocked(chest, eye, &hit)) {
            const Vec3 to = sub(eye, chest);
            const float full = length(to);
            if (full > 0.01f) eye = add(chest, scale(to, std::max(0.0f, length(sub(hit, chest)) - 0.12f) / full));
        }
        if (const auto g = ground_at(eye[0], eye[2], world(me.position[1]) + 0.3f, true)) {
            // Over the ground under the lens: kerb or bank, it never goes into it.
            if (*g > film_floor_) film_floor_ = *g;
            else film_floor_ += (*g - film_floor_) * ease(dt, 0.12f);
            if (!film_floor_known_) { film_floor_ = *g; film_floor_known_ = true; }
            eye[1] = std::max(eye[1], film_floor_ + lens_clearance);
        }
    }
    if (s.assist && aiming_at) {
        // Framing a skater: the aim keeps on them as a person would (late, a little loose), and the
        // stick shifts the framing; let go and it settles back on them.
        const Vec3 now_point = aim_point(subject_, s);
        history_.push_back({now, now_point});
        while (!history_.empty() && now - history_.front().time > 0.6) history_.pop_front();
        const Vec3 late = delayed_point(now, 0.03f + 0.12f * std::clamp(s.reaction, 0.0f, 1.0f));
        const Vec3 to = sub(late, eye);
        want_yaw = yaw_of(to[0], to[2]) + offset_yaw_;
        want_pitch = frame_pitch(eye, late, subject_.airborne, 0.0f, s) + offset_pitch_;
        if (!touched && now - stick_idle_since_ > 0.35) {
            const float k = ease(dt, 0.9f);
            offset_yaw_ = mix(offset_yaw_, 0.0f, k);
            offset_pitch_ = mix(offset_pitch_, 0.0f, k);
        }
        omega = mix(11.0f, 5.5f, std::clamp(s.reaction, 0.0f, 1.0f));
        damping = mix(0.95f, 0.7f, human);
    } else {
        // Your own framing, held relative to the way you ride: turn and the camera turns with you.
        if (s.recenter && !touched && now - stick_idle_since_ > 0.6) {
            const float k = ease(dt, 0.8f);
            offset_yaw_ = mix(offset_yaw_, 0.0f, k);
            offset_pitch_ = mix(offset_pitch_, -0.12f, k);
        }
        want_yaw = heading + offset_yaw_;
        want_pitch = offset_pitch_;
    }
    steer_aim(dt, want_yaw, want_pitch, omega, damping);
    const float turn = dt > 0 ? wrap_angle(heading - last_operator_yaw_) / std::max(dt, 0.001f) : 0.0f;
    last_operator_yaw_ = heading;
    lean_ = mix(lean_, std::clamp(-turn * speed * 0.012f, -0.12f, 0.12f), ease(dt, 0.25f));
    float yaw = aim_.yaw, pitch = aim_.pitch, roll = lean_;
    // Pushing: you can tell from your own speed picking up on the ground.
    pushing_ = !me.airborne && speed > 0.5f && me.velocity[1] > -0.2f;
    shake(now, speed, human, yaw, pitch, roll, eye);
    if (film_floor_known_) eye[1] = std::max(eye[1], film_floor_ + lens_clearance);
    Frame frame;
    frame.camera = build(eye, yaw, std::clamp(pitch, -1.45f, 1.45f), roll);
    frame.fov = render_fov(s.lens);
    frame.valid = valid_camera(frame.camera);
    return frame;
}
} // namespace dingosdk::filmer
