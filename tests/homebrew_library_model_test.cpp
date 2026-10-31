// homebrew_library_model_test.cpp - Real library projection tests.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: MIT

#include "homebrew/queue_layout.hpp"
#include "homebrew/simulation.hpp"
#include "homebrew/store_art.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <vector>
#include <unistd.h>
#include "steam_id.h"

namespace {

using playden::GameSummary;
using playden::homebrew::AuthState;
using playden::homebrew::CloudRequestKind;
using playden::homebrew::CloudStatus;
using playden::homebrew::LibraryUpdate;
using playden::homebrew::ModelMode;
using playden::homebrew::Route;
using playden::homebrew::RowKind;
using playden::homebrew::ServiceAction;
using playden::homebrew::Simulation;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

Simulation make_live() {
    return Simulation("fixtures-not-used", "profile-not-used.json", ModelMode::Live);
}


GameSummary game(playden::GameId id, std::string title, std::string art = {},
                 std::string description = {}) {
    GameSummary result;
    result.id = id;
    result.title = std::move(title);
    result.artwork_source = std::move(art);
    result.description = std::move(description);
    return result;
}

const playden::homebrew::Game* find_game(const Simulation& model, playden::GameId id) {
    const auto found = std::find_if(model.state().games.begin(), model.state().games.end(),
                                    [id](const auto& item) { return item.id == id; });
    return found == model.state().games.end() ? nullptr : &*found;
}

bool has_row(const Simulation& model, const std::string& key) {
    return std::any_of(model.view().rows.begin(), model.view().rows.end(),
                       [&key](const auto& row) { return row.key == key; });
}

bool has_legend(const Simulation& model, const std::string& legend) {
    const auto& legends = model.view().legends;
    return std::find(legends.begin(), legends.end(), legend) != legends.end();
}

void activate(Simulation& model, const std::string& key) {
    check(model.action("focus:" + key), "requested row exists");
    check(model.action("cross"), "requested row activates");
}

void projects_only_owned_facts_and_delivery_status() {
    auto model = make_live();
    model.UpdateAccount({AuthState::SignedIn, "account", "Owner", {}, {}, false});

    auto summary = game(SteamId(10), "Known title", "https://cdn.invalid/10.jpg", "Known description");
    summary.developer = "Not projected by this boundary";
    summary.playtime_minutes = 91;
    summary.last_played_unix_seconds = 4'102'444'800ULL;
    check(model.UpdateLibrary({{summary}, false, {}, false, true, 1234}),
          "valid successful live snapshot is accepted");

    const auto* projected = find_game(model, SteamId(10));
    check(projected != nullptr && projected->title == "Known title"
              && projected->description == "Known description"
              && projected->source == "Steam"
              && projected->artwork_source == "https://cdn.invalid/10.jpg",
          "shared identity, text, artwork and known storefront are projected");
    check(projected->playtime_minutes == 91 && projected->last_played == 4'102'444'800LL,
          "owned play history is projected without narrowing valid timestamps");
    check(projected->genres.empty() && projected->controller_support.empty()
              && !projected->download_bytes && !projected->install_bytes
              && !projected->added_at && !projected->art_pending && !projected->art_failed,
          "projection does not invent install, controller, metadata or artwork state");
    check(model.state().account().authentication == AuthState::SignedIn
              && model.state().account().cache_available
              && model.state().account().last_sync == 1234
              && !model.state().library_loading && !model.state().library_syncing
              && !model.state().library_offline && model.state().account().error.empty(),
          "successful delivery updates library status without repurposing authentication");
}

void loading_and_failure_preserve_last_good_snapshot() {
    auto model = make_live();
    model.UpdateAccount({AuthState::SignedIn, "account", "Owner", {}, {}, false});
    check(model.UpdateLibrary({{game(SteamId(1), "One"), game(SteamId(2), "Two")}, false, {}, false, true, 100}),
          "initial snapshot accepted");

    check(model.UpdateLibrary({{}, true, {}, false, true, std::nullopt}),
          "loading delivery status accepted");
    check(model.state().games.size() == 2 && model.state().library_loading
              && model.state().library_syncing && model.state().account().last_sync == 100,
          "loading preserves games and prior retrieval time");

    check(model.UpdateLibrary({{}, false, "Refresh failed", true, true, std::nullopt}),
          "failure delivery status accepted");
    check(model.state().games.size() == 2 && !model.state().library_loading
              && !model.state().library_syncing && model.state().library_offline
              && model.state().account().error == "Refresh failed",
          "failed refresh keeps the last good catalog and exposes stale error state");

    check(model.UpdateLibrary({{}, false, {}, false, true, 200}),
          "genuine successful empty snapshot accepted");
    check(model.state().games.empty() && model.state().account().last_sync == 200
              && model.state().account().error.empty() && !model.state().library_offline,
          "successful empty ownership replaces stale games and clears failure state");

    check(model.UpdateLibrary({{game(SteamId(3), "Three")}, false, {}, false, true, 201})
              && model.action("details:3"),
          "details can be open before an empty ownership result");
    check(model.UpdateLibrary({{}, false, {}, false, true, 202})
              && model.state().navigation.route == Route::Home
              && !model.state().navigation.detail_id
              && !model.state().navigation.selected_game_id.valid()
              && has_row(model, "home-library"),
          "empty ownership leaves a removed details record for its empty origin surface");
}

void rejects_invalid_snapshots_transactionally_and_leaves_simulation_alone() {
    auto model = make_live();
    model.UpdateAccount({AuthState::SignedIn, "account", "Owner", {}, {}, false});
    check(model.UpdateLibrary({{game(SteamId(5), "Five")}, false, {}, false, true, 50}),
          "baseline snapshot accepted");

    const auto assert_unchanged = [&model](const char* message) {
        check(model.state().games.size() == 1 && model.state().games.front().id == SteamId(5)
                  && model.state().account().last_sync == 50
                  && model.state().account().cache_available
                  && model.state().account().error.empty() && !model.state().library_offline,
              message);
    };
    check(!model.UpdateLibrary({{game(SteamId(0), "Zero")}, false, "must not leak", true, false, 60}),
          "zero app ID is rejected");
    assert_unchanged("zero-ID rejection is transactional");
    check(!model.UpdateLibrary({{game(SteamId(6), "")}, false, {}, false, false, 60}),
          "empty title is rejected");
    assert_unchanged("empty-title rejection is transactional");
    check(!model.UpdateLibrary({{game(SteamId(6), "First"), game(SteamId(6), "Duplicate")}, false, {}, false, false, 60}),
          "duplicate app ID is rejected");
    assert_unchanged("duplicate rejection is transactional");

    Simulation simulation("fixtures-not-used", "profile-not-used.json", ModelMode::Simulation);
    const auto prior_games = simulation.state().games.size();
    check(!simulation.UpdateLibrary({{game(SteamId(88), "Live only")}, false, {}, false, true, 90})
              && simulation.state().games.size() == prior_games,
          "production library API cannot mutate deterministic simulation state");
}

// Keys name games canonically: leading zeros, signs or garbage name no game, even when the number
// would match one.
void non_canonical_game_keys_select_nothing() {
    auto model = make_live();
    model.UpdateAccount({AuthState::SignedIn, "account", "Owner", {}, {}, false});
    check(model.UpdateLibrary({{game(SteamId(19), "Nineteen"), game(SteamId(20), "Twenty")}, false, {}, false, true, 1}),
          "library loaded");
    check(model.action("r1") && model.action("select:20"), "a canonical key selects its game");
    for (const char* key : {"0020", "+19", "19x", "019", " 19", "local-", "gog-19", "4294967315", ""}) {
        const std::string text(key);
        check(!model.action("select:" + text) && !model.action("details:" + text) &&
                  !model.action("rate:" + text + ":works") && !model.action("note:" + text + ":hi"),
              "a non-canonical key is refused");
        check(model.state().navigation.selected_game_id == SteamId(20) && model.state().library.ratings.empty() &&
                  model.state().library.notes.empty(),
              "a non-canonical key selects nothing");
    }
}

void preserves_identity_and_scroll_then_chooses_a_stable_neighbor() {
    auto model = make_live();
    model.UpdateAccount({AuthState::SignedIn, "account", "Owner", {}, {}, false});
    std::vector<GameSummary> initial;
    for (std::uint32_t id = 1; id <= 30; ++id)
        initial.push_back(game(SteamId(id), "Game " + (id < 10 ? std::string("0") : std::string{}) + std::to_string(id)));
    check(model.UpdateLibrary({initial, false, {}, false, true, 1}), "large snapshot accepted");
    check(model.action("r1") && model.state().navigation.route == Route::Library,
          "navigation enters library");
    check(model.action("select:13") && model.action("down"), "selection moves into a scrolled row");
    const auto selected_id = model.state().navigation.selected_game_id;
    const auto scroll = model.state().navigation.scroll_offsets.at("library-grid");
    check(selected_id == SteamId(19) && scroll == 3, "test starts with stable identity and nonzero scroll");

    auto reordered = initial;
    for (auto& item : reordered)
        if (item.id == selected_id) item.title = "AAA selected moved";
    std::reverse(reordered.begin(), reordered.end());
    check(model.UpdateLibrary({reordered, false, {}, false, true, 2}), "reordered snapshot accepted");
    check(model.state().navigation.selected_game_id == selected_id
              && model.state().navigation.focus_key == "game-19"
              && model.state().navigation.scroll_offsets.at("library-grid") == scroll,
          "refresh preserves selected Steam ID, focus and scroll across reorder");

    const int prior_index = model.state().navigation.selected_index;
    reordered.erase(std::remove_if(reordered.begin(), reordered.end(), [selected_id](const auto& item) {
        return item.id == selected_id;
    }), reordered.end());
    check(model.UpdateLibrary({reordered, false, {}, false, true, 3}), "removal snapshot accepted");
    const auto visible = model.visible_games();
    const int expected_index = std::min(prior_index, static_cast<int>(visible.size()) - 1);
    check(!visible.empty() && model.state().navigation.selected_index == expected_index
              && model.state().navigation.selected_game_id == visible[static_cast<std::size_t>(expected_index)]->id
              && model.state().navigation.focus_key == "game-" + ToKey(model.state().navigation.selected_game_id)
              && model.state().navigation.scroll_offsets.at("library-grid") == scroll,
          "removed selection chooses the item at the prior visible index and preserves scroll");

    const auto detail_id = model.state().navigation.selected_game_id;
    check(model.action("details:" + ToKey(detail_id))
              && model.action("focus:details-more"),
          "selected neighbor opens with a stable details focus");
    auto detail_refresh = reordered;
    for (auto& item : detail_refresh) if (item.id == detail_id) item.title = "AAA detail moved";
    check(model.UpdateLibrary({detail_refresh, false, {}, false, true, 4}),
          "details refresh accepted");
    check(model.state().navigation.detail_id == detail_id
              && model.state().navigation.selected_game_id == detail_id
              && model.state().navigation.focus_key == "details-more"
              && model.state().navigation.scroll_offsets.at("library-grid") == scroll,
          "details refresh preserves game identity, action focus and scroll by Steam ID");
}

void live_details_keep_personal_edits_and_block_fixture_operations() {
    auto model = make_live();
    model.UpdateAccount({AuthState::SignedIn, "account", "Owner", {}, {}, false});
    check(model.UpdateLibrary({{game(SteamId(7), "Seven")}, false, {}, false, true, 1}),
          "live game delivered");
    check(model.action("select:7") && model.action("square")
              && model.action("note:7:personal note")
              && model.action("collection:create:Personal")
              && model.action("collection:add:collection-1:7"),
          "favorites, notes and collections remain editable in live mode");
    check(model.UpdateLibrary({{game(SteamId(8), "Eight"), game(SteamId(7), "Seven renamed")}, false, {}, false, true, 2}),
          "follow-up live snapshot accepted");
    check(model.state().library.favorites.contains(SteamId(7))
              && model.state().library.notes.at(SteamId(7)) == "personal note"
              && model.state().library.collections.front().members.contains(SteamId(7)),
          "refresh preserves account-scoped personal annotations");

    check(model.action("details:7"), "live details open");
    check(has_row(model, "details-favorite")
              && std::none_of(model.view().rows.begin(), model.view().rows.end(),
                              [](const auto& row) { return row.key == "details-install" && row.enabled; })
              && !has_row(model, "details-play")
              && !has_row(model, "details-simulate-launch"),
          "live details expose personal actions without fixture install/session actions");
    const auto focused = model.view().focused_row;
    check(focused >= 0 && model.view().rows[static_cast<std::size_t>(focused)].enabled
              && model.view().rows[static_cast<std::size_t>(focused)].key == "details-favorite",
          "live details enter on a visible enabled favorite action");
    check(model.action("focus:details-more") && model.action("cross"), "personal context opens");
    check(has_row(model, "favorite") && has_row(model, "collection") && has_row(model, "note")
              && has_row(model, "hide") && !has_row(model, "verify") && !has_row(model, "logs"),
          "live context retains personal actions and hides fixture operations");
    const auto uninstall = std::find_if(model.view().rows.begin(), model.view().rows.end(),
                                        [](const auto& row) { return row.key == "uninstall"; });
    check(uninstall != model.view().rows.end() && !uninstall->enabled, "live Uninstall is disabled until installed");
}

const playden::homebrew::ViewRow* find_row(const Simulation& model, const std::string& key) {
    const auto found = std::find_if(model.view().rows.begin(), model.view().rows.end(),
                                    [&key](const auto& row) { return row.key == key; });
    return found == model.view().rows.end() ? nullptr : &*found;
}

void live_install_requests_and_projection() {
    auto model=make_live();
    model.UpdateAccount({AuthState::SignedIn,"123","Synthetic",{},{},true});
    check(model.UpdateLibrary({{game(SteamId(42),"Synthetic")},false,{},false,false,{}}),"library loaded");
    check(model.action("details:42"), "details open");
    check(has_row(model, "details-checking"), "install state waits for Steam details");
    {
        const auto* waiting = find_row(model, "details-install");
        check(waiting && !waiting->enabled && waiting->value == "…", "Install stays in place, disabled, while Steam answers");
    }
    check(model.UpdateAppDetails(SteamId(42), 300'000'000, 600'000'000, true), "app details apply");
    check(has_row(model, "details-install"), "an installable game offers Install");
    check(model.state().navigation.focus_key == "details-install",
          "focus moves from Favorite to Install once Steam's details arrive");
    model.UpdateVolumes({{"internal", "Console storage", true, 2'000'000'000, 1'000'000'000}}, "internal");
    check(find_row(model, "details-install")->value == "600 MB",
          "Install shows the install size (not the download size), the one size a game has");
    check(find_row(model, "details-install")->subtitle == "To Console storage · 1.0 GB free",
          "Install names the destination and its free space");
    activate(model, "details-install");
    check(model.view().surface == "install" && model.TakeInstallRequests().empty(),
          "Install opens the install sheet first, even when the game fits");
    activate(model, "install-confirm");
    auto requests = model.TakeInstallRequests();
    check(requests.size() == 1 && requests[0].kind == playden::homebrew::InstallRequestKind::Install && requests[0].game_id == SteamId(42),
          "Install sends one request");
    model.UpdateInstalls({{SteamId(42), playden::homebrew::JobState::Downloading, 50, 200, {}}}, {});
    check(has_row(model, "details-download") && !has_row(model, "details-install"), "a running job replaces Install");
    check(model.state().navigation.focus_key == "details-download", "focus follows Install to the download");
    activate(model, "details-download");
    check(model.state().navigation.route == playden::homebrew::Route::Downloads, "the download row opens Downloads");
    activate(model, "job-42");
    activate(model, "job-pause-42");
    requests = model.TakeInstallRequests();
    check(requests.size() == 1 && requests[0].kind == playden::homebrew::InstallRequestKind::Pause, "Pause goes to the service");
    model.UpdateInstalls({}, {{SteamId(42), 600'000'000}});
    check(model.action("details:42") && has_row(model, "details-setup"), "a finished install shows setup readiness");
    check(!has_row(model, "details-runtime"), "no runtime row before the runtime host reports");
    model.UpdateRuntime(playden::homebrew::RuntimeState::Ready,
                        {{SteamId(42), "Not prepared", "Makes a private bottle and its own PS5 title", true, false}});
    check(model.action("details:42") && has_row(model, "details-play"), "an installed game offers Play");
    const auto* play = find_row(model, "details-play");
    check(play && play->enabled && play->action == "runtime-play", "Play is offered before the game is prepared");
    activate(model, "details-play");
    const auto plays = model.TakeRuntimePlayRequests();
    check(plays.size() == 1 && plays[0] == SteamId(42), "Play on an unprepared game goes to the controller, which prepares first");
    check(find_row(model, "details-setup") && find_row(model, "details-setup")->value == "Not set up",
          "setup readiness appears as a fact");
    model.RequestRuntimePrepare(SteamId(42));
    check(model.TakeRuntimePrepareRequests().size() == 1, "Prepare can be queued without a button press");
    model.UpdateRuntime(playden::homebrew::RuntimeState::Ready, {{SteamId(42), "Preparing", "Copying the bottle", false, false}});
    check(model.action("details:42"), "details reopen");
    check(find_row(model, "details-play")->progress == 0.0F,
          "a step without a known fraction shows no bar");
    {
        playden::homebrew::LiveRuntimeGame copying{SteamId(42), "Preparing", "Copying the Windows environment", false, false};
        copying.progress = 0.42F;
        model.UpdateRuntime(playden::homebrew::RuntimeState::Ready, {copying});
        check(find_row(model, "details-play")->progress == 0.42F,
              "a copy with a known fraction shows it on Play");
        model.UpdateRuntime(playden::homebrew::RuntimeState::Ready, {{SteamId(42), "Preparing", "Copying the bottle", false, false}});
    }
    check(model.state().installations.size() == 1 && !model.state().installations[0].runtime_ready, "preparing is not ready");
    model.UpdateRuntime(playden::homebrew::RuntimeState::Ready, {{SteamId(42), "Ready", "FAKE78001", false, true}});
    model.UpdateInstalls({}, {{SteamId(42), 600'000'000}});
    check(model.state().installations[0].runtime_ready, "readiness survives install refreshes");
    check(model.action("details:42"), "details reopen");
    check(model.action("focus:details-more") && model.action("cross"), "context opens");
    activate(model, "uninstall");
    activate(model, "confirm-destructive");
    requests = model.TakeInstallRequests();
    check(requests.size() == 1 && requests[0].kind == playden::homebrew::InstallRequestKind::Uninstall && requests[0].game_id == SteamId(42),
          "Uninstall confirms and goes to the service");
    model.UpdateAppDetails(SteamId(42), std::nullopt, std::nullopt, false);
    model.UpdateInstalls({}, {});
    check(model.action("details:42") && has_row(model, "details-unavailable"), "a game without Windows content says so");
}

void live_install_volumes() {
    using playden::homebrew::InstallRequestKind;
    using playden::homebrew::JobState;
    auto model = make_live();
    model.UpdateAccount({AuthState::SignedIn, "123", "Synthetic", {}, {}, true});
    check(model.UpdateLibrary({{game(SteamId(42), "Installed"), game(SteamId(43), "Downloading"), game(SteamId(44), "On USB"), game(SteamId(45), "New")},
                               false, {}, false, false, {}}), "library loaded");
    model.UpdateVolumes({{"internal", "Console storage", true, std::nullopt, std::nullopt},
                         {"m2", "M.2 SSD", true, 2'000, 500},
                         {"usb0", "USB drive", false, std::nullopt, std::nullopt},
                         {"ext0", "Extended storage", true, 4'000, 3'000, false}}, "internal");
    model.UpdateAppDetails(SteamId(43), 400, 800, true);
    model.UpdateInstalls({{SteamId(43), JobState::Downloading, 100, 400, {}, "m2"}},
                         {{SteamId(42), 600, "internal", true}, {SteamId(44), 0, "usb0", false}});

    const auto& volumes = model.state().volumes;
    check(volumes.size() == 4 && model.state().default_volume_id == "internal", "volumes and default projected");
    check(!volumes[0].capacity_known && volumes[0].games_bytes == 600 && volumes[0].used_bytes == 600
              && volumes[0].reserved_bytes == 0, "a drive without a space report counts finished installs");
    check(volumes[1].capacity_known && volumes[1].capacity_bytes == 2'000 && volumes[1].used_bytes == 1'500
              && volumes[1].reserved_bytes == 800, "a reported drive uses its own figures plus full install reservations");
    check(!volumes[2].available, "an absent drive stays listed as unavailable");
    check(volumes[3].available && !volumes[3].writable && volumes[3].capacity_known && volumes[3].capacity_bytes == 4'000,
          "an attached drive Playden can't write to keeps its space report");

    // Settings shows the default and changes it through the install service.
    model.action("l1");
    const auto* storage = find_row(model, "settings-storage");
    check(storage && storage->enabled && storage->value == "Console storage", "Settings shows the default storage");
    activate(model, "settings-storage");
    const auto* usb = find_row(model, "volume-usb0");
    check(has_row(model, "volume-internal") && has_row(model, "volume-m2") && usb && !usb->enabled,
          "the picker lists every drive and disables an absent one");
    const auto* locked = find_row(model, "volume-ext0");
    check(locked && !locked->enabled && locked->subtitle == "No write access" && usb->subtitle == "Disconnected",
          "an attached drive Playden can't write to is told apart from an absent one");
    const auto* m2 = find_row(model, "volume-m2");
    check(m2 && m2->value.find("free") != std::string::npos, "a reported drive shows its free space");
    activate(model, "volume-m2");
    auto requests = model.TakeInstallRequests();
    check(requests.size() == 1 && requests[0].kind == InstallRequestKind::SetDefaultVolume && requests[0].volume_id == "m2",
          "choosing a drive asks the install service to use it");
    check(model.state().default_volume_id == "m2", "the choice shows immediately");
    activate(model, "settings-storage");
    activate(model, "volume-m2");
    check(model.TakeInstallRequests().empty(), "choosing the current default sends nothing");
    const auto unsaved = [&] {
        const auto& modals = model.state().navigation.modals;
        return std::count_if(modals.begin(), modals.end(), [](const auto& modal) { return modal.title == "Install drive not saved"; });
    };
    const std::vector<playden::homebrew::LiveVolume> live_volumes{{"internal", "Console storage", true, std::nullopt, std::nullopt},
                                               {"m2", "M.2 SSD", true, 2'000, 500},
                                               {"usb0", "USB drive", false, std::nullopt, std::nullopt},
                                               {"ext0", "Extended storage", true, 4'000, 3'000, false}};
    model.UpdateVolumes(live_volumes, "m2", false);
    check(unsaved() == 1, "a drive choice that couldn't be saved is said");
    model.UpdateVolumes(live_volumes, "m2", false);
    check(unsaved() == 1, "and said once, not on every refresh");
    model.action("circle");

    // Details show where a game is and where a new install goes.
    check(model.action("details:42"), "installed game opens");
    const auto* installed = find_row(model, "details-installed");
    check(!installed, "installed size and location are shown only in facts");
    check(model.action("details:44"), "game on an absent drive opens");
    installed = find_row(model, "details-installed");
    check(installed && installed->title == "Storage disconnected" && installed->subtitle == "Connect USB drive to use this game",
          "a game on an absent drive says so even without a known size");
    check(model.action("details:45") && model.UpdateAppDetails(SteamId(45), 1'000, 2'000, true), "new game details");
    const auto* install = find_row(model, "details-install");
    check(install && install->subtitle.find("To M.2 SSD · ") == 0 && install->subtitle.find(" free") != std::string::npos &&
              install->subtitle.find("reservation") == std::string::npos, "Install names the default drive and its free space");
}

const playden::homebrew::Notice* find_notice(const Simulation& model, const std::string& id) {
    const auto found = std::find_if(model.state().notices.begin(), model.state().notices.end(),
                                    [&id](const auto& notice) { return notice.id == id; });
    return found == model.state().notices.end() ? nullptr : &*found;
}

void live_install_confirmation_checks_space_and_unknowns() {
    using playden::homebrew::JobState;
    using playden::homebrew::InstallRequestKind;
    constexpr std::uint64_t margin = 64ULL * 1024 * 1024;
    auto model = make_live();
    model.UpdateAccount({AuthState::SignedIn, "123", "Synthetic", {}, {}, true});
    check(model.UpdateLibrary({{game(SteamId(42), "New"), game(SteamId(43), "Queued")}, false, {}, false, false, {}}), "library loaded");
    model.UpdateAppDetails(SteamId(42), 100, 600, true);
    model.UpdateAppDetails(SteamId(43), 100, 700, true);
    model.UpdateVolumes({{"internal", "Console storage", true, margin + 2'000, margin + 599},
                         {"m2", "M.2 SSD", true, margin + 2'000, margin + 1'000}}, "internal");
    check(model.action("details:42"), "details open");

    // A game that does not fit the default drive opens the sheet, which explains and cannot confirm.
    activate(model, "details-install");
    check(model.view().surface == "install" && !find_row(model, "install-confirm")->enabled,
          "a game that does not fit opens the install sheet without a usable Install");
    // Its exact size has nothing to change (the controller answers so for a game without overlapping parts).
    for (const auto& size_check : model.TakeSizeChecks()) model.UpdateMeasuredSize(size_check, false, false, {});
    check(model.view().subtitle.find("Not enough space") != std::string::npos, "the sheet says why");
    check(model.TakeInstallRequests().empty(), "nothing is queued while it does not fit");

    // Another drive is for this install only: the default stays.
    activate(model, "storage-picker");
    check(model.view().title == "Install to" && model.view().subtitle.find("For this game only") == 0,
          "the picker says the choice is for this game only");
    activate(model, "volume-m2");
    check(model.view().surface == "install" && find_row(model, "install-confirm")->enabled,
          "a drive with room enables Install");
    // Back on Install, the sheet moves on from there: Down goes to the row under it, not under Change storage.
    check(model.state().navigation.focus_key == "install-confirm" && model.action("down") &&
              model.state().navigation.focus_key == "storage-picker" && model.action("up") &&
              model.state().navigation.focus_key == "install-confirm",
          "the focus moves on from Install");
    check(model.view().subtitle.find("To M.2 SSD (this game only)") == 0 &&
              model.view().subtitle.find("Other games still install to Console storage") != std::string::npos,
          "the sheet names the one-off drive and the unchanged default");
    check(model.TakeInstallRequests().empty() && model.state().default_volume_id == "internal",
          "choosing a drive for one install does not change the default");
    activate(model, "install-confirm");
    auto requests = model.TakeInstallRequests();
    check(requests.size() == 1 && requests[0].kind == InstallRequestKind::Install && requests[0].game_id == SteamId(42) &&
              requests[0].volume_id == "m2", "the install goes to the chosen drive");

    // Reservations: the rest of each download's estimate, less what it already holds on the drive.
    model.UpdateInstalls({{SteamId(43), JobState::Downloading, 90, 100, {}, "m2"}}, {});
    check(model.state().volumes[1].reserved_bytes == 700, "a download that has reported nothing reserves its estimate");
    model.UpdateInstalls({{SteamId(43), JobState::Downloading, 90, 100, {}, "m2", {}, {}, 300}}, {});
    check(model.state().volumes[1].reserved_bytes == 400, "bytes a download already holds on the drive are not reserved twice");
    model.UpdateInstalls({{SteamId(43), JobState::Downloading, 90, 100, {}, "m2", {}, {}, 900}}, {});
    check(model.state().volumes[1].reserved_bytes == 0, "a preallocated download reserves nothing more");
    model.UpdateInstalls({{SteamId(43), JobState::Failed, 90, 100, {}, "m2"}}, {});
    check(model.state().volumes[1].reserved_bytes == 0, "failed downloads release live reservations");
    model.UpdateInstalls({{SteamId(43), JobState::Cancelled, 90, 100, {}, "m2"}}, {});
    check(model.state().volumes[1].reserved_bytes == 0, "cancelled downloads hold no reservation");
    model.UpdateInstalls({{SteamId(43), JobState::Downloaded, 100, 100, {}, "m2"}}, {});
    check(model.state().volumes[1].reserved_bytes == 0, "completed downloads hold no reservation");
    model.UpdateInstalls({}, {});

    // A game that fits still opens the sheet; Install there sends it to the default drive.
    model.UpdateVolumes({{"internal", "Console storage", true, margin + 2'000, margin + 1'000}}, "internal");
    activate(model, "details-install");
    check(model.view().surface == "install" && model.TakeInstallRequests().empty(), "a game that fits opens the sheet too");
    activate(model, "install-confirm");
    requests = model.TakeInstallRequests();
    check(requests.size() == 1 && requests[0].volume_id.empty(), "the sheet installs to the default drive");

    // An unknown size can be installed when the drive has the margin; the downloader checks it.
    model.UpdateAppDetails(SteamId(42), std::nullopt, std::nullopt, true);
    check(find_row(model, "details-install")->value == "Size unknown", "unknown size never displays zero");
    activate(model, "details-install");
    activate(model, "install-confirm");
    check(model.TakeInstallRequests().size() == 1, "unknown size installs with a pre-download check");
    model.UpdateVolumes({{"internal", "Console storage", true, margin + 2'000, 0}}, "internal");
    activate(model, "details-install");
    check(model.view().surface == "install" && !find_row(model, "install-confirm")->enabled,
          "unknown size still needs the known download safety margin");
    activate(model, "cancel");

    // A drive whose free space cannot be read blocks; a console that cannot read it at all does not.
    model.UpdateVolumes({{"internal", "Console storage", true, std::nullopt, std::nullopt}}, "internal");
    activate(model, "details-install");
    check(!find_row(model, "install-confirm")->enabled &&
              model.view().subtitle.find("can't be read") != std::string::npos,
          "unknown capacity is honestly unavailable and blocks confirmation");
    check(model.TakeInstallRequests().empty(), "unknown capacity sends no install request");
    activate(model, "cancel");
    model.SetFreeSpaceSupported(false);
    model.UpdateVolumes({{"internal", "Console storage", true, std::nullopt, std::nullopt}}, "internal");
    activate(model, "details-install");
    activate(model, "install-confirm");
    check(model.TakeInstallRequests().size() == 1, "a console that cannot read free space still installs, as before");
}

// Owned DLC the download includes: an "Includes" status row on the install sheet, only when there is some.
void live_install_dlc_row() {
    auto model = make_live();
    model.UpdateAccount({AuthState::SignedIn, "123", "Synthetic", {}, {}, true});
    check(model.UpdateLibrary({{game(SteamId(42), "With DLC"), game(SteamId(43), "Without DLC")}, false, {}, false, false, {}}),
          "library loaded");
    model.UpdateVolumes({{"internal", "Console storage", true, 2'000'000'000, 1'000'000'000}}, "internal");
    model.UpdateAppDetails(SteamId(42), 100, 600, true, {"english", "german"}, 3);
    check(model.action("details:42"), "details open");
    activate(model, "details-install");
    const auto* row = find_row(model, "install-dlc");
    check(model.view().surface == "install" && row && row->title == "Includes" && row->value == "3 DLC" &&
              row->kind == RowKind::Status && !row->enabled,
          "the sheet says how many DLC the download includes");
    activate(model, "cancel");
    // DLC alone doesn't open the sheet; without DLC there is no row.
    model.UpdateAppDetails(SteamId(43), 100, 600, true, {"english", "german"}, 0);
    check(model.action("details:43"), "details open");
    activate(model, "details-install");
    check(model.view().surface == "install" && find_row(model, "install-dlc") == nullptr, "no DLC, no row");
    activate(model, "cancel");
    model.UpdateAppDetails(SteamId(42), 100, 600, true, {"german"}, 3);
    check(model.action("details:42"), "details open");
    activate(model, "details-install");
    check(model.view().surface == "install" && find_row(model, "install-language") == nullptr &&
              model.TakeInstallRequests().empty(),
          "a game with one language opens the sheet without a Language row");
}

// A game in several languages: the install sheet's Language row chooses one for this install only.
void live_install_language_row() {
    using playden::homebrew::InstallRequestKind;
    auto model = make_live();
    model.UpdateAccount({AuthState::SignedIn, "123", "Synthetic", {}, {}, true});
    check(model.UpdateLibrary({{game(SteamId(42), "Localised"), game(SteamId(43), "Neutral")}, false, {}, false, false, {}}),
          "library loaded");
    model.UpdateVolumes({{"internal", "Console storage", true, 2'000'000'000, 1'000'000'000}}, "internal");
    model.UpdateAppDetails(SteamId(43), 100, 600, true, {"german"});
    check(model.action("details:43"), "details open");
    activate(model, "details-install");
    check(model.view().surface == "install" && find_row(model, "install-language") == nullptr &&
              find_row(model, "storage-picker") != nullptr && model.TakeInstallRequests().empty(),
          "a game with one language opens the sheet with its Storage row, without a Language row");
    activate(model, "cancel");

    model.UpdateAppDetails(SteamId(42), 100, 600, true, {"english", "german", "french"});
    check(model.action("details:42"), "details open");
    activate(model, "details-install");
    check(model.view().surface == "install" && model.TakeInstallRequests().empty(),
          "a game in several languages opens the install sheet");
    const auto* row = find_row(model, "install-language");
    check(row && row->title == "Language" && row->value == "English" && row->subtitle == "From Settings › Downloads",
          "the Language row shows the language the Settings one resolves to");
    activate(model, "install-language");
    check(model.view().surface == "install-language", "the row opens the game's languages");
    std::vector<std::string> keys;
    for (const auto& item : model.view().rows) keys.push_back(item.key);
    check(keys == std::vector<std::string>{"install-language-english", "install-language-german", "install-language-french",
                                           "cancel"}, "the picker lists only the languages the game carries");
    check(find_row(model, "install-language-english")->selected && !find_row(model, "install-language-german")->selected,
          "the language in use is ticked");
    activate(model, "install-language-german");
    check(model.view().surface == "install" && find_row(model, "install-language")->value == "German" &&
              find_row(model, "install-language")->subtitle == "This game only",
          "the choice is shown on the sheet");
    check(model.state().content_language == "english", "the Settings language does not change");
    activate(model, "install-confirm");
    auto requests = model.TakeInstallRequests();
    check(requests.size() == 1 && requests[0].kind == InstallRequestKind::Install && requests[0].language == "german",
          "the install request carries this install's language");

    // The next install starts from the Settings language again.
    check(model.action("details:42"), "details open");
    activate(model, "details-install");
    check(find_row(model, "install-language")->value == "English", "the one-off choice does not stay");
    activate(model, "install-confirm");
    requests = model.TakeInstallRequests();
    check(requests.size() == 1 && requests[0].language.empty(), "no choice sends the Settings language");
}

// A game with several versions: the install sheet's Version row chooses one for this install only,
// and the size the sheet checks follows it.
void live_install_version_row() {
    using playden::homebrew::GameVersion;
    using playden::homebrew::InstallRequestKind;
    auto model = make_live();
    model.UpdateAccount({AuthState::SignedIn, "123", "Synthetic", {}, {}, true});
    check(model.UpdateLibrary({{game(SteamId(42), "Versions"), game(SteamId(43), "Single")}, false, {}, false, false, {}}),
          "library loaded");
    model.UpdateVolumes({{"internal", "Console storage", true, 2'000'000'000, 1'000'000'000}}, "internal");
    model.UpdateAppDetails(SteamId(43), 100, 600, true, {}, 0, {GameVersion{"public", "", 1760000000, 9000, 100, 600}});
    check(model.action("details:43"), "details open");
    activate(model, "details-install");
    check(model.view().surface == "install" && find_row(model, "install-version") == nullptr &&
              model.TakeInstallRequests().empty(),
          "a game with one version opens the sheet without a Version row");
    activate(model, "cancel");

    model.UpdateAppDetails(SteamId(42), 100, 600, true, {}, 0,
                           {GameVersion{"public", "", 1760000000, 9000, 100, 600},
                            GameVersion{"4.04", "Patch 4.04 (pre next-gen)", 1670000000, 4040, 200, 1'500'000'000}});
    check(model.action("details:42"), "details open");
    activate(model, "details-install");
    check(model.view().surface == "install" && model.TakeInstallRequests().empty(), "a game with versions opens the install sheet");
    check(find_row(model, "install-language") == nullptr, "one language, no Language row");
    const auto* row = find_row(model, "install-version");
    check(row && row->title == "Version" && row->value == "Public" && row->subtitle == "The current version",
          "the Version row starts on Public");
    check(find_row(model, "install-confirm")->enabled, "Public fits");
    activate(model, "install-version");
    check(model.view().surface == "install-version", "the row opens the versions");
    std::vector<std::string> keys;
    for (const auto& item : model.view().rows) keys.push_back(item.key);
    check(keys == std::vector<std::string>{"install-version-public", "install-version-4.04", "cancel"}, "public first, then branches");
    const auto* old = find_row(model, "install-version-4.04");
    check(old->title == "4.04" && old->subtitle == "Patch 4.04 (pre next-gen)" && old->value == "Built 02 Dec 2022" && !old->selected,
          "a branch shows its description and build date");
    check(find_row(model, "install-version-public")->selected && find_row(model, "install-version-public")->title == "Public" &&
              find_row(model, "install-version-public")->value == "Built 09 Oct 2025",
          "Public is ticked");
    activate(model, "install-version-4.04");
    row = find_row(model, "install-version");
    check(model.view().surface == "install" && row->value == "4.04" && row->subtitle == "Patch 4.04 (pre next-gen)",
          "the choice is shown on the sheet");
    // 4.04 needs 1.5 GB: it does not fit the 1 GB free, so its size is the one checked.
    check(!find_row(model, "install-confirm")->enabled, "the size follows the chosen version");
    activate(model, "install-version");
    activate(model, "install-version-public");
    check(find_row(model, "install-version")->value == "Public" && find_row(model, "install-confirm")->enabled,
          "back on Public");
    activate(model, "install-confirm");
    auto requests = model.TakeInstallRequests();
    check(requests.size() == 1 && requests[0].kind == InstallRequestKind::Install && requests[0].branch.empty(),
          "Public sends no branch");
    check(requests[0].install_bytes == std::optional<std::uint64_t>(600), "Public's estimate goes with it");

    // A branch that fits is sent with the install, and doesn't stay for the next one.
    model.UpdateAppDetails(SteamId(42), 100, 600, true, {}, 0,
                           {GameVersion{"public", "", 1760000000, 9000, 100, 600},
                            GameVersion{"4.04", "Patch 4.04 (pre next-gen)", 1670000000, 4040, 200, 700}});
    check(model.action("details:42"), "details open");
    activate(model, "details-install");
    activate(model, "install-version");
    activate(model, "install-version-4.04");
    activate(model, "install-confirm");
    requests = model.TakeInstallRequests();
    check(requests.size() == 1 && requests[0].branch == "4.04", "the install request carries the version");
    check(requests[0].install_bytes == std::optional<std::uint64_t>(700), "and the version's install estimate");
    check(model.action("details:42"), "details open");
    activate(model, "details-install");
    check(find_row(model, "install-version")->value == "Public", "the one-off choice does not stay");
}

// The install sheet's size follows the language chosen on it as well as the version.
void live_install_language_size() {
    using playden::homebrew::GameVersion;
    auto model = make_live();
    model.UpdateAccount({AuthState::SignedIn, "123", "Synthetic", {}, {}, true});
    check(model.UpdateLibrary({{game(SteamId(42), "Voices")}, false, {}, false, false, {}}), "library loaded");
    model.UpdateVolumes({{"internal", "Console storage", true, 2'000'000'000, 1'000'000'000}}, "internal");
    GameVersion version{"public", "", 1760000000, 9000, 300'000'000, 600'000'000, {}};
    version.languages = {{"english", 300'000'000, 600'000'000}, {"russian", 900'000'000, 1'500'000'000}};
    model.UpdateAppDetails(SteamId(42), 300'000'000, 600'000'000, true, {"english", "russian"}, 0, {version});
    check(model.action("details:42"), "details open");
    activate(model, "details-install");
    check(model.view().surface == "install" && model.view().subtitle.find("Needs about 600 MB") != std::string::npos &&
              find_row(model, "install-confirm")->enabled,
          "English: the Settings language's size");
    activate(model, "install-language");
    activate(model, "install-language-russian");
    check(model.view().surface == "install" && model.view().subtitle.find("Needs about 1.5 GB") != std::string::npos,
          "Russian: its own size");
    check(!find_row(model, "install-confirm")->enabled, "and that size is the one checked");
}

// Steam's figures add up each part of a game; the sheet asks for the exact size of the chosen version and language,
// and a game that only fits by it can install once it is known.
void live_install_exact_size() {
    using playden::homebrew::GameVersion;
    using playden::homebrew::SizeCheckRequest;
    auto model = make_live();
    model.UpdateAccount({AuthState::SignedIn, "123", "Synthetic", {}, {}, true});
    check(model.UpdateLibrary({{game(SteamId(400), "Portal")}, false, {}, false, false, {}}), "library loaded");
    model.UpdateVolumes({{"internal", "Console storage", true, 2'000'000'000, 1'000'000'000}}, "internal");
    GameVersion version{"public", "", 1760000000, 9000, 900'000'000, 1'500'000'000, {}};
    version.languages = {{"english", 900'000'000, 1'500'000'000}, {"russian", 1'000'000'000, 1'600'000'000}};
    model.UpdateAppDetails(SteamId(400), 900'000'000, 1'500'000'000, true, {"english", "russian"}, 0, {version});
    check(model.action("details:400"), "details open");
    activate(model, "details-install");
    auto checks = model.TakeSizeChecks();
    check(checks.size() == 1 && checks[0].id == SteamId(400) && checks[0].branch == "public" && checks[0].language == "english",
          "the sheet asks for its choice's exact size");
    check(model.view().subtitle.find("Needs about 1.5 GB") != std::string::npos &&
              model.view().subtitle.find("Checking its exact size") != std::string::npos &&
              !find_row(model, "install-confirm")->enabled && find_row(model, "install-confirm")->subtitle == "Checking size",
          "Steam's sum doesn't fit: the sheet waits for the exact size");
    check(model.TakeSizeChecks().empty(), "asked once");
    model.UpdateMeasuredSize(checks[0], true, false, {600'000'000, 400'000'000});
    check(model.view().subtitle.find("Needs 600 MB") != std::string::npos && find_row(model, "install-confirm")->enabled,
          "the exact size fits");
    // Another language is another check; one that fails keeps Steam's sum and isn't asked again while the sheet is open.
    activate(model, "install-language");
    activate(model, "install-language-russian");
    checks = model.TakeSizeChecks();
    check(checks.size() == 1 && checks[0].language == "russian", "Russian is checked too");
    model.UpdateMeasuredSize(checks[0], false, true, {});
    check(model.view().subtitle.find("Needs about 1.6 GB") != std::string::npos &&
              model.view().subtitle.find("Not enough space") != std::string::npos && !find_row(model, "install-confirm")->enabled,
          "a failed check keeps Steam's sum");
    check(model.TakeSizeChecks().empty(), "not asked again while the sheet is open");
    model.action("circle");
    activate(model, "details-install");
    activate(model, "install-language");
    activate(model, "install-language-russian");
    checks = model.TakeSizeChecks();
    check(checks.size() == 1 && checks[0].language == "russian", "asked again when the sheet opens next");
    // The service dropped it (the language changed in Settings): asked again.
    model.ResetSizeChecks();
    checks = model.TakeSizeChecks();
    check(checks.size() == 1 && checks[0].language == "russian", "a dropped check is asked again");
    // Nothing to measure: Steam's figures stand, and the sheet says so as before.
    model.UpdateMeasuredSize(checks[0], false, false, {});
    check(model.view().subtitle.find("Not enough space") != std::string::npos, "a choice with nothing to check keeps Steam's sum");
    check(model.TakeSizeChecks().empty(), "answered: not asked again");
    // New details (a newer build) forget the game's exact sizes: the sheet checks again.
    model.UpdateAppDetails(SteamId(400), 900'000'000, 1'500'000'000, true, {"english", "russian"}, 0, {version});
    checks = model.TakeSizeChecks();
    check(checks.size() == 1 && checks[0].language == "russian", "new details are checked again");
    activate(model, "install-language");
    activate(model, "install-language-english");
    model.TakeSizeChecks();
    check(model.view().subtitle.find("Needs about 1.5 GB") != std::string::npos, "the old exact size is gone");
}

// A branch named in lower case shows as the picker shows it.
void live_install_version_title() {
    using playden::homebrew::GameVersion;
    auto model = make_live();
    model.UpdateAccount({AuthState::SignedIn, "123", "Synthetic", {}, {}, true});
    check(model.UpdateLibrary({{game(SteamId(42), "Beta")}, false, {}, false, false, {}}), "library loaded");
    model.UpdateVolumes({{"internal", "Console storage", true, 2'000'000'000, 1'000'000'000}}, "internal");
    model.UpdateAppDetails(SteamId(42), 100, 600, true, {}, 0,
                           {GameVersion{"public", "", 1760000000, 9000, 100, 600}, GameVersion{"beta", "", 1750000000, 9001, 100, 600}});
    check(model.action("details:42"), "details open");
    activate(model, "details-install");
    activate(model, "install-version");
    check(find_row(model, "install-version-beta")->title == "Beta", "the picker says Beta");
    activate(model, "install-version-beta");
    check(find_row(model, "install-version")->value == "Beta", "and so does the sheet");
}

void live_install_notifications() {
    using playden::homebrew::JobState;
    auto model = make_live();
    model.SetWallClock(1'000);
    model.UpdateAccount({AuthState::SignedIn, "123", "Synthetic", {}, {}, true});
    check(model.UpdateLibrary({{game(SteamId(42), "Finishing"), game(SteamId(43), "Failing"), game(SteamId(44), "Removed")}, false, {}, false, false, {}}),
          "library loaded");

    // The first snapshot is restored state: a failed job from before restart is not announced.
    model.UpdateInstalls({{SteamId(42), JobState::Downloading, 10, 100, {}, "internal"}, {SteamId(43), JobState::Failed, 0, 100, "Old failure", "internal"}},
                         {{SteamId(44), 500, "internal", true}});
    check(model.state().notices.empty(), "restored history raises no notification");

    // A job that finishes into an install, and a job that newly fails.
    model.UpdateInstalls({{SteamId(43), JobState::Downloading, 5, 100, {}, "internal"}}, {{SteamId(42), 100, "internal", true}, {SteamId(44), 500, "internal", true}});
    model.UpdateInstalls({{SteamId(43), JobState::Failed, 5, 100, "Steam refused the download.", "internal"}},
                         {{SteamId(42), 100, "internal", true}, {SteamId(44), 500, "internal", true}});
    const auto* done = find_notice(model, "install-42");
    check(done && !done->persistent && !done->failure && done->message == "Download complete" && done->detail == "Finishing",
          "a finished download is announced with the game's name");
    const auto* failed = find_notice(model, "install-43");
    check(failed && failed->failure && !failed->persistent && failed->message == "Download failed" &&
              failed->detail == "Failing · Steam refused the download." && model.state().navigation.modals.empty(),
          "a new failure is announced with its reason in a corner, not a dialog");
    model.UpdateInstalls({{SteamId(43), JobState::Failed, 5, 100, "Steam refused the download.", "internal"}},
                         {{SteamId(42), 100, "internal", true}, {SteamId(44), 500, "internal", true}});
    check(std::count_if(model.state().notices.begin(), model.state().notices.end(),
                        [](const auto& notice) { return notice.id == "install-43"; }) == 1 &&
              model.state().navigation.modals.empty(),
          "a failure that persists is not announced twice");

    // The More menu's Favorite says what it will do and closes the menu.
    check(model.action("details:44") && model.action("focus:details-more") && model.action("cross"), "menu opens");
    check(find_row(model, "favorite") && find_row(model, "favorite")->title == "Add to Favorites", "menu offers to add");
    activate(model, "favorite");
    check(model.state().navigation.modals.empty() && model.state().library.favorites.contains(SteamId(44)),
          "Favorite from the menu marks the game and closes the menu");
    check(model.action("focus:details-more") && model.action("cross") &&
              find_row(model, "favorite")->title == "Remove from Favorites", "menu offers to remove a favorite");
    activate(model, "favorite");
    check(!model.state().library.favorites.contains(SteamId(44)), "removing works the same way");

    // Uninstall: only a removal the player asked for is announced.
    check(model.action("details:44") && model.action("focus:details-more") && model.action("cross"), "context opens");
    activate(model, "uninstall");
    activate(model, "confirm-destructive");
    model.TakeInstallRequests();
    model.UpdateInstalls({{SteamId(43), JobState::Failed, 5, 100, "Steam refused the download.", "internal"}}, {{SteamId(42), 100, "internal", true}});
    const auto* removed = find_notice(model, "install-44");
    check(removed && removed->message == "Game uninstalled" && removed->detail == "Removed", "an uninstall is announced");

    // Notifications count down only while visible and then disappear.
    check(model.action("details:42") && model.action("focus:details-more") && model.action("cross")
              && !model.state().navigation.modals.empty(), "a dialog opens");
    model.SetWallClock(1'030);
    check(find_notice(model, "install-42") != nullptr, "notifications wait behind a dialog");
    check(model.action("circle"), "the dialog closes");
    model.SetWallClock(1'033);
    check(find_notice(model, "install-42") != nullptr, "a visible notification lasts a few seconds");
    model.SetWallClock(1'040);
    check(!find_notice(model, "install-42") && !find_notice(model, "install-44"), "visible notifications expire");

    // Controller: the pad reads as disconnected for the first frames after start; that says nothing.
    model.SetControllerConnected(false);
    model.SetControllerConnected(true);
    check(!find_notice(model, "controller") && !find_notice(model, "controller-connected"),
          "the controller found at start is not announced");
    // Then a lasting warning while disconnected, a short notice on reconnect.
    model.SetControllerConnected(false);
    const auto* lost = find_notice(model, "controller");
    check(lost && lost->persistent && lost->failure, "a disconnected controller raises a lasting warning");
    model.SetControllerConnected(true);
    check(!find_notice(model, "controller") && find_notice(model, "controller-connected"),
          "reconnecting clears the warning and says so briefly");
}

void live_log_viewer() {
    using playden::homebrew::InstallRequestKind;
    using playden::homebrew::JobState;
    using playden::homebrew::LogState;
    const auto directory = std::filesystem::temp_directory_path() / ("playden-model-logs-" + std::to_string(::getpid()));
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory / "launch.log");  // unreadable as a file
    {
        std::ofstream log(directory / "install.log");
        for (int index = 0; index < 12; ++index)
            log << "2026-10-05T07:00:" << (10 + index) << "Z app=" << (index % 2 ? 43 : 42) << " step=trace result=r" << index << " detail=\n";
    }
    auto model = make_live();
    model.UpdateAccount({AuthState::SignedIn, "123", "Synthetic", {}, {}, true});
    check(model.UpdateLibrary({{game(SteamId(42), "Fine"), game(SteamId(43), "Broken")}, false, {}, false, false, {}}), "library loaded");
    model.UpdateInstalls({{SteamId(43), JobState::Failed, 0, 100, "Download failed.", "internal"}}, {});

    // Without a log folder the Settings entry is unavailable.
    model.action("l1");
    const auto* logs_row = find_row(model, "settings-logs");
    check(logs_row && !logs_row->enabled, "no log folder: Logs is unavailable");
    model.SetLogDirectory(directory);
    logs_row = find_row(model, "settings-logs");
    check(logs_row && logs_row->enabled, "with a log folder: Logs opens");

    activate(model, "settings-logs");
    check(has_row(model, "log-install") && has_row(model, "log-auth") && has_row(model, "log-launch"), "the log picker lists each log");
    activate(model, "log-install");
    check(model.state().log_state == LogState::Ready && model.state().log_lines.size() == 12, "the install log loads every line");
    check(model.state().log_offset == 4 && has_row(model, "log-line-11") && !has_row(model, "log-line-3"),
          "the log opens at its newest lines");
    check(model.state().log_context.find("12 lines") != std::string::npos, "the log says how many lines it shows");
    check(has_row(model, "logs-refresh") && !has_row(model, "job-retry-43"), "a general log offers Refresh, not Retry");
    activate(model, "logs-back");

    activate(model, "log-auth");
    check(model.state().log_state == LogState::Ready && model.state().log_lines.empty() && has_row(model, "logs-empty"),
          "a log that does not exist yet is empty, not an error");
    activate(model, "logs-back");
    activate(model, "log-launch");
    check(model.state().log_state == LogState::Failed && has_row(model, "logs-error"), "an unreadable log reports a failure");
    activate(model, "logs-back");
    model.action("circle");

    // From a failed download: that game's lines only, with Retry.
    model.action("l1");
    model.action("l1");
    check(model.state().navigation.route == Route::Downloads, "Downloads open");
    activate(model, "job-43");
    activate(model, "logs");
    check(model.state().log_lines.size() == 6, "a game's log keeps only that game's lines");
    check(std::all_of(model.state().log_lines.begin(), model.state().log_lines.end(),
                      [](const std::string& line) { return line.find("app=43 ") != std::string::npos; }), "only app 43 lines");
    check(model.state().log_context.rfind("Broken · ", 0) == 0, "the game's log names the game");
    check(has_row(model, "job-retry-43"), "a failed download's log offers Retry");
    {
        std::ofstream log(directory / "install.log", std::ios::app);
        log << "2026-10-05T07:01:00Z app=43 step=install result=network detail=\n";
    }
    activate(model, "logs-refresh");
    check(model.state().log_lines.size() == 7, "Refresh reads new lines");
    activate(model, "job-retry-43");
    const auto requests = model.TakeInstallRequests();
    check(requests.size() == 1 && requests[0].kind == InstallRequestKind::Resume && requests[0].game_id == SteamId(43),
          "Retry from the log resumes the download");
    check(model.state().navigation.modals.empty(), "Retry closes the log and the job's actions");

    // Resume all puts every paused download back in the queue.
    model.UpdateInstalls({{SteamId(43), JobState::Paused, 10, 100, {}}, {SteamId(44), JobState::Paused, 0, 0, {}},
                          {SteamId(45), JobState::Downloading, 5, 50, {}}}, {});
    activate(model, "job-45");
    check(has_row(model, "job-resume-all"), "a running download's actions offer Resume all while others wait");
    activate(model, "job-resume-all");
    const auto resumed = model.TakeInstallRequests();
    check(resumed.size() == 2 && resumed[0].kind == InstallRequestKind::Resume && resumed[0].game_id == SteamId(43) &&
              resumed[1].kind == InstallRequestKind::Resume && resumed[1].game_id == SteamId(44),
          "Resume all resumes each paused download");
    check(model.state().navigation.modals.empty(), "Resume all closes the actions");
    model.UpdateInstalls({{SteamId(43), JobState::Paused, 10, 100, {}}, {SteamId(45), JobState::Downloading, 5, 50, {}}}, {});
    activate(model, "job-43");
    check(!has_row(model, "job-resume-all"), "a single paused download offers only its own Resume");
    model.action("circle");
    std::filesystem::remove_all(directory);
}

void live_cloud_saves() {
    using playden::homebrew::CloudRequestKind;
    using playden::homebrew::CloudStatus;
    auto model = make_live();
    model.SetWallClock(10 * 86'400);
    model.UpdateAccount({AuthState::SignedIn, "123", "Player", {}, {}, true});
    check(model.UpdateLibrary({{game(SteamId(42), "Saved Game"), game(SteamId(43), "Not Installed")}, false, {}, false, false, {}}), "library loaded");
    model.UpdateInstalls({}, {{SteamId(42), 100, "internal", true}});

    // A runtime that can't start is said on every screen, with the reason in Settings (#64).
    model.UpdateRuntimeProblem("managed runtime data root is inaccessible");
    check(std::any_of(model.state().notices.begin(), model.state().notices.end(),
                      [](const auto& notice) { return notice.id == "runtime" && notice.persistent; }),
          "a runtime that can't start shows a lasting notice");
    check(model.state().runtime_problem.find("/data/playden") != std::string::npos, "the reason says what to fix");
    model.UpdateRuntimeProblem("");
    check(model.state().runtime_problem.empty() &&
              std::none_of(model.state().notices.begin(), model.state().notices.end(),
                           [](const auto& notice) { return notice.id == "runtime"; }),
          "the notice goes once the runtime starts");

    // Settings turns automatic Cloud saves on and off; it is off by default (a fresh install never
    // changes Cloud saves unasked, #69) and saved with the preferences.
    check(!model.state().cloud_saves_auto && !model.ExportPreferences().cloud_saves_auto, "automatic Cloud saves start off");
    model.action("l1");
    const auto* automatic = find_row(model, "settings-cloud-saves");
    check(automatic && !automatic->selected, "Settings shows automatic Cloud saves off");
    activate(model, "settings-cloud-saves");
    check(model.state().cloud_saves_auto && model.ExportPreferences().cloud_saves_auto, "the toggle turns it on");
    activate(model, "settings-cloud-saves");
    check(!model.state().cloud_saves_auto, "and off again");
    activate(model, "settings-cloud-saves");
    check(model.state().cloud_saves_auto, "and on for what follows");

    check(model.action("details:43") && model.UpdateAppDetails(SteamId(43), 1, 2, true) && !has_row(model, "details-cloud"),
          "a game that is not installed has no Cloud saves row");
    check(model.action("details:42"), "installed game opens");
    const auto* saves = find_row(model, "details-cloud");
    check(saves && saves->enabled && saves->action == "cloud-sync" && saves->value == "Sync", "an installed game offers Cloud saves");
    activate(model, "details-cloud");
    auto requests = model.TakeCloudRequests();
    check(requests.size() == 1 && requests[0].kind == CloudRequestKind::Sync && requests[0].game_id == SteamId(42), "Cloud saves asks for a sync");

    model.UpdateCloud({{SteamId(42), CloudStatus::Checking, "Checking Steam Cloud…"}});
    saves = find_row(model, "details-cloud");
    check(saves && !saves->enabled && saves->value == "Checking…", "a running sync cannot be started twice");
    check(saves->kind == playden::homebrew::RowKind::Action, "Cloud saves stays a button while it works");
    model.UpdateCloud({{SteamId(42), CloudStatus::Synced, "2 saves downloaded"}});
    saves = find_row(model, "details-cloud");
    check(saves && saves->enabled && saves->value == "Synced" && saves->subtitle == "2 saves downloaded", "a finished sync says what moved");
    const auto* synced = find_notice(model, "cloud-42");
    check(synced && synced->message == "Cloud saves synced" && synced->detail == "Saved Game · 2 saves downloaded", "a sync is announced");

    // A conflict opens a dialog describing both sides; the choice goes to the service.
    model.UpdateCloud({{SteamId(42), CloudStatus::Conflict, "Saves here and in Steam Cloud both changed.", 2, 3, 2'048, 4'096,
                        9 * 86'400, 10 * 86'400}});
    check(find_notice(model, "cloud-42")->message == "Cloud saves need a choice", "a pending choice is announced");
    saves = find_row(model, "details-cloud");
    check(saves && saves->action == "cloud-choose" && saves->value == "Choose", "a conflict asks for a choice");
    activate(model, "details-cloud");
    const auto& modal = model.state().navigation.modals.back();
    check(modal.id == "confirmation" && modal.body.find("Steam Cloud: 3 files") != std::string::npos
              && modal.body.find("This console: 2 files") != std::string::npos
              && modal.body.find("changed yesterday") != std::string::npos,
          "the choice dialog describes both sides");
    check(has_row(model, "cloud-use-cloud") && has_row(model, "cloud-use-local"), "both choices are offered");
    activate(model, "cancel");
    model.UpdateCloud({{SteamId(42), CloudStatus::Conflict, "Saves here and in Steam Cloud both changed.", 0, 1, 0, 4'096, 0, 10 * 86'400}});
    activate(model, "details-cloud");
    const auto& empty_side = model.state().navigation.modals.back().body;
    check(empty_side.find("Steam Cloud: 1 file, ") != std::string::npos && empty_side.find("This console: no saves.") != std::string::npos,
          "a side without saves says so, and one file is not \"1 files\"");
    activate(model, "cancel");
    model.UpdateCloud({{SteamId(42), CloudStatus::Conflict, "Saves here and in Steam Cloud both changed.", 2, 3, 2'048, 4'096,
                        9 * 86'400, 10 * 86'400}});
    activate(model, "details-cloud");
    activate(model, "cloud-use-local");
    requests = model.TakeCloudRequests();
    check(requests.size() == 1 && requests[0].kind == CloudRequestKind::UseLocal && model.state().navigation.modals.empty(),
          "choosing local saves goes to the service and closes the dialog");

    // Uploads for an account Playden has not synced before need its confirmation.
    model.UpdateCloud({{SteamId(42), CloudStatus::NeedsAccount, "Confirm this Steam account."}});
    activate(model, "details-cloud");
    check(model.state().navigation.modals.back().body.find("Player") != std::string::npos && has_row(model, "cloud-confirm-account"),
          "the account dialog names the signed-in account");
    activate(model, "cloud-confirm-account");
    requests = model.TakeCloudRequests();
    check(requests.size() == 1 && requests[0].kind == CloudRequestKind::ConfirmAccount, "confirming goes to the service");

    model.UpdateCloud({{SteamId(42), CloudStatus::Failed, "Steam is not reachable. Try again."}});
    check(!model.state().navigation.modals.empty() && model.state().navigation.modals.back().title == "Cloud sync failed",
          "a sync failure waits in a dialog");
    activate(model, "alert-ok");
    check(find_row(model, "details-cloud")->value == "Retry", "a failure can be retried");
}

void refresh_actions_preserve_authentication_and_wall_clock_is_external() {
    auto model = make_live();
    model.UpdateAccount({AuthState::SignedIn, "account", "Owner", {}, {}, false});
    check(model.UpdateLibrary({{game(SteamId(1), "One")}, false, {}, false, true, 100}),
          "baseline for refresh action accepted");
    model.action("l1");
    activate(model, "settings-account");
    activate(model, "account-sync");
    const auto actions = model.TakeServiceActions();
    check(actions.size() == 1 && actions.front() == ServiceAction::RefreshLibrary
              && model.state().account().authentication == AuthState::SignedIn
              && model.state().library_syncing && model.state().library_loading,
          "visible refresh action starts library delivery without changing account authentication");
    check(model.UpdateLibrary({{}, true, {}, false, true, std::nullopt})
              && model.state().account().authentication == AuthState::SignedIn,
          "loading update retains authenticated account state");
    check(model.UpdateLibrary({{game(SteamId(1), "One refreshed")}, false, {}, false, true, 101})
              && model.state().account().authentication == AuthState::SignedIn
              && !model.state().library_syncing && !model.state().library_loading,
          "terminal library update clears delivery status without restoring authentication");

    auto offline = make_live();
    offline.UpdateAccount({AuthState::Offline, "account", "Owner", {}, {}, false});
    check(offline.action("refresh-library")
              && offline.state().account().authentication == AuthState::Offline
              && offline.state().library_syncing && offline.state().library_loading,
          "direct refresh preserves offline authentication state");

    check(model.state().clock_ticks == 0, "live model starts without invented wall time");
    model.SetWallClock(4'102'444'900LL);
    check(model.state().clock_ticks == 4'102'444'900LL,
          "controller can inject current Unix time into live presentation");
    model.SetWallClock(4'102'444'900LL);
    check(model.state().clock_ticks == 4'102'444'900LL,
          "repeating wall time is an idempotent update");

    Simulation simulation("fixtures-not-used", "profile-not-used.json", ModelMode::Simulation);
    const auto fixture_clock = simulation.state().clock_ticks;
    simulation.SetWallClock(1);
    check(simulation.state().clock_ticks == fixture_clock,
          "external wall clock cannot alter deterministic simulation time");
}

void live_empty_filter_keeps_the_signed_in_account() {
    // The Playable filter keeps only games the player rated; with no ratings the grid is empty,
    // and its only row must undo the filter rather than start a new Steam sign-in.
    auto model=make_live();
    model.UpdateAccount({AuthState::SignedIn,"123","Synthetic",{},{},true});
    check(model.UpdateLibrary({{game(SteamId(42),"Synthetic")},false,{},false,false,{}}),"library loaded");
    model.TakeServiceActions();
    check(model.action("r1") && model.action("triangle"),"live filter modal opens");
    activate(model,"filter-compat-playable");
    check(model.visible_games().empty(),"no rated game passes the Playable filter");
    const auto* empty=find_row(model,"library-empty");
    check(empty!=nullptr && empty->action=="filters-reset" && empty->title=="Reset filters",
          "a filtered-out library offers Reset filters, not Connect Steam");
    activate(model,"library-empty");
    const auto actions=model.TakeServiceActions();
    check(std::find(actions.begin(),actions.end(),ServiceAction::BeginQr)==actions.end() &&
          model.state().account().authentication==AuthState::SignedIn,"resetting filters keeps the Steam sign-in");
    check(model.state().library.filters.empty() && model.visible_games().size()==1 &&
          model.state().navigation.focus_key=="game-42","reset filters shows the library again");

    // An empty scope offers the whole library back.
    check(model.action("left") && model.action("down") && model.state().navigation.focus_key=="rail-favorites" &&
          model.action("cross"),"Favorites scope opens");
    check(model.visible_games().empty(),"no favorites yet");
    empty=find_row(model,"library-empty");
    check(empty!=nullptr && empty->action=="library-show-all","an empty scope offers Show all games");
    activate(model,"library-empty");
    check(model.state().library.scope=="all" && model.visible_games().size()==1 &&
          model.state().account().authentication==AuthState::SignedIn,"Show all games returns to every game");

    // Without a catalog there is nothing to show yet, so Connect Steam stays.
    auto signed_out=make_live();
    check(signed_out.action("r1"),"library opens before sign-in");
    empty=find_row(signed_out,"library-empty");
    check(empty==nullptr || empty->action=="connect","a missing catalog still offers Connect Steam");
}

void live_filters_only_use_available_facts() {
    auto model=make_live();
    model.UpdateAccount({AuthState::SignedIn,"123","Synthetic",{},{},true});
    check(model.UpdateLibrary({{game(SteamId(42),"Synthetic")},false,{},false,false,{}}),"library loaded");
    check(model.state().games.front().folded_title=="synthetic","production delivery caches Unicode key once");
    check(model.action("r1") && model.action("triangle"),"live filter modal opens");
    const auto& actions=model.state().navigation.modals.back().action_keys;
    for(const auto& unavailable:{"sort-recent-added","sort-install-size","filter-installed","filter-genre-adventure","filter-controller-full"})
        check(std::find(actions.begin(),actions.end(),unavailable)==actions.end(),"unknown facts are not filter choices");
    check(std::find(actions.begin(),actions.end(),"filter-compat-playable")!=actions.end(),"personal compatibility remains filterable");
    check(std::find(actions.begin(),actions.end(),"filter-hide-demos")==actions.end(),"a library without demos has no Demos row");

    // A CM acquisition date makes "Recently added" a real fact, sorted newest first.
    auto dated=make_live();
    dated.UpdateAccount({AuthState::SignedIn,"123","Synthetic",{},{},true});
    auto older=game(SteamId(42),"Alpha"),newer=game(SteamId(43),"Beta"),undated=game(SteamId(44),"Gamma");
    older.added_at_unix_seconds=1400000000;newer.added_at_unix_seconds=1500000000;
    check(dated.UpdateLibrary({{older,newer,undated},false,{},false,false,{}}),"dated library loaded");
    check(dated.state().games.front().added_at==1400000000,"acquisition date projected");
    check(dated.action("r1") && dated.action("triangle"),"dated filter modal opens");
    const auto& dated_actions=dated.state().navigation.modals.back().action_keys;
    check(std::find(dated_actions.begin(),dated_actions.end(),"sort-recent-added")!=dated_actions.end(),"recently added offered when dates exist");
    activate(dated,"sort-recent-added");
    check(dated.state().library.sort=="recent-added","recently added sort selected");
    const auto ordered=dated.visible_games();
    check(ordered.size()==3 && ordered[0]->id==SteamId(43) && ordered[1]->id==SteamId(42) && ordered[2]->id==SteamId(44),
          "newest acquisition first, undated last");
}

// Demos show by default. Sort and filters › Demos hides them, the organization keeps that, and the
// row stays while demos are hidden so they can be shown again.
void live_demos_filter_defaults_on() {
    auto model=make_live();
    model.UpdateAccount({AuthState::SignedIn,"123","Synthetic",{},{},true});
    auto demo=game(SteamId(43),"Synthetic Demo");demo.demo=true;
    check(model.UpdateLibrary({{game(SteamId(42),"Synthetic"),demo},false,{},false,false,{}}),"library with a demo loaded");
    check(model.state().games.back().demo && !model.state().games.front().demo,"demo mark projected");
    check(model.action("r1") && model.visible_games().size()==2,"demos show by default");
    check(model.action("triangle"),"live filter modal opens");
    const auto* row=find_row(model,"filter-hide-demos");
    check(row!=nullptr && row->title=="Demos" && row->kind==RowKind::Toggle && row->selected && row->value=="On","Demos is a toggle, on");
    activate(model,"filter-hide-demos");
    check(model.visible_games().size()==1 && model.visible_games().front()->id==SteamId(42),"turning Demos off hides the demo");
    check(model.ExportOrganization().filters==std::set<std::string>{"hide-demos"},"hidden demos are saved");
    check(model.action("triangle"),"filter modal opens again");
    row=find_row(model,"filter-hide-demos");
    check(row!=nullptr && !row->selected && row->value=="Off","Demos shows off");
    activate(model,"filter-hide-demos");
    check(model.visible_games().size()==2 && model.ExportOrganization().filters.empty(),"turning Demos on shows it again");

    // Hidden demos the library no longer has can still be shown again.
    auto restored=make_live();
    restored.UpdateAccount({AuthState::SignedIn,"123","Synthetic",{},{},true});
    check(restored.UpdateLibrary({{game(SteamId(42),"Synthetic")},false,{},false,false,{}}),"library without demos loaded");
    auto organization=restored.ExportOrganization();organization.filters={"hide-demos"};
    check(restored.ApplyOrganization(1,restored.BeginOrganizationLoad(1),organization),"saved organization applies");
    check(restored.action("r1") && restored.action("triangle"),"filter modal opens");
    row=find_row(restored,"filter-hide-demos");
    check(row!=nullptr && !row->selected,"Demos stays listed while demos are hidden");
}

// A game a Steam family member shares keeps that mark in the Library; the rest of the library does not.
void live_family_shared_projected() {
    auto model=make_live();
    model.UpdateAccount({AuthState::SignedIn,"123","Synthetic",{},{},true});
    auto shared=game(SteamId(43),"Synthetic Shared");shared.family_shared=true;
    check(model.UpdateLibrary({{game(SteamId(42),"Synthetic"),shared},false,{},false,false,{}}),"library with a shared game loaded");
    check(model.state().games.back().family_shared && !model.state().games.front().family_shared,"family mark projected");
    check(model.action("r1") && model.visible_games().size()==2,"shared games show with the rest");
}

// Family sharing works like Demos: on by default, turning it off hides shared games and is saved, and the
// row is listed only while the library has a shared game or they are hidden.
void live_family_filter_defaults_on() {
    auto model=make_live();
    model.UpdateAccount({AuthState::SignedIn,"123","Synthetic",{},{},true});
    check(model.UpdateLibrary({{game(SteamId(42),"Synthetic")},false,{},false,false,{}}),"library without shared games loaded");
    check(model.action("r1") && model.action("triangle"),"live filter modal opens");
    check(find_row(model,"filter-hide-family")==nullptr,"a library without shared games has no Family sharing row");
    model.action("circle");

    auto shared=game(SteamId(43),"Synthetic Shared");shared.family_shared=true;auto demo=game(SteamId(44),"Synthetic Demo");demo.demo=true;
    check(model.UpdateLibrary({{game(SteamId(42),"Synthetic"),shared,demo},false,{},false,false,{}}),"library with a shared game loaded");
    check(model.action("triangle"),"filter modal opens");
    const auto* row=find_row(model,"filter-hide-family");
    check(row!=nullptr && row->title=="Family sharing" && row->kind==RowKind::Toggle && row->selected && row->value=="On",
          "Family sharing is a toggle, on");
    activate(model,"filter-hide-family");
    check(model.visible_games().size()==2 && model.ExportOrganization().filters==std::set<std::string>{"hide-family"},
          "turning Family sharing off hides the shared game and is saved");
    check(model.action("triangle"),"filter modal opens again");
    activate(model,"filter-hide-demos");
    check(model.visible_games().size()==1 && model.visible_games().front()->id==SteamId(42),"both filters compose");
    check(model.ExportOrganization().filters==std::set<std::string>{"hide-demos","hide-family"},"both hidden kinds are saved");
    check(model.action("triangle"),"filter modal opens again");
    row=find_row(model,"filter-hide-family");
    check(row!=nullptr && !row->selected && row->value=="Off","Family sharing shows off");
    activate(model,"filter-hide-family");
    check(model.visible_games().size()==2 && model.ExportOrganization().filters==std::set<std::string>{"hide-demos"},
          "turning Family sharing on shows it again");
}

// Sort and filters offers Refresh library: it asks the controller for Steam's current list and the
// Library stays on screen. While a refresh runs, the row waits.
void live_sort_sheet_refreshes_the_library() {
    auto model=make_live();
    model.UpdateAccount({AuthState::SignedIn,"123","Synthetic",{},{},true});
    check(model.UpdateLibrary({{game(SteamId(42),"Synthetic")},false,{},false,false,{}}),"library loaded");
    model.TakeServiceActions();
    check(model.action("r1") && model.action("triangle"),"live filter modal opens");
    const auto* row=find_row(model,"refresh-library");
    check(row!=nullptr && row->title=="Refresh library" && row->enabled,"the sheet offers Refresh library");
    activate(model,"refresh-library");
    const auto actions=model.TakeServiceActions();
    check(actions.size()==1 && actions.front()==ServiceAction::RefreshLibrary,"Refresh library asks for a refresh");
    check(model.state().navigation.modals.empty() && model.state().navigation.route==Route::Library &&
          model.state().library_syncing && model.state().library_loading &&
          model.state().account().authentication==AuthState::SignedIn,"the Library stays while it refreshes");
    check(model.action("triangle"),"filter modal opens while refreshing");
    row=find_row(model,"refresh-library");
    check(row!=nullptr && !row->enabled,"a refresh in progress is not started again");
    check(model.action("focus:refresh-library") && model.action("cross") && model.TakeServiceActions().empty(),
          "the disabled row asks for nothing");
}

// "More" on Continue playing shows the Library most recently played first, for that visit only: the
// sort the player chose stays saved and comes back when the Library is opened again.
void continue_playing_more_keeps_the_chosen_sort() {
    auto model=make_live();
    model.UpdateAccount({AuthState::SignedIn,"123","Synthetic",{},{},true});
    std::vector<GameSummary> games;
    for(std::uint32_t id=1;id<=25;++id) {
        auto summary=game(SteamId(id),"Game "+std::to_string(100+id));
        summary.last_played_unix_seconds=1'700'000'000ULL+id;
        summary.playtime_minutes=id%2 ? 1000+id : id;
        games.push_back(std::move(summary));
    }
    check(model.UpdateLibrary({std::move(games),false,{},false,false,{}}),"played library loaded");
    check(model.action("r1") && model.action("triangle"),"live filter modal opens");
    activate(model,"sort-playtime");
    check(model.state().library.sort=="playtime" && model.visible_games().front()->id==SteamId(25),"playtime sort chosen");
    check(model.action("l1") && model.state().navigation.route==Route::Home,"back to Home");
    for(int step=0;step<30 && model.state().navigation.focus_key!="shelf-continue-more";++step) model.action("right");
    check(model.state().navigation.focus_key=="shelf-continue-more","More on Continue playing focused");
    check(model.action("cross") && model.state().navigation.route==Route::Library,"More opens the Library");
    const auto more=model.visible_games();
    check(more.size()==25 && more.front()->id==SteamId(25) && more[1]->id==SteamId(24),"More lists the most recently played first");
    check(model.state().library.sort=="playtime" && model.ExportOrganization().sort=="playtime",
          "More does not replace the saved sort");
    check(model.action("details:24") && model.action("circle") && model.state().navigation.route==Route::Library &&
          model.visible_games()[1]->id==SteamId(24),"a game opened from More returns to the same order");
    check(model.action("l1") && model.action("r1") && model.state().navigation.route==Route::Library,"Library opened again");
    const auto again=model.visible_games();
    check(again.front()->id==SteamId(25) && again[1]->id==SteamId(23),"the chosen playtime sort is back");

    // Choosing a sort in the More view keeps it.
    check(model.action("l1"),"back to Home");
    for(int step=0;step<30 && model.state().navigation.focus_key!="shelf-continue-more";++step) model.action("right");
    check(model.action("cross") && model.action("triangle"),"sort sheet from the More view");
    activate(model,"sort-name");
    check(model.state().library.sort=="name" && model.visible_games().front()->id==SteamId(1),"a chosen sort replaces the More order");
    check(model.action("l1") && model.action("r1") && model.state().library.sort=="name" &&
          model.visible_games().front()->id==SteamId(1),"and stays chosen");
}

void sync_automatically(Simulation& model);

void live_uninstall_removes_title_and_keeps_saves() {
    using playden::homebrew::InstallRequestKind;
    using playden::homebrew::LiveRuntimeGame;
    using playden::homebrew::RowKind;
    auto model = make_live();
    model.UpdateAccount({AuthState::SignedIn, "123", "Synthetic", {}, {}, true});
    check(model.UpdateLibrary({{game(SteamId(42), "Synthetic"), game(SteamId(43), "No title")}, false, {}, false, false, {}}), "library loaded");
    model.UpdateAppDetails(SteamId(42), 300, 600, true);
    model.UpdateInstalls({}, {{SteamId(42), 600'000'000}, {SteamId(43), 100}});
    LiveRuntimeGame ready;
    ready.game_id = SteamId(42); ready.status = "Ready"; ready.ready = true; ready.can_play = true; ready.has_title = true;
    LiveRuntimeGame plain;
    plain.game_id = SteamId(43); plain.status = "Not prepared"; plain.can_prepare = true;
    model.UpdateRuntime(playden::homebrew::RuntimeState::Ready, {ready, plain});

    // With automatic Cloud sync off the uninstall does not sync, and the dialog does not say it does.
    check(model.action("details:42") && model.action("focus:details-more") && model.action("cross"), "context opens");
    activate(model, "uninstall");
    check(model.state().navigation.modals.back().body.find("Steam Cloud saves are synced first") == std::string::npos &&
              model.state().navigation.modals.back().body.find("Automatic Cloud sync is off, so its saves are not synced first.") !=
                  std::string::npos,
          "with automatic sync off the dialog says the saves are not synced first");
    activate(model, "cancel");
    while (!model.state().navigation.modals.empty()) model.action("circle");
    sync_automatically(model);

    // A prepared game: the dialog says the PS5 title goes too and offers deleting saves, off by default.
    check(model.action("details:42") && model.action("focus:details-more") && model.action("cross"), "context opens");
    activate(model, "uninstall");
    const auto& modal = model.state().navigation.modals.back();
    check(modal.body.find("PS5 title are removed") != std::string::npos, "the dialog names the PS5 title");
    check(modal.body.find("Steam Cloud saves are synced first") != std::string::npos, "with automatic sync on, Cloud is synced first");
    const auto* toggle = find_row(model, "uninstall-delete-saves");
    check(toggle && toggle->kind == RowKind::Toggle && !toggle->selected, "deleting saves is a choice, off by default");
    activate(model, "uninstall-delete-saves");
    toggle = find_row(model, "uninstall-delete-saves");
    check(toggle && toggle->selected && !model.state().navigation.modals.empty(), "the choice toggles in the open dialog");
    check(model.state().navigation.modals.back().body.find("saves and settings on this console are deleted") != std::string::npos &&
          model.state().navigation.modals.back().body.find("are kept") == std::string::npos,
          "the dialog says the saves go when the choice is on");
    activate(model, "confirm-destructive");
    auto requests = model.TakeInstallRequests();
    check(requests.size() == 1 && requests[0].kind == InstallRequestKind::Uninstall && requests[0].game_id == SteamId(42) &&
              requests[0].delete_saves, "Uninstall carries the saves choice");
    check(model.action("details:42"), "details reopen after confirm");
    const auto* queued_play = find_row(model, "details-play");
    check(queued_play && !queued_play->enabled && queued_play->title == "Uninstall queued" &&
              queued_play->value == "Uninstalling…", "Play shows Uninstall queued while pending");
    const auto* queued_cloud = find_row(model, "details-cloud");
    check(queued_cloud && !queued_cloud->enabled && queued_cloud->action.empty(),
          "Cloud saves is disabled while uninstall is queued");

    // While it runs, Play waits; an interrupted removal offers Finish removing.
    ready.uninstall_status = "Removing PS5 title…"; ready.can_play = false;
    model.UpdateRuntime(playden::homebrew::RuntimeState::Ready, {ready, plain});
    check(model.action("details:42"), "details reopen");
    const auto* play = find_row(model, "details-play");
    check(play && !play->enabled && play->value == "Removing PS5 title…", "Play shows the uninstall in progress");
    check(play && play->title == "Uninstalling", "Play shows Uninstalling label");
    const auto* active_cloud = find_row(model, "details-cloud");
    check(active_cloud && !active_cloud->enabled && active_cloud->action.empty(),
          "Cloud saves is disabled during active uninstall");
    ready.progress = 0.42F;
    model.UpdateRuntime(playden::homebrew::RuntimeState::Ready, {ready, plain});
    play = find_row(model, "details-play");
    check(play && !play->enabled && play->title == "Uninstalling" && play->progress == 0.42F &&
              play->value == "Removing PS5 title 42%", "Uninstall progress shows percentage and Uninstalling title");
    ready.progress.reset();
    check(find_row(model, "details-setup") && find_row(model, "details-setup")->value == "Removing",
          "active uninstall replaces Ready with Removing in setup facts");
    check(!has_row(model, "details-hint"), "active uninstall has no interrupted setup hint");
    LiveRuntimeGame stuck;
    stuck.game_id = SteamId(42); stuck.status = "Removal incomplete"; stuck.can_prepare = true; stuck.has_title = true;
    stuck.detail = "The console has not confirmed that the PS5 title is gone. Retry checks again.";
    model.UpdateRuntime(playden::homebrew::RuntimeState::Ready, {stuck, plain});
    check(model.action("details:42"), "details reopen");
    const auto* retry = find_row(model, "details-play");
    check(retry && retry->title == "Finish removing" && retry->enabled && retry->action == "runtime-remove-retry",
          "an unfinished removal offers Finish removing");
    activate(model, "details-play");
    requests = model.TakeInstallRequests();
    check(requests.size() == 1 && requests[0].kind == InstallRequestKind::Uninstall && !requests[0].delete_saves,
          "Retry continues the uninstall with the saved choice");
    model.NotifyUninstallStopped(SteamId(42), "The PS5 title could not be removed.");
    check(!model.state().navigation.modals.empty() && model.state().navigation.modals.back().title == "Uninstall stopped" &&
              model.state().navigation.modals.back().action_keys == std::vector<std::string>{"alert-ok"},
          "a stopped uninstall says why in a dialog with OK");
    activate(model, "alert-ok");
    check(model.state().navigation.modals.empty(), "OK dismisses the failure dialog");
    model.NotifyPlayStopped(SteamId(42), "This game's program has Steam DRM that Playden can't unpack on the PS5 yet.");
    model.NotifyPlayStopped(SteamId(42), "This game's program has Steam DRM that Playden can't unpack on the PS5 yet.");
    check(model.state().navigation.modals.size() == 1 && model.state().navigation.modals.back().title == "Can't play" &&
              model.state().navigation.modals.back().body.find("Steam DRM") != std::string::npos,
          "a refused Play says why, once however often it is pressed");
    activate(model, "alert-ok");
    check(model.state().navigation.modals.empty(), "a repeated refusal is not asked twice");

    // A game without a PS5 title and without Cloud: no saves choice; its saves exist only here.
    model.UpdateCloud({{SteamId(43), playden::homebrew::CloudStatus::Unsupported, "No Steam Cloud saves"}});
    check(model.action("details:43") && model.action("focus:details-more") && model.action("cross"), "context opens");
    activate(model, "uninstall");
    check(!has_row(model, "uninstall-delete-saves"), "no saves choice without a PS5 title");
    check(model.state().navigation.modals.back().body.find("exist only on this console") != std::string::npos,
          "without Cloud the dialog says the saves exist only here");
    activate(model, "cancel");
    while (!model.state().navigation.modals.empty()) model.action("circle");

    // An uninstalled game with kept saves: Details shows them and can delete them.
    model.UpdateInstalls({}, {{SteamId(43), 100}});
    model.UpdateKeptBottles({{SteamId(42), "Synthetic", 1760000000, 5'000'000}});
    check(model.action("details:42"), "details open");
    const auto* kept = find_row(model, "details-kept");
    check(kept && kept->title == "Saves from a previous install" && !kept->value.empty(), "kept saves are shown with their size");
    check(model.state().navigation.focus_key != "details-kept-delete", "Delete saves is never focused by default");
    {
        // While Steam is still checked there is no Install yet: focus still avoids Delete saves.
        model.UpdateInstalls({}, {});
        model.UpdateKeptBottles({{SteamId(42), "Synthetic", 1760000000, 5'000'000}, {SteamId(43), "No title", 1760000000, 1'000'000}});
        check(model.action("details:43") && has_row(model, "details-checking") && has_row(model, "details-kept-delete"),
              "kept saves while Steam is checked");
        check(model.state().navigation.focus_key != "details-kept-delete", "a second Cross cannot land on Delete saves");
        model.UpdateInstalls({}, {{SteamId(43), 100}});
        model.UpdateKeptBottles({{SteamId(42), "Synthetic", 1760000000, 5'000'000}});
        check(model.action("details:42"), "details reopen");
    }
    check(has_row(model, "details-install"), "Install brings them back");
    activate(model, "details-kept-delete");
    check(model.state().navigation.modals.back().title == "Delete kept saves?", "deleting asks first");
    activate(model, "confirm-destructive");
    auto deletes = model.TakeKeptDeleteRequests();
    check(deletes.size() == 1 && deletes[0] == SteamId(42), "the confirmed deletion goes to the runtime");

    // Settings shows kept game data as one row that opens the manager.
    model.UpdateKeptBottles({{SteamId(42), "Synthetic", 1760000000, 5'000'000}, {SteamId(43), "", 1760000000, 3'000'000},
                             {SteamId(44), "Third", 0, 1'000'000}});
    model.action("l1");
    check(model.state().navigation.route == playden::homebrew::Route::Settings, "Settings opens");
    check(!has_row(model, "settings-kept-42"), "kept games are not listed one by one in Settings");
    const auto* entry = find_row(model, "settings-kept");
    check(entry && entry->title == "Kept game data" && entry->value.find("3 games · ") == 0, "the row counts kept games and size");
    activate(model, "settings-kept");
    check(model.state().navigation.modals.back().id == "kept-data", "the row opens the kept game data manager");
    const auto* named = find_row(model, "kept-toggle-43");
    check(named && named->title == "No title" && named->kind == RowKind::Toggle && !named->selected,
          "a game without a kept name uses the library's, unchecked");
    check(find_row(model, "kept-toggle-42")->subtitle == "Kept 2025-10-09", "each game shows when it was kept");
    check(!find_row(model, "kept-selection-delete")->enabled, "nothing to delete until a game is checked");
    activate(model, "kept-toggle-43");
    check(find_row(model, "kept-toggle-43")->selected && !model.state().navigation.modals.empty(), "checking stays in the manager");
    check(find_row(model, "kept-selection-delete")->value.find("1 game · ") == 0, "Delete names how many");
    activate(model, "kept-select-all");
    check(find_row(model, "kept-toggle-42")->selected && find_row(model, "kept-toggle-44")->selected, "Select all checks every game");
    check(find_row(model, "kept-select-all")->title == "Clear selection", "with all checked it clears instead");
    activate(model, "kept-select-all");
    check(!find_row(model, "kept-toggle-42")->selected, "Clear selection unchecks every game");
    activate(model, "kept-toggle-42");
    activate(model, "kept-toggle-44");
    activate(model, "kept-selection-delete");
    const auto& confirm = model.state().navigation.modals.back();
    check(confirm.title == "Delete kept game data?" && confirm.body.find("2 games (6 MB)") != std::string::npos,
          "the confirmation names how many games and how much space");
    activate(model, "cancel");
    check(model.TakeKeptDeleteRequests().empty() && model.state().navigation.modals.back().id == "kept-data",
          "cancelling deletes nothing and returns to the manager");
    activate(model, "kept-selection-delete");
    activate(model, "confirm-destructive");
    deletes = model.TakeKeptDeleteRequests();
    check(deletes == std::vector<playden::homebrew::GameId>{SteamId(42), SteamId(44)}, "the confirmed games go to the runtime");
    check(model.state().navigation.modals.empty() && model.state().navigation.focus_key == "settings-kept",
          "the manager closes and focus returns to its row");

    // Kept data changes while the manager is open: it lists what is left, and closes when nothing is.
    activate(model, "settings-kept");
    model.UpdateKeptBottles({{SteamId(43), "", 1760000000, 3'000'000}});
    check(!has_row(model, "kept-toggle-42") && has_row(model, "kept-toggle-43"), "the open manager follows the kept list");
    model.UpdateKeptBottles({});
    check(model.state().navigation.modals.empty(), "the manager closes when nothing is kept");
    entry = find_row(model, "settings-kept");
    check(entry && entry->value == "None" && !entry->enabled, "nothing kept says so");
    check(model.state().navigation.focus_key == "settings-kept", "focus stays on the row");
}

std::vector<std::string> installed_order(const Simulation& model) {
    std::vector<std::string> order;
    for (const auto& row : model.view().rows)
        if (row.key.rfind("manage-toggle-", 0) == 0) order.push_back(row.key.substr(14));
    return order;
}

void live_storage_section_and_installed_games() {
    using playden::homebrew::InstallRequestKind;
    using playden::homebrew::JobState;
    using playden::homebrew::LiveInstallJob;
    using playden::homebrew::LiveRuntimeGame;
    using playden::homebrew::RuntimeState;
    constexpr std::int64_t now = 1'800'000'000;
    constexpr std::int64_t day = 86'400;
    constexpr std::uint64_t gb = 1'000'000'000;
    auto model = make_live();
    model.SetWallClock(now);
    model.UpdateAccount({AuthState::SignedIn, "123", "Synthetic", {}, {}, true});
    model.UpdateVolumes({{"internal", "Console storage", true, 800 * gb, 500 * gb}}, "internal");

    // Nothing installed: the Storage rows say so, and leftovers stay hidden.
    model.action("l1");
    check(model.state().navigation.route == Route::Settings, "Settings opens");
    const auto section_of = [&model](const std::string& key) {
        const auto* row = find_row(model, key);
        return row ? row->section : std::string("missing");
    };
    for (const auto* key : {"settings-storage", "settings-installed", "settings-kept", "settings-runtime"})
        check(section_of(key) == "Storage", "the storage rows are in the Storage section");
    std::vector<std::string> library;
    for (const auto& row : model.view().rows) if (row.section == "Library") library.push_back(row.key);
    check(library == std::vector<std::string>{"settings-import", "settings-title-store", "settings-cloud-saves"},
          "Library starts with Import games, then the store name and Cloud saves rows");
    check(find_row(model, "settings-storage")->action == "storage-picker", "Games storage opens the drive picker");
    std::vector<std::string> order;
    for (const auto& row : model.view().rows) if (row.section == "Storage") order.push_back(row.key);
    check(order == std::vector<std::string>{"settings-storage", "settings-installed", "settings-kept", "settings-runtime"},
          "Storage lists its rows in order, without the hidden cleanup row");
    std::vector<std::string> downloads;
    for (const auto& row : model.view().rows) if (row.section == "Downloads") downloads.push_back(row.key);
    check(downloads == std::vector<std::string>{"settings-content-language", "settings-downloads-slow",
                                                "settings-download-server"},
          "Downloads holds the download language, the slow-network toggle and the download server");
    {
        const auto* language = find_row(model, "settings-content-language");
        check(language->title == "Download language" && language->value == "English" &&
                  language->subtitle == "Used for new installs. A game without it falls back to English or its first language.",
              "the download language row names the language in use");
    }
    // Downloads for slow networks (#29): off by default, a toggle saved with the preferences.
    check(!find_row(model, "settings-downloads-slow")->selected && !model.ExportPreferences().downloads_slow_network,
          "standard downloads by default");
    activate(model, "settings-downloads-slow");
    check(find_row(model, "settings-downloads-slow")->selected && model.ExportPreferences().downloads_slow_network,
          "the toggle turns downloads for slow networks on");
    activate(model, "settings-downloads-slow");
    check(!model.ExportPreferences().downloads_slow_network, "and off again");
    std::vector<std::string> sections;
    for (const auto& row : model.view().rows)
        if (sections.empty() || sections.back() != row.section) sections.push_back(row.section);
    check(sections == std::vector<std::string>{"Account", "Library", "Storage", "Downloads", "Display", "Updates", "Web access",
                                                  "About", "Quit Playden"},
          "the rail order is Account, Library, Storage, Downloads, Display, Updates, Web access, About, Quit");
    // Web access: on by default; the address and its QR once the server reports them; the reason when it can't open.
    check(find_row(model, "settings-web-access")->selected && model.ExportPreferences().web_access, "web access is on by default");
    check(!find_row(model, "settings-web-address"), "no address before the server reports one");
    model.ApplyLiveWebAccess({"on", "http://192.168.1.50:7031/", 7031, {}});
    const auto* address = find_row(model, "settings-web-address");
    check(address && address->qr == "http://192.168.1.50:7031/" && address->value == "http://192.168.1.50:7031" &&
              address->section == "Web access" && address->subtitle.find("your Steam name removed") != std::string::npos &&
              !address->enabled,
          "the page's address shows with its QR code");
    model.ApplyLiveWebAccess({"on", "", 7032, {}});
    check(find_row(model, "settings-web-address") && find_row(model, "settings-web-address")->qr.empty() &&
              find_row(model, "settings-web-address")->title == "Address unknown" &&
              find_row(model, "settings-web-address")->value == "port 7032",
          "an unknown address is said, never guessed");
    model.ApplyLiveWebAccess({"failed", "", 0, "port 7039: Address already in use (errno 48)"});
    check(find_row(model, "settings-web-problem") &&
              find_row(model, "settings-web-problem")->subtitle.rfind(
                  "Web access could not start: port 7039: Address already in use (errno 48).", 0) == 0,
          "a port that could not be opened says why");
    activate(model, "settings-web-access");
    check(!find_row(model, "settings-web-access")->selected && !model.ExportPreferences().web_access &&
              !find_row(model, "settings-web-problem") && !find_row(model, "settings-web-address"),
          "the toggle turns web access off");
    activate(model, "settings-web-access");
    check(model.ExportPreferences().web_access, "and on again");
    check(!model.state().quit_requested, "not quitting yet");
    model.RequestQuit();
    check(model.state().quit_requested, "the page's Quit Playden quits when nothing is being prepared");
    const auto* empty = find_row(model, "settings-installed");
    check(empty && !empty->enabled && empty->value == "None", "nothing installed says None");
    check(find_row(model, "settings-kept")->value == "None" && !has_row(model, "settings-runtime-cleanup"),
          "kept data says None and the cleanup row is hidden");
    model.SetRuntimeLeftovers(1);
    order.clear();
    for (const auto& row : model.view().rows) if (row.section == "Storage") order.push_back(row.key);
    check(order == std::vector<std::string>{"settings-storage", "settings-installed", "settings-kept",
                                            "settings-runtime-cleanup", "settings-runtime"},
          "unused settings data sits before Runtime status when there is some");
    model.SetRuntimeLeftovers(0);
    // The Settings drive picker changes the default, not one install's drive.
    activate(model, "settings-storage");
    check(model.state().navigation.modals.back().id == "storage" &&
              model.state().navigation.modals.back().title == "Choose storage", "Games storage opens the default picker");
    model.action("circle");
    // Up and Down stay in the section; the rail moves to another one.
    check(model.action("focus:settings-storage"), "Games storage is focused");
    model.action("up");
    check(model.state().navigation.focus_key == "settings-storage" &&
              model.view().rows[static_cast<std::size_t>(model.view().focused_row)].section == "Storage",
          "Up on Storage's first row does not leave the section");
    if (model.state().navigation.tabs_focused) model.action("down");
    check(model.action("focus:settings-download-server"), "Download server is focused");
    model.action("down");
    check(model.state().navigation.focus_key == "settings-download-server", "Down on Downloads' last row stays on it");
    model.action("left");
    model.action("down");
    model.action("right");
    check(model.state().navigation.focus_key == "settings-motion", "the rail moves to the next section");

    const auto played = [&](playden::GameId id, std::string title, std::optional<std::int64_t> days) {
        auto summary = game(id, std::move(title));
        if (days) summary.last_played_unix_seconds = static_cast<std::uint64_t>(now - *days * day);
        return summary;
    };
    check(model.UpdateLibrary({{played(SteamId(10), "Alpha", 10), played(SteamId(11), "Beta", std::nullopt), played(SteamId(12), "Gamma", 100),
                                played(SteamId(13), "Delta", 1), played(SteamId(14), "Echo", 50), played(SteamId(15), "Foxtrot", 30),
                                played(SteamId(16), "Golf", 20)}, false, {}, false, false, {}}), "library loaded");
    model.UpdateInstalls({LiveInstallJob{SteamId(15), JobState::Queued, 0, gb, {}, "internal"}},
                         {{SteamId(10), 5 * gb, "internal"}, {SteamId(11), 20 * gb, "internal"}, {SteamId(12), 5 * gb, "internal"},
                          {SteamId(13), 1 * gb, "internal"}, {SteamId(14), 2 * gb, "internal"}, {SteamId(15), 3 * gb, "internal"},
                          {SteamId(16), 4 * gb, "internal"}});
    LiveRuntimeGame titled; titled.game_id = SteamId(10); titled.status = "Ready"; titled.ready = true; titled.has_title = true;
    LiveRuntimeGame running; running.game_id = SteamId(14); running.status = "Ready"; running.ready = true; running.play_status = "Playing";
    LiveRuntimeGame preparing; preparing.game_id = SteamId(16); preparing.status = "Preparing";
    model.UpdateRuntime(RuntimeState::Ready, {titled, running, preparing});
    const auto* entry = find_row(model, "settings-installed");
    check(entry && entry->enabled && entry->value == "7 games · 40.0 GB", "the row counts installed games and size");

    // Largest first, ties by title; then never played first and the longest unplayed after.
    activate(model, "settings-installed");
    check(model.state().navigation.route == Route::ManageGames && model.state().navigation.modals.empty() &&
              model.state().navigation.manage_tab == playden::homebrew::ManageTab::Uninstall,
          "the row opens Manage installed games on its Uninstall tab");
    check(model.state().navigation.focus_key == "manage-toggle-11", "focus starts on the first game");
    check(installed_order(model) == std::vector<std::string>{"11", "10", "12", "16", "15", "14", "13"}, "sorted by size");
    check(find_row(model, "manage-sort")->value == "Size", "the sort row names the order");
    const auto* alpha = find_row(model, "manage-toggle-10");
    check(alpha && alpha->kind == RowKind::Toggle && alpha->title == "Alpha" && alpha->value == "5.0 GB" &&
              alpha->subtitle == "Console storage" && alpha->badge == "10 days ago" && alpha->enabled && !alpha->selected,
          "a game row shows its drive, its size and when it was last played");
    check(find_row(model, "manage-toggle-11")->badge == "Never", "a game never played says so");
    activate(model, "manage-sort");
    check(find_row(model, "manage-sort")->value == "Last played" && model.state().navigation.focus_key == "manage-sort",
          "Sort switches to last played and keeps the focus");
    check(installed_order(model) == std::vector<std::string>{"11", "12", "14", "15", "16", "10", "13"},
          "never played first, then the oldest");
    activate(model, "manage-sort");

    // Games that can't go now say why and can't be checked.
    const auto* echo = find_row(model, "manage-toggle-14");
    const auto* foxtrot = find_row(model, "manage-toggle-15");
    const auto* golf = find_row(model, "manage-toggle-16");
    check(echo && !echo->enabled && echo->subtitle == "Playing: close it to uninstall" && echo->badge == "Playing now",
          "a running game is blocked");
    check(foxtrot && !foxtrot->enabled && foxtrot->subtitle == "In Queue", "a game with a download is blocked");
    check(golf && !golf->enabled && golf->subtitle == "Busy", "a game being prepared is blocked");
    activate(model, "manage-toggle-14");
    check(!find_row(model, "manage-toggle-14")->selected, "a blocked game can't be checked");
    check(!find_row(model, "manage-primary")->enabled, "nothing to uninstall until a game is checked");
    activate(model, "manage-select-all");
    for (const auto* key : {"manage-toggle-10", "manage-toggle-11", "manage-toggle-12", "manage-toggle-13"})
        check(find_row(model, key)->selected, "Select all checks every game that can go");
    for (const auto* key : {"manage-toggle-14", "manage-toggle-15", "manage-toggle-16"})
        check(!find_row(model, key)->selected, "Select all skips blocked games");
    check(find_row(model, "manage-select-all")->title == "Clear selection", "with all checked it clears instead");
    check(model.view().footer == "4 selected · frees 31.0 GB", "the footer names how many and how much");
    check(find_row(model, "manage-primary")->title == "Uninstall 4…", "the footer button counts the selection");
    // A game that starts running leaves the selection.
    running.game_id = SteamId(13);
    model.UpdateRuntime(RuntimeState::Ready, {titled, running, preparing});
    check(!find_row(model, "manage-toggle-13")->selected && find_row(model, "manage-toggle-13")->subtitle == "Playing: close it to uninstall",
          "a game that becomes blocked leaves the selection");
    running.game_id = SteamId(14);
    model.UpdateRuntime(RuntimeState::Ready, {titled, running, preparing});
    activate(model, "manage-select-all");
    activate(model, "manage-select-all");
    check(!find_row(model, "manage-toggle-10")->selected, "Clear selection unchecks every game");

    // A batch of three: the confirmation names them and the space, with the saves choice. With automatic
    // Cloud sync off it does not claim a sync.
    activate(model, "manage-toggle-10");
    activate(model, "manage-toggle-11");
    activate(model, "manage-toggle-12");
    activate(model, "manage-primary");
    check(model.state().navigation.modals.back().body.find("Steam Cloud saves are synced first") == std::string::npos &&
              model.state().navigation.modals.back().body.find("Automatic Cloud sync is off, so their saves are not synced first.") !=
                  std::string::npos,
          "with automatic sync off the batch dialog says the saves are not synced first");
    activate(model, "cancel");
    model.action("circle");
    activate(model, "settings-cloud-saves");
    check(model.state().cloud_saves_auto, "automatic Cloud sync is turned on");
    activate(model, "settings-installed");
    activate(model, "manage-toggle-10");
    activate(model, "manage-toggle-11");
    activate(model, "manage-toggle-12");
    activate(model, "manage-primary");
    const auto& confirm = model.state().navigation.modals.back();
    check(confirm.destructive && confirm.title == "Uninstall 3 games?", "the confirmation counts the games");
    check(confirm.body.find("Beta, Alpha and Gamma. Frees about 30.0 GB.") == 0, "it names them and the space freed");
    check(confirm.body.find("Steam Cloud saves are synced first. If that fails for a game, that game stays installed and Queue "
                            "says why.") != std::string::npos, "Cloud is synced first, and a failure keeps that game");
    const auto* saves = find_row(model, "uninstall-delete-saves");
    check(saves && !saves->selected, "deleting saves is a choice, off by default");
    activate(model, "uninstall-delete-saves");
    check(model.state().navigation.modals.back().body.find("saves and settings on this console are deleted") != std::string::npos,
          "the dialog says the saves go when the choice is on");
    activate(model, "cancel");
    check(model.TakeInstallRequests().empty() && model.state().navigation.modals.empty() &&
              model.state().navigation.route == Route::ManageGames,
          "Cancel uninstalls nothing and returns to the list");
    activate(model, "manage-primary");
    activate(model, "confirm-destructive");
    auto requests = model.TakeInstallRequests();
    check(requests.size() == 3 && requests[0].game_id == SteamId(11) && requests[1].game_id == SteamId(10) && requests[2].game_id == SteamId(12) &&
              std::all_of(requests.begin(), requests.end(), [](const auto& request) {
                  return request.kind == InstallRequestKind::Uninstall && !request.delete_saves;
              }),
          "each checked game is uninstalled, keeping its saves");
    // The list closes on the Queue, which follows the uninstalls, and a notice says what started.
    check(model.state().navigation.modals.empty() && model.state().navigation.route == Route::Downloads,
          "the confirmed list closes on the Queue");
    check(!model.state().notices.empty() && model.state().notices.back().id == "uninstall-batch" &&
              model.state().notices.back().message == "Uninstalling 3 games" &&
              model.state().notices.back().detail.empty() && !model.state().notices.back().failure,
          "a notice says what started");
    const auto notices = model.state().notices.size();
    model.UpdatePreparationQueue({std::nullopt, {}, {}, {}, {{SteamId(11), "Removing PS5 title…"}, {SteamId(10), ""}, {SteamId(12), ""}}});
    titled.uninstall_status = "Waiting to uninstall";
    model.UpdateRuntime(RuntimeState::Ready, {titled, running, preparing});
    for (int i = 0; i < 6 && model.state().navigation.route != Route::Settings; ++i) model.action("r1");
    activate(model, "settings-installed");
    check(!find_row(model, "manage-toggle-10")->enabled && find_row(model, "manage-toggle-10")->subtitle == "Waiting to uninstall",
          "a game waiting to uninstall says so and can't be chosen");
    model.action("circle");

    // One stops on Cloud: Queue keeps the reason, and a corner notice says so instead of a dialog.
    model.NotifyUninstallStopped(SteamId(12), "Cloud saves could not be synced.");
    check(model.state().navigation.modals.empty() && model.state().notices.size() == notices + 1 &&
              model.state().notices.back().message == "Uninstall stopped" && !model.state().notices.back().alert,
          "a stop in the batch is a corner notice, not a dialog");

    // The other two finish without a notice each.
    model.UpdatePreparationQueue({});
    titled.uninstall_status.clear();
    model.UpdateRuntime(RuntimeState::Ready, {running, preparing});
    model.UpdateInstalls({LiveInstallJob{SteamId(15), JobState::Queued, 0, gb, {}, "internal"}},
                         {{SteamId(12), 5 * gb, "internal"}, {SteamId(13), 1 * gb, "internal"}, {SteamId(14), 2 * gb, "internal"},
                          {SteamId(15), 3 * gb, "internal"}, {SteamId(16), 4 * gb, "internal"}});
    model.NotifyUninstallFinished(SteamId(11));
    model.NotifyUninstallFinished(SteamId(10));
    check(model.state().notices.size() == notices + 1, "the batch posts no Game uninstalled notices");

    // The list shows the stopped game with its reason; it can go again.
    activate(model, "settings-installed");
    check(installed_order(model) == std::vector<std::string>{"12", "16", "15", "14", "13"},
          "the list shows what is installed now, without the uninstalled games");
    check(find_row(model, "manage-toggle-12")->enabled &&
              find_row(model, "manage-toggle-12")->subtitle == "Stopped: Cloud saves could not be synced.",
          "the stopped game says why and can be chosen");
    activate(model, "manage-toggle-12");
    check(model.view().footer == "1 selected · frees 5.0 GB", "the footer sums the selection");
    activate(model, "manage-primary");
    check(model.state().navigation.modals.back().title == "Uninstall 1 game?", "the retry asks for the one game");
    activate(model, "confirm-destructive");
    requests = model.TakeInstallRequests();
    check(requests.size() == 1 && requests[0].game_id == SteamId(12), "the stopped game is uninstalled again");
    check(model.state().navigation.modals.empty() && model.state().notices.back().message == "Uninstalling Gamma" &&
              model.state().navigation.route == Route::Downloads, "one game's notice names it");
    check(model.state().ended_uninstalls.size() == 2 &&
              std::all_of(model.state().ended_uninstalls.begin(), model.state().ended_uninstalls.end(),
                          [](const auto& ended) { return ended.stopped.empty(); }),
          "going again clears the stop; the finished ones stay");
    model.UpdateInstalls({LiveInstallJob{SteamId(15), JobState::Queued, 0, gb, {}, "internal"}},
                         {{SteamId(13), 1 * gb, "internal"}, {SteamId(14), 2 * gb, "internal"}, {SteamId(15), 3 * gb, "internal"},
                          {SteamId(16), 4 * gb, "internal"}});
    for (int i = 0; i < 6 && model.state().navigation.route != Route::Settings; ++i) model.action("r1");
    activate(model, "settings-installed");
    check(!has_row(model, "manage-toggle-12"), "the uninstalled game leaves the list");

    // The footer is reached from the last game and left again; the toolbar is above the first game.
    check(model.action("focus:manage-toggle-13"), "the last game is focused");
    model.action("down");
    check(model.state().navigation.focus_key == "manage-primary", "Down from the last game reaches the footer");
    model.action("right");
    model.action("down");
    check(model.state().navigation.focus_key == "manage-primary", "the footer has one button and Down stays");
    model.action("up");
    check(model.state().navigation.focus_key == "manage-toggle-13", "Up returns to the game the focus left");
    check(model.action("focus:manage-toggle-16"), "a game mid-list is focused");
    model.action("up");
    model.action("up");
    check(model.state().navigation.focus_key == "manage-select-all", "Up from the first game reaches the toolbar");
    model.action("right");
    check(model.state().navigation.focus_key == "manage-sort", "Right moves along the toolbar");
    model.action("right");
    check(model.state().navigation.focus_key == "manage-sort", "the Uninstall toolbar ends at Sort");
    model.action("down");
    check(model.state().navigation.focus_key == "manage-toggle-16", "Down from the toolbar reaches the first game");

    // Triangle is Uninstall selected from anywhere in the list, and nothing without a selection.
    check(model.action("focus:manage-toggle-16"), "a game is focused");
    model.action("triangle");
    check(model.state().navigation.modals.empty() && model.TakeInstallRequests().empty(),
          "Triangle does nothing with nothing checked");
    activate(model, "manage-toggle-13");
    check(model.action("focus:manage-toggle-16"), "a game is focused");
    model.action("triangle");
    check(model.state().navigation.modals.back().title == "Uninstall 1 game?", "Triangle opens the confirmation");
    activate(model, "cancel");
    check(model.state().navigation.focus_key == "manage-toggle-16", "cancelling returns to the row the player was on");
    activate(model, "manage-toggle-13");

    // Every checked game became blocked while the dialog was open: nothing goes, and the list says so.
    model.action("circle");
    activate(model, "settings-installed");
    activate(model, "manage-toggle-13");
    activate(model, "manage-primary");
    running.game_id = SteamId(13);
    model.UpdateRuntime(RuntimeState::Ready, {running, preparing});
    activate(model, "confirm-destructive");
    check(model.TakeInstallRequests().empty() && model.view().subtitle == "None of the selected games can be uninstalled now.",
          "without a batch the list says nothing could go");

    // Circle closes the screen. A game uninstalled on its own still says why it stopped in a dialog.
    model.action("circle");
    check(model.state().navigation.route == Route::Settings && model.state().navigation.focus_key == "settings-installed",
          "Circle closes back to the Settings row");
    check(find_row(model, "settings-installed")->value == "4 games · 10.0 GB", "the Settings row follows");
    model.NotifyUninstallStopped(SteamId(13), "Cloud saves could not be synced.");
    check(!model.state().navigation.modals.empty() && model.state().navigation.modals.back().title == "Uninstall stopped",
          "a stop outside a batch is told in a dialog");
    model.action("circle");

    // Without a batch the list follows the installs, and closes when none is left.
    model.UpdateRuntime(RuntimeState::Ready, {});
    model.UpdateVolumes({{"internal", "Console storage", true, 800 * gb, 500 * gb}, {"usb0", "USB drive", true, {}, {}}},
                        "internal");
    model.UpdateInstalls({}, {{SteamId(12), 5 * gb, "internal"}, {SteamId(13), 1 * gb, "usb0"}});
    activate(model, "settings-installed");
    check(installed_order(model) == std::vector<std::string>{"12", "13"}, "the list shows what is installed now");
    check(find_row(model, "manage-toggle-12")->subtitle == "Console storage" &&
              find_row(model, "manage-toggle-13")->value == "1.0 GB",
          "each row names its drive");
    check(find_row(model, "manage-toggle-13")->subtitle == "Stopped: Cloud saves could not be synced.",
          "a stopped uninstall keeps its reason in the list until the game goes again or Queue dismisses it");
    check(model.action("focus:manage-toggle-13"), "the last game is focused");
    model.UpdateInstalls({}, {{SteamId(12), 5 * gb, "internal"}});
    check(installed_order(model) == std::vector<std::string>{"12"}, "the list follows an uninstall that finished");
    check(model.state().navigation.focus_key == "manage-toggle-12", "focus moves to the nearest game, not the footer");
    model.UpdateInstalls({}, {});
    check(model.state().navigation.route == Route::Settings, "the list closes when nothing is installed");
    entry = find_row(model, "settings-installed");
    check(entry && entry->value == "None" && !entry->enabled, "nothing installed says None");
}

void live_settings_focus_stays_near_a_removed_row() {
    auto model = make_live();
    model.UpdateAccount({AuthState::SignedIn, "123", "Synthetic", {}, {}, true});
    check(model.UpdateLibrary({{game(SteamId(42), "Synthetic")}, false, {}, false, false, {}}), "library loaded");
    model.SetRuntimeLeftovers(2);
    model.UpdateVolumes({{"internal", "Console storage", true, {}, {}}}, "internal");
    model.action("l1");
    check(find_row(model, "settings-runtime-cleanup")->value == "2 · Console storage",
          "the row says where the unused settings data is");
    activate(model, "settings-runtime-cleanup");
    check(!model.TakeRuntimeCleanupRequest() && !model.state().navigation.modals.empty() &&
              model.state().navigation.modals.back().destructive, "cleanup asks before deleting");
    check(model.state().navigation.modals.back().body.find("on Console storage") != std::string::npos,
          "the confirmation says where the data is");
    activate(model, "cancel");
    check(!model.TakeRuntimeCleanupRequest() && has_row(model, "settings-runtime-cleanup"), "Cancel keeps the data");
    activate(model, "settings-runtime-cleanup");
    activate(model, "runtime-cleanup-confirm");
    check(model.TakeRuntimeCleanupRequest(), "cleanup goes to the runtime");
    model.SetRuntimeLeftovers(0);
    const auto& rows = model.view().rows;
    const auto focused = rows[static_cast<std::size_t>(model.view().focused_row)];
    check(!has_row(model, "settings-runtime-cleanup"), "the cleaned row is gone");
    check(focused.section == "Storage" && !focused.rail_only && !model.state().navigation.settings_rail_focused,
          "focus moves to the nearest row in the same section, not the rail");
    check(focused.enabled, "a row that can be chosen is preferred");
}

void preparation_steps_read_as_player_words() {
    using playden::homebrew::PreparationStepWords;
    for (const char* step : {"stage-title", "copy-template", "validate-artifacts", "publish-profile", "query-native",
                             "register", "await-native", "capture-settings", "something-new"}) {
        const auto words = PreparationStepWords(step);
        check(!words.empty() && words.find('-') == std::string::npos, "every step reads as words, never its identifier");
    }
    check(PreparationStepWords("copy-template") == "Copying the Windows environment", "the template copy is named");
    check(PreparationStepWords("await-native") == "Waiting for the PS5", "registration waits for the PS5");
    check(PreparationStepWords("").empty(), "no step, no words");
    using playden::homebrew::PreparationProgressWords;
    check(PreparationProgressWords(false, "") == "Step 1 of 6 · Reserving a PS5 title", "before its record, the title is reserved");
    check(PreparationProgressWords(true, "copy-template") == "Step 3 of 6 · Copying the Windows environment",
          "the template copy is step 3");
    check(PreparationProgressWords(true, "await-native") == "Step 6 of 6 · Waiting for the PS5", "registration is the last step");
    check(PreparationProgressWords(true, "something-new") == "Preparing", "an unknown step has no number");
}

void live_details_focus_moves_to_play_once_enabled() {
    using playden::homebrew::LiveRuntimeGame;
    using playden::homebrew::RuntimeState;
    auto model = make_live();
    model.UpdateAccount({AuthState::SignedIn, "123", "Synthetic", {}, {}, true});
    check(model.UpdateLibrary({{game(SteamId(42), "Synthetic")}, false, {}, false, false, {}}), "library loaded");
    model.UpdateAppDetails(SteamId(42), 300, 600, true);
    model.UpdateInstalls({}, {{SteamId(42), 600'000'000}});
    check(model.action("details:42"), "details open");
    // The runtime is still checking: no Play yet, focus waits on the first enabled action.
    check(model.state().navigation.focus_key != "details-play", "no Play while the runtime checks");
    LiveRuntimeGame checking; checking.game_id = SteamId(42); checking.status = "Checking";
    model.UpdateRuntime(RuntimeState::Checking, {checking});
    check(model.state().navigation.focus_key == "details-cloud", "a disabled Play leaves focus on Cloud saves for now");
    LiveRuntimeGame ready; ready.game_id = SteamId(42); ready.status = "Ready"; ready.ready = true; ready.can_play = true; ready.has_title = true;
    model.UpdateRuntime(RuntimeState::Ready, {ready});
    check(model.state().navigation.focus_key == "details-play", "focus moves to Play once it is enabled");

    // Once the player moves, Play becoming enabled leaves focus where they put it.
    model.UpdateRuntime(RuntimeState::Checking, {checking});
    check(model.action("details:42"), "details reopen");
    check(model.action("right"), "the player moves");
    const auto chosen = model.state().navigation.focus_key;
    model.UpdateRuntime(RuntimeState::Ready, {ready});
    check(model.state().navigation.focus_key == chosen, "focus stays where the player moved it");

    // Left and Right step between the drawn buttons only: the install line and the PS5 title note are not stops.
    check(model.action("focus:details-play"), "focus Play");
    check(model.action("left") && model.state().navigation.focus_key == "details-play", "Left from Play stays on Play");
    std::vector<std::string> visited{model.state().navigation.focus_key};
    for (int step = 0; step < 6; ++step) {
        check(model.action("right"), "move right");
        if (model.state().navigation.focus_key != visited.back()) visited.push_back(model.state().navigation.focus_key);
    }
    for (const auto& key : visited) {
        const auto* item = find_row(model, key);
        check(item && (item->kind == playden::homebrew::RowKind::Action || item->kind == playden::homebrew::RowKind::Toggle),
              "every stop is a button");
    }
    check(std::find(visited.begin(), visited.end(), "details-runtime") == visited.end() &&
              std::find(visited.begin(), visited.end(), "details-installed") == visited.end(),
          "notes are skipped");
}

// Stay signed in: on by default; a saved Off comes back with the preferences.
void live_stay_signed_in_preference_loads() {
    auto model = make_live();
    check(model.state().stay_signed_in && model.ExportPreferences().steam_stay_signed_in, "stay signed in by default");
    playden::PreferencesLoadResult stay_off;
    stay_off.preferences.steam_stay_signed_in = false;
    check(model.ApplyPreferences(model.BeginPreferencesLoad(), stay_off) && !model.state().stay_signed_in &&
              !model.ExportPreferences().steam_stay_signed_in,
          "a saved stay signed in off loads");
}

// Settings › Sync Cloud saves automatically, on (it starts off).
void sync_automatically(Simulation& model) {
    playden::PreferencesLoadResult loaded;
    loaded.preferences.cloud_saves_auto = true;
    check(model.ApplyPreferences(model.BeginPreferencesLoad(), loaded) && model.state().cloud_saves_auto,
          "automatic Cloud saves on");
}

void live_ready_to_play_quits_to_home() {
    using playden::homebrew::LiveRuntimeGame;
    using playden::homebrew::RuntimeState;
    auto model = make_live();
    sync_automatically(model);
    model.UpdateAccount({AuthState::SignedIn, "123", "Synthetic", {}, {}, true});
    check(model.UpdateLibrary({{game(SteamId(42), "Synthetic"), game(SteamId(43), "Other")}, false, {}, false, false, {}}), "library loaded");
    model.UpdateAppDetails(SteamId(42), 300, 600, true);
    model.UpdateInstalls({}, {{SteamId(42), 600'000'000}, {SteamId(43), 100}});
    LiveRuntimeGame waiting; waiting.game_id = SteamId(42); waiting.status = "Waiting for the PS5"; waiting.awaiting_registration = true;
    waiting.can_prepare = true; waiting.has_title = true;
    waiting.detail = "Waiting for the PS5, which adds the game to Home when Playden quits";
    model.UpdateRuntime(RuntimeState::Ready, {waiting});

    // Details: Finish setup quits to Home without asking; SETUP explains the pending registration.
    check(model.action("details:42"), "details open");
    const auto* play = find_row(model, "details-play");
    check(play && play->enabled && play->action == "quit-to-home" && play->title == "Finish setup" && play->value == "Quits to Home", "Finish setup explains quitting to Home");
    check(find_row(model, "details-setup")->value == "Waiting for the PS5", "setup says the title waits for the PS5");
    check(model.state().navigation.focus_key == "details-play", "focus rests on Play");
    // Its saves sync first (#50): Play waits in a dialog, then quits once they are in step.
    model.UpdateCloud({{SteamId(42), CloudStatus::UpToDate, "Saves are up to date"}});
    activate(model, "details-play");
    auto requests = model.TakeCloudRequests();
    check(requests.size() == 1 && requests[0].kind == CloudRequestKind::Sync && requests[0].game_id == SteamId(42), "Play syncs saves first");
    check(!model.state().quit_requested && model.state().navigation.modals.back().title == "Syncing Cloud saves…",
          "Play waits for the sync in a dialog");
    model.UpdateCloud({{SteamId(42), CloudStatus::UpToDate, "Saves are up to date"}});
    check(!model.state().quit_requested, "a result that is not the sync's answer does not count");
    model.NotifyCloudAnswered(SteamId(43), CloudStatus::Synced);
    check(!model.state().quit_requested, "another game's answer does not count");
    model.UpdateCloud({{SteamId(42), CloudStatus::Synced, "1 save downloaded"}});
    model.NotifyCloudAnswered(SteamId(42), CloudStatus::Synced);
    check(model.state().quit_requested && model.state().navigation.modals.empty(), "Play quits to Home once saves are in step");
    // A choice or a failure lets the player play without syncing.
    auto choosing = make_live();
    sync_automatically(choosing);
    choosing.UpdateAccount({AuthState::SignedIn, "123", "Synthetic", {}, {}, true});
    check(choosing.UpdateLibrary({{game(SteamId(42), "Synthetic")}, false, {}, false, false, {}}), "library loaded");
    choosing.UpdateInstalls({}, {{SteamId(42), 600'000'000}});
    choosing.UpdateRuntime(RuntimeState::Ready, {waiting});
    check(choosing.action("details:42"), "details open");
    activate(choosing, "details-play");
    choosing.UpdateCloud({{SteamId(42), CloudStatus::Conflict, "Both changed."}});
    choosing.NotifyCloudAnswered(SteamId(42), CloudStatus::Conflict);
    check(!choosing.state().quit_requested && choosing.state().navigation.modals.back().title == "Cloud saves need a choice" &&
              has_row(choosing, "saves-quit-anyway"),
          "a conflict asks before quitting");
    activate(choosing, "saves-quit-anyway");
    check(choosing.state().quit_requested, "Play without syncing quits");
    // The console refusing the sync (the title may run) does not leave Play waiting.
    auto refused = make_live();
    sync_automatically(refused);
    refused.UpdateAccount({AuthState::SignedIn, "123", "Synthetic", {}, {}, true});
    check(refused.UpdateLibrary({{game(SteamId(42), "Synthetic")}, false, {}, false, false, {}}), "library loaded");
    refused.UpdateInstalls({}, {{SteamId(42), 600'000'000}});
    refused.UpdateRuntime(RuntimeState::Ready, {waiting});
    check(refused.action("details:42"), "details open");
    activate(refused, "details-play");
    refused.NotifyCloudRefused(SteamId(42));
    check(!refused.state().quit_requested && refused.state().navigation.modals.back().title == "The game may still be running" &&
              has_row(refused, "saves-quit-anyway"),
          "a refused sync says why and offers Play without syncing");
    // With automatic sync off, Play quits without moving saves.
    auto manual = make_live();
    manual.UpdateAccount({AuthState::SignedIn, "123", "Synthetic", {}, {}, true});
    check(manual.UpdateLibrary({{game(SteamId(42), "Synthetic")}, false, {}, false, false, {}}), "library loaded");
    manual.UpdateInstalls({}, {{SteamId(42), 600'000'000}});
    manual.UpdateRuntime(RuntimeState::Ready, {waiting});
    manual.UpdateCloud({{SteamId(42), CloudStatus::UpToDate, "Saves are up to date"}});
    check(manual.action("details:42"), "details open");
    activate(manual, "details-play");
    check(manual.TakeCloudRequests().empty() && manual.state().quit_requested, "automatic sync off: Play quits at once");
    // A game without Steam Cloud quits at once.
    auto local = make_live();
    local.UpdateAccount({AuthState::SignedIn, "123", "Synthetic", {}, {}, true});
    check(local.UpdateLibrary({{game(SteamId(42), "Synthetic")}, false, {}, false, false, {}}), "library loaded");
    local.UpdateInstalls({}, {{SteamId(42), 600'000'000}});
    local.UpdateRuntime(RuntimeState::Ready, {waiting});
    local.UpdateCloud({{SteamId(42), CloudStatus::Unsupported, "This game does not use Steam Cloud."}});
    check(local.action("details:42"), "details open");
    activate(local, "details-play");
    check(local.state().quit_requested && local.state().navigation.modals.empty(), "Play quits to Home without a dialog");

    // The prompt: shown when nothing else is open, once per game, Play quits, Later closes.
    auto prompted = make_live();
    sync_automatically(prompted);
    prompted.UpdateAccount({AuthState::SignedIn, "123", "Synthetic", {}, {}, true});
    check(prompted.UpdateLibrary({{game(SteamId(42), "Synthetic"), game(SteamId(43), "Other")}, false, {}, false, false, {}}), "library loaded");
    prompted.UpdateInstalls({}, {{SteamId(42), 600'000'000}, {SteamId(43), 100}});
    LiveRuntimeGame other = waiting; other.game_id = SteamId(43);
    prompted.UpdateRuntime(RuntimeState::Ready, {waiting, other});
    prompted.action("l1");
    activate(prompted, "settings-about");  // a sheet is open: the prompt waits
    const auto open_before = prompted.state().navigation.modals.size();
    prompted.NotifyReadyToPlay(SteamId(42));
    prompted.NotifyReadyToPlay(SteamId(42));
    check(prompted.state().navigation.modals.size() == open_before, "the prompt waits for the open sheet");
    check(prompted.action("circle"), "the sheet closes");
    check(prompted.state().navigation.modals.empty(), "the prompt waits for its game's page");
    check(prompted.action("details:42"), "the announced game's page opens");
    check(!prompted.state().navigation.modals.empty(), "then the prompt shows");
    const auto& prompt = prompted.state().navigation.modals.back();
    check(prompt.title == "Synthetic is ready to play" && prompt.action_keys[static_cast<std::size_t>(prompt.focused_action)] == "quit-to-home",
          "the prompt names the game and focuses Play");
    check(find_row(prompted, "quit-to-home")->title == "Play" && find_row(prompted, "ready-later")->title == "Later", "Play or Later");
    activate(prompted, "ready-later");
    check(prompted.state().navigation.modals.empty() && !prompted.state().quit_requested, "Later closes it, once per game");
    // A game that stopped waiting (the check found it registered) is not announced.
    prompted.NotifyReadyToPlay(SteamId(43));
    check(prompted.state().navigation.modals.empty(), "a background game does not interrupt another game's page");
    check(prompted.action("details:43"), "the other game's page opens");
    check(!prompted.state().navigation.modals.empty(), "another game is announced");
    activate(prompted, "quit-to-home");
    requests = prompted.TakeCloudRequests();
    check(requests.size() == 1 && requests[0].game_id == SteamId(43), "Play in the prompt syncs the announced game");
    prompted.NotifyCloudAnswered(SteamId(43), CloudStatus::Synced);
    check(prompted.state().quit_requested, "Play in the prompt quits to Home");
    // Quitting would cut off another game's preparation: the quit waits for it in a dialog.
    auto busy = make_live();
    busy.UpdateAccount({AuthState::SignedIn, "123", "Synthetic", {}, {}, true});
    check(busy.UpdateLibrary({{game(SteamId(42), "Synthetic"), game(SteamId(43), "Other")}, false, {}, false, false, {}}), "library loaded");
    busy.UpdateInstalls({}, {{SteamId(42), 600'000'000}, {SteamId(43), 100}});
    LiveRuntimeGame preparing; preparing.game_id = SteamId(43); preparing.status = "Preparing";
    busy.UpdateRuntime(RuntimeState::Ready, {waiting, preparing});
    busy.UpdateCloud({{SteamId(42), CloudStatus::Unsupported, "This game does not use Steam Cloud."}});
    check(busy.action("details:42"), "details open");
    activate(busy, "details-play");
    check(!busy.state().quit_requested && busy.state().quit_waiting &&
              busy.state().navigation.modals.back().title == "Waiting for Other",
          "Play waits for the other game's preparation");
    LiveRuntimeGame prepared = preparing; prepared.status = "Ready"; prepared.ready = true;
    busy.UpdateRuntime(RuntimeState::Ready, {waiting, preparing});
    check(!busy.state().quit_requested, "still waiting while it prepares");
    busy.UpdateRuntime(RuntimeState::Ready, {waiting, prepared});
    check(busy.state().quit_requested && !busy.state().quit_waiting && busy.state().navigation.modals.empty(),
          "Playden quits once the preparation is done");
    // Games queued while none is being prepared (between two): the quit waits until the queue is held.
    auto queued = make_live();
    queued.UpdateAccount({AuthState::SignedIn, "123", "Synthetic", {}, {}, true});
    check(queued.UpdateLibrary({{game(SteamId(42), "Synthetic"), game(SteamId(43), "Other")}, false, {}, false, false, {}}), "library loaded");
    queued.UpdateInstalls({}, {{SteamId(42), 600'000'000}, {SteamId(43), 100}});
    LiveRuntimeGame next; next.game_id = SteamId(43); next.status = "Not prepared"; next.queued = true;
    queued.UpdateRuntime(RuntimeState::Ready, {waiting, next});
    queued.UpdatePreparationQueue({std::nullopt, {SteamId(43)}, {}, {}});
    queued.UpdateCloud({{SteamId(42), CloudStatus::Unsupported, "This game does not use Steam Cloud."}});
    check(queued.action("details:42"), "details open");
    activate(queued, "details-play");
    check(!queued.state().quit_requested && queued.state().quit_waiting &&
              queued.state().navigation.modals.back().title == "Waiting for the queue",
          "a queued game holds the quit");
    queued.UpdateRuntime(RuntimeState::Ready, {waiting, next});
    check(queued.state().quit_requested, "with the queue held and nothing preparing, Playden quits");
    auto cancelled = make_live();
    cancelled.UpdateAccount({AuthState::SignedIn, "123", "Synthetic", {}, {}, true});
    check(cancelled.UpdateLibrary({{game(SteamId(42), "Synthetic"), game(SteamId(43), "Other")}, false, {}, false, false, {}}), "library loaded");
    cancelled.UpdateInstalls({}, {{SteamId(42), 600'000'000}, {SteamId(43), 100}});
    cancelled.UpdateRuntime(RuntimeState::Ready, {waiting, preparing});
    cancelled.UpdateCloud({{SteamId(42), CloudStatus::Unsupported, "This game does not use Steam Cloud."}});
    check(cancelled.action("details:42"), "details open");
    activate(cancelled, "details-play");
    activate(cancelled, "cancel");
    cancelled.UpdateRuntime(RuntimeState::Ready, {waiting, prepared});
    check(!cancelled.state().quit_requested && !cancelled.state().quit_waiting, "Cancel keeps Playden open");
    // A game held behind another game's preparation says what it waits for.
    auto held = make_live();
    held.UpdateAccount({AuthState::SignedIn, "123", "Synthetic", {}, {}, true});
    check(held.UpdateLibrary({{game(SteamId(42), "Synthetic"), game(SteamId(43), "Other")}, false, {}, false, false, {}}), "library loaded");
    held.UpdateInstalls({}, {{SteamId(42), 600'000'000}, {SteamId(43), 100}});
    LiveRuntimeGame recovering; recovering.game_id = SteamId(42); recovering.status = "Needs recovery"; recovering.has_title = true;
    held.UpdateRuntime(RuntimeState::Ready, {recovering, preparing});
    check(held.action("details:42"), "details open");
    const auto* held_play = find_row(held, "details-play");
    check(held_play && !held_play->enabled && held_play->value == "Waiting" &&
              held_play->subtitle.empty(),
          "a held setup action waits without adding a second status line");
    auto dropped = make_live();
    dropped.UpdateAccount({AuthState::SignedIn, "123", "Synthetic", {}, {}, true});
    check(dropped.UpdateLibrary({{game(SteamId(42), "Synthetic")}, false, {}, false, false, {}}), "library loaded");
    dropped.UpdateInstalls({}, {{SteamId(42), 600'000'000}});
    LiveRuntimeGame ready; ready.game_id = SteamId(42); ready.status = "Ready"; ready.ready = true; ready.can_play = true;
    dropped.UpdateRuntime(RuntimeState::Ready, {ready});
    dropped.NotifyReadyToPlay(SteamId(42));
    check(dropped.state().navigation.modals.empty(), "a Ready game needs no prompt");
}

void live_ready_prompt_matches_the_open_game() {
    using playden::homebrew::LiveRuntimeGame;
    using playden::homebrew::RuntimeState;
    auto model = make_live();
    sync_automatically(model);
    model.UpdateAccount({AuthState::SignedIn, "123", "Synthetic", {}, {}, true});
    check(model.UpdateLibrary({{game(SteamId(1684930), "CULTIC"), game(SteamId(548430), "Deep Rock Galactic")}, false, {}, false, false, {}}),
          "library loaded");
    model.UpdateInstalls({}, {{SteamId(1684930), 100}, {SteamId(548430), 100}});
    LiveRuntimeGame cultic; cultic.game_id = SteamId(1684930); cultic.awaiting_registration = true; cultic.has_title = true;
    auto deep_rock = cultic; deep_rock.game_id = SteamId(548430);
    model.UpdateRuntime(RuntimeState::Ready, {cultic, deep_rock});
    check(model.action("details:1684930"), "CULTIC page opens");
    model.NotifyReadyToPlay(SteamId(548430));
    check(model.state().navigation.modals.empty(), "Deep Rock Galactic readiness stays queued on CULTIC's page");
    model.NotifyReadyToPlay(SteamId(1684930));
    check(model.state().navigation.modals.size() == 1 &&
              model.state().navigation.modals.back().title == "CULTIC is ready to play",
          "the current game's readiness is not blocked by an earlier background game");
    activate(model, "ready-later");
    check(model.state().navigation.modals.empty(), "Later does not expose a background game's prompt");
    check(model.action("details:548430"), "Deep Rock Galactic page opens");
    check(model.state().navigation.modals.size() == 1 &&
              model.state().navigation.modals.back().title == "Deep Rock Galactic is ready to play",
          "the deferred prompt names the game on its own page");
    activate(model, "quit-to-home");
    const auto requests = model.TakeCloudRequests();
    check(requests.size() == 1 && requests[0].game_id == SteamId(548430), "Play syncs the game named in the prompt");
}

// A setup waiting behind another game's says so: the button goes quiet and the page names the game ahead.
void live_queued_setup_names_the_game_ahead() {
    using playden::homebrew::RuntimeState;
    using playden::homebrew::LiveRuntimeGame;
    auto model = make_live();
    model.UpdateAccount({AuthState::SignedIn, "123", "Synthetic", {}, {}, true});
    check(model.UpdateLibrary({{game(SteamId(42), "Synthetic"), game(SteamId(43), "Ahead")}, false, {}, false, false, {}}), "library loaded");
    model.UpdateInstalls({}, {{SteamId(42), 600'000'000}, {SteamId(43), 700'000'000}});
    LiveRuntimeGame queued; queued.game_id = SteamId(42); queued.status = "Needs recovery"; queued.has_title = true;
    queued.queued = true;
    LiveRuntimeGame ahead; ahead.game_id = SteamId(43); ahead.status = "Preparing"; ahead.detail = "Step 3 of 6 · Copying";
    model.UpdateRuntime(RuntimeState::Ready, {queued, ahead});
    check(model.action("details:42"), "details open");
    const auto* play = find_row(model, "details-play");
    check(play && play->title == "Finish setup" && play->value == "Queued" && !play->enabled && play->action.empty(),
          "a queued setup can't be asked for again and says it is queued");
    const auto* setup = find_row(model, "details-setup");
    check(setup && setup->value == "Waiting for Ahead" && setup->tone == "busy", "setup names the game ahead");
    const auto* hint = find_row(model, "details-hint");
    check(hint && hint->title == "Setup starts when Ahead is ready." && hint->tone == "busy", "hint says when it starts");
}

void live_play_does_the_next_useful_thing() {
    using playden::homebrew::RuntimeState;
    using playden::homebrew::LiveRuntimeGame;
    auto model = make_live();
    model.UpdateAccount({AuthState::SignedIn, "123", "Synthetic", {}, {}, true});
    check(model.UpdateLibrary({{game(SteamId(42), "Synthetic")}, false, {}, false, false, {}}), "library loaded");
    model.UpdateInstalls({}, {{SteamId(42), 600'000'000}});
    struct Case {
        const char* status; const char* label; const char* value; const char* hint;
        const char* setup; const char* tone; const char* action;
    };
    const std::vector<Case> cases{
        {"Ready", "Play", "", "", "Ready", "good", "runtime-play"},
        {"Not prepared", "Play", "Sets up first", "", "Not set up", "quiet", "runtime-play"},
        {"Kit update available", "Play", "Kit update first", "", "Kit update available", "busy", "runtime-play"},
        {"Needs repair", "Play", "Repairs first", "Some of Synthetic's setup files changed.", "Needs repair", "attention", "runtime-play"},
        {"Needs recovery", "Finish setup", "", "Setup stopped early.", "Interrupted", "attention", "runtime-prepare"},
        {"Waiting for the PS5", "Finish setup", "Quits to Home", "Quit Playden and Synthetic appears on the PS5 Home screen.", "Waiting for the PS5", "busy", "quit-to-home"},
        {"Failed", "Finish setup", "", "Setup failed.", "Failed", "problem", "runtime-prepare"},
        {"Registration pending", "Check again", "", "The PS5 hasn't confirmed Synthetic yet.", "Not confirmed", "attention", "runtime-prepare"},
        {"Removal incomplete", "Finish removing", "", "Removing Synthetic from the PS5 Home screen stopped partway.", "Removing", "attention", "runtime-remove-retry"},
        {"No kit", "Open Settings", "", "Playden needs its runtime kit to set up games.", "No kit", "problem", ""},
    };
    for (const auto& expected : cases) {
        LiveRuntimeGame state; state.game_id = SteamId(42); state.status = expected.status;
        state.detail = "raw technical detail for bug reports";
        state.ready = state.status == "Ready"; state.can_play = state.ready;
        state.can_prepare = !state.ready && state.status != "No kit";
        state.has_title = state.status != "No kit" && state.status != "Not prepared";
        state.awaiting_registration = state.status == "Waiting for the PS5";
        model.UpdateRuntime(RuntimeState::Ready, {state});
        check(model.action("details:42"), "details open");
        const auto* play = find_row(model, "details-play");
        check(play && play->enabled && play->title == expected.label && play->value == expected.value,
              (state.status + ": main action names the next step").c_str());
        check(play->subtitle.empty(), "main action does not repeat the hint or technical detail");
        const auto* hint = find_row(model, "details-hint");
        if (std::string(expected.hint).empty()) check(!hint, "ready and routine setup have no attention hint");
        else check(hint && hint->title == expected.hint && hint->tone == expected.tone && !hint->enabled,
                   (state.status + ": one plain, coloured hint").c_str());
        const auto* setup = find_row(model, "details-setup");
        check(setup && setup->value == expected.setup && setup->tone == expected.tone && !setup->enabled,
              (state.status + ": setup fact states readiness").c_str());
        check(!has_row(model, "details-runtime") && !has_row(model, "details-installed") &&
                  !has_row(model, "details-update-runtime"), "details avoid duplicated facts and separate kit update action");
        if (state.status == "No kit") {
            activate(model, "details-play");
            check(model.state().navigation.route == Route::Settings, "missing kit opens Settings");
            check(model.TakeRuntimePlayRequests().empty() && model.TakeRuntimePrepareRequests().empty(),
                  "missing kit does not queue an impossible preparation");
            check(model.state().navigation.focus_key == "about-kit", "missing kit opens Runtime kit in About");
            while (!model.state().navigation.modals.empty()) check(model.action("circle"), "close kit information");
        } else {
            check(play->action == expected.action, "redesigned action preserves existing behavior");
            if (play->action == "runtime-play" || play->action == "runtime-prepare") {
                const bool starts = play->action == "runtime-play";
                activate(model, "details-play");
                check((starts ? model.TakeRuntimePlayRequests() : model.TakeRuntimePrepareRequests()) ==
                          std::vector<playden::GameId>{SteamId(42)}, "action requests the selected game");
            }
        }
    }
    // Raw failures must not leak into the page, even when they contain paths or jargon.
    for (const std::string status : {"Needs recovery", "Failed"}) {
        for (const auto& [detail, expected] : std::vector<std::pair<std::string, std::string>>{
                 {"insufficient verified free space for package copy", status == "Failed"
                     ? "Setup failed: console storage was full." : "Setup stopped early: console storage was full."},
                 {"kit source is missing, linked, or has wrong mode", "The runtime kit is damaged. Reinstall it."},
                 {"unknown failure /private/technical/path", status == "Failed" ? "Setup failed." : "Setup stopped early."},
                 {"", status == "Failed" ? "Setup failed." : "Setup stopped early."}}) {
            LiveRuntimeGame state; state.game_id = SteamId(42); state.status = status; state.detail = detail; state.can_prepare = true;
            model.UpdateRuntime(RuntimeState::Ready, {state});
            check(model.action("details:42"), "failed details open");
            const auto* hint = find_row(model, "details-hint");
            check(hint && hint->title == expected, (status + ": " + detail + " -> " + (hint ? hint->title : "missing hint")).c_str());
            check(model.state().runtime_games.at(SteamId(42)).detail == detail, "raw failure remains available for diagnostics");
        }
    }
    for (const std::string status : {"Needs recovery", "Failed"}) {
        for (const std::string code : {"space-unavailable", "kit-source", "kit-invalid-mode", "unknown-code"}) {
            LiveRuntimeGame state; state.game_id = SteamId(42); state.status = status; state.can_prepare = true;
            state.detail = "technical failure context"; state.failure_code = code;
            model.UpdateRuntime(RuntimeState::Ready, {state});
            const std::string expected = code == "space-unavailable"
                ? (status == "Failed" ? "Setup failed: console storage was full." : "Setup stopped early: console storage was full.")
                : code.starts_with("kit-") ? "The runtime kit is damaged. Reinstall it."
                : status == "Failed" ? "Setup failed." : "Setup stopped early.";
            check(find_row(model, "details-hint") && find_row(model, "details-hint")->title == expected,
                  "recorded failure code determines the plain hint");
        }
    }
    LiveRuntimeGame preparing; preparing.game_id = SteamId(42); preparing.status = "Preparing";
    preparing.detail = "Step 3 of 6 · Copying the Windows environment";
    model.UpdateRuntime(RuntimeState::Ready, {preparing});
    check(find_row(model, "details-play") && !find_row(model, "details-play")->enabled, "preparing cannot start again");
    check(find_row(model, "details-setup") && find_row(model, "details-setup")->value == preparing.detail,
          "setup fact uses the existing preparation progress words");
    for (const auto& [runtime_state, status] : std::vector<std::pair<RuntimeState, std::string>>{
             {RuntimeState::Checking, "Checking"}, {RuntimeState::Unavailable, "Unavailable"}}) {
        LiveRuntimeGame transient; transient.game_id = SteamId(42); transient.status = status;
        model.UpdateRuntime(runtime_state, {transient});
        check(find_row(model, "details-setup") && find_row(model, "details-setup")->value == status,
              "transient runtime availability has a setup fact");
        check(!find_row(model, "details-play")->enabled && !has_row(model, "details-hint"),
              "transient runtime availability does not offer an impossible action or attention hint");
    }
    model.UpdateInstalls({}, {});
    check(!has_row(model, "details-setup") && !has_row(model, "details-hint"), "uninstalled games have no setup facts or hints");
}

// A start from Home that failed is reported as Playden starts, before the library: the dialog still names the game.
void live_launch_failure_before_the_library_names_the_game() {
    using playden::homebrew::LiveRuntimeGame;
    using playden::homebrew::RuntimeState;
    auto model = make_live();
    model.UpdateAccount({AuthState::SignedIn, "123", "Synthetic", {}, {}, true});
    LiveRuntimeGame stopped; stopped.game_id = SteamId(42); stopped.status = "Ready"; stopped.ready = true; stopped.can_play = true;
    stopped.play_detail = "The game could not start.";
    model.UpdateRuntime(RuntimeState::Ready, {stopped});
    check(model.state().navigation.modals.empty() && model.state().notices.empty(),
          "a launch failure for a game not listed yet waits for the library");
    check(model.UpdateLibrary({{game(SteamId(42), "BeamNG.drive")}, false, {}, false, false, {}}), "library loaded");
    const auto& modals = model.state().navigation.modals;
    check(modals.size() == 1 && modals.back().title == "Can't play" &&
              modals.back().body.rfind("BeamNG.drive · The game could not start.", 0) == 0,
          "the waiting launch failure names the game once the library arrives");
    activate(model, "alert-ok");
    model.UpdateLibrary({{game(SteamId(42), "BeamNG.drive")}, false, {}, false, false, {}});
    check(model.state().navigation.modals.empty(), "the launch failure is told once");

    // A library that cannot load still tells a waiting failure, without the name.
    auto offline = make_live();
    offline.UpdateAccount({AuthState::SignedIn, "123", "Synthetic", {}, {}, true});
    offline.UpdateRuntime(RuntimeState::Ready, {stopped});
    offline.UpdateLibrary({{}, false, "Library sync is unavailable. Try again later.", false, false, {}});
    const auto& offline_modals = offline.state().navigation.modals;
    check(!offline_modals.empty() && offline_modals.back().body.rfind("A game · The game could not start.", 0) == 0,
          "a library that could not load still tells the waiting failure");
}

void live_launch_failure_remains_visible_without_a_details_status_line() {
    using playden::homebrew::LiveRuntimeGame;
    using playden::homebrew::RuntimeState;
    auto model = make_live();
    model.UpdateAccount({AuthState::SignedIn, "123", "Synthetic", {}, {}, true});
    check(model.UpdateLibrary({{game(SteamId(42), "Synthetic")}, false, {}, false, false, {}}), "library loaded");
    model.UpdateInstalls({}, {{SteamId(42), 600'000'000}});
    LiveRuntimeGame ready; ready.game_id = SteamId(42); ready.status = "Ready"; ready.ready = true; ready.can_play = true;
    model.UpdateRuntime(RuntimeState::Ready, {ready});
    check(model.action("details:42"), "details open");
    ready.play_detail = "Could not start: technical /private/launch/path";
    model.UpdateRuntime(RuntimeState::Ready, {ready});
    check(model.state().navigation.modals.size() == 1 && model.state().navigation.modals.back().title == "Can't play",
          "a failed launch remains visible after removing the details subtitle");
    check(model.state().navigation.modals.back().body.find("/private/launch/path") == std::string::npos,
          "launch failure notice keeps technical paths out of player copy");
    activate(model, "alert-ok");
    model.UpdateRuntime(RuntimeState::Ready, {ready});
    check(model.state().navigation.modals.empty(), "an unchanged launch refusal is not repeated on refresh");
    check(!has_row(model, "details-hint"), "a ready game's launch refusal does not invent a setup hint");
    activate(model, "details-play");
    check(model.TakeRuntimePlayRequests() == std::vector<playden::GameId>{SteamId(42)}, "retry requests another play");
    model.UpdateRuntime(RuntimeState::Ready, {ready});
    check(model.state().navigation.modals.size() == 1, "a fresh attempt can report the same refusal again");
    activate(model, "alert-ok");
    ready.play_detail = "Cloud saves could not be checked. Press Play again to start without syncing.";
    model.UpdateRuntime(RuntimeState::Ready, {ready});
    check(model.state().navigation.modals.size() == 1 &&
              model.state().navigation.modals.back().body.find(ready.play_detail) != std::string::npos,
          "Cloud retry guidance remains reachable after removing the details subtitle");
}

void live_store_details_fill_the_description() {
    auto model = make_live();
    model.UpdateAccount({AuthState::SignedIn, "123", "Synthetic", {}, {}, true});
    check(model.UpdateLibrary({{game(SteamId(42), "Synthetic")}, false, {}, false, false, {}}), "library loaded");
    model.UpdateStoreDetails(SteamId(42), "A synthetic game.", {"Action", "Indie"}, "full");
    const auto& games = model.state().games;
    check(games[0].description == "A synthetic game." && games[0].genres.size() == 2 && games[0].controller_support == "full",
          "the store fills the description, genres and controller support");
    check(model.UpdateLibrary({{game(SteamId(42), "Synthetic")}, false, {}, false, false, {}}), "library refreshed");
    check(model.state().games[0].description == "A synthetic game.", "a library refresh keeps the store description");
}

} // namespace

const playden::homebrew::ViewRow* saves_row(const Simulation& model, const std::string& key) {
    for (const auto& row : model.view().rows) if (row.key == key) return &row;
    return nullptr;
}

// Games never uploaded to this Steam account are confirmed together; choices stay per game.
void live_saves_upload_all() {
    using playden::homebrew::LiveCloudGame;
    using playden::homebrew::LiveSavesJob;
    using playden::homebrew::SavesJobKind;
    auto model = make_live();
    model.UpdateAccount({AuthState::SignedIn, "123", "Synthetic", {}, {}, true});
    check(model.UpdateLibrary({{game(SteamId(42), "Alpha"), game(SteamId(43), "Beta"), game(SteamId(44), "Gamma")}, false, {}, false, false, {}}),
          "upload all: library loaded");
    model.UpdateInstalls({}, {{SteamId(42), 600'000'000}, {SteamId(43), 100'000'000}, {SteamId(44), 100'000'000}});
    model.action("l1");
    model.action("l1");
    model.TakeCloudRequests();

    LiveCloudGame conflict{SteamId(42), CloudStatus::Conflict, "Both changed."};
    LiveCloudGame synced{SteamId(43), CloudStatus::Synced, "1 save uploaded"};
    model.UpdateCloud({conflict, synced});
    check(!saves_row(model, "saves-upload-all") && !has_legend(model, "Square Upload all"),
          "upload all: hidden while no game waits for the account");

    LiveCloudGame first{SteamId(43), CloudStatus::NeedsAccount, "Confirm this Steam account."};
    LiveCloudGame second{SteamId(44), CloudStatus::NeedsAccount, "Confirm this Steam account."};
    model.UpdateCloud({first, second});
    check(saves_row(model, "saves-upload-all") && saves_row(model, "saves-upload-all")->value == "2" &&
              has_legend(model, "Square Upload all"),
          "upload all: offered with the number of games");
    check(model.action("focus:saves-filter-4"), "upload all: focus the last filter");
    model.action("right");
    check(model.state().navigation.focus_key == "saves-upload-all", "upload all: sits between the chips and Sync all");
    model.action("right");
    check(model.state().navigation.focus_key == "saves-sync-all", "upload all: Sync all follows it");

    // One confirmation, then one request per waiting game; the conflict is left for the player.
    model.action("square");
    check(!model.state().navigation.modals.empty() &&
              model.state().navigation.modals.back().title == "Upload all saves to this Steam account?" &&
              model.state().navigation.modals.back().body.find("2 games' saves for Synthetic") != std::string::npos,
          "upload all: Square asks once for every game");
    check(model.TakeCloudRequests().empty(), "upload all: nothing is sent before the confirmation");
    activate(model, "cloud-confirm-account-all");
    auto requests = model.TakeCloudRequests();
    check(requests.size() == 2 && requests[0].kind == CloudRequestKind::ConfirmAccount && requests[0].game_id == SteamId(43) &&
              requests[1].kind == CloudRequestKind::ConfirmAccount && requests[1].game_id == SteamId(44),
          "upload all: confirms each waiting game");
    check(model.state().navigation.modals.empty(), "upload all: the dialog closes");

    // Not while Sync all runs.
    model.UpdateSavesJob({SavesJobKind::SyncAll, 0, 3, SteamId(42), false});
    check(!saves_row(model, "saves-upload-all"), "upload all: hidden while Sync all runs");
    model.action("square");
    check(model.state().navigation.modals.empty(), "upload all: Square does nothing while Sync all runs");
}

void live_saves_tab() {
    using playden::homebrew::LiveCloudGame;
    using playden::homebrew::LiveSavesJob;
    using playden::homebrew::SavesJobKind;
    auto model = make_live();
    model.SetWallClock(1'800'000'000);
    model.UpdateAccount({AuthState::SignedIn, "123", "Synthetic", {}, {}, true});
    check(model.UpdateLibrary({{game(SteamId(42), "Alpha"), game(SteamId(43), "Beta"), game(SteamId(44), "Gamma")}, false, {}, false, false, {}}),
          "library loaded");
    model.UpdateInstalls({}, {{SteamId(42), 600'000'000}, {SteamId(43), 100'000'000}});
    model.UpdateKeptBottles({{SteamId(44), "Gamma", 1'700'000'000, 1'900'000'000}});

    // The tab sits between Downloads and Settings; opening it asks for a quick check.
    model.action("l1");
    model.action("l1");
    check(model.state().navigation.route == Route::Saves, "Saves is the tab before Settings");
    auto requests = model.TakeCloudRequests();
    check(requests.size() == 1 && requests[0].kind == CloudRequestKind::QuickCheck, "opening Saves checks what may have changed");

    LiveCloudGame first{SteamId(42), CloudStatus::Conflict, "Both changed.", 2, 3, 900'000, 1'200'000, 1'799'000'000, 1'799'900'000};
    first.first_sync = true;
    LiveCloudGame synced{SteamId(43), CloudStatus::Synced, "1 save uploaded", 4, 4, 312'000, 312'000};
    synced.last_sync = 1'800'000'000 - 7200;
    LiveCloudGame kept{SteamId(44), CloudStatus::UpToDate, "Saves are up to date", 6, 6, 420'000, 420'000};
    kept.background = true;
    model.UpdateCloud({first, synced, kept});
    check(saves_row(model, "save-42") && saves_row(model, "save-42")->value == "Choose saves" &&
              saves_row(model, "save-42")->subtitle == "First sync · both sides already have saves",
          "a first sync with saves on both sides asks for a choice");
    check(saves_row(model, "save-43")->value == "In sync" && saves_row(model, "save-43")->badge == "Synced 2 h ago" &&
              saves_row(model, "save-43")->aside == "312 KB",
          "a synced game says when");
    check(saves_row(model, "save-44")->subtitle == "Not installed", "kept data of an uninstalled game is listed");
    check(saves_row(model, "saves-filter-1")->value == "1" && saves_row(model, "saves-filter-4")->value == "1", "filters count");

    // Chips and Sync all form one row above the list.
    check(model.action("focus:saves-filter-0"), "focus the first filter");
    model.action("down");
    check(model.state().navigation.focus_key == "save-42", "Down enters the list");
    model.action("up");
    check(model.state().navigation.focus_key == "saves-filter-0", "Up returns to the current filter");
    activate(model, "saves-filter-1");
    check(saves_row(model, "save-42") && !saves_row(model, "save-43") && !saves_row(model, "save-44"), "Needs you shows only choices");
    activate(model, "saves-filter-0");

    // A choice found again by a later check is not announced again.
    LiveCloudGame checking_again = first;
    checking_again.status = CloudStatus::Checking;
    model.UpdateCloud({checking_again});
    const auto notices_before = model.state().notices.size();
    model.UpdateCloud({first});
    check(model.state().notices.size() == notices_before, "a known choice is not announced again");

    // Sync all, and Stop while it runs; a finished Sync all says what happened.
    check(model.action("triangle"), "Triangle works on Saves");
    requests = model.TakeCloudRequests();
    check(requests.size() == 1 && requests[0].kind == CloudRequestKind::SyncAll, "Triangle starts Sync all");
    model.UpdateSavesJob({SavesJobKind::SyncAll, 1, 3, SteamId(43), false});
    check(saves_row(model, "saves-sync-all")->title == "Stop" && model.view().subtitle.find("Syncing 2 of 3") == 0,
          "a running Sync all shows its progress and Stop");
    activate(model, "saves-sync-all");
    requests = model.TakeCloudRequests();
    check(requests.size() == 1 && requests[0].kind == CloudRequestKind::StopJob, "Stop stops the job");
    model.UpdateSavesJob({});
    check(saves_row(model, "saves-sync-all")->title == "Sync all", "Sync all comes back");
    bool announced = false;
    for (const auto& notice : model.state().notices) announced |= notice.id == "saves-sync-all";
    check(announced, "a finished Sync all is announced");

    // A background failure never opens a dialog; one the player asked for does.
    LiveCloudGame failed{SteamId(43), CloudStatus::Failed, "Steam is not reachable. Try again."};
    failed.background = true;
    model.UpdateCloud({failed});
    check(model.state().navigation.modals.empty() && saves_row(model, "save-43")->value == "Couldn't sync",
          "a background failure shows only in the list");

    // A game's page: the choice goes to the service; Circle returns to its row.
    activate(model, "save-42");
    check(model.state().navigation.route == Route::SaveDetails && has_row(model, "saves-use-cloud") && has_row(model, "saves-use-local"),
          "a first sync offers both sides");
    activate(model, "saves-use-cloud");
    requests = model.TakeCloudRequests();
    check(requests.size() == 1 && requests[0].kind == CloudRequestKind::UseCloud && requests[0].game_id == SteamId(42), "the choice is sent");
    model.action("circle");
    check(model.state().navigation.route == Route::Saves && model.state().navigation.focus_key == "save-42", "Circle returns to the row");

    // Deleting an installed game's saves asks first.
    model.UpdateCloud({synced});
    activate(model, "save-43");
    activate(model, "saves-delete");
    check(model.state().navigation.modals.back().title == "Delete saves on this console?" &&
              model.state().navigation.modals.back().body.find("Steam Cloud keeps its copy") != std::string::npos,
          "deleting saves asks and says Cloud keeps a copy");
    activate(model, "saves-delete-confirm");
    requests = model.TakeCloudRequests();
    check(requests.size() == 1 && requests[0].kind == CloudRequestKind::RemoveLocal && requests[0].game_id == SteamId(43), "delete is sent");
    model.action("circle");

    // An uninstalled game's leftovers: removing them frees the kept data.
    activate(model, "save-44");
    check(!has_row(model, "saves-delete") && has_row(model, "saves-leftover"), "an uninstalled game offers Remove leftover data");
    activate(model, "saves-leftover");
    check(model.state().navigation.modals.back().body.find("1.9 GB") != std::string::npos, "the size is shown first");
    activate(model, "saves-leftover-confirm");
    requests = model.TakeCloudRequests();
    check(requests.size() == 1 && requests[0].kind == CloudRequestKind::RemoveLeftover && requests[0].game_id == SteamId(44) &&
              model.state().navigation.route == Route::Saves,
          "removal is sent and the list comes back");

    // While a game is checked, its actions wait.
    LiveCloudGame queued = synced;
    queued.queued = true;
    model.UpdateCloud({queued});
    activate(model, "save-43");
    check(saves_row(model, "saves-sync") && !saves_row(model, "saves-sync")->enabled, "actions wait while the game is checked");

    // The game page's More menu leads to the same page.
    check(model.action("details:43"), "details open");
    activate(model, "details-more");
    activate(model, "manage-saves");
    check(model.state().navigation.route == Route::SaveDetails && model.state().navigation.save_id == SteamId(43) &&
              model.state().navigation.modals.empty(),
          "More > Manage saves opens the game's saves");
}

// A move's job in Queue says "move": Pause, Resume and Cancel name it, and Cancel says the copy goes and the
// game stays. Once the title may use the copy, it can't be paused or canceled.
void live_move_job_actions_say_move() {
    using namespace playden::homebrew;
    auto model = make_live();
    model.UpdateAccount({AuthState::SignedIn, "123", "Synthetic", {}, {}, true});
    check(model.UpdateLibrary({{game(SteamId(42), "Moving")}, false, {}, false, false, {}}), "move job: library loaded");
    model.UpdateVolumes({{"internal", "Console storage", true, 2'000'000'000, 1'000'000'000},
                         {"usb", "USB Drive", true, 2'000'000'000, 1'000'000'000}}, "internal");
    LiveInstallJob job;
    job.game_id = SteamId(42);
    job.state = JobState::Downloading;
    job.volume_id = "usb";
    job.move = true;
    job.from_volume_id = "internal";
    model.UpdateInstalls({job}, {{SteamId(42), 100, "internal", true}});
    model.action("r1");
    model.action("r1");
    check(model.state().navigation.route == Route::Downloads, "move job: Queue open");
    activate(model, "job-42");
    check(find_row(model, "job-pause-42") && find_row(model, "job-pause-42")->title == "Pause move" &&
              find_row(model, "job-cancel-42")->title == "Cancel move…",
          "move job: its actions say move");
    activate(model, "job-cancel-42");
    const auto& confirmation = model.state().navigation.modals.back();
    check(confirmation.title == "Cancel the move?" &&
              confirmation.body == "The copy on USB Drive is deleted. The game stays on Console storage.",
          "move job: Cancel says the copy goes and the game stays");
    activate(model, "cancel");
    activate(model, "cancel");
    // A failed move to a drive that isn't connected: Cancel says its part copy stays there.
    model.UpdateVolumes({{"internal", "Console storage", true, 2'000'000'000, 1'000'000'000},
                         {"usb", "USB Drive", false, 2'000'000'000, 1'000'000'000}}, "internal");
    job.state = JobState::Failed;
    model.UpdateInstalls({job}, {{SteamId(42), 100, "internal", true}});
    activate(model, "job-42");
    activate(model, "job-cancel-42");
    check(model.state().navigation.modals.back().body ==
              "USB Drive isn't connected, so the part copied there stays. The game stays on Console storage.",
          "move job: Cancel to a disconnected drive says the copy stays there");
    activate(model, "cancel");
    activate(model, "cancel");
    // A drive that isn't listed at all starts the sentence with a capital.
    model.UpdateVolumes({{"internal", "Console storage", true, 2'000'000'000, 1'000'000'000}}, "internal");
    activate(model, "job-42");
    activate(model, "job-cancel-42");
    check(model.state().navigation.modals.back().body ==
              "The other drive isn't connected, so the part copied there stays. The game stays on Console storage.",
          "move job: an unlisted drive is named in a full sentence");
    activate(model, "cancel");
    activate(model, "cancel");
    model.UpdateVolumes({{"internal", "Console storage", true, 2'000'000'000, 1'000'000'000},
                         {"usb", "USB Drive", true, 2'000'000'000, 1'000'000'000}}, "internal");
    job.state = JobState::Paused;
    model.UpdateInstalls({job}, {{SteamId(42), 100, "internal", true}});
    activate(model, "job-42");
    check(find_row(model, "job-resume-42") && find_row(model, "job-resume-42")->title == "Resume move", "move job: Resume says move");
    activate(model, "cancel");
    job.state = JobState::Downloading;
    job.locked = true;
    model.UpdateInstalls({job}, {{SteamId(42), 100, "usb", true}});
    activate(model, "job-42");
    check(!find_row(model, "job-pause-42") && !find_row(model, "job-cancel-42"), "move job: a switching move can't be stopped");
    activate(model, "cancel");
    // A check says how far it got; the switch says so while it checks the copy again, then switches.
    const auto state_text = [&model] {
        const auto found = std::find_if(model.state().jobs.begin(), model.state().jobs.end(),
                                        [](const Job& entry) { return entry.game_id == SteamId(42); });
        return found == model.state().jobs.end() ? std::string() : found->state_text;
    };
    job.move_step = LiveInstallJob::MoveStep::Verifying;
    job.checked_files = 12'345;
    job.check_files = 61'384;
    model.UpdateInstalls({job}, {{SteamId(42), 100, "internal", true}});
    check(state_text() == "Checking the copy · 12,345 of 61,384 files", "move job: the check counts its files");
    job.move_step = LiveInstallJob::MoveStep::Switching;
    job.checked_files = 900;
    job.check_files = 1'000;
    model.UpdateInstalls({job}, {{SteamId(42), 100, "internal", true}});
    check(state_text() == "Checking the copy · 900 of 1,000 files", "move job: the switch's check counts too");
    job.checked_files = 1'000;
    model.UpdateInstalls({job}, {{SteamId(42), 100, "internal", true}});
    check(state_text() == "Switching to the copy", "move job: then it switches");
}

void live_completed_downloads_follow_the_queue() {
    using namespace playden::homebrew;
    auto model = make_live();
    model.SetWallClock(100'000);
    model.UpdateAccount({AuthState::SignedIn, "123", "Synthetic", {}, {}, true});
    check(model.UpdateLibrary({{game(SteamId(42), "Finished"), game(SteamId(43), "Downloading"), game(SteamId(45), "Earlier"), game(SteamId(46), "Removed")},
                               false, {}, false, false, {}}), "library loaded");
    // 46 is listed by the service but no longer installed; 43 is downloading again.
    model.UpdateInstalls({{SteamId(43), JobState::Downloading, 10, 100, {}, "internal"}},
                         {{SteamId(42), 100, "internal", true}, {SteamId(43), 50, "internal", true}, {SteamId(45), 300, "m2", true}},
                         {{SteamId(42), 100, "internal", 99'880}, {SteamId(45), 300, "m2", 92'800}, {SteamId(43), 50, "internal", 99'000}, {SteamId(46), 9, "internal", 99'990}});
    const auto& jobs = model.state().jobs;
    check(jobs.size() == 3 && jobs[0].game_id == SteamId(43) && jobs[0].state == JobState::Downloading, "the queue comes first");
    check(jobs[1].game_id == SteamId(42) && jobs[1].state == JobState::Downloaded && jobs[1].finished_at == 99'880 &&
              jobs[1].completed_bytes == 100 && jobs[1].total_bytes == 100 && jobs[2].game_id == SteamId(45) && jobs[2].volume_id == "m2",
          "finished downloads follow it, newest first, without games downloading again or uninstalled");
    check(model.state().volumes.empty() || std::all_of(model.state().volumes.begin(), model.state().volumes.end(),
                                                       [](const Volume& volume) { return volume.reserved_bytes == 0 || volume.id == "internal"; }),
          "finished downloads reserve no space");

    model.action("r1");
    model.action("r1");
    check(model.state().navigation.route == Route::Downloads, "Downloads open");
    check(has_row(model, "job-43") && has_row(model, "job-42") && has_row(model, "job-45"), "every entry has a row");
    activate(model, "job-42");
    check(has_row(model, "job-open-42") && has_row(model, "job-dismiss-42") && has_row(model, "job-clear-completed") &&
              !has_row(model, "job-cancel-42"), "a finished download offers Open, Dismiss and Clear completed");
    activate(model, "job-dismiss-42");
    auto requests = model.TakeInstallRequests();
    check(requests.size() == 1 && requests[0].kind == InstallRequestKind::DismissCompleted && requests[0].game_id == SteamId(42),
          "Dismiss tells the download service");
    check(!has_row(model, "job-42") && has_row(model, "job-45"), "the dismissed entry goes at once");

    activate(model, "job-45");
    check(!has_row(model, "job-clear-completed"), "one finished download needs no Clear completed");
    model.action("circle");
    model.UpdateInstalls({}, {{SteamId(42), 100, "internal", true}, {SteamId(43), 50, "internal", true}, {SteamId(45), 300, "m2", true}},
                         {{SteamId(43), 50, "internal", 99'950}, {SteamId(45), 300, "m2", 92'800}});
    activate(model, "job-43");
    activate(model, "job-clear-completed");
    requests = model.TakeInstallRequests();
    check(requests.size() == 1 && requests[0].kind == InstallRequestKind::ClearCompleted, "Clear completed tells the service");
    check(std::none_of(model.state().jobs.begin(), model.state().jobs.end(),
                       [](const Job& job) { return job.state == JobState::Downloaded; }) && !has_row(model, "queue-downloads"),
          "Clear completed empties the list");
}

void live_queue_game_page_returns_to_its_row() {
    using namespace playden::homebrew;
    auto model = make_live();
    model.SetWallClock(100'000);
    model.UpdateAccount({AuthState::SignedIn, "123", "Synthetic", {}, {}, true});
    check(model.UpdateLibrary({{game(SteamId(42), "Updated"), game(SteamId(43), "Installed"), game(SteamId(44), "Another"),
                                game(SteamId(46), "One more"), game(SteamId(47), "And another"), game(SteamId(45), "Earlier")},
                               false, {}, false, false, {}}), "library loaded");
    // 42 finished an update that downloaded 30 of its 100 bytes. 45 is far down the list.
    model.UpdateInstalls({}, {{SteamId(42), 100, "internal", true}, {SteamId(43), 50, "internal", true}, {SteamId(44), 10, "internal", true},
                              {SteamId(46), 10, "internal", true}, {SteamId(47), 10, "internal", true}, {SteamId(45), 300, "m2", true}},
                         {{SteamId(43), 50, "internal", 99'990}, {SteamId(42), 100, "internal", 99'880, 30}, {SteamId(44), 10, "internal", 99'870},
                          {SteamId(46), 10, "internal", 99'860}, {SteamId(47), 10, "internal", 99'850}, {SteamId(45), 300, "m2", 92'800}});
    const auto& jobs = model.state().jobs;
    const auto updated = std::find_if(jobs.begin(), jobs.end(), [](const Job& job) { return job.game_id == SteamId(42); });
    const auto installed = std::find_if(jobs.begin(), jobs.end(), [](const Job& job) { return job.game_id == SteamId(43); });
    check(updated != jobs.end() && updated->update && updated->total_bytes == 30 && updated->completed_bytes == 30,
          "a finished update lists what it downloaded, not the whole game");
    check(installed != jobs.end() && !installed->update && installed->total_bytes == 50, "a finished install lists the game's size");

    model.action("r1");
    model.action("r1");
    check(model.state().navigation.route == Route::Downloads && model.state().queue_expanded == kQueueDownloads, "Downloads open");
    // The page's first action, as the Library opens it.
    check(model.action("details:45"), "details open");
    const auto first_action = model.state().navigation.focus_key;
    model.action("circle");
    check(model.state().navigation.route == Route::Downloads, "back in Downloads");
    activate(model, "job-45");
    activate(model, "job-open-45");
    check(model.state().navigation.route == Route::Details && model.state().navigation.detail_id == SteamId(45), "Open game shows its page");
    check(model.state().navigation.focus_key == first_action,
          "the page starts on its first action, not on the button at the Queue row's place");
    model.action("circle");
    check(model.state().navigation.route == Route::Downloads && model.state().navigation.focus_key == "job-45" &&
              model.state().queue_expanded == kQueueDownloads && has_row(model, "job-45"),
          "Back returns to the same row, with Downloads still open");

}

void game_collections_sheet_adds_and_removes_the_game() {
    using namespace playden::homebrew;
    auto model = make_live();
    model.UpdateAccount({AuthState::SignedIn, "123", "Synthetic", {}, {}, true});
    check(model.UpdateLibrary({{game(SteamId(42), "Metro"), game(SteamId(43), "Other")}, false, {}, false, false, {}}), "library loaded");
    check(model.action("details:42") && model.action("focus:details-more") && model.action("cross"), "game actions open");
    activate(model, "collection");
    check(model.state().navigation.modals.back().id == "collections" && has_row(model, "collection-new") &&
              !has_row(model, "collection-manage") && find_row(model, "collection-new")->title == "New collection…",
          "with no collections the sheet offers New collection");

    activate(model, "collection-new");
    model.text("To test");
    check(model.action("done"), "the name is entered");
    const auto& collections = model.state().library.collections;
    check(collections.size() == 1 && collections[0].name == "To test" && collections[0].members.contains(SteamId(42)),
          "a collection made from a game's sheet holds that game");
    const auto toggle = "collection-toggle-" + collections[0].id;
    const auto* row = find_row(model, toggle);
    check(row && row->title == "To test" && row->selected && row->value == "1 game" &&
              model.state().navigation.focus_key == toggle && has_row(model, "collection-manage"),
          "the new collection shows in the open sheet, checked and focused");

    check(model.action("cross"), "the checkbox toggles");
    check(!model.state().library.collections[0].members.contains(SteamId(42)) && !find_row(model, toggle)->selected &&
              model.state().navigation.modals.back().id == "collections",
          "unchecking removes the game and keeps the sheet open");

    activate(model, "collection-manage");
    const auto id = model.state().library.collections[0].id;
    check(find_row(model, "collection-rename-" + id)->title == "Rename \u201cTo test\u201d\u2026" &&
              find_row(model, "collection-pin-" + id) && find_row(model, "collection-delete-" + id)->dangerous,
          "Manage collections renames, pins and deletes each collection");
    activate(model, "collection-rename-" + id);
    model.text(" later");
    check(model.action("done"), "the new name is entered");
    check(model.state().library.collections[0].name == "To test later" &&
              find_row(model, "collection-rename-" + id)->title == "Rename \u201cTo test later\u201d\u2026",
          "a rename shows in the open sheet");
}

void live_download_card_keeps_queue_order() {
    using namespace playden::homebrew;
    auto model = make_live();
    // The download retried a moment ago waits in the queue; a failed one behind it must not take the card.
    model.UpdateInstalls({{SteamId(42), JobState::Queued, 0, 100, {}}, {SteamId(43), JobState::Failed, 0, 100, "Could not reach Steam."}}, {});
    model.action("r1");
    model.action("r1");
    check(model.state().navigation.route == Route::Downloads, "Downloads open");
    check(model.view().rows.size() > 2 && model.view().rows[0].key == "queue-downloads" &&
              model.view().rows[1].key == "job-42" && model.view().rows[2].key == "job-43",
          "the first download in the queue keeps the top spot ahead of a failed one");
    model.UpdateInstalls({{SteamId(42), JobState::Queued, 0, 100, {}}, {SteamId(43), JobState::Downloading, 5, 100, {}}}, {});
    check(model.view().rows[1].key == "job-43", "a running download takes the top spot");
}

void live_download_reorder_reaches_service() {
    using namespace playden::homebrew;
    auto model = make_live();
    model.UpdateInstalls({{SteamId(43), JobState::Downloading, 10, 100, {}},
                         {SteamId(44), JobState::Queued, 0, 100, {}},
                         {SteamId(45), JobState::Queued, 0, 100, {}}}, {});
    model.action("r1");
    model.action("r1");
    check(model.state().navigation.route == Route::Downloads, "Downloads open for reordering");
    activate(model, "job-44");
    activate(model, "job-down-44");
    auto requests = model.TakeInstallRequests();
    check(requests.size() == 1 && requests[0].game_id == SteamId(44) && requests[0].kind == InstallRequestKind::MoveDown,
          "Move down sends the order change to the download service");
    activate(model, "job-45");
    activate(model, "job-up-45");
    requests = model.TakeInstallRequests();
    check(requests.size() == 1 && requests[0].game_id == SteamId(45) && requests[0].kind == InstallRequestKind::MoveUp,
          "Move up sends the order change to the download service");
}

void live_sign_in_on_full_storage_offers_uninstall() {
    auto model = make_live();
    constexpr std::uint64_t gb = 1'000'000'000;
    model.UpdateVolumes({{"internal", "Console storage", true, 800 * gb, 0}}, "internal");
    model.UpdateInstalls({}, {{SteamId(12), 5 * gb, "internal"}, {SteamId(13), 1 * gb, "internal"}});
    playden::homebrew::AccountUpdate full;
    full.authentication = AuthState::Failed;
    full.error = "Playden can't save your sign-in.";
    full.storage_full = true;
    model.UpdateAccount(full);
    check(model.view().title == "Console storage is full", "a sign-in that can't be saved says storage is full");
    const auto* installed = find_row(model, "signin-installed");
    check(installed && installed->enabled && installed->value == "2 games · 6.0 GB" && has_row(model, "qr-retry"),
          "it offers the installed games and Retry");
    check(model.state().navigation.focus_key == "signin-installed", "the installed games are focused");
    activate(model, "signin-installed");
    check(model.state().navigation.route == Route::ManageGames, "the list opens to uninstall from");
    model.UpdateAccount(full);
    check(model.state().navigation.route == Route::ManageGames, "the retry every few seconds leaves the open list alone");
    full.error = "Playden still can't save your sign-in.";
    model.UpdateAccount(full);
    check(model.state().navigation.route == Route::ManageGames, "a retry that fails again leaves it open too");
    model.action("circle");
    check(model.state().navigation.route == Route::SignIn && model.state().navigation.focus_key == "signin-installed",
          "Circle returns to the sign-in screen");
    // The saved sign-in goes through while the screen is open: Circle goes Home, not back to a sign-in.
    activate(model, "signin-installed");
    model.UpdateAccount({AuthState::SignedIn, "123", "Synthetic", {}, {}, true});
    check(model.state().navigation.route == Route::ManageGames, "a sign-in that works leaves the screen open");
    model.action("circle");
    check(model.state().navigation.route == Route::Home, "Circle then goes Home");
    model.UpdateAccount(full);

    playden::homebrew::AccountUpdate busy;
    busy.authentication = AuthState::Failed;
    busy.error = "Account storage is unavailable. Retrying your saved sign-in…";
    model.UpdateAccount(busy);
    check(model.view().title == "Connect Steam" && !has_row(model, "signin-installed"),
          "other storage trouble keeps the usual screen");
}

// Keys of the Queue rows in screen order, storage left out.
std::vector<std::string> queue_keys(const Simulation& model) {
    std::vector<std::string> keys;
    for (const auto& row : model.view().rows)
        if (row.key.rfind("storage-", 0) != 0) keys.push_back(row.key);
    return keys;
}

const playden::homebrew::ViewRow* queue_row(const Simulation& model, const std::string& key) {
    for (const auto& row : model.view().rows)
        if (row.key == key) return &row;
    return nullptr;
}

// Queue: an imported game copied into Playden runs in Downloads, counted as copying, not downloading.
void live_queue_counts_copies() {
    using namespace playden::homebrew;
    auto model = make_live();
    model.UpdateAccount({AuthState::SignedIn, "123", "Synthetic", {}, {}, true});
    const auto rally = playden::GameId::Local("6d1f3a52-0c55-4f3e-9b2a-5d1c7e0a9f11");
    const auto sky = playden::GameId::Local("7c9e6679-7425-40de-944b-e07fc1f90ae7");
    check(model.UpdateLibrary({{game(SteamId(20), "Hollow")}, false, {}, false, false, {}}), "Steam library");
    LibraryUpdate imported{{game(rally, "Rally"), game(sky, "Sky")}, false, {}, false, false, {}};
    imported.source = playden::Store::kLocal;
    check(model.UpdateLibrary(imported), "imported library");
    model.UpdateInstalls({{rally, JobState::Downloading, 64, 100, {}}, {SteamId(20), JobState::Queued, 0, 100, {}}},
                         {{sky, 1, "internal", true}}, {{sky, 1, "internal", 1'000}});
    model.action("r1");
    model.action("r1");
    check(model.state().navigation.route == Route::Downloads && model.view().subtitle == "1 copying · 1 waiting",
          "the page counts the copy as copying");
    check(queue_row(model, "queue-downloads")->value == "1 copying · 1 waiting · 1 completed",
          "Downloads counts the copy as copying");
}

// Queue: both workflows, one section expanded at a time. A collapsed section keeps its card and counts.
// Waiting preparations are numbered in the runtime's order, completed ones follow them in the same list,
// and Prepare next moves a waiting game first without touching the one being prepared.
void live_queue_shows_downloads_and_preparations() {
    using namespace playden::homebrew;
    auto model = make_live();
    model.UpdateAccount({AuthState::SignedIn, "123", "Synthetic", {}, {}, true});
    check(model.UpdateLibrary({{game(SteamId(10), "Ninja"), game(SteamId(11), "Shovel"), game(SteamId(12), "Celeste"), game(SteamId(13), "Stardew"),
                                game(SteamId(14), "Thomas"), game(SteamId(20), "Hollow"), game(SteamId(21), "Dead Cells"), game(SteamId(22), "Hades"),
                                game(SteamId(23), "Monaco")}, false, {}, false, false, {}}), "library loaded");
    model.UpdateInstalls({{SteamId(20), JobState::Downloading, 64, 100, {}}, {SteamId(21), JobState::Queued, 0, 100, {}},
                          {SteamId(22), JobState::Queued, 0, 100, {}}},
                         {{SteamId(10), 1, "internal", true}, {SteamId(11), 1, "internal", true}, {SteamId(12), 1, "internal", true},
                          {SteamId(13), 1, "internal", true}, {SteamId(14), 1, "internal", true}, {SteamId(23), 1, "internal", true}},
                         {{SteamId(23), 1, "internal", 1'000}});
    LiveRuntimeGame preparing; preparing.game_id = SteamId(10); preparing.status = "Preparing"; preparing.detail = "Creating game files…";
    std::vector<LiveRuntimeGame> runtime{preparing};
    for (const std::uint32_t id : {11u, 12u, 13u}) {
        LiveRuntimeGame waiting; waiting.game_id = SteamId(id); waiting.status = "Not prepared"; waiting.queued = true;
        runtime.push_back(waiting);
    }
    LiveRuntimeGame ready; ready.game_id = SteamId(14); ready.status = "Ready"; ready.ready = true; ready.prepared = true;
    runtime.push_back(ready);
    model.UpdateRuntime(RuntimeState::Ready, runtime);
    model.UpdatePreparationQueue({SteamId(10), {SteamId(11), SteamId(12), SteamId(13)}, {{SteamId(14), 2'000}}, {}});
    model.action("r1");
    model.action("r1");
    check(model.state().navigation.route == Route::Downloads && model.view().title == "Queue", "the Queue tab opens");
    check(model.view().subtitle == "1 downloading · 1 preparing · 5 waiting", "the page counts running and waiting work");
    check((queue_keys(model) == std::vector<std::string>{"queue-downloads", "job-20", "queue-preparation", "prep-active-10",
                                                         "prep-11", "prep-12", "prep-13", "prep-done-14-2000"}),
          "games waiting for preparation open the Preparation section, its completed entry last");
    check(queue_row(model, "queue-downloads")->value == "2 more games queued · 1 completed",
          "collapsed Downloads keeps its card, how many more wait, and its completed count");
    check(queue_row(model, "queue-preparation")->value == "1 preparing · 3 waiting · 1 completed",
          "expanded Preparation separates preparing from waiting");
    check(queue_row(model, "prep-11")->value == "Up next" && queue_row(model, "prep-11")->subtitle == "Starts after Ninja" &&
              queue_row(model, "prep-12")->value == "Queued", "the first waiting game is up next");

    activate(model, "queue-downloads");
    check((queue_keys(model) == std::vector<std::string>{"queue-downloads", "job-20", "job-21", "job-22", "job-23",
                                                         "queue-preparation", "prep-active-10"}),
          "Downloads expands and Preparation collapses to its card");
    check(queue_row(model, "queue-preparation")->value == "3 more games queued · 1 completed", "collapsed Preparation counts");
    model.UpdateInstalls({{SteamId(20), JobState::Downloading, 70, 100, {}}, {SteamId(21), JobState::Queued, 0, 100, {}},
                          {SteamId(22), JobState::Queued, 0, 100, {}}},
                         {{SteamId(10), 1, "internal", true}, {SteamId(11), 1, "internal", true}, {SteamId(12), 1, "internal", true},
                          {SteamId(13), 1, "internal", true}, {SteamId(14), 1, "internal", true}, {SteamId(23), 1, "internal", true}},
                         {{SteamId(23), 1, "internal", 1'000}});
    check(has_row(model, "job-21") && !has_row(model, "prep-11"), "progress keeps the player's choice");
    check(model.action("focus:job-23") && model.action("down") && model.state().navigation.focus_key == "queue-preparation",
          "Down from the last download reaches the Preparation heading");
    check(has_row(model, "job-21") && !has_row(model, "prep-11"), "moving onto the other heading doesn't open it");

    activate(model, "queue-downloads");
    check((queue_keys(model) == std::vector<std::string>{"queue-downloads", "job-20", "queue-preparation", "prep-active-10"}),
          "the open section's heading closes it, both keeping their cards");
    check(model.state().navigation.focus_key == "queue-downloads", "focus stays on the heading");
    check(queue_row(model, "queue-downloads")->value == "2 more games queued · 1 completed", "closed Downloads counts");
    model.UpdateInstalls({{SteamId(20), JobState::Downloading, 80, 100, {}}, {SteamId(21), JobState::Queued, 0, 100, {}},
                          {SteamId(22), JobState::Queued, 0, 100, {}}},
                         {{SteamId(10), 1, "internal", true}, {SteamId(11), 1, "internal", true}, {SteamId(12), 1, "internal", true},
                          {SteamId(13), 1, "internal", true}, {SteamId(14), 1, "internal", true}, {SteamId(23), 1, "internal", true}},
                         {{SteamId(23), 1, "internal", 1'000}});
    check(!has_row(model, "job-21") && !has_row(model, "prep-11"), "both stay closed while the queues change");
    activate(model, "queue-preparation");
    activate(model, "prep-11");
    check(!has_row(model, "prep-next-11") && has_row(model, "prep-open-11"), "the game up next has no Prepare next");
    model.action("circle");
    activate(model, "prep-13");
    check(has_row(model, "prep-next-13"), "a later waiting game offers Prepare next");
    activate(model, "prep-next-13");
    check(model.TakePrepareNextRequests() == std::vector<GameId>{SteamId(13)}, "Prepare next reaches the runtime");
    check((queue_keys(model) == std::vector<std::string>{"queue-downloads", "job-20", "queue-preparation", "prep-active-10",
                                                         "prep-13", "prep-11", "prep-12", "prep-done-14-2000"}),
          "the game moves first, the others keep their order, the game being prepared stays");
    check(model.state().navigation.focus_key == "prep-13" && queue_row(model, "prep-13")->value == "Up next",
          "focus stays on the game, now up next");

    activate(model, "prep-done-14-2000");
    check(has_row(model, "prep-open-14") && has_row(model, "prep-dismiss-14-2000") && !has_row(model, "prep-clear-completed"),
          "a finished preparation offers Game details and Dismiss");
    activate(model, "prep-dismiss-14-2000");
    check((model.TakePreparedDismissals() == std::vector<LiveCompletedPreparation>{{SteamId(14), 2'000}}), "Dismiss reaches the runtime");
    check(!has_row(model, "prep-done-14-2000") && model.state().navigation.focus_key == "prep-12",
          "the entry goes and focus stays in the section");
    // The runtime's next snapshot, still listing it, doesn't bring it back.
    model.UpdatePreparationQueue({SteamId(10), {SteamId(13), SteamId(11), SteamId(12)}, {{SteamId(14), 2'000}}, {}});
    check(!has_row(model, "prep-done-14-2000"), "a dismissal waiting for the runtime holds");
}

// With nothing unfinished the completed entries stay; Clear completed clears only its own workflow.
// With nothing at all only the drives are left.
void live_queue_history_and_empty_states() {
    using namespace playden::homebrew;
    auto model = make_live();
    model.UpdateAccount({AuthState::SignedIn, "123", "Synthetic", {}, {}, true});
    check(model.UpdateLibrary({{game(SteamId(14), "Thomas"), game(SteamId(15), "Metal Slug"), game(SteamId(23), "Monaco")}, false, {}, false, false, {}}),
          "library loaded");
    model.action("r1");
    model.action("r1");
    check(model.view().subtitle.empty() && !has_row(model, "queue-downloads") && !has_row(model, "queue-preparation"),
          "nothing in the queue");
    model.UpdateInstalls({}, {{SteamId(14), 1, "internal", true}, {SteamId(15), 1, "internal", true}, {SteamId(23), 1, "internal", true}},
                         {{SteamId(23), 1, "internal", 1'000}});
    model.UpdatePreparationQueue({std::nullopt, {}, {{SteamId(15), 3'000}, {SteamId(14), 2'000}}, {}});
    check(model.view().subtitle == "No active work · 3 completed", "history alone says so");
    check(queue_row(model, "queue-downloads")->value == "1 completed" &&
              queue_row(model, "queue-preparation")->value == "2 completed", "both headings keep their history");
    activate(model, "queue-preparation");
    check(has_row(model, "prep-done-15-3000") && has_row(model, "prep-done-14-2000") && !has_row(model, "job-23"),
          "a section with only completed entries expands");
    activate(model, "prep-done-15-3000");
    activate(model, "prep-clear-completed");
    check(model.TakePreparedClear() && model.TakeInstallRequests().empty(), "Clear completed clears only preparations");
    // Games waiting with none being prepared (Steam details still on their way) still count when collapsed.
    model.UpdatePreparationQueue({std::nullopt, {SteamId(14)}, {{SteamId(15), 4'000}}, {}});
    check(model.state().queue_expanded == "downloads", "focus left on a download keeps Downloads open");
    check(queue_row(model, "queue-preparation")->value == "1 waiting · 1 completed", "a collapsed section counts what waits");
    model.UpdatePreparationQueue({});
    check(!has_row(model, "queue-preparation") && has_row(model, "job-23"), "Preparation empties; Downloads keeps its entry");
}

// Queue lists uninstalls in Preparation: those at work, those waiting their turn, a stop with its reason
// and finished ones, counted with the preparations and in the tab's badge.
void live_queue_lists_uninstalls() {
    using namespace playden::homebrew;
    constexpr std::int64_t now = 1'800'000'000;
    auto model = make_live();
    model.SetWallClock(now);
    model.UpdateAccount({AuthState::SignedIn, "123", "Synthetic", {}, {}, true});
    check(model.UpdateLibrary({{game(SteamId(10), "Ninja"), game(SteamId(11), "Shovel"), game(SteamId(40), "Alpha"), game(SteamId(41), "Beta"),
                                game(SteamId(42), "Gamma")}, false, {}, false, false, {}}), "library loaded");
    model.UpdateInstalls({}, {{SteamId(10), 1, "internal", true}, {SteamId(11), 1, "internal", true}, {SteamId(40), 1, "internal", true},
                              {SteamId(41), 1, "internal", true}, {SteamId(42), 1, "internal", true}});
    model.UpdatePreparationQueue({SteamId(10), {SteamId(11)}, {}, {},
                                  {{SteamId(40), "Removing PS5 title…"}, {SteamId(41), ""}, {SteamId(42), ""}}});
    model.action("r1");
    model.action("r1");
    check((queue_keys(model) == std::vector<std::string>{"queue-preparation", "prep-active-10", "prep-unin-40", "prep-11", "prep-unin-41",
                                                         "prep-unin-42"}),
          "a preparation keeps the card; the uninstall at work follows it, those waiting come after the waiting preparations");
    check(queue_row(model, "queue-preparation")->value == "1 preparing · 1 uninstalling · 3 waiting" &&
              model.view().subtitle == "1 preparing · 1 uninstalling · 3 waiting",
          "the heading and the page count uninstalls at work and waiting");
    check(queue_row(model, "prep-unin-40")->value == "Uninstalling" && queue_row(model, "prep-unin-40")->subtitle == "Removing PS5 title…" &&
              queue_row(model, "prep-unin-40")->tone == "busy",
          "the uninstall at work says its step");
    check(queue_row(model, "prep-unin-41")->value == "Waiting to uninstall" && queue_row(model, "prep-unin-41")->subtitle.empty() &&
              queue_row(model, "prep-11")->value == "Up next",
          "a waiting uninstall says so, unnumbered; the preparation queue keeps its own order");
    check(QueuePending(model.state()) == 5, "the tab badge counts preparations and uninstalls running or waiting");
    activate(model, "prep-unin-41");
    check(has_row(model, "prep-open-41") && !has_row(model, "prep-next-41") && model.view().title == "Uninstall actions",
          "a waiting uninstall offers its game's page");
    model.action("circle");

    // With no preparation running the first uninstall at work takes the card.
    model.UpdatePreparationQueue({std::nullopt, {}, {}, {},
                                  {{SteamId(40), "Deleting files…"}, {SteamId(41), "Removing PS5 title…"}, {SteamId(42), ""}}});
    const auto layout = BuildQueueLayout(model.state());
    check(layout.preparation.active && layout.preparation.active->key == "prep-unin-40" && layout.preparation.active->uninstall &&
              layout.preparation.active->status == "Uninstalling" && layout.preparation.active->note == "Deleting files…",
          "the uninstall at work takes the card");
    check((queue_keys(model) == std::vector<std::string>{"queue-preparation", "prep-unin-40", "prep-unin-41", "prep-unin-42"}),
          "a second one at work is a row after the card, then the one waiting");
    check(queue_row(model, "queue-preparation")->value == "2 uninstalling · 1 waiting", "both at work are counted");
    activate(model, "queue-preparation");
    check(queue_row(model, "queue-preparation")->value == "2 more games queued", "a closed section counts uninstalls after its card");
    activate(model, "queue-preparation");

    // One finishes, one stops (on its own, so the stop is a dialog too), one goes on.
    model.NotifyUninstallFinished(SteamId(40));
    model.NotifyUninstallStopped(SteamId(41), "Choose which Cloud saves to keep, then uninstall again.");
    activate(model, "alert-ok");
    model.UpdatePreparationQueue({std::nullopt, {}, {}, {}, {{SteamId(42), "Removing PS5 title…"}}});
    const auto done_key = "prep-unin-done-40-" + std::to_string(now);
    check((queue_keys(model) == std::vector<std::string>{"queue-preparation", "prep-unin-42", "prep-unin-attn-41", done_key}),
          "the one at work, then the stop, then the finished one");
    check(queue_row(model, "prep-unin-attn-41")->value == "Uninstall stopped" &&
              queue_row(model, "prep-unin-attn-41")->subtitle == "Choose which Cloud saves to keep, then uninstall again." &&
              queue_row(model, "prep-unin-attn-41")->tone == "attention",
          "a stopped uninstall needs the player and says why");
    check(queue_row(model, done_key)->value == "Uninstalled" && queue_row(model, done_key)->tone == "good", "a finished one says so");
    check(queue_row(model, "queue-preparation")->value == "1 uninstalling · 1 needs attention · 1 completed" &&
              model.view().subtitle == "1 uninstalling · 1 needs attention",
          "stops and finishes are counted apart from the work");
    check(QueuePending(model.state()) == 1, "the badge counts only what runs or waits");

    // A stop and a finish can be dismissed; Clear completed takes finished uninstalls with finished preparations.
    activate(model, "prep-unin-attn-41");
    check(has_row(model, "prep-open-41") && has_row(model, "uninstall-dismiss-41-" + std::to_string(now)),
          "a stopped uninstall offers its game's page and Dismiss");
    activate(model, "uninstall-dismiss-41-" + std::to_string(now));
    check(!has_row(model, "prep-unin-attn-41"), "Dismiss takes the stop off the list");
    activate(model, done_key);
    check(has_row(model, "uninstall-dismiss-40-" + std::to_string(now)) && !has_row(model, "prep-clear-completed"),
          "a finished uninstall offers Dismiss");
    model.action("circle");
    model.UpdatePreparationQueue({std::nullopt, {}, {{SteamId(10), now - 60}}, {}, {}});
    check((queue_keys(model) == std::vector<std::string>{"queue-preparation", done_key, "prep-done-10-" + std::to_string(now - 60)}),
          "finished uninstalls and preparations are listed together, newest first");
    activate(model, done_key);
    activate(model, "prep-clear-completed");
    check(model.TakePreparedClear() && model.state().ended_uninstalls.empty(), "Clear completed clears both");

    // A game shows once: finished, it leaves the running list at once (before the controller's next
    // list); uninstalled again, its earlier finish is not listed while it goes.
    model.UpdatePreparationQueue({std::nullopt, {}, {}, {}, {{SteamId(42), "Deleting files…"}}});
    model.NotifyUninstallFinished(SteamId(42));
    const auto gamma_done = "prep-unin-done-42-" + std::to_string(now);
    check((queue_keys(model) == std::vector<std::string>{"queue-preparation", gamma_done}),
          "a finished uninstall is listed once, as done, at once");
    model.UpdatePreparationQueue({std::nullopt, {}, {}, {}, {{SteamId(42), "Deleting files…"}}});
    check((queue_keys(model) == std::vector<std::string>{"queue-preparation", "prep-unin-42"}),
          "while the game is uninstalled again its earlier finish is not listed");

    // Signing out drops the account's ended uninstalls; one still running may finish, unlisted.
    model.UpdatePreparationQueue({std::nullopt, {}, {}, {}, {{SteamId(41), "Removing PS5 title…"}}});
    model.NotifyUninstallStopped(SteamId(40), "Its drive is not connected. Connect it, then uninstall again.");
    while (!model.state().navigation.modals.empty()) model.action("circle");
    check(model.state().ended_uninstalls.size() == 2, "a finish and a stop are listed");
    model.UpdateAccount({AuthState::SignedOut, {}, {}, {}, {}, true});
    check(model.state().ended_uninstalls.empty(), "signing out drops the previous account's ended uninstalls");
    model.NotifyUninstallFinished(SteamId(41));
    check(model.state().ended_uninstalls.empty(), "an uninstall of the previous account that ends later is not listed");
}

// A game whose preparation needs the player stays in Preparation without a running preparation.
void live_queue_lists_preparations_needing_attention() {
    using namespace playden::homebrew;
    auto model = make_live();
    model.UpdateAccount({AuthState::SignedIn, "123", "Synthetic", {}, {}, true});
    check(model.UpdateLibrary({{game(SteamId(30), "Waits"), game(SteamId(31), "Broke"), game(SteamId(32), "Refused"), game(SteamId(33), "Fine")}, false, {}, false,
                               false, {}}), "library loaded");
    LiveRuntimeGame waits; waits.game_id = SteamId(30); waits.status = "Waiting for the PS5"; waits.awaiting_registration = true;
    LiveRuntimeGame broke; broke.game_id = SteamId(31); broke.status = "Failed"; broke.detail = "Disk full"; broke.can_prepare = true;
    LiveRuntimeGame refused; refused.game_id = SteamId(32); refused.status = "Not prepared"; refused.can_prepare = true;
    LiveRuntimeGame fine; fine.game_id = SteamId(33); fine.status = "Ready"; fine.ready = true;
    model.UpdateRuntime(RuntimeState::Ready, {waits, broke, refused, fine});
    model.UpdatePreparationQueue({std::nullopt, {}, {}, {{SteamId(32), "No verified runtime kit is installed."}}});
    model.action("r1");
    model.action("r1");
    check((queue_keys(model) == std::vector<std::string>{"queue-preparation", "prep-attn-30", "prep-attn-31", "prep-attn-32"}),
          "waiting for the PS5, failed and refused games are listed");
    check(queue_row(model, "queue-preparation")->value == "3 need attention" && model.view().subtitle == "3 need attention",
          "they are not counted as waiting work");
    check(queue_row(model, "prep-attn-30")->value == "Waiting for the PS5" &&
              queue_row(model, "prep-attn-31")->subtitle == "Disk full" &&
              queue_row(model, "prep-attn-32")->subtitle == "No verified runtime kit is installed.",
          "each says why");
    activate(model, "prep-attn-31");
    check(has_row(model, "prep-retry-31"), "a failed preparation can be tried again");
    activate(model, "prep-retry-31");
    check(model.TakeRuntimePrepareRequests() == std::vector<GameId>{SteamId(31)}, "Try again asks for it");
    activate(model, "prep-attn-30");
    check(!has_row(model, "prep-retry-30"), "a game waiting for the PS5 is not prepared again from here");
    model.action("circle");
    // Closed with one being prepared: what needs the player is never counted as queued.
    model.UpdatePreparationQueue({SteamId(33), {}, {}, {{SteamId(32), "No verified runtime kit is installed."}}});
    check(model.action("focus:queue-preparation"), "the heading takes focus");
    activate(model, "queue-preparation");
    check(!has_row(model, "prep-attn-30") &&
              queue_row(model, "queue-preparation")->value == "No other games queued · 3 need attention",
          "closed, it counts what needs attention apart from the queue");
}

// A game Playden refused to prepare (SteamStub it can't unpack) is not stuck in Queue: its actions there uninstall it,
// or take an imported game playing from its folder out of the library, after the same question as its page asks.
void live_refused_preparation_offers_uninstall() {
    using namespace playden::homebrew;
    auto model = make_live();
    model.UpdateAccount({AuthState::SignedIn, "123", "Synthetic", {}, {}, true});
    const auto local = playden::GameId::Local("6d1f3a52-0c55-4f3e-9b2a-5d1c7e0a9f11");
    check(model.UpdateLibrary({{game(SteamId(36), "Doom 3"), game(SteamId(37), "Elsewhere")}, false, {}, false, false, {}}),
          "library loaded");
    LibraryUpdate imported{{game(local, "Rally")}, false, {}, false, false, {}};
    imported.source = playden::Store::kLocal;
    check(model.UpdateLibrary(imported), "imported library");
    model.UpdateInstalls({}, {{SteamId(36), 600'000'000}});
    model.UpdateExternalGames({{local, "usb0", true, ""}});
    const std::string drm = "This game's program has Steam DRM in a form Playden can't unpack yet.";
    LiveRuntimeGame doom; doom.game_id = SteamId(36); doom.status = "Not prepared"; doom.can_prepare = true;
    LiveRuntimeGame rally; rally.game_id = local; rally.status = "Not prepared"; rally.can_prepare = true;
    LiveRuntimeGame other; other.game_id = SteamId(37); other.status = "Not prepared"; other.can_prepare = true;
    model.UpdateRuntime(RuntimeState::Ready, {doom, rally, other});
    model.UpdatePreparationQueue({std::nullopt, {}, {}, {{SteamId(36), drm}, {local, drm}, {SteamId(37), drm}}});
    model.action("r1");
    model.action("r1");
    activate(model, "prep-attn-36");
    check(has_row(model, "prep-retry-36") && has_row(model, "prep-open-36") && has_row(model, "uninstall") &&
              find_row(model, "uninstall")->title == "Uninstall…" && find_row(model, "uninstall")->enabled,
          "a refused installed game can be uninstalled from Queue");
    activate(model, "uninstall");
    check(model.state().navigation.modals.size() == 2 && model.state().navigation.modals.back().title == "Uninstall this game?",
          "it asks first, as the game page does");
    while (!model.state().navigation.modals.empty()) model.action("circle");
    check(model.state().navigation.route == Route::Downloads && model.TakeInstallRequests().empty(), "Cancel asks for nothing");
    activate(model, "prep-attn-36");
    activate(model, "uninstall");
    activate(model, "confirm-destructive");
    const auto requests = model.TakeInstallRequests();
    check(requests.size() == 1 && requests[0].kind == InstallRequestKind::Uninstall && requests[0].game_id == SteamId(36) &&
              model.state().navigation.modals.empty() && model.state().navigation.route == Route::Downloads,
          "confirmed, that game is uninstalled and Queue stays open");
    // An imported game playing from its folder leaves the library instead.
    activate(model, "prep-attn-" + playden::ToKey(local));
    check(has_row(model, "import-remove") && !has_row(model, "uninstall"), "an imported game is removed from the library");
    activate(model, "import-remove");
    activate(model, "confirm-destructive");
    const auto removed = model.TakeImportRequests();
    check(removed.size() == 1 && removed[0].kind == ImportRequestKind::Remove && removed[0].game_id == local &&
              model.state().navigation.route == Route::Downloads,
          "confirmed, that game leaves the library and Queue stays open");
    // A game without files on this console has nothing to uninstall.
    activate(model, "prep-attn-37");
    check(!has_row(model, "uninstall") && !has_row(model, "import-remove"), "nothing to remove without its files");
}

// A setup an earlier kit left (its title or bottle from that kit) can't be finished by Try again: Queue and the
// game page offer Recreate bottle instead, say so in plain words, and ask before it runs.
void live_setup_from_an_earlier_kit_offers_recreate() {
    using namespace playden::homebrew;
    using Requests = std::vector<std::pair<GameId, MaintenanceKind>>;
    auto model = make_live();
    model.UpdateAccount({AuthState::SignedIn, "123", "Synthetic", {}, {}, true});
    check(model.UpdateLibrary({{game(SteamId(34), "Spire"), game(SteamId(35), "Broke")}, false, {}, false, false, {}}),
          "library loaded");
    model.UpdateInstalls({}, {{SteamId(34), 600'000'000}, {SteamId(35), 600'000'000}});
    const std::string words = "Started with an earlier kit · Recreate the bottle to finish (saves are kept)";
    LiveRuntimeGame spire; spire.game_id = SteamId(34); spire.status = "Needs recovery"; spire.has_title = true;
    spire.can_prepare = true; spire.setup_other_kit = true; spire.detail = words;
    LiveRuntimeGame broke; broke.game_id = SteamId(35); broke.status = "Failed"; broke.has_title = true;
    broke.can_prepare = true; broke.detail = "immutable artifacts or template provenance do not match";
    model.UpdateRuntime(RuntimeState::Ready, {spire, broke});
    model.action("r1");
    model.action("r1");
    check(queue_row(model, "prep-attn-34") && queue_row(model, "prep-attn-34")->value == "Interrupted" &&
              queue_row(model, "prep-attn-34")->subtitle == words,
          "the row says what to do, not the check that failed");
    activate(model, "prep-attn-34");
    check(has_row(model, "prep-recreate-34") && !has_row(model, "prep-retry-34") && has_row(model, "prep-open-34"),
          "Recreate bottle takes Try again's place");
    check(find_row(model, "prep-recreate-34")->title == "Recreate bottle…", "the action is named");
    activate(model, "prep-recreate-34");
    check(model.state().navigation.modals.size() == 1 && model.state().navigation.modals.back().title == "Recreate the bottle?",
          "it asks first, as Maintenance does");
    model.action("circle");
    check(model.state().navigation.modals.empty() && model.TakeMaintenanceRequests().empty(), "Cancel asks for nothing");
    activate(model, "prep-attn-34");
    activate(model, "prep-recreate-34");
    activate(model, "confirm-destructive");
    check(model.TakeMaintenanceRequests() == Requests{{SteamId(34), MaintenanceKind::RecreateBottle}} &&
              model.TakeRuntimePrepareRequests().empty(),
          "confirmed, it is Maintenance's Recreate bottle for that game");
    // A setup that failed under the installed kit is still tried again, with its own words.
    activate(model, "prep-attn-35");
    check(has_row(model, "prep-retry-35") && !has_row(model, "prep-recreate-35"), "other failures keep Try again");
    model.action("circle");
    check(queue_row(model, "prep-attn-35")->subtitle == "immutable artifacts or template provenance do not match",
          "a real integrity failure keeps its message");

    // The game page: the main action and its hint.
    spire.maintenance.reset(); spire.maintenance_running = false;
    model.UpdateRuntime(RuntimeState::Ready, {spire, broke});
    check(model.action("details:34"), "details open");
    const auto* play = find_row(model, "details-play");
    check(play && play->enabled && play->title == "Recreate bottle…" && play->action == "prep-recreate-34",
          "the game page offers Recreate bottle instead of Finish setup");
    check(find_row(model, "details-hint") &&
              find_row(model, "details-hint")->title ==
                  "Setup started with an earlier kit. Recreate bottle makes it again; saves and settings are kept.",
          "the hint says why");
    activate(model, "details-play");
    activate(model, "confirm-destructive");
    check(model.TakeMaintenanceRequests() == Requests{{SteamId(34), MaintenanceKind::RecreateBottle}},
          "the game page's Recreate bottle asks for the same");
    check(find_row(model, "details-play") && !find_row(model, "details-play")->enabled, "it waits while it runs");

    // Maintenance › Update runtime has nothing to update: it points to Recreate bottle, never "Already up to date".
    spire.activity = TitleActivity::Stopped;
    model.UpdateRuntime(RuntimeState::Ready, {spire, broke});
    activate(model, "details-more");
    activate(model, "game-maintenance");
    model.TakeActivityRequests();
    model.UpdateRuntime(RuntimeState::Ready, {spire, broke});
    const auto* update = find_row(model, "maintenance-update");
    check(update && !update->enabled &&
              update->subtitle == "Setup was started with an earlier kit. Use Recreate bottle to finish it; saves are kept.",
          "Update runtime points to Recreate bottle");
    const auto* recreate = find_row(model, "maintenance-recreate");
    check(recreate && recreate->enabled, "Recreate bottle is there to use");
}

// The game page of a waiting game names the game right ahead of it and offers Prepare next.
void live_details_prepare_next() {
    using namespace playden::homebrew;
    auto model = make_live();
    model.UpdateAccount({AuthState::SignedIn, "123", "Synthetic", {}, {}, true});
    check(model.UpdateLibrary({{game(SteamId(40), "Busy"), game(SteamId(41), "First"), game(SteamId(42), "Second")}, false, {}, false, false, {}}),
          "library loaded");
    model.UpdateInstalls({}, {{SteamId(40), 1, "internal", true}, {SteamId(41), 1, "internal", true}, {SteamId(42), 1, "internal", true}});
    LiveRuntimeGame busy; busy.game_id = SteamId(40); busy.status = "Preparing";
    LiveRuntimeGame first; first.game_id = SteamId(41); first.status = "Not prepared"; first.queued = true;
    LiveRuntimeGame second; second.game_id = SteamId(42); second.status = "Not prepared"; second.queued = true;
    model.UpdateRuntime(RuntimeState::Ready, {busy, first, second});
    model.UpdatePreparationQueue({SteamId(40), {SteamId(41), SteamId(42)}, {}, {}});
    check(model.action("details:41"), "details open");
    check(!has_row(model, "details-prepare-next") && find_row(model, "details-setup")->value == "Waiting for Busy",
          "the first waiting game waits for the one being prepared");
    model.action("circle");
    check(model.action("details:42"), "details open");
    check(find_row(model, "details-setup")->value == "Waiting for First" &&
              find_row(model, "details-hint")->title == "2 of 2 waiting. Setup starts when First is ready.",
          "a later game names the game right ahead and its place");
    activate(model, "details-prepare-next");
    check(model.TakePrepareNextRequests() == std::vector<GameId>{SteamId(42)} &&
              (model.state().preparation_queue.waiting == std::vector<GameId>{SteamId(42), SteamId(41)}), "Prepare next from the game page");
    check(find_row(model, "details-setup")->value == "Waiting for Busy" && !has_row(model, "details-prepare-next"),
          "now it is next");
}

// A Steam game's art is at the addresses Playden always used (the artwork backend accepts exactly
// these); a game from another store has none yet, so its tile shows its title.
void store_art_follows_the_store() {
    using namespace playden::homebrew;
    Game steam;
    steam.id = SteamId(440);
    check(CoverUrl(steam) == "https://cdn.cloudflare.steamstatic.com/steam/apps/440/library_600x900.jpg", "Steam cover");
    check(HeroUrl(steam) == "https://cdn.cloudflare.steamstatic.com/steam/apps/440/library_hero.jpg", "Steam hero");
    check(LogoUrl(steam) == "https://cdn.cloudflare.steamstatic.com/steam/apps/440/logo.png", "Steam logo");
    Game local;
    local.id = playden::GameId::Local("0f8fad5b-d9cb-469f-a165-70867728950e");
    check(CoverUrl(local).empty() && HeroUrl(local).empty() && LogoUrl(local).empty(), "imported games have no store art");
    check(CoverUrl(playden::GameId{}).empty(), "an invalid game has no art");
}

// A game of a store without Cloud saves (an imported game Playden copied in), with Steam signed in or
// out and automatic sync on: Details has no Cloud saves row, Play starts it and Finish setup quits to
// Home without asking the Cloud, Saves doesn't list it, and uninstalling says its saves are only here.
// A new Steam sign-in clears Steam's installs and downloads only.
void live_store_without_cloud_never_asks_the_cloud() {
    using playden::homebrew::LiveInstallJob;
    using playden::homebrew::LiveRuntimeGame;
    using playden::homebrew::RuntimeState;
    const auto local = playden::GameId::Local("0f8fad5b-d9cb-469f-a165-70867728950e");
    const auto key = playden::ToKey(local);
    for (const auto authentication : {AuthState::SignedIn, AuthState::SignedOut}) {
        auto model = make_live();
        sync_automatically(model);
        model.UpdateAccount({authentication, "123", "Synthetic", {}, {}, authentication == AuthState::SignedIn});
        if (authentication == AuthState::SignedIn)
            check(model.UpdateLibrary({{game(SteamId(42), "Steam game")}, false, {}, false, false, {}}), "Steam library");
        LibraryUpdate imported{{game(local, "Imported")}, false, {}, false, false, {}};
        imported.source = playden::Store::kLocal;
        check(model.UpdateLibrary(imported), "imported library");
        model.UpdateInstalls({}, {{local, 100'000'000, "internal", true}, {SteamId(42), 600'000'000, "internal", true}});
        LiveRuntimeGame ready; ready.game_id = local; ready.status = "Ready"; ready.ready = true; ready.can_play = true;
        ready.has_title = true; ready.prepared = true;
        model.UpdateRuntime(RuntimeState::Ready, {ready});
        (void)model.TakeCloudRequests();

        check(model.action("details:" + key), "details open");
        check(!has_row(model, "details-cloud"), "no Cloud saves row for an imported game");
        const auto* play = find_row(model, "details-play");
        check(play && play->enabled && play->action == "runtime-play", "Play starts it");
        activate(model, "details-play");
        check(model.TakeRuntimePlayRequests() == std::vector<playden::GameId>{local}, "Play reaches the runtime");
        check(model.TakeCloudRequests().empty(), "Play asks nothing of the Cloud");
        // Waiting for the PS5: Finish setup quits to Home straight away.
        LiveRuntimeGame waiting = ready; waiting.ready = false; waiting.can_play = false; waiting.status = "Waiting for the PS5";
        waiting.awaiting_registration = true; waiting.can_prepare = true;
        model.UpdateRuntime(RuntimeState::Ready, {waiting});
        check(find_row(model, "details-play")->action == "quit-to-home", "Finish setup quits to Home");
        activate(model, "details-play");
        check(model.state().quit_requested && model.TakeCloudRequests().empty(), "it quits without syncing saves");
    }
    {
        auto model = make_live();
        sync_automatically(model);
        model.SetWallClock(1'800'000'000);
        model.UpdateAccount({AuthState::SignedIn, "123", "Synthetic", {}, {}, true});
        check(model.UpdateLibrary({{game(SteamId(42), "Steam game")}, false, {}, false, false, {}}), "Steam library");
        LibraryUpdate imported{{game(local, "Imported")}, false, {}, false, false, {}};
        imported.source = playden::Store::kLocal;
        check(model.UpdateLibrary(imported), "imported library");
        const auto other = playden::GameId::Local("7c9e6679-7425-40de-944b-e07fc1f90ae7");
        LibraryUpdate more{{game(local, "Imported"), game(other, "Copying")}, false, {}, false, false, {}};
        more.source = playden::Store::kLocal;
        check(model.UpdateLibrary(more), "imported library");
        model.UpdateInstalls({{other, playden::homebrew::JobState::Downloading, 10, 100, {}, "internal"},
                              {SteamId(43), playden::homebrew::JobState::Queued, 0, 0, {}, "internal"}},
                             {{local, 100'000'000, "internal", true}, {SteamId(42), 600'000'000, "internal", true}});
        // Saves lists the Steam game only.
        model.action("l1");
        model.action("l1");
        check(model.state().navigation.route == Route::Saves, "Saves open");
        check(saves_row(model, "save-42") && !saves_row(model, "save-" + key), "Saves lists Steam games only");
        (void)model.TakeCloudRequests();  // opening Saves checks the Steam game
        // Uninstalling it says its saves exist only on this console.
        check(model.action("details:" + key) && model.action("focus:details-more") && model.action("cross"), "context opens");
        activate(model, "uninstall");
        check(model.state().navigation.modals.back().body.find("Its saves exist only on this console") != std::string::npos &&
                  model.state().navigation.modals.back().body.find("Cloud") == std::string::npos &&
                  model.state().navigation.modals.back().body.find("Steam") == std::string::npos,
              "no Cloud sync before uninstalling, and no word of Steam");
        check(model.action("circle") && model.TakeCloudRequests().empty(), "nothing asked of the Cloud");
        // A new Steam sign-in takes Steam's installs and downloads away, not the imported ones.
        model.UpdateAccount({AuthState::Waiting, {}, {}, "https://s.team/q/synthetic", {}, true});
        const auto& state = model.state();
        check(std::none_of(state.installations.begin(), state.installations.end(),
                           [](const playden::homebrew::Installation& item) { return item.game_id.store == playden::Store::kSteam; }) &&
                  std::any_of(state.installations.begin(), state.installations.end(),
                              [&](const playden::homebrew::Installation& item) { return item.game_id == local; }),
              "Steam's installs go, the imported game's stay");
        check(std::none_of(state.jobs.begin(), state.jobs.end(),
                           [](const playden::homebrew::Job& job) { return job.game_id.store == playden::Store::kSteam; }) &&
                  std::any_of(state.jobs.begin(), state.jobs.end(), [&](const playden::homebrew::Job& job) { return job.game_id == other; }),
              "Steam's downloads go, the imported game's copy stays");
    }
}

// Live: the Import sheet sends Look for games and the add to the Local store, follows the scan, and
// keeps the focus on its row when the candidates change.
void live_import_sheet_sends_requests() {
    using playden::homebrew::ImportRequestKind;
    using playden::homebrew::LiveImportCandidate;
    auto model = make_live();
    check(model.state().navigation.route == Route::Startup && has_row(model, "startup-import"), "Startup offers Import games");
    activate(model, "startup-import");
    check(model.view().surface == "import-games" && model.view().rows.size() == 2, "the empty sheet opens");
    activate(model, "import-look");
    auto requests = model.TakeImportRequests();
    check(requests.size() == 1 && requests.front().kind == ImportRequestKind::LookForGames, "Look for games reaches the store");
    model.UpdateImportCandidates({}, true);
    check(find_row(model, "import-look")->title == "Looking for games…" && !find_row(model, "import-look")->enabled,
          "the scan shows");
    model.action("cross");
    check(model.TakeImportRequests().empty(), "no second scan while one runs");
    LiveImportCandidate delores;
    delores.volume_id = "usb0"; delores.folder = "playden/import/Delores"; delores.path = "/mnt/usb0/playden/import/Delores";
    delores.title = "Delores"; delores.total_bytes = 230'000'000; delores.ascii_ok = true;
    delores.programs = {{"Delores.exe", 9'000'000, false}, {"setup.exe", 1'000'000, true}};
    LiveImportCandidate drop = delores;
    drop.volume_id = "internal"; drop.folder = "playden-import/Delores"; drop.path = "/data/playden-import/Delores";
    model.UpdateImportCandidates({delores, drop}, false);
    check(model.state().navigation.focus_key == "import-look" && find_row(model, "import-look")->enabled,
          "the focus stays on Look for games when the scan ends");
    check(has_row(model, "import-group-usb0") && has_row(model, "import-group-internal") &&
              find_row(model, "import-candidate-usb0:playden/import/Delores")->value == "230 MB",
          "candidates are listed by drive");
    activate(model, "import-candidate-internal:playden-import/Delores");
    check(model.view().surface == "import-choice" &&
              model.view().subtitle.rfind("Starts with Delores.exe, which you can change in Game settings.", 0) == 0,
          "one program besides an installer skips the program list");
    check(model.view().surface == "import-choice" && find_row(model, "import-play-in-place")->title == "Play from the drop folder" &&
              find_row(model, "import-copy")->title == "Import into Playden…" && !has_row(model, "import-move"),
          "a drop-folder game plays from there or is imported into Playden");
    activate(model, "import-play-in-place");
    requests = model.TakeImportRequests();
    check(requests.size() == 1 && requests.front().kind == ImportRequestKind::Add && requests.front().volume_id == "internal" &&
              requests.front().folder == "playden-import/Delores" && requests.front().program == "Delores.exe" &&
              requests.front().prepare,
          "choosing the program adds the folder, to be prepared there");
    check(model.view().surface == "import-games" &&
              model.state().navigation.focus_key == "import-candidate-internal:playden-import/Delores",
          "back on the sheet at the folder");
    // A new scan without the focused folder gives the focus to Look for games; an open program
    // list of a folder that went away closes.
    activate(model, "import-candidate-usb0:playden/import/Delores");
    model.UpdateImportCandidates({drop}, false);
    check(model.view().surface == "import-games" && model.state().navigation.focus_key == "import-look",
          "a folder that went away closes its wizard");
    check(model.action("circle") && model.state().navigation.modals.empty(), "the sheet closes");
}

// Live: the add choice (play in place, copy with its drive, move), the copy or move sent once the
// store gives the game its ID, the game's page once the library lists it, refusals said to the
// player, and the page of an imported game (Copy/Move, progress, Remove from library).
void live_import_choice_and_details() {
    using playden::homebrew::ImportRequest;
    using playden::homebrew::ImportRequestKind;
    using playden::homebrew::LiveImportCandidate;
    auto model = make_live();
    check(model.view().subtitle == "Connect a store or import games to fill your library.", "Startup mentions importing");
    model.UpdateVolumes({{"internal", "Console storage", true, 900'000'000'000, 500'000'000'000},
                         {"usb0", "USB drive", true, 900'000'000'000, 500'000'000'000},
                         {"usb1", "USB drive 2", true, 900'000'000'000, 500'000'000'000},
                         {"tiny", "Tiny drive", true, 16'000'000'000, 8'000'000'000}}, "internal");
    LiveImportCandidate rally;
    rally.volume_id = "usb0"; rally.folder = "playden/import/Rally"; rally.path = "/mnt/usb0/playden/import/Rally";
    rally.title = "Rally"; rally.total_bytes = 8'900'000'000; rally.ascii_ok = true; rally.file_count = 5000;
    rally.programs = {{"Rally.exe", 9'000'000, false}};
    LiveImportCandidate cafe = rally;
    cafe.folder = "playden/import/Café"; cafe.path = "/mnt/usb0/playden/import/Café"; cafe.title = "Café"; cafe.ascii_ok = false;
    LiveImportCandidate drop = rally;
    drop.volume_id = "internal"; drop.folder = "playden-import/Drop"; drop.path = "/data/playden-import/Drop"; drop.title = "Drop";
    model.UpdateImportCandidates({rally, cafe, drop}, false);
    activate(model, "startup-import");
    // Look for games pressed twice before the store answers asks once.
    activate(model, "import-look");
    model.action("cross");
    auto requests = model.TakeImportRequests();
    check(requests.size() == 1 && requests[0].kind == ImportRequestKind::LookForGames &&
              find_row(model, "import-look")->title == "Looking for games…",
          "a second Look for games before the store answers is ignored");
    model.UpdateImportCandidates({rally, cafe, drop}, false);

    // A copy-only folder: Play from the drive is there but disabled, saying why.
    activate(model, "import-candidate-usb0:playden/import/Café");
    check(model.view().surface == "import-choice" && !find_row(model, "import-play-in-place")->enabled &&
              find_row(model, "import-play-in-place")->subtitle.find("characters the PS5 can't start a game from") != std::string::npos &&
              model.state().navigation.focus_key == "import-copy" &&
              model.view().subtitle == "Starts with Rally.exe, which you can change in Game settings. "
                                        "You can import it into Playden later from the game's page.",
          "a folder the PS5 can't play from can only be imported");
    activate(model, "import-play-in-place");
    check(model.view().surface == "import-choice" && model.TakeImportRequests().empty(), "its Play from the drive does nothing");
    model.action("circle");

    // A drive folder: play in place, or Import into Playden with its size (no estimate), then the drive.
    activate(model, "import-candidate-usb0:playden/import/Rally");
    check(find_row(model, "import-play-in-place")->title == "Play from the USB drive" &&
              find_row(model, "import-play-in-place")->value.empty() &&
              find_row(model, "import-copy")->value == "8.9 GB" && !has_row(model, "import-move") && !has_row(model, "storage-picker"),
          "a drive folder plays from there or is imported, with its size");
    activate(model, "import-copy");
    check(model.view().surface == "import-storage" && model.view().subtitle == "8.9 GB will be copied there." &&
              find_row(model, "volume-internal")->subtitle == "Default · Available" &&
              find_row(model, "volume-usb1")->subtitle == "Available" &&
              find_row(model, "volume-usb0")->subtitle == "The game is on this drive" && !find_row(model, "volume-usb0")->enabled &&
              find_row(model, "volume-usb0")->tone.empty() &&
              find_row(model, "volume-tiny")->subtitle == "Not enough space" && !find_row(model, "volume-tiny")->enabled &&
              find_row(model, "volume-tiny")->tone == "warn" && model.state().navigation.focus_key == "volume-internal",
          "step 2 lists the drives: the default, the game's own drive and one too small disabled");
    activate(model, "volume-usb0");
    activate(model, "volume-tiny");
    check(model.view().surface == "import-storage" && model.TakeImportRequests().empty(), "a disabled drive does nothing");
    model.action("circle");
    check(model.view().surface == "import-choice", "Back returns to step 1");
    activate(model, "import-copy");
    activate(model, "volume-usb1");
    requests = model.TakeImportRequests();
    check(requests.size() == 1 && requests[0].kind == ImportRequestKind::Add && requests[0].volume_id == "usb0" &&
              requests[0].program == "Rally.exe" && !requests[0].prepare,
          "Import here adds the folder first, prepared once copied");
    check(model.view().surface == "import-games" && find_row(model, "import-candidate-usb0:playden/import/Rally")->value == "Adding…",
          "the sheet says the folder is being added");
    const auto id = playden::GameId::Local("6d1f3a52-0c55-4f3e-9b2a-5d1c7e0a9f11");
    rally.imported = id;
    model.UpdateImportCandidates({rally, cafe, drop}, false);
    requests = model.TakeImportRequests();
    check(requests.size() == 1 && requests[0].kind == ImportRequestKind::CopyIntoPlayden && requests[0].game_id == id &&
              requests[0].volume_id == "usb1",
          "the copy goes to the chosen drive once the game has its ID");
    LibraryUpdate imported{{game(id, "Rally")}, false, {}, false, false, {}};
    imported.source = playden::Store::kLocal;
    check(model.UpdateLibrary(imported), "imported library");
    check(model.state().navigation.route == Route::Details && model.state().navigation.detail_id == id &&
              model.state().navigation.detail_origin == Route::Home && model.state().navigation.modals.empty(),
          "the added game's page opens, even from Startup");

    // Its page while the copy is planned (asked for, no job yet): Copying, disabled, until the job is listed.
    model.UpdateExternalGames({{id, "usb0", true, ""}});
    const auto* starting = find_row(model, "details-download");
    check(starting && starting->title == "Copying" && starting->value == "…" && !starting->enabled &&
              !has_row(model, "details-copy") && !has_row(model, "details-cancel-copy") && !find_row(model, "details-remove")->enabled,
          "a copy asked for shows Copying, disabled, before its job is listed");
    model.UpdateInstalls({{id, playden::homebrew::JobState::Queued, 0, 8'900'000'000, {}, "usb1"}}, {});
    check(find_row(model, "details-download")->enabled && has_row(model, "details-cancel-copy"), "the listed job takes over");
    model.UpdateInstalls({}, {});

    // Its page while it plays from the drive, then while it copies.
    check(find_row(model, "details-copy") && find_row(model, "details-copy")->title == "Import into Playden…" &&
              find_row(model, "details-remove") && !has_row(model, "details-cloud") && !has_row(model, "details-uninstall"),
          "an imported game on a drive offers Copy into Playden and Remove from library");
    // The page's Import into Playden opens step 2 with the folder's size (from the scan, the game has none).
    activate(model, "details-copy");
    check(model.view().surface == "import-storage" && model.view().subtitle == "8.9 GB will be copied there." &&
              !find_row(model, "volume-usb0")->enabled && model.view().eyebrow == "Import games · Rally",
          "the page's Import into Playden opens the drive choice with the folder's size");
    model.action("circle");
    check(model.state().navigation.modals.empty(), "Back closes the page's drive choice");
    // A folder the PS5 can't play from (its name) still offers the copy.
    model.UpdateExternalGames({{id, "usb0", true, "The game's folder name has characters the PS5 can't play from."}});
    check(has_row(model, "details-installed") && has_row(model, "details-copy") && has_row(model, "details-remove"),
          "a copy-only folder says why and offers Import into Playden");
    model.UpdateExternalGames({{id, "usb0", true, ""}});
    model.UpdateInstalls({{id, playden::homebrew::JobState::Downloading, 4'450'000'000, 8'900'000'000, {}, "usb1"}}, {});
    const auto* copying = find_row(model, "details-download");
    check(copying && copying->title == "Copying" && copying->progress > 0.49F && copying->progress < 0.51F &&
              !has_row(model, "details-copy") && !find_row(model, "details-remove")->enabled,
          "the copy's progress replaces Import into Playden");
    // A failed copy can be cancelled from the page; the game plays in place again.
    model.UpdateInstalls({{id, playden::homebrew::JobState::Failed, 4'450'000'000, 8'900'000'000, "Copy failed", "usb1"}}, {});
    check(find_row(model, "details-cancel-copy") && !find_row(model, "details-remove")->enabled, "a failed copy offers Cancel copy");
    activate(model, "details-cancel-copy");
    check(model.view().surface == "confirmation", "Cancel copy asks first");
    activate(model, "confirm-destructive");
    const auto cancels = model.TakeInstallRequests();
    check(cancels.size() == 1 && cancels[0].kind == playden::homebrew::InstallRequestKind::Cancel && cancels[0].game_id == id,
          "Cancel copy cancels the copy job like Queue does");
    model.UpdateInstalls({}, {});
    check(has_row(model, "details-copy") && !has_row(model, "details-cancel-copy") && find_row(model, "details-remove")->enabled,
          "after the cancel the game plays in place again");
    // Copied: Uninstall, not Remove from library.
    model.UpdateInstalls({}, {{id, 8'900'000'000, "usb1", true}});
    model.UpdateExternalGames({});
    check(has_row(model, "details-uninstall") && !has_row(model, "details-remove") && !has_row(model, "details-copy"),
          "a copied game uninstalls");
    playden::homebrew::LiveRuntimeGame titled;
    titled.game_id = id; titled.status = "Ready"; titled.ready = true; titled.can_play = true; titled.has_title = true;
    model.UpdateRuntime(playden::homebrew::RuntimeState::Ready, {titled});
    activate(model, "details-uninstall");
    check(model.state().navigation.modals.back().body.find("adding the game again brings them back") != std::string::npos &&
              model.state().navigation.modals.back().body.find("installing") == std::string::npos,
          "an imported game's uninstall dialog says adding it again brings the saves back");
    activate(model, "uninstall-delete-saves");
    activate(model, "uninstall-delete-saves");
    check(model.state().navigation.modals.back().body.find("adding the game again") != std::string::npos,
          "the saves choice keeps the imported wording");
    model.action("circle");
    model.TakeInstallRequests();

    // A refused copy or add is said to the player.
    model.NotifyImportFailed({ImportRequestKind::CopyIntoPlayden, id, "usb1"}, "Not enough space on USB drive 2.");
    check(!model.state().navigation.modals.empty() && model.state().navigation.modals.back().title == "Can't copy Rally" &&
              model.state().navigation.modals.back().body == "Not enough space on USB drive 2.",
          "a refused copy opens a dialog");
    model.action("cross");

    // Home with only the imported game, signed out: about the library, not the account.
    model.action("circle");
    check(model.state().navigation.route == Route::Home && model.view().subtitle == "Your library", "Home is neutral");

    // The drop folder: play from there, or move.
    activate(model, "home-connect");
    while (!model.state().navigation.modals.empty()) model.action("circle");
    model.action("l1");
    for (int step = 0; step < 4 && model.state().navigation.route != Route::Settings; ++step) model.action("r1");
    model.action("focus:settings-import");
    model.action("cross");
    activate(model, "import-candidate-internal:playden-import/Drop");
    check(find_row(model, "import-play-in-place")->title == "Play from the drop folder" &&
              find_row(model, "import-copy")->subtitle == "Takes the game out of the drop folder. You pick the storage next." &&
              !has_row(model, "storage-picker"),
          "a drop-folder game plays from there or is imported");
    activate(model, "import-copy");
    check(find_row(model, "volume-internal")->subtitle == "Instant · moves the folder" && find_row(model, "volume-internal")->enabled &&
              find_row(model, "volume-usb0")->subtitle == "Copies, then frees the drop folder" &&
              find_row(model, "volume-tiny")->subtitle == "Not enough space",
          "the drop folder's step 2: console storage moves, other drives copy");
    activate(model, "volume-internal");
    requests = model.TakeImportRequests();
    check(requests.size() == 1 && requests[0].kind == ImportRequestKind::Add, "the move adds the folder first");
    model.NotifyImportFailed(requests[0], "The game's folder can't be used.");
    const auto* refused = find_row(model, "import-candidate-internal:playden-import/Drop");
    check(refused && refused->enabled && refused->value != "Adding…" &&
              refused->subtitle == "Not added · The game's folder can't be used.",
          "a refused add shows on its row");
    drop.imported = playden::GameId::Local("7c9e6679-7425-40de-944b-e07fc1f90ae7");
    model.UpdateImportCandidates({rally, cafe, drop}, false);
    check(model.TakeImportRequests().empty(), "a refused add sends nothing more when the folder is added later");

    // The drop folder to another drive: a copy to that drive once the game has its ID.
    drop.imported = {};
    model.UpdateImportCandidates({rally, cafe, drop}, false);
    activate(model, "import-candidate-internal:playden-import/Drop");
    activate(model, "import-copy");
    activate(model, "volume-usb0");
    requests = model.TakeImportRequests();
    check(requests.size() == 1 && requests[0].kind == ImportRequestKind::Add && requests[0].volume_id == "internal",
          "the drop folder's copy adds the folder first");
    drop.imported = playden::GameId::Local("7c9e6679-7425-40de-944b-e07fc1f90ae7");
    model.UpdateImportCandidates({rally, cafe, drop}, false);
    requests = model.TakeImportRequests();
    check(requests.size() == 1 && requests[0].kind == ImportRequestKind::CopyIntoPlayden && requests[0].volume_id == "usb0",
          "then copies it to the chosen drive");
}


void live_download_server_picker() {
    using playden::homebrew::DownloadServerRequest;
    using playden::homebrew::LiveDownloadServers;
    using playden::homebrew::JobState;
    auto model = make_live();
    model.UpdateAccount({AuthState::SignedIn, "account", "Owner", {}, {}, false});
    model.action("l1");
    LiveDownloadServers servers;
    servers.phase = "listed";
    servers.servers = {{"akamai.invalid", "Akamai"}, {"fastly.invalid", "Fastly"}, {"cache.invalid", "Network cache", true}};
    model.ApplyLiveDownloadServers(servers);
    check(find_row(model, "settings-download-server")->value == "Steam default" &&
              find_row(model, "settings-download-server")->subtitle == "Uses the server Steam lists first.",
          "the server row explains Steam default");
    activate(model, "settings-download-server");
    check(model.TakeDownloadServerRequests() == std::vector<DownloadServerRequest>{DownloadServerRequest::List},
          "opening the picker asks the worker for a new list");
    check(find_row(model, "download-server-listing")->title == "Getting the server list…" &&
              model.state().navigation.focus_key == "download-server-auto" &&
              model.view().subtitle == "Applies to downloads that start or resume from now.",
          "listing keeps Steam default available and explains when the choice applies");
    model.ApplyLiveDownloadServers(servers);
    check(model.state().navigation.modals.back().action_keys ==
                  std::vector<std::string>{"download-server-fastest", "download-server-auto", "download-server-0",
                                           "download-server-1", "download-server-2", "download-server-test", "cancel"} &&
              find_row(model, "download-server-fastest")->title == "Fastest" &&
              find_row(model, "download-server-fastest")->subtitle ==
                  "Tests the servers when a download starts and uses the fastest one." &&
              !find_row(model, "download-server-fastest")->selected &&
              find_row(model, "download-server-auto")->title == "Steam default" &&
              find_row(model, "download-server-auto")->selected,
          "Fastest comes first, then Steam default, the listed servers and Test servers");
    check(find_row(model, "download-server-2")->subtitle == "HTTP", "plain HTTP servers are marked");
    activate(model, "download-server-1");
    check(model.state().navigation.modals.empty() && model.state().download_server == "fastly.invalid" &&
              model.ExportPreferences().download_server == "fastly.invalid" && model.PreferencesDirty() &&
              find_row(model, "settings-download-server")->value == "Fastly" &&
              find_row(model, "settings-download-server")->subtitle == "If it stops answering, Playden uses another one.",
          "choosing a server closes the picker and records a preference change");
    auto restarted = make_live();
    playden::PreferencesLoadResult loaded;
    loaded.preferences = model.ExportPreferences();
    check(restarted.ApplyPreferences(restarted.BeginPreferencesLoad(), loaded) &&
              restarted.state().download_server == "fastly.invalid" && !restarted.PreferencesDirty(),
          "loading saved preferences restores the chosen server");
    restarted.UpdateAccount({AuthState::SignedIn, "account", "Owner", {}, {}, false});
    restarted.action("l1");
    restarted.ApplyLiveDownloadServers(servers);
    activate(restarted, "settings-download-server");
    (void)restarted.TakeDownloadServerRequests();
    restarted.ApplyLiveDownloadServers(servers);
    check(restarted.state().navigation.focus_key == "download-server-1", "the picker opens focused on the current choice");
    activate(restarted, "download-server-test");
    check(restarted.TakeDownloadServerRequests() == std::vector<DownloadServerRequest>{DownloadServerRequest::Test},
          "Test servers asks the service to test without closing the picker");
    servers.phase = "testing";
    servers.testing_index = 1;
    restarted.ApplyLiveDownloadServers(servers);
    check(find_row(restarted, "download-server-test")->title == "Testing Fastly (2 of 3)…" &&
              !find_row(restarted, "download-server-test")->enabled, "testing names the current server and progress");
    restarted.action("circle");
    check(!restarted.state().navigation.modals.empty() &&
              restarted.TakeDownloadServerRequests() == std::vector<DownloadServerRequest>{DownloadServerRequest::CancelTest},
          "the first Circle cancels the test and keeps the picker open");
    restarted.ApplyLiveDownloadServers(servers);  // a worker request may still be unwinding
    restarted.action("circle");
    check(restarted.state().navigation.modals.empty() && restarted.TakeDownloadServerRequests().empty(),
          "a second Circle closes while cancellation is pending");
    servers.phase = "listed";
    restarted.ApplyLiveDownloadServers(servers);
    activate(restarted, "settings-download-server");
    (void)restarted.TakeDownloadServerRequests();
    restarted.ApplyLiveDownloadServers(servers);
    activate(restarted, "download-server-test");
    (void)restarted.TakeDownloadServerRequests();
    servers.phase = "tested";
    servers.rates = {{"akamai.invalid", 41.2}, {"fastly.invalid", 3.1}, {"cache.invalid", 0, true}};
    restarted.ApplyLiveDownloadServers(servers);
    check(find_row(restarted, "download-server-0")->value == "41.2 MB/s" &&
              find_row(restarted, "download-server-0")->badge == "Fastest" &&
              find_row(restarted, "download-server-2")->value == "Failed" &&
              restarted.state().navigation.focus_key == "download-server-0" &&
              restarted.state().download_server == "fastly.invalid" && !restarted.PreferencesDirty(),
          "a finished test displays rates and focuses the fastest without selecting or saving it");
    activate(restarted, "download-server-0");
    check(restarted.ExportPreferences().download_server == "akamai.invalid" && restarted.PreferencesDirty(),
          "one Cross chooses the fastest result");
    servers.servers.erase(servers.servers.begin());
    servers.preferred_name = "Akamai";
    restarted.ApplyLiveDownloadServers(servers);
    check(find_row(restarted, "settings-download-server")->value == "Akamai" &&
              find_row(restarted, "settings-download-server")->subtitle ==
                  "Not offered on this network now. Steam default is used.", "a missing stored server falls back visibly");
    activate(restarted, "settings-download-server");
    (void)restarted.TakeDownloadServerRequests();
    servers.phase = "list-failed";
    restarted.ApplyLiveDownloadServers(servers);
    check(find_row(restarted, "download-server-list-failed")->title == "Could not get the server list." &&
              find_row(restarted, "download-server-retry")->title == "Try again", "a failed listing offers retry");
    activate(restarted, "download-server-retry");
    check(restarted.TakeDownloadServerRequests() == std::vector<DownloadServerRequest>{DownloadServerRequest::List},
          "retry lists again on the worker");
    servers.phase = "listed";
    restarted.ApplyLiveDownloadServers(servers);
    restarted.UpdateAccount({AuthState::Offline, "account", "Owner", {}, {}, false});
    check(!find_row(restarted, "download-server-test")->enabled &&
              find_row(restarted, "download-server-test")->subtitle == "Sign in to Steam to test servers." &&
              find_row(restarted, "download-server-0")->enabled, "offline accounts can choose servers but cannot test");
    activate(restarted, "download-server-test");
    check(restarted.TakeDownloadServerRequests().empty(), "a disabled test sends no request");
    restarted.UpdateAccount({AuthState::SignedIn, "account", "Owner", {}, {}, false});
    restarted.UpdateLibrary({{game(SteamId(10), "Downloading game")}, false, {}, false, true, 1234});
    activate(restarted, "settings-download-server");
    (void)restarted.TakeDownloadServerRequests();
    restarted.ApplyLiveDownloadServers(servers);
    restarted.UpdateInstalls({{SteamId(10), JobState::Downloading, 1, 100}}, {});
    check(!find_row(restarted, "download-server-test")->enabled &&
              find_row(restarted, "download-server-test")->subtitle == "Pause downloads to test servers.",
          "an active download disables measurement");
    restarted.UpdateInstalls({{SteamId(10), JobState::Paused, 1, 100}}, {});
    check(find_row(restarted, "download-server-test")->enabled, "pausing the download enables measurement");
    activate(restarted, "download-server-auto");
    check(restarted.state().download_server.empty() && restarted.ExportPreferences().download_server.empty(),
          "Steam default clears the stored choice");
    activate(restarted, "settings-download-server");
    (void)restarted.TakeDownloadServerRequests();
    restarted.ApplyLiveDownloadServers(servers);
    activate(restarted, "download-server-fastest");
    check(restarted.state().navigation.modals.empty() && restarted.ExportPreferences().download_server == "fastest" &&
              find_row(restarted, "settings-download-server")->value == "Fastest" &&
              find_row(restarted, "settings-download-server")->subtitle ==
                  "Tests the servers when a download starts and uses the fastest one.",
          "choosing Fastest stores it and the row says so");
    activate(restarted, "settings-download-server");
    (void)restarted.TakeDownloadServerRequests();
    check(restarted.state().navigation.focus_key == "download-server-fastest", "the picker opens on Fastest while listing");
    restarted.ApplyLiveDownloadServers(servers);
    check(restarted.state().navigation.focus_key == "download-server-fastest" &&
              find_row(restarted, "download-server-fastest")->selected && !find_row(restarted, "download-server-auto")->selected,
          "the picker marks and focuses Fastest");
    restarted.UpdateInstalls({{SteamId(10), JobState::Paused, 1, 100}}, {});
    activate(restarted, "download-server-test");
    (void)restarted.TakeDownloadServerRequests();
    servers.phase = "tested";
    servers.rates = {{"fastly.invalid", 3.1}, {"cache.invalid", 9.5}};
    restarted.ApplyLiveDownloadServers(servers);
    check(restarted.state().navigation.focus_key == "download-server-1" && restarted.state().download_server == "fastest",
          "after a test the focus goes to the fastest tested server, not to Fastest");
    // A version 8 file written with Fastest loads as Fastest.
    auto reloaded = make_live();
    playden::PreferencesLoadResult fastest;
    fastest.preferences.download_server = "fastest";
    check(reloaded.ApplyPreferences(reloaded.BeginPreferencesLoad(), fastest) &&
              reloaded.state().download_server == "fastest", "a stored Fastest loads");
    reloaded.UpdateAccount({AuthState::SignedIn, "account", "Owner", {}, {}, false});
    reloaded.action("l1");
    servers.phase = "listed";
    reloaded.ApplyLiveDownloadServers(servers);
    check(find_row(reloaded, "settings-download-server")->value == "Fastest", "a loaded Fastest shows in Settings");
}

void leftover_titles_require_confirmation_and_follow_the_queue() {
    auto model = make_live();
    model.UpdateAccount({AuthState::SignedIn, "account", "Owner", {}, {}, false});
    model.action("l1");
    check(!has_row(model, "settings-leftovers"), "no leftover row without titles");
    model.UpdateLeftoverTitles({{"FAKE78001", "Lost game (Steam)", 12'000'000}, {"FAKE78002", "", 3'000'000}});
    const auto* row = find_row(model, "settings-leftovers");
    check(row && row->title == "Leftover PS5 titles" && row->value == "2 titles · 15 MB" && row->section == "Storage" &&
              !has_row(model, "settings-leftover-FAKE78001"),
          "Storage has one Leftover PS5 titles row with their count and size, like Kept game data");
    activate(model, "settings-leftovers");
    check(model.state().navigation.modals.back().id == "leftover-titles" && model.view().surface == "leftover-titles",
          "the row opens the leftover titles manager");
    const auto* named = find_row(model, "leftover-toggle-FAKE78001");
    check(named && named->title == "Lost game (Steam)" && named->subtitle == "FAKE78001" && named->value == "12 MB" &&
              named->kind == RowKind::Toggle && !named->selected,
          "each title is a checkbox with its ID and size");
    check(find_row(model, "leftover-toggle-FAKE78002")->title == "FAKE78002", "an unnamed title uses its ID");
    check(!find_row(model, "leftover-selection-delete")->enabled, "nothing to delete before choosing");
    activate(model, "leftover-toggle-FAKE78001");
    check(find_row(model, "leftover-selection-delete")->value == "1 title · 12 MB", "Delete selected counts the choice");
    activate(model, "leftover-selection-delete");
    check(model.TakeLeftoverDeleteRequests().empty(), "opening the confirmation deletes nothing");
    check(model.state().navigation.focus_key == "cancel" &&
              model.state().navigation.modals.back().body.find("Lost game (Steam) (FAKE78001)") != std::string::npos &&
              model.state().navigation.modals.back().body.find("move to Kept game data") != std::string::npos,
          "confirmation starts on Cancel and describes the deletion scope");
    activate(model, "cancel");
    check(model.TakeLeftoverDeleteRequests().empty() && model.state().navigation.modals.back().id == "leftover-titles",
          "Cancel deletes nothing and returns to the manager");
    activate(model, "leftover-select-all");
    activate(model, "leftover-selection-delete");
    check(model.state().navigation.modals.back().body.find("2 leftover PS5 titles") != std::string::npos,
          "several titles are named by count");
    activate(model, "confirm-destructive");
    check(model.TakeLeftoverDeleteRequests() == std::vector<std::string>{"FAKE78001", "FAKE78002"},
          "every confirmed title is requested, in list order");
    check(model.TakeLeftoverDeleteRequests().empty(), "requests are consumed once");
    check(model.state().navigation.modals.empty() && model.state().navigation.focus_key == "settings-leftovers",
          "confirming closes the manager and returns to the Storage row");
    model.UpdateLeftoverDeletions({{"FAKE78001", "Lost game (Steam)", "Removing PS5 title…", ""},
                                  {"FAKE78002", "", "", ""}});
    const auto layout = playden::homebrew::BuildQueueLayout(model.state());
    check(layout.preparation.active && layout.preparation.active->key == "prep-leftover-FAKE78001" &&
              layout.preparation.active->note == "Removing PS5 title…" && layout.preparation.waiting == 1 &&
              playden::homebrew::QueuePending(model.state()) == 2,
          "Queue keeps active and waiting titles distinct with the actual step");
    activate(model, "settings-leftovers");
    const auto* queued = find_row(model, "leftover-toggle-FAKE78002");
    check(queued && !queued->enabled && queued->subtitle == "FAKE78002 · Queued" &&
              find_row(model, "leftover-toggle-FAKE78001")->subtitle == "FAKE78001 · Removing PS5 title…",
          "a queued title shows its step and cannot be chosen again");
    check(!find_row(model, "leftover-select-all")->enabled, "Select all has nothing to choose while both are queued");
    model.UpdateLeftoverDeletions({{"FAKE78001", "Lost game (Steam)", "", "The PS5 refused removal."}});
    const auto stopped = playden::homebrew::BuildQueueLayout(model.state());
    check(stopped.preparation.attention == 1 && stopped.preparation.rows.front().note == "The PS5 refused removal.",
          "a failed leftover deletion remains visible in Queue");
    const auto* failed = find_row(model, "leftover-toggle-FAKE78001");
    check(failed && failed->enabled && failed->subtitle == "FAKE78001 · The PS5 refused removal.",
          "a failed title shows why and can be chosen again");
    activate(model, "leftover-toggle-FAKE78002");
    activate(model, "leftover-selection-delete");
    model.UpdateLeftoverTitles({{"FAKE78001", "Lost game (Steam)", 12'000'000}});
    activate(model, "confirm-destructive");
    check(model.TakeLeftoverDeleteRequests().empty(), "a title that disappeared while confirming is not requested");
    model.UpdateLeftoverDeletions({});
    activate(model, "settings-leftovers");
    activate(model, "leftover-toggle-FAKE78001");
    activate(model, "leftover-selection-delete");
    model.UpdateLeftoverTitles({});
    check(model.state().navigation.modals.empty() && !has_row(model, "settings-leftovers"),
          "the manager and its confirmation close and the row disappears when no leftovers remain");
    check(model.TakeLeftoverDeleteRequests().empty(), "closing the confirmation requested nothing");
}

// An installed game with an update or new DLC: the details offer it after Play, the sheet confirms it, the
// queue follows the job, and Play waits while the update puts its files in place.
void live_game_update() {
    using playden::homebrew::GameUpdate;
    using playden::homebrew::InstallRequestKind;
    using playden::homebrew::JobState;
    using playden::homebrew::LiveInstallJob;
    auto model = make_live();
    model.UpdateAccount({AuthState::SignedIn, "123", "Synthetic", {}, {}, true});
    check(model.UpdateLibrary({{game(SteamId(42), "Updated")}, false, {}, false, false, {}}), "library loaded");
    model.UpdateVolumes({{"internal", "Console storage", true, 100'000'000'000, 50'000'000'000}}, "internal");
    model.UpdateInstalls({}, {{SteamId(42), 6'000'000'000, "internal"}});
    playden::homebrew::LiveRuntimeGame ready{SteamId(42), "Ready", "FAKE78001", false, true};
    ready.can_play = true;
    ready.activity = playden::homebrew::TitleActivity::Stopped;
    model.UpdateRuntime(playden::homebrew::RuntimeState::Ready, {ready});
    model.UpdateAppDetails(SteamId(42), 100, 6'000'000'000, true);
    check(model.action("details:42"), "details open");
    check(!has_row(model, "details-update") && !has_row(model, "details-update-note"), "nothing to update, no button");

    model.UpdateAppDetails(SteamId(42), 100, 6'000'000'000, true, {}, 0, {}, GameUpdate{true, false, 1'200'000'000, 1'400'000'000, 1, true});
    const auto& rows = model.view().rows;
    const auto play_at = std::find_if(rows.begin(), rows.end(), [](const auto& row) { return row.key == "details-play"; });
    const auto update_at = std::find_if(rows.begin(), rows.end(), [](const auto& row) { return row.key == "details-update"; });
    check(play_at != rows.end() && update_at == play_at + 1 && update_at->title == "Update…" && update_at->enabled,
          "Update follows Play");
    const auto* note = find_row(model, "details-update-note");
    check(note && note->title == "Update available" && note->value == "1.2 GB", "the note says the update's size");
    activate(model, "details-update");
    check(model.view().surface == "update" && model.view().title == "Update game", "Update opens its sheet");
    const auto* confirm = find_row(model, "update-confirm");
    check(confirm && confirm->title == "Download and install" && confirm->value == "1.2 GB" && confirm->enabled &&
              confirm->subtitle == "Starts the download",
          "the sheet's confirm row has the size, as the install sheet");
    check(model.view().subtitle.find("On Console storage. ") != std::string::npos &&
              model.view().subtitle.find("Downloads 1.2 GB") != std::string::npos,
          "the sheet says where and how much");
    activate(model, "update-confirm");
    auto requests = model.TakeInstallRequests();
    check(requests.size() == 1 && requests[0].kind == InstallRequestKind::Update && requests[0].game_id == SteamId(42),
          "confirming sends one update request");
    check(model.view().surface != "update", "the sheet closes");

    // DLC bought since wins the label, even when an update downloads with it.
    model.UpdateAppDetails(SteamId(42), 100, 6'000'000'000, true, {}, 0, {}, GameUpdate{true, true, 300'000'000, 300'000'000, 2, true});
    check(find_row(model, "details-update")->title == "DLC available…" &&
              find_row(model, "details-update-note")->title == "DLC available" &&
              find_row(model, "details-update-note")->value == "300 MB",
          "new DLC says DLC available");

    // Less room than the changed files need: the sheet shows the size and lets it start; the download checks the
    // room itself.
    model.UpdateVolumes({{"internal", "Console storage", true, 100'000'000'000, 200'000'000}}, "internal");
    activate(model, "details-update");
    confirm = find_row(model, "update-confirm");
    check(confirm && confirm->enabled && confirm->subtitle == "Starts the download", "the room doesn't block the update");
    check(model.view().subtitle.find("Downloads 300 MB") != std::string::npos, "the sheet says the size");
    activate(model, "update-confirm");
    requests = model.TakeInstallRequests();
    check(requests.size() == 1 && requests[0].kind == InstallRequestKind::Update, "the update is sent");
    model.UpdateVolumes({{"internal", "Console storage", true, 100'000'000'000, 50'000'000'000}}, "internal");
    // A disconnected drive still blocks it.
    model.UpdateVolumes({{"internal", "Console storage", false, 100'000'000'000, 50'000'000'000}}, "internal");
    check(model.action("details:42"), "details reopen");
    activate(model, "details-update");
    confirm = find_row(model, "update-confirm");
    check(confirm && !confirm->enabled && confirm->subtitle == "Drive disconnected", "a missing drive blocks the update");
    activate(model, "cancel");
    model.UpdateVolumes({{"internal", "Console storage", true, 100'000'000'000, 50'000'000'000}}, "internal");

    // The update downloads: the button goes, the note follows the job, Play stays.
    LiveInstallJob job;
    job.game_id = SteamId(42);
    job.state = JobState::Downloading;
    job.done_bytes = 150'000'000;
    job.total_bytes = 300'000'000;
    job.volume_id = "internal";
    job.update = true;
    model.UpdateInstalls({job}, {{SteamId(42), 6'000'000'000, "internal"}});
    check(model.action("details:42"), "details reopen");
    check(!has_row(model, "details-update"), "no Update button while the update runs");
    const auto* updating = find_row(model, "details-updating");
    check(updating && updating->title == "Updating" && updating->value == "50%", "the note follows the update");
    check(find_row(model, "details-play")->enabled, "the game plays while its update downloads");
    const auto queued = std::find_if(model.state().jobs.begin(), model.state().jobs.end(),
                                     [](const auto& value) { return value.game_id == SteamId(42); });
    check(queued != model.state().jobs.end() && queued->stage_text == "Updating" && queued->state_text.empty(),
          "the queue says Updating with the job's own state");

    // Putting the files in place: Play waits and says why; Pause and Cancel are not offered.
    job.applying = true;
    job.locked = true;
    model.UpdateInstalls({job}, {{SteamId(42), 6'000'000'000, "internal"}});
    check(model.action("details:42"), "details reopen");
    const auto* play = find_row(model, "details-play");
    check(play && !play->enabled && play->action.empty() && play->value == "Finishing the update…",
          "Play is disabled while the update puts its files in place");
    model.action("focus:details-play");
    model.action("cross");
    check(model.TakeRuntimePlayRequests().empty(), "a locked game is not started");
    check(find_row(model, "details-updating")->value == "Applying", "the note names the step");
    const auto locked = std::find_if(model.state().jobs.begin(), model.state().jobs.end(),
                                     [](const auto& value) { return value.game_id == SteamId(42); });
    check(locked->state_text == "Applying" && locked->locked, "the queue names the step");

    // Finished: "Update complete", and what Steam said is asked again before the button comes back.
    model.UpdateInstalls({}, {{SteamId(42), 6'100'000'000, "internal"}});
    const auto* done = find_notice(model, "install-42");
    check(done && done->message == "Update complete", "a finished update says so");
    check(model.action("details:42") && !has_row(model, "details-update") && find_row(model, "details-play")->enabled,
          "after the update, no button and Play again");
}

int main() {
    live_game_update();
    live_install_language_row();
    live_install_version_row();
    live_install_language_size();
    live_install_exact_size();
    live_install_version_title();
    live_install_dlc_row();
    leftover_titles_require_confirmation_and_follow_the_queue();
    live_download_server_picker();
    live_import_sheet_sends_requests();
    live_import_choice_and_details();
    store_art_follows_the_store();
    non_canonical_game_keys_select_nothing();
    live_stay_signed_in_preference_loads();
    live_launch_failure_remains_visible_without_a_details_status_line();
    live_launch_failure_before_the_library_names_the_game();
    live_download_reorder_reaches_service();
    live_play_does_the_next_useful_thing();
    live_queued_setup_names_the_game_ahead();
    live_store_details_fill_the_description();
    live_ready_to_play_quits_to_home();
    live_ready_prompt_matches_the_open_game();
    live_details_focus_moves_to_play_once_enabled();
    live_settings_focus_stays_near_a_removed_row();
    live_sign_in_on_full_storage_offers_uninstall();
    live_storage_section_and_installed_games();
    preparation_steps_read_as_player_words();
    live_install_requests_and_projection();
    live_install_volumes();
    live_uninstall_removes_title_and_keeps_saves();
    live_install_confirmation_checks_space_and_unknowns();
    live_install_notifications();
    live_completed_downloads_follow_the_queue();
    live_move_job_actions_say_move();
    live_queue_game_page_returns_to_its_row();
    live_download_card_keeps_queue_order();
    game_collections_sheet_adds_and_removes_the_game();
    live_log_viewer();
    live_cloud_saves();
    live_saves_tab();
    live_saves_upload_all();
    live_store_without_cloud_never_asks_the_cloud();
    live_filters_only_use_available_facts();
    live_demos_filter_defaults_on();
    live_family_shared_projected();
    live_family_filter_defaults_on();
    live_sort_sheet_refreshes_the_library();
    continue_playing_more_keeps_the_chosen_sort();
    live_empty_filter_keeps_the_signed_in_account();
    projects_only_owned_facts_and_delivery_status();
    loading_and_failure_preserve_last_good_snapshot();
    rejects_invalid_snapshots_transactionally_and_leaves_simulation_alone();
    preserves_identity_and_scroll_then_chooses_a_stable_neighbor();
    live_details_keep_personal_edits_and_block_fixture_operations();
    refresh_actions_preserve_authentication_and_wall_clock_is_external();
    live_queue_shows_downloads_and_preparations();
    live_queue_counts_copies();
    live_queue_history_and_empty_states();
    live_queue_lists_preparations_needing_attention();
    live_refused_preparation_offers_uninstall();
    live_setup_from_an_earlier_kit_offers_recreate();
    live_queue_lists_uninstalls();
    live_details_prepare_next();
    std::cout << "homebrew_library_model_test passed\n";
    return 0;
}
