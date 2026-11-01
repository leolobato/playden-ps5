// browse.hpp - Pure catalog projection and controller navigation helpers.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: MIT
#pragma once

#include "homebrew/simulation.hpp"

#include <charconv>
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

// Moves only Home and Library browse focus. Returning true means the route
// consumed the direction even when focus was already against an edge.
bool move_browser(State& state, int delta_x, int delta_y);

// Details shows this many tags, genres first. The rest are a "+N" chip that lists them all.
inline constexpr std::size_t kDetailTagPreview = 6;

// A focusable chip on a game's page: one tag, or the "+N" that opens the rest.
[[nodiscard]] inline bool DetailChipKey(std::string_view key) noexcept {
    constexpr std::string_view kPrefix = "detail-tag-";
    return key == "detail-tags-more" ||
           (key.size() > kPrefix.size() && key.compare(0, kPrefix.size(), kPrefix) == 0);
}

// A ticked library tag, drawn as a chip. The index follows the saved set's order.
[[nodiscard]] inline std::string SelectedTagKey(std::size_t index) {
    return "selected-tag-" + std::to_string(index);
}
[[nodiscard]] inline bool SelectedTagIndex(std::string_view key, std::size_t& index) noexcept {
    constexpr std::string_view kPrefix = "selected-tag-";
    if (key.size() <= kPrefix.size() || key.compare(0, kPrefix.size(), kPrefix) != 0) return false;
    const char* const begin = key.data() + kPrefix.size();
    const auto parsed = std::from_chars(begin, key.data() + key.size(), index);
    return parsed.ec == std::errc{} && parsed.ptr == key.data() + key.size();
}

[[nodiscard]] std::size_t previous_caret(std::string_view utf8, std::size_t caret) noexcept;
[[nodiscard]] std::size_t next_caret(std::string_view utf8, std::size_t caret) noexcept;

} // namespace playden::homebrew
