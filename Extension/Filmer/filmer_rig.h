#pragma once
// The Filmer's camera operator: where a skater with a VX1000 would be, and where they would point it.
//
// Pure math, no game or Windows types, so it builds and runs in the unit tests. The game-facing half
// (filmer.cpp) feeds it the skaters it reads each client tick and writes the frame it returns to the
// native camera.
//
// Conventions are the game's: Y is up, metres, and a camera matrix is four rows of four floats:
// right, up, back (minus the way it looks) and position, the fourth lanes 0, 0, 0, 1. Yaw is the
// heading of a direction in the ground plane, atan2(x, z); pitch is positive looking up.
#include <array>
#include <cstdint>
#include <deque>
#include <vector>
#include <functional>
#include <optional>
#include <random>
#include <unordered_map>

namespace dingosdk::filmer {
using Vec3 = std::array<float, 3>;
using Matrix = std::array<float, 16>;

// off: the game's camera. follow: an AI filmer rides with a skater (you, or someone in the session)
// while you play normally. film: you are the filmer: the left stick rides your skater, the right stick
// points the camera.
enum class Mode : std::uint8_t { off, follow, film };
// Where the AI filmer rides relative to the skater. Auto moves between the others the way a filmer
// picks angles through a line.
enum class Style : std::uint8_t { automatic, behind, side, ahead };
// Which side of the skater the AI filmer keeps to (auto: wherever they happen to be).
enum class Side : std::uint8_t { automatic, left, right };
// fisheye: a Century MK1 on the VX, ridden close. long_lens: the VX's own lens from further away.
enum class Lens : std::uint8_t { fisheye, long_lens };
// The lens slap that marks a clip: how long it takes (seconds), and when the palm hits the glass.
constexpr double slap_duration = 0.6, slap_contact = 0.13;

struct RigSettings {
    Style style = Style::automatic;
    Side side = Side::automatic;
    Lens lens = Lens::fisheye;
    float distance = 1.8f;  // metres from the skater with the fisheye (the long lens rides about 3x as far)
    float height = 0.15f;   // camera above the ground: down by the board, the classic fisheye angle
    float tilt = 0.5f;      // framing: 0 the board near the centre, 1 the board low in the frame (more skater)
    bool stop_at_drops = true; // the filmer stops at the top of a set or a gap you jumped, and films from there
    bool lunge = true;      // fisheye: on an air the filmer pushes in close and low under the trick, then pulls back
    float human = 0.5f;     // 0 steady as a robot, 1 a sketchy filmer: shake, drift, late corrections
    float reaction = 0.4f;  // 0 snaps onto the action, 1 lazy and late
    // film mode
    bool assist = true;     // keep pointing at the target; the stick nudges the framing
    float sensitivity = 1.0f;
    bool invert_y = false;
    bool recenter = false;  // without assist, the aim drifts back ahead when the stick is let go
    float hold_height = 0.2f;  // camera above the ground under you: held low by the board, out at arm's length
};

// One skater as the rig sees it.
struct Subject {
    bool valid{};
    Vec3 position{};          // the skater's root: on the ground while they ride
    Vec3 facing{0, 0, 1};     // the way they face, horizontal
    std::optional<bool> airborne; // known for your own skater; worked out from height for others
    bool bailing{};
};

struct StickInput {
    float x{}, y{}; // the right stick, -1..1 past its dead zone; up is +y
};

struct Frame {
    bool valid{};
    Matrix camera{};
    float fov{}; // the game camera's vertical field of view, degrees
};

// The game camera's field of view each lens renders at. The fisheye's bend is drawn over it by the
// overlay (filmer_look.cpp), which needs the same number.
constexpr float fisheye_render_fov = 100.0f;
constexpr float long_lens_fov = 46.0f;
constexpr float render_fov(Lens lens) noexcept { return lens == Lens::long_lens ? long_lens_fov : fisheye_render_fov; }

// The game's collision: the first surface on the segment from `from` to `to`, if any. Without it the
// rig rides blind (the tests).
using Cast = std::function<bool(const Vec3 &from, const Vec3 &to, Vec3 &hit)>;

class Rig {
public:
    Rig();
    void set_world(Cast cast) { cast_ = std::move(cast); }
    // follow: the filmer stops where they are and films from there, or rides again.
    void set_planted(bool planted) noexcept;
    bool planted() const noexcept { return planted_; }
    // follow: stopped at the top of a drop you went over (stop_at_drops).
    bool at_edge() const noexcept { return edge_; }
    // What the operator is working from, for `filmer trace`.
    struct Debug {
        Vec3 subject{}, operator_at{};
        float ground{}, floor{}, level{}, root_above_ground{}, sample_interval{}, aim_yaw{}, aim_pitch{};
        bool world{}, root_known{}, airborne{}, edge{}, hold{}, planted{};
    };
    Debug debug() const noexcept {
        return {subject_.position, operator_, operator_ground_, operator_floor_, level_, root_above_ground_,
                static_cast<float>(subject_.interval), aim_.yaw, aim_.pitch, world_answers(), root_known_,
                subject_.airborne, edge_, holding_, planted_};
    }
    // Forget everything: the next frame starts the filmer afresh behind the skater.
    void reset() noexcept;
    // AI filmer on `subject`. `now` in seconds on any steady clock, `dt` the seconds since the last call.
    Frame follow(double now, float dt, const Subject &subject, const RigSettings &settings);
    // You film: the camera is in your skater's hands. `target`, when there is one, is who the assist frames.
    Frame film(double now, float dt, const Subject &self, const Subject *target, const StickInput &stick,
               const RigSettings &settings);
    // A hand slapping the lens: the camera jolts. The overlay draws the hand.
    void slap(double now) noexcept;
    // For the menu: where auto is riding (degrees round the skater; 0 behind, 180 ahead).
    float angle_degrees() const noexcept;

private:
    struct Tracked {
        bool have{};
        Vec3 position{}, velocity{};
        float heading_x{0}, heading_z{1}; // eased direction of travel
        float ground{};                   // the ground under them, held through airs
        bool airborne{};
        double air_since{};
        double last_seen{};
        // The game's samples of the skater as they arrived: it can hand over the same position for a
        // frame or two (its physics steps slower than the picture). Between them the skater is
        // placed as the game draws them, so nothing steps.
        std::deque<std::pair<double, Vec3>> samples;
        double interval{1.0 / 60.0};
    };
    // Where the skater has been, at their feet. `air`: they were in the air there (a gap, a set, an
    // ollie); `takeoff` the ground they left from. `id` counts up for as long as the trail is kept.
    struct TrailPoint { Vec3 at{}; bool air{}; float takeoff{}; std::uint64_t id{}; };
    // Walking the trail from where the filmer is towards where they want to be: the level they stay
    // at (they ride slopes and banks, not up onto ledges), and where they must stop (a drop, a gap,
    // or a jump you have not landed yet).
    struct Walk { std::size_t reach{}; float level{}; bool edge{}, hold{}; };
    Walk walk(std::size_t from, std::size_t to, float level, bool stop_at_drops) const;
    std::optional<std::size_t> trail_index_near(float x, float z, float within) const;
    std::size_t trail_index_back(float arc) const;
    struct Aim {
        float yaw{}, pitch{}, yaw_rate{}, pitch_rate{};
        bool have{};
    };
    struct Wobble {
        std::array<float, 3> phase{}, frequency{};
        float sample(double t) const noexcept;
    };
    struct Delayed { double time{}; Vec3 point{}; };

