// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <optional>

namespace playden::homebrew {

// What a game page's details were asked with. What an uninstalled game's page offers (its DLC list) comes from the
// licenses and from it not being installed.
struct DetailsAsked {
    std::uint64_t licenses_read = 0;
    bool installed = false;
    bool operator==(const DetailsAsked& other) const { return licenses_read == other.licenses_read && installed == other.installed; }
};

enum class DetailsAsk { kNothing, kOnce, kAgain };

// `new_visit`: the page just opened. An installed game is asked again at each visit, as Steam may have a newer version
// since; any game whose answer is from before its licenses or install state changed is asked again, even while its
// page stays open, and so is one this page never asked for: an update check may have answered it while it was installed.
// Otherwise one answer per session will do.
inline DetailsAsk NextDetailsAsk(const std::optional<DetailsAsked>& last, const DetailsAsked& now, bool new_visit) {
    const bool outdated = !last || !(*last == now);
    if (!new_visit && !outdated) return DetailsAsk::kNothing;
    return now.installed || outdated ? DetailsAsk::kAgain : DetailsAsk::kOnce;
}

}  // namespace playden::homebrew
