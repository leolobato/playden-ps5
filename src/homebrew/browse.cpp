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

// The rail's store scopes, in the rail's order: Steam, GOG, then imported games (Store::kLocal).
constexpr std::pair<std::string_view, Store> kStoreScopes[] = {
    {"steam", Store::kSteam}, {"gog", Store::kGog}, {"imported", Store::kLocal}};

std::optional<Store> rail_store(std::string_view scope)
{
    for (const auto& [name, store] : kStoreScopes)
        if (scope == name)
            return store;
    return std::nullopt;
}

bool has_store_games(const State& state, Store store)
{
    return std::any_of(state.games.begin(), state.games.end(),
                       [store](const Game& game) { return game.id.store == store; });
}

std::string rail_key(std::string_view scope)
{
    return "rail-" + std::string(scope);
}

// The scope prefix of each store's collections, and the store it names.
constexpr std::pair<std::string_view, Store> kStoreCollectionPrefixes[] = {
    {"steam-collection:", Store::kSteam}, {"gog-tag:", Store::kGog}};

std::string_view store_name(Store store)
{
    for (const auto& [name, value] : kStoreScopes)
        if (value == store)
            return name;
    return {};
}

bool store_has_collections(const State& state, Store store)
{
    return store != Store::kLocal && !store_collections_of(state, store).empty();
}

