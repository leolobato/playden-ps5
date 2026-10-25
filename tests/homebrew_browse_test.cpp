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
    state.library.query="no match";
    check(filtered_games(state).empty(),"live empty result");
    state.library.query.clear();
    check(filtered_games(state).size()==6,"clearing live query restores records");
}

// The rail's "Imported" scope shows only once the library has an imported game, after Hidden, and
// lists only the imported games.
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
    const auto hidden = std::find(scopes.begin(), scopes.end(), "hidden");
    check(has_imported_games(state) && hidden != scopes.end() && hidden + 1 != scopes.end() && *(hidden + 1) == "imported",
          "the Imported scope follows Hidden");
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
    imported_scope_lists_local_games(fixtures);
    imported_games_fill_home_signed_out();
    std::cout << "homebrew browse tests passed\n";
    return 0;
}