    void track(double now, float dt, const Subject &subject, Tracked &tracked, bool own_trail, bool learn_root);
    void learn_root(float dt, const Vec3 &root);
    // `always`: the ground under the lens itself, asked whatever is left of this tick's budget.
    std::optional<float> ground_at(float x, float z, float near_y, bool always = false) const;
    bool blocked(const Vec3 &from, const Vec3 &to, Vec3 *hit = nullptr) const;
    const TrailPoint *nearest_trail(float x, float z, float within) const;
    bool step_allowed(const Vec3 &to, float to_ground, bool stop_at_drops) const;
    float frame_pitch(const Vec3 &eye, const Vec3 &root, bool airborne, float lift, const RigSettings &settings) const;
    Vec3 trail_point(float arc, Vec3 fallback) const;
    void choose_angle(double now, float speed);
    Vec3 aim_point(const Tracked &tracked, const RigSettings &settings) const;
    Vec3 delayed_point(double now, float delay) const;
    void steer_aim(float dt, float want_yaw, float want_pitch, float omega, float damping);
    float half_view(const RigSettings &settings) const;
    Matrix build(const Vec3 &eye, float yaw, float pitch, float roll) const;
    void shake(double now, float speed, float human, float &yaw, float &pitch, float &roll, Vec3 &eye) const;

