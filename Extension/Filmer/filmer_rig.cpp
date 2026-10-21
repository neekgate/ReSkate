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
void angle_spring(float &x, float &v, float target, float omega, float damping, float dt, float target_rate = 0.0f) {
    const float goal = x + wrap_angle(target - x);
    spring(x, v, goal, target_rate, omega, damping, dt);
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
    plan_angle_ = angle_;
    plan_risk_ = 0.0f;
    plan_scale_ = 1.0f;
    clear_since_ = -1.0;
    plan_next_ = plan_keep_ = 0.0;
    fresh_ = true;
    halt_ = Halt::none;
    halt_speed_ = 0.0f;
    halt_stopped_ = halt_zoom_rolled_ = drifting_ = false;
    bail_over_since_ = -1.0;
    auto_zoom_ = 1.0f;
    speed_slow_ = 0.0f;
    pushed_ = was_on_foot_ = on_foot_ = false;
    caution_ = false;
    caution_next_ = 0.0;
    grinding_ = false;
    grind_weight_ = board_weight_ = 0.0f;
    grind_over_since_ = -1.0;
    post_ = Post::none;
    raised_was_ = ledge_side_ = false;
    wall_raise_ = wall_raise_target_ = wall_raise_rate_ = 0.0f;
    wall_next_ = 0.0;
    hidden_since_ = seen_low_since_ = -1.0;
    peeking_ = false;
    skater_accel_ = last_speed_ = 0.0f;
    lift_ = lift_rate_ = air_lift_ = air_lift_rate_ = 0.0f;
    focus_have_ = false;
    floor_rate_ = 0.0f;
    rest_have_ = false;
    still_since_ = rest_bad_since_ = -1.0;
    rest_next_check_ = 0.0;
    rolling_stop_ = stop_due_ = framed_ = pan_active_ = false;
    stop_lift_ = stop_lift_rate_ = 0.0f;
    tracking_ = false; // (the tripod stays where it was set down)
    lost_since_ = -1.0;
}

void Rig::set_planted(bool planted) noexcept {
    if (planted_ && !planted && operator_have_) {
        // Riding again: from far off the filmer cuts to a new spot behind you instead of a long chase.
        const float away = flat_length(sub(subject_.position, operator_));
        if (away > 12.0f) operator_have_ = false;
    }
    planted_ = planted;
    edge_ = false;
    // Stair stop: still rolling, they brake to a halt (short of a drop ahead) instead of stopping dead.
    rolling_stop_ = planted && stair_style_ && operator_have_ && flat_length(operator_velocity_) > 0.3f;
    stop_due_ = rolling_stop_;
    if (!planted) framed_ = pan_active_ = composing_ = false;
}

