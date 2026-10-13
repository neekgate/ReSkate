#include "drone_outfit.h"
#include "Extension/RoadRash/skater_items.h"
#include "Extension/Skater/no_bail.h"
#include <format>
#include <stdexcept>

namespace dingosdk::drone::outfit {
namespace {
using road_rash::SkaterItems;
NoBailSkater local_skater() {
    NoBailSkater skater;
    if (!no_bail_skater(skater)) throw std::runtime_error("No local skater right now (load into a level first).");
    return skater;
}
}

std::vector<std::string> find_items(std::string_view word, std::size_t limit, std::size_t& total) {
    total = 0;
    const auto skater = local_skater();
    const SkaterItems m{road_rash::readable, skater.base};
    const auto wanted = road_rash::lower(word);
    const auto manager = m.ptr(m.base, addr::engine::cosmetics_manager);
    m.check(m.ptr(manager) == m.base + addr::engine::cosmetics_manager_vtable && m.get<std::uint8_t>(manager, 0xa8) == 1,
            "The cosmetic catalog is not ready.");
    const auto buckets = m.ptr(manager, 0x30);
    const auto size = m.get<std::uint32_t>(manager, 0x38), count = m.get<std::uint32_t>(manager, 0x3c);
    m.check(size && size <= 16384 && count && count <= 8192, "The cosmetic catalog exceeds its bounds.");
    std::vector<std::string> out;
    std::uint32_t visited{};
    for (std::uint32_t bucket = 0; bucket < size && visited < count; ++bucket) {
        auto node = m.ptr(buckets, bucket * 8ULL);
        for (std::uint32_t chain = 0; node && chain < count && visited < count; ++chain, ++visited, node = m.ptr(node, 0x10)) {
            const auto asset = m.ptr(node, 8) & ~std::uintptr_t{4};
            if (!asset) continue;
            auto name = m.text(m.ptr(asset, 0x38));
            if (name.empty() || road_rash::lower(name).find(wanted) == std::string::npos) continue;
            ++total;
            if (out.size() < limit) out.push_back(std::move(name));
        }
    }
    return out;
}

std::vector<std::string> slots() {
    const auto skater = local_skater();
    const SkaterItems m{road_rash::readable, skater.base};
    const auto recipe = m.capture(skater.entity);
    const auto resource = m.resource(m.component(skater.entity));
    const auto table = m.ptr(resource, 0x28);
    const auto count = m.count(table, 24, multiplayer::max_cosmetic_slots);
    std::vector<std::string> out;
    for (std::size_t i = 0; i < count; ++i) {
        const auto slot = m.ptr(table, i * 24 + 8) & ~std::uintptr_t{4};
        const auto name = m.text(m.ptr(slot, 0x20));
        const auto& held = i < recipe.items.size() ? recipe.items[i].asset : std::string{};
        out.push_back(std::format("{:>2}  {:<28} {}", i, name, held.empty() ? "-" : held));
    }
    return out;
}
}
