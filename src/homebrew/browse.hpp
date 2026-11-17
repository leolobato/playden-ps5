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
// One Library rail entry. Its focus key is "rail-" + key.
enum class RailEntryKind {
    kScope,            // a scope: Installed, All, Favorites, Hidden, a store without collections, a collection
    kStoreFolder,      // "store-steam", "store-gog": a store with collections, which Cross opens and closes. In the
                       // store section, or alone where the section would be when one store has every game (2A)
    kStoreAll,         // "steam", "gog": the first row in an open store, all its games
    kStoreCollection,  // "steam-collection:<id>", "gog-tag:<id>": one of an open store's collections
    kNewCollection,    // "new-collection"
};
struct RailEntry {
    std::string key;
    RailEntryKind kind = RailEntryKind::kScope;
    std::string store;     // folders and the rows inside them: "steam", "gog"
    bool open = false;     // a folder that is open
    bool section = false;  // in the store section, which dividers set apart
};
// The rail's entries in order: Installed, All, Favorites, Hidden, the store section (a folder with its rows when
// open), the collections, then "new-collection".
[[nodiscard]] std::vector<RailEntry> library_rail_entries(const State& state);
// The keys of library_rail_entries. `selected_index` counts in this list; dividers are not in it.
[[nodiscard]] std::vector<std::string> library_rail_scopes(const State& state);
// The scope of a store collection: "steam-collection:<id>" or "gog-tag:<id>".
[[nodiscard]] std::string store_collection_scope(const StoreCollection& collection);
// Whether `scope` has a store collection's form (whatever the ID), and its store's rail name ("steam", "gog").
[[nodiscard]] bool is_store_collection_scope(std::string_view scope);
// The store a scope belongs to: "steam" for "steam" and "steam-collection:…", "gog" for "gog" and "gog-tag:…".
[[nodiscard]] std::string scope_store(std::string_view scope);
// The store collection a scope names, or null.
[[nodiscard]] const StoreCollection* find_store_collection(const State& state, std::string_view scope);
// A store's collections in name order (case-folded), as the rail lists them: those with a library game, and the
// chosen one.
[[nodiscard]] std::vector<const StoreCollection*> store_collections_of(const State& state, Store store);
// How many of a store collection's games are in the library (hidden ones too).
[[nodiscard]] std::size_t store_collection_count(const State& state, const StoreCollection& collection);
// The store open in the rail: the one the player opened, else the store of the chosen scope. Empty: none.
[[nodiscard]] std::string library_rail_open_store(const State& state);
// Moves the rail focus to the entry with `key` (focus_key and selected_index). False when it isn't in the rail.
bool focus_library_rail_entry(State& state, const std::string& key);
// Opens the store of the chosen scope and returns the rail index of the scope's entry (or the marked one), as
// Left from the grid needs.
int library_rail_index_for_scope(State& state);
// While the rail has the focus: keeps it on an entry that is in the rail. A focused entry that left it (its collection
// was deleted, its store closed or lost its collections) gives the focus to its store's entry, or the marked one;
// selected_index follows the focus key.
void reconcile_library_rail_focus(State& state);
// A chosen store collection that its store no longer has (its list is known) falls back to the store's games, or
// All when the store has no games. True when the scope changed.
bool reconcile_store_collection_scope(State& state);
// The rail's store section: "steam", "gog" and "imported" (Store::kLocal), each while it has games, in
// that order. Empty while the games come from one store, where an entry would repeat All.
[[nodiscard]] std::vector<std::string> library_store_scopes(const State& state);
// The rail entry marked as chosen: the Library's scope, or "all" for a store scope whose entry is left out
// because all the games are that store's, or a closed store's folder for its scope or one of its collections.
// A scope with no entry otherwise (a store with no games left) marks none.
[[nodiscard]] std::string library_rail_marked_scope(const State& state);
// The Store filter's store ("steam", "gog", "imported", from the filter "store-<store>"), or empty for Any.
[[nodiscard]] std::string library_store_filter(const State& state);
// The stores the Sort and filters sheet offers under Store, after Any: as library_store_scopes, plus the store picked
// while it is not among them. Empty: the sheet has no Store group.
[[nodiscard]] std::vector<std::string> library_filter_stores(const State& state);
// Indexes in library_rail_scopes that have a divider drawn above them: around the store section (with an open
// store's rows), or around the 2A folder.
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