void Rig::begin_tick(double now) noexcept {
    now_ = now;
    probe_budget_ = 12;
    ray_budget_ = 12;
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

bool Rig::blocked(const Vec3 &from, const Vec3 &to, Vec3 *hit, bool always) const {
    if (!cast_ || (ray_budget_ == 0 && !always)) return false;
    if (ray_budget_ > 0) --ray_budget_;
    Vec3 found{};
    if (!cast_(from, to, found)) return false;
    if (hit) *hit = found;
    return true;
}

// In the way at the knee (or wherever `above` metres over the ground at each end): a hit that is only
// the ground itself (the ray cut across the brow of a hill, the lip of a bank) does not count; a face
// standing up from it (a wall, a ledge, a trunk) does.
bool Rig::obstructed(const Vec3 &from, const Vec3 &to, float above, bool always) const {
    Vec3 hit{};
    if (!blocked(from, to, &hit, always)) return false;
    const float base = std::max(from[1], to[1]) - above;
    const auto g = ground_at(hit[0], hit[2], hit[1] + 0.3f, always);
    return !(g && std::abs(*g - hit[1]) < 0.12f && *g <= base + 0.2f);
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
    // Up onto what you ollied onto (a ledge, a manual pad): an ollie up, never higher than that.
    if (rise > 0.3f && !(join_ok_ && to_ground <= join_to_ + 0.15f && rise <= join_height + 0.15f)) return false;
    if (!stop_at_drops) return true;
    return rise >= -0.85f; // a curb, a planter's edge: rolled off; a set or a big drop: not
}

// The vertical half of the view (radians): the fisheye's frame edge sees half the game camera's FOV.
float Rig::half_view(const RigSettings &s) const { return render_fov(s.lens) / zoom_ * 0.5f * degrees; }

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

// The zoom eases to where it is wanted, as a servo zoom does; riding again (or the fisheye on), back
// out to the long lens's widest.
void Rig::ease_zoom(float dt, const RigSettings &s) noexcept {
    if (s.lens != Lens::long_lens || !stationary()) zoom_target_ = 1.0f;
    // A zoom of the filmer's own (a slam, a stop) goes in slowly and comes back out unhurried.
    const float want = std::max(zoom_target_, auto_zoom_);
    const float tau = auto_zoom_ > 1.001f ? 1.4f : zoom_ > want + 0.01f ? 0.8f : 0.15f;
    zoom_ += (want - zoom_) * ease(dt, tau);
    if (std::abs(zoom_ - want) < 0.002f) zoom_ = want;
}

bool Rig::stationary() const noexcept {
    if (tripod_mode_) return tripod_have_;
    if (filming_) return self_.have && flat_length(self_.velocity) < 0.5f;
    return operator_have_ && ((planted_ && !rolling_stop_) || (halt_ != Halt::none && halt_stopped_));
}

void Rig::look(float right, float down) noexcept {
    if (!stationary() || !std::isfinite(right) || !std::isfinite(down)) return;
    if (tripod_mode_) {
        // Turning the tripod head: its resting framing too, while it is not panning with anyone.
        if (!tracking_) {
            rest_yaw_ = wrap_angle(rest_yaw_ - right);
            rest_pitch_ = std::clamp(rest_pitch_ - down, -1.35f, 1.35f);
        }
        aim_.yaw = wrap_angle(aim_.yaw - right);
        aim_.pitch = std::clamp(aim_.pitch - down, -1.35f, 1.35f);
        aim_.yaw_rate = aim_.pitch_rate = 0.0f;
    } else if (filming_) {
        // As the right stick does: the framing's offset (the assist keeps framing them past it).
        offset_yaw_ = wrap_angle(offset_yaw_ - right);
        offset_pitch_ = std::clamp(offset_pitch_ - down, -1.2f, 1.2f);
        stick_idle_since_ = now_;
    } else if (aim_.have) {
        aim_.yaw = wrap_angle(aim_.yaw - right);
        aim_.pitch = std::clamp(aim_.pitch - down, -1.35f, 1.35f);
        aim_.yaw_rate = aim_.pitch_rate = 0.0f;
        pan_active_ = composing_ = false; // your framing now, not the stair stop's
    }
}

// Stair stop: where to brake to. At a firm stop from their speed (5 m/s^2, a few metres at most), or
// short of a drop the ground ahead shows (the first step, a ledge's end), whichever comes first.
void Rig::compute_rolling_stop(float floor) noexcept {
    const float speed = flat_length(operator_velocity_);
    if (speed < 0.3f) { rolling_stop_ = false; return; }
    stop_dir_ = Vec3{operator_velocity_[0] / speed, 0.0f, operator_velocity_[2] / speed};
    float reach = std::clamp(speed * speed / (2.0f * 5.0f), 0.3f, 4.0f);
    if (cast_) {
        probe_budget_ += 16;
        for (float d = 0.4f; d <= reach + 1.2f; d += 0.4f) {
            const Vec3 at = add(operator_, scale(stop_dir_, d));
            const auto g = ground_at(at[0], at[2], floor + 0.5f);
            if (!g || *g < floor - 0.35f) {
                reach = std::min(reach, std::max(0.15f, d - 0.6f));
                break;
            }
        }
    }
    stop_point_ = add(operator_, scale(stop_dir_, reach));
    roll_speed_ = speed;
}

// Stair stop: the pitch that takes in the set (or the rail, the ledge) ahead: down to a point past the
// first drop the ground shows that way, a little above its bottom; with no drop to see, a little down.
// Always at least a touch down from where it was, never steeper than about 57 degrees.
float Rig::stair_pan_pitch(const Vec3 &eye, float yaw, float pitch, float floor) const {
    const Vec3 way{std::sin(yaw), 0.0f, std::cos(yaw)};
    float target = pitch - 0.14f;
    if (cast_) {
        probe_budget_ += 20;
        for (float d = 0.6f; d <= 8.0f; d += 0.4f) {
            const Vec3 at = add(eye, scale(way, d));
            const auto g = ground_at(at[0], at[2], floor + 0.5f);
            if (!g || *g < floor - 0.35f) {
                const float low = g ? *g : floor - 1.5f;
                const float out = d + 1.5f;
                target = std::atan2(low + 0.3f - eye[1], out);
                break;
            }
        }
    }
    return std::min(std::max(target, -1.0f), pitch - 0.06f);
}

void Rig::zoom_by(float factor, const RigSettings &settings) noexcept {
    if (settings.lens != Lens::long_lens || !stationary() || !(factor > 0.0f) || !std::isfinite(factor)) return;
    zoom_target_ = std::clamp(zoom_target_ * factor, 1.0f, max_zoom);
}

Rig::Cruise Rig::cruise(float gap) const noexcept {
    if (!filming_ || !framing_ || !self_.have || !subject_.have) return {};
    // How far they are ahead of you along the way they ride (behind you: negative).
    const Vec3 heading{subject_.heading_x, 0.0f, subject_.heading_z};
    const Vec3 rel{subject_.position[0] - self_.position[0], 0.0f, subject_.position[2] - self_.position[2]};
    const float ahead = rel[0] * heading[0] + rel[2] * heading[2];
    const float their_speed = flat_length(subject_.velocity), my_speed = flat_length(self_.velocity);
    if (their_speed < 1.0f) return {}; // they have stopped: nothing to keep up with
    // Their speed, and a pull on the gap: closing at up to 4 m/s, falling back at up to 3.
    const float want = std::clamp(their_speed + std::clamp((ahead - gap) * 0.8f, -3.0f, 4.0f), 0.0f, 14.0f);
    return {true, want, want < my_speed - 0.5f && ahead < gap, ahead};
}

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
    // (More than 4 m in one tick is never riding: back to a marker, a teleport to another player.)
    if (!t.have || length(sub(p, t.position)) > std::max(4.0f, 30.0f * dt) || now - t.last_seen > 1.0) {
        if (own_trail) fresh_ = true;
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
    // (Turned round the way the angle goes, so a reversal (fakie, turning round on the spot) swings
    // the heading round instead of leaving it stuck pointing the old way.)
    {
        float want_x = 0.0f, want_z = 0.0f, tau = 0.0f;
        if (speed > 0.8f) {
            want_x = t.velocity[0] / speed;
            want_z = t.velocity[2] / speed;
            tau = 0.3f;
        } else if (const float fl = std::sqrt(subject.facing[0] * subject.facing[0] + subject.facing[2] * subject.facing[2]); fl > 0.01f) {
            want_x = subject.facing[0] / fl;
            want_z = subject.facing[2] / fl;
            tau = 1.5f;
        }
        if (tau > 0.0f) {
            const float from = std::atan2(t.heading_x, t.heading_z);
            const float to = std::atan2(want_x, want_z);
            const float turned = from + wrap_angle(to - from) * ease(dt, tau);
            t.heading_x = std::sin(turned);
            t.heading_z = std::cos(turned);
        }
    }
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

// Something you can ride on, under trail point `k` (a ledge, a manual pad, a planter's top), not a
// rail: the same top a little either side of your line.
bool Rig::wide_at(std::size_t k) const {
    if (!cast_ || k >= trail_.size()) return false;
    const Vec3 &here = trail_[k].at;
    const Vec3 &prev = trail_[k > 0 ? k - 1 : std::min(k + 1, trail_.size() - 1)].at;
    Vec3 along = sub(here, prev);
    const float l = flat_length(along);
    if (l < 0.01f) return false;
    const Vec3 across{-along[2] / l * 0.35f, 0.0f, along[0] / l * 0.35f};
    const float top = world(here[1]);
    for (const float side_sign : {1.0f, -1.0f}) {
        // (Asked whatever the budget: an answer a couple of seconds old is used again, so this is cheap.)
        ++probe_budget_;
        const auto g = ground_at(here[0] + across[0] * side_sign, here[2] + across[2] * side_sign, top + 0.3f);
        if (!g || std::abs(*g - top) > 0.15f) return false;
    }
    return true;
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
        const bool jumped = air_start != none;
        const std::size_t before = jumped ? (air_start > 0 ? air_start - 1 : 0) : k - 1;
        // Off it with an ollie (or a flip, or just off the end of it, in the air) and down further than
        // a filmer would huck after you: they stop at the top. Rolled down it (a ramp, a bank, however
        // fast and far): they roll down after you.
        if (jumped && stop_at_drops && result.level - ground > huck_height) { result.edge = true; result.reach = before; return result; }
        if (ground < result.level - 0.35f) {
            result.level = ground; // down after you: a curb, a ramp, a small set hucked
        } else if (jumped && std::abs(ground - result.level) <= 0.35f && cast_) {
            // Over a jump at the same level: is there ground under it (an ollie), or nothing (a gap)?
            const Vec3 &a = trail_[before].at;
            const float span = flat_length(sub(point.at, a));
            if (span > 1.2f) {
                for (const float u : {0.33f, 0.5f, 0.67f}) {
                    const auto g = ground_at(a[0] + (point.at[0] - a[0]) * u, a[2] + (point.at[2] - a[2]) * u, result.level);
                    if (!g || *g < world(a[1]) - huck_height) { // well below where you took off: a gap
                        if (stop_at_drops) { result.edge = true; result.reach = before; return result; }
                        break;
                    }
                }
            }
            result.level = ground;
        } else if (std::abs(ground - result.level) <= 0.35f) {
            result.level = ground; // rolling along, up and down banks and slopes
        } else if (jumped && ground - result.level <= join_height && wide_at(k)) {
            result.level = ground; // ollied up onto a ledge or a manual pad: they ollie up after you
        }
        // (Higher up otherwise, after a jump: a rail, or too high. The filmer stays at their level.)
        air_start = none;
        result.reach = k;
    }
    // Still in the air at the end: if there is ground under the jump so far, not far below where you
    // took off (an ollie on flat, on a slope, up onto something), they ride on; over a gap or off a
    // drop too high to huck (or with no way to tell) they ease off to wait at the takeoff until you land.
    if (air_start != none) {
        bool level_ground = static_cast<bool>(cast_);
        const float takeoff = world(trail_[air_start > 0 ? air_start - 1 : 0].at[1]);
        for (std::size_t k = air_start; level_ground && k <= to && k < trail_.size(); k += 3) {
            const auto g = ground_at(trail_[k].at[0], trail_[k].at[2], trail_[k].at[1]);
            if (!g || *g < takeoff - huck_height) level_ground = false;
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
void Rig::choose_angle(double now, float speed, std::uint8_t angles) {
    if (now < angle_until_) return;
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    const float draw = unit(random_);
    const float side = angle_target_ >= 0 ? 1.0f : -1.0f;
    float keep_side = unit(random_) < 0.65f ? side : -side;
    if (side_lock_ != 0.0f) keep_side = side_lock_;
    // Among the angles chosen (all, if none): behind most, then out to the side, now and then ahead
    // (only riding fast enough to stay in front; slower, ahead falls back to behind if it can).
    if ((angles & 7u) == 0) angles = 7;
    float weight_behind = (angles & angle_behind) ? 0.5f : 0.0f, weight_side = (angles & angle_side) ? 0.35f : 0.0f,
          weight_ahead = (angles & angle_ahead) && (speed > 3.5f || angles == angle_ahead) ? 0.15f : 0.0f;
    if (weight_behind + weight_side + weight_ahead <= 0.0f) weight_behind = 1.0f;
    const float roll = draw * (weight_behind + weight_side + weight_ahead);
    float pick;
    double hold;
    if (roll < weight_behind) { pick = (18.0f + 22.0f * unit(random_)) * keep_side; hold = 5.0 + 6.0 * unit(random_); }
    else if (roll < weight_behind + weight_side) { pick = (55.0f + 25.0f * unit(random_)) * keep_side; hold = 3.5 + 4.0 * unit(random_); }
    else { pick = (160.0f + 15.0f * unit(random_)) * keep_side; hold = 3.0 + 2.5 * unit(random_); }
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

void Rig::steer_aim(float dt, float want_yaw, float want_pitch, float omega, float damping, float yaw_rate, float pitch_rate) {
    if (!aim_.have) {
        aim_ = {want_yaw, want_pitch, 0, 0, true};
        return;
    }
    angle_spring(aim_.yaw, aim_.yaw_rate, want_yaw, omega, damping, dt, yaw_rate);
    spring(aim_.pitch, aim_.pitch_rate, std::clamp(want_pitch, -1.35f, 1.35f), pitch_rate, omega, damping, dt);
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

// The way planner. A filmer riding behind someone sees what is coming: the bench on their side, the
// tree on the corner, the wall the line runs along, the drop off the side of the path. A few times a
// second the spots the filmer could take round the skater (the side it keeps to, a quarter round,
// straight behind, the other side, and each closer in or further out) are scored where the skater is
// now and where they will be over the next second and a bit, and on the way the filmer would ride
// between them, a body's width wide:
//   no ground level with the skater's there (a drop beside the line, a gap, something raised),
//   a wall between the spot and the skater (no footy, and the lens behind it),
//   anything in the way of riding there (a trunk, a post, a corner).
// The side it keeps to costs least only while it is clear: with something coming up there, the
// clearest side and distance win, and that choice is held a few seconds once past it (a filmer
// carving back over, not snapping back). With nowhere clear at all (wedged between walls) they wait
// where they are for you to come past (`trapped`). Never in front of you.
float Rig::plan(double now, float want, float d, float height, float speed, bool &trapped) {
    trapped = false;
    if (!cast_ || !world_answers() || !operator_have_) {
        plan_angle_ = want;
        plan_scale_ = 1.0f;
        plan_risk_ = 0.0f;
        return want;
    }
    if (now < plan_next_) {
        trapped = halt_ == Halt::wait && clear_since_ < 0.0;
        return plan_angle_;
    }
    plan_next_ = now + 0.15;
    const auto &t = subject_;
    const Vec3 heading{t.heading_x, 0.0f, t.heading_z};
    const Vec3 side{t.heading_z, 0.0f, -t.heading_x};
    const float sign = want > 0.0f ? 1.0f : want < 0.0f ? -1.0f : (plan_angle_ >= 0.0f ? 1.0f : -1.0f);
    const float quarter = std::min(std::abs(want), 22.0f * degrees) * sign;
    const float flat_speed = std::min(speed, 12.0f);
    const Vec3 way = flat_speed > 0.5f ? Vec3{t.velocity[0] / std::max(speed, 0.01f), 0.0f, t.velocity[2] / std::max(speed, 0.01f)} : heading;
    // Looked at further ahead than they will be in a second and a half: a wall or a bin coming up on
    // their side is seen in time to move over (gradually) before they get to it.
    const std::array<float, 4> ahead{0.0f, 0.5f, 1.0f, 1.6f};
    const std::size_t samples = flat_speed > 0.5f ? 4 : 1;
    const float level = operator_floor_;
    // Where the skater is up on something (a ledge, a rail), the spots are judged against the filmer's
    // own level; otherwise against the ground under the skater there (riding down a bank or a ramp,
    // the spots beside them are lower too, and that is no drop).
    const bool up = t.ground > level_ + 0.3f && !joining_;
    std::array<float, 4> reference{};
    probe_budget_ += 64;
    ray_budget_ += 120;
    for (std::size_t i = 0; i < samples; ++i) {
        const float along = flat_speed * ahead[i];
        const float x = t.position[0] + way[0] * along, z = t.position[2] + way[2] * along;
        reference[i] = level;
        if (!up)
            if (const auto g = ground_at(x, z, world(t.ground) + 0.5f)) reference[i] = *g;
    }
    const auto score = [&](float a, float scale_by) {
        float cost = std::abs(a - want) / degrees * 0.012f + std::abs(scale_by - 1.0f) * 0.8f;
        // The other side from the one they are on: a ride across behind you, so only for a reason.
        if (std::abs(plan_angle_) > 5.0f * degrees && a * plan_angle_ < 0.0f) cost += 0.3f;
        // Told which side by you (the side buttons): never the other.
        if (side_hard_ != 0.0f && a * side_hard_ < -1.0f * degrees) return 1e6f;
        const float dist = d * scale_by;
        Vec3 last{operator_[0], level, operator_[2]};
        for (std::size_t i = 0; i < samples; ++i) {
            const float weight = i == 0 ? 1.0f : i == 1 ? 0.85f : i == 2 ? 0.7f : 0.55f;
            const float along = flat_speed * ahead[i];
            const Vec3 at{t.position[0] + way[0] * along, t.position[1], t.position[2] + way[2] * along};
            const Vec3 line = add(at, scale(heading, -dist * std::cos(a)));
            const Vec3 spot = add(line, scale(side, dist * std::sin(a)));
            // Judged against the ground on your line level with the spot (on a hill the spots behind
            // you are higher, and that is no ledge).
            float ref = reference[i];
            if (!up && std::abs(std::sin(a)) > 0.05f)
                if (const auto gl = ground_at(line[0], line[2], reference[i] + 1.5f)) ref = *gl;
            if (!up && std::abs(std::sin(a)) <= 0.05f) ref = ground_at(line[0], line[2], reference[i] + 1.5f).value_or(reference[i]);
            const auto g = ground_at(spot[0], spot[2], ref + 0.5f);
            float ground = ref;
            if (!g) cost += 2.5f * weight;                    // nothing under it: off an edge, a gap
            else if (*g < ref - 0.6f) cost += 3.0f * weight;  // a drop beside the line
            else if (*g > ref + 0.3f) cost += 2.0f * weight;  // up on something (a planter, a ledge)
            else ground = *g;
            const Vec3 eye{spot[0], ground + height + 0.1f, spot[2]};
            const Vec3 centre{at[0], (up ? world(at[1]) : reference[i]) + 0.9f, at[2]};
            if (obstructed(centre, eye, 0.25f)) cost += 2.5f * weight; // a wall between: no footy
            // Elbow room: a wall, a bin or a post right beside the spot (within 0.7 m either side) is
            // a spot to keep off, before they are brushing past it.
            if (i <= 2) {
                for (const float side_sign : {1.0f, -1.0f}) {
                    const Vec3 from{spot[0], ground + 0.3f, spot[2]};
                    const Vec3 to{spot[0] + side[0] * 0.7f * side_sign, ground + 0.3f, spot[2] + side[2] * 0.7f * side_sign};
                    if (obstructed(from, to, 0.3f)) cost += 0.45f * weight;
                }
            }
            // The way there, a body wide (a trunk, a post, a corner): either shoulder, low (a planter, a kerb-high box).
            const Vec3 move{spot[0] - last[0], 0.0f, spot[2] - last[2]};
            const float length_moved = flat_length(move);
            if (length_moved > 0.05f) {
                const Vec3 across{-move[2] / length_moved * 0.3f, 0.0f, move[0] / length_moved * 0.3f};
                for (const float shoulder : {1.0f, -1.0f}) {
                    const Vec3 from{last[0] + across[0] * shoulder, last[1] + 0.25f, last[2] + across[2] * shoulder};
                    const Vec3 to{spot[0] + across[0] * shoulder, ground + 0.25f, spot[2] + across[2] * shoulder};
                    if (obstructed(from, to, 0.25f)) cost += 0.9f * weight;
                }
            }
            last = Vec3{spot[0], ground, spot[2]};
        }
        return cost;
    };
    const float candidates[] = {want, quarter, 0.0f, -quarter, -want, plan_angle_};
    struct Option { float angle{}, scale_by{1.0f}, cost{1e9f}; };
    Option best, second;
    float want_cost = 0.0f, current_cost = 1e9f;
    for (std::size_t c = 0; c < std::size(candidates); ++c) {
        const float a = candidates[c];
        bool repeat = false;
        for (std::size_t k = 0; k < c; ++k) if (std::abs(candidates[k] - a) < 2.0f * degrees) repeat = true;
        if (repeat) continue;
        const float cost = score(a, 1.0f);
        if (c == 0) want_cost = cost;
        if (std::abs(a - plan_angle_) < 2.0f * degrees && std::abs(plan_scale_ - 1.0f) < 0.05f) current_cost = cost;
        if (cost < best.cost) { second = best; best = {a, 1.0f, cost}; }
        else if (cost < second.cost) second = {a, 1.0f, cost};
    }
    // Nothing quite clear at the usual distance: closer in, or further out, at the best two angles.
    if (best.cost > 0.6f) {
        for (const auto &option : {best, second}) {
            if (option.cost >= 1e8f) continue;
            for (const float scale_by : {0.7f, 1.4f}) {
                const float cost = score(option.angle, scale_by);
                if (std::abs(option.angle - plan_angle_) < 2.0f * degrees && std::abs(plan_scale_ - scale_by) < 0.05f) current_cost = cost;
                if (cost < best.cost) best = {option.angle, scale_by, cost};
            }
        }
    }
    // Kept while it is clear (no dithering); a choice made to get round something is held a few
    // seconds, so they carve back over after it, not snap back.
    const bool keep = now < plan_keep_ && current_cost < 0.6f;
    if (!keep) {
        const bool evasive = want_cost - best.cost > 0.5f;
        if (std::abs(best.angle - plan_angle_) > 1.0f * degrees || std::abs(best.scale_by - plan_scale_) > 0.05f)
            plan_keep_ = now + (evasive ? 2.5 : 0.8);
        plan_angle_ = best.angle;
        plan_scale_ = best.scale_by;
    }
    plan_risk_ = std::max(0.0f, want_cost - std::min(best.cost, current_cost));
    // Wedged: every spot is bad. Room again once something is clear for a moment.
    if (best.cost > 3.0f) clear_since_ = -1.0;
    else if (clear_since_ < 0.0 && best.cost < 1.5f) clear_since_ = now;
    trapped = best.cost > 3.0f || (halt_ == Halt::wait && (clear_since_ < 0.0 || now - clear_since_ < 0.4));
    return plan_angle_;
}

// You standing still: a spot near you, behind (left or right of your back, straight behind when
// those are no good), with level ground, a clear view of you and a clear way there. The nearest of
// those to where the filmer is: they roll over and stop, they do not circle round you.
bool Rig::rest_spot_ok(const Vec3 &spot, float, float height) const {
    const auto &t = subject_;
    const float level = operator_have_ ? operator_floor_ : world(t.ground);
    const auto g = ground_at(spot[0], spot[2], level + 0.5f);
    if (!g || *g < level - 0.85f || *g > level + 0.3f) return false;
    const Vec3 centre{t.position[0], world(t.position[1]) + 0.9f, t.position[2]};
    return !blocked(centre, Vec3{spot[0], *g + height + 0.1f, spot[2]});
}

bool Rig::choose_rest(float d, float height) {
    const auto &t = subject_;
    const Vec3 heading{t.heading_x, 0.0f, t.heading_z};
    const Vec3 side{t.heading_z, 0.0f, -t.heading_x};
    const float current = operator_have_ ? ((operator_[0] - t.position[0]) * side[0] + (operator_[2] - t.position[2]) * side[2] >= 0.0f ? 1.0f : -1.0f)
                                         : (side_lock_ != 0.0f ? side_lock_ : 1.0f);
    const float first = side_lock_ != 0.0f ? side_lock_ : current;
    const float near = std::clamp(d, 1.2f, 8.0f);
    struct Option { float angle, distance; };
    const Option options[] = {{30.0f * first, near}, {50.0f * first, near}, {30.0f * -first, near}, {50.0f * -first, near},
                              {0.0f, near}, {30.0f * first, near * 0.7f}, {30.0f * -first, near * 0.7f}, {0.0f, near * 0.7f}};
    probe_budget_ += 12;
    ray_budget_ += 24;
    float best_score = 1e9f;
    bool found = false;
    for (const auto &o : options) {
        const float a = o.angle * degrees;
        const Vec3 spot = add(Vec3{t.position[0], 0.0f, t.position[2]}, add(scale(heading, -o.distance * std::cos(a)), scale(side, o.distance * std::sin(a))));
        if (cast_ && !rest_spot_ok(spot, d, height)) continue;
        // The nearest good one to where the filmer is (and on the side they are on).
        float score = operator_have_ ? flat_length(sub(spot, operator_)) : 0.0f;
        if (o.angle * current < 0.0f) score += 1.5f;
        if (cast_ && operator_have_) {
            const Vec3 from{operator_[0], operator_floor_ + 0.45f, operator_[2]};
            if (blocked(from, Vec3{spot[0], operator_floor_ + 0.45f, spot[2]})) score += 4.0f;
        }
        if (score < best_score) {
            best_score = score;
            rest_spot_ = spot;
            found = true;
        }
    }
    if (!found) {
        // Nothing clear round you (a tight spot): straight behind, close.
        rest_spot_ = add(Vec3{t.position[0], 0.0f, t.position[2]}, scale(heading, -near * 0.6f));
    }
    rest_from_ = t.position;
    rest_have_ = true;
    rest_bad_since_ = -1.0;
    return found;
}

// Stopping by themselves: rolling on along the way they were riding and braking gently to a halt
// (about 2.5 m/s/s, harder only if there is not the room), short of anything ahead that drops away.
void Rig::begin_halt(Halt why, double now, float room) {
    if (halt_ == why) {
        // Already stopping: a drop seen closer than where they meant to stop moves the stop point in.
        const float remaining = (halt_point_[0] - operator_[0]) * halt_dir_[0] + (halt_point_[2] - operator_[2]) * halt_dir_[2];
        if (room < remaining) halt_point_ = add(operator_, scale(halt_dir_, std::max(room, 0.0f)));
        return;
    }
    const bool rolling = halt_ != Halt::none;
    halt_ = why;
    halt_since_ = now;
    halt_zoom_rolled_ = false;
    drifting_ = false;
    drift_done_ = 0.0f;
    bail_over_since_ = -1.0;
    if (rolling) return; // already braking: carry on to the same stop
    const float v = flat_length(operator_velocity_);
    halt_speed_ = v;
    halt_stopped_ = v < 0.05f;
    halt_dir_ = v > 0.05f ? Vec3{operator_velocity_[0] / v, 0.0f, operator_velocity_[2] / v} : Vec3{subject_.heading_x, 0.0f, subject_.heading_z};
    // Never rolling on past you (you slammed, you stopped in front of them): short of you, by a body.
    {
        const float to_you = (subject_.position[0] - operator_[0]) * halt_dir_[0] + (subject_.position[2] - operator_[2]) * halt_dir_[2];
        const float across = std::abs((subject_.position[0] - operator_[0]) * halt_dir_[2] - (subject_.position[2] - operator_[2]) * halt_dir_[0]);
        if (to_you > 0.0f && across < 2.0f) room = std::min(room, std::max(0.0f, to_you - 1.4f));
    }
    // (Short of room, they may use some of the margin rather than stop dead.)
    float reach = std::max(std::min(v * v / (2.0f * 2.5f), std::max(room, 0.0f)), std::min(v * v / (2.0f * 6.0f), std::max(room + 0.4f, 0.0f)));
    if (cast_ && reach > 0.2f) {
        probe_budget_ += 12;
        for (float step = 0.4f; step <= reach + 0.8f; step += 0.4f) {
            const Vec3 at = add(operator_, scale(halt_dir_, step));
            const auto g = ground_at(at[0], at[2], operator_floor_ + 0.5f);
            if (!g || *g < operator_floor_ - 0.5f) {
                reach = std::min(reach, std::max(0.1f, step - 0.6f));
                break;
            }
        }
    }
    halt_point_ = add(operator_, scale(halt_dir_, reach));
}

void Rig::resume_behind() noexcept {
    // A cut: the next tick places them straight in behind you.
    operator_have_ = false;
    halt_ = Halt::none;
    post_ = Post::none;
    planted_ = edge_ = holding_ = false;
    caution_ = false;
    auto_zoom_ = 1.0f;
}

std::string Rig::doing() const {
    if (!operator_have_) return "placing the filmer";
    if (planted_) return "stopped by you (left stick to ride on)";
    switch (halt_) {
    case Halt::drop: return "stopped at the top: too high to huck (push, step off or left stick to carry on)";
    case Halt::lag: return "too far behind: stopped (push, step off or left stick to carry on)";
    case Halt::slam: return "you slammed: stopped, filming you";
    case Halt::wait: return "no room to ride: waiting for you to come past";
    case Halt::none: break;
    }
    if (post_ == Post::rail_end) return "racing to the end of the ledge for a low angle";
    if (post_ == Post::ledge) return "behind the ledge, camera up over it";
    if (joining_) return operator_floor_ < join_to_ - 0.2f ? "ollying up onto the ledge after you" : "up on the ledge with you";
    if (grinding_) return grind_style_ == 1 ? "out wide for the landing" : "out wide, camera up a little";
    if (rest_have_) return "you are still: waiting behind you";
    if (caution_) return "easing off: a drop ahead";
    if (wall_raise_ > 0.2f) return "camera raised to see you over something";
    return "riding with you";
}

Frame Rig::follow(double now, float dt, const Subject &subject, const RigSettings &s) {
    if (!subject.valid || !finite(subject.position)) return {};
    dt = std::clamp(dt, 0.0f, 0.1f);
    begin_tick(now);
    filming_ = framing_ = false;
    tripod_mode_ = false;
    stair_style_ = s.stop_style == StopStyle::stair;
    slam_zoom_ = s.slam_zoom;
    stop_zoom_ = s.stop_zoom;
    ff_have_ = false;
    track(now, dt, subject, subject_, true, true);
    ease_zoom(dt, s);
    const auto &t = subject_;
    const float speed = flat_length(t.velocity);
    const float human = std::clamp(s.human, 0.0f, 1.0f), reaction = std::clamp(s.reaction, 0.0f, 1.0f);
    const bool fish = s.lens == Lens::fisheye;
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    float distance = std::clamp(s.distance, 0.6f, 6.0f) * (fish ? 1.0f : 3.2f);
    // Never right on top of you: the least gap between the filmer and you while you skate.
    const float min_gap = fish ? std::clamp(distance * 0.8f, 1.0f, 1.4f) : 2.5f;
    // You pushing (your speed rising on the flat), or stepping off the board: what sends a filmer who
    // stopped by themselves on again.
    {
        const bool foot = subject.on_foot.value_or(false);
        pushed_ = !t.airborne && !subject.bailing && t.velocity[1] > -0.4f && speed > 1.2f && speed - speed_slow_ > 0.6f;
        if (foot && !was_on_foot_) pushed_ = true;
        was_on_foot_ = foot;
        on_foot_ = foot;
        speed_slow_ += (speed - speed_slow_) * ease(dt, 0.6f);
        if (dt > 0.0f) skater_accel_ += ((speed - last_speed_) / dt - skater_accel_) * ease(dt, 0.15f);
        last_speed_ = speed;
        if (speed < speed_slow_) speed_slow_ = speed; // slowing down never builds up a "push"
    }
    // The lunge: with the fisheye, on a flatground trick the filmer pushes in closer and lower under
    // it, and eases back out as you land and ride away.
    const bool lunging = s.lunge && fish && t.airborne && flat_air_ && !planted_ && !edge_ && !holding_ && halt_ == Halt::none;
    spring(lunge_, lunge_rate_, lunging ? 1.0f : 0.0f, 0.0f, lunging ? 6.0f : 2.5f, 1.0f, dt);
    lunge_ = std::clamp(lunge_, 0.0f, 1.0f);
    // Pull back a little at speed and in the air, so a trick stays in the frame (unless lunging in).
    distance += std::min(speed * 0.04f, 0.5f);
    if (t.airborne) distance += (std::clamp(t.position[1] - t.ground, 0.0f, 3.0f) * 0.35f + 0.3f) * (1.0f - lunge_);
    distance = std::max(distance * mix(1.0f, 0.65f, lunge_), std::min(distance, min_gap));
    // The side they keep to.
    side_lock_ = s.side == Side::left ? 1.0f : s.side == Side::right ? -1.0f : 0.0f;
    side_hard_ = s.side_hard ? side_lock_ : 0.0f;
    // Stopped by themselves, and what ends it.
    if (operator_have_ && halt_ != Halt::none) {
        const float away = flat_length(sub(t.position, operator_));
        const bool long_unseen = unseen_since_ >= 0 && now - unseen_since_ > 8.0;
        switch (halt_) {
        case Halt::drop:
        case Halt::lag:
            // You push or step off the board (or click the left stick): straight back in behind you.
            // Long gone (out of sight, or a long way off): the same, as the next clip.
            // (Still close by: they just ride on after you, a push at a time.)
            if ((pushed_ && now - halt_since_ > 0.5) || away > 60.0f || long_unseen) {
                if (away < std::max(6.0f, distance * 2.5f) && !long_unseen) halt_ = Halt::none;
                else resume_behind();
            } else if (halt_ == Halt::lag && away < std::max(12.0f, distance * 5.0f) * 0.5f) {
                halt_ = Halt::none; // you came back
            }
            break;
        case Halt::slam:
            // Back up: carry on (from far off, they wait for you to push).
            if (subject.bailing) bail_over_since_ = -1.0;
            else if (bail_over_since_ < 0.0) bail_over_since_ = now;
            else if (now - bail_over_since_ > 0.8) {
                halt_ = Halt::none;
                if (away > std::max(12.0f, distance * 5.0f)) begin_halt(Halt::lag, now, 0.0f);
            }
            break;
        case Halt::wait:
        case Halt::none:
            break;
        }
        if (halt_ == Halt::none) auto_zoom_ = 1.0f;
    }
    // Lost sight of you for a while when riding along: cut.
    if (!planted_ && halt_ == Halt::none && !edge_ && !holding_ && post_ == Post::none && unseen_since_ >= 0 && now - unseen_since_ > 2.5)
        operator_have_ = false;
    // Placed afresh (back to a marker, a teleport to someone, a cut): straight in behind you, a little
    // to the side the default cameras sit (the side chosen, or the one they were on), never round in
    // front and never riding round from where they were.
    if (fresh_ || !operator_have_) {
        const float keep_side = side_lock_ != 0.0f ? side_lock_ : (angle_target_ >= 0.0f ? 1.0f : -1.0f);
        angle_ = angle_target_ = plan_angle_ = 16.0f * degrees * keep_side;
        angle_rate_ = 0.0f;
        angle_until_ = now + 3.0;
        plan_next_ = plan_keep_ = 0.0;
        plan_scale_ = 1.0f;
        rest_have_ = false;
        still_since_ = -1.0;
        operator_have_ = false;
        halt_ = Halt::none;
        post_ = Post::none;
        caution_ = false;
        drifting_ = false;
        auto_zoom_ = 1.0f;
        wall_raise_target_ = 0.0f;
        hidden_since_ = -1.0;
        peeking_ = false;
        fresh_ = false;
    }
    const float sign = side_lock_ != 0.0f ? side_lock_ : (angle_target_ >= 0 ? 1.0f : -1.0f);
    // The angle round the skater: behind, or out to the side, never in front (a filmer behind you on
    // their board, not riding backwards ahead of you).
    switch (s.style) {
    case Style::automatic: {
        const std::uint8_t allowed = static_cast<std::uint8_t>(s.angles & (angle_behind | angle_side));
        choose_angle(now, speed, allowed ? allowed : angle_behind);
        if (side_lock_ != 0.0f) angle_target_ = std::abs(angle_target_) * side_lock_;
        break;
    }
    case Style::behind: angle_target_ = 14.0f * degrees * sign; break;
    case Style::side:
    case Style::ahead: angle_target_ = 70.0f * degrees * sign; break;
    }
    angle_target_ = std::clamp(angle_target_, -75.0f * degrees, 75.0f * degrees);
    // Up on something (a ledge, a rail, a planter you manual along): the filmer stays down at their
    // level and, picking one at the start, either rides out a little wider with the camera up a bit
    // (to see the top of what you grind), pushes well out for the landing and the roll away, or, on a
    // slow grind with time to get there, races to the far end for a low angle as you come off it.
    {
        const float up = t.ground - level_;
        // (Held for a moment first: a landing's first tick can still read high.)
        const bool up_there = operator_have_ && !t.airborne && up > 0.3f && up < 3.0f && speed > 0.5f && !joining_;
        if (!up_there) grind_seen_since_ = -1.0;
        else if (grind_seen_since_ < 0.0) grind_seen_since_ = now;
        const bool grinding = up_there && (grinding_ || now - grind_seen_since_ > 0.25);
        if (grinding && !grinding_) {
            grind_since_ = now;
            const float roll = unit(random_);
            grind_style_ = roll < 0.3f ? 1 : 0;
            // The end of it, along the way you grind (where the ground under your line drops away).
            if (cast_ && world_answers() && speed < 3.5f && roll > 0.55f && halt_ == Halt::none) {
                probe_budget_ += 24;
                const Vec3 heading_now{t.heading_x, 0.0f, t.heading_z};
                const float top = world(t.ground);
                float end_at = -1.0f;
                for (float along = 0.5f; along <= 10.0f; along += 0.5f) {
                    const auto g = ground_at(t.position[0] + heading_now[0] * along, t.position[2] + heading_now[2] * along, top + 0.2f);
                    if (!g || *g < top - 0.3f) { end_at = along; break; }
                }
                if (end_at > 2.5f) {
                    const Vec3 side_now{t.heading_z, 0.0f, -t.heading_x};
                    const float out = sign * 1.3f;
                    const Vec3 end_point{t.position[0] + heading_now[0] * end_at, 0.0f, t.position[2] + heading_now[2] * end_at};
                    const Vec3 spot = add(end_point, add(scale(heading_now, 1.4f), scale(side_now, out)));
                    const auto g = ground_at(spot[0], spot[2], operator_floor_ + 0.5f);
                    const float ride = flat_length(sub(spot, operator_));
                    // There before you come off it, at a filmer's pace (about 6 m/s), with a moment to spare.
                    if (g && std::abs(*g - operator_floor_) < 0.3f && ride / 6.0f + 0.6f < end_at / std::max(speed, 0.5f)) {
                        post_ = Post::rail_end;
                        post_spot_ = spot;
                        post_since_ = now;
                        grind_style_ = 2;
                    }
                }
            }
        }
        if (grinding) grind_over_since_ = -1.0;
        else if (grinding_ && grind_over_since_ < 0.0) grind_over_since_ = now;
        grinding_ = grinding;
        const bool grind_on = grinding_ || (grind_over_since_ >= 0.0 && now - grind_over_since_ < 1.2);
        grind_weight_ += ((grind_on ? 1.0f : 0.0f) - grind_weight_) * ease(dt, grind_on ? 0.6f : 1.5f);
        // Through a grind the board in the middle of the frame; after it (and after any trick), slowly
        // back to framing your body.
        board_weight_ += ((grinding_ ? 1.0f : 0.0f) - board_weight_) * ease(dt, grinding_ ? 0.4f : 2.0f);
        // The end of the ledge: there, they wait, low, until you have come off it and rolled away.
        if (post_ == Post::rail_end && !grinding_ && !t.airborne && grind_over_since_ >= 0.0 && now - grind_over_since_ > 1.2) post_ = Post::none;
        if (post_ == Post::rail_end && now - post_since_ > 12.0) post_ = Post::none;
    }
    // A filmer never holds exactly the same spot.
    const float wander_angle = wobble_[0].sample(now * 0.31) * 9.0f * degrees * human;
    const float wander_distance = wobble_[1].sample(now * 0.27) * 0.35f * human +
        // Variety: a slow drift closer and further through a line (a filmer moving in and out).
        distance * 0.45f * std::clamp(s.distance_variety, 0.0f, 1.0f) * wobble_[4].sample(now * 0.06);
    // Through a grind: out wider (well out for the landing shot).
    const float grind_out = grind_weight_ * (grind_style_ == 1 ? 0.7f : 0.25f);
    const float base_d = std::max(min_gap, (distance + wander_distance) * (1.0f + grind_out));
    const Vec3 heading{t.heading_x, 0, t.heading_z};
    const Vec3 side{t.heading_z, 0, -t.heading_x};
    // Variety: lower and higher through a line, slowly (a filmer crouching, then standing a little).
    float height = std::max(0.1f, (std::clamp(s.height, 0.08f, 2.5f) +
        0.35f * std::clamp(s.height_variety, 0.0f, 1.0f) * wobble_[5].sample(now * 0.05)) * mix(1.0f, 0.75f, lunge_));
    if (post_ == Post::rail_end) height = std::max(0.1f, height * 0.6f); // the low angle at the end
    // The way planner: the side (or straight behind), and how close, that is clear where you are
    // heading. Moving round to a new angle takes the time a filmer needs to ride there: a carve, a
    // little quicker when getting out of the way of something.
    bool trapped = false;
    const float planned = plan(now, angle_target_ + (grind_weight_ > 0.05f ? sign * 12.0f * degrees * grind_weight_ : 0.0f), base_d, height, speed, trapped);
    {
        const float most = (plan_risk_ > 0.5f ? 35.0f : 15.0f) * degrees;
        spring(angle_, angle_rate_, planned, 0.0f, plan_risk_ > 0.5f ? 1.6f : 0.8f, 1.0f, dt);
        angle_rate_ = std::clamp(angle_rate_, -most, most);
    }
    plan_scale_ = std::clamp(plan_scale_, 0.7f, 1.4f);
    const float d = std::max(min_gap, base_d * plan_scale_);
    const float angle = std::clamp(angle_ + wander_angle * (plan_risk_ > 0.5f ? 0.3f : 1.0f), -85.0f * degrees, 85.0f * degrees);
    // Where on the trail the filmer is, and the level they ride at.
    std::optional<std::size_t> at_index;
    if (operator_have_) {
        at_index = trail_index_near(operator_[0], operator_[2], 4.0f);
        if (at_index && *at_index > last_index_) {
            // Ridden on along the trail since the last tick: follow its slopes to here.
            level_ = walk(last_index_, *at_index, level_, false).level;
        }
        if (at_index) last_index_ = *at_index;
        // And the ground the filmer is actually on, where the game answers: the level never drifts
        // from it (riding up a long gentle slope, it would, and everything ahead would look raised).
        if (cast_ && world_answers() && root_known_ && !edge_ && !holding_)
            level_ += (operator_floor_ + root_above_ground_ - level_) * ease(dt, 0.3f);
    }
    Vec3 desired;
    float ground = level_;
    bool edge = false;
    holding_ = false;
    Vec3 base{};
    float offset = 0.0f;
    bool raised_ahead = false;
    std::size_t rise_index = 0;
    if (trail_.size() >= 2) {
        // Behind and beside: on the trail the skater left, out to the side; never past a drop too high
        // to huck after an ollie, a gap, or a jump you are still in the air for.
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
        // Something raised on your line ahead of the filmer (the planter, the ledge you ollied onto):
        // seen well before they reach it (four metres and more at speed), so they pick a side, or stop
        // at its foot and raise the camera, before they are nosed up against it.
        const float look_ahead = 4.0f + std::min(flat_length(operator_velocity_), 8.0f) * 0.4f;
        const std::size_t ahead = trail_index_back(std::max(0.25f, arc - look_ahead));
        const std::size_t from = at_index ? std::min(*at_index, index) : index;
        // Ollied up onto something low and wide (a ledge, a manual pad): they ollie up after you and
        // ride it (a little lift of the camera as they go up), then down off the end after you, or stop
        // at the end if it is too high to huck off.
        {
            float lvl = ground;
            bool jumped = false;
            const bool was_joining = joining_;
            joining_ = false;
            for (std::size_t i = from > 12 ? from - 12 : 0; i < trail_.size(); ++i) {
                if (trail_[i].air) { jumped = true; continue; }
                const float y = trail_[i].at[1];
                if (y > lvl + 0.3f) {
                    if (jumped && y - lvl <= join_height && wide_at(i)) {
                        if (!joining_) { join_point_ = trail_[i].at; join_rise_ = y - lvl; }
                        joining_ = true;
                        lvl = y;
                    } else if (i <= std::max(ahead, index)) {
                        raised_ahead = true;
                        rise_index = i;
                        break;
                    }
                } else if (y < lvl - 0.35f) {
                    lvl = y;
                }
                jumped = false;
            }
            // Once on their way up, they carry on up (until they are up there, or a few seconds on).
            if (joining_ && !was_joining) join_since_ = now;
            if (!joining_ && was_joining && operator_floor_ < join_to_ - 0.2f && now - join_since_ < 3.0 && raised_ahead) {
                joining_ = true;
                raised_ahead = false;
            }
        }
    } else {
        base = sub(Vec3{t.position[0], t.ground, t.position[2]}, scale(heading, d * std::cos(angle)));
        offset = d * std::sin(angle);
        if (!operator_have_) ground = t.ground;
    }
    join_ok_ = joining_ && operator_have_;
    join_to_ = world(join_point_[1]);
    // Out to the side only where the ground is the filmer's level: not over a gap or off the end of a
    // ledge. Where you are up on something (a ledge, a rail, a planter) they keep at least a metre off
    // your line, beside it, never on it. The share of the offset used eases, so the spot never jumps.
    const bool raised = raised_ahead || base[1] > ground + 0.3f;
    const float lean = offset == 0.0f ? sign : (offset > 0.0f ? 1.0f : -1.0f);
    // Just seen: ride round beside it, or (now and then, a low one, or when there is no room beside
    // it) stop at its foot with the camera raised over it, as if following you up onto it.
    bool side_found = false;
    float side_wanted = std::max(std::abs(offset), 1.0f) * lean;
    if (raised && cast_) {
        for (const float flip : {1.0f, -1.0f}) {
            for (const float out : {std::max(std::abs(offset), 1.0f), 1.6f, 2.2f, 2.8f, 3.5f}) {
                // Level there and half a metre further in towards your line: room to ride by.
                const Vec3 spot = add(base, scale(side, out * lean * flip));
                const Vec3 inner = add(base, scale(side, (out - 0.5f) * lean * flip));
                const auto g = ground_at(spot[0], spot[2], ground);
                const auto gi = ground_at(inner[0], inner[2], ground);
                if (g && gi && std::abs(*g - world(ground)) < 0.3f && std::abs(*gi - world(ground)) < 0.3f) {
                    side_wanted = out * lean * flip;
                    side_found = true;
                    break;
                }
            }
            if (side_found) break;
        }
    }
    if (raised && !raised_was_ && operator_have_ && post_ == Post::none && rise_index > 0) {
        const float rise = trail_[rise_index].at[1] - ground;
        ledge_side_ = side_found && !(rise < 0.8f && unit(random_) < 0.35f);
        if (!ledge_side_ && rise < 1.3f) {
            // The foot of it: back along the trail from where you popped onto it (before the air), out
            // of the way of the face.
            std::size_t pop = rise_index;
            while (pop > 0 && (trail_[pop - 1].air || trail_[pop - 1].at[1] > ground + 0.3f)) --pop;
            if (pop > 0) --pop;
            const Vec3 face = trail_[pop].at;
            const Vec3 before_face = trail_[pop > 0 ? pop - 1 : 0].at;
            Vec3 back_dir = sub(before_face, face);
            const float bl = flat_length(back_dir);
            back_dir = bl > 0.01f ? scale(Vec3{back_dir[0], 0.0f, back_dir[2]}, 1.0f / bl) : scale(heading, -1.0f);
            post_ = Post::ledge;
            post_spot_ = add(Vec3{face[0], 0.0f, face[2]}, scale(back_dir, 0.9f));
            // The face itself (a little past where you popped), which they never get closer than 0.7 m to.
            ledge_face_ = trail_[rise_index].at;
            ledge_back_ = back_dir;
            post_since_ = now;
            ledge_lift_ = std::clamp(rise + 0.25f, 0.3f, 1.4f);
        }
    }
    raised_was_ = raised;
    if (post_ == Post::ledge) {
        // Back down off it (or a long way along it): they carry on, beside it if you are still up there.
        if (!raised && !t.airborne) post_ = Post::none;
        else if (flat_length(sub(t.position, operator_)) > 4.5f) { post_ = Post::none; ledge_side_ = true; }
    }
    if (joining_ && operator_floor_ < join_to_ - 0.2f) {
        // Ollying up after you: in behind you, on your line, so they land on it, not off its side.
        raised_offset_ += (0.0f - raised_offset_) * ease(dt, 0.15f);
        side_keep_ = 0.0f;
        desired = Vec3{base[0], ground, base[2]};
        if (std::abs(raised_offset_) > 0.02f) desired = add(desired, scale(side, raised_offset_));
    } else if (raised) {
        raised_offset_ += (side_wanted - raised_offset_) * ease(dt, 0.25f);
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
    if (post_ != Post::none) desired = Vec3{post_spot_[0], ground, post_spot_[2]};
    // You standing still: the filmer settles on a spot behind you (left or right of your back, straight
    // behind when those are no good) and stays there, still, so you can still see ahead of you. Only if
    // it goes bad (you turned round and it is in front of you now, or a wall is between) after a moment
    // they move to another.
    const bool still = speed < 0.5f && !t.airborne;
    if (!still) {
        still_since_ = -1.0;
        rest_have_ = false;
    } else if (still_since_ < 0) {
        still_since_ = now;
    }
    if (still && now - still_since_ > 0.4 && operator_have_ && !planted_ && !edge && !holding_ && halt_ == Halt::none && post_ == Post::none) {
        if (!rest_have_ || flat_length(sub(t.position, rest_from_)) > 1.0f) {
            choose_rest(std::max(d, min_gap + 0.2f), height);
        } else if (now >= rest_next_check_) {
            rest_next_check_ = now + 0.5;
            const Vec3 rel = sub(rest_spot_, t.position);
            const bool in_front = rel[0] * heading[0] + rel[2] * heading[2] > 0.2f * d;
            const bool hidden = cast_ && !rest_spot_ok(rest_spot_, d, height);
            if (!in_front && !hidden) rest_bad_since_ = -1.0;
            else if (rest_bad_since_ < 0) rest_bad_since_ = now;
            else if (now - rest_bad_since_ > 1.2) choose_rest(std::max(d, min_gap + 0.2f), height);
        }
        desired = Vec3{rest_spot_[0], ground, rest_spot_[2]};
    }
    const Vec3 centre{t.position[0], world(t.position[1]) + 0.9f, t.position[2]};
    // Never behind a wall: where the spot is out of sight of the skater, it comes in towards them.
    if (cast_ && !edge && !holding_ && halt_ == Halt::none && post_ == Post::none) {
        const Vec3 spot{desired[0], ground + height + 0.1f, desired[2]};
        Vec3 hit{};
        if (blocked(centre, spot, &hit)) {
            const Vec3 to = sub(spot, centre);
            const float full = flat_length(to);
            if (full > 0.01f) {
                const float keep = std::max(min_gap, flat_length(sub(hit, centre)) - 0.4f);
                desired = add(Vec3{centre[0], ground, centre[2]}, scale(Vec3{to[0], 0, to[2]}, keep / full));
            }
        }
    }
    // Gone closer to look over what hides you.
    if (peeking_) desired = Vec3{peek_spot_[0], ground, peek_spot_[2]};
    // Never in front of you: riding along, the spot is at least half a metre behind your shoulders.
    if (!still && speed > 1.0f && post_ != Post::rail_end && !peeking_) {
        const float along = (desired[0] - t.position[0]) * heading[0] + (desired[2] - t.position[2]) * heading[2];
        if (along > -0.5f) {
            desired[0] -= heading[0] * (along + 0.5f);
            desired[2] -= heading[2] * (along + 0.5f);
        }
    }
    // And never too close to you.
    {
        const Vec3 rel{desired[0] - t.position[0], 0.0f, desired[2] - t.position[2]};
        const float gap = flat_length(rel);
        if (gap < min_gap) {
            const Vec3 out = gap > 0.05f ? scale(rel, 1.0f / gap) : scale(heading, -1.0f);
            desired[0] = t.position[0] + out[0] * min_gap;
            desired[2] = t.position[2] + out[2] * min_gap;
        }
    }
    if (edge && !edge_) edge_since_ = now;
    edge_ = edge;
    // A new spot: far off the one wanted (a teleport, a respawn), or after a cut. Never while stopped.
    if (!operator_have_ || (!planted_ && !edge_ && !holding_ && halt_ == Halt::none && post_ == Post::none &&
                            flat_length(sub(operator_, desired)) > 15.0f)) {
        operator_ = desired;
        operator_velocity_ = t.velocity;
        operator_velocity_[1] = 0;
        level_ = ground;
        operator_ground_ = cast_ ? ground_at(desired[0], desired[2], ground).value_or(world(ground)) : ground;
        operator_floor_ = operator_ground_;
        floor_rate_ = 0.0f;
        operator_height_ = operator_ground_ + height;
        operator_height_rate_ = 0;
        last_desired_ = desired;
        last_index_ = trail_index_near(desired[0], desired[2], 4.0f).value_or(trail_.empty() ? 0 : trail_.size() - 1);
        operator_have_ = true;
        edge_ = holding_ = false;
        unseen_since_ = -1.0;
        lens_path_.clear();
        eye_have_ = false;
        placed_at_ = now;
    }
    // Stopping by themselves.
    if (!planted_) {
        // Over a drop too high to huck: braking to a stop short of the edge.
        if (edge_) {
            const float to_edge = (edge_point_[0] - operator_[0]) * edge_dir_[0] + (edge_point_[2] - operator_[2]) * edge_dir_[2];
            begin_halt(Halt::drop, now, to_edge - 0.5f);
        }
        // You slammed: stop where they are (rolling to it), still filming you.
        if (subject.bailing && halt_ != Halt::slam) begin_halt(Halt::slam, now, 6.0f);
        // Too far behind for good footy, and not closing the gap (for a second): ease off and roll to
        // a stop.
        {
            const Vec3 rel{t.position[0] - operator_[0], 0.0f, t.position[2] - operator_[2]};
            const float away = flat_length(rel);
            const float closing = away > 0.01f ? ((operator_velocity_[0] - t.velocity[0]) * rel[0] + (operator_velocity_[2] - t.velocity[2]) * rel[2]) / away : 0.0f;
            // (Only when they cannot keep up: held up by something, or you are faster than a filmer
            // could ever push. Riding as fast as they can and getting closer, they keep at it.)
            const bool held_up = flat_length(operator_velocity_) < std::max(1.5f, speed * 0.6f);
            if (halt_ == Halt::none && post_ == Post::none && !peeking_ && away > std::max(15.0f, distance * 6.0f) && closing < 0.3f && held_up) {
                if (losing_since_ < 0.0) losing_since_ = now;
            } else {
                losing_since_ = -1.0;
            }
            if (losing_since_ >= 0.0 && now - losing_since_ > 1.0) begin_halt(Halt::lag, now, 8.0f);
        }
        // Nowhere clear to ride beside you (an alley, between walls): in behind you on your own line,
        // which you just rode, so is clear; they carry on filming, never stop for it.
        if (trapped && halt_ == Halt::none && post_ == Post::none && !still) trail_follow_until_ = now + 0.5;
        if (halt_ == Halt::wait) halt_ = Halt::none;
        // Waiting and you rode past them (or off away from them): on again, behind you.
        if (halt_ == Halt::wait && speed > 1.0f) {
            const float along = (operator_[0] - t.position[0]) * heading[0] + (operator_[2] - t.position[2]) * heading[2];
            if (along < -min_gap && now - halt_since_ > 0.8) halt_ = Halt::none;
        }
        if (halt_ == Halt::wait && now - halt_since_ > 1.5) halt_ = Halt::none; // look again, riding
    }
    if (halt_ == Halt::none) { drifting_ = false; auto_zoom_ = 1.0f; }
    // A drop ahead of you that would be too high to huck: a filmer sees it coming and eases off, so
    // that if you ollie off it they can stop at the top without slamming on the brakes. (You roll down
    // it instead, a ramp or a bank: they carry on after you.)
    if (cast_ && world_answers() && operator_have_ && now >= caution_next_ && s.stop_at_drops) {
        caution_next_ = now + 0.2;
        if (caution_) {
            const float passed = (t.position[0] - caution_point_[0]) * heading[0] + (t.position[2] - caution_point_[2]) * heading[2];
            // Rolled down it (on the ground, below its top), or well past it: clear.
            if ((!t.airborne && world(t.ground) < caution_top_ - 0.5f) || passed > 3.0f || speed < 0.5f) caution_ = false;
        }
        if (!caution_ && !t.airborne && speed > 2.0f) {
            probe_budget_ += 10;
            const Vec3 way{t.velocity[0] / speed, 0.0f, t.velocity[2] / speed};
            // A lip: the ground falls away steeper than a ramp (or is not there), and a huck's height
            // down within a couple of metres of it. (A hill or a bank goes down gently: no lip.)
            const float top = world(t.ground);
            float last = top;
            for (float along = 0.6f; along <= 6.0f; along += 0.6f) {
                const auto g = ground_at(t.position[0] + way[0] * along, t.position[2] + way[2] * along, last + 0.3f);
                const bool lip = !g || last - *g > 0.33f;
                if (lip) {
                    float lowest = g ? *g : top - 10.0f;
                    for (float more = 0.6f; g && more <= 2.4f && lowest > top - huck_height; more += 0.6f) {
                        const auto further = ground_at(t.position[0] + way[0] * (along + more), t.position[2] + way[2] * (along + more), last + 0.3f);
                        lowest = further ? std::min(lowest, *further) : top - 10.0f;
                    }
                    if (lowest < top - huck_height) {
                        caution_ = true;
                        caution_point_ = Vec3{t.position[0] + way[0] * (along - 0.6f), 0.0f, t.position[2] + way[2] * (along - 0.6f)};
                        caution_top_ = top;
                    }
                    break;
                }
                last = *g;
            }
        }
    }
    // Steering round what is in the way (a pole, a bench, a bin, a low wall): a few times a second
    // they look ahead the way they are riding, and if it is not clear, take the nearest way round that
    // is (towards their spot first), keeping to that side until they are past it (no dithering).
    // (Ollying up after you onto a ledge: on your line, which you just rode; no steering off it.)
    const bool popping_up = joining_ && operator_floor_ < join_to_ - 0.2f;
    if (cast_ && !planted_ && !edge_ && !holding_ && halt_ == Halt::none && !popping_up) {
        const Vec3 to{desired[0] - operator_[0], 0.0f, desired[2] - operator_[2]};
        const float dist = flat_length(to);
        const float riding = flat_length(operator_velocity_);
        // The way to their spot (a turn off it is the way round; turned off the way they happen to be
        // riding, turns would pile up on turns), or, right by it, the way they ride.
        Vec3 way{};
        if (dist > 0.8f) way = scale(to, 1.0f / dist);
        else if (riding > 0.8f) way = scale(Vec3{operator_velocity_[0], 0.0f, operator_velocity_[2]}, 1.0f / riding);
        else if (speed > 0.8f) way = Vec3{t.heading_x, 0.0f, t.heading_z};
        else if (dist > 0.4f) way = scale(to, 1.0f / dist);
        if (flat_length(way) > 0.5f) {
            const auto rotated = [](const Vec3 &v, float a) {
                return Vec3{v[0] * std::cos(a) - v[2] * std::sin(a), 0.0f, v[0] * std::sin(a) + v[2] * std::cos(a)};
            };
            if (now >= steer_next_) {
                steer_next_ = now + 0.1;
                const float look = std::clamp(1.0f + std::max(riding, speed) * 0.3f, 1.5f, 3.2f);
                probe_budget_ += 30; // the look round has its own questions
                ray_budget_ += 24;
                // 0 clear, 1 in the way (or no ground a board would roll on), 2 clear down the middle
                // but too tight for a body (a doorway, a gap between two things).
                const auto check = [&](const Vec3 &heading_way) -> int {
                    // Ground at a third, two thirds and the end: each step up no more than a kerb, each
                    // step down no steeper than a ramp (a slope is many small steps; a bench or a
                    // planter is one big one, a ledge's end a sudden drop).
                    float last = operator_floor_;
                    const float piece = look / 3.0f;
                    // Following you up onto a ledge: its top is ground to them, not a wall.
                    const bool popping = join_ok_ && flat_length(sub(operator_, join_point_)) < look + 1.0f;
                    const float start_low = popping ? std::max(operator_floor_, join_to_) + 0.22f : operator_floor_ + 0.22f;
                    for (const float u : {1.0f / 3.0f, 2.0f / 3.0f, 1.0f}) {
                        const Vec3 at = add(operator_, scale(heading_way, look * u));
                        const auto g = ground_at(at[0], at[2], last + 0.5f);
                        if (!g || (*g - last > 0.25f && !(popping && *g <= join_to_ + 0.15f))) return 1;
                        if (s.stop_at_drops && last - *g > piece * 1.2f + 0.1f) return 1;
                        last = *g;
                    }
                    const Vec3 end = add(operator_, scale(heading_way, look));
                    if (obstructed(Vec3{operator_[0], start_low, operator_[2]}, Vec3{end[0], (popping ? std::max(last, join_to_) : last) + 0.22f, end[2]}, 0.22f, true) ||
                        obstructed(Vec3{operator_[0], operator_floor_ + 1.0f, operator_[2]}, Vec3{end[0], last + 1.0f, end[2]}, 1.0f, true))
                        return 1;
                    // A body wide: either shoulder, at the knee.
                    const Vec3 across{-heading_way[2] * 0.2f, 0.0f, heading_way[0] * 0.2f};
                    for (const float shoulder : {1.0f, -1.0f})
                        if (obstructed(Vec3{operator_[0] + across[0] * shoulder, start_low, operator_[2] + across[2] * shoulder},
                                       Vec3{end[0] + across[0] * shoulder, (popping ? std::max(last, join_to_) : last) + 0.22f, end[2] + across[2] * shoulder}, 0.22f, true))
                            return 2;
                    return 0;
                };
                const auto clear = [&](const Vec3 &heading_way) { return check(heading_way) == 0; };
                const int straight = check(way);
                const bool near_line = at_index && flat_length(sub(trail_[*at_index].at, operator_)) < 1.5f;
                if (straight == 0) {
                    if (now >= steer_hold_) { steer_target_ = 0.0f; steer_side_ = 0.0f; }
                } else if (straight == 2 && near_line) {
                    // Tight, but you rode through it: through it on your line, down the middle.
                    steer_target_ = 0.0f;
                    steer_side_ = 0.0f;
                    trail_follow_until_ = now + 0.8;
                } else {
                    // The smallest turn that is clear, either side; the side already being taken
                    // round first at each angle, else the one towards their spot.
                    float first = steer_side_;
                    if (first == 0.0f) first = (way[0] * to[2] - way[2] * to[0]) >= 0.0f ? 1.0f : -1.0f;
                    bool found = false;
                    for (const float a : {20.0f, 35.0f, 50.0f, 70.0f, 90.0f, 120.0f}) {
                        for (const float side_sign : {first, -first}) {
                            if (clear(rotated(way, a * degrees * side_sign))) {
                                steer_target_ = a * degrees * side_sign;
                                steer_side_ = side_sign;
                                found = true;
                                break;
                            }
                        }
                        if (found) break;
                    }
                    if (!found) {
                        // No clear way round: through it on the line you rode, which is known to be clear.
                        steer_target_ = 0.0f;
                        steer_side_ = 0.0f;
                        trail_follow_until_ = now + 0.8;
                    }
                    steer_hold_ = now + 0.6;
                }
            }
            if (now < trail_follow_until_ && at_index && !trail_.empty()) {
                // On your own line: a point on it a little ahead of where they are.
                std::size_t k = *at_index;
                float went = 0.0f;
                while (k + 1 < trail_.size() && went < 0.7f) { went += flat_length(sub(trail_[k + 1].at, trail_[k].at)); ++k; }
                desired = Vec3{trail_[k].at[0], desired[1], trail_[k].at[2]};
            }
            // Turned into gradually, as on a board: a carve, not a snap.
            steer_angle_ += std::clamp((steer_target_ - steer_angle_) * ease(dt, 0.18f), -120.0f * degrees * dt, 120.0f * degrees * dt);
            if (std::abs(steer_angle_) > 0.5f * degrees) {
                // Ride that way at their pace, then back towards the spot once past.
                const Vec3 turned = rotated(way, steer_angle_);
                desired = add(Vec3{operator_[0], desired[1], operator_[2]}, scale(turned, std::max(dist, 1.2f)));
            }
        }
    } else {
        steer_angle_ = steer_target_ = 0.0f;
        steer_side_ = 0.0f;
    }
    // Riding with you: at your speed (not the spot's jumps from tick to tick).
    const Vec3 desired_rate{t.velocity[0], 0.0f, t.velocity[2]};
    last_desired_ = desired;
    const Vec3 before = operator_;
    const Vec3 velocity_before = operator_velocity_;
    if (planted_ && rolling_stop_) {
        // Stair stop: braking along the way they rode to the stop point, hard enough to make it.
        if (stop_due_) {
            compute_rolling_stop(operator_floor_);
            stop_due_ = false;
        }
        const float remaining = (stop_point_[0] - operator_[0]) * stop_dir_[0] + (stop_point_[2] - operator_[2]) * stop_dir_[2];
        if (!rolling_stop_ || remaining <= 0.02f || roll_speed_ <= 0.05f) {
            rolling_stop_ = false;
            operator_velocity_ = {};
        } else {
            const float brake = std::min(roll_speed_ * roll_speed_ / (2.0f * remaining), 9.0f);
            roll_speed_ = std::max(0.0f, roll_speed_ - brake * dt);
            operator_ = add(operator_, scale(stop_dir_, std::min(roll_speed_ * dt, remaining)));
            operator_velocity_ = scale(stop_dir_, roll_speed_);
        }
    } else if (planted_) {
        operator_velocity_ = {};
    } else if (halt_ != Halt::none && !peeking_) {
        // Rolling to their stop, braking gently (harder only if a drop is close).
        const float remaining = (halt_point_[0] - operator_[0]) * halt_dir_[0] + (halt_point_[2] - operator_[2]) * halt_dir_[2];
        if (remaining <= 0.02f || halt_speed_ <= 0.05f) {
            halt_speed_ = 0.0f;
            operator_velocity_ = {};
            halt_stopped_ = true;
        } else {
            const float brake = std::clamp(halt_speed_ * halt_speed_ / (2.0f * remaining), 1.0f, 12.0f);
            halt_speed_ = std::max(0.0f, halt_speed_ - brake * dt);
            operator_ = add(operator_, scale(halt_dir_, std::min(halt_speed_ * dt, remaining)));
            operator_velocity_ = scale(halt_dir_, halt_speed_);
        }
    } else {
        // At a waiting spot (the end of the ledge, the foot of one): ride there and stop.
        const bool stopping = edge_ || holding_ || post_ != Post::none || peeking_ || halt_ != Halt::none;
        const float omega = post_ != Post::none || peeking_ ? 1.8f : mix(mix(3.2f, 2.0f, reaction), 4.5f, lunge_);
        // Never overtaking you: closing on the line of your shoulders, they ease off in time (no more
        // than about 6 m/s/s of braking to stop half a metre behind it).
        if (speed > 1.0f && post_ != Post::rail_end) {
            const float gap = -((operator_[0] - t.position[0]) * heading[0] + (operator_[2] - t.position[2]) * heading[2]) - 0.5f;
            const float closing = (operator_velocity_[0] - t.velocity[0]) * heading[0] + (operator_velocity_[2] - t.velocity[2]) * heading[2];
            const float most = std::sqrt(12.0f * std::max(gap, 0.0f));
            if (closing > most) {
                operator_velocity_[0] -= heading[0] * (closing - most);
                operator_velocity_[2] -= heading[2] * (closing - most);
            }
        }
        for (std::size_t axis : {std::size_t{0}, std::size_t{2}})
            spring(operator_[axis], operator_velocity_[axis], desired[axis],
                   stopping ? 0.0f : std::clamp(desired_rate[axis], -25.0f, 25.0f), omega, 1.0f, dt);
        // A person on a board: they speed up a push at a time (about 3 m/s/s), brake harder, and are
        // no rocket. The spring's wishes are cut to that.
        if (dt > 0.0f) {
            Vec3 v{(operator_[0] - before[0]) / dt, 0.0f, (operator_[2] - before[2]) / dt};
            const Vec3 change = sub(v, velocity_before);
            const float faster = flat_length(v) > flat_length(velocity_before) ? 1.0f : 0.0f;
            // Pushing off when you push off: they speed up as you do (and a little more while they are
            // behind where they want to be), so the gap stays about the same. Just placed, they are
            // given a moment to get up to your speed.
            const bool settling_in = now - placed_at_ < 1.5;
            const float behind_by = std::max(0.0f, flat_length(sub(desired, before)) - 0.5f);
            const float push = 3.2f + std::max(0.0f, skater_accel_) * 1.1f + std::min(behind_by * 1.5f, 3.0f);
            const float allowed = (faster > 0.0f ? (lunge_ > 0.1f || settling_in ? std::max(6.0f, push) : push) : 7.0f) * dt;
            const float changed = flat_length(change);
            if (changed > allowed) v = add(velocity_before, scale(change, allowed / changed));
            const float top = std::max(7.5f, speed * 1.2f + 1.0f);
            if (const float v_now = flat_length(v); v_now > top) v = scale(v, top / v_now);
            // Easing off before a drop they may have to stop at (and at a jump you are still in the air
            // over): never faster than they could stop from, gently, before it.
            const auto cap_towards = [&](const Vec3 &point, float margin, float decel) {
                const Vec3 to_point{point[0] - before[0], 0.0f, point[2] - before[2]};
                const float dist = flat_length(to_point);
                if (dist < 0.01f) return;
                const Vec3 dir = scale(to_point, 1.0f / dist);
                const float towards = v[0] * dir[0] + v[2] * dir[2];
                const float most_speed = std::sqrt(2.0f * decel * std::max(0.0f, dist - margin));
                if (towards > most_speed) { v[0] -= dir[0] * (towards - most_speed); v[2] -= dir[2] * (towards - most_speed); }
            };
            if (caution_) cap_towards(caution_point_, 0.8f, 3.5f);
            if (holding_) cap_towards(edge_point_, 0.5f, 5.0f);
            if (post_ == Post::ledge) cap_towards(ledge_face_, 0.9f, 6.0f);
            operator_velocity_ = v;
            operator_ = Vec3{before[0] + v[0] * dt, 0.0f, before[2] + v[2] * dt};
        }
        // Still past it (you braked hard, or turned across them): they drop back, at a rider's pace.
        if (speed > 1.0f && post_ != Post::rail_end) {
            const float past = (operator_[0] - t.position[0]) * heading[0] + (operator_[2] - t.position[2]) * heading[2] + 0.3f;
            if (past > 0.0f) {
                const float back = std::min(past, 3.0f * dt);
                operator_[0] -= heading[0] * back;
                operator_[2] -= heading[2] * back;
            }
        }
    }
    // Stopping at an edge: never past it, however fast they were riding.
    if (edge_ || holding_) {
        const float past = (operator_[0] - edge_point_[0]) * edge_dir_[0] + (operator_[2] - edge_point_[2]) * edge_dir_[2];
        if (past > 0.0f) {
            operator_[0] -= edge_dir_[0] * past;
            operator_[2] -= edge_dir_[2] * past;
            const float v = operator_velocity_[0] * edge_dir_[0] + operator_velocity_[2] * edge_dir_[2];
            if (v > 0.0f) { operator_velocity_[0] -= edge_dir_[0] * v; operator_velocity_[2] -= edge_dir_[2] * v; }
            halt_speed_ = 0.0f;
        }
    }
    // Waiting behind a ledge: never nosed up against its face.
    if (post_ == Post::ledge) {
        const float in_front = -((operator_[0] - ledge_face_[0]) * ledge_back_[0] + (operator_[2] - ledge_face_[2]) * ledge_back_[2]) + 0.7f;
        if (in_front > 0.0f) {
            const float step = std::min(in_front, 2.0f * dt + 0.02f);
            operator_[0] += ledge_back_[0] * step;
            operator_[2] += ledge_back_[2] * step;
        }
    }
    // Never too close to you: you rolled up on them, they back off, unhurried.
    if (!planted_) {
        const Vec3 rel{operator_[0] - t.position[0], 0.0f, operator_[2] - t.position[2]};
        const float gap = flat_length(rel);
        if (gap < min_gap - 0.05f) {
            const Vec3 out = gap > 0.05f ? scale(rel, 1.0f / gap) : scale(heading, -1.0f);
            const float step = std::min(min_gap - gap, 2.0f * dt);
            operator_[0] += out[0] * step;
            operator_[2] += out[2] * step;
        }
    }
    if (cast_ && (!planted_ || rolling_stop_)) {
        // Each step is checked against the world: real ground under it, nothing in the way a body wide
        // (either shoulder, at the knee), nothing a board would not roll up or off. Blocked, the filmer
        // slides along what stopped them, or stays.
        const auto try_step = [&](const Vec3 &to) -> bool {
            const auto g = ground_at(to[0], to[2], operator_ground_ + 0.5f, true); // from well above: a rise is found, never missed
            if (!g || !step_allowed(to, *g, s.stop_at_drops)) return false;
            const Vec3 move{to[0] - before[0], 0.0f, to[2] - before[2]};
            const float l = flat_length(move);
            if (l > 1e-4f) {
                // Looked for a little ahead of the step, so the body never ends up against it.
                const Vec3 dir = scale(move, 1.0f / l);
                const Vec3 reach = add(to, scale(dir, 0.3f));
                const Vec3 across{-dir[2] * 0.22f, 0.0f, dir[0] * 0.22f};
                // Ollying up onto the ledge after you: looked along over its top, not into its face.
                const bool popping = join_ok_ && flat_length(sub(to, join_point_)) < 1.6f;
                const float low_from = popping ? std::max(operator_ground_, join_to_) : operator_ground_;
                const float low_to = popping ? std::max(*g, join_to_) : *g;
                if (obstructed(Vec3{before[0], low_from + 0.25f, before[2]}, Vec3{reach[0], low_to + 0.25f, reach[2]}, 0.25f, true)) return false;
                for (const float shoulder : {1.0f, -1.0f})
                    if (obstructed(Vec3{before[0] + across[0] * shoulder, low_from + 0.25f, before[2] + across[2] * shoulder},
                                   Vec3{reach[0] + across[0] * shoulder, low_to + 0.25f, reach[2] + across[2] * shoulder}, 0.25f, true))
                        return false;
            }
            operator_ground_ = *g;
            return true;
        };
        if (flat_length(sub(operator_, before)) > 0.0005f && !try_step(operator_)) {
            // Nosed up against something (a planter, a ledge's end, a trunk): step round it, out to the
            // side of the line they want, at a walking pace. Failing that, slide along it, or stay.
            const Vec3 moved = sub(operator_, before);
            const float l = flat_length(moved);
            const Vec3 across{-moved[2] / l, 0.0f, moved[0] / l};
            const float which = (desired[0] - before[0]) * across[0] + (desired[2] - before[2]) * across[2] >= 0.0f ? 1.0f : -1.0f;
            // (A small step at a walking pace, and it leaves them no faster sideways than that: a
            // shuffle round the corner, not a throw off it.)
            const float sidestep = std::clamp(l * 0.5f, 1.5f * dt, 0.06f);
            const Vec3 round = add(before, scale(across, which * sidestep));
            const Vec3 round_other = add(before, scale(across, -which * sidestep));
            const Vec3 along_x{operator_[0], 0, before[2]}, along_z{before[0], 0, operator_[2]};
            const float shuffle = std::min(sidestep / std::max(dt, 1e-3f), 1.5f);
            if (try_step(round)) { operator_ = round; operator_velocity_ = scale(across, which * shuffle); }
            else if (try_step(along_x)) { operator_ = along_x; operator_velocity_[2] = 0; }
            else if (try_step(along_z)) { operator_ = along_z; operator_velocity_[0] = 0; }
            else if (try_step(round_other)) { operator_ = round_other; operator_velocity_ = scale(across, -which * shuffle); }
            else { operator_ = before; operator_velocity_ = {}; }
        }
        operator_[1] = 0;
    } else if (!cast_) {
        operator_ground_ = ground; // no world to ask: the level from the trail
    }
    // The ground under them: rising, taken at once, so the lens never sinks into it; falling, followed
    // closely (riding down a hill they stay down by the ground, never floating over it like a drone),
    // only an odd one-tick answer is smoothed over.
    {
        const float previous = operator_floor_;
        if (operator_ground_ > operator_floor_) operator_floor_ = operator_ground_;
        else operator_floor_ += (operator_ground_ - operator_floor_) * ease(dt, 0.05f);
        if (dt > 0.0f) floor_rate_ += ((operator_floor_ - previous) / dt - floor_rate_) * ease(dt, 0.1f);
    }
    // Stair stop: once stopped the camera rises a little, to see over the lip and down the set.
    spring(stop_lift_, stop_lift_rate_, stair_style_ && framed_ ? std::clamp(s.stop_lift, 0.0f, 1.5f) : 0.0f, 0.0f, 4.0f, 1.0f, dt);
    // The camera raised: a little over a grind (or hardly at all, pushed out for the landing), over the
    // ledge they wait behind, and to see you over a wall or a fence. Gently, at a person's pace.
    {
        const float grind_lift = grind_weight_ * (grind_style_ == 1 ? 0.1f : grind_style_ == 2 ? 0.0f : 0.35f);
        const float ledge_lift = post_ == Post::ledge ? ledge_lift_ : 0.0f;
        const float lift_target = std::max({grind_lift, ledge_lift, wall_raise_target_});
        // (Up to see you over a wall: a touch quicker, as the arms go up.)
        const bool for_wall = wall_raise_target_ > 0.0f && wall_raise_target_ >= lift_target - 1e-3f;
        spring(lift_, lift_rate_, lift_target, 0.0f, for_wall ? 6.5f : 1.8f, 1.0f, dt);
        lift_rate_ = std::clamp(lift_rate_, -0.8f, for_wall ? 3.6f : 0.8f);
        lift_ = std::max(0.0f, lift_);
        wall_raise_ = wall_raise_target_ > 0.0f ? std::min(lift_, wall_raise_target_) : 0.0f;
        // Through an air the hands come up a touch and back: a soft rise and fall with you, not a jolt.
        // Ollying up onto a ledge after you: the camera goes up with them just before, not after.
        float air_target = t.airborne && lunge_ < 0.2f ? 0.08f : 0.0f;
        if (join_ok_ && operator_floor_ < join_to_ - 0.2f && flat_length(sub(operator_, join_point_)) < 1.4f)
            air_target = std::max(air_target, join_to_ - operator_floor_ + 0.05f);
        spring(air_lift_, air_lift_rate_, air_target, 0.0f, 5.0f, 1.0f, dt);
    }
    const float extra = stop_lift_ + lift_ + air_lift_;
    spring(operator_height_, operator_height_rate_, operator_floor_ + height + extra, floor_rate_, 5.5f, 0.9f, dt);
    operator_height_ = std::clamp(operator_height_, operator_floor_ + lens_clearance,
                                  operator_floor_ + std::max(height + extra + 0.15f, std::min(raise_ceiling, height + 0.15f)));
    const float operator_speed = flat_length(operator_velocity_);
    // Pushing to keep up: a dip and a nod every stride.
    const float behind = flat_length(sub(desired, operator_));
    pushing_ = !planted_ && halt_ == Halt::none && !edge_ && speed > 1.5f && (behind > 0.6f || operator_speed < speed - 0.6f || operator_speed > flat_length(velocity_before) + 0.5f * dt);
    if (halt_ == Halt::none && post_ != Post::none && operator_speed > 1.5f && behind > 1.0f) pushing_ = true;
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
    // Can the filmer see you? Hidden behind something not too tall (you went over a fence or a wall,
    // dropped down behind it, slammed behind it), after a beat the camera is lifted, gently, as far as
    // it takes to see you again (your body or at least your head), up to about 7 ft. If no height up to
    // that sees you from where they are, they roll up closer to whatever is in the way (never past a
    // drop, never into it) and look over from there. Once you are in sight from down low again for a
    // moment, back down; and they ride on as usual.
    if (cast_) {
        const Vec3 head{centre[0], centre[1] + 0.65f, centre[2]};
        const auto sees = [&](const Vec3 &from, bool always) {
            return !blocked(from, centre, nullptr, always) || !blocked(from, head, nullptr, always);
        };
        const bool hidden = !sees(eye, false);
        if (!hidden) unseen_since_ = -1.0;
        else if (unseen_since_ < 0) unseen_since_ = now;
        if (hidden && hidden_since_ < 0.0) {
            hidden_since_ = now;
            wall_delay_ = 0.05f + 0.1f * unit(random_);
        } else if (!hidden && wall_raise_target_ <= 0.0f && !peeking_) {
            hidden_since_ = -1.0;
        }
        if (now >= wall_next_) {
            wall_next_ = now + 0.2;
            const float base_eye = operator_floor_ + height;
            const float away = flat_length(sub(t.position, operator_));
            const float raised_to = operator_floor_ + height + wall_raise_target_;
            if (hidden && hidden_since_ >= 0.0 && now - hidden_since_ > wall_delay_ && away < 30.0f) {
                // Higher (than it is already going to), from where they are.
                bool found = false;
                for (const float above : {0.5f, 0.8f, 1.1f, 1.4f, 1.7f, 1.95f, raise_ceiling}) {
                    const float h = operator_floor_ + above;
                    if (h <= std::max(base_eye, raised_to) + 0.05f) continue;
                    if (sees(Vec3{eye[0], h, eye[2]}, true)) {
                        wall_raise_target_ = std::min(raise_ceiling, above + 0.2f) - height;
                        found = true;
                        break;
                    }
                }
                if (!found && !sees(Vec3{eye[0], raised_to, eye[2]}, true)) {
                    // Not from here: closer to what is in the way, a metre at a time, up to just short
                    // of it, looking from 7 ft at most.
                    wall_raise_target_ = raise_ceiling - height;
                    Vec3 hit{};
                    const Vec3 top{eye[0], operator_floor_ + raise_ceiling, eye[2]};
                    if (blocked(top, centre, &hit, true) || blocked(Vec3{eye[0], operator_floor_ + 0.5f, eye[2]}, centre, &hit, true)) {
                        const Vec3 to{hit[0] - operator_[0], 0.0f, hit[2] - operator_[2]};
                        const float reach = flat_length(to) - 0.6f;
                        if (reach > 0.4f) {
                            const Vec3 dir = scale(to, 1.0f / (reach + 0.6f));
                            for (float step = std::min(1.0f, reach); step <= reach + 1e-3f; step += 1.0f) {
                                const Vec3 at = add(operator_, scale(dir, std::min(step, reach)));
                                const auto g = ground_at(at[0], at[2], operator_floor_ + 0.5f, true);
                                if (!g || std::abs(*g - operator_floor_) > 0.3f) break; // no ground at their level: not past here
                                if (sees(Vec3{at[0], *g + raise_ceiling, at[2]}, true) || step + 1.0f > reach + 1e-3f) {
                                    peek_spot_ = Vec3{at[0], 0.0f, at[2]};
                                    peeking_ = true;
                                    if (sees(Vec3{at[0], *g + raise_ceiling, at[2]}, true)) break;
                                }
                            }
                        }
                    }
                }
                seen_low_since_ = -1.0;
            } else if (wall_raise_target_ > 0.0f || peeking_) {
                // Raised (or gone closer): is there a view from down at the usual height again?
                if (sees(Vec3{eye[0], base_eye, eye[2]}, true)) {
                    if (seen_low_since_ < 0.0) seen_low_since_ = now;
                    else if (now - seen_low_since_ > 1.0) { wall_raise_target_ = 0.0f; hidden_since_ = -1.0; peeking_ = false; }
                } else {
                    seen_low_since_ = -1.0;
                }
                if (away > 35.0f) { wall_raise_target_ = 0.0f; hidden_since_ = -1.0; peeking_ = false; }
            }
        }
    }
    // The zoom: you slammed and look small in the frame (a long way off), slowly in until you fit it;
    // now and then (about 4 in 10) when they stop by themselves, a slight zoom onto you. Off again once
    // they ride on.
    if (halt_ != Halt::none && (halt_stopped_ || operator_speed < 0.3f) && !halt_zoom_rolled_) {
        halt_zoom_rolled_ = true;
        const float far_off = std::max(length(sub(centre, eye)), 0.5f);
        const float base_half = render_fov(s.lens) * 0.5f * degrees;
        const float share = std::atan(0.9f / far_off) / base_half; // half your height against half the frame
        const float most = fish ? 2.0f : max_zoom;
        const bool seen = unseen_since_ < 0.0;
        if (halt_ == Halt::slam) {
            if (slam_zoom_ && seen && share < 0.45f) auto_zoom_ = std::clamp(0.55f / share, 1.0f, most);
        } else if (stop_zoom_ && seen && unit(random_) < 0.4f && share < 0.4f) {
            auto_zoom_ = std::clamp(0.4f / share, 1.0f, fish ? 1.5f : 2.5f);
        }
    }
    // Aim: at where the skater was a moment ago (reaction), a little off-centre (a person framing),
    // caught up hard when the skater is about to leave the frame (a filmer never loses a trick).
    Vec3 now_point = aim_point(t, s);
    // The height the framing follows is eased: up and down with an ollie as a soft rise and fall
    // (never out of the frame: the frame limits below still hold the board in it).
    if (!focus_have_) { focus_y_ = now_point[1]; focus_have_ = true; }
    focus_y_ += (now_point[1] - focus_y_) * ease(dt, t.airborne ? 0.4f : 0.15f);
    const float true_y = now_point[1];
    now_point[1] = focus_y_;
    history_.push_back({now, now_point});
    while (!history_.empty() && now - history_.front().time > 0.6) history_.pop_front();
    const Vec3 late = delayed_point(now, 0.04f + 0.16f * reaction * (0.5f + human));
    // Stopped by you (the left stick): the camera stays on the framing it had, held by hand (it does
    // not follow you round). Stopped by themselves: still, but panning with you.
    edge_locked_ = false;
    const bool frozen = planted_ && !rolling_stop_;
    // Stair stop: from the moment you stop them (still braking), they stop following you and frame the set.
    const bool framing_set = stair_style_ && planted_;
    if (!frozen && !framing_set) {
        framed_ = pan_active_ = composing_ = false;
    } else if (!framed_ && aim_.have) {
        // Just stopped. Stair stop: the shot is framed on the set (the way they were riding), tilted
        // down over it from where the camera will have risen to, and held.
        framed_ = true;
        if (stair_style_) {
            set_yaw_ = rolling_stop_ ? yaw_of(stop_dir_[0], stop_dir_[2]) : aim_.yaw;
            const Vec3 from = rolling_stop_ ? Vec3{stop_point_[0], eye[1], stop_point_[2]} : eye;
            pan_pitch_ = stair_pan_pitch(Vec3{from[0], from[1] + std::clamp(s.stop_lift, 0.0f, 1.5f), from[2]}, set_yaw_, aim_.pitch, operator_floor_);
            pan_active_ = composing_ = true;
            compose_start_ = now;
        }
    }
    // The end of the shot: stopped by themselves and you rode a good way off, the pan carries on
    // slowly the way you were going and drifts down at the same pace, as a filmer lets the shot go.
    if (halt_ != Halt::none && halt_ != Halt::slam && aim_.have) {
        const Vec3 rel{t.position[0] - eye[0], 0.0f, t.position[2] - eye[2]};
        const float away = flat_length(rel);
        const bool leaving = away > 12.0f && (t.velocity[0] * rel[0] + t.velocity[2] * rel[2]) > 0.5f * away;
        if (!drifting_ && leaving) {
            drifting_ = true;
            drift_done_ = 0.0f;
            // The way the pan was going (the way you cross the frame).
            const float cross = rel[0] * t.velocity[2] - rel[2] * t.velocity[0];
            drift_sign_ = std::abs(aim_.yaw_rate) > 0.02f ? (aim_.yaw_rate > 0.0f ? 1.0f : -1.0f) : (cross > 0.0f ? -1.0f : 1.0f);
        } else if (drifting_ && away < 9.0f) {
            drifting_ = false;
        }
    }
    if ((frozen || framing_set) && composing_) {
        // Framing the set: a quick, smooth swing round to look along it and down over it, as a filmer
        // brings the camera onto the spot (the faintest settle as it lands), then held. It does not
        // follow you: you ride into the shot.
        steer_aim(dt, set_yaw_, pan_pitch_, 8.0f, 0.9f);
        const bool calm = std::abs(aim_.yaw_rate) + std::abs(aim_.pitch_rate) < 0.05f &&
                          std::abs(wrap_angle(aim_.yaw - set_yaw_)) < 0.02f && std::abs(aim_.pitch - pan_pitch_) < 0.02f;
        if (rolling_stop_) compose_start_ = now; // held from when they have come to a halt
        else if ((now - compose_start_ > 0.2 && calm) || now - compose_start_ > 1.5) {
            composing_ = pan_active_ = false;
            aim_.yaw_rate = aim_.pitch_rate = 0.0f;
        }
    } else if (frozen && pan_active_) {
        aim_.pitch += (pan_pitch_ - aim_.pitch) * ease(dt, 0.4f);
        aim_.pitch_rate = 0.0f;
    } else if (drifting_ && aim_.have) {
        // About 5 degrees a second each way, easing in, for a while (then held).
        const float rate = 5.0f * degrees * std::clamp(drift_done_ / (8.0f * degrees) + 0.3f, 0.3f, 1.0f);
        if (drift_done_ < 30.0f * degrees) {
            const float step = rate * dt;
            aim_.yaw = wrap_angle(aim_.yaw + drift_sign_ * step);
            aim_.pitch = std::max(aim_.pitch - step, -0.9f);
            drift_done_ += step;
        }
        aim_.yaw_rate = aim_.pitch_rate = 0.0f;
    }
    if ((!frozen && !composing_ && !drifting_) || !aim_.have) {
        const Vec3 to_late = sub(late, eye), to_now = sub(now_point, eye);
        float want_yaw = yaw_of(to_late[0], to_late[2]);
        float want_pitch = frame_pitch(eye, late, t.airborne, 0.0f, s);
        // Through a grind, the board in the middle of the frame.
        if (board_weight_ > 0.01f) {
            const float board_pitch = std::atan2(late[1] + 0.05f - eye[1], std::max(flat_length(to_late), 0.3f));
            want_pitch = mix(want_pitch, board_pitch, board_weight_);
        }
        want_yaw += wobble_[2].sample(now * 0.4) * 3.5f * degrees * human;
        want_pitch += wobble_[3].sample(now * 0.17) * 2.5f * degrees * human;
        const float true_yaw = yaw_of(to_now[0], to_now[2]);
        const Vec3 real_point{now_point[0], true_y, now_point[2]};
        const float true_pitch = frame_pitch(eye, real_point, t.airborne, 0.0f, s);
        // How far off the skater is, against how much of the frame there is to the edge.
        const float view = half_view(s) * (fish ? 1.0f : 0.95f);
        const float off = aim_.have ? std::max(std::abs(wrap_angle(true_yaw - aim_.yaw)), std::abs(true_pitch - aim_.pitch)) : 0.0f;
        const float slack = view * mix(0.3f, 0.55f, human);
        const float catch_up = off > slack ? std::clamp((off - slack) / slack * 3.0f, 0.0f, 5.0f) : 0.0f;
        const float aim_omega = mix(9.0f, 4.2f, reaction) * (1.0f + catch_up) * (halt_ != Halt::none ? 0.7f : 1.0f);
        const float aim_damping = mix(0.95f, 0.62f, human);
        steer_aim(dt, want_yaw, want_pitch, aim_omega, aim_damping);
        // Never let them out of the frame: past 85% of the way to the edge sideways, or the board about to
        // leave the bottom or the top, the aim is dragged back in.
        if (aim_.have) {
            const float limit = view * 0.85f;
            const float dy = wrap_angle(true_yaw - aim_.yaw);
            if (std::abs(dy) > limit) aim_.yaw = wrap_angle(true_yaw - std::copysign(limit, dy));
            const float horizontal = std::max(flat_length(to_now), 0.3f);
            const float feet = std::atan2(true_y + 0.05f - eye[1], horizontal);
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
    frame.fov = render_fov(s.lens) / zoom_;
    frame.valid = valid_camera(frame.camera);
    return frame;
}

void Rig::place_tripod(const Vec3 &eye, float yaw, float pitch) noexcept {
    if (!finite(eye) || !std::isfinite(yaw) || !std::isfinite(pitch)) return;
    tripod_eye_ = eye;
    rest_yaw_ = wrap_angle(yaw);
    rest_pitch_ = std::clamp(pitch, -1.35f, 1.35f);
    aim_ = {rest_yaw_, rest_pitch_, 0, 0, true};
    tripod_have_ = true;
    tracking_ = false;
    lost_since_ = -1.0;
    history_.clear();
}

void Rig::place_tripod_here(const Subject &self, const RigSettings &settings) noexcept {
    if (!self.valid || !finite(self.position)) return;
    const Vec3 &p = self.position;
    float ground = root_known_ ? p[1] - root_above_ground_ : p[1];
    if (cast_ && world_answers()) {
        probe_budget_ += 2;
        if (const auto g = ground_at(p[0], p[2], p[1]); g && *g <= p[1] + 0.2f && *g > p[1] - 2.0f) ground = *g;
    }
    const float fl = std::sqrt(self.facing[0] * self.facing[0] + self.facing[2] * self.facing[2]);
    const float yaw = fl > 0.01f ? yaw_of(self.facing[0], self.facing[2]) : aim_.yaw;
    place_tripod(Vec3{p[0], ground + std::clamp(settings.tripod_height, 0.1f, 2.5f), p[2]}, yaw, -0.06f);
}

// Tripod: the camera stays where it was set down. It holds its framing until the skater rides into
// it, then pans and tilts with them as a filmer on a tripod head does (smooth, a beat late, no
// handheld shake), and once it has lost them (out of sight, out of range) for a couple of seconds it
// settles back to the framing it was set to.
Frame Rig::tripod(double now, float dt, const Subject &subject, const RigSettings &s) {
    dt = std::clamp(dt, 0.0f, 0.1f);
    begin_tick(now);
    filming_ = framing_ = false;
    tripod_mode_ = true;
    stair_style_ = false;
    ff_have_ = false;
    const bool have_subject = subject.valid && finite(subject.position);
    if (have_subject) track(now, dt, subject, subject_, false, true);
    ease_zoom(dt, s);
    if (!tripod_have_) return {};
    const Vec3 eye = tripod_eye_;
    const float view = half_view(s);
    const float across = std::atan(std::tan(view) * (4.0f / 3.0f)); // the 4:3 frame's half width
    const float reaction = std::clamp(s.reaction, 0.0f, 1.0f);
    if (have_subject && subject_.have) {
        const Vec3 centre{subject_.position[0], world(subject_.position[1]) + 0.9f, subject_.position[2]};
        const Vec3 to = sub(centre, eye);
        const float distance = length(to);
        const bool visible = distance < 60.0f && !(cast_ && blocked(eye, centre));
        const float bearing = yaw_of(to[0], to[2]), rise = std::atan2(to[1], std::max(flat_length(to), 0.3f));
        const bool in_frame = visible && std::abs(wrap_angle(bearing - aim_.yaw)) < across * 0.9f && std::abs(rise - aim_.pitch) < view * 0.95f;
        if (!tracking_ && in_frame && s.tripod_track) {
            tracking_ = true;
            lost_since_ = -1.0;
            history_.clear();
        }
        if (tracking_) {
            if (visible) lost_since_ = -1.0;
            else if (lost_since_ < 0) lost_since_ = now;
            if (lost_since_ >= 0 && now - lost_since_ > 2.0) tracking_ = false;
        }
    } else {
        tracking_ = false;
    }
    if (!s.tripod_track) tracking_ = false; // a locked-off shot
    if (tracking_) {
        const Vec3 now_point = aim_point(subject_, s);
        history_.push_back({now, now_point});
        while (!history_.empty() && now - history_.front().time > 0.6) history_.pop_front();
        const Vec3 late = delayed_point(now, 0.05f + 0.15f * reaction);
        const Vec3 to = sub(late, eye);
        steer_aim(dt, yaw_of(to[0], to[2]), frame_pitch(eye, late, subject_.airborne, 0.0f, s), mix(8.0f, 4.0f, reaction), 1.0f);
        // Never let them out of the frame sideways.
        const Vec3 to_now = sub(now_point, eye);
        const float dy = wrap_angle(yaw_of(to_now[0], to_now[2]) - aim_.yaw), limit = across * 0.85f;
        if (std::abs(dy) > limit) aim_.yaw = wrap_angle(yaw_of(to_now[0], to_now[2]) - std::copysign(limit, dy));
    } else {
        steer_aim(dt, rest_yaw_, rest_pitch_, 2.5f, 1.0f); // back to its own framing, unhurried
    }
    float yaw = aim_.yaw, pitch = aim_.pitch, roll = 0.0f;
    Vec3 lens = eye;
    shake(now, 0.0f, std::clamp(s.human, 0.0f, 1.0f) * 0.12f, yaw, pitch, roll, lens);
    Frame frame;
    frame.camera = build(lens, yaw, std::clamp(pitch, -1.45f, 1.45f), roll);
    frame.fov = render_fov(s.lens) / zoom_;
    frame.valid = valid_camera(frame.camera);
    return frame;
}

Frame Rig::film(double now, float dt, const Subject &self, const Subject *target, const StickInput &stick,
                const RigSettings &s) {
    if (!self.valid || !finite(self.position)) return {};
    dt = std::clamp(dt, 0.0f, 0.1f);
    begin_tick(now);
    filming_ = true;
    tripod_mode_ = false;
    track(now, dt, self, self_, false, true);
    const bool aiming_at = target && target->valid && finite(target->position);
    if (aiming_at) track(now, dt, *target, subject_, true, false);
    if (aiming_at != framing_) ff_have_ = false;
    framing_ = aiming_at;
    ease_zoom(dt, s);
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
    // Pointed well up at the sky, the camera comes up to your eye, close to your face; pointed well
    // down, it is held in over your stomach (a filmer does not hold it at the ground and look down at
    // it from arm's length). Eased, so the camera rises and falls with the tilt, never jumps.
    {
        const float pitch_now = aim_.have ? aim_.pitch : 0.0f;
        const float to_eye = std::clamp((pitch_now - 0.35f) / 0.45f, 0.0f, 1.0f);
        const float tucked = std::clamp((-pitch_now - 0.6f) / 0.35f, 0.0f, 1.0f);
        film_up_ += (to_eye - film_up_) * ease(dt, 0.2f);
        film_down_ += (tucked - film_down_) * ease(dt, 0.2f);
    }
    const float hold_base = std::clamp(s.hold_height, 0.05f, 1.8f);
    const float eye_line = 1.5f, stomach = 1.0f;
    float hold = hold_base;
    hold = mix(hold, std::max(hold, stomach), film_down_);
    hold = mix(hold, std::max(hold, eye_line), film_up_);
    const Vec3 body{me.position[0], operator_height_ + hold, me.position[2]};
    const float reach_yaw = aim_.have ? aim_.yaw : heading;
    const float reach = mix(0.62f, 0.3f, std::max(film_up_, film_down_));
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
    float yaw_rate = 0.0f, pitch_rate = 0.0f;
    if (s.assist && aiming_at) {
        // Framing a skater: the aim keeps on them as a person would (a beat late), and the stick
        // shifts the framing; let go and it settles back on them. Worked out from where the camera is
        // held from (your body), not the lens out at arm's length (which swings with the aim, and
        // would chase itself). How fast their bearing moves is fed forward, so the aim turns with them
        // at their pace (less lag, no overshoot) and only small errors are corrected; the hand's shake
        // comes after, from shake().
        const Vec3 now_point = aim_point(subject_, s);
        history_.push_back({now, now_point});
        while (!history_.empty() && now - history_.front().time > 0.6) history_.pop_front();
        const Vec3 late = delayed_point(now, 0.03f + 0.12f * std::clamp(s.reaction, 0.0f, 1.0f));
        const Vec3 pivot{body[0], eye[1], body[2]};
        const Vec3 to = sub(late, pivot);
        want_yaw = yaw_of(to[0], to[2]) + offset_yaw_;
        want_pitch = frame_pitch(pivot, late, subject_.airborne, 0.0f, s) + offset_pitch_;
        if (!touched && now - stick_idle_since_ > 0.35) {
            const float k = ease(dt, 0.9f);
            offset_yaw_ = mix(offset_yaw_, 0.0f, k);
            offset_pitch_ = mix(offset_pitch_, 0.0f, k);
        }
        if (ff_have_ && dt > 0.0f) {
            const float raw_yaw = std::clamp(wrap_angle(want_yaw - ff_last_yaw_) / dt, -8.0f, 8.0f);
            const float raw_pitch = std::clamp((want_pitch - ff_last_pitch_) / dt, -4.0f, 4.0f);
            ff_yaw_rate_ += (raw_yaw - ff_yaw_rate_) * ease(dt, 0.2f);
            ff_pitch_rate_ += (raw_pitch - ff_pitch_rate_) * ease(dt, 0.2f);
        } else {
            ff_yaw_rate_ = ff_pitch_rate_ = 0.0f;
        }
        ff_last_yaw_ = want_yaw;
        ff_last_pitch_ = want_pitch;
        ff_have_ = true;
        // Half of it: all of it amplifies the small noise in where they are (measured on a trace:
        // half frames them closer than none, as smoothly).
        yaw_rate = ff_yaw_rate_ * 0.5f;
        pitch_rate = ff_pitch_rate_ * 0.5f;
        omega = mix(11.0f, 5.5f, std::clamp(s.reaction, 0.0f, 1.0f));
        damping = 1.0f;
    } else {
        ff_have_ = false;
        // Your own framing, held relative to the way you ride: turn and the camera turns with you.
        if (s.recenter && !touched && now - stick_idle_since_ > 0.6) {
            const float k = ease(dt, 0.8f);
            offset_yaw_ = mix(offset_yaw_, 0.0f, k);
            offset_pitch_ = mix(offset_pitch_, -0.12f, k);
        }
        want_yaw = heading + offset_yaw_;
        want_pitch = offset_pitch_;
    }
    // A filmer turns from the waist, not right round: the camera points no further than square off
    // either side of the way you face (from your 9 o'clock round the front to your 3).
    // The middle is the way your body faces (from your hips): on a board that is toe side, where your
    // feet point, not the nose; on foot, ahead.
    {
        constexpr float side = 1.5707963f;
        const float middle = body_yaw_ ? *body_yaw_ : heading;
        const float rel = wrap_angle(want_yaw - middle);
        // Pushing past the stop (the stick, or the assist after someone round behind you): said, so
        // that on foot your whole body can turn round after the camera (filmer.cpp).
        edge_push_ = std::abs(rel) > side + 2.0f * degrees ? (rel > 0.0f ? 1.0f : -1.0f) : 0.0f;
        if (std::abs(rel) > side) {
            want_yaw = middle + std::copysign(side, rel);
            yaw_rate = 0.0f;
            // Your own framing stops at the edge too (no winding on past it).
            if (!(s.assist && aiming_at)) offset_yaw_ = wrap_angle(want_yaw - heading);
        }
        offset_yaw_ = std::clamp(offset_yaw_, -side * 2.0f, side * 2.0f);
    }
    steer_aim(dt, want_yaw, want_pitch, omega, damping, yaw_rate, pitch_rate);
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
    frame.fov = render_fov(s.lens) / zoom_;
    frame.valid = valid_camera(frame.camera);
    return frame;
}
} // namespace dingosdk::filmer
