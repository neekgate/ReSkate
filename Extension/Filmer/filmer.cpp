#include "filmer.h"
#include "filmer_beacon.h"
#include "filmer_held.h"
#include "filmer_outfit.h"
#include "Extension/Multiplayer/Remote/native_prop.h"
#include "Engine/Core/Json/json.h"
#include "Engine/Core/Log/logging.h"
#include "Extension/Console/commands.h"
#include "Extension/Multiplayer/Hud/follow_camera.h"
#include "Extension/Multiplayer/Hud/native_party.h"
#include "Extension/Multiplayer/Net/protocol.h"
#include "Extension/Profile/local_profile_runtime.h"
#include "Engine/Game/UI/game_view.h"
#include "Engine/Game/Multiplayer/session_tools.h"
#include "Extension/Skater/client_source_spawn.h"
#include "Extension/Trainer/trainer.h"
#include "Extension/UI/Overlay/overlay.h"
#include <windows.h>
#include <shlobj.h>
#include <algorithm>
#include <utility>
#include <array>
#include <atomic>
#include <cctype>
#include <charconv>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>

// The game-facing half of the Filmer: reads the skaters, runs the rig, holds the camera and the right
// stick, keeps the settings and the clip log. Game thread only, apart from the pad filter.
namespace dingosdk::filmer {
namespace {
constexpr unsigned filter_slot = 1;            // DingoSDKOverlaySetGamepadFilterSlot
constexpr std::uint16_t left_thumb = 0x0040;   // XINPUT_GAMEPAD_LEFT_THUMB: following, stop and film / ride again
constexpr std::uint16_t right_thumb = 0x0080;  // XINPUT_GAMEPAD_RIGHT_THUMB: the stick click, a slap
constexpr float stick_dead_zone = 7849.0f;     // the left stick's XInput dead zone: fine framing
constexpr std::size_t clip_history = 20;

// What the pad filter takes from the game. Read on whatever thread the game polls its pad from.
std::atomic<bool> take_right_stick{};
std::atomic<std::uint16_t> masked_buttons{};

void filter_pad(unsigned, overlay::GamepadValues &pad) noexcept {
    if (take_right_stick.load(std::memory_order_acquire)) {
        pad.right_x = 0;
        pad.right_y = 0;
    }
    pad.buttons = static_cast<std::uint16_t>(pad.buttons & ~masked_buttons.load(std::memory_order_acquire));
}

struct State {
    bool loaded{};
    // The last frame the camera filmed (the tripod is set down where it was), and whether the tripod
    // is still to be set down (the mode was chosen without saying where).
    Frame last_frame;
    bool tripod_place_due{};
    // `filmer trace`: every tick of the next 30 seconds to trace.csv, to see what the filmer saw.
    std::ofstream trace;
    double trace_until{};
    Settings settings;
    Mode last_mode = Mode::follow; // what `filmer toggle` turns on
    Rig rig;
    Mode running = Mode::off;      // the mode the rig last ran in: a change starts it afresh
    bool held{};                   // the camera is the Filmer's
    bool hid_hud{};                // the Filmer hid the game's interface and shows it again when it stops
    ULONGLONG next_attempt{};      // after a refused activation, the next try
    std::string status = "Off.", last;
    std::string subject_name;
    std::vector<PlayerRow> players;
    std::vector<multiplayer::FilmablePlayer> others;
    ULONGLONG players_at{};
    std::uint64_t chosen_target{}; // film mode, target auto: who the assist settled on
    double last_time{};
    // Clips.
    double rolling_since{};
    int clip{1};
    std::vector<Clip> clips;
    double slap_at{-100.0};
    std::uint64_t slap_serial{};
    std::uint16_t previous_buttons{};
    // The snapshot and the settings file.
    std::uint64_t revision{};
    bool view_due{true}, save_due{};
    ULONGLONG next_publish{}, next_save{};
    bool filter_installed{};
    bool world_set{};
    bool planted_shown{}, edge_shown{}, tracking_shown{};
    // What the menu was last shown of the stationary camera's zoom and the cruise control.
    bool stationary_shown{}, cruising_shown{};
    float zoom_shown{1.0f};
    std::string cruise_note;
    float camera_fov{};
    bool holdtest{};               // `filmer holdtest`: the camera held out on your skater, the game's camera
    bool holding{};                // the arm holds the camera out this tick (the camcorder goes in the hand)
    std::string held_status;
    ULONGLONG next_held_status{};
};
State &state() {
    static auto *value = new State;
    return *value;
}

void say(logging::Level level, const std::string &text) { logging::write(level, logging::Channel::skater, text); }

std::string lower(std::string_view text) {
    std::string result(text);
    std::ranges::transform(result, result.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

// ---- storage: %LOCALAPPDATA%\ReSkate\filmer ----------------------------------------------------
std::filesystem::path data_directory() {
    PWSTR raw{};
    std::filesystem::path result;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw)) && raw)
        result = std::filesystem::path(raw) / L"ReSkate" / L"filmer";
    if (raw) CoTaskMemFree(raw);
    return result;
}

std::optional<Json> read_json(const std::filesystem::path &path) {
    try {
        std::ifstream file(path, std::ios::binary);
        if (!file) return std::nullopt;
        const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (text.empty() || text.size() > 256 * 1024) return std::nullopt;
        return Json::parse(text);
    } catch (...) {
        return std::nullopt;
    }
}

float clamped(const Json &o, const char *key, float fallback, float low, float high) {
    if (!o.contains(key) || !o.at(key).is_number()) return fallback;
    const float value = o.at(key).get<float>();
    return std::isfinite(value) ? std::clamp(value, low, high) : fallback;
}
bool flag(const Json &o, const char *key, bool fallback) {
    return o.contains(key) && o.at(key).is_boolean() ? o.at(key).get<bool>() : fallback;
}

void load_store() {
    auto &s = state();
    s.loaded = true;
    const auto json = read_json(data_directory() / L"filmer.json");
    if (!json || !json->is_object()) return;
    try {
        const Settings d;
        auto &r = s.settings.rig;
        const auto &o = *json;
        const auto style = o.value("style", std::string("auto"));
        r.style = style == "behind" ? Style::behind : style == "side" ? Style::side : style == "ahead" ? Style::ahead : Style::automatic;
        const auto side = o.value("side", std::string("auto"));
        r.side = side == "left" ? Side::left : side == "right" ? Side::right : Side::automatic;
        r.lens = o.value("lens", std::string("fisheye")) == "long" ? Lens::long_lens : Lens::fisheye;
        r.distance = clamped(o, "distance", d.rig.distance, 0.6f, 6.0f);
        r.height = clamped(o, "height", d.rig.height, 0.08f, 2.5f);
        r.human = clamped(o, "human", d.rig.human, 0.0f, 1.0f);
        r.reaction = clamped(o, "reaction", d.rig.reaction, 0.0f, 1.0f);
        r.assist = flag(o, "assist", d.rig.assist);
        r.sensitivity = clamped(o, "sensitivity", d.rig.sensitivity, 0.2f, 3.0f);
        r.invert_y = flag(o, "invert", d.rig.invert_y);
        r.recenter = flag(o, "recenter", d.rig.recenter);
        r.hold_height = clamped(o, "hold", d.rig.hold_height, 0.05f, 1.8f);
        r.tilt = clamped(o, "tilt", d.rig.tilt, 0.0f, 1.0f);
        r.stop_at_drops = flag(o, "stop_at_drops", d.rig.stop_at_drops);
        r.cruise = flag(o, "cruise", d.rig.cruise);
        r.stop_style = o.value("stop_style", std::string("instant")) == "stair" ? StopStyle::stair : StopStyle::instant;
        r.angles = static_cast<std::uint8_t>(std::clamp(o.value("angles", 7), 1, 7));
        r.distance_variety = clamped(o, "distance_variety", d.rig.distance_variety, 0.0f, 1.0f);
        r.height_variety = clamped(o, "height_variety", d.rig.height_variety, 0.0f, 1.0f);
        r.tripod_track = flag(o, "tripod_track", d.rig.tripod_track);
        r.stop_lift = clamped(o, "stop_lift", d.rig.stop_lift, 0.0f, 1.5f);
        r.tripod_height = clamped(o, "tripod_height", d.rig.tripod_height, 0.1f, 2.5f);
        r.cruise_gap = clamped(o, "cruise_gap", d.rig.cruise_gap, 1.0f, 12.0f);
        r.mouse_look = flag(o, "mouse_look", d.rig.mouse_look);
        r.mouse_sensitivity = clamped(o, "mouse_sensitivity", d.rig.mouse_sensitivity, 0.1f, 5.0f);
        r.lunge = flag(o, "lunge", d.rig.lunge);
        s.settings.stick_clicks = flag(o, "stick_clicks", d.stick_clicks);
        s.settings.hold_camera = flag(o, "hold_camera", d.hold_camera);
        s.settings.left_hand = false; // the left hand never held it properly: always the right
        s.settings.show_cameras = flag(o, "show_cameras", d.show_cameras);
        s.settings.stand_in = flag(o, "stand_in", d.stand_in);
        s.settings.camera_worn = flag(o, "camera_worn", d.camera_worn);
        s.settings.crouch = flag(o, "crouch", d.crouch);
        s.settings.crouch_depth = clamped(o, "crouch_depth2", d.crouch_depth, 0.0f, 0.5f);
        s.settings.hunch_degrees = clamped(o, "hunch_degrees2", d.hunch_degrees, 0.0f, 60.0f);
        s.settings.two_hands = flag(o, "two_hands", d.two_hands);
        s.settings.camera_item = o.value("camera_item", std::string());
        if (s.settings.camera_item.size() > 200) s.settings.camera_item.clear();
        s.settings.camera_slot = o.value("camera_slot", std::string());
        if (s.settings.camera_slot.size() > 40) s.settings.camera_slot.clear();
        s.settings.camera_model = o.value("camera_model", std::string());
        if (s.settings.camera_model.size() > 200) s.settings.camera_model.clear();
        // Saved before only world and ReSkate props were allowed (a phone crashed the game): the default.
        if (const auto model = lower(s.settings.camera_model); !model.empty() && !model.starts_with("world/") && !model.starts_with("reskate/"))
            s.settings.camera_model.clear();
        // Settings saved before the camera could go this low keep their old height: start them low.
        // (Schema 3: lower again, and the hold height now measured from the ground, not the hips.)
        if (o.value("schema", 1) < 3) {
            r.height = d.rig.height;
            r.hold_height = d.rig.hold_height;
        }
        s.settings.hide_hud = flag(o, "hide_hud", d.hide_hud);
        s.settings.take_stick = flag(o, "take_stick", d.take_stick);
        const auto last = o.value("last_mode", std::string("follow"));
        s.last_mode = last == "film" ? Mode::film : last == "tripod" ? Mode::tripod : Mode::follow;
        if (o.contains("look") && o.at("look").is_object()) {
            const auto &l = o.at("look");
            auto &k = s.settings.look;
            k.enabled = flag(l, "enabled", d.look.enabled);
            k.fisheye = flag(l, "fisheye", d.look.fisheye);
            k.bend = clamped(l, "bend", d.look.bend, 0.0f, 1.0f);
            k.bars = flag(l, "bars", d.look.bars);
            k.hood = flag(l, "hood", d.look.hood);
            k.grade = flag(l, "grade", d.look.grade);
            k.soft = flag(l, "soft", d.look.soft);
            k.scanlines = flag(l, "scanlines", d.look.scanlines);
            k.grain = flag(l, "grain", d.look.grain);
            k.date = flag(l, "date", d.look.date);
            k.viewfinder = flag(l, "viewfinder", d.look.viewfinder);
            k.anywhere = flag(l, "anywhere", d.look.anywhere);
            k.age = clamped(l, "age", d.look.age, 0.0f, 1.0f);
            k.dropouts = flag(l, "dropouts", d.look.dropouts);
            k.head_switching = flag(l, "head_switching", d.look.head_switching);
            k.tracking = flag(l, "tracking", d.look.tracking);
            k.smear = flag(l, "smear", d.look.smear);
            k.iris = flag(l, "iris", d.look.iris);
        }
    } catch (...) {
        say(logging::Level::warning, "Filmer: filmer.json is damaged; starting from what could be read.");
    }
}

void save_store() {
    auto &s = state();
    s.save_due = false;
    try {
        const auto &r = s.settings.rig;
        Json json = Json::object();
        json["schema"] = 3;
        json["style"] = style_name(r.style);
        json["side"] = r.side == Side::left ? "left" : r.side == Side::right ? "right" : "auto";
        json["lens"] = lens_name(r.lens);
        json["distance"] = r.distance;
        json["height"] = r.height;
        json["human"] = r.human;
        json["reaction"] = r.reaction;
        json["assist"] = r.assist;
        json["sensitivity"] = r.sensitivity;
        json["invert"] = r.invert_y;
        json["recenter"] = r.recenter;
        json["hold"] = r.hold_height;
        json["tilt"] = r.tilt;
        json["stop_at_drops"] = r.stop_at_drops;
        json["cruise"] = r.cruise;
        json["stop_style"] = r.stop_style == StopStyle::stair ? "stair" : "instant";
        json["angles"] = static_cast<int>(r.angles);
        json["distance_variety"] = r.distance_variety;
        json["height_variety"] = r.height_variety;
        json["tripod_track"] = r.tripod_track;
        json["stop_lift"] = r.stop_lift;
        json["tripod_height"] = r.tripod_height;
        json["cruise_gap"] = r.cruise_gap;
        json["mouse_look"] = r.mouse_look;
        json["mouse_sensitivity"] = r.mouse_sensitivity;
        json["lunge"] = r.lunge;
        json["stick_clicks"] = s.settings.stick_clicks;
        json["hold_camera"] = s.settings.hold_camera;
        json["camera_hand"] = s.settings.left_hand ? "left" : "right";
        json["show_cameras"] = s.settings.show_cameras;
        json["stand_in"] = s.settings.stand_in;
        json["camera_worn"] = s.settings.camera_worn;
        json["crouch"] = s.settings.crouch;
        json["crouch_depth2"] = s.settings.crouch_depth;
        json["hunch_degrees2"] = s.settings.hunch_degrees;
        json["two_hands"] = s.settings.two_hands;
        json["camera_item"] = s.settings.camera_item;
        json["camera_slot"] = s.settings.camera_slot;
        json["camera_model"] = s.settings.camera_model;
        json["hide_hud"] = s.settings.hide_hud;
        json["take_stick"] = s.settings.take_stick;
        json["last_mode"] = mode_name(s.last_mode);
        const auto &k = s.settings.look;
        Json look = Json::object();
        look["enabled"] = k.enabled;
        look["fisheye"] = k.fisheye;
        look["bend"] = k.bend;
        look["bars"] = k.bars;
        look["hood"] = k.hood;
        look["grade"] = k.grade;
        look["soft"] = k.soft;
        look["scanlines"] = k.scanlines;
        look["grain"] = k.grain;
        look["date"] = k.date;
        look["viewfinder"] = k.viewfinder;
        look["anywhere"] = k.anywhere;
        look["age"] = k.age;
        look["dropouts"] = k.dropouts;
        look["head_switching"] = k.head_switching;
        look["tracking"] = k.tracking;
        look["smear"] = k.smear;
        look["iris"] = k.iris;
        json["look"] = std::move(look);
        const auto directory = data_directory();
        if (directory.empty()) return;
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        const auto target = directory / L"filmer.json", temporary = directory / L"filmer.json.tmp";
        {
            std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
            file << json.dump(2);
            if (!file) return;
        }
        std::filesystem::rename(temporary, target, error);
    } catch (...) {}
}

void changed(State &s) {
    s.view_due = true;
    s.save_due = true;
    s.next_save = GetTickCount64() + 1000;
}

// ---- the skaters ---------------------------------------------------------------------------------
Vec3 facing_of(const multiplayer::Transform &root) {
    const auto body = multiplayer::to_matrix(root);
    const float x = body[8], z = body[10];
    const float l = std::sqrt(x * x + z * z);
    return l > 0.01f ? Vec3{x / l, 0, z / l} : Vec3{0, 0, 1};
}

// Your skater: the pose the session notes each tick when there is one, otherwise (skating alone, with
// no session at all) the trainer's measurement of the skater, taken every tick while you play.
Subject local_subject() {
    Subject result;
    const auto telemetry = trainer::telemetry();
    multiplayer::Transform root;
    if (multiplayer::latest_local_skater(root)) {
        result.valid = true;
        result.position = root.position;
        result.facing = facing_of(root);
    } else if (telemetry.skater) {
        const float heading = telemetry.heading * 3.14159265f / 180.0f;
        result.valid = true;
        result.position = telemetry.position;
        result.facing = {std::sin(heading), 0, std::cos(heading)};
    } else {
        return result;
    }
    if (telemetry.skater) {
        result.airborne = telemetry.airborne;
        // 300s are a wipeout (Extension/Trainer/trainer.cpp).
        result.bailing = telemetry.physics_state >= 300 && telemetry.physics_state < 400;
    }
    return result;
}

Subject remote_subject(const multiplayer::FilmablePlayer &player) {
    Subject result;
    result.valid = true;
    result.position = player.root.position;
    result.facing = facing_of(player.root);
    return result;
}

const multiplayer::FilmablePlayer *find_player(const State &s, std::uint64_t id) {
    for (const auto &player : s.others)
        if (player.id == id) return &player;
    return nullptr;
}

// A name or an id from the command line: an exact id, else the first name that starts with it.
std::optional<std::uint64_t> match_player(const State &s, const std::string &text) {
    std::uint64_t id{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), id);
    if (parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && find_player(s, id)) return id;
    const auto wanted = lower(text);
    for (const auto &player : s.others)
        if (lower(player.name) == wanted) return player.id;
    for (const auto &player : s.others)
        if (lower(player.name).starts_with(wanted)) return player.id;
    return std::nullopt;
}

