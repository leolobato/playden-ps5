// browse.hpp - Pure catalog projection and controller navigation helpers.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: MIT
#pragma once

#include "homebrew/simulation.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace playden::homebrew {

// Continue playing shows at most this many games; the rest are a "More" cell that opens the Library.
inline constexpr std::size_t kShelfLimit = 20;

struct Shelf {
    std::string key;
    std::string title;
    std::vector<GameId> game_ids;
    int more = 0;  // games left out; above 0, a "More" cell follows the last game
};

// Focusable cells in a shelf: its games, plus the "More" cell when games were left out.
[[nodiscard]] inline int shelf_cells(const Shelf& shelf) noexcept {
    return static_cast<int>(shelf.game_ids.size()) + (shelf.more > 0 ? 1 : 0);
}
[[nodiscard]] inline std::string shelf_more_key(const Shelf& shelf) { return "shelf-" + shelf.key + "-more"; }

struct ShelfWindow {
    int first = 0;
    int count = 0;
};

[[nodiscard]] std::vector<const Game*> filtered_games(const State& state);
[[nodiscard]] std::vector<Shelf> shelves(const State& state);
[[nodiscard]] ShelfWindow shelf_window(int item_count, int focused_item,
                                       int capacity = 7) noexcept;
// The Library rail's focusable entries: Installed, All, Favorites, Hidden, the store scopes, the
// collections, then "new-collection". `selected_index` counts in this list; dividers are not in it.
[[nodiscard]] std::vector<std::string> library_rail_scopes(const State& state);
// The rail's store section: "steam", "gog" and "imported" (Store::kLocal), each while it has games, in
// that order. Empty while the games come from one store, where an entry would repeat All.
[[nodiscard]] std::vector<std::string> library_store_scopes(const State& state);
// The scope the rail marks as chosen: the Library's scope, or "all" for a store scope whose entry is left out
// because all the games are that store's. A scope with no entry otherwise (a store with no games left) marks none.
[[nodiscard]] std::string library_rail_marked_scope(const State& state);
// The Store filter's store ("steam", "gog", "imported", from the filter "store-<store>"), or empty for Any.
[[nodiscard]] std::string library_store_filter(const State& state);
// The stores the Sort and filters sheet offers under Store, after Any: as library_store_scopes, plus the store picked
// while it is not among them. Empty: the sheet has no Store group.
[[nodiscard]] std::vector<std::string> library_filter_stores(const State& state);
// Indexes in library_rail_scopes that have a divider drawn above them: around the store section.
[[nodiscard]] std::vector<int> library_rail_dividers(const State& state);
// The library has games imported from a drive or the drop folder.
[[nodiscard]] bool has_imported_games(const State& state);
// The library has GOG games.
[[nodiscard]] bool has_gog_games(const State& state);

// Moves only Home and Library browse focus. Returning true means the route
// consumed the direction even when focus was already against an edge.
bool move_browser(State& state, int delta_x, int delta_y);

[[nodiscard]] std::size_t previous_caret(std::string_view utf8, std::size_t caret) noexcept;
[[nodiscard]] std::size_t next_caret(std::string_view utf8, std::size_t caret) noexcept;

} // namespace playden::homebrew
