// browse.cpp - Pure catalog projection and controller navigation helpers.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: MIT

#include "homebrew/browse.hpp"
#include "library/query.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <set>
#include <utility>

namespace playden::homebrew {
namespace {

constexpr int kLibraryColumns = 6;

bool catalog_available(const State& state)
{
    // Steam's catalog needs its account; another store's games (imported ones) are listed signed out too.
    return state.account().cache_available || state.account().authentication == AuthState::SignedIn ||
           state.account().authentication == AuthState::Offline ||
           std::any_of(state.games.begin(), state.games.end(), [](const Game& game) { return game.id.store != Store::kSteam; });
}

bool installed(const State& state, GameId id)
{
    // A game playing in place from its own folder (imported) counts as installed here.
    if (std::any_of(state.external_games.begin(), state.external_games.end(),
                    [id](const LiveExternalGame& value) { return value.game_id == id; }))
        return true;
    return std::any_of(state.installations.begin(), state.installations.end(),
                       [&state, id](const Installation& value) {
                           // Live installs are known from the install service's records, even
                           // when an old record carries no size.
                           return value.game_id == id && (value.installed_bytes > 0 || !state.simulation_mode);
                       });
}

std::string ascii_lower(std::string_view value)
{
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char byte) {
        return byte >= 'A' && byte <= 'Z' ? static_cast<char>(byte - 'A' + 'a')
                                         : static_cast<char>(byte);
    });
    return result;
}

const Collection* selected_collection(const State& state)
{
    const auto found = std::find_if(state.library.collections.begin(), state.library.collections.end(),
                                    [&state](const Collection& collection) {
                                        return collection.id == state.library.scope;
                                    });
    return found == state.library.collections.end() ? nullptr : &*found;
}

template <typename Value>
bool optional_descending(const std::optional<Value>& left, const std::optional<Value>& right)
{
    if (left.has_value() != right.has_value())
        return left.has_value();
    return left && right && *left > *right;
}

const Game* find_game(const State& state, GameId id)
{
    const auto found = std::find_if(state.games.begin(), state.games.end(),
                                    [id](const Game& game) { return game.id == id; });
    return found == state.games.end() ? nullptr : &*found;
}

void append_known(const State& state, Shelf& shelf, GameId id, std::set<GameId>& seen)
{
    const Game* game = find_game(state, id);
    if (game != nullptr && !state.library.hidden.contains(id) && seen.insert(id).second)
        shelf.game_ids.push_back(id);
}

void append_shelf(std::vector<Shelf>& result, Shelf shelf)
{
    if (!shelf.game_ids.empty())
        result.push_back(std::move(shelf));
}

std::string rail_key(std::string_view scope)
{
    return "rail-" + std::string(scope);
}

void select_game(State& state, GameId id, int index)
{
    state.navigation.selected_game_id = id;
    state.navigation.selected_index = index;
    state.navigation.focus_key = "game-" + ToKey(id);
}

} // namespace

