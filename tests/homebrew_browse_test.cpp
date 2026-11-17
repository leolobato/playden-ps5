// homebrew_browse_test.cpp - Catalog projection and browse navigation tests.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: MIT

#include "homebrew/browse.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <unistd.h>
#include <vector>
#include "steam_id.h"

namespace {

using namespace playden::homebrew;

void check(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

std::filesystem::path temp_profile()
{
    const auto path = std::filesystem::temp_directory_path() / ("playden-homebrew-browse-test-" + std::to_string(getpid()) + ".json");
    std::error_code error;
    std::filesystem::remove(path, error);
    return path;
}

State fixture_state(const std::filesystem::path& fixtures)
{
    Simulation simulation(fixtures, temp_profile());
    check(simulation.scenario("default"), "default fixture scenario loads");
    return simulation.state();
}

void filtering_uses_real_fixture(const std::filesystem::path& fixtures)
{
    State state = fixture_state(fixtures);
    check(filtered_games(state).size() == 23, "All omits the fixture's hidden game");

    state.library.scope = "installed";
    check(filtered_games(state).size() == 5, "Installed scope uses available installations");
    state.library.scope = "favorites";
    check(filtered_games(state).size() == 2, "Favorites scope uses persisted favorite IDs");
    state.library.scope = "hidden";
    check(filtered_games(state).size() == 1 && filtered_games(state).front()->id == SteamId(900024),
          "Hidden scope exposes only hidden games");
    state.library.scope = "collection-1";
    check(filtered_games(state).size() == 3, "collection scope uses its exact membership");

    state.library.scope = "all";
    state.library.query = "night orchard";
    check(filtered_games(state).size() == 1 && filtered_games(state).front()->id == SteamId(900010),
          "query matching is case insensitive");
    state.library.query = "  night orchard \t";
    check(filtered_games(state).size() == 1 && filtered_games(state).front()->id == SteamId(900010),
          "query matching ignores spaces around the query");
    state.library.query = "   ";
    check(filtered_games(state).size() == 23, "a blank query matches every game");
    state.library.query.clear();
    state.library.filters = {"installed"};
    check(filtered_games(state).size() == 5, "installed filter composes with All");
    state.library.filters = {"genre-adventure"};
    check(filtered_games(state).size() == 6, "genre token matches the fixture model");
    state.library.filters = {"controller-full"};
    check(filtered_games(state).size() == 8, "controller token matches the fixture model");
    state.library.ratings[SteamId(900001)] = Compatibility::Works;
    state.library.ratings[SteamId(900002)] = Compatibility::Playable;
    state.library.ratings[SteamId(900003)] = Compatibility::Broken;
    state.library.filters = {"compat-playable"};
    check(filtered_games(state).size() == 2, "compatibility filter accepts Works and Playable only");

    state.library.filters.clear();
    const auto every_game = filtered_games(state).size();
    auto& demo = *std::find_if(state.games.begin(), state.games.end(), [](const Game& game) { return game.id == SteamId(900010); });
    demo.demo = true;
    check(filtered_games(state).size() == every_game, "demos show with no filter");
    state.library.filters = {"hide-demos"};
    const auto without_demos = filtered_games(state);
    check(without_demos.size() == every_game - 1 &&
              std::none_of(without_demos.begin(), without_demos.end(), [](const Game* game) { return game->demo; }),
          "hiding demos leaves the full games");
    state.library.ratings[SteamId(900010)] = Compatibility::Works;
    state.library.filters = {"compat-playable", "hide-demos"};
    check(filtered_games(state).size() == 2, "hiding demos composes with Playable");
    state.library.ratings.erase(SteamId(900010));
    state.library.filters.clear();
    demo.demo = false;
    demo.family_shared = true;
    check(filtered_games(state).size() == every_game, "family-shared games show with no filter");
    state.library.filters = {"hide-family"};
    const auto owned = filtered_games(state);
    check(owned.size() == every_game - 1 &&
              std::none_of(owned.begin(), owned.end(), [](const Game* game) { return game->family_shared; }),
          "hiding family sharing leaves the owned games");
    state.library.filters.clear();
    demo.family_shared = false;
    state.account().authentication = AuthState::SignedOut;
    state.account().cache_available = false;
    check(filtered_games(state).empty(), "signed out without cache has no browsable catalog");
    state.account().cache_available = true;
    check(filtered_games(state).size() == 23, "signed out cached catalog remains browsable");
}

template <typename Projection>
void check_unknown_last(State state, std::string sort, Projection projection, const char* message)
{
    state.library.sort = std::move(sort);
    state.games.front().*projection = std::nullopt;
    const auto games = filtered_games(state);
    bool saw_unknown = false;
    bool known_after_unknown = false;
    for (const Game* game : games)
    {
        if (!(game->*projection).has_value())
            saw_unknown = true;
        else if (saw_unknown)
            known_after_unknown = true;
    }
    check(saw_unknown && !known_after_unknown, message);
}

void all_sorts_keep_unknown_values_last(const std::filesystem::path& fixtures)
{
    State state = fixture_state(fixtures);
    state.library.sort = "name";
    const auto named = filtered_games(state);
    check(named.front()->title == "A Very Long Journey Beyond the Last Observatory: Director’s Synthetic Edition",
          "name sort is ascending");
    check_unknown_last(state, "recent-played", &Game::last_played,
                       "recently played keeps an unknown value last");
    check_unknown_last(state, "recent-added", &Game::added_at,
                       "recently added keeps an unknown value last");
    check_unknown_last(state, "playtime", &Game::playtime_minutes,
                       "playtime keeps an unknown value last");
    check_unknown_last(state, "install-size", &Game::install_bytes,
                       "install size keeps an unknown value last");
}

void shelves_are_nonempty_and_restore_children(const std::filesystem::path& fixtures)
{
    State state = fixture_state(fixtures);
    const auto projected = shelves(state);
    check(projected.size() == 5, "fixture builds four standard shelves and one pinned shelf");
    for (const Shelf& shelf : projected)
        check(!shelf.game_ids.empty(), "empty Home shelves are omitted");
    check(projected[0].key == "continue" && projected[1].key == "downloading" &&
              projected[2].key == "installed" && projected[3].key == "favorites" &&
              projected[4].key == "collection-collection-1",
          "Home shelf order is stable");

    state.navigation.route = Route::Home;
    state.navigation.home_shelf = 0;
    state.navigation.home_shelf_focus[projected[0].key] = 2;
    check(move_browser(state, 0, 1), "Home consumes vertical movement");
    check(state.navigation.home_shelf == 1, "down moves to the next shelf");
    move_browser(state, 1, 0);
    check(state.navigation.home_shelf_focus[projected[1].key] == 1,
          "horizontal movement updates only the active shelf child");
    move_browser(state, 0, 1);
    move_browser(state, 0, -1);
    check(state.navigation.home_shelf == 1 &&
              state.navigation.home_shelf_focus[projected[1].key] == 1,
          "returning to a Home shelf restores its child");

    state.library.favorites.clear();
    for (std::size_t index = 0; index < 10; ++index)
        state.library.favorites.insert(state.games[index].id);
    const auto expanded = shelves(state);
    const auto favorite = std::find_if(expanded.begin(), expanded.end(), [](const Shelf& shelf) {
        return shelf.key == "favorites";
    });
    check(favorite != expanded.end() && favorite->game_ids.size() == 10,
          "expanded Favorites fixture has more items than one Home row");
    state.navigation.home_shelf = static_cast<int>(std::distance(expanded.begin(), favorite));
    state.navigation.home_shelf_focus["favorites"] = 6;
    move_browser(state, 1, 0);
    check(state.navigation.home_shelf_focus["favorites"] == 7,
          "controller right reaches the eighth Favorites child");
    const ShelfWindow window = shelf_window(static_cast<int>(favorite->game_ids.size()), 7);
    check(window.count == 7 && window.first <= 7 && 7 < window.first + window.count,
          "seven-cell Home projection keeps the eighth focused child visible");
    check(shelf_window(3, 2).first == 0 && shelf_window(3, 2).count == 3,
          "short Home shelves retain their original positions");
}

void library_grid_and_rail_edges(const std::filesystem::path& fixtures)
{
    State state = fixture_state(fixtures);
    state.navigation.route = Route::Library;
    const auto games = filtered_games(state);
    check(games.size() > 12, "fixture has multiple six-column rows");

    state.navigation.selected_index = 5;
    state.navigation.selected_game_id = games[5]->id;
    move_browser(state, 1, 0);
    check(state.navigation.selected_index == 5, "right clamps at the end of a grid row");
    move_browser(state, 0, 1);
    check(state.navigation.selected_index == 11, "down preserves the six-column position");
    move_browser(state, 0, 20);
    check(state.navigation.selected_index == static_cast<int>(games.size()) - 1,
          "vertical movement clamps within a short final row");

    state.navigation.selected_index = 6;
    state.navigation.selected_game_id = games[6]->id;
    move_browser(state, -1, 0);
    check(state.navigation.library_rail_focused && state.navigation.focus_key == "rail-all",
          "left from the first column enters the rail at the active scope");
    move_browser(state, 0, 99);
    const auto scopes = library_rail_scopes(state);
    check(scopes.back() == "new-collection", "rail exposes the new collection action");
    check(state.navigation.focus_key == "rail-" + scopes.back(), "rail movement clamps at its final scope");
    state.navigation.focus_key = "rail-all";
    move_browser(state, 1, 0);
    check(!state.navigation.library_rail_focused && state.navigation.selected_game_id == games[6]->id,
          "right restores the prior grid game after rail focus");

    state.library.query = "no fixture game matches this";
    state.navigation.library_rail_focused = false;
    state.navigation.focus_key = "clear-search";
    move_browser(state, -1, 0);
    check(state.navigation.library_rail_focused && state.navigation.focus_key == "rail-all",
          "an empty result grid can still move left into the rail");

    state.library.collections.clear();
    for (int index = 0; index < 16; ++index)
        state.library.collections.push_back({"collection-" + std::to_string(index),
                                             "Collection " + std::to_string(index), false, {}});
    const auto many_scopes = library_rail_scopes(state);
    check(many_scopes.size() == 21, "sixteen collections extend the complete Library rail");
    const ShelfWindow rail_window = shelf_window(static_cast<int>(many_scopes.size()), 19, 12);
    check(rail_window.count == 12 && rail_window.first <= 19 &&
              19 < rail_window.first + rail_window.count,
          "twelve-row Library rail projection keeps a late collection focused");
}

void download_window_keeps_late_jobs_visible(const std::filesystem::path& fixtures)
{
    State state = fixture_state(fixtures);
    state.jobs.clear();
    for (int index = 0; index < 15; ++index)
    {
        Job job;
        job.id = "bulk-" + std::to_string(index);
        job.game_id = state.games[static_cast<std::size_t>(index)].id;
        job.queue_order = index;
        job.state = index == 0 ? JobState::Downloading : JobState::Queued;
        state.jobs.push_back(std::move(job));
    }
    const int focused_secondary = 12;
    const ShelfWindow window = shelf_window(static_cast<int>(state.jobs.size()) - 1,
                                            focused_secondary, 6);
    check(window.count == 6 && window.first <= focused_secondary &&
              focused_secondary < window.first + window.count,
          "six-row Downloads projection keeps a late queued job focused");
}

void utf8_caret_boundaries()
{
    std::string text = "Aé星ر";
    check(previous_caret(text, text.size()) == 6, "previous caret crosses one Arabic code point");
    check(previous_caret(text, 6) == 3, "previous caret crosses one CJK code point");
    check(previous_caret(text, 2) == 1, "previous caret repairs an interior byte position");
    check(next_caret(text, 1) == 3, "next caret crosses one accented code point");
    check(next_caret(text, 2) == 3, "next caret repairs an interior byte position");
    const std::size_t caret = 6;
    const std::size_t prior = previous_caret(text, caret);
    text.erase(prior, caret - prior);
    check(text == "Aéر", "UTF-8 backspace range removes one complete code point");
}

void disconnected_installations_remain_installed(const std::filesystem::path& fixtures)
{
    auto state = fixture_state(fixtures);
    state.library.scope = "installed";
    const auto count = filtered_games(state).size();
    for (auto& installation : state.installations) installation.available = false;
    check(filtered_games(state).size() == count && count == 5,
          "disconnect changes availability without removing installed identities");
    const auto projected = shelves(state);
    const auto recent = std::find_if(projected.begin(), projected.end(), [](const Shelf& shelf) {
        return shelf.key == "installed";
    });
    check(recent != projected.end() && recent->game_ids.size() == 5,
          "Recently installed keeps disconnected games visible");
}

// Recently installed lists the newest install first, by the time each game was installed (a move keeps it),
// whatever order the installations come in; games without a time follow.
void recently_installed_follows_install_times(const std::filesystem::path& fixtures)
{
    auto state = fixture_state(fixtures);
    check(state.installations.size() >= 3, "fixture has installations");
    const auto first = state.installations[0].game_id, second = state.installations[1].game_id,
               third = state.installations[2].game_id;
    state.installations[0].installed_at = 300;
    state.installations[1].installed_at = 100;
    state.installations[2].installed_at = 200;
    const auto projected = shelves(state);
    const auto recent = std::find_if(projected.begin(), projected.end(), [](const Shelf& shelf) {
        return shelf.key == "installed";
    });
    check(recent != projected.end() && recent->game_ids.size() >= 3 && recent->game_ids[0] == first &&
              recent->game_ids[1] == third && recent->game_ids[2] == second,
          "Recently installed goes by install time, newest first");
}

void live_unicode_query() {
    State state;
    state.simulation_mode=false;
    const auto scopes=library_rail_scopes(state);
    check(scopes.front()=="installed" && scopes[1]=="all","live scopes offer Installed now that installs are real");
    state.account().authentication=AuthState::SignedIn;
    for(const auto& [id,title]:std::vector<std::pair<playden::GameId,std::string>>{
        {SteamId(9),"Straße"},{SteamId(8),"STRASSE"},{SteamId(7),"CAFÉ: Beyond"},{SteamId(6),"Αθήνα"},{SteamId(5),"東京物語"},{SteamId(4),std::string("bad\xff",4)}}) {
        Game game;game.id=id;game.title=title;state.games.push_back(game);
    }
    state.library.query="strasse";
    auto result=filtered_games(state);
    check(result.size()==2 && result[0]->id==SteamId(8) && result[1]->id==SteamId(9),"live Unicode folding expands sharp s with deterministic ID ties");
    state.library.query="café:";
    check(filtered_games(state).size()==1 && filtered_games(state)[0]->id==SteamId(7),"live accented case folding preserves punctuation");
    state.library.query="ΑΘΉΝΑ";
    check(filtered_games(state).size()==1 && filtered_games(state)[0]->id==SteamId(6),"live Greek case folding");
    state.library.query="東京";
    check(filtered_games(state).size()==1,"live exact CJK substring");
    state.library.query=std::string("bad\xff",4);
    check(filtered_games(state).size()==1,"malformed input matches byte-exactly");
    state.library.query=" strasse  ";
    check(filtered_games(state).size()==2,"live query ignores spaces around it");
    state.library.query="\xC2\xA0strasse\xE3\x80\x80";
    check(filtered_games(state).size()==2,"live query ignores Unicode spaces around it");
    state.library.query="  ";
    check(filtered_games(state).size()==6,"live blank query matches every record");
    state.library.query="no match";
    check(filtered_games(state).empty(),"live empty result");
    state.library.query.clear();
    check(filtered_games(state).size()==6,"clearing live query restores records");
}

// The rail's "Imported" scope shows only once the library has an imported game, in the store section after
// Steam, and lists only the imported games.
void imported_scope_lists_local_games(const std::filesystem::path& fixtures)
{
    State state = fixture_state(fixtures);
    state.navigation.route = Route::Library;
    auto scopes = library_rail_scopes(state);
    check(std::find(scopes.begin(), scopes.end(), "imported") == scopes.end() && !has_imported_games(state),
          "no Imported scope without imported games");
    Game imported;
    imported.id = playden::GameId::Local("0f8fad5b-d9cb-469f-a165-70867728950e");
    imported.title = "Night Sky";
    state.games.push_back(imported);
    scopes = library_rail_scopes(state);
    check(has_imported_games(state) && std::vector<std::string>(scopes.begin() + 4, scopes.begin() + 6) ==
                                           std::vector<std::string>{"steam", "imported"},
          "the Imported scope follows Steam's, after Hidden");
    state.library.scope = "imported";
    const auto games = filtered_games(state);
    check(games.size() == 1 && games.front()->id == imported.id, "the Imported scope lists only imported games");
    state.library.scope = "all";
    check(filtered_games(state).size() > 1, "All still lists every game");
}

// Signed out with only imported games playing from their folders, Home shows them under Recently
// installed, newest first, and the Installed scope lists them.
void imported_games_fill_home_signed_out()
{
    State state;
    state.simulation_mode = false;
    state.account().authentication = AuthState::SignedOut;
    const auto add = [&state](const char* uuid, const char* title, std::int64_t added) {
        Game game;
        game.id = playden::GameId::Local(uuid);
        game.title = title;
        game.added_at = added;
        state.games.push_back(game);
        state.external_games.push_back({game.id, "usb0", true, ""});
        return game.id;
    };
    const auto older = add("0f8fad5b-d9cb-469f-a165-70867728950e", "Bastion", 100);
    const auto newer = add("7c9e6679-7425-40de-944b-e07fc1f90ae7", "GRIS", 200);
    const auto projected = shelves(state);
    const auto recent = std::find_if(projected.begin(), projected.end(), [](const Shelf& shelf) {
        return shelf.key == "installed";
    });
    check(recent != projected.end() && recent->game_ids == std::vector<GameId>{newer, older},
          "Recently installed lists imported games newest first");
    state.library.scope = "installed";
    check(filtered_games(state).size() == 2, "the Installed scope lists imported games playing in place");
}

} // namespace

void continue_playing_ends_in_more(const std::filesystem::path& fixtures)
{
    State state = fixture_state(fixtures);
    // Make 30 played games: Continue playing keeps the 20 most recent and a "More" cell.
    const Game base = state.games.front();
    state.games.clear();
    for (int index = 0; index < 30; ++index)
    {
        Game game = base;
        game.id = static_cast<GameId>(SteamId(500000 + index));
        game.title = "Played " + std::to_string(index);
        game.playtime_minutes = 10;
        game.last_played = 1000 + index;
        state.games.push_back(game);
    }
    state.library = {};
    const auto projected = shelves(state);
    check(!projected.empty() && projected[0].key == "continue", "Continue playing is the first shelf");
    const Shelf& continued = projected[0];
    check(continued.game_ids.size() == kShelfLimit && continued.more == 10 && shelf_cells(continued) == 21,
          "Continue playing shows 20 games and counts the rest");
    check(continued.game_ids.front() == SteamId(500029), "the most recently played game comes first");
    check(shelf_more_key(continued) == "shelf-continue-more", "the More cell has a stable focus key");

    state.navigation.route = Route::Home;
    state.navigation.home_shelf = 0;
    state.navigation.home_shelf_focus["continue"] = 19;
    move_browser(state, 1, 0);
    check(state.navigation.home_shelf_focus["continue"] == 20 && !state.navigation.selected_game_id.valid() &&
              state.navigation.focus_key == "shelf-continue-more",
          "Right after the 20th game focuses More, with no game selected");
    move_browser(state, 1, 0);
    check(state.navigation.home_shelf_focus["continue"] == 20, "More is the end of the shelf");
    move_browser(state, -1, 0);
    check(state.navigation.selected_game_id == continued.game_ids.back(), "Left from More returns to the last game");

    state.games.resize(20);
    const auto exact = shelves(state);
    check(exact[0].game_ids.size() == 20 && exact[0].more == 0 && shelf_cells(exact[0]) == 20,
          "a shelf of exactly 20 games has no More cell");
}

// The rail's "GOG" scope shows only once the library has a GOG game, between Steam and Imported, and lists only the
// GOG games; the rail reaches it.
void gog_scope_lists_gog_games(const std::filesystem::path& fixtures)
{
    State state = fixture_state(fixtures);
    state.navigation.route = Route::Library;
    auto scopes = library_rail_scopes(state);
    check(std::find(scopes.begin(), scopes.end(), "gog") == scopes.end() && !has_gog_games(state),
          "no GOG scope without GOG games");
    Game gog;
    gog.id = playden::GameId::Gog(1000000001);
    gog.title = "Synthetic GOG game";
    state.games.push_back(gog);
    scopes = library_rail_scopes(state);
    check(has_gog_games(state) && std::vector<std::string>(scopes.begin() + 4, scopes.begin() + 6) ==
                                      std::vector<std::string>{"steam", "gog"},
          "without imported games, Steam and GOG follow Hidden");
    Game imported;
    imported.id = playden::GameId::Local("0f8fad5b-d9cb-469f-a165-70867728950e");
    imported.title = "Night Sky";
    state.games.push_back(imported);
    scopes = library_rail_scopes(state);
    check(std::vector<std::string>(scopes.begin() + 4, scopes.begin() + 7) ==
              std::vector<std::string>{"steam", "gog", "imported"},
          "GOG comes between Steam and Imported");
    state.library.scope = "gog";
    const auto games = filtered_games(state);
    check(games.size() == 1 && games.front()->id == gog.id, "the GOG scope lists only GOG games");
    state.library.scope = "imported";
    check(filtered_games(state).size() == 1 && filtered_games(state).front()->id == imported.id,
          "the Imported scope doesn't list GOG games");
    // The rail steps onto it like any other scope.
    state.library.scope = "all";
    state.navigation.library_rail_focused = false;
    move_browser(state, -1, 0);
    for (int step = 0; step < 10 && state.navigation.focus_key != "rail-gog"; ++step) move_browser(state, 0, 1);
    check(state.navigation.library_rail_focused && state.navigation.focus_key == "rail-gog", "the rail reaches GOG");
}

// The rail's store section: Installed, All, Favorites, Hidden, a divider, Steam, GOG and Imported (each while it has
// games), a divider, then the collections and New collection. It shows only while the games come from more than one
// store; the dividers are not entries, so Up and Down step from Hidden straight onto Steam and from the last store onto
// the first collection.
void rail_store_section(const std::filesystem::path& fixtures)
{
    State state = fixture_state(fixtures);
    state.navigation.route = Route::Library;
    state.library.collections = {{"collection-1", "Weekend", false, {}}};
    check(library_store_scopes(state).empty() && library_rail_dividers(state).empty() &&
              library_rail_scopes(state) ==
                  std::vector<std::string>{"installed", "all", "favorites", "hidden", "collection-1", "new-collection"},
          "Steam's games only: the rail is as before, without a Steam entry or dividers");

    Game gog;
    gog.id = playden::GameId::Gog(1000000001);
    gog.title = "Synthetic GOG game";
    state.games.push_back(gog);
    Game imported;
    imported.id = playden::GameId::Local("0f8fad5b-d9cb-469f-a165-70867728950e");
    imported.title = "Night Sky";
    state.games.push_back(imported);
    const auto scopes = library_rail_scopes(state);
    check(scopes == std::vector<std::string>{"installed", "all", "favorites", "hidden", "steam", "gog", "imported",
                                             "collection-1", "new-collection"},
          "three stores: Steam, GOG and Imported between the built-in scopes and the collections");
    check(library_rail_dividers(state) == std::vector<int>{4, 7}, "a divider before Steam and one before the collections");

    // Down from Hidden lands on Steam, and Down from Imported on the collection: the dividers take no step.
    state.library.scope = "hidden";
    state.library.filters.insert("empty-fixture");
    move_browser(state, -1, 0);
    check(state.navigation.library_rail_focused && state.navigation.focus_key == "rail-hidden" &&
              state.navigation.selected_index == 3,
          "the rail opens on Hidden");
    const std::vector<std::string> expected{"rail-steam", "rail-gog", "rail-imported", "rail-collection-1",
                                            "rail-new-collection"};
    for (std::size_t step = 0; step < expected.size(); ++step)
    {
        move_browser(state, 0, 1);
        check(state.navigation.focus_key == expected[step] &&
                  state.navigation.selected_index == static_cast<int>(step) + 4,
              "Down steps over the dividers onto the next entry");
    }
    for (int step = 0; step < 4; ++step) move_browser(state, 0, -1);
    check(state.navigation.focus_key == "rail-steam" && state.navigation.selected_index == 4,
          "Up comes back onto Steam");
    move_browser(state, 0, -1);
    check(state.navigation.focus_key == "rail-hidden", "and Up from Steam onto Hidden");
    state.library.filters.erase("empty-fixture");

    // The Steam scope lists only Steam's games.
    state.library.scope = "all";
    const std::size_t every = filtered_games(state).size();
    state.library.scope = "steam";
    const auto steam = filtered_games(state);
    check(!steam.empty() && steam.size() + 2 == every && std::all_of(steam.begin(), steam.end(), [](const Game* game) {
              return game->id.store == playden::Store::kSteam;
          }),
          "the Steam scope lists only Steam's games");
    check(library_rail_marked_scope(state) == "steam", "the rail marks Steam");

    // Two stores: only theirs show, still between dividers.
    state.games.pop_back();
    check(library_rail_scopes(state) == std::vector<std::string>{"installed", "all", "favorites", "hidden", "steam", "gog",
                                                                 "collection-1", "new-collection"} &&
              library_rail_dividers(state) == std::vector<int>{4, 6},
          "Steam and GOG: two store entries");

    // A store scope whose store left keeps its scope (stores load one at a time): its view is empty and the rail marks
    // nothing. A scope whose store is the only one left lists every game, so the rail marks All.
    state.library.scope = "imported";
    check(filtered_games(state).empty() && library_rail_marked_scope(state) == "imported",
          "Imported without imported games: an empty view, nothing marked");
    std::erase_if(state.games, [](const Game& game) { return game.id.store == playden::Store::kGog; });
    state.library.scope = "steam";
    check(library_store_scopes(state).empty() && library_rail_marked_scope(state) == "all" &&
              filtered_games(state).size() == state.games.size() - state.library.hidden.size(),
          "Steam alone: the Steam scope lists every game and the rail marks All");
    state.navigation.library_rail_focused = false;
    state.navigation.selected_index = 0;
    state.navigation.selected_game_id = filtered_games(state).front()->id;
    move_browser(state, -1, 0);
    check(state.navigation.focus_key == "rail-all", "left from the grid enters the rail on All");
}

// The Store filter ("store-<store>") lists one store's games, with the scope and the other filters. The sheet offers the
// stores the rail's section shows, plus the one picked while it is alone or has no games; a store this build doesn't
// know is Any.
void store_filter_narrows_the_library(const std::filesystem::path& fixtures)
{
    State state = fixture_state(fixtures);
    check(library_filter_stores(state).empty() && library_store_filter(state).empty(),
          "Steam's games only: no Store group, Any");
    state.library.filters.insert("store-steam");
    check(library_filter_stores(state) == std::vector<std::string>{"steam"}, "a picked store stays listed alone");
    state.library.filters.erase("store-steam");

    Game gog;
    gog.id = playden::GameId::Gog(1000000001);
    gog.title = "Synthetic GOG game";
    state.games.push_back(gog);
    check(library_filter_stores(state) == std::vector<std::string>{"steam", "gog"}, "Steam and GOG");
    const std::size_t every = filtered_games(state).size();

    state.library.filters.insert("store-gog");
    check(library_store_filter(state) == "gog" && filtered_games(state).size() == 1 &&
              filtered_games(state).front()->id == gog.id,
          "the GOG filter lists only GOG's games");
    check(library_filter_stores(state) == std::vector<std::string>{"steam", "gog"}, "the list doesn't change");
    state.library.scope = "steam";
    check(filtered_games(state).empty(), "it combines with the scope");
    state.library.scope = "all";
    state.library.filters = {"store-steam"};
    check(filtered_games(state).size() + 1 == every, "the Steam filter leaves GOG out");
    state.library.filters = {"store-imported"};
    check(filtered_games(state).empty() &&
              library_filter_stores(state) == std::vector<std::string>{"steam", "gog", "imported"},
          "Imported without imported games: nothing, and Imported stays listed");
    state.library.filters = {"store-epic"};
    check(filtered_games(state).size() == every && library_filter_stores(state) == std::vector<std::string>{"steam", "gog"},
          "a store this build doesn't know is Any");
}

// Steam collections and GOG tags (#536): a store with collections is a folder in the rail that Cross opens, with
// "All <store> games" first and its collections in name order; one store open at a time; Left closes it.
void rail_store_collections(const std::filesystem::path& fixtures)
{
    using playden::GameId;
    using playden::Store;
    State state = fixture_state(fixtures);
    state.navigation.route = Route::Library;
    state.library.collections = {{"collection-1", "Weekend", false, {}}};
    Game gog;
    gog.id = GameId::Gog(1000000001);
    gog.title = "Synthetic GOG game";
    state.games.push_back(gog);
    Game imported;
    imported.id = GameId::Local("0f8fad5b-d9cb-469f-a165-70867728950e");
    imported.title = "Night Sky";
    state.games.push_back(imported);
    const GameId hidden = *state.library.hidden.begin();
    // Members: two library games, the hidden one, and an app the library doesn't have.
    state.store_collections = {
        {Store::kSteam, "uc-b", "beta", {SteamId(900001), SteamId(900002), hidden, SteamId(4242)}},
        {Store::kSteam, "uc-a", "Alpha", {SteamId(900003)}},
        {Store::kSteam, "uc-c", "\xC3\x81gua", {SteamId(900002)}},
        // Only apps the library doesn't have (a Steam ROM Manager list of non-Steam shortcuts): not in the rail.
        {Store::kSteam, "srm-d", "Emulators", {SteamId(4242)}},
        {Store::kSteam, "uc-e", "Empty", {}},
    };
    state.store_collections_known = {Store::kSteam, Store::kGog};

    state.navigation.library_rail_open_store = std::string();
    check(library_rail_scopes(state) == std::vector<std::string>{"installed", "all", "favorites", "hidden", "store-steam",
                                                                 "gog", "imported", "collection-1", "new-collection"},
          "closed: Steam is a folder entry, GOG without tags stays its scope, Imported never opens");
    check(library_rail_dividers(state) == std::vector<int>{4, 7}, "closed: dividers as before");

    state.navigation.library_rail_open_store = "steam";
    check(library_rail_scopes(state) ==
              std::vector<std::string>{"installed", "all", "favorites", "hidden", "store-steam", "steam",
                                       "steam-collection:uc-a", "steam-collection:uc-b", "steam-collection:uc-c", "gog",
                                       "imported", "collection-1", "new-collection"},
          "open: All Steam games first, then the collections in case-folded name order (as game titles sort)");
    check(library_rail_dividers(state) == std::vector<int>{4, 11}, "open: the second divider follows the open rows");
    state.library.scope = "steam-collection:uc-e";
    check(library_rail_scopes(state) ==
              std::vector<std::string>{"installed", "all", "favorites", "hidden", "store-steam", "steam",
                                       "steam-collection:uc-a", "steam-collection:uc-b", "steam-collection:uc-e",
                                       "steam-collection:uc-c", "gog", "imported", "collection-1", "new-collection"},
          "a chosen collection without library games keeps its row");
    state.library.scope = "all";
    const auto steam_only = state.store_collections;
    state.store_collections = {steam_only[3], steam_only[4]};
    check(library_rail_scopes(state) == std::vector<std::string>{"installed", "all", "favorites", "hidden", "steam", "gog",
                                                                 "imported", "collection-1", "new-collection"},
          "a store whose collections have no library games is no folder");
    state.store_collections = steam_only;

    state.store_collections.push_back({Store::kGog, "7", "Classics", {gog.id}});
    state.navigation.library_rail_open_store = "gog";
    check(library_rail_scopes(state) == std::vector<std::string>{"installed", "all", "favorites", "hidden", "store-steam",
                                                                 "store-gog", "gog", "gog-tag:7", "imported",
                                                                 "collection-1", "new-collection"},
          "one store open at a time");

    // Filtering: the members in the library, hidden ones left out as for every scope but Hidden.
    state.library.scope = "steam-collection:uc-b";
    auto games = filtered_games(state);
    check(games.size() == 2 && std::none_of(games.begin(), games.end(), [&](const Game* game) { return game->id == hidden; }),
          "a store collection lists its library games, not the hidden one or an unknown app");
    check(store_collection_count(state, state.store_collections[0]) == 3, "its count includes the hidden game");
    state.library.scope = "gog-tag:7";
    check(filtered_games(state).size() == 1, "a GOG tag lists its games");
    state.library.scope = "steam-collection:uc-missing";
    check(filtered_games(state).empty(), "a store collection the store hasn't given lists nothing");

    // Unset, the store of the chosen scope opens; marked: the scope's row, or the closed store's folder.
    state.library.scope = "steam-collection:uc-a";
    state.navigation.library_rail_open_store.reset();
    check(library_rail_open_store(state) == "steam" && library_rail_marked_scope(state) == "steam-collection:uc-a",
          "on loading with a Steam collection chosen, Steam is open and the collection marked");
    state.navigation.library_rail_open_store = std::string();
    check(library_rail_marked_scope(state) == "store-steam", "with Steam closed, its folder is marked");
    state.library.scope = "steam";
    check(library_rail_marked_scope(state) == "store-steam", "the saved Steam scope marks the closed folder");
    state.navigation.library_rail_open_store.reset();
    check(library_rail_marked_scope(state) == "steam", "and its All Steam games row once open");
    state.library.scope = "all";
    check(library_rail_open_store(state).empty(), "All opens no store");

    // Left: closes the open store from its folder or a row inside it; elsewhere it stays.
    state.library.filters.insert("empty-fixture");
    state.navigation.library_rail_focused = true;
    state.navigation.library_rail_open_store = "steam";
    check(focus_library_rail_entry(state, "steam-collection:uc-c"), "a Steam collection takes the focus");
    move_browser(state, -1, 0);
    check(library_rail_open_store(state).empty() && state.navigation.focus_key == "rail-store-steam" &&
              state.navigation.selected_index == 4,
          "Left in an open store closes it and focuses the store");
    state.navigation.library_rail_open_store = "steam";
    move_browser(state, -1, 0);
    check(library_rail_open_store(state).empty() && state.navigation.focus_key == "rail-store-steam",
          "Left on the open store closes it");
    focus_library_rail_entry(state, "hidden");
    move_browser(state, -1, 0);
    check(state.navigation.focus_key == "rail-hidden" && state.navigation.library_rail_focused, "Left on Hidden stays");
    state.library.filters.erase("empty-fixture");

    // Left from the grid's first column: the shown collection's own row, its store open again.
    state.library.scope = "steam-collection:uc-b";
    state.navigation.library_rail_open_store = std::string();
    state.navigation.library_rail_focused = false;
    games = filtered_games(state);
    state.navigation.selected_game_id = games.front()->id;
    state.navigation.selected_index = 0;
    move_browser(state, -1, 0);
    check(state.navigation.library_rail_focused && library_rail_open_store(state) == "steam" &&
              state.navigation.focus_key == "rail-steam-collection:uc-b" && state.navigation.selected_index == 7,
          "Left from the grid returns to the open collection");

    // The focused collection is deleted (a refresh): the focus moves to its store, never stays on a missing entry.
    state.navigation.library_rail_focused = true;
    check(focus_library_rail_entry(state, "steam-collection:uc-c"), "Água takes the focus");
    const auto kept = state.store_collections;
    std::erase_if(state.store_collections, [](const StoreCollection& value) { return value.id == "uc-c"; });
    reconcile_library_rail_focus(state);
    check(state.navigation.focus_key == "rail-steam" || state.navigation.focus_key == "rail-store-steam",
          "the focus leaves the deleted collection for its store");
    const auto keys_now = library_rail_scopes(state);
    check(keys_now[static_cast<std::size_t>(state.navigation.selected_index)] == state.navigation.focus_key.substr(5),
          "selected_index follows the focus key");
    state.store_collections = kept;
    state.navigation.library_rail_focused = false;

    // A deleted collection falls back to its store once the store's list is known.
    state.library.scope = "steam-collection:uc-gone";
    state.store_collections_known = {};
    check(!reconcile_store_collection_scope(state) && state.library.scope == "steam-collection:uc-gone",
          "the list not known yet: the scope waits");
    state.store_collections_known = {Store::kSteam};
    check(reconcile_store_collection_scope(state) && state.library.scope == "steam",
          "known and gone: the scope becomes the store's games");

    // 2A: Steam has every game; its collections get a lone folder with no "All" row.
    std::erase_if(state.games, [](const Game& game) { return game.id.store != Store::kSteam; });
    state.library.scope = "all";
    state.navigation.library_rail_open_store = std::string();
    check(library_rail_scopes(state) == std::vector<std::string>{"installed", "all", "favorites", "hidden", "store-steam",
                                                                 "collection-1", "new-collection"} &&
              library_rail_dividers(state) == std::vector<int>{4, 5},
          "one store: a Steam collections folder between dividers");
    state.navigation.library_rail_open_store = "steam";
    check(library_rail_scopes(state) == std::vector<std::string>{"installed", "all", "favorites", "hidden", "store-steam",
                                                                 "steam-collection:uc-a", "steam-collection:uc-b",
                                                                 "steam-collection:uc-c", "collection-1",
                                                                 "new-collection"},
          "open: its collections only, no All Steam games row");
    state.library.scope = "steam";
    check(library_rail_marked_scope(state) == "all", "a saved Steam scope with only Steam games still marks All");
    state.library.scope = "steam-collection:uc-gone";
    check(reconcile_store_collection_scope(state) && state.library.scope == "steam",
          "a deleted collection falls back to Steam's games (All's games) in 2A too");

    // Without store collections, the rail is exactly today's.
    state.store_collections.clear();
    state.navigation.library_rail_open_store.reset();
    state.library.scope = "all";
    check(library_rail_scopes(state) ==
                  std::vector<std::string>{"installed", "all", "favorites", "hidden", "collection-1", "new-collection"} &&
              library_rail_dividers(state).empty(),
          "no store collections: the rail is unchanged");
}

int main(int argc, char** argv)
{
    check(argc == 2, "fixture directory argument is required");
    const std::filesystem::path fixtures(argv[1]);
    live_unicode_query();
    filtering_uses_real_fixture(fixtures);
    all_sorts_keep_unknown_values_last(fixtures);
    shelves_are_nonempty_and_restore_children(fixtures);
    continue_playing_ends_in_more(fixtures);
    library_grid_and_rail_edges(fixtures);
    download_window_keeps_late_jobs_visible(fixtures);
    utf8_caret_boundaries();
    disconnected_installations_remain_installed(fixtures);
    recently_installed_follows_install_times(fixtures);
    imported_scope_lists_local_games(fixtures);
    gog_scope_lists_gog_games(fixtures);
    rail_store_section(fixtures);
    rail_store_collections(fixtures);
    store_filter_narrows_the_library(fixtures);
    imported_games_fill_home_signed_out();
    std::cout << "homebrew browse tests passed\n";
    return 0;
}
