#pragma once
// The Filmer: skate. filmed the way a skater with a VX1000 films a line.
//
//   follow  an AI filmer rides with a skater (you, or someone in the session) and films them, with
//           a filmer's habits: late corrections, a loose frame, a hand that is never quite still,
//           but never losing the trick.
//   film    you are the filmer: the left stick rides your skater, the right stick points the camera
//           (the game no longer sees it, so no flicks while you film). The assist keeps a friend in
//           frame; the stick nudges the framing.
//
// Either way the overlay can make the picture look like it came off a VX1000 with a Century MK1
// fisheye (filmer_look.cpp), and a hand slapped over the lens marks the end of a clip.
//
// The game thread owns the Filmer (filmer.cpp). Every change is a `filmer ...` console command, so the
// menu page only queues commands and reads the snapshot below.
#include "filmer_rig.h"
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace dingosdk::filmer {
// What the overlay does to the picture while the Filmer has the camera. Everything can be switched off.
struct Look {
    bool enabled = true;     // the whole look
    bool fisheye = true;     // the MK1's bend (only with the fisheye lens)
    float bend = 1.0f;       // how much of it, 0..1
    bool bars = true;        // 4:3, black at the sides
    bool hood = true;        // the dark round edge of the fisheye's hood in the corners
    bool grade = true;       // the VX's colour: punchy, warm, a little crushed
    bool soft = true;        // standard definition: soft, with the camcorder's sharpening halo
    bool scanlines = true;   // interlaced fields
    bool grain = true;       // tape noise
    bool date = false;       // a date stamp in the corner
    bool viewfinder = false; // REC, the tape counter and the battery, as in the viewfinder
};

struct Settings {
    Mode mode = Mode::off;
    RigSettings rig;
    Look look;
    bool hide_hud = true;   // hide the game's interface while filming
    bool take_stick = true; // film mode: keep the right stick from the game
    // follow: whom the AI films (0: you). film: whom the assist frames (0: whoever is closest ahead).
    std::uint64_t subject{}, target{};
};

struct PlayerRow {
    std::uint64_t id{};
    std::string name;
    float distance{};
};

// One clip: from the camera rolling (or the last slap) to a slap.
struct Clip {
    int number{};
    double seconds{};      // how long it ran
    std::string ended_at;  // local time, for finding it in a recording
};

struct View {
    std::uint64_t revision{};
    Settings settings;
    bool active{};            // the Filmer has the camera this moment
    std::string status;       // what it is doing, or why not
    std::string last;         // what the last command answered
    std::string subject_name; // who is being filmed
    std::vector<PlayerRow> players;
    float auto_angle{};       // auto: degrees round the skater
    // The overlay's clock (now()): when the camera started rolling this clip and when the lens was
    // last slapped.
    double rolling_since{}, slap_at{-100.0};
    std::uint64_t slap_serial{};
    int clip{1};
    std::vector<Clip> clips; // the latest few, newest last
};

// Thread-safe snapshot (filmer_view.cpp, part of the overlay).
std::shared_ptr<const View> view() noexcept;
void publish(std::shared_ptr<const View>) noexcept;
// Seconds on one steady clock shared by the game thread and the overlay.
double now() noexcept;

// Game thread (filmer.cpp).
// Each client tick, after the session placed the other skaters for this frame. `ready` while the
// local skater can be controlled; `camera_phase` while the native camera may be changed.
void tick(std::uintptr_t base, std::uintptr_t client, bool ready, bool camera_phase) noexcept;
// Runs one `filmer` command and returns the line to print. Only from the console dispatcher.
// `hud_hidden`: whether the game's interface is hidden right now (the Filmer hides it while it films
// and only shows it again if it was the one that hid it).
std::string command(std::string_view verb, const std::vector<std::string> &arguments, bool hud_hidden);

// Names, for the command line and the menu.
const char *mode_name(Mode) noexcept;
const char *style_name(Style) noexcept;
const char *lens_name(Lens) noexcept;
} // namespace dingosdk::filmer