std::vector<const Game*> filtered_games(const State& state)
{
    std::vector<const Game*> result;
    if (!catalog_available(state) || state.library.filters.contains("empty-fixture"))
        return result;

    const std::string query = state.simulation_mode ? ascii_lower(state.library.query)
                                                   : FoldLibraryText(state.library.query);
    std::map<GameId,std::string> folded_titles;
    if(!state.simulation_mode)
        for(const auto& game:state.games)
            if(game.folded_title.empty()) folded_titles.emplace(game.id,FoldLibraryText(game.title));
    const auto folded_title=[&](const Game& game)->const std::string& {
        return game.folded_title.empty() ? folded_titles.at(game.id) : game.folded_title;
    };
    const Collection* collection = selected_collection(state);
    for (const Game& game : state.games)
    {
        if (state.library.scope == "installed" && !installed(state, game.id))
            continue;
        if (state.library.scope == "favorites" && !state.library.favorites.contains(game.id))
            continue;
        if (state.library.scope == "hidden" && !state.library.hidden.contains(game.id))
            continue;
        if (state.library.scope == "imported" && game.id.store != Store::kLocal)
            continue;
        if (collection != nullptr && !collection->members.contains(game.id))
            continue;
        if (state.library.scope != "hidden" && state.library.hidden.contains(game.id))
            continue;
        if (!query.empty()) {
            const bool matches=state.simulation_mode
                ? ascii_lower(game.title).find(query)!=std::string::npos
                : folded_title(game).find(query)!=std::string::npos;
            if(!matches) continue;
        }
        if (state.library.filters.contains("installed") && !installed(state, game.id))
            continue;
        // Demos show unless the player hides them, so the stored filter is the hiding one.
        if (state.library.filters.contains("hide-demos") && game.demo)
            continue;
        if (state.library.filters.contains("hide-family") && game.family_shared)
            continue;
        if (state.library.filters.contains("genre-adventure") &&
            std::find(game.genres.begin(), game.genres.end(), "Adventure") == game.genres.end())
            continue;
        if (state.library.filters.contains("controller-full") && game.controller_support != "full")
            continue;
        if (state.library.filters.contains("compat-playable"))
        {
            const auto rating = state.library.ratings.find(game.id);
            if (rating == state.library.ratings.end() ||
                (rating->second != Compatibility::Playable && rating->second != Compatibility::Works))
                continue;
        }
        result.push_back(&game);
    }

    const auto& sort = state.library.view_sort.empty() ? state.library.sort : state.library.view_sort;
    if (sort == "name")
    {
        std::stable_sort(result.begin(), result.end(), [&](const Game* left, const Game* right) {
            if(!state.simulation_mode) {
                const auto& a=folded_title(*left);const auto& b=folded_title(*right);
                return a==b ? left->id<right->id : a<b;
            }
            return ascii_lower(left->title) < ascii_lower(right->title);
        });
    }
    else if (sort == "recent-played")
    {
        std::stable_sort(result.begin(), result.end(), [](const Game* left, const Game* right) {
            return optional_descending(left->last_played, right->last_played);
        });
    }
    else if (sort == "recent-added")
    {
        std::stable_sort(result.begin(), result.end(), [](const Game* left, const Game* right) {
            return optional_descending(left->added_at, right->added_at);
        });
    }
    else if (sort == "playtime")
    {
        std::stable_sort(result.begin(), result.end(), [](const Game* left, const Game* right) {
            return optional_descending(left->playtime_minutes, right->playtime_minutes);
        });
    }
    else if (sort == "install-size")
    {
        std::stable_sort(result.begin(), result.end(), [](const Game* left, const Game* right) {
            return optional_descending(left->install_bytes, right->install_bytes);
        });
    }
    return result;
}

std::vector<Shelf> shelves(const State& state)
{
    std::vector<Shelf> result;
    if (!catalog_available(state))
        return result;

    Shelf continued{"continue", "Continue playing", {}};
    std::vector<const Game*> played;
    for (const Game& game : state.games)
        if (!state.library.hidden.contains(game.id) && game.playtime_minutes.value_or(0) > 0)
            played.push_back(&game);
    std::stable_sort(played.begin(), played.end(), [](const Game* left, const Game* right) {
        return optional_descending(left->last_played, right->last_played);
    });
    for (const Game* game : played)
    {
        if (continued.game_ids.size() == kShelfLimit)
        {
            continued.more = static_cast<int>(played.size() - kShelfLimit);
            break;
        }
        continued.game_ids.push_back(game->id);
    }
    append_shelf(result, std::move(continued));

    Shelf downloading{"downloading", "Downloading", {}};
    std::vector<const Job*> jobs;
    for (const Job& job : state.jobs)
        if (job.state != JobState::Cancelled && job.state != JobState::Downloaded)
            jobs.push_back(&job);
    std::stable_sort(jobs.begin(), jobs.end(), [](const Job* left, const Job* right) {
        return left->queue_order < right->queue_order;
    });
    std::set<GameId> seen;
    for (const Job* job : jobs)
        append_known(state, downloading, job->game_id, seen);
    append_shelf(result, std::move(downloading));

    Shelf recent{"installed", "Recently installed", {}};
    seen.clear();
    for (auto installation = state.installations.rbegin(); installation != state.installations.rend();
         ++installation)
        if (installation->installed_bytes > 0)
            append_known(state, recent, installation->game_id, seen);
    // Imported games playing from their folder, newest first: without a store sign-in, they are
    // the whole of Home.
    std::vector<const Game*> in_place;
    for (const LiveExternalGame& external : state.external_games)
        if (const Game* game = find_game(state, external.game_id))
            in_place.push_back(game);
    std::stable_sort(in_place.begin(), in_place.end(), [](const Game* left, const Game* right) {
        return optional_descending(left->added_at, right->added_at);
    });
    for (const Game* game : in_place)
        append_known(state, recent, game->id, seen);
    append_shelf(result, std::move(recent));

    Shelf favorites{"favorites", "Favorites", {}};
    for (const Game& game : state.games)
        if (state.library.favorites.contains(game.id) && !state.library.hidden.contains(game.id))
            favorites.game_ids.push_back(game.id);
    append_shelf(result, std::move(favorites));

    for (const Collection& collection : state.library.collections)
    {
        if (!collection.pinned)
            continue;
        Shelf shelf{"collection-" + collection.id, collection.name, {}};
        for (const Game& game : state.games)
            if (collection.members.contains(game.id) && !state.library.hidden.contains(game.id))
                shelf.game_ids.push_back(game.id);
        append_shelf(result, std::move(shelf));
    }
    return result;
}

