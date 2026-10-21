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
#include <string>
#include <unordered_map>

namespace dingosdk::filmer {
using Vec3 = std::array<float, 3>;
using Matrix = std::array<float, 16>;

// off: the game's camera. follow: an AI filmer rides with a skater (you, or someone in the session)
// while you play normally. film: you are the filmer: the left stick rides your skater, the right stick
// points the camera.
enum class Mode : std::uint8_t { off, follow, film, tripod };
// Where the AI filmer rides relative to the skater. Auto moves between the others the way a filmer
// picks angles through a line.
enum class Style : std::uint8_t { automatic, behind, side, ahead };
// How the AI filmer stops (the left stick, or at a drop). instant: on the spot, holding the framing.
// stair: rolls to a halt short of the first step, lifts the camera a little and pans down to take in
// the whole set or rail.
enum class StopStyle : std::uint8_t { instant, stair };
constexpr std::uint8_t angle_behind = 1, angle_side = 2, angle_ahead = 4;
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
    StopStyle stop_style = StopStyle::instant;
    // The angles Auto moves between (angle_behind | angle_side | angle_ahead): one alone is held.
    std::uint8_t angles = angle_behind | angle_side;
    // Through a line the filmer drifts closer and further (distance) and lower and higher (height): 0
    // never, 1 a lot (about half the distance either way; a foot or so of height).
    float distance_variety = 0.0f, height_variety = 0.0f;
    bool tripod_track = true; // the tripod pans with the skater in frame; off, a locked-off shot
    float stop_lift = 0.3f; // stair stop: how much the camera rises once stopped (metres)
    float tripod_height = 0.5f; // tripod set down where you stand: the lens above the ground
    bool lunge = true;      // fisheye: on an air the filmer pushes in close and low under the trick, then pulls back
    float human = 0.5f;     // 0 steady as a robot, 1 a sketchy filmer: shake, drift, late corrections
    float reaction = 0.4f;  // 0 snaps onto the action, 1 lazy and late
    // film mode
    bool assist = true;     // keep pointing at the target; the stick nudges the framing
    float sensitivity = 1.0f;
    bool invert_y = false;
    bool recenter = false;  // without assist, the aim drifts back ahead when the stick is let go
    float hold_height = 0.2f;  // camera above the ground under you: held low by the board, out at arm's length
    // Cruise control (film mode, framing a friend): you ride at the speed that holds you `cruise_gap`
    // metres behind them; you steer and aim. Your own push and brake still work.
    bool cruise = false;
    float cruise_gap = 3.0f;
    // The mouse turns the camera while it is held still (the AI filmer stopped, or you standing still
    // filming); the wheel zooms the long lens then.
    bool mouse_look = true;
    float mouse_sensitivity = 1.0f;
    // follow: when you slam, the filmer stops (still panning with you) and, if you are small in the
    // frame, zooms in slowly until you fit it.
    bool slam_zoom = true;
    // follow: when the filmer stops by itself (a drop too high to huck, too far behind, nowhere clear),
    // now and then (about 4 times in 10) a slight zoom onto you.
    bool stop_zoom = true;
};

// The highest a filmer hucks down after you (metres, from where they ride to where you landed):
// bigger than that (a big set, a gap down) they stop at the top and film you from there.
constexpr float huck_height = 1.1f;
// The highest a filmer holds the camera up to see over something (about 7 ft, above their ground).
constexpr float raise_ceiling = 2.13f;