// The menu's list of players (who is there, how far): a few times a second is plenty. The positions
// the camera frames are fetched every tick (tick()): from a snapshot this old the assist jumped
// after them four times a second.
void refresh_players(State &s, const Subject &self) {
    std::vector<PlayerRow> rows;
    rows.reserve(s.others.size());
    for (const auto &player : s.others) {
        float distance = 0;
        if (self.valid) {
            const float dx = player.root.position[0] - self.position[0], dy = player.root.position[1] - self.position[1],
                        dz = player.root.position[2] - self.position[2];
            distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        }
        rows.push_back({player.id, player.name, distance});
    }
    std::ranges::sort(rows, {}, &PlayerRow::distance);
    const bool same = rows.size() == s.players.size() &&
        std::equal(rows.begin(), rows.end(), s.players.begin(), [](const PlayerRow &a, const PlayerRow &b) {
            return a.id == b.id && a.name == b.name && std::abs(a.distance - b.distance) < 1.0f;
        });
    if (!same) {
        s.players = std::move(rows);
        s.view_due = true;
    }
}

// Film mode, target auto: the skater the assist frames is whoever is close and ahead of where you
// ride, kept until someone is clearly better placed.
const multiplayer::FilmablePlayer *auto_target(State &s, const Subject &self) {
    if (!self.valid || s.others.empty()) {
        s.chosen_target = 0;
        return nullptr;
    }
    const auto score = [&](const multiplayer::FilmablePlayer &player) {
        const float dx = player.root.position[0] - self.position[0], dz = player.root.position[2] - self.position[2];
        const float distance = std::sqrt(dx * dx + dz * dz);
        const float ahead = distance > 0.01f ? (dx * self.facing[0] + dz * self.facing[2]) / distance : 1.0f;
        return distance + (1.0f - ahead) * 6.0f;
    };
    const multiplayer::FilmablePlayer *best{};
    float best_score = 1e9f;
    for (const auto &player : s.others) {
        const float value = score(player);
        if (value < best_score) {
            best_score = value;
            best = &player;
        }
    }
    if (const auto *current = find_player(s, s.chosen_target)) {
        const float kept = score(*current);
        if (kept < 40.0f && (!best || best_score > kept * 0.6f)) return current;
    }
    if (!best || best_score > 45.0f) {
        s.chosen_target = 0;
        return nullptr;
    }
    s.chosen_target = best->id;
    return best;
}

