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
[[nodiscard]] std::vector<std::string> library_rail_scopes(const State& state);
// The library has games imported from a drive or the drop folder: the rail shows "Imported".
[[nodiscard]] bool has_imported_games(const State& state);
// The library has GOG games: the rail shows "GOG", after "Imported".
[[nodiscard]] bool has_gog_games(const State& state);

// Moves only Home and Library browse focus. Returning true means the route
// consumed the direction even when focus was already against an edge.
bool move_browser(State& state, int delta_x, int delta_y);

[[nodiscard]] std::size_t previous_caret(std::string_view utf8, std::size_t caret) noexcept;
[[nodiscard]] std::size_t next_caret(std::string_view utf8, std::size_t caret) noexcept;

} // namespace playden::homebrew