// One skater as the rig sees it.
struct Subject {
    bool valid{};
    Vec3 position{};          // the skater's root: on the ground while they ride
    Vec3 facing{0, 0, 1};     // the way they face, horizontal
    std::optional<bool> airborne; // known for your own skater; worked out from height for others
    bool bailing{};
    std::optional<bool> on_foot;  // off the board (known for your own skater)
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
// The long lens's zoom range, as a multiple of its widest (46 degrees down to about 11.5).
constexpr float max_zoom = 4.0f;

// The game's collision: the first surface on the segment from `from` to `to`, if any. Without it the
// rig rides blind (the tests).
using Cast = std::function<bool(const Vec3 &from, const Vec3 &to, Vec3 &hit)>;

class Rig {
public:
    Rig();
    void set_world(Cast cast) { cast_ = std::move(cast); }
    // Which way your body faces (yaw, radians; from your hips, filmer_held.h), when known: the middle
    // of the way the camera may turn while you film (on a board, toe side; on foot, ahead).
    void set_body_yaw(std::optional<float> yaw) noexcept { body_yaw_ = yaw; }
    // On foot the camera turns all the way round (your body pivots after it, filmer_held.h); on a board
    // it stops square off either side.
    void set_pivot(bool on_foot) noexcept { pivot_ = on_foot; }
    // follow: the filmer stops where they are and films from there, or rides again.
    void set_planted(bool planted) noexcept;
    bool planted() const noexcept { return planted_; }
    // follow: stopped at the top of a drop you went over (stop_at_drops).
    bool at_edge() const noexcept { return edge_; }
    // follow: the filmer has stopped by itself, why, and what it is waiting for:
    //   drop   you went off something too high for them to huck: they stopped at the top
    //   lag    too far behind for good footy
    //   slam   you slammed
    //   wait   nowhere clear to ride (wedged by walls): they wait for you to come past
    // Still panning with you. Drop and lag stops end when you push, step off the board, or click the
    // left stick (resume_behind); a slam when you are back up; a wait when there is room again.
    enum class Halt : std::uint8_t { none, drop, lag, slam, wait };
    Halt halt() const noexcept { return halt_; }
    bool lagging() const noexcept { return halt_ == Halt::lag; }
    // Straight back in behind you (the left stick while they are stopped by themselves).
    void resume_behind() noexcept;
    // follow: what the filmer is up to, for the menu.
    std::string doing() const;
    // follow: you are standing still and the filmer has settled on a spot behind you.
    bool resting() const noexcept { return rest_have_; }
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
    // Tripod: a camera set down where it was placed (place_tripod). It holds its framing until `subject`
    // rides into the frame, pans with them while it can see them, and settles back when it has lost them.
    Frame tripod(double now, float dt, const Subject &subject, const RigSettings &settings);
    // Sets the tripod down: the lens at `eye`, framing `yaw` and `pitch` while nobody is in it.
    void place_tripod(const Vec3 &eye, float yaw, float pitch) noexcept;
    // ...or where `self` stands, `settings.tripod_height` above the ground, facing the way they face.
    void place_tripod_here(const Subject &self, const RigSettings &settings) noexcept;
    bool tripod_placed() const noexcept { return tripod_have_; }
    bool tripod_tracking() const noexcept { return tracking_; }
    // You film: the camera is in your skater's hands. `target`, when there is one, is who the assist frames.
    Frame film(double now, float dt, const Subject &self, const Subject *target, const StickInput &stick,
               const RigSettings &settings);
    // A hand slapping the lens: the camera jolts. The overlay draws the hand.
    void slap(double now) noexcept;
    std::optional<float> body_yaw_;
    bool pivot_{};
    // For the menu: where auto is riding (degrees round the skater; 0 behind, 180 ahead).
    float angle_degrees() const noexcept;
    // The camera is held still: the AI filmer stopped (the left stick, or braked at a drop), or you
    // standing still filming. Mouse look and the zoom work then.
    bool stationary() const noexcept;
    // Mouse look, radians: right and down positive (as the mouse moves). Ignored unless stationary.
    void look(float right, float down) noexcept;
    // The long lens's zoom, by a factor (above 1 in, below out). Ignored unless stationary with the
    // long lens; once riding again it eases back out.
    void zoom_by(float factor, const RigSettings &settings) noexcept;
    float zoom() const noexcept { return zoom_; }
    float zoom_target() const noexcept { return zoom_target_; }
    // Cruise control (film mode, framing someone): the speed you should ride at to hold `gap` metres
    // behind them, and whether to ease off (you are on top of them). `active` false: nothing to hold.
    struct Cruise { bool active{}; float speed{}; bool slow{}; float behind{}; };
    Cruise cruise(float gap) const noexcept;

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
    // `always`: asked whatever is left of this tick's budget (what the body is about to do).
    bool blocked(const Vec3 &from, const Vec3 &to, Vec3 *hit = nullptr, bool always = false) const;
    bool obstructed(const Vec3 &from, const Vec3 &to, float above, bool always = false) const;
    const TrailPoint *nearest_trail(float x, float z, float within) const;
    bool step_allowed(const Vec3 &to, float to_ground, bool stop_at_drops) const;
    float frame_pitch(const Vec3 &eye, const Vec3 &root, bool airborne, float lift, const RigSettings &settings) const;
    Vec3 trail_point(float arc, Vec3 fallback) const;
    void choose_angle(double now, float speed, std::uint8_t angles);
    Vec3 aim_point(const Tracked &tracked, const RigSettings &settings) const;
    Vec3 delayed_point(double now, float delay) const;
    void ease_zoom(float dt, const RigSettings &settings) noexcept;
    void steer_aim(float dt, float want_yaw, float want_pitch, float omega, float damping, float yaw_rate = 0.0f, float pitch_rate = 0.0f);
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
    bool edge_locked_{};
    // Stair stop: rolling to a halt at stop_point_ (along stop_dir_), the framing it then pans to, and
    // how far the camera has risen (eased).
    bool stair_style_{}, rolling_stop_{}, stop_due_{}, framed_{}, pan_active_{};
    bool slam_zoom_{true}, stop_zoom_{true};
    // ...and while it turns to frame the set after stopping (then held). set_yaw_: the way the set goes.
    bool composing_{};
    double compose_start_{};
    float set_yaw_{};
    Vec3 stop_point_{}, stop_dir_{0, 0, 1};
    float roll_speed_{}, pan_pitch_{}, stop_lift_{}, stop_lift_rate_{};
    void compute_rolling_stop(float floor) noexcept;
    float stair_pan_pitch(const Vec3 &eye, float yaw, float pitch, float floor) const;
    // Tripod: where it stands, its resting framing, and whether it is panning with someone.
    bool tripod_mode_{}, tripod_have_{}, tracking_{};
    Vec3 tripod_eye_{};
    float rest_yaw_{}, rest_pitch_{};
    double lost_since_{-1.0};
    // Which of follow / film last ran, and whether film had someone to frame.
    bool filming_{}, framing_{};
    // Film mode's aim: how fast the target's bearing is moving (fed forward, so the aim keeps up
    // without lag or overshoot), from the last want.
    float ff_yaw_rate_{}, ff_pitch_rate_{}, ff_last_yaw_{}, ff_last_pitch_{};
    bool ff_have_{};
    // The long lens's zoom: wanted and eased.
    float zoom_{1.0f}, zoom_target_{1.0f};
    // Steering round obstacles: the angle off the way to the spot (eased), the one being steered to,
    // the side being taken round (+1/-1, 0 none) and until when it is kept.
    float steer_angle_{}, steer_target_{}, steer_side_{};
    double steer_next_{}, steer_hold_{}, trail_follow_until_{};            // stopped at a drop and come to a standstill: the framing is held
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
    // The way planner (follow): a few times a second it looks at the spots the filmer could take (out
    // to the side the default cameras sit, the other side, straight behind you) where you are now and
    // where you are heading, and keeps to the one with ground at their level, no wall between them and
    // you and nothing in the way of getting there. `plan_angle_` the angle chosen round you,
    // `plan_risk_` how bad the wanted angle looked (above 0: the planner turned away from it).
    float plan(double now, float want, float d, float height, float speed, bool &trapped);
    float plan_angle_{}, plan_risk_{}, plan_scale_{1.0f};
    double clear_since_{-1.0};
    double plan_next_{}, plan_keep_{};
    // The skater was just placed afresh (a teleport, back to a marker, a respawn).
    bool fresh_{};
    // Stopped by themselves (Halt): rolling to a stop along halt_dir_ at halt_point_, how fast still,
    // whether they have come to a halt, the zoom roll for this stop, and the drift of the pan at the end
    // of the shot.
    Halt halt_{Halt::none};
    double halt_since_{}, bail_over_since_{-1.0};
    Vec3 halt_point_{}, halt_dir_{0, 0, 1};
    float halt_speed_{};
    bool halt_stopped_{}, halt_zoom_rolled_{};
    void begin_halt(Halt why, double now, float room);
    bool drifting_{};
    float drift_sign_{1.0f}, drift_done_{};
    float auto_zoom_{1.0f};
    // The skater pushing (their speed rising on the flat), stepping off the board.
    float speed_slow_{};
    bool pushed_{}, was_on_foot_{}, on_foot_{};
    // A drop ahead of you, seen before you get to it: the filmer eases off so they could stop at it.
    bool caution_{};
    Vec3 caution_point_{};
    float caution_top_{};
    double caution_next_{};
    // Up on something (a ledge, a rail): how, and since when. 0 raise the camera a little and ride out
    // wider, 1 push well out for the landing and roll away, 2 race to the end of it for a low angle.
    bool grinding_{};
    int grind_style_{};
    double grind_since_{}, grind_over_since_{-1.0}, grind_seen_since_{-1.0};
    float grind_weight_{}, board_weight_{};
    // A spot ridden to and waited at: the end of a ledge (a low angle), or the foot of a ledge you
    // ollied onto (the camera raised over it, as if following behind on it).
    enum class Post : std::uint8_t { none, rail_end, ledge };
    Post post_{Post::none};
    Vec3 post_spot_{};
    double post_since_{};
    float ledge_lift_{};
    bool raised_was_{}, ledge_side_{};
    // The camera raised to see you over a wall or a fence (up to raise_ceiling), and when to look again.
    float wall_raise_{}, wall_raise_target_{}, wall_raise_rate_{};
    double wall_next_{}, hidden_since_{-1.0}, seen_low_since_{-1.0};
    float wall_delay_{0.4f};
    // The camera's own lift: the grind's, the ledge's and the wall's, eased together; and the little
    // lift of the hands through an air.
    float lift_{}, lift_rate_{}, air_lift_{}, air_lift_rate_{};
    // The height the framing follows: eased, so an ollie is a soft rise and fall, not a jolt.
    float focus_y_{};
    bool focus_have_{};
    float floor_rate_{};
    double placed_at_{-100.0}, losing_since_{-1.0};
    // You standing still: the spot the filmer settled on, where you were then, and how long the spot
    // has been bad (in front of you, or out of sight of you).
    bool rest_have_{};
    Vec3 rest_spot_{}, rest_from_{};
    double still_since_{-1.0}, rest_bad_since_{-1.0}, rest_next_check_{};
    bool choose_rest(float d, float height);
    bool rest_spot_ok(const Vec3 &spot, float d, float height) const;
};

// Helpers the tests and the overlay share.
float wrap_angle(float radians) noexcept;
bool valid_camera(const Matrix &matrix) noexcept;
} // namespace dingosdk::filmer
