#include "filmer.h"
#include "Engine/Core/Json/json.h"
#include "Engine/Core/Log/logging.h"
#include "Extension/Console/commands.h"
#include "Extension/Multiplayer/Hud/follow_camera.h"
#include "Extension/Multiplayer/Hud/native_party.h"
#include "Extension/Multiplayer/Net/protocol.h"
#include "Extension/Skater/client_source_spawn.h"
#include "Extension/Trainer/trainer.h"
#include "Extension/UI/Overlay/overlay.h"
#include <windows.h>
#include <shlobj.h>
#include <algorithm>
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
constexpr std::uint16_t right_thumb = 0x0080;  // XINPUT_GAMEPAD_RIGHT_THUMB: the stick click, a slap while filming
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
        r.lens = o.value("lens", std::string("fisheye")) == "long" ? Lens::long_lens : Lens::fisheye;
        r.distance = clamped(o, "distance", d.rig.distance, 0.8f, 6.0f);
        r.height = clamped(o, "height", d.rig.height, 0.2f, 2.5f);
        r.human = clamped(o, "human", d.rig.human, 0.0f, 1.0f);
        r.reaction = clamped(o, "reaction", d.rig.reaction, 0.0f, 1.0f);
        r.assist = flag(o, "assist", d.rig.assist);
        r.sensitivity = clamped(o, "sensitivity", d.rig.sensitivity, 0.2f, 3.0f);
        r.invert_y = flag(o, "invert", d.rig.invert_y);
        r.recenter = flag(o, "recenter", d.rig.recenter);
        r.hold_height = clamped(o, "hold", d.rig.hold_height, 0.3f, 1.8f);
        s.settings.hide_hud = flag(o, "hide_hud", d.hide_hud);
        s.settings.take_stick = flag(o, "take_stick", d.take_stick);
        s.last_mode = o.value("last_mode", std::string("follow")) == "film" ? Mode::film : Mode::follow;
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
        json["schema"] = 1;
        json["style"] = style_name(r.style);
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

void refresh_players(State &s, const Subject &self) {
    s.others = multiplayer::filmable_players();
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
    s.rolling_since = t + 0.6; // the next clip starts once the hand is off the lens
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

// The right stick and its click are the camera's while you film; nothing is taken otherwise.
void update_filter(State &s) {
    const bool film = s.settings.mode == Mode::film && s.held && s.settings.take_stick;
    take_right_stick.store(film, std::memory_order_release);
    masked_buttons.store(film ? right_thumb : std::uint16_t{0}, std::memory_order_release);
    const bool wanted = s.settings.mode != Mode::off;
    if (wanted != s.filter_installed) {
        DingoSDKOverlaySetGamepadFilterSlot(filter_slot, wanted ? &filter_pad : nullptr);
        s.filter_installed = wanted;
    }
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
    // The stick's click slaps the lens while you film.
    const bool pressed = (pad.buttons & right_thumb) != 0, was = (s.previous_buttons & right_thumb) != 0;
    s.previous_buttons = pad.buttons;
    if (pressed && !was && s.settings.mode == Mode::film && s.held) s.last = slap(s);
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

void tick(std::uintptr_t base, std::uintptr_t client, bool ready, bool camera_phase) noexcept {
    try {
        auto &s = state();
        if (!s.loaded) {
            load_store();
            s.view_due = true;
        }
        const double t = now();
        const float dt = static_cast<float>(std::clamp(t - s.last_time, 0.0, 0.1));
        s.last_time = t;
        const auto ticks = GetTickCount64();
        const Subject self = local_subject();
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
            set_status(s, "Waiting for your skater.");
        } else {
            Frame frame;
            std::string name;
            if (s.settings.mode == Mode::follow) {
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
                if (subject.valid) frame = s.rig.follow(t, dt, subject, s.settings.rig);
            } else {
                const multiplayer::FilmablePlayer *target{};
                if (s.settings.target) target = find_player(s, s.settings.target);
                else target = auto_target(s, self);
                const Subject target_subject = target ? remote_subject(*target) : Subject{};
                name = target ? target->name : "your own framing";
                const StickInput stick = read_stick(s);
                frame = s.rig.film(t, dt, self, target ? &target_subject : nullptr, stick, s.settings.rig);
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
                    if (!s.held) {
                        s.held = true;
                        s.view_due = true;
                        s.rolling_since = t;
                        say(logging::Level::info, std::format("Filmer: rolling ({}).", mode_name(s.settings.mode)));
                    }
                    set_status(s, std::string(s.settings.mode == Mode::follow ? "Filming " : "You film ") + name + ".");
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
        // The look's clock runs on even when nothing changed, so the snapshot is not republished every
        // tick: only when something did, and a few times a second for the menu's distances.
        if (s.view_due || ticks >= s.next_publish) publish_view(s);
        if (s.save_due && ticks >= s.next_save) save_store();
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
        if (v == "auto") r.style = Style::automatic;
        else if (v == "behind") r.style = Style::behind;
        else if (v == "side") r.style = Style::side;
        else if (v == "ahead") r.style = Style::ahead;
        else return "error: style is auto, behind, side or ahead.";
        changed(s);
        return std::string("style = ") + style_name(r.style);
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
    if (key == "distance") return real(r.distance, 0.8f, 6.0f);
    if (key == "height") return real(r.height, 0.2f, 2.5f);
    if (key == "human") return real(r.human, 0.0f, 1.0f);
    if (key == "reaction") return real(r.reaction, 0.0f, 1.0f);
    if (key == "sensitivity") return real(r.sensitivity, 0.2f, 3.0f);
    if (key == "hold") return real(r.hold_height, 0.3f, 1.8f);
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
        reply = set_mode(s, mode, hud_hidden);
    } else if (verb == "mode") {
        const auto wanted = lower(arg(0));
        if (wanted == "off") reply = set_mode(s, Mode::off, hud_hidden);
        else if (wanted == "follow") reply = set_mode(s, Mode::follow, hud_hidden);
        else if (wanted == "film") reply = set_mode(s, Mode::film, hud_hidden);
        else reply = "error: mode is off, follow or film.";
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