    Cast cast_;
    // The game's answers, kept a couple of seconds per 25 cm cell (they are slow, and asking the same
    // question every tick only adds noise), and a budget of new questions per tick.
    struct Probe { bool hit{}; float y{}; double time{}; };
    mutable std::unordered_map<std::uint64_t, Probe> probes_;
    mutable unsigned probe_budget_{}, ray_budget_{};
    double now_{};
    void begin_tick(double now) noexcept;
    // The skater's position is their body's root, a little above the ground the game reports under
    // them. Heights from the trail are in the first, heights from the game in the second: this is
    // the difference, measured as they ride.
    // Learnt from a run of probes (the median), then only nudged by answers near it, so a rail or a
    // ledge under the skater never moves it.
    float root_above_ground_{};
    bool root_known_{};
    std::vector<float> root_samples_;
    float world(float trail_y) const noexcept { return trail_y - root_above_ground_; }
    // How often the game answers when asked for the ground (eased). Next to never: its collision is
    // not there (not loaded, or not reachable), so the ground is taken as level and the trail decides.
    mutable float ground_answers_{1.0f};
    mutable unsigned ground_asked_{};
    bool world_answers() const noexcept { return ground_asked_ < 40 || ground_answers_ > 0.1f; }
    float side_keep_{1.0f};         // how much of the side offset is used, eased (never jumps)
    float raised_offset_{};         // the side offset in use, eased (beside a ledge or a planter it grows)
    std::mt19937 random_;
    Tracked subject_, self_;
    std::deque<TrailPoint> trail_;
    std::deque<Delayed> history_;
    Aim aim_;
    // The filmer's own body.
    bool operator_have_{};
    Vec3 operator_{}, operator_velocity_{}, last_desired_{};
    float operator_height_{}, operator_height_rate_{};
    float last_operator_yaw_{}, lean_{};
    float operator_ground_{};       // the ground under the filmer (probed)
    float operator_floor_{};        // the same, eased: a one-tick odd answer never lifts the camera
    float film_floor_{};            // film mode: the ground under the lens, the same way
    bool film_floor_known_{};
    // The lens's own way (follow): along the path the body rode, at the body's eased speed, so a
    // refused step never shows. `along`: metres along it.
    struct LensPoint { Vec3 at{}; float along{}; };
    std::deque<LensPoint> lens_path_;
    float lens_at_{}, lens_speed_{};
    Vec3 eye_last_{};
    bool eye_have_{};
    float lens_ground_{};
    float lunge_{}, lunge_rate_{};  // 0..1: how far into a lunge under an air
    float side_lock_{};             // +1 the skater's left, -1 their right, 0 either
    bool planted_{}, edge_{}, holding_{};
    bool edge_locked_{};            // stopped at a drop and come to a standstill: the framing is held
    float level_{};                 // the ground level the filmer rides at
    std::size_t last_index_{};      // the trail point nearest the filmer, last tick
    Vec3 edge_point_{}, edge_dir_{0, 0, 1}; // where the filmer stopped, and which way is over it
    std::uint64_t next_trail_id_{};
    bool flat_air_{};               // the jump in progress is a flatground trick (the lunge)
    float takeoff_height_{};
    double edge_since_{}, landed_since_{}, unseen_since_{-1.0};
    // Auto: the angle ridden round the skater (radians) and the one being moved to.
    float angle_{}, angle_target_{}, angle_rate_{};
    double angle_until_{};
    // Film mode: the stick's own offset from the assist's aim, and from straight ahead without it.
    float offset_yaw_{}, offset_pitch_{};
    double stick_idle_since_{};
    float push_phase_{};
    bool pushing_{};
    double slap_at_{-100.0};
    float slap_yaw_{}, slap_pitch_{};
    std::array<Wobble, 9> wobble_{};
};

// Helpers the tests and the overlay share.
float wrap_angle(float radians) noexcept;
bool valid_camera(const Matrix &matrix) noexcept;
} // namespace dingosdk::filmer
