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
        note(follow ? "Nobody else is in the session: the Filmer films you." : "Nobody else is in the session: frame your own shots.");
    ImGui::PopID();
}
} // namespace

void filmer_page(SkateMenu &menu, const Model &, const CallbacksV3 &callbacks) {
    const auto snapshot = filmer::view();
    if (!snapshot) return;
    const auto &v = *snapshot;
    const auto &s = v.settings;
    const auto &r = s.rig;
    category_tabs(menu, menu.filmer_tab, {"CAMERA", "IN HAND", "LOOK", "CLIPS", "CONTROLS", "DEBUG"}, "filmer-tabs");
    ImGui::PushID(menu.filmer_tab);
    ImGui::BeginChild("filmer-page", ImVec2(0, page_body_height(menu)), ImGuiChildFlags_NavFlattened);
    switch (menu.filmer_tab) {
    case 0: {
    begin_card(menu, "filmer-camera", "CAMERA", "Who holds the VX, and who is in front of it.");
    int mode = static_cast<int>(s.mode);
    if (choice(menu, "mode", mode, {"Off", "Filmer", "You film", "Tripod"}, true, 0.0f,
               {"The game's own camera.", "Another skater rides with you on their board and films you (or someone in the session): behind you or out to your side, never in front. You skate as normal.",
                "You are the filmer: left stick rides, right stick aims.",
                "A camera set down on a tripod: it holds its framing, and pans with the skater once they ride into it."}))
        filmer_command(menu, callbacks, std::string("mode ") + filmer::mode_name(static_cast<filmer::Mode>(mode)));
    info(menu, "Status", v.status);
    if (s.mode == filmer::Mode::follow || s.mode == filmer::Mode::tripod) {
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
        note(s.camera_cycle ? "Click the right stick to cycle the cameras: high, low, first person (when it is in the cycle), the Filmer. See CONTROLS."
                            : "Put \"Filmer on / off\" on a button in Settings > Binds to start filming without the menu.");
    }
    end_card();

    begin_card(menu, "filmer-rig", "THE FILMER", "How they ride and how good they are.");
    field(menu, "Lens", "Fisheye: a Century MK1 on the VX, ridden close. Long lens: the VX's own, from further back.");
    int lens = r.lens == filmer::Lens::long_lens ? 1 : 0;
    if (choice(menu, "lens", lens, {"Fisheye (MK1)", "Long lens"})) filmer_command(menu, callbacks, lens ? "set lens long" : "set lens fisheye");
    if (s.mode == filmer::Mode::tripod) {
        note("Set it down where the camera is now (stop the Filmer where you want it first), or where you stand. "
             "Turn it with the mouse while nobody is in frame; with the long lens the wheel zooms. "
             "Clicking the left stick sets it down where you stand.");
        if (primary_button(menu, "SET IT DOWN WHERE THE CAMERA IS", v.active)) filmer_command(menu, callbacks, "tripod");
        if (primary_button(menu, "SET IT DOWN WHERE I STAND")) filmer_command(menu, callbacks, "tripod here");
        filmer_toggle(menu, callbacks, "Tracking", "On: it holds its framing until the skater rides into it, then pans with them. Off: a locked-off shot that never moves. Bindable.", "track", r.tripod_track);
        filmer_slider(menu, callbacks, "Tripod height", "Set down where you stand: how high the lens is above the ground.", "tripodheight", r.tripod_height, 0.1f, 2.5f, "%.2f m");
    } else if (s.mode != filmer::Mode::film) {
        field(menu, "Angles", "Where the Filmer rides. Switch on the angles you want: one is held; with both, they move between them through a line as a filmer does. They find their way: the side that is clear where you are heading, or straight behind you, never in front.");
        const std::uint8_t angles = r.angles;
        const auto angle_row = [&](const char *label, const char *hint, const char *name, std::uint8_t bit) {
            bool on = (angles & bit) != 0;
            // The last one on stays on: there is always an angle.
            if (toggle_row(menu, label, hint, on, !(on && angles == bit))) filmer_command(menu, callbacks, std::string("angle toggle ") + name);
        };
        angle_row("Behind", "A little off to one side, behind you: the classic follow.", "behind", filmer::angle_behind);
        angle_row("Side", "Out wide, alongside you.", "side", filmer::angle_side);
        field(menu, "Side", "Which side of you the Filmer keeps to when it is clear (they take the other side, or straight behind, to get round what is in the way). Auto: wherever they happen to be.");
        int side = static_cast<int>(r.side);
        if (choice(menu, "side", side, {"Auto", "Left", "Right"}))
            filmer_command(menu, callbacks, std::string("set side ") + (side == 1 ? "left" : side == 2 ? "right" : "auto"));
        filmer_slider(menu, callbacks, "Distance", "How close they ride (metres; the long lens rides about 3x further).", "distance", r.distance, 0.6f, 6.0f, "%.1f m");
        filmer_slider(menu, callbacks, "Camera height", "Above the ground. Down by the board (0.15 m) is the classic fisheye angle; about a foot (0.3 m) sees more of the spot.", "height", r.height, 0.08f, 2.5f, "%.2f m");
        filmer_slider(menu, callbacks, "Distance variety", "Through a line the filmer drifts closer and further, slowly. 0: always the distance above; 1: about half of it either way.", "distvariety", r.distance_variety, 0.0f, 1.0f, "%.2f");
        filmer_slider(menu, callbacks, "Height variety", "Through a line the camera goes lower and higher, slowly (a filmer crouching, then standing a little). 0: always the height above; 1: a foot or so either way.", "heightvariety", r.height_variety, 0.0f, 1.0f, "%.2f");
        filmer_toggle(menu, callbacks, "Lunge", "Fisheye, flatground tricks only: as you pop, the filmer pushes in a little closer and lower, then eases back out as you land.", "lunge", r.lunge, r.lens == filmer::Lens::fisheye);
        filmer_toggle(menu, callbacks, "Stop at drops", "Ollie (or flip) off something too high for a filmer to huck after you (a big set, a gap down) and they ease off as you near it, stop at the top and film you land and ride away; push, step off the board or click the left stick and they are back behind you. Rolling down a hill or a bank, they follow.", "drops", r.stop_at_drops);
        filmer_toggle(menu, callbacks, "Slam zoom", "When you slam the filmer stops, still filming you; if you are small in the frame, the lens slowly zooms in until you fill it.", "slamzoom", r.slam_zoom);
        filmer_toggle(menu, callbacks, "Zoom when they stop", "Now and then (about 4 times in 10) when the filmer stops by themselves, a slight zoom onto you.", "stopzoom", r.stop_zoom);
        field(menu, "Stop", "How the filmer stops (the left stick, or at a drop). Instant: on the spot, holding the framing. Stair: rolls to a halt short of the first step, rises a little and pans down to take in the whole set or rail.");
        int stop = static_cast<int>(r.stop_style);
        if (choice(menu, "stop", stop, {"Instant", "Stair"})) filmer_command(menu, callbacks, stop ? "set stop stair" : "set stop instant");
        if (r.stop_style == filmer::StopStyle::stair)
            filmer_slider(menu, callbacks, "Stair lift", "Stair stop: how much the camera rises once stopped.", "lift", r.stop_lift, 0.0f, 1.5f, "%.2f m");
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
        filmer_toggle(menu, callbacks, "Cruise control", "Filming a friend, you ride at the speed that keeps you the gap behind them: you steer and aim, your push and brake still work. Off in a session whose host has turned boosts off. Bindable.", "cruise", r.cruise);
        filmer_slider(menu, callbacks, "Cruise gap", "How far behind the skater cruise control holds you.", "gap", r.cruise_gap, 1.0f, 12.0f, "%.1f m");
        if (r.cruise && !v.cruise_note.empty()) note(v.cruise_note.c_str());
    }
    filmer_toggle(menu, callbacks, "Mouse look", "While the camera is held still (the Filmer stopped, or you standing still filming), the mouse turns it, and with the long lens the wheel zooms.", "mouse", r.mouse_look);
    filmer_slider(menu, callbacks, "Mouse speed", "How fast the mouse turns the camera.", "mousesens", r.mouse_sensitivity, 0.1f, 5.0f, "x %.2f");
    if (r.lens == filmer::Lens::long_lens) {
        field(menu, "Zoom", "Long lens, while the camera is held still: the wheel, or the zoom binds. Riding again, it eases back out.");
        const bool can = v.active && v.stationary;
        int pick = -1; // neither shown as chosen: two buttons
        if (choice(menu, "zoom", pick, {"Zoom out", "Zoom in"}, can) && pick >= 0) filmer_command(menu, callbacks, pick ? "zoom in" : "zoom out");
        note(std::format("{:.1f}x{}", v.zoom, can ? "" : "  (hold the camera still to zoom)").c_str());
    }
    end_card();

        break;
    }
    case 1: {
    begin_card(menu, "filmer-hand", "CAMERA IN HAND", "The VX in your hand, as the others in the session see you filming.");
    filmer_toggle(menu, callbacks, "Hold the camera out", "You film: your arm holds the camera out where the lens is, low by the board with the fisheye. Others in the session see it in your hand (they need this build too). You look through it, so you never see it yourself.", "holdcam", s.hold_camera);
    filmer_toggle(menu, callbacks, "I wear the camcorder", "The camcorder cosmetic (hand wear, made with ReSkate Studio from the fingerless glove) goes in your hand while you film and comes off after; your hand turns so its lens points where you aim, and no separate model is placed. Others see it in your outfit.", "worn", s.camera_worn);
    filmer_toggle(menu, callbacks, "Two hands", "Your other hand holds the camera too, from the side and underneath, as a filmer cradles it.", "twohands", s.two_hands);
    filmer_toggle(menu, callbacks, "Hide the board on foot", "Filming on foot, the board in your hand (or on the ground) is out of sight, for you and the others; it is back when you stop or ride.", "hideboard", s.hide_board);
    filmer_toggle(menu, callbacks, "Still upper body on foot", "Filming on foot, your back, neck and head hold still: the game's idle fidgets do not show through.", "still", s.still_body);
    filmer_toggle(menu, callbacks, "Crouch while filming", "While the camera is held out your skater bends the knees a little and hunches over it, looking down at it, on the board and off it, feet where they are. The others see it too.", "crouch", s.crouch);
    filmer_slider(menu, callbacks, "Knee bend", "How far your hips go down.", "crouchdepth", s.crouch_depth, 0.0f, 0.5f, "%.2f m");
    filmer_slider(menu, callbacks, "Hunch", "How far your back curls over toward the camera; the head looks down at it.", "hunch", s.hunch_degrees, 0.0f, 60.0f, "%.0f deg");
    filmer_toggle(menu, callbacks, "Show other filmers' cameras", "A camera in the hand of anyone in the session who is filming.", "cameras", s.show_cameras);
    filmer_toggle(menu, callbacks, "Stand-in when there is no model", "The camera's own model has to be built with ReSkate Studio (assets/models/vx1000). Until it is, show a traffic cone in the hand instead of nothing.", "standin", s.stand_in);
    note("Camera model: Camcorder Video Camera by James.Hasegawa (CC BY 4.0), rescaled for the game.");
    end_card();

        break;
    }
    case 2: {
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

        break;
    }
    case 3: {
    begin_card(menu, "filmer-clips", "CLIPS", "Slap the lens to end a clip.");
    info(menu, "Rolling", std::format("Clip {}", v.clip));
    if (primary_button(menu, "SLAP THE LENS", s.mode != filmer::Mode::off)) filmer_command(menu, callbacks, "slap");
    note(s.slap_button == filmer::SlapButton::none ? "Put \"Filmer: slap the lens\" on a button in Settings > Binds, or choose a slap button in CONTROLS."
                                                   : "The slap button (CONTROLS), or put \"Filmer: slap the lens\" on a button in Settings > Binds.");
    for (auto it = v.clips.rbegin(); it != v.clips.rend() && it - v.clips.rbegin() < 6; ++it)
        info(menu, std::format("Clip {}", it->number).c_str(), std::format("{:.1f} s, ended {}", it->seconds, it->ended_at));
    if (!v.clips.empty()) note("Every clip is listed with its time in %LOCALAPPDATA%\\ReSkate\\filmer\\clips.csv, for finding them in a recording.");
    end_card();

        break;
    }
    case 4: {
    begin_card(menu, "filmer-controls", "CONTROLLER", "The Filmer's buttons. Each is taken from the game only while the Filmer has the camera.");
    filmer_toggle(menu, callbacks, "Filmer in the camera cycle", "The game's camera height button (click the right stick) goes high, low, ReSkate's first person (when it is in the cycle: Skater settings), then the Filmer filming you, and back to high.", "cycle", s.camera_cycle);
    field(menu, "Slap the lens", "The button that slaps the lens to end a clip, while the Filmer has the camera. The right stick's click is the camera cycle's while the Filmer follows you with the cycle on, so it only slaps when you film or with the cycle off.");
    {
        int slap = static_cast<int>(s.slap_button);
        if (choice(menu, "slap", slap, {"Right stick click", "RB (right bumper)", "D-pad down", "D-pad up", "View / Back", "None"}))
            filmer_command(menu, callbacks, std::string("set slapbutton ") + filmer::slap_button_name(static_cast<filmer::SlapButton>(slap)));
    }
    filmer_toggle(menu, callbacks, "Side buttons (D-pad left / right)", "While the Filmer follows you: D-pad left sends them to your left, right to your right, and they keep to it, going round whatever is coming up on that side (so you choose the way round a bin or a wall). Press the same side again and they pick their own side again. The game does not get D-pad left and right while this is on.", "sidebuttons", s.side_buttons);
    if (s.side_buttons) note(r.side_hard ? (r.side == filmer::Side::left ? "Keeping to your left (D-pad left again: their choice)." : "Keeping to your right (D-pad right again: their choice).") : "Picking their own side.");
    filmer_toggle(menu, callbacks, "Left stick click", "While the Filmer follows: click the left stick to have them stop and film from where they are (again to ride on). On the tripod: sets it down where you stand. The game does not get that click while this is on.", "clicks", s.stick_clicks);
    filmer_toggle(menu, callbacks, "Take the right stick (you film)", "Keep the right stick (and its click) from the game while you film. Off: it also flicks tricks.", "stick", s.take_stick);
    note("Every Filmer action (on / off, slap, stop and film, next angle, zoom) can also go on any button in Settings > Binds.");
    end_card();
        break;
    }
    default: {
    begin_card(menu, "filmer-debug", "DEBUG", "For checking the camera in hand and the Filmer.");
    info(menu, "Status", v.status);
    if (s.mode == filmer::Mode::follow) {
        info(menu, "Filmer", v.planted ? std::string("stopped by you") : v.doing.empty() ? std::string("riding with you") : v.doing);
        info(menu, "Angle", std::format("{:.0f} deg round you", v.auto_angle));
    }
    if (primary_button(menu, v.holdtest ? "STOP THE HOLD TEST" : "HOLD TEST: SEE YOUR SKATER HOLD IT"))
        filmer_command(menu, callbacks, std::string("holdtest ") + (v.holdtest ? "off" : "on"));
    note("Hold test: with the game's own camera (or the Filmer's), your skater holds the camera out in front at arm's length so you can see it. A stand-in cone shows if the model is not built.");
    if (!v.held_status.empty()) note(v.held_status.c_str());
    if (primary_button(menu, "TRACE 30 SECONDS", s.mode != filmer::Mode::off)) filmer_command(menu, callbacks, "trace");
    note("Writes what the Filmer sees each tick to %LOCALAPPDATA%\\ReSkate\\filmer\\trace.csv, to send for tuning.");
    end_card();
        break;
    }
    }
    ImGui::EndChild();
    ImGui::PopID();
}
} // namespace dingosdk::overlay::menu