// ---- clips ---------------------------------------------------------------------------------------
std::string clock_text() {
    const auto now_time = std::time(nullptr);
    std::tm local{};
    if (localtime_s(&local, &now_time) != 0) return {};
    std::array<char, 32> text{};
    const auto length = std::strftime(text.data(), text.size(), "%Y-%m-%d %H:%M:%S", &local);
    return std::string(text.data(), length);
}

void log_clip(const Clip &clip, std::string_view mode, std::string_view subject) {
    try {
        const auto directory = data_directory();
        if (directory.empty()) return;
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        const auto path = directory / L"clips.csv";
        const bool fresh = !std::filesystem::exists(path, error);
        std::ofstream file(path, std::ios::binary | std::ios::app);
        if (fresh) file << "ended_at,clip,seconds,mode,subject\n";
        std::string name(subject);
        std::ranges::replace(name, ',', ' ');
        file << clip.ended_at << ',' << clip.number << ',' << std::format("{:.1f}", clip.seconds) << ',' << mode << ',' << name << '\n';
    } catch (...) {}
}

std::string slap(State &s) {
    if (s.settings.mode == Mode::off) return "The Filmer is off: nothing is rolling.";
    const double t = now();
    // A second slap straight after the first is the same mark.
    if (t - s.slap_at < 0.8) return "Already slapped.";
    s.rig.slap(t);
    s.slap_at = t;
    ++s.slap_serial;
    Clip clip{s.clip, std::max(0.0, t - s.rolling_since), clock_text()};
    log_clip(clip, mode_name(s.settings.mode), s.subject_name);
    s.clips.push_back(clip);
    if (s.clips.size() > clip_history) s.clips.erase(s.clips.begin());
    ++s.clip;
    s.rolling_since = t + slap_duration; // the next clip starts once the hand is off the lens
    s.view_due = true;
    say(logging::Level::info, std::format("Filmer: clip {} marked ({:.1f} s).", clip.number, clip.seconds));
    return std::format("Clip {} marked: {:.1f} s.", clip.number, clip.seconds);
}