ShelfWindow shelf_window(int item_count, int focused_item, int capacity) noexcept
{
    item_count = std::max(0, item_count);
    capacity = std::max(1, capacity);
    if (item_count == 0)
        return {};
    focused_item = std::clamp(focused_item, 0, item_count - 1);
    const int count = std::min(item_count, capacity);
    const int first = std::clamp(focused_item - capacity / 2, 0, item_count - count);
    return {first, count};
}

bool has_imported_games(const State& state)
{
    return std::any_of(state.games.begin(), state.games.end(),
                       [](const Game& game) { return game.id.store == Store::kLocal; });
}

std::vector<std::string> library_rail_scopes(const State& state)
{
    std::vector<std::string> result{"installed", "all", "favorites", "hidden"};
    if (has_imported_games(state))
        result.push_back("imported");
    for (const Collection& collection : state.library.collections)
        result.push_back(collection.id);
    result.push_back("new-collection");
    return result;
}

bool move_browser(State& state, int delta_x, int delta_y)
{
    if (state.navigation.route == Route::Home)
    {
        const std::vector<Shelf> projected = shelves(state);
        // No shelves: Home's empty state has only its buttons, which Up and Down step through.
        if (projected.empty())
            return false;
        int shelf_index = std::clamp(state.navigation.home_shelf, 0,
                                     static_cast<int>(projected.size()) - 1);
        int child = std::clamp(state.navigation.home_shelf_focus[projected[static_cast<std::size_t>(shelf_index)].key],
                               0, shelf_cells(projected[static_cast<std::size_t>(shelf_index)]) - 1);
        if (delta_y != 0)
        {
            state.navigation.home_shelf_focus[projected[static_cast<std::size_t>(shelf_index)].key] = child;
            shelf_index = std::clamp(shelf_index + delta_y, 0, static_cast<int>(projected.size()) - 1);
            const Shelf& shelf = projected[static_cast<std::size_t>(shelf_index)];
            child = std::clamp(state.navigation.home_shelf_focus[shelf.key], 0, shelf_cells(shelf) - 1);
        }
        if (delta_x != 0)
        {
            const Shelf& shelf = projected[static_cast<std::size_t>(shelf_index)];
            child = std::clamp(child + delta_x, 0, shelf_cells(shelf) - 1);
        }
        const Shelf& shelf = projected[static_cast<std::size_t>(shelf_index)];
        state.navigation.home_shelf = shelf_index;
        state.navigation.home_shelf_focus[shelf.key] = child;
        if (child == static_cast<int>(shelf.game_ids.size()))
        {
            // The "More" cell: no game is selected there.
            state.navigation.selected_game_id = {};
            state.navigation.selected_index = child;
            state.navigation.focus_key = shelf_more_key(shelf);
        }
        else
            select_game(state, shelf.game_ids[static_cast<std::size_t>(child)], child);
        state.navigation.scroll_offsets["home"] = shelf_index;
        return true;
    }

    if (state.navigation.route != Route::Library)
        return false;

    const std::vector<std::string> scopes = library_rail_scopes(state);
    if (state.navigation.library_rail_focused)
    {
        int rail_index = 0;
        const std::string prefix = "rail-";
        const std::string current = state.navigation.focus_key.rfind(prefix, 0) == 0
                                        ? state.navigation.focus_key.substr(prefix.size())
                                        : state.library.scope;
        const auto found = std::find(scopes.begin(), scopes.end(), current);
        if (found != scopes.end())
            rail_index = static_cast<int>(std::distance(scopes.begin(), found));
        if (delta_y != 0)
            rail_index = std::clamp(rail_index + delta_y, 0, static_cast<int>(scopes.size()) - 1);
        state.navigation.selected_index = rail_index;
        state.navigation.focus_key = rail_key(scopes[static_cast<std::size_t>(rail_index)]);
        if (delta_x > 0)
        {
            const std::vector<const Game*> games = filtered_games(state);
            if (!games.empty())
            {
                int index = std::clamp(state.navigation.library_grid_column, 0,
                                       std::min(kLibraryColumns - 1, static_cast<int>(games.size()) - 1));
                const auto selected = std::find_if(games.begin(), games.end(), [&state](const Game* game) {
                    return game->id == state.navigation.selected_game_id;
                });
                if (selected != games.end())
                    index = static_cast<int>(std::distance(games.begin(), selected));
                state.navigation.library_rail_focused = false;
                state.navigation.library_grid_column = index % kLibraryColumns;
                select_game(state, games[static_cast<std::size_t>(index)]->id, index);
            }
        }
        return true;
    }

    const std::vector<const Game*> games = filtered_games(state);
    if (games.empty())
    {
        if (delta_x < 0)
        {
            state.navigation.library_rail_focused = true;
            const auto scope = std::find(scopes.begin(), scopes.end(), state.library.scope);
            const int rail_index = scope == scopes.end() ? (state.simulation_mode ? 1 : 0) : static_cast<int>(std::distance(scopes.begin(), scope));
            state.navigation.selected_index = rail_index;
            state.navigation.focus_key = rail_key(scopes[static_cast<std::size_t>(rail_index)]);
        }
        return true;
    }
    int index = std::clamp(state.navigation.selected_index, 0, static_cast<int>(games.size()) - 1);
    const auto selected = std::find_if(games.begin(), games.end(), [&state](const Game* game) {
        return game->id == state.navigation.selected_game_id;
    });
    if (selected != games.end())
        index = static_cast<int>(std::distance(games.begin(), selected));
    int column = index % kLibraryColumns;

    if (delta_x < 0 && column == 0)
    {
        state.navigation.library_grid_column = column;
        state.navigation.library_rail_focused = true;
        const auto scope = std::find(scopes.begin(), scopes.end(), state.library.scope);
        const int rail_index = scope == scopes.end() ? (state.simulation_mode ? 1 : 0) : static_cast<int>(std::distance(scopes.begin(), scope));
        state.navigation.selected_index = rail_index;
        state.navigation.focus_key = rail_key(scopes[static_cast<std::size_t>(rail_index)]);
        return true;
    }
    if (delta_x != 0)
    {
        const int candidate = index + delta_x;
        if (candidate >= 0 && candidate < static_cast<int>(games.size()) &&
            candidate / kLibraryColumns == index / kLibraryColumns)
            index = candidate;
    }
    if (delta_y != 0)
    {
        const int rows = (static_cast<int>(games.size()) + kLibraryColumns - 1) / kLibraryColumns;
        const int row = std::clamp(index / kLibraryColumns + delta_y, 0, rows - 1);
        index = std::min(row * kLibraryColumns + column, static_cast<int>(games.size()) - 1);
    }
    state.navigation.library_grid_column = index % kLibraryColumns;
    state.navigation.scroll_offsets["library-grid"] = index / kLibraryColumns;
    select_game(state, games[static_cast<std::size_t>(index)]->id, index);
    return true;
}

std::size_t previous_caret(std::string_view utf8, std::size_t caret) noexcept
{
    caret = std::min(caret, utf8.size());
    if (caret == 0)
        return 0;
    --caret;
    while (caret > 0 && (static_cast<unsigned char>(utf8[caret]) & 0xC0U) == 0x80U)
        --caret;
    return caret;
}

std::size_t next_caret(std::string_view utf8, std::size_t caret) noexcept
{
    caret = std::min(caret, utf8.size());
    if (caret == utf8.size())
        return caret;
    ++caret;
    while (caret < utf8.size() && (static_cast<unsigned char>(utf8[caret]) & 0xC0U) == 0x80U)
        ++caret;
    return caret;
}

} // namespace playden::homebrew
