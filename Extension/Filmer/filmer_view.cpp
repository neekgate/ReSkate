#include "filmer.h"
#include <chrono>
#include <mutex>

// The snapshot the menu page and the look read. Kept apart from filmer.cpp so the overlay library
// links without the game-facing half.
namespace dingosdk::filmer {
namespace {
struct Feed {
    std::mutex mutex;
    std::shared_ptr<const View> view = std::make_shared<const View>();
};
Feed &feed() {
    static auto *value = new Feed;
    return *value;
}
} // namespace

std::shared_ptr<const View> view() noexcept {
    auto &f = feed();
    std::lock_guard lock(f.mutex);
    return f.view;
}

void publish(std::shared_ptr<const View> next) noexcept {
    if (!next) return;
    auto &f = feed();
    std::lock_guard lock(f.mutex);
    f.view = std::move(next);
}

double now() noexcept {
    static const auto start = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

const char *mode_name(Mode mode) noexcept {
    switch (mode) {
    case Mode::follow: return "follow";
    case Mode::film: return "film";
    case Mode::tripod: return "tripod";
    case Mode::off: break;
    }
    return "off";
}

const char *style_name(Style style) noexcept {
    switch (style) {
    case Style::behind: return "behind";
    case Style::side: return "side";
    case Style::ahead: return "ahead";
    case Style::automatic: break;
    }
    return "auto";
}

const char *lens_name(Lens lens) noexcept { return lens == Lens::long_lens ? "long" : "fisheye"; }

const char *slap_button_name(SlapButton button) noexcept {
    switch (button) {
    case SlapButton::right_stick: return "rs";
    case SlapButton::right_bumper: return "rb";
    case SlapButton::left_bumper: return "lb";
    case SlapButton::dpad_down: return "down";
    case SlapButton::dpad_up: return "up";
    case SlapButton::back: return "back";
    case SlapButton::none: break;
    }
    return "none";
}
} // namespace dingosdk::filmer