void publish_view(State &s) {
    s.view_due = false;
    s.next_publish = GetTickCount64() + 250;
    auto next = std::make_shared<View>();
    next->revision = ++s.revision;
    next->settings = s.settings;
    next->active = s.held;
    next->status = s.status;
    next->last = s.last;
    next->subject_name = s.subject_name;
    next->players = s.players;
    next->auto_angle = s.rig.angle_degrees();
    next->planted = s.rig.planted();
    next->at_edge = s.rig.at_edge();
    next->camera_fov = s.camera_fov;
    next->stationary = s.stationary_shown;
    next->zoom = s.zoom_shown;
    next->cruising = s.cruising_shown;
    next->cruise_note = s.cruise_note;
    next->holdtest = s.holdtest;
    next->held_status = s.held_status;
    next->rolling_since = s.rolling_since;
    next->slap_at = s.slap_at;
    next->slap_serial = s.slap_serial;
    next->clip = s.clip;
    next->clips = s.clips;
    publish(std::move(next));
}

void set_status(State &s, std::string text) {
    if (text == s.status) return;
    s.status = std::move(text);
    s.view_due = true;
    // In the log too: what the Filmer is doing, or why it is not filming.
    say(logging::Level::info, "Filmer: " + s.status);
}

// The right stick and its click are the camera's while you film; following, the two stick clicks are
// the filmer's (stop and film, slap). Nothing is taken otherwise.
void update_filter(State &s) {
    const bool film = s.settings.mode == Mode::film && s.held && s.settings.take_stick;
    const bool follow = (s.settings.mode == Mode::follow || s.settings.mode == Mode::tripod) && s.held && s.settings.stick_clicks;
    take_right_stick.store(film, std::memory_order_release);
    masked_buttons.store(film ? right_thumb : follow ? static_cast<std::uint16_t>(left_thumb | right_thumb) : std::uint16_t{0},
                         std::memory_order_release);
    const bool wanted = s.settings.mode != Mode::off;
    if (wanted != s.filter_installed) {
        DingoSDKOverlaySetGamepadFilterSlot(filter_slot, wanted ? &filter_pad : nullptr);
        s.filter_installed = wanted;
    }
}

// The stick clicks: the right one slaps the lens (filming, or following with stick_clicks); the left
// one, following, stops the filmer to film from where they stand, and sends them riding again.
void read_clicks(State &s, std::uint16_t buttons) {
    const auto pressed = [&](std::uint16_t button) { return (buttons & button) != 0 && (s.previous_buttons & button) == 0; };
    const bool follow = s.settings.mode == Mode::follow && s.settings.stick_clicks;
    const bool tripod = s.settings.mode == Mode::tripod && s.settings.stick_clicks;
    if (s.held && pressed(right_thumb) && (s.settings.mode == Mode::film || follow || tripod)) s.last = slap(s);
    if (s.held && tripod && pressed(left_thumb)) {
        // The tripod: set down again where you stand, facing your way.
        if (const Subject self = local_subject(); self.valid) {
            s.rig.place_tripod_here(self, s.settings.rig);
            s.last = "Tripod set down where you stand.";
            s.view_due = true;
        }
    }
    if (s.held && follow && pressed(left_thumb)) {
        s.rig.set_planted(!s.rig.planted());
        s.last = s.rig.planted() ? "The filmer stops and films from here." : "The filmer rides with you again.";
        s.view_due = true;
    }
    s.previous_buttons = buttons;
}

void read_follow_clicks(State &s) {
    overlay::GamepadValues pad;
    if (!DingoSDKOverlayReadGamepad(&pad)) {
        s.previous_buttons = 0;
        return;
    }
    read_clicks(s, pad.buttons);
}

StickInput read_stick(State &s) {
    StickInput stick;
    overlay::GamepadValues pad;
    if (!DingoSDKOverlayReadGamepad(&pad)) {
        s.previous_buttons = 0;
        return stick;
    }
    const float x = static_cast<float>(pad.right_x), y = static_cast<float>(pad.right_y);
    const float magnitude = std::sqrt(x * x + y * y);
    if (magnitude > stick_dead_zone) {
        const float scale = std::clamp((magnitude - stick_dead_zone) / (32767.0f - stick_dead_zone), 0.0f, 1.0f) / magnitude;
        stick.x = x * scale;
        stick.y = y * scale;
    }
    read_clicks(s, pad.buttons);
    return stick;
}

// Gives the camera back. Cheap when the Filmer holds nothing, so it runs every tick while it should
// not film: a lease left behind by a refused frame is given back too.
void release_camera(State &s, std::uintptr_t base, std::uintptr_t client, bool camera_phase) {
    std::string detail;
    if (update_filmer_camera(base, client, false, camera_phase, nullptr, detail) && s.held) {
        s.held = false;
        s.view_due = true;
    }
}
} // namespace

