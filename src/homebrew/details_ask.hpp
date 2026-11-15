// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <map>
#include <optional>

#include "library/store.h"

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

// The game pages' asks, one tick at a time. `page` is the game whose page (or its session) is on screen, none on any
// other screen. A page left for another screen keeps its game for Back, but a visit only starts when the page opens
// again: asking for it on every tick away from it would ask Steam for the same game without end.
class DetailsPageAsks {
 public:
  DetailsAsk Tick(const std::optional<GameId>& page, const DetailsAsked& now) {
    if (!page) {
      shown_.reset();
      return DetailsAsk::kNothing;
    }
    const auto asked = asked_.find(*page);
    const auto ask = NextDetailsAsk(asked == asked_.end() ? std::nullopt : std::optional<DetailsAsked>(asked->second), now,
                                    page != shown_);
    shown_ = page;
    if (ask != DetailsAsk::kNothing) asked_[*page] = now;
    return ask;
  }
  // The page is gone from screen: its next opening is a new visit.
  void Leave() { shown_.reset(); }
  // Another account or language: every page is asked again.
  void Clear() {
    shown_.reset();
    asked_.clear();
  }

 private:
  std::optional<GameId> shown_;
  std::map<GameId, DetailsAsked> asked_;  // what each visited page's details were last asked with
};

}  // namespace playden::homebrew
