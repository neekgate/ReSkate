#pragma once
// What the drone can put on the pilot's skater while it flies (goggles, a hat): for now, finding
// the items. `droneitems` lists the installed cosmetics whose names contain a word, and the local
// skater's outfit slots, so the right item can be chosen. Game thread (console commands).
#include <string>
#include <string_view>
#include <vector>

namespace dingosdk::drone::outfit {
// Installed cosmetic names containing `word` (any case), at most `limit` of them; `total` is how many matched.
std::vector<std::string> find_items(std::string_view word, std::size_t limit, std::size_t& total);
// The local skater's outfit slots, one line each: index, slot name and what it holds now.
std::vector<std::string> slots();
}