void tick(std::uintptr_t base, std::uintptr_t client, bool ready, bool camera_phase, int game_state) noexcept {
    try {
        auto &s = state();
        if (!s.loaded) {
            load_store();
            s.view_due = true;
        }
        if (!s.world_set) {
            // The game's own collision (the Park Editor's ray, as the drone uses): the AI filmer rides
            // real ground and stays out of walls.
            s.rig.set_world([](const Vec3 &from, const Vec3 &to, Vec3 &hit) {
                static unsigned asked{}, answered{};
                static bool said{};
                const auto found = local_surface_hit(from, to);
                ++asked;
                if (found) ++answered;
                // Said once: without the game's collision the AI filmer goes by your trail alone.
                if (!said && asked >= 600) {
                    said = true;
                    say(logging::Level::info, std::format("Filmer: the game's collision query answered {} of the first {} questions{}", answered, asked,
                        answered ? "." : "; the AI filmer goes by your trail alone (it still stops at drops, but cannot see gaps or walls)."));
                }
                if (!found) return false;
                hit = *found;
                return true;
            });
            s.world_set = true;
        }
        // The game camera's own field of view, for the look drawn over a camera that is not the Filmer's.
        if (!s.held)
            if (const auto game = latest_game_view(std::chrono::milliseconds(1000)); game && game->vertical_fov > 10.0f && game->vertical_fov < 170.0f)
                if (std::abs(game->vertical_fov - s.camera_fov) > 0.5f) {
                    s.camera_fov = game->vertical_fov;
                    s.view_due = true;
                }
        const double t = now();
        const float dt = static_cast<float>(std::clamp(t - s.last_time, 0.0, 0.1));
        s.last_time = t;
        const auto ticks = GetTickCount64();
        const Subject self = local_subject();
        // The camera in your hand this tick (filmer_held.h): filled in below while you film.
        held::Request hold_request;
        hold_request.hand = s.settings.left_hand ? hold::Hand::left : hold::Hand::right;
        hold_request.worn = s.settings.camera_worn;
        hold_request.crouch = s.settings.crouch ? s.settings.crouch_depth : 0.0f;
        hold_request.two_hands = s.settings.two_hands;
        hold_request.hunch = s.settings.crouch ? s.settings.hunch_degrees * 3.14159265f / 180.0f : 0.0f;
        // Where everyone is this frame (the session places them every frame).
        s.others = multiplayer::filmable_players();
        if (ticks >= s.players_at) {
            s.players_at = ticks + 250;
            refresh_players(s, self);
        }
        if (s.settings.mode != s.running) {
            s.rig.reset();
            if (s.running == Mode::off) s.rolling_since = t;
            s.running = s.settings.mode;
            s.chosen_target = 0;
            s.view_due = true;
        }
        update_filter(s);
        if (s.settings.mode == Mode::off) {
            release_camera(s, base, client, camera_phase);
            set_status(s, s.held ? "Giving the camera back." : "Off.");
        } else if (!ready || !self.valid) {
            // Loading, a menu, a cutscene: the game's camera, until the skater is back.
            release_camera(s, base, client, camera_phase);
            s.rig.reset();
            set_status(s, std::format("Waiting for your skater (game state {}{}).", game_state,
                                      self.valid ? ", skater found but not in control" : ""));
        } else {
            Frame frame;
            std::string name;
            // The mouse, while the camera is held still: it turns the camera, and the wheel zooms the
            // long lens (the rig ignores both otherwise). Read every tick so nothing piles up.
            {
                overlay::MouseLook mouse;
                if (DingoSDKOverlayReadMouseLook(&mouse) && s.settings.rig.mouse_look && s.held) {
                    // About a third of a degree a count at 1x, slower zoomed in (the same feel on screen).
                    const float per_count = 0.0022f * s.settings.rig.mouse_sensitivity / s.rig.zoom();
                    if (mouse.x || mouse.y)
                        s.rig.look(static_cast<float>(mouse.x) * per_count,
                                   static_cast<float>(mouse.y) * per_count * (s.settings.rig.invert_y ? -1.0f : 1.0f));
                    if (mouse.wheel) s.rig.zoom_by(std::pow(1.15f, static_cast<float>(mouse.wheel) / 120.0f), s.settings.rig);
                }
            }
            if (s.settings.mode == Mode::tripod) {
                // Set down where the camera was (the mode chosen while filming), or where you stand.
                if (s.tripod_place_due || !s.rig.tripod_placed()) {
                    if (s.held && s.last_frame.valid) {
                        const auto &m = s.last_frame.camera;
                        s.rig.place_tripod(Vec3{m[12], m[13], m[14]}, std::atan2(-m[8], -m[10]), std::asin(std::clamp(-m[9], -1.0f, 1.0f)));
                    } else if (self.valid) {
                        s.rig.place_tripod_here(self, s.settings.rig);
                    }
                    s.tripod_place_due = false;
                }
                Subject subject = self;
                name = "you";
                if (s.settings.subject) {
                    if (const auto *player = find_player(s, s.settings.subject)) {
                        subject = remote_subject(*player);
                        name = player->name;
                    } else {
                        subject = {};
                        name = "nobody (they left)";
                    }
                }
                read_follow_clicks(s);
                frame = s.rig.tripod(t, dt, subject, s.settings.rig);
                if (s.rig.tripod_tracking() != s.tracking_shown) {
                    s.tracking_shown = s.rig.tripod_tracking();
                    s.view_due = true;
                }
            } else if (s.settings.mode == Mode::follow) {
                Subject subject = self;
                name = "you";
                if (s.settings.subject) {
                    if (const auto *player = find_player(s, s.settings.subject)) {
                        subject = remote_subject(*player);
                        name = player->name;
                    } else {
                        subject = {};
                        name = "nobody (they left)";
                    }
                }
                read_follow_clicks(s);
                if (subject.valid) frame = s.rig.follow(t, dt, subject, s.settings.rig);
                if (s.rig.planted() != s.planted_shown || s.rig.at_edge() != s.edge_shown) {
                    s.planted_shown = s.rig.planted();
                    s.edge_shown = s.rig.at_edge();
                    s.view_due = true;
                }
            } else {
                const multiplayer::FilmablePlayer *target{};
                if (s.settings.target) target = find_player(s, s.settings.target);
                else target = auto_target(s, self);
                const Subject target_subject = target ? remote_subject(*target) : Subject{};
                name = target ? target->name : "your own framing";
                const StickInput stick = read_stick(s);
                frame = s.rig.film(t, dt, self, target ? &target_subject : nullptr, stick, s.settings.rig);
            }
            // Cruise control: filming a friend, you ride at the speed that keeps you the gap behind them
            // (the game's own push speed carrier, the trainer's auto push's; your push and brake still
            // work). Off where the session's host has turned boosts off: it is a speed boost.
            {
                float cruise_speed = 0.0f;
                bool cruise_slow = false;
                std::string note;
                if (s.settings.mode == Mode::film && s.settings.rig.cruise && s.held) {
                    if (multiplayer_session_active() && !session_boosts_allowed()) {
                        note = "Cruise control is off in this session: the host has turned boosts off.";
                    } else if (const auto c = s.rig.cruise(s.settings.rig.cruise_gap); c.active) {
                        cruise_speed = c.speed;
                        cruise_slow = c.slow;
                    } else {
                        note = "Cruise control waits for someone to frame, rolling.";
                    }
                }
                set_filmer_cruise(cruise_speed, cruise_slow);
                if ((cruise_speed > 0.0f) != s.cruising_shown || note != s.cruise_note) {
                    s.cruising_shown = cruise_speed > 0.0f;
                    s.cruise_note = std::move(note);
                    s.view_due = true;
                }
            }
            if (s.rig.stationary() != s.stationary_shown || std::abs(s.rig.zoom() - s.zoom_shown) > 0.05f) {
                s.stationary_shown = s.rig.stationary();
                s.zoom_shown = s.rig.zoom();
                s.view_due = true;
            }
            if (s.trace.is_open()) {
                if (t > s.trace_until) {
                    s.trace.close();
                    say(logging::Level::info, "Filmer: trace written to %LOCALAPPDATA%\\ReSkate\\filmer\\trace.csv.");
                } else {
                    const auto d = s.rig.debug();
                    s.trace << std::format("{:.4f},{:.4f},{},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.4f},{:.4f},{:.4f},{},{},{},{},{},{},{:.3f},{:.3f},{:.3f},{}\n",
                        t, dt, mode_name(s.settings.mode), self.position[0], self.position[1], self.position[2],
                        d.subject[0], d.subject[1], d.subject[2], d.operator_at[0], d.operator_at[2], d.ground, d.floor, d.level,
                        d.root_above_ground, d.aim_yaw, d.aim_pitch, d.world ? 1 : 0, d.root_known ? 1 : 0, d.airborne ? 1 : 0,
                        d.edge ? 1 : 0, d.hold ? 1 : 0, d.planted ? 1 : 0, frame.camera[12], frame.camera[13], frame.camera[14], frame.valid ? 1 : 0);
                }
            }
            if (name != s.subject_name) {
                s.subject_name = name;
                s.view_due = true;
            }
            if (!frame.valid) {
                release_camera(s, base, client, camera_phase);
                set_status(s, "Nobody to film.");
            } else if (s.held || ticks >= s.next_attempt) {
                std::string detail;
                const bool applied = update_filmer_camera(base, client, true, camera_phase, &frame.camera, detail, frame.fov);
                if (applied) {
                    s.last_frame = frame;
                    // You film: your arm holds the camera with its lens where the picture is taken from.
                    if (s.settings.mode == Mode::film && s.settings.hold_camera) {
                        hold_request.hold = true;
                        hold_request.hide_own = true;
                        hold_request.lens = {frame.camera[12], frame.camera[13], frame.camera[14]};
                        hold_request.aim = hold::from_rows(frame.camera);
                    }
                    if (!s.held) {
                        s.held = true;
                        s.view_due = true;
                        s.rolling_since = t;
                        say(logging::Level::info, std::format("Filmer: rolling ({}).", mode_name(s.settings.mode)));
                    }
                    // The field of view it is filming at (the long lens zoomed in, say), for the look.
                    if (std::abs(frame.fov - s.camera_fov) > 0.3f) {
                        s.camera_fov = frame.fov;
                        s.view_due = true;
                    }
                    std::string where;
                    if (s.settings.mode == Mode::follow && s.rig.planted()) where = " (stopped: click the left stick to ride again)";
                    else if (s.settings.mode == Mode::follow && s.rig.at_edge()) where = " (stopped at the drop)";
                    if (s.settings.mode == Mode::tripod)
                        set_status(s, s.rig.tripod_tracking() ? "Tripod: panning with " + name + "." : "Tripod: waiting for " + name + " to ride into the frame.");
                    else
                        set_status(s, std::string(s.settings.mode == Mode::follow ? "Filming " : "You film ") + name + "." + where);
                } else if (!detail.empty()) {
                    // Refused (another camera has the view, the camera phase is not here yet): try
                    // again shortly. Without a reason it was only busy this tick, and nothing changes.
                    const bool was_held = s.held;
                    if (s.held) {
                        s.held = false;
                        s.view_due = true;
                    }
                    // Lost mid-shot: straight back next tick. Never had it: not every tick.
                    s.next_attempt = was_held ? ticks : ticks + 500;
                    set_status(s, detail);
                }
            }
        }
        // `filmer holdtest`: with the game's own camera, your skater holds the camera out in front at arm's length,
        // as a fisheye filmer does, and you see it in your hand.
        if (s.holdtest && !hold_request.hold && ready && self.valid) {
            hold_request.hold = true;
            hold_request.relative = true;
            hold_request.lens = {0.8f, -0.3f, 0.0f};
            hold_request.facing = self.facing;
            hold_request.aim = hold::from_yaw_pitch(std::atan2(self.facing[0], self.facing[2]), -0.3f);
            hold_request.show_own = true;
        }
        held::set_local(hold_request);
        s.holding = hold_request.hold;
        if (hold_request.hold) {
            std::uint8_t flags = beacon::flag_filming;
            if (s.settings.left_hand) flags |= beacon::flag_left_hand;
            if (s.settings.camera_worn) flags |= beacon::flag_worn;
            if (s.settings.rig.lens == Lens::long_lens) flags |= beacon::flag_long_lens;
            beacon::publish_local(flags, hold_request.aim);
        }
        // The look's clock runs on even when nothing changed, so the snapshot is not republished every
        // tick: only when something did, and a few times a second for the menu's distances.
        if (s.view_due || ticks >= s.next_publish) publish_view(s);
        if (s.save_due && ticks >= s.next_save) save_store();
    } catch (...) {}
}

