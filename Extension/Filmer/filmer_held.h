#pragma once
// The camera in a filmer's hand, in the game: your arm holding it out while you film, and the camera
// model in your hand and in every other filmer's (filmer_hold.h has the maths, filmer_beacon.h the
// packet that says who is filming).
//
// Your arm: as your skater's pose is handed to the renderer, its upper arm and forearm are bent so the hand holds
// the camera where the Filmer's lens is (out of reach, as near as the arm goes). Your pose goes to the
// others with the arm in it, so they see it held out with no more than the packet's flag. You never see
// your own camera while you film (you look through it): `filmer holdtest` shows it on your skater with
// the game's own camera, to see how it looks.
//
// Everyone's camera: a model placed every frame at the palm, turned the way they aim.
#include "filmer_hold.h"
#include <cstdint>
#include <string>

namespace dingosdk::multiplayer {
struct Pose;
}

namespace dingosdk::filmer::held {
struct Request {
    bool hold{};             // the local arm holds the camera out this frame
    hold::Hand hand = hold::Hand::right;
    // Where the lens should be: in the world, or (relative) metres ahead of the shoulder along
    // `facing` and up from it.
    bool relative{};
    hold::Vec3 lens{};
    hold::Vec3 facing{0, 0, 1};
    hold::Quat aim{0, 0, 0, 1};
    bool show_own{};         // the camera model on your own skater too
    bool worn{};             // the camcorder is worn as hand wear: the hand turns too, and no model is placed
    float crouch{};          // how far the hips go down while the camera is held (metres; 0: standing)
    float hunch{};           // how far the back curls over toward the camera (radians), the head looking down at it
    bool hide_own{};         // you look through the camera: your camera arm is not drawn on your own screen
};
// Game thread, every Filmer tick.
void set_local(const Request &request) noexcept;

// Client thread, each frame the session shows another player: their pose as shown, for the camera
// in their hand when they film. Cheap when they are not filming.
void note_remote_pose(std::uint64_t id, const multiplayer::Pose &pose) noexcept;

struct Options {
    bool show_others = true;  // the cameras in other filmers' hands
    bool stand_in = false;    // a stand-in prop when the camera's model is not loaded
    std::string blueprint;    // the camera's model; empty: the default
};
// Client thread, every frame while in a level (`in_level`) and after the Filmer's tick: hooks the
// animation once, and places the camera models.
void tick(std::uintptr_t base, std::uintptr_t client, bool in_level, const Options &options) noexcept;
// One line for `filmer status` and the menu.
std::string status();
} // namespace dingosdk::filmer::held
