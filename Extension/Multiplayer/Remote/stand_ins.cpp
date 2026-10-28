#include "stand_ins.h"
#include <atomic>
#include <mutex>

namespace dingosdk::multiplayer::stand_ins {
namespace {
std::mutex mutex;
std::shared_ptr<const Policy> policy;
std::vector<Shown> recent; // newest first, under mutex
std::atomic<std::uint64_t> changes{1};
} // namespace
std::shared_ptr<const Policy> current() {
    std::lock_guard lock(mutex);
    return policy;
}
std::uint64_t generation() { return changes.load(std::memory_order_acquire); }
std::string set(Policy next) {
    std::lock_guard lock(mutex);
    if (policy && policy->owner != next.owner) return "\"" + policy->owner + "\" chooses the stand-ins already";
    if (next.items.size() > max_items) next.items.resize(max_items);
    if (next.exclude.size() > max_items) next.exclude.resize(max_items);
    for (auto &rule : next.exclude) rule = lower(rule);
    // What is excluded now is not listed as shown any more: the players wearing it are dressed again.
    std::erase_if(recent, [&](const Shown &s) { return excluded(next, s.item); });
    policy = std::make_shared<const Policy>(std::move(next));
    changes.fetch_add(1, std::memory_order_acq_rel);
    return {};
}
void clear(std::string_view owner) {
    std::lock_guard lock(mutex);
    if (!policy || policy->owner != owner) return;
    policy.reset();
    recent.clear();
    changes.fetch_add(1, std::memory_order_acq_rel);
}
void note_shown(std::string item, std::string replacing) {
    if (item.empty()) return;
    std::lock_guard lock(mutex);
    if (!policy) return;
    std::erase_if(recent, [&](const Shown &s) { return s.item == item; });
    recent.insert(recent.begin(), Shown{std::move(item), std::move(replacing)});
    if (recent.size() > max_shown) recent.resize(max_shown);
}
std::vector<Shown> shown() {
    std::lock_guard lock(mutex);
    return recent;
}
} // namespace dingosdk::multiplayer::stand_ins