namespace {
// A model the camera may be made of: a world prop (as the traffic cone is) or ReSkate's own. Character
// props (a phone) are made to hang on a skater's skeleton; spawned on their own they crash the game.
bool spawnable_model(std::string_view name) {
    return name.starts_with("world/") || name.starts_with("reskate/");
}
} // namespace

void tick_held(std::uintptr_t base, std::uintptr_t client, bool in_level) noexcept {
    try {
        auto &s = state();
        if (!s.loaded) return;
        held::Options options;
        options.show_others = s.settings.show_cameras;
        options.stand_in = s.settings.stand_in || s.holdtest;
        options.blueprint = spawnable_model(lower(s.settings.camera_model)) ? s.settings.camera_model : std::string();
        held::tick(base, client, in_level, options);
        // The camcorder worn as hand wear goes in your hand while the arm holds it out.
        outfit::tick(in_level && s.settings.camera_worn && s.holding, s.settings.camera_item, s.settings.camera_slot);
        if (const auto ticks = GetTickCount64(); ticks >= s.next_held_status) {
            s.next_held_status = ticks + 500;
            auto text = held::status();
            if (s.settings.camera_worn) text += "; " + outfit::status();
            if (text != s.held_status) {
                s.held_status = std::move(text);
                s.view_due = true;
            }
        }
    } catch (...) {}
}

// ---- commands ------------------------------------------------------------------------------------
namespace {
std::optional<float> number(const std::string &text) {
    float value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || !std::isfinite(value)) return std::nullopt;
    return value;
}
std::optional<bool> boolean(const std::string &text) {
    const auto value = lower(text);
    if (value == "1" || value == "on" || value == "true" || value == "yes") return true;
    if (value == "0" || value == "off" || value == "false" || value == "no") return false;
    return std::nullopt;
}

std::string set_mode(State &s, Mode mode, bool hud_hidden) {
    const Mode before = s.settings.mode;
    if (mode == before) return std::format("The Filmer is already {}.", mode_name(mode));
    s.settings.mode = mode;
    if (mode != Mode::off) s.last_mode = mode;
    if (mode == Mode::tripod) s.tripod_place_due = true;
    changed(s);
    // The game's interface while filming: hidden if asked, and only shown again if it was the Filmer
    // that hid it.
    if (before == Mode::off && mode != Mode::off && s.settings.hide_hud && !hud_hidden) {
        console::request_debug(overlay::DebugAction::set_game_ui_hidden, true);
        s.hid_hud = true;
    } else if (mode == Mode::off && s.hid_hud) {
        console::request_debug(overlay::DebugAction::set_game_ui_hidden, false);
        s.hid_hud = false;
    }
    switch (mode) {
    case Mode::follow: return "Filmer on: an AI filmer follows " + (s.settings.subject ? std::string("them") : std::string("you")) + ".";
    case Mode::film: return "Filmer on: you film. Left stick rides, right stick aims, click it to slap the lens.";
    case Mode::tripod: return "Filmer on: a tripod. It holds its framing and pans with " + (s.settings.subject ? std::string("them") : std::string("you")) +
                              " once in it. Click the left stick (or filmer tripod here) to set it down where you stand.";
    case Mode::off: break;
    }
    return "Filmer off.";
}

