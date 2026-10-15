#include "filmer.h"
#include "Extension/UI/Overlay/skate_menu_internal.h"
#include "Extension/UI/skate_theme.h"
#include <algorithm>
#include <format>
#include <string>

// The FILMER page. Presentation thread: reads the Filmer's snapshot and queues `filmer ...` console
// commands; it never touches the game.
namespace dingosdk::overlay::menu {
namespace {
struct Page {
    // The value under the pointer while a slider is dragged: the snapshot lags a frame or two.
    std::string active;
    float active_value{};
};
Page &page() {
    static auto *value = new Page;
    return *value;
}

void filmer_command(SkateMenu &menu, const CallbacksV3 &callbacks, const std::string &text) {
    send_console(menu, callbacks, "filmer " + text);
}

void filmer_slider(SkateMenu &menu, const CallbacksV3 &callbacks, const char *label, const char *tooltip, const char *key,
            float value, float low, float high, const char *format) {
    auto &p = page();
    field(menu, label, tooltip);
    ImGui::PushID(key);
    float shown = p.active == key ? p.active_value : value;
    if (ImGui::SliderFloat("##value", &shown, low, high, format, ImGuiSliderFlags_AlwaysClamp))
        filmer_command(menu, callbacks, std::format("set {} {:.3f}", key, shown));
    if (ImGui::IsItemActive()) {
        p.active = key;
        p.active_value = shown;
    } else if (p.active == key) {
        p.active.clear();
    }
    ImGui::PopID();
}

void filmer_toggle(SkateMenu &menu, const CallbacksV3 &callbacks, const char *label, const char *hint, const char *key, bool value,
            bool available = true) {
    bool on = value;
    if (toggle_row(menu, label, hint, on, available)) filmer_command(menu, callbacks, std::format("set {} {}", key, on ? 1 : 0));
}

// Whom to film: you or someone in the session (follow), whoever is closest ahead or someone (film).
void people(SkateMenu &menu, const CallbacksV3 &callbacks, const filmer::View &view, bool follow) {
    const auto chosen = follow ? view.settings.subject : view.settings.target;
    const char *verb = follow ? "subject" : "target";
    const char *first = follow ? "Me" : "Whoever is closest ahead";
    ImGui::PushID(verb);
    if (ImGui::Selectable(first, chosen == 0)) filmer_command(menu, callbacks, std::format("{} {}", verb, follow ? "me" : "auto"));
    for (const auto &player : view.players) {
        ImGui::PushID(static_cast<int>(player.id & 0x7fffffff));
        const auto label = std::format("{}   {:.0f} m", player.name, player.distance);
        if (ImGui::Selectable(label.c_str(), chosen == player.id)) filmer_command(menu, callbacks, std::format("{} {}", verb, player.id));
        ImGui::PopID();
    }
    if (view.players.empty())
        note(follow ? "Nobody else is in the session: the AI filmer films you." : "Nobody else is in the session: frame your own shots.");
    ImGui::PopID();
}
} // namespace

void filmer_page(SkateMenu &menu, const Model &, const CallbacksV3 &callbacks) {
    const auto snapshot = filmer::view();
    if (!snapshot) return;
    const auto &v = *snapshot;
    const auto &s = v.settings;
    const auto &r = s.rig;
    ImGui::BeginChild("filmer-page", ImVec2(0, page_body_height(menu)));

    begin_card(menu, "filmer-camera", "CAMERA", "Who holds the VX, and who is in front of it.");
    int mode = static_cast<int>(s.mode);
    if (choice(menu, "mode", mode, {"Off", "AI filmer", "You film"}, true, 0.0f,
               {"The game's own camera.", "An AI filmer rides with a skater and films them. You skate as normal.",
                "You are the filmer: left stick rides, right stick aims, click it to slap the lens."}))
        filmer_command(menu, callbacks, std::string("mode ") + filmer::mode_name(static_cast<filmer::Mode>(mode)));
    info(menu, "Status", v.status);
    if (s.mode == filmer::Mode::follow) {
        ImGui::Spacing();
        ImGui::PushFont(menu.bold);
        ImGui::TextUnformatted("FILM");
        ImGui::PopFont();
        people(menu, callbacks, v, true);
    } else if (s.mode == filmer::Mode::film) {
        ImGui::Spacing();
        ImGui::PushFont(menu.bold);
        ImGui::TextUnformatted("THE ASSIST FRAMES");
        ImGui::PopFont();
        people(menu, callbacks, v, false);
    } else {
        note("Put \"Filmer on / off\" on a button in Settings > Binds to start filming without the menu.");
    }
    end_card();

    begin_card(menu, "filmer-rig", "THE FILMER", "How they ride and how good they are.");
    field(menu, "Lens", "Fisheye: a Century MK1 on the VX, ridden close. Long lens: the VX's own, from further back.");
    int lens = r.lens == filmer::Lens::long_lens ? 1 : 0;
    if (choice(menu, "lens", lens, {"Fisheye (MK1)", "Long lens"})) filmer_command(menu, callbacks, lens ? "set lens long" : "set lens fisheye");
    if (s.mode != filmer::Mode::film) {
        field(menu, "Angle", "Where the AI filmer rides. Auto picks angles through a line like a filmer does.");
        int style = static_cast<int>(r.style);
        if (choice(menu, "style", style, {"Auto", "Behind", "Side", "Ahead"}))
            filmer_command(menu, callbacks, std::string("set style ") + filmer::style_name(static_cast<filmer::Style>(style)));
        field(menu, "Side", "Which side of you the AI filmer keeps to. Auto: wherever they happen to be.");
        int side = static_cast<int>(r.side);
        if (choice(menu, "side", side, {"Auto", "Left", "Right"}))
            filmer_command(menu, callbacks, std::string("set side ") + (side == 1 ? "left" : side == 2 ? "right" : "auto"));
        filmer_slider(menu, callbacks, "Distance", "How close they ride (metres; the long lens rides about 3x further).", "distance", r.distance, 0.6f, 6.0f, "%.1f m");
        filmer_slider(menu, callbacks, "Camera height", "Above the ground. Down by the board (0.15 m) is the classic fisheye angle; about a foot (0.3 m) sees more of the spot.", "height", r.height, 0.08f, 2.5f, "%.2f m");
        filmer_toggle(menu, callbacks, "Lunge", "Fisheye, flatground tricks only: as you pop, the filmer pushes in a little closer and lower, then eases back out as you land.", "lunge", r.lunge, r.lens == filmer::Lens::fisheye);
        filmer_toggle(menu, callbacks, "Stop at drops", "The filmer only rides where you rolled: at a set or a gap you jumped they stop at the edge, film you land and ride away, then catch up.", "drops", r.stop_at_drops);
        filmer_toggle(menu, callbacks, "Stick clicks", "While the AI films: click the left stick to have them stop and film from where they are (again to ride on), the right stick to slap the lens. The game does not get those clicks while this is on.", "clicks", s.stick_clicks);
        if (s.mode == filmer::Mode::follow && primary_button(menu, v.planted ? "RIDE AGAIN" : "STOP AND FILM FROM HERE", v.active))
            filmer_command(menu, callbacks, "plant");
    }
    filmer_slider(menu, callbacks, "Tilt", "How the skater sits in the frame: 0 the board near the middle, 1 the board low with more of the skater above. The board is always kept in.", "tilt", r.tilt, 0.0f, 1.0f, "%.2f");
    filmer_slider(menu, callbacks, "Human", "0: steady as a robot. 1: a sketchy filmer: shaky, a loose frame, late corrections.", "human", r.human, 0.0f, 1.0f, "%.2f");
    filmer_slider(menu, callbacks, "Reaction", "0: snaps onto the action. 1: lazy and late (but never loses the trick).", "reaction", r.reaction, 0.0f, 1.0f, "%.2f");
    if (s.mode == filmer::Mode::film) {
        filmer_toggle(menu, callbacks, "Aim assist", "Keep pointing at the skater you film; the stick shifts the framing and it settles back.", "assist", r.assist);
        filmer_slider(menu, callbacks, "Stick speed", "How fast the right stick turns the camera.", "sensitivity", r.sensitivity, 0.2f, 3.0f, "x %.2f");
        filmer_toggle(menu, callbacks, "Invert up / down", "Push up to point the camera down.", "invert", r.invert_y);
        filmer_toggle(menu, callbacks, "Recenter", "Without the assist: let go and the camera drifts back to where you ride.", "recenter", r.recenter, !r.assist);
        filmer_slider(menu, callbacks, "Hold height", "Where you hold the camera above the ground: low by the board and out at arm's length, clear of your body.", "hold", r.hold_height, 0.05f, 1.8f, "%.2f m");
        filmer_toggle(menu, callbacks, "Take the right stick", "Keep the right stick (and its click) from the game while you film. Off: it also flicks tricks.", "stick", s.take_stick);
        filmer_toggle(menu, callbacks, "Cruise control", "Filming a friend, you ride at the speed that keeps you the gap behind them: you steer and aim, your push and brake still work. Off in a session whose host has turned boosts off. Bindable.", "cruise", r.cruise);
        filmer_slider(menu, callbacks, "Cruise gap", "How far behind the skater cruise control holds you.", "gap", r.cruise_gap, 1.0f, 12.0f, "%.1f m");
        if (r.cruise && !v.cruise_note.empty()) note(v.cruise_note.c_str());
    }
    filmer_toggle(menu, callbacks, "Mouse look", "While the camera is held still (the AI filmer stopped, or you standing still filming), the mouse turns it, and with the long lens the wheel zooms.", "mouse", r.mouse_look);
    filmer_slider(menu, callbacks, "Mouse speed", "How fast the mouse turns the camera.", "mousesens", r.mouse_sensitivity, 0.1f, 5.0f, "x %.2f");
    if (r.lens == filmer::Lens::long_lens) {
        field(menu, "Zoom", "Long lens, while the camera is held still: the wheel, or the zoom binds. Riding again, it eases back out.");
        const bool can = v.active && v.stationary;
        int pick = -1; // neither shown as chosen: two buttons
        if (choice(menu, "zoom", pick, {"Zoom out", "Zoom in"}, can) && pick >= 0) filmer_command(menu, callbacks, pick ? "zoom in" : "zoom out");
        note(std::format("{:.1f}x{}", v.zoom, can ? "" : "  (hold the camera still to zoom)").c_str());
    }
    end_card();

    begin_card(menu, "filmer-look", "VX1000 LOOK", "What the tape looks like. Each part can be switched off.");
    filmer_toggle(menu, callbacks, "VX look", "The whole look, drawn over the game while the Filmer films.", "look", s.look.enabled);
    const bool look = s.look.enabled;
    filmer_toggle(menu, callbacks, "Fisheye bend", "The MK1's curve. Only with the fisheye lens.", "fisheye", s.look.fisheye, look && r.lens == filmer::Lens::fisheye);
    if (look && s.look.fisheye && r.lens == filmer::Lens::fisheye)
        filmer_slider(menu, callbacks, "Bend", "How much of the curve.", "bend", s.look.bend, 0.0f, 1.0f, "%.2f");
    filmer_toggle(menu, callbacks, "4:3", "Black at the sides, like a TV in 1996.", "bars", s.look.bars, look);
    filmer_toggle(menu, callbacks, "Hood edge", "The fisheye's dark round edge in the corners.", "hood", s.look.hood, look);
    filmer_toggle(menu, callbacks, "VX colour", "Punchy, warm, a little crushed.", "grade", s.look.grade, look);
    filmer_toggle(menu, callbacks, "Standard definition", "Soft, with the camcorder's sharpening halo and colour bleed.", "soft", s.look.soft, look);
    filmer_toggle(menu, callbacks, "Interlacing", "Every other line a touch darker.", "scanlines", s.look.scanlines, look);
    filmer_toggle(menu, callbacks, "Tape grain", "Fine noise.", "grain", s.look.grain, look);
    filmer_toggle(menu, callbacks, "Date stamp", "The date and time in the corner.", "date", s.look.date, look);
    filmer_toggle(menu, callbacks, "Viewfinder", "REC, the tape counter and the battery, as through the viewfinder.", "viewfinder", s.look.viewfinder, look);
    filmer_toggle(menu, callbacks, "Auto-iris", "The camera's iris lags the light: into the sun the picture blows out for a moment, into shade it goes murky, then each settles.", "iris", s.look.iris, look);
    filmer_toggle(menu, callbacks, "Over any camera", "Draw the look even when the Filmer is not filming: over the replay editor's cameras, Freecam, the game's own camera. (Clips the replay editor exports are made without it: record the screen.)", "anywhere", s.look.anywhere, look);
    filmer_toggle(menu, callbacks, "Hide the game's HUD", "While the Filmer films (shown again when it stops, if it hid it).", "hidehud", s.hide_hud);
    note("HDR displays get the bend, 4:3, hood and interlacing; the colour and grain are for SDR.");
    end_card();

    begin_card(menu, "filmer-tape", "TAPE", "How worn the tape is. Each flaw can be switched off.");
    const bool tape = s.look.enabled;
    filmer_slider(menu, callbacks, "Tape age", "0: a fresh tape. 1: one that has been dubbed and played to death: more dropouts and a worse bottom edge.", "age", s.look.age, 0.0f, 1.0f, "%.2f");
    filmer_toggle(menu, callbacks, "Dropouts", "Now and then a short white streak where the tape lost its coating (more, the older it is).", "dropouts", s.look.dropouts, tape);
    filmer_toggle(menu, callbacks, "Head switching", "The torn, noisy lines along the very bottom of the picture.", "headswitch", s.look.head_switching, tape);
    filmer_toggle(menu, callbacks, "Tracking at each clip", "As a clip starts recording (the camera rolling, or after a slap), a band of tearing rolls up the picture.", "tracking", s.look.tracking, tape);
    filmer_toggle(menu, callbacks, "Red smear", "Reds trail to the right, as on a composite tape.", "smear", s.look.smear, tape);
    end_card();

    begin_card(menu, "filmer-clips", "CLIPS", "Slap the lens to end a clip.");
    info(menu, "Rolling", std::format("Clip {}", v.clip));
    if (primary_button(menu, "SLAP THE LENS", s.mode != filmer::Mode::off)) filmer_command(menu, callbacks, "slap");
    note("Click the right stick (filming, or following with Stick clicks on), or put \"Filmer: slap the lens\" on a button in Settings > Binds.");
    for (auto it = v.clips.rbegin(); it != v.clips.rend() && it - v.clips.rbegin() < 6; ++it)
        info(menu, std::format("Clip {}", it->number).c_str(), std::format("{:.1f} s, ended {}", it->seconds, it->ended_at));
    if (!v.clips.empty()) note("Every clip is listed with its time in %LOCALAPPDATA%\\ReSkate\\filmer\\clips.csv, for finding them in a recording.");
    end_card();

    ImGui::EndChild();
}
} // namespace dingosdk::overlay::menu