// The store that has every game in the library, when one does (no store section then).
std::optional<Store> only_store(const State& state)
{
    if (state.games.empty())
        return std::nullopt;
    const Store store = state.games.front().id.store;
    if (std::all_of(state.games.begin(), state.games.end(), [store](const Game& game) { return game.id.store == store; }))
        return store;
    return std::nullopt;
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

    // "portal " still finds Portal.
    const std::string search = TrimLibraryQuery(state.library.query);
    const std::string query = state.simulation_mode ? ascii_lower(search) : FoldLibraryText(search);
    std::map<GameId,std::string> folded_titles;
    if(!state.simulation_mode)
        for(const auto& game:state.games)
            if(game.folded_title.empty()) folded_titles.emplace(game.id,FoldLibraryText(game.title));
    const auto folded_title=[&](const Game& game)->const std::string& {
        return game.folded_title.empty() ? folded_titles.at(game.id) : game.folded_title;
    };
    const Collection* collection = selected_collection(state);
    // A store collection lists its games in the library; one not (yet) known lists none.
    const bool store_scope = is_store_collection_scope(state.library.scope);
    const StoreCollection* store_collection = store_scope ? find_store_collection(state, state.library.scope) : nullptr;
    if (store_scope && store_collection == nullptr)
        return result;
    const std::optional<Store> store_filter = rail_store(library_store_filter(state));
    for (const Game& game : state.games)
    {
        if (state.library.scope == "installed" && !installed(state, game.id))
            continue;
        if (state.library.scope == "favorites" && !state.library.favorites.contains(game.id))
            continue;
        if (state.library.scope == "hidden" && !state.library.hidden.contains(game.id))
            continue;
        if (const auto store = rail_store(state.library.scope); store && game.id.store != *store)
            continue;
        if (collection != nullptr && !collection->members.contains(game.id))
            continue;
        if (store_collection != nullptr && !store_collection->members.contains(game.id))
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
        // The Store filter ("store-steam", "store-gog", "store-imported"); one naming no store this build knows is Any.
        if (store_filter && game.id.store != *store_filter)
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
    // Newest install first (a move keeps the game's time); without times, the latest listed first.
    std::vector<const Installation*> installed;
    for (auto installation = state.installations.rbegin(); installation != state.installations.rend();
         ++installation)
        if (installation->installed_bytes > 0)
            installed.push_back(&*installation);
    std::stable_sort(installed.begin(), installed.end(), [](const Installation* left, const Installation* right) {
        return left->installed_at > right->installed_at;
    });
    for (const Installation* installation : installed)
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
    return has_store_games(state, Store::kLocal);
}

bool has_gog_games(const State& state)
{
    return has_store_games(state, Store::kGog);
}

std::vector<std::string> library_store_scopes(const State& state)
{
    std::vector<std::string> result;
    for (const auto& [name, store] : kStoreScopes)
        if (has_store_games(state, store))
            result.emplace_back(name);
    // Games of one store only: its entry would repeat All.
    if (result.size() < 2)
        result.clear();
    return result;
}

std::string library_store_filter(const State& state)
{
    for (const std::string& filter : state.library.filters)
        if (filter.rfind("store-", 0) == 0)
            return filter.substr(6);
    return {};
}

std::vector<std::string> library_filter_stores(const State& state)
{
    std::vector<std::string> result = library_store_scopes(state);
    // The store picked stays listed while it has no games, or alone, so the filter can be set back to Any.
    const std::string chosen = library_store_filter(state);
    if (rail_store(chosen) && std::find(result.begin(), result.end(), chosen) == result.end())
    {
        result.clear();
        for (const auto& [name, store] : kStoreScopes)
            if (name == chosen || has_store_games(state, store))
                result.emplace_back(name);
    }
    return result;
}

std::string store_collection_scope(const StoreCollection& collection)
{
    for (const auto& [prefix, store] : kStoreCollectionPrefixes)
        if (store == collection.store)
            return std::string(prefix) + collection.id;
    return {};
}

bool is_store_collection_scope(std::string_view scope)
{
    return std::any_of(std::begin(kStoreCollectionPrefixes), std::end(kStoreCollectionPrefixes),
                       [scope](const auto& entry) { return scope.rfind(entry.first, 0) == 0 && scope.size() > entry.first.size(); });
}

std::string scope_store(std::string_view scope)
{
    for (const auto& [prefix, store] : kStoreCollectionPrefixes)
        if (scope.rfind(prefix, 0) == 0 && scope.size() > prefix.size())
            return std::string(store_name(store));
    if (const auto store = rail_store(scope); store && *store != Store::kLocal)
        return std::string(scope);
    return {};
}

const StoreCollection* find_store_collection(const State& state, std::string_view scope)
{
    for (const StoreCollection& collection : state.store_collections)
    {
        for (const auto& [prefix, store] : kStoreCollectionPrefixes)
            if (store == collection.store && scope.size() == prefix.size() + collection.id.size() &&
                scope.rfind(prefix, 0) == 0 && scope.substr(prefix.size()) == collection.id)
                return &collection;
    }
    return nullptr;
}

std::vector<const StoreCollection*> store_collections_of(const State& state, Store store)
{
    std::vector<std::pair<std::string, const StoreCollection*>> named;
    std::set<GameId> library;  // filled on the first collection of the store: the rail asks often
    bool library_ready = false;
    for (const StoreCollection& collection : state.store_collections)
    {
        if (collection.store != store)
            continue;
        if (!library_ready)
        {
            for (const Game& game : state.games)
                library.insert(game.id);
            library_ready = true;
        }
        // A collection with none of the library's games (a Steam ROM Manager list of non-Steam shortcuts, an unused
        // GOG tag) stays out of the rail, unless it is the chosen scope: the rail keeps the row the grid shows.
        const bool has_games = std::any_of(collection.members.begin(), collection.members.end(),
                                           [&library](const GameId& member) { return library.contains(member); });
        if (has_games || state.library.scope == store_collection_scope(collection))
            named.emplace_back(FoldLibraryText(collection.name), &collection);
    }
    std::stable_sort(named.begin(), named.end(), [](const auto& left, const auto& right) {
        return left.first == right.first ? left.second->id < right.second->id : left.first < right.first;
    });
    std::vector<const StoreCollection*> result;
    result.reserve(named.size());
    for (const auto& [name, collection] : named)
        result.push_back(collection);
    return result;
}

std::size_t store_collection_count(const State& state, const StoreCollection& collection)
{
    return static_cast<std::size_t>(std::count_if(state.games.begin(), state.games.end(), [&collection](const Game& game) {
        return collection.members.contains(game.id);
    }));
}

std::string library_rail_open_store(const State& state)
{
    if (state.navigation.library_rail_open_store)
        return *state.navigation.library_rail_open_store;
    return scope_store(state.library.scope);
}

std::vector<RailEntry> library_rail_entries(const State& state)
{
    std::vector<RailEntry> result;
    for (const char* scope : {"installed", "all", "favorites", "hidden"})
        result.push_back({scope, RailEntryKind::kScope});
    const std::string open = library_rail_open_store(state);
    const auto add_folder = [&](std::string_view name, Store store, bool with_all) {
        const bool is_open = open == name;
        result.push_back({"store-" + std::string(name), RailEntryKind::kStoreFolder, std::string(name), is_open, true});
        if (!is_open)
            return;
        if (with_all)
            result.push_back({std::string(name), RailEntryKind::kStoreAll, std::string(name), false, true});
        for (const StoreCollection* collection : store_collections_of(state, store))
            result.push_back({store_collection_scope(*collection), RailEntryKind::kStoreCollection, std::string(name), false,
                              true});
    };
    const std::vector<std::string> stores = library_store_scopes(state);
    for (const std::string& name : stores)
    {
        const Store store = *rail_store(name);
        if (store_has_collections(state, store))
            add_folder(name, store, true);
        else
            result.push_back({name, RailEntryKind::kScope, {}, false, true});
    }
    // 2A: one store has every game, so there is no store section; its collections get a folder there, without an
    // "All" row (All lists the same games).
    if (stores.empty())
        if (const auto store = only_store(state); store && store_has_collections(state, *store))
            add_folder(store_name(*store), *store, false);
    for (const Collection& collection : state.library.collections)
        result.push_back({collection.id, RailEntryKind::kScope});
    result.push_back({"new-collection", RailEntryKind::kNewCollection});
    return result;
}

std::vector<std::string> library_rail_scopes(const State& state)
{
    std::vector<std::string> result;
    for (RailEntry& entry : library_rail_entries(state))
        result.push_back(std::move(entry.key));
    return result;
}

std::string library_rail_marked_scope(const State& state)
{
    const std::vector<RailEntry> entries = library_rail_entries(state);
    const auto has = [&entries](std::string_view key) {
        return std::any_of(entries.begin(), entries.end(), [key](const RailEntry& entry) {
            return entry.key == key && entry.kind != RailEntryKind::kStoreFolder;
        });
    };
    if (has(state.library.scope))
        return state.library.scope;
    // A store scope without its entry because every game is that store's: it lists what All does.
    if (const auto store = rail_store(state.library.scope);
        store && has_store_games(state, *store) && only_store(state) == store)
        return "all";
    // The store's scope or one of its collections, with the store closed: its folder.
    if (const std::string store = scope_store(state.library.scope); !store.empty())
    {
        const std::string folder = "store-" + store;
        if (std::any_of(entries.begin(), entries.end(), [&folder](const RailEntry& entry) { return entry.key == folder; }))
            return folder;
    }
    return state.library.scope;
}

std::vector<int> library_rail_dividers(const State& state)
{
    const std::vector<RailEntry> entries = library_rail_entries(state);
    const auto first = std::find_if(entries.begin(), entries.end(), [](const RailEntry& entry) { return entry.section; });
    if (first == entries.end())
        return {};
    const auto after = std::find_if(first, entries.end(), [](const RailEntry& entry) { return !entry.section; });
    return {static_cast<int>(std::distance(entries.begin(), first)), static_cast<int>(std::distance(entries.begin(), after))};
}

bool focus_library_rail_entry(State& state, const std::string& key)
{
    const std::vector<std::string> keys = library_rail_scopes(state);
    const auto found = std::find(keys.begin(), keys.end(), key);
    if (found == keys.end())
        return false;
    state.navigation.selected_index = static_cast<int>(std::distance(keys.begin(), found));
    state.navigation.focus_key = rail_key(key);
    return true;
}

int library_rail_index_for_scope(State& state)
{
    // Left from the grid lands on the shown scope's own row, so its store opens.
    if (const std::string store = scope_store(state.library.scope); !store.empty())
        state.navigation.library_rail_open_store = store;
    const std::vector<std::string> scopes = library_rail_scopes(state);
    const auto scope = std::find(scopes.begin(), scopes.end(), library_rail_marked_scope(state));
    return scope == scopes.end() ? (state.simulation_mode ? 1 : 0) : static_cast<int>(std::distance(scopes.begin(), scope));
}

void reconcile_library_rail_focus(State& state)
{
    if (!state.navigation.library_rail_focused)
        return;
    const std::vector<std::string> keys = library_rail_scopes(state);
    const std::string prefix = "rail-";
    const std::string current = state.navigation.focus_key.rfind(prefix, 0) == 0
                                    ? state.navigation.focus_key.substr(prefix.size())
                                    : std::string();
    if (focus_library_rail_entry(state, current))
        return;
    if (const std::string store = scope_store(current); !store.empty() && focus_library_rail_entry(state, "store-" + store))
        return;
    if (focus_library_rail_entry(state, library_rail_marked_scope(state)))
        return;
    const int index = std::clamp(state.navigation.selected_index, 0, static_cast<int>(keys.size()) - 1);
    focus_library_rail_entry(state, keys[static_cast<std::size_t>(index)]);
}

bool reconcile_store_collection_scope(State& state)
{
    if (!is_store_collection_scope(state.library.scope) || find_store_collection(state, state.library.scope))
        return false;
    const std::string store = scope_store(state.library.scope);
    const auto known = rail_store(store);
    if (!known || !state.store_collections_known.contains(*known))
        return false;  // the list may still come: the grid waits, as for the library
    // The store's own games while it has any (its "All" row, its scope row, or All itself when it has every game).
    state.library.scope = has_store_games(state, *known) ? store : "all";
    return true;
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
                                        : library_rail_marked_scope(state);
        auto found = std::find(scopes.begin(), scopes.end(), current);
        // An entry that left the rail (its store closed or its list changed): the marked one.
        if (found == scopes.end())
            found = std::find(scopes.begin(), scopes.end(), library_rail_marked_scope(state));
        if (found != scopes.end())
            rail_index = static_cast<int>(std::distance(scopes.begin(), found));
        if (delta_x < 0)
        {
            // Left closes the open store: on its folder, or from a row inside it, which moves the focus to the folder.
            const std::vector<RailEntry> entries = library_rail_entries(state);
            const RailEntry& entry = entries[static_cast<std::size_t>(rail_index)];
            if ((entry.kind == RailEntryKind::kStoreFolder && entry.open) || entry.kind == RailEntryKind::kStoreAll ||
                entry.kind == RailEntryKind::kStoreCollection)
            {
                state.navigation.library_rail_open_store = std::string();
                focus_library_rail_entry(state, "store-" + entry.store);
                return true;
            }
        }
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
            const int rail_index = library_rail_index_for_scope(state);
            state.navigation.selected_index = rail_index;
            state.navigation.focus_key = rail_key(library_rail_scopes(state)[static_cast<std::size_t>(rail_index)]);
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
        const int rail_index = library_rail_index_for_scope(state);
        state.navigation.selected_index = rail_index;
        state.navigation.focus_key = rail_key(library_rail_scopes(state)[static_cast<std::size_t>(rail_index)]);
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