std::string set_option(State &s, const std::string &key, const std::string &value) {
    auto &r = s.settings.rig;
    auto &k = s.settings.look;
    const auto as_number = number(value);
    const auto as_flag = boolean(value);
    const auto real = [&](float &field, float low, float high) -> std::string {
        if (!as_number) return "error: " + key + " takes a number.";
        field = std::clamp(*as_number, low, high);
        changed(s);
        return std::format("{} = {:.2f}", key, field);
    };
    const auto toggle = [&](bool &field) -> std::string {
        if (!as_flag) return "error: " + key + " takes 0 or 1.";
        field = *as_flag;
        changed(s);
        return key + (field ? " on" : " off");
    };
    if (key == "style") {
        const auto v = lower(value);
        if (v == "auto") { r.style = Style::automatic; r.angles = 7; }
        else if (v == "behind") { r.style = Style::behind; r.angles = angle_behind; }
        else if (v == "side") { r.style = Style::side; r.angles = angle_side; }
        else if (v == "ahead") { r.style = Style::ahead; r.angles = angle_ahead; }
        else return "error: style is auto, behind, side or ahead.";
        changed(s);
        return std::string("style = ") + style_name(r.style);
    }
    if (key == "holdcam") return toggle(s.settings.hold_camera);
    if (key == "cameras") return toggle(s.settings.show_cameras);
    if (key == "standin") return toggle(s.settings.stand_in);
    if (key == "worn") return toggle(s.settings.camera_worn);
    if (key == "crouch") return toggle(s.settings.crouch);
    if (key == "twohands") return toggle(s.settings.two_hands);
    if (key == "crouchdepth") return real(s.settings.crouch_depth, 0.0f, 0.5f);
    if (key == "hunch") return real(s.settings.hunch_degrees, 0.0f, 60.0f);
    if (key == "camitem") {
        const auto v = value == "default" ? std::string() : value;
        if (v.size() > 200 || v.find_first_of(" \t\r\n") != std::string::npos) return "error: camitem is a cosmetic's name (or default).";
        s.settings.camera_item = v;
        changed(s);
        return "Camcorder cosmetic: " + (v.empty() ? std::string(outfit::default_item) : v) + ".";
    }
    if (key == "camslot") {
        const auto v = lower(value == "default" ? std::string() : value);
        if (v.size() > 40 || v.find_first_of(" \t\r\n") != std::string::npos) return "error: camslot is a word of an outfit slot's name (hand, eyewear) or default.";
        s.settings.camera_slot = v;
        changed(s);
        return "The camcorder goes in the " + (v.empty() ? std::string("hand") : v) + " slot while you film.";
    }

    if (key == "cammodel") {
        const auto v = value == "default" ? std::string() : value;
        if (v.size() > 200 || v.find_first_of(" \t\r\n") != std::string::npos) return "error: cammodel is a blueprint name (or default).";
        if (!v.empty() && !spawnable_model(lower(v)))
            return "error: the camera can only be a world prop (world/...) or ReSkate's own (reskate/...): character props such as the phone crash the game when spawned on their own.";
        s.settings.camera_model = v;
        changed(s);
        return "Camera model: " + (v.empty() ? std::string(multiplayer::held_camera_blueprint) : v) + ".";
    }
    if (key == "distvariety") return real(r.distance_variety, 0.0f, 1.0f);
    if (key == "heightvariety") return real(r.height_variety, 0.0f, 1.0f);
    if (key == "track") return toggle(r.tripod_track);
    if (key == "side") {
        const auto v = lower(value);
        if (v == "auto") r.side = Side::automatic;
        else if (v == "left") r.side = Side::left;
        else if (v == "right") r.side = Side::right;
        else if (v == "swap") r.side = r.side == Side::left ? Side::right : Side::left;
        else return "error: side is auto, left, right or swap.";
        changed(s);
        return std::string("side = ") + (r.side == Side::left ? "left" : r.side == Side::right ? "right" : "auto");
    }
    if (key == "lens") {
        const auto v = lower(value);
        if (v == "fisheye" || v == "fish" || v == "mk1") r.lens = Lens::fisheye;
        else if (v == "long" || v == "longlens" || v == "long_lens") r.lens = Lens::long_lens;
        else return "error: lens is fisheye or long.";
        s.rig.reset();
        changed(s);
        return std::string("lens = ") + lens_name(r.lens);
    }
    if (key == "distance") return real(r.distance, 0.6f, 6.0f);
    if (key == "height") return real(r.height, 0.08f, 2.5f);
    if (key == "human") return real(r.human, 0.0f, 1.0f);
    if (key == "reaction") return real(r.reaction, 0.0f, 1.0f);
    if (key == "sensitivity") return real(r.sensitivity, 0.2f, 3.0f);
    if (key == "hold") return real(r.hold_height, 0.05f, 1.8f);
    if (key == "tilt") return real(r.tilt, 0.0f, 1.0f);
    if (key == "drops") return toggle(r.stop_at_drops);
    if (key == "clicks") return toggle(s.settings.stick_clicks);
    if (key == "anywhere") return toggle(k.anywhere);
    if (key == "lunge") return toggle(r.lunge);
    if (key == "cruise") return toggle(r.cruise);
    if (key == "stop") {
        const auto v = lower(value);
        if (v != "instant" && v != "stair") return "error: stop is instant or stair.";
        r.stop_style = v == "stair" ? StopStyle::stair : StopStyle::instant;
        changed(s);
        return v == "stair" ? "Stops roll to a halt short of the first step, rise a little and pan down over the set."
                            : "Stops are on the spot, holding the framing.";
    }
    if (key == "lift") return real(r.stop_lift, 0.0f, 1.5f);
    if (key == "tripodheight") return real(r.tripod_height, 0.1f, 2.5f);
    if (key == "gap") return real(r.cruise_gap, 1.0f, 12.0f);
    if (key == "mouse") return toggle(r.mouse_look);
    if (key == "mousesens") return real(r.mouse_sensitivity, 0.1f, 5.0f);
    if (key == "age") return real(k.age, 0.0f, 1.0f);
    if (key == "dropouts") return toggle(k.dropouts);
    if (key == "headswitch") return toggle(k.head_switching);
    if (key == "tracking") return toggle(k.tracking);
    if (key == "smear") return toggle(k.smear);
    if (key == "iris") return toggle(k.iris);
    if (key == "assist") return toggle(r.assist);
    if (key == "invert") return toggle(r.invert_y);
    if (key == "recenter") return toggle(r.recenter);
    if (key == "hidehud") return toggle(s.settings.hide_hud);
    if (key == "stick") return toggle(s.settings.take_stick);
    if (key == "look") return toggle(k.enabled);
    if (key == "fisheye") return toggle(k.fisheye);
    if (key == "bend") return real(k.bend, 0.0f, 1.0f);
    if (key == "bars") return toggle(k.bars);
    if (key == "hood") return toggle(k.hood);
    if (key == "grade") return toggle(k.grade);
    if (key == "soft") return toggle(k.soft);
    if (key == "scanlines") return toggle(k.scanlines);
    if (key == "grain") return toggle(k.grain);
    if (key == "date") return toggle(k.date);
    if (key == "viewfinder") return toggle(k.viewfinder);
    return "error: unknown option " + key + ".";
}

std::string describe(const State &s) {
    const auto &r = s.settings.rig;
    std::string text = std::format("Filmer {}: {} | lens {} | style {} | distance {:.1f} m | height {:.1f} m | human {:.2f} | reaction {:.2f}",
        mode_name(s.settings.mode), s.status, lens_name(r.lens), style_name(r.style), r.distance, r.height, r.human, r.reaction);
    if (s.settings.mode != Mode::off) text += " | filming " + s.subject_name;
    text += std::format(" | clip {}", s.clip);
    text += "\n" + held::status();
    return text;
}
} // namespace

