// Controller-only traversal catches actions exposed only through replay setters.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: MIT
#include "homebrew/simulation.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <unistd.h>

using namespace playden::homebrew;
static void require(bool okay, const char* message) {
    if (!okay) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
static void press(Simulation& simulation, const char* key) {
    require(simulation.action(key), key);
}
// Settings: Up and Down stay in a section; another section is reached through the rail.
static void enter_settings_section(playden::homebrew::Simulation& model, int index) {
  using playden::homebrew::Route;
  if (model.state().navigation.route != Route::Settings || !model.state().navigation.modals.empty()) return;
  const auto& rows = model.view().rows;
  const std::string wanted = rows[static_cast<std::size_t>(index)].section;
  const auto here = [&] { return rows[static_cast<std::size_t>(model.view().focused_row)].section; };
  if (here() == wanted) return;
  std::vector<std::string> order;
  for (const auto& row : rows) if (order.empty() || order.back() != row.section) order.push_back(row.section);
  const auto position = [&](const std::string& section) { return std::find(order.begin(), order.end(), section) - order.begin(); };
  model.action("left");
  for (int step = 0; step < 16 && here() != wanted; ++step) model.action(position(here()) < position(wanted) ? "down" : "up");
  model.action("right");
}
static void choose(Simulation& simulation, const std::string& key) {
    const auto& rows = simulation.view().rows;
    const auto target = std::find_if(rows.begin(), rows.end(), [&](const ViewRow& row) { return row.key == key; });
    if (target == rows.end()) { std::fprintf(stderr, "Unreachable controller row %s in %s\n", key.c_str(), simulation.view().surface.c_str()); std::exit(1); }
    const int index = static_cast<int>(std::distance(rows.begin(), target));
    enter_settings_section(simulation, index);
    const bool details_row = simulation.state().navigation.route == Route::Details &&
                             simulation.state().navigation.modals.empty();
    const auto tag = [](const std::string& value) {
        return value.rfind("detail-tag-", 0) == 0 || value == "detail-tags-more";
    };
    if (details_row && tag(key) != tag(simulation.state().navigation.focus_key))
        press(simulation, tag(key) ? "down" : "up");
    for (int count = 0; count < 100 && simulation.view().focused_row != index; ++count)
        press(simulation, simulation.view().focused_row < index
                              ? (details_row ? "right" : "down")
                              : (details_row ? "left" : "up"));
    require(simulation.state().navigation.focus_key == key, "navigation reaches requested visible row");
    press(simulation, "cross");
}

int main(int argc, char** argv) {
    require(argc == 2, "fixture argument");
    const auto profile = std::filesystem::temp_directory_path() / ("playden-hui-controller-tour-test-" + std::to_string(getpid()) + ".json");
    std::error_code error;
    std::filesystem::remove(profile, error);
    Simulation simulation(argv[1], profile);
    simulation.scenario("first-run");
    choose(simulation, "startup-connect");
    choose(simulation, "qr-fixture-controls");
    choose(simulation, "inject-qr-expired");
    require(simulation.state().account().authentication == AuthState::Expired, "controller can expire QR challenge");
    choose(simulation, "qr-refresh");
    choose(simulation, "qr-fixture-controls");
    choose(simulation, "inject-qr-approved");
    choose(simulation, "sync-advance");
    require(simulation.state().navigation.route == Route::Home, "controller completes QR approval and sync");
    // Enter a Home shelf, visit details, then restore the actual shelf child.
    press(simulation, "right");
    const auto home_id = simulation.state().navigation.selected_game_id;
    const auto shelf = simulation.state().navigation.home_shelf;
    press(simulation, "cross");
    press(simulation, "circle");
    require(simulation.state().navigation.route == Route::Home &&
            simulation.state().navigation.selected_game_id == home_id &&
            simulation.state().navigation.home_shelf == shelf, "Home detail return restores shelf identity");
    press(simulation, "r1");
    press(simulation, "cross");
    const auto game = simulation.state().navigation.selected_game_id;
    choose(simulation, "details-more");
    choose(simulation, "game-settings");
    choose(simulation, "setting-extra_arguments");
    simulation.text("é星ر");
    press(simulation, "square");
    require(simulation.state().editor.value == "é星", "controller backspace preserves complete UTF-8 codepoints");
    press(simulation, "circle");
    require(simulation.state().navigation.focus_key == "setting-extra_arguments", "keyboard Cancel restores exact modal opener");
    press(simulation, "circle");
    require(!simulation.state().navigation.modals.empty() && simulation.state().navigation.modals.back().id == "context" &&
            simulation.state().navigation.focus_key == "game-settings", "Circle on Game settings returns to Game actions");
    choose(simulation, "note");
    choose(simulation, "rating-playable");
    require(simulation.state().library.ratings.at(game) == Compatibility::Playable, "rating reachable through controller");
    require(simulation.state().navigation.focus_key == "note", "rating picker restores its context opener");
    choose(simulation, "collection");
    choose(simulation, "collection-toggle-collection-1");
    choose(simulation, "collection-manage");
    choose(simulation, "collection-pin-collection-1");
    choose(simulation, "collection-delete-collection-1");
    require(simulation.state().navigation.focus_key == "cancel", "collection deletion defaults to Cancel");
    choose(simulation, "confirm-destructive");
    require(simulation.state().games.size() == 24 && simulation.state().library.collections.empty(),
            "controller deletes only collection organization");
    require(!simulation.state().navigation.modals.empty() && simulation.state().navigation.modals.back().id == "collections-manage",
            "deleting returns to the collection sheets, which list what is left");
    // Leave stacked menus and reach account destruction through normal sections.
    while (!simulation.state().navigation.modals.empty()) press(simulation, "circle");
    press(simulation, "circle");
    press(simulation, "r1");
    press(simulation, "r1");
    press(simulation, "r1");
    choose(simulation, "settings-signout");
    choose(simulation, "confirm-destructive");
    Simulation restored(argv[1], profile);
    require(restored.state().account().authentication == AuthState::SignedOut &&
            restored.visible_games().empty(), "controller sign-out remains signed out after restart");
    require(restored.state().account().id.empty() && restored.state().account().display_name.empty(),
            "sign-out removes visible identity on restart");
    simulation.scenario("default");
    press(simulation, "l1");
    choose(simulation, "settings-logs");
    require(simulation.state().log_state == LogState::Loading, "logs visibly start Loading");
    choose(simulation, "logs-fixture-controls");
    choose(simulation, "inject-logs-failure");
    require(simulation.view().surface == "logs" && simulation.state().log_state == LogState::Failed,
            "controller injects and displays log failure");
    choose(simulation, "logs-retry");
    choose(simulation, "logs-fixture-controls");
    choose(simulation, "inject-logs-empty");
    require(simulation.state().log_lines.empty() && simulation.state().log_state == LogState::Ready,
            "controller can inspect empty logs");
    choose(simulation, "logs-fixture-controls");
    choose(simulation, "inject-logs-ready");
    require(simulation.state().log_lines.size() <= 128, "logs have a hard record bound");
    press(simulation, "r2");
    require(simulation.state().log_offset > 0, "controller pages bounded log records");
    press(simulation, "circle");
    require(simulation.state().navigation.focus_key == "settings-logs", "Logs restores Settings opener");
    simulation.scenario("default");
    press(simulation, "cross");
    choose(simulation, "details-simulate-launch");
    choose(simulation, "session-fixture-controls");
    choose(simulation, "advance-clock");
    choose(simulation, "advance-clock");
    press(simulation, "circle");
    require(simulation.state().session == SessionState::Running, "controller advances preparation to running");
    choose(simulation, "session-quit");
    require(simulation.state().navigation.focus_key == "cancel", "Quit game defaults to Cancel");
    choose(simulation, "confirm-destructive");
    require(simulation.state().session == SessionState::Stopping, "controller quit visibly stops");
    choose(simulation, "session-fixture-controls");
    choose(simulation, "inject-stop-failure");
    choose(simulation, "session-force-stop");
    require(simulation.state().navigation.focus_key == "cancel", "force-stop defaults to Cancel");
    choose(simulation, "confirm-destructive");
    require(simulation.state().session == SessionState::Idle, "controller can recover failed stop");
    simulation.scenario("stress");
    for (int page = 0; page < 50; ++page) {
        press(simulation, "r2");
        require(simulation.view().rows.size() <= 24, "controller traversal materializes at most 24 games");
        const auto selected_id = simulation.state().navigation.selected_game_id;
        simulation.tick(1);
        require(simulation.state().navigation.selected_game_id == selected_id,
                "job updates preserve focus during stress traversal");
        require(std::any_of(simulation.view().rows.begin(), simulation.view().rows.end(),
                [selected_id](const ViewRow& row) { return row.game_id == selected_id; }),
                "the bounded presentation window contains the selected game");
    }
    require(simulation.state().navigation.selected_index == 996,
            "controller paging reaches the final row and preserves its column");
    press(simulation, "right"); press(simulation, "right"); press(simulation, "right");
    require(simulation.state().navigation.selected_index == 999,
            "controller traverses the final row to the 1,000th game");
    std::filesystem::remove(profile, error);
    std::puts("PASS: QR, Home return, UTF-8/editor opener, organization, sign-out, bounded log recovery, failed session stop");
}