std::string command(std::string_view verb, const std::vector<std::string> &arguments, bool hud_hidden) {
    auto &s = state();
    if (!s.loaded) load_store();
    const auto arg = [&](std::size_t i) { return i < arguments.size() ? arguments[i] : std::string{}; };
    std::string reply;
    if (verb == "status") {
        reply = describe(s);
    } else if (verb == "toggle") {
        const auto wanted = lower(arg(0));
        Mode mode = s.settings.mode == Mode::off ? s.last_mode : Mode::off;
        if (wanted == "follow") mode = s.settings.mode == Mode::follow ? Mode::off : Mode::follow;
        else if (wanted == "film") mode = s.settings.mode == Mode::film ? Mode::off : Mode::film;
        else if (wanted == "tripod") mode = s.settings.mode == Mode::tripod ? Mode::off : Mode::tripod;
        reply = set_mode(s, mode, hud_hidden);
    } else if (verb == "mode") {
        const auto wanted = lower(arg(0));
        if (wanted == "off") reply = set_mode(s, Mode::off, hud_hidden);
        else if (wanted == "follow") reply = set_mode(s, Mode::follow, hud_hidden);
        else if (wanted == "film") reply = set_mode(s, Mode::film, hud_hidden);
        else if (wanted == "tripod") reply = set_mode(s, Mode::tripod, hud_hidden);
        else reply = "error: mode is off, follow, film or tripod.";
    } else if (verb == "subject" || verb == "target") {
        const bool subject = verb == "subject";
        std::string wanted;
        for (std::size_t i = 0; i < arguments.size(); ++i) wanted += (i ? " " : "") + arguments[i];
        const auto key = lower(wanted);
        auto &field = subject ? s.settings.subject : s.settings.target;
        if (key.empty() || key == "me" || key == "auto" || key == "0") {
            field = 0;
            s.rig.reset();
            reply = subject ? "The AI filmer follows you." : "The assist frames whoever is closest ahead.";
        } else if (const auto id = match_player(s, wanted)) {
            field = *id;
            s.rig.reset();
            const auto *player = find_player(s, *id);
            reply = std::string(subject ? "The AI filmer follows " : "The assist frames ") + (player ? player->name : wanted) + ".";
        } else {
            reply = "error: nobody called " + wanted + " is in the session.";
        }
        s.view_due = true;
    } else if (verb == "set") {
        reply = arguments.size() >= 2 ? set_option(s, lower(arg(0)), arg(1)) : "error: filmer set <option> <value>.";
    } else if (verb == "plant") {
        if (s.settings.mode != Mode::follow) reply = "Only the AI filmer stops to film (filmer mode follow).";
        else {
            const auto wanted = lower(arg(0));
            const bool plant = wanted == "on" || wanted == "1" ? true : wanted == "off" || wanted == "0" ? false : !s.rig.planted();
            s.rig.set_planted(plant);
            reply = plant ? "The filmer stops and films from here." : "The filmer rides with you again.";
        }
    } else if (verb == "angle") {
        // The angles the AI filmer films from: one held, or several Auto moves between.
        auto &r = s.settings.rig;
        const auto wanted = lower(arg(0)), which = lower(arg(1));
        const auto bit = [](const std::string &name) -> std::uint8_t {
            return name == "behind" ? angle_behind : name == "side" ? angle_side : name == "ahead" ? angle_ahead : 0;
        };
        if (wanted == "next") {
            // Auto (all of them), behind, side, ahead, and round again.
            r.angles = static_cast<std::uint8_t>(r.angles == 7 ? angle_behind : r.angles == angle_behind ? angle_side
                                                 : r.angles == angle_side ? angle_ahead : 7);
        } else if (wanted == "auto" || wanted == "all") {
            r.angles = 7;
        } else if (bit(wanted)) {
            r.angles = bit(wanted);
        } else if (wanted == "toggle" && bit(which)) {
            const auto toggled = static_cast<std::uint8_t>(r.angles ^ bit(which));
            if (toggled) r.angles = toggled; // never none
        } else {
            reply = "error: filmer angle next|auto|behind|side|ahead|toggle behind|side|ahead.";
        }
        if (reply.empty()) {
            r.style = r.angles == angle_behind ? Style::behind : r.angles == angle_side ? Style::side
                    : r.angles == angle_ahead ? Style::ahead : Style::automatic;
            changed(s);
            std::string names;
            for (const auto &[b, name] : {std::pair{angle_behind, "behind"}, std::pair{angle_side, "side"}, std::pair{angle_ahead, "ahead"}})
                if (r.angles & b) names += (names.empty() ? "" : ", ") + std::string(name);
            reply = r.style == Style::automatic ? "Angles: auto between " + names + "." : "Angle: " + names + ", held.";
        }
    } else if (verb == "tripod" && lower(arg(0)) == "track") {
        auto &r = s.settings.rig;
        const auto wanted = lower(arg(1));
        r.tripod_track = wanted == "on" || wanted == "1" ? true : wanted == "off" || wanted == "0" ? false : !r.tripod_track;
        changed(s);
        reply = r.tripod_track ? "Tripod tracking on: it pans with the skater once they ride into the frame."
                               : "Tripod tracking off: a locked-off shot.";
    } else if (verb == "tripod") {
        // Set the tripod down: where the Filmer's camera is now (an AI filmer you stopped where you
        // like, say), or with "here" (or no camera rolling) where you stand, facing your way.
        const bool here = lower(arg(0)) == "here" || !s.held || !s.last_frame.valid;
        if (here) {
            const Subject self = local_subject();
            if (!self.valid) reply = "error: no skater to set the tripod down by.";
            else s.rig.place_tripod_here(self, s.settings.rig);
        } else {
            const auto &m = s.last_frame.camera;
            s.rig.place_tripod(Vec3{m[12], m[13], m[14]}, std::atan2(-m[8], -m[10]), std::asin(std::clamp(-m[9], -1.0f, 1.0f)));
        }
        if (reply.empty()) {
            const auto before = s.settings.mode;
            if (before != Mode::tripod) (void)set_mode(s, Mode::tripod, hud_hidden);
            s.tripod_place_due = false;
            reply = here ? "Tripod set down where you stand, facing your way. It pans with you once you ride into the frame."
                         : "Tripod set down where the camera was. It pans with you once you ride into the frame.";
        }
    } else if (verb == "cruise") {
        const auto wanted = lower(arg(0));
        auto &r = s.settings.rig;
        r.cruise = wanted == "on" || wanted == "1" ? true : wanted == "off" || wanted == "0" ? false : !r.cruise;
        changed(s);
        reply = r.cruise ? std::format("Cruise control on: filming someone, you hold {:.1f} m behind them (you steer and aim).", r.cruise_gap)
                         : "Cruise control off.";
    } else if (verb == "zoom") {
        const auto wanted = lower(arg(0));
        const float factor = wanted == "in" ? 1.25f : wanted == "out" ? 0.8f : wanted == "reset" ? 0.0001f : 0.0f;
        if (factor == 0.0f) reply = "error: filmer zoom in|out|reset.";
        else if (s.settings.rig.lens != Lens::long_lens) reply = "The zoom is the long lens's: filmer set lens long.";
        else if (!s.rig.stationary()) reply = "The zoom works while the camera is held still (stopped, or you standing still filming).";
        else {
            s.rig.zoom_by(factor, s.settings.rig);
            reply = std::format("Zoom {:.1f}x.", s.rig.zoom_target());
        }
    } else if (verb == "trace") {
        try {
            const auto directory = data_directory();
            std::error_code error;
            std::filesystem::create_directories(directory, error);
            if (s.trace.is_open()) s.trace.close();
            s.trace.open(directory / L"trace.csv", std::ios::binary | std::ios::trunc);
            s.trace << "time,dt,mode,game_x,game_y,game_z,skater_x,skater_y,skater_z,filmer_x,filmer_z,ground,floor,level,root_above_ground,"
                       "aim_yaw,aim_pitch,world,root_known,airborne,edge,hold,planted,eye_x,eye_y,eye_z,valid\n";
            s.trace_until = now() + 30.0;
            reply = s.trace ? "Tracing the Filmer for 30 seconds to %LOCALAPPDATA%\\ReSkate\\filmer\\trace.csv: skate a line now."
                            : "error: could not write trace.csv.";
        } catch (...) {
            reply = "error: could not write trace.csv.";
        }
    } else if (verb == "holdtest") {
        // Your skater holds the camera out with the game's own camera, to see the arm and the model.
        const auto wanted = lower(arg(0));
        s.holdtest = wanted == "on" || wanted == "1" ? true : wanted == "off" || wanted == "0" ? false : !s.holdtest;
        reply = !s.holdtest ? "Hold test off."
              : s.settings.mode == Mode::film ? "Hold test on: switch the Filmer off (or to another mode) to see your skater hold the camera."
                                              : "Hold test on: your skater holds the camera out in front at arm's length. Others in the session see it too.";
    } else if (verb == "slap") {
        reply = slap(s);
    } else if (verb == "clips") {
        if (s.clips.empty()) reply = "No clips marked yet. Slap the lens to end one.";
        else {
            reply = "Clips (newest last):";
            for (const auto &clip : s.clips) reply += std::format("\n  #{} {:.1f} s, ended {}", clip.number, clip.seconds, clip.ended_at);
            reply += "\nThe full list is in %LOCALAPPDATA%\\ReSkate\\filmer\\clips.csv.";
        }
    } else if (verb == "reset") {
        const auto mode = s.settings.mode;
        const auto subject = s.settings.subject, target = s.settings.target;
        s.settings = {};
        s.settings.mode = mode;
        s.settings.subject = subject;
        s.settings.target = target;
        s.rig.reset();
        changed(s);
        reply = "Filmer settings back to their defaults.";
    } else {
        reply = "error: unknown filmer command.";
    }
    s.last = reply;
    s.view_due = true;
    return reply;
}
} // namespace dingosdk::filmer
