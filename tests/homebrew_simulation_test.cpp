// homebrew_simulation_test.cpp - Deterministic Playden UI simulation acceptance tests.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: MIT

#include "homebrew/simulation.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>
#include <unistd.h>
#include "steam_id.h"

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

std::filesystem::path temp_profile() {
    auto path = std::filesystem::canonical(std::filesystem::temp_directory_path()) / ("playden-homebrew-simulation-test-" + std::to_string(getpid()) + ".json");
    std::error_code error;
    std::filesystem::remove(path, error);
    return path;
}

void t01_authentication_ignores_late_approval(const std::filesystem::path& fixtures,
                                               const std::filesystem::path& profile) {
    playden::homebrew::Simulation simulation(fixtures, profile);
    simulation.scenario("startup-loading");
    check(simulation.view().rows.front().key == "startup-loading", "T01 startup exposes deterministic loading");
    simulation.tick(1);
    check(simulation.view().rows.front().key == "startup-connect", "T01 advancing the clock completes startup loading");
    simulation.scenario("signed-out");
    check(simulation.state().navigation.route == playden::homebrew::Route::Startup,
          "T01 signed-out scenario starts at first run");
    simulation.action("cross");
    check(simulation.state().account().authentication == playden::homebrew::AuthState::Waiting,
          "T01 Connect Steam opens a waiting challenge");
    playden::homebrew::Simulation waiting_restart(fixtures, profile);
    check(waiting_restart.state().account().authentication == playden::homebrew::AuthState::SignedOut
              && waiting_restart.state().navigation.route == playden::homebrew::Route::Startup
              && waiting_restart.state().account().id.empty() && waiting_restart.state().account().display_name.empty()
              && !waiting_restart.state().account().cache_available,
          "T01 restarting an anonymous waiting challenge invalidates it without leaking catalog identity");
    simulation.scenario("qr-expired");
    check(simulation.state().account().authentication == playden::homebrew::AuthState::Expired,
          "T01 injected QR expiry is visible");
    simulation.action("cross");
    simulation.scenario("qr-approved");
    simulation.tick(1);
    check(simulation.state().navigation.route == playden::homebrew::Route::Home,
          "T01 approved refreshed challenge reaches Home");
    simulation.scenario("signed-out");
    simulation.action("cross");
    simulation.action("circle");
    simulation.scenario("late-approval");
    simulation.tick(5);
    check(simulation.state().account().authentication == playden::homebrew::AuthState::Cancelled,
          "T01 late approval cannot revive a cancelled challenge");
    simulation.scenario("signed-out"); simulation.action("focus:startup-skip"); simulation.action("cross");
    check(std::any_of(simulation.view().rows.begin(), simulation.view().rows.end(),
                      [](const playden::homebrew::ViewRow& row) { return row.key == "home-connect"; }),
          "T01 empty signed-out Home offers both Library and Connect");
    simulation.scenario("signed-out"); simulation.action("cross"); simulation.action("inject:qr-approved");
    playden::homebrew::Simulation syncing_restart(fixtures, profile);
    check(syncing_restart.state().account().authentication == playden::homebrew::AuthState::Failed
              && syncing_restart.state().navigation.route == playden::homebrew::Route::SignIn
              && syncing_restart.state().navigation.focus_key == "qr-retry",
          "T01 interrupted sync restarts at a controller-recoverable retry state");
    syncing_restart.action("cross");
    check(syncing_restart.state().account().authentication == playden::homebrew::AuthState::Waiting,
          "T01 retry creates a fresh challenge after interrupted sync");
}

void t02_focus_and_nested_modal_restoration(const std::filesystem::path& fixtures,
                                             const std::filesystem::path& profile) {
    playden::homebrew::Simulation simulation(fixtures, profile);
    simulation.scenario("default");
    simulation.action("r1");
    simulation.action("down");
    simulation.action("down");
    const auto selected = simulation.state().navigation.selected_game_id;
    const auto scroll = simulation.state().navigation.scroll_offsets.at("library-grid");
    simulation.action("cross");
    simulation.action("options");
    check(simulation.state().navigation.modals.size() == 1, "T02 More opens one modal");
    simulation.action("circle");
    check(simulation.state().navigation.focus_key == "details-more", "T02 closing More restores its opener");
    simulation.action("circle");
    check(simulation.state().navigation.selected_game_id == selected,
          "T02 returning to Library restores selected game identity");
    check(simulation.state().navigation.scroll_offsets.at("library-grid") == scroll,
          "T02 returning to Library restores exact viewport");
}

void public_contract_and_snapshot(const std::filesystem::path& fixtures,
                                  const std::filesystem::path& profile) {
    playden::homebrew::Simulation simulation(fixtures, profile);
    simulation.scenario("default");
    check(simulation.state().games.size() == 24, "curated fixture exposes all 24 games");
    check(!simulation.view().rows.empty(), "active surface exposes reusable rows");
    check(simulation.snapshot().find("\"simulation\":true") != std::string::npos,
          "snapshot marks the state as a UI simulation");
}

void activate(playden::homebrew::Simulation& simulation, const std::string& key) {
    check(simulation.action("focus:" + key), "test focus key exists");
    check(simulation.action("cross"), "focused action activates");
}

void t03_circle_leaves_an_applied_search(const std::filesystem::path& fixtures,
                                         const std::filesystem::path& profile) {
    playden::homebrew::Simulation simulation(fixtures, profile);
    simulation.action("route:library");
    const auto has_legend = [&](const std::string& legend) {
        const auto& legends = simulation.view().legends;
        return std::find(legends.begin(), legends.end(), legend) != legends.end();
    };
    check(has_legend("Touchpad Search") && !has_legend("Search") && !has_legend("Circle Clear search"),
          "T03 Library names the touchpad for Search");
    simulation.action("search");
    check(has_legend("Options Done"), "T03 the keyboard names Options for Done");
    simulation.text("Night Orchard");
    simulation.action("options");
    check(!simulation.state().editor.active && simulation.state().library.query == "Night Orchard",
          "T03 Options commits the keyboard like Done");
    check(simulation.visible_games().size() == 1 && has_legend("Circle Clear search"),
          "T03 an applied search offers Circle to clear it");
    simulation.action("circle");
    check(simulation.state().library.query.empty() && simulation.visible_games().size() > 1
              && simulation.state().navigation.route == playden::homebrew::Route::Library,
          "T03 Circle clears the search and stays in the Library");
    simulation.action("circle");
    check(simulation.state().navigation.route == playden::homebrew::Route::Home,
          "T03 Circle without a search goes back Home");
}

void t03_search_filters_and_focus_reconciliation(const std::filesystem::path& fixtures,
                                                 const std::filesystem::path& profile) {
    playden::homebrew::Simulation simulation(fixtures, profile);
    simulation.action("route:library");
    simulation.action("search");
    simulation.text("temporary");
    check(simulation.visible_games().empty() && simulation.view().subtitle.find("0 results") != std::string::npos,
          "T03 search result count updates live before commit");
    simulation.action("circle");
    check(simulation.state().library.query.empty(), "T03 cancelling search restores the original query");
    simulation.action("search");
    simulation.text("Night Orchard");
    simulation.action("done");
    check(simulation.visible_games().size() == 1 && simulation.visible_games().front()->id == SteamId(900010),
          "T03 committed search updates results and count");
    check(simulation.state().navigation.selected_game_id == SteamId(900010) && simulation.state().navigation.focus_key == "game-900010",
          "T03 committing search reconciles logical selection to the visible result");
    const bool prior_favorite = simulation.state().library.favorites.contains(SteamId(900001));
    simulation.action("square");
    check(simulation.state().library.favorites.contains(SteamId(900010)) && simulation.state().library.favorites.contains(SteamId(900001)) == prior_favorite,
          "T03 Square after controller search favorites the focused result");
    simulation.action("square");
    activate(simulation, "game-900010");
    simulation.action("circle");
    simulation.action("options");
    activate(simulation, "hide");
    check(simulation.visible_games().empty() && simulation.state().navigation.focus_key == "clear-search",
          "T03 removing the focused result reconciles focus to Clear search");
    activate(simulation, "clear-search");
    check(!simulation.visible_games().empty(), "T03 Clear search recovers results");
    simulation.action("triangle");
    activate(simulation, "sort-playtime");
    simulation.action("triangle");
    activate(simulation, "filter-controller-full");
    simulation.action("triangle");
    const auto filter_row = [&simulation](const std::string& key) {
        return *std::find_if(simulation.view().rows.begin(), simulation.view().rows.end(),
                             [&key](const auto& row) { return row.key == key; });
    };
    check(filter_row("filter-controller-full").selected && filter_row("filter-controller-full").value == "On" &&
              filter_row("filter-installed").value == "Off" && filter_row("sort-playtime").checked,
          "T03 a filter shows On when it applies, and the sort in use has its tick");
    activate(simulation, "filters-reset");
    check(simulation.state().library.filters.empty() && simulation.state().library.sort == "name",
          "T03 reset clears every filter and restores name sort");
    simulation.action("select:900012");
    const auto selected = simulation.state().navigation.selected_game_id;
    const auto scroll = simulation.state().navigation.scroll_offsets.at("library-grid");
    simulation.action("inject:library-refresh");
    check(simulation.state().library_loading && simulation.view().subtitle.find("Refreshing") != std::string::npos,
          "T03 refresh has a visible loading state before completion");
    simulation.tick(1);
    check(!simulation.state().library_loading && simulation.state().navigation.selected_game_id == selected
              && simulation.state().navigation.scroll_offsets.at("library-grid") == scroll,
          "T03 refresh preserves exact library selection and viewport");
}

void t04_organization_persists_without_deleting_games(const std::filesystem::path& fixtures,
                                                       const std::filesystem::path& profile) {
    playden::homebrew::Simulation simulation(fixtures, profile);
    simulation.action("route:library");
    simulation.action("select:900012");
    simulation.action("square");
    check(simulation.state().library.favorites.contains(SteamId(900012)), "T04 favorite toggles by stable ID");
    check(simulation.action("collection:create:Night games"), "T04 collection create action is available");
    check(simulation.action("collection:add:collection-2:900012"), "T04 collection membership changes");
    check(simulation.action("collection:rename:collection-2:After dark"), "T04 collection rename works");
    check(simulation.action("collection:pin:collection-2"), "T04 collection pin works");
    check(simulation.action("rate:900012:playable"), "T04 local compatibility rating works");
    check(simulation.action("note:900012:Synthetic note"), "T04 note edit works");
    playden::homebrew::Simulation organized(fixtures, profile);
    check(organized.state().library.collections.size() == 2 && organized.state().library.collections.back().pinned
              && organized.state().library.collections.back().members.contains(SteamId(900012)),
          "T04 collection name, pin, and membership survive restart");
    check(organized.state().library.ratings.at(SteamId(900012)) == playden::homebrew::Compatibility::Playable
              && organized.state().library.notes.at(SteamId(900012)) == "Synthetic note",
          "T04 rating and note survive restart");
    check(simulation.action("collection:delete:collection-2"), "T04 collection delete works");
    check(simulation.state().games.size() == 24, "T04 deleting a collection never deletes a game");
    playden::homebrew::Simulation restored(fixtures, profile);
    check(restored.state().library.favorites.contains(SteamId(900012)), "T04 favorites survive restart");
}

void t05_install_rechecks_storage_and_submits_once(const std::filesystem::path& fixtures,
                                                   const std::filesystem::path& profile) {
    playden::homebrew::Simulation simulation(fixtures, profile);
    simulation.action("details:900012");
    activate(simulation, "details-install");
    activate(simulation, "storage-picker");
    activate(simulation, "volume-external");
    simulation.action("inject:storage-disconnect");
    activate(simulation, "install-confirm");
    check(simulation.state().jobs.size() == 3, "T05 disconnect before confirmation blocks install");
    simulation.action("inject:storage-reconnect");
    activate(simulation, "install-confirm");
    check(simulation.state().jobs.size() == 4, "T05 successful confirmation creates one job");
    check(simulation.action("install:900012:external"), "T05 duplicate install submission is accepted idempotently");
    check(simulation.state().jobs.size() == 4, "T05 duplicate confirmation cannot create a second job");
}

void t06_queue_transitions_are_single_active_and_safe(const std::filesystem::path& fixtures,
                                                      const std::filesystem::path& profile) {
    playden::homebrew::Simulation simulation(fixtures, profile);
    simulation.action("route:downloads");
    activate(simulation, "job-job-1");
    activate(simulation, "job-pause-job-1");
    simulation.tick(1);
    activate(simulation, "job-job-1");
    activate(simulation, "job-resume-job-1");
    int active = 0;
    for (const auto& job : simulation.state().jobs)
        if (job.state == playden::homebrew::JobState::Downloading || job.state == playden::homebrew::JobState::Verifying) ++active;
    check(active == 1, "T06 resume preserves the one-active-transfer invariant");
    check(simulation.action("job:move-up:job-3"), "T06 queued jobs can move up");
    check(simulation.action("job:cancel:job-2"), "T06 cancel opens safe confirmation");
    check(simulation.state().navigation.modals.back().focused_action == 0, "T06 destructive cancel defaults to decline");
    simulation.action("circle");
    check(simulation.state().jobs[1].state != playden::homebrew::JobState::Cancelled, "T06 declining cancel leaves the job intact");
    simulation.action("job:fail:job-1:download");
    check(simulation.action("job:retry:job-1"), "T06 failure retry is reachable");
    check(simulation.state().jobs.front().state == playden::homebrew::JobState::Downloading,
          "T06 retry resumes the failed stage");
    simulation.action("details:900001"); simulation.action("options"); activate(simulation, "verify");
    simulation.action("details:900001"); simulation.action("options"); activate(simulation, "verify");
    const auto verifies = std::count_if(simulation.state().jobs.begin(), simulation.state().jobs.end(),
        [](const playden::homebrew::Job& job) { return job.kind == "verify" && job.game_id == SteamId(900001); });
    check(verifies == 1, "T06 repeated Verify creates one queued operation");
    active = 0;
    for (const auto& job : simulation.state().jobs)
        if (job.state == playden::homebrew::JobState::Downloading || job.state == playden::homebrew::JobState::Verifying) ++active;
    check(active == 1, "T06 queued verification preserves one active transfer");
}

void t07_interruption_and_verification_recovery(const std::filesystem::path& fixtures,
                                                const std::filesystem::path& profile) {
    playden::homebrew::Simulation simulation(fixtures, profile);
    simulation.action("inject:storage-disconnect");
    playden::homebrew::Simulation restored(fixtures, profile);
    check(restored.state().jobs.front().state == playden::homebrew::JobState::Interrupted,
          "T07 interrupted work survives restart without invented completion");
    restored.action("inject:storage-reconnect");
    check(restored.action("job:retry:job-1"), "T07 interrupted download can recover");
    restored.action("inject:verification-failed");
    check(restored.state().jobs.front().retry_stage == playden::homebrew::JobStage::Verify,
          "T07 verification failure preserves its retry stage");
    restored.scenario("runtime-unavailable");
    check(!restored.state().installations.front().runtime_ready,
          "T07 downloaded/installed remains distinct from runtime readiness");
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream file(path);
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

// Bundled presets and the HL2 recipe, re-targeted at fixture game 900001.
void load_settings_data(playden::homebrew::Simulation& simulation, const std::filesystem::path& fixtures) {
    namespace rt = playden::runtime;
    const auto source = fixtures.parent_path().parent_path();
    std::vector<rt::SettingsPreset> presets;
    rt::GameRecipe recipe;
    std::string reason;
    check(rt::ParseSettingsPresets(read_text(source / "runtime/presets.json"), &presets, &reason), "presets load");
    check(rt::ParseGameRecipe(read_text(source / "runtime/recipes/steam-220-hl2-v1.json"), &recipe, &reason), "recipe loads");
    recipe.app_id = 900001;
    recipe.launch_entries.push_back({"hl2-lostcoast", "Lost Coast", "hl2.exe", ""});
    simulation.SetSettingsData(std::move(presets), {std::move(recipe)});
}

const playden::homebrew::ViewRow* find_row(const playden::homebrew::Simulation& simulation, const std::string& key) {
    for (const auto& row : simulation.view().rows)
        if (row.key == key) return &row;
    return nullptr;
}

void t08_nested_settings_restore_openers(const std::filesystem::path& fixtures,
                                         const std::filesystem::path& profile) {
    playden::homebrew::Simulation simulation(fixtures, profile);
    load_settings_data(simulation, fixtures);
    simulation.action("details:900001");
    activate(simulation, "details-more");
    activate(simulation, "game-settings");
    check(find_row(simulation, "setting-resolution") && find_row(simulation, "setting-resolution")->value == "1920×1080",
          "T08 sheet shows the recipe resolution");
    check(find_row(simulation, "setting-frame_limit") && !find_row(simulation, "setting-frame_limit")->enabled,
          "T08 dxvk.conf rows are listed but disabled until the kit reads dxvk.conf");
    activate(simulation, "setting-resolution");
    activate(simulation, "choice-2");
    check(simulation.state().navigation.focus_key == "setting-resolution", "T08 picker restores its opener");
    check(simulation.state().game_settings.at(SteamId(900001)).overrides.at("resolution") == "2560x1440", "T08 picker saves the override");
    check(find_row(simulation, "setting-resolution")->modified, "T08 changed row is marked");
    check(find_row(simulation, "preset-picker")->value == "Custom · from Playden default", "T08 preset reads Custom after a change");
    activate(simulation, "setting-extra_arguments");
    simulation.text("--fixture");
    simulation.action("circle");
    check(!simulation.state().game_settings.at(SteamId(900001)).overrides.count("extra_arguments"), "T08 cancelling text edit discards changes");
    activate(simulation, "setting-extra_arguments");
    simulation.text("--fixture");
    simulation.action("done");
    check(simulation.state().game_settings.at(SteamId(900001)).overrides.at("extra_arguments") == "--fixture",
          "T08 committing text edit saves the override");
    activate(simulation, "setting-extra_arguments");
    simulation.text(" \"unclosed");
    simulation.action("done");
    check(simulation.state().editor.active && simulation.state().game_settings.at(SteamId(900001)).overrides.at("extra_arguments") == "--fixture",
          "T08 invalid arguments keep the editor open and the old value");
    simulation.action("circle");
    // Choosing the inherited value removes the override instead of storing a copy.
    activate(simulation, "setting-controller");
    activate(simulation, "choice-0");
    check(!simulation.state().game_settings.at(SteamId(900001)).overrides.count("controller"), "T08 inherited choice is not an override");
    // Triangle resets the focused row to the preset value.
    check(simulation.action("focus:setting-resolution") && simulation.action("triangle"), "T08 triangle resets");
    check(!simulation.state().game_settings.at(SteamId(900001)).overrides.count("resolution"), "T08 reset removes the override");
    simulation.action("circle");
    simulation.action("circle");  // Game settings returned to Game actions
    simulation.action("details:900002");
    activate(simulation, "details-more");
    activate(simulation, "game-settings");
    activate(simulation, "setting-extra_arguments");
    check(simulation.state().editor.value.empty(), "T08 custom arguments do not leak to another game");
    simulation.action("circle");
    simulation.action("circle");
    playden::homebrew::Simulation restored(fixtures, profile);
    check(restored.state().game_settings.at(SteamId(900001)).overrides.at("extra_arguments") == "--fixture"
              && !restored.state().game_settings.count(SteamId(900002)),
          "T08 per-game overrides persist independently");
}

void t08b_settings_sheet_rules(const std::filesystem::path& fixtures, const std::filesystem::path& profile) {
    namespace rt = playden::runtime;
    playden::homebrew::Simulation simulation(fixtures, profile);
    load_settings_data(simulation, fixtures);
    simulation.action("details:900001");
    activate(simulation, "details-more");
    activate(simulation, "game-settings");
    // Rows by topic: the frame-rate rows sit together in Display, and focus steps over the headings.
    const auto& keys = simulation.state().navigation.modals.back().action_keys;
    const std::vector<std::string> top(keys.begin(), keys.begin() + std::min<std::size_t>(keys.size(), 24));
    check(top == std::vector<std::string>{"preset-picker", "save-my-preset", "heading-game", "setting-launch_option", "setting-extra_arguments",
                                          "heading-display", "setting-resolution", "setting-refresh", "setting-frame_limit",
                                          "setting-framegen", "setting-vsync", "setting-performance_overlay", "setting-overlay_detail",
                                          "setting-scaling", "setting-window_view", "heading-controls", "setting-controller",
                                          "setting-controller_preset", "button-mapping", "controller-tuning", "heading-sound",
                                          "setting-volume", "section-compatibility", "section-advanced"},
          "T08b the sheet groups rows by topic");
    check(find_row(simulation, "heading-display") && find_row(simulation, "heading-display")->title == "Display" &&
              !find_row(simulation, "heading-display")->enabled,
          "T08b topic headings only label rows");
    check(find_row(simulation, "setting-refresh") && find_row(simulation, "setting-refresh")->title == "Refresh rate",
          "T08b Refresh rate is listed without a 120 Hz kit");
    simulation.action("focus:setting-resolution");
    simulation.action("down");
    check(simulation.state().navigation.focus_key == "setting-refresh", "T08b Refresh rate follows Resolution");
    simulation.action("down");
    check(simulation.state().navigation.focus_key == "setting-frame_limit", "T08b Frame limit follows Refresh rate");
    simulation.action("focus:setting-window_view");
    simulation.action("down");
    check(simulation.state().navigation.focus_key == "setting-controller", "T08b Down steps over a heading");
    simulation.action("focus:setting-launch_option");
    simulation.action("up");
    check(simulation.state().navigation.focus_key == "save-my-preset", "T08b Up steps over a heading");
    // Compatibility expands inline; graphics is locked by the recipe and scaling is
    // disabled on DXVK with its reason.
    check(!find_row(simulation, "setting-graphics"), "T08b Compatibility starts collapsed");
    activate(simulation, "section-compatibility");
    const auto* graphics = find_row(simulation, "setting-graphics");
    check(graphics && graphics->enabled && graphics->badge == "Required by this game", "T08b graphics is locked by the recipe");
    const auto* scaling = find_row(simulation, "setting-scaling");
    check(scaling && !scaling->enabled && scaling->subtitle == "DXVK games always fit the screen and keep their shape.", "T08b scaling shows its reason");
    activate(simulation, "setting-graphics");
    check(simulation.state().navigation.modals.back().title == "Change anyway?", "T08b a locked row asks before it changes");
    simulation.action("circle");
    check(simulation.state().navigation.modals.back().id == "game-settings", "T08b locked rows do not open a picker");
    // Launch options come from the recipe.
    activate(simulation, "setting-launch_option");
    check(find_row(simulation, "choice-1") && find_row(simulation, "choice-1")->title == "Lost Coast", "T08b launch options list recipe entries");
    activate(simulation, "choice-1");
    // A preset clears overrides and becomes the base.
    activate(simulation, "preset-picker");
    activate(simulation, "preset-mouse-driven");
    const auto& record = simulation.state().game_settings.at(SteamId(900001));
    check(record.preset_id == "mouse-driven" && record.overrides.empty(), "T08b choosing a preset clears overrides");
    check(find_row(simulation, "preset-picker")->value == "Mouse-driven game", "T08b preset row names the preset");
    rt::EffectiveSettings settings;
    check(simulation.EffectiveSettingsFor(SteamId(900001), &settings) && settings.values.at("controller").value == "keyboard",
          "T08b preset values apply");
    // The sheet's settings produce a profile prospero-win accepts.
    rt::ProsperoProfile generated;
    std::string reason;
    check(rt::RenderProsperoProfile(simulation.RecipeFor(SteamId(900001)), settings, {"playden-steam-900001", "/data/playden/games/900001"},
                                    &generated, &reason), "T08b sheet settings render a valid prospero profile");
    // Reset all returns to the preset.
    // Controller tuning is a submenu of Controls; its row counts the changed rows in it.
    check(find_row(simulation, "controller-tuning") && find_row(simulation, "controller-tuning")->value == "Default" &&
              !find_row(simulation, "setting-pointer_speed"),
          "T08b controller tuning rows live in their submenu");
    activate(simulation, "controller-tuning");
    check(simulation.state().navigation.modals.back().id == "controller-tuning", "T08b controller tuning opens");
    activate(simulation, "setting-pointer_speed");
    activate(simulation, "choice-4");
    check(!simulation.state().game_settings.at(SteamId(900001)).overrides.empty(), "T08b override recorded");
    check(simulation.state().navigation.modals.back().id == "controller-tuning", "T08b a choice returns to controller tuning");
    simulation.action("circle");
    check(find_row(simulation, "controller-tuning") && find_row(simulation, "controller-tuning")->value == "1 changed",
          "T08b controller tuning counts its changes");
    activate(simulation, "reset-settings");
    check(simulation.state().game_settings.at(SteamId(900001)).overrides.empty(), "T08b reset all clears overrides");
    simulation.action("circle");
    const auto toast = std::find_if(simulation.state().notices.begin(), simulation.state().notices.end(),
                                    [](const playden::homebrew::Notice& notice) { return notice.id == "settings-saved"; });
    check(toast != simulation.state().notices.end() && toast->message == "Settings saved · Mouse-driven game"
              && toast->detail == "1 change applies on next launch",
          "T08b closing the sheet announces what changed");
    check(simulation.state().navigation.modals.back().id == "context", "T08b closing Game settings returns to Game actions");
    activate(simulation, "game-settings");
    // dxvk.conf rows appear when the kit reads dxvk.conf.
    rt::KitCapabilities kit;
    kit.dxvk_config = true;
    simulation.SetKitCapabilities(kit);
    check(find_row(simulation, "setting-frame_limit") && find_row(simulation, "setting-frame_limit")->enabled,
          "T08b frame limit appears once the kit supports it");
    // Button mapping: inherit shows the recipe binding; a common key and a typed key save.
    activate(simulation, "button-mapping");
    check(find_row(simulation, "setting-button.options")->value == "Escape", "T08b recipe binding shows");
    activate(simulation, "setting-button.touchpad");
    activate(simulation, "choice-17");
    check(simulation.state().game_settings.at(SteamId(900001)).overrides.at("button.touchpad") == "f5", "T08b common key saves");
    activate(simulation, "setting-button.l3");
    activate(simulation, "choice-custom");
    simulation.text("vk:0x41");
    simulation.action("done");
    check(simulation.state().game_settings.at(SteamId(900001)).overrides.at("button.l3") == "vk:0x41", "T08b typed key saves");
    activate(simulation, "setting-button.touchpad");
    activate(simulation, "choice-0");
    check(!simulation.state().game_settings.at(SteamId(900001)).overrides.count("button.touchpad"), "T08b inherit removes the binding");
    simulation.action("circle");
    check(find_row(simulation, "button-mapping")->modified, "T08b changed bindings mark the row");
    check(!find_row(simulation, "clear-shader-cache"), "T08b maintenance is not in the settings sheet");
    // Maintenance lives on the game page, each action behind a confirmation.
    simulation.action("circle");
    activate(simulation, "game-maintenance");
    check(find_row(simulation, "maintenance-shader") && find_row(simulation, "maintenance-shader")->badge == "Game must be closed",
          "T08b maintenance lists clear shader cache");
    check(find_row(simulation, "maintenance-recreate")->badge == "Prepares bottle", "T08b recreate prepares the bottle");
    simulation.action("r1");
    check(simulation.state().navigation.modals.back().focused_action == 5, "T08b R1 jumps five rows in maintenance");
    simulation.action("l1");
    check(simulation.view().legends.size() == 3 && simulation.view().legends[1] == "L1/R1 Jump", "T08b maintenance names the jump");
    activate(simulation, "maintenance-shader");
    check(simulation.state().navigation.modals.back().id == "confirmation", "T08b maintenance asks first");
    activate(simulation, "confirm-destructive");
    check(simulation.state().navigation.modals.back().id == "game-maintenance", "T08b confirming returns to maintenance");
    check(std::any_of(simulation.state().notices.begin(), simulation.state().notices.end(),
                      [](const playden::homebrew::Notice& notice) { return notice.id == "maintenance-900001"; }),
          "T08b clear shader cache reports");
    // Games without a recipe still get a usable sheet.
    simulation.action("circle");
    check(simulation.state().navigation.modals.back().id == "context", "T08b closing Maintenance returns to Game actions");
    simulation.action("circle");
    simulation.action("details:900003");
    activate(simulation, "details-more");
    activate(simulation, "game-settings");
    check(find_row(simulation, "setting-resolution") && find_row(simulation, "setting-resolution")->value == "1920×1080",
          "T08b games without a recipe show Playden defaults");
}

// The preset chooser compares the focused preset with the current values; the Community
// entry is listed but waits for the shared index; L1/R1 jump five rows; Advanced ends with
// the read-only effective command.
void settings_sheet_details(const std::filesystem::path& fixtures, const std::filesystem::path& profile) {
    playden::homebrew::Simulation simulation(fixtures, profile);
    load_settings_data(simulation, fixtures);
    simulation.action("details:900001");
    activate(simulation, "details-more");
    activate(simulation, "game-settings");
    activate(simulation, "preset-picker");
    check(simulation.view().comparison.has_value(), "preset chooser compares the focused preset");
    check(simulation.action("focus:preset-mouse-driven"), "focus another preset");
    const auto& comparison = *simulation.view().comparison;
    check(!comparison.rows.empty() && !comparison.note.empty() && !comparison.title.empty(),
          "the comparison lists settings and closes with what applies");
    // A game without community recipes says so in a row that can't be chosen.
    const auto* community = find_row(simulation, "recipes-none");
    check(community && !community->enabled && community->title == "None yet", "the chooser says the game has no recipes");
    // Headings set the players' recipes apart from the built-in presets, and focus never stops on them.
    const auto* heading = find_row(simulation, "chooser-heading-community");
    check(heading && heading->title.find("Community recipes") == 0 && find_row(simulation, "chooser-heading-presets"),
          "the chooser has Community recipes and Built-in presets headings");
    for (int step = 0; step < 12; ++step) {
        simulation.action("down");
        check(!playden::homebrew::IsChooserHeading(simulation.state().navigation.focus_key), "focus skips the headings");
    }
    simulation.action("focus:recipes-none");
    simulation.action("cross");
    check(simulation.state().navigation.modals.back().id == "preset-picker", "the community row does nothing");
    simulation.action("circle");
    simulation.action("focus:save-my-preset");
    simulation.action("r1");
    check(simulation.state().navigation.modals.back().focused_action == 6, "R1 jumps five rows");
    simulation.action("l1");
    check(simulation.state().navigation.modals.back().focused_action == 1, "L1 jumps back");
    activate(simulation, "section-advanced");
    const auto* command = find_row(simulation, "effective-command");
    check(command && command->value == "Read only" && !command->subtitle.empty(), "Advanced ends with the effective command");
    activate(simulation, "effective-command");
    check(simulation.state().navigation.modals.back().title == "Effective command" &&
              !simulation.state().navigation.modals.back().body.empty(),
          "the effective command opens in full");
    simulation.action("circle");
    simulation.action("circle");
}

void settings_revision_exhaustion_preserves_record(const std::filesystem::path& fixtures,
                                                 const std::filesystem::path& profile) {
    playden::homebrew::Simulation simulation(fixtures, profile);
    load_settings_data(simulation, fixtures);
    const auto settings_file = profile.string() + ".settings";
    playden::runtime::GameSettingsRecord record;
    record.revision = std::numeric_limits<std::uint64_t>::max();
    record.overrides["resolution"] = "1280x720";
    std::string reason;
    check(playden::runtime::SaveGameSettingsFile(settings_file, {{SteamId(900001), record}}, &reason), "seed exhausted revision");
    simulation.SetGameSettingsFile(settings_file);
    simulation.action("details:900001");
    activate(simulation, "details-more");
    activate(simulation, "game-settings");
    activate(simulation, "setting-resolution");
    activate(simulation, "choice-2");
    check(simulation.state().game_settings.at(SteamId(900001)) == record,
          "exhausted settings revision rejects change without wrapping");
    simulation.action("circle");
    playden::homebrew::Simulation restored(fixtures, profile);
    check(restored.state().game_settings.count(SteamId(900001)) && restored.state().game_settings.at(SteamId(900001)) == record,
          "fixture settings restore preserves the exact uint64 revision");
    record.revision = 9007199254740993ULL;
    check(playden::runtime::SaveGameSettingsFile(settings_file, {{SteamId(900001), record}}, &reason), "seed exact revision above double precision");
    simulation.SetGameSettingsFile(settings_file);
    simulation.action("circle");
    playden::homebrew::Simulation precise(fixtures, profile);
    check(precise.state().game_settings.count(SteamId(900001)) && precise.state().game_settings.at(SteamId(900001)) == record,
          "fixture revision cannot round through double precision");
    std::filesystem::remove(settings_file);
}

void settings_save_failure_does_not_claim_success(const std::filesystem::path& fixtures,
                                                const std::filesystem::path& profile) {
    playden::homebrew::Simulation simulation(fixtures, profile);
    load_settings_data(simulation, fixtures);
    simulation.action("details:900001");
    activate(simulation, "details-more");
    activate(simulation, "game-settings");
    std::filesystem::remove(profile);
    std::filesystem::create_directory(profile);
    activate(simulation, "setting-resolution");
    activate(simulation, "choice-2");
    simulation.action("circle");
    check(std::none_of(simulation.state().notices.begin(), simulation.state().notices.end(),
                       [](const auto& notice) { return notice.id == "settings-saved"; }),
          "failed settings persistence cannot announce saved");
    std::filesystem::remove_all(profile);
}

void t09_uninstall_and_reset_preserve_declared_data(const std::filesystem::path& fixtures,
                                                    const std::filesystem::path& profile) {
    playden::homebrew::Simulation simulation(fixtures, profile);
    simulation.action("details:900001");
    simulation.action("options");
    activate(simulation, "uninstall");
    check(simulation.state().navigation.modals.back().focused_action == 0, "T09 uninstall defaults to Cancel");
    simulation.action("circle");
    simulation.action("circle");
    simulation.action("options");
    activate(simulation, "uninstall");
    activate(simulation, "confirm-destructive");
    simulation.action("details:900001"); simulation.action("options"); activate(simulation, "uninstall"); activate(simulation, "confirm-destructive");
    const auto uninstalls = std::count_if(simulation.state().jobs.begin(), simulation.state().jobs.end(),
        [](const playden::homebrew::Job& job) { return job.game_id == SteamId(900001) && job.kind == "uninstall" && job.state != playden::homebrew::JobState::Cancelled; });
    check(uninstalls == 1, "T09 repeated uninstall confirmation is idempotent");
    int active = 0;
    for (const auto& job : simulation.state().jobs)
        if (job.state == playden::homebrew::JobState::Downloading || job.state == playden::homebrew::JobState::Verifying) ++active;
    check(active == 1, "T09 queued uninstall preserves the one-active-operation invariant");
    simulation.action("job:fail:job-1:download"); simulation.action("job:fail:job-2:download"); simulation.action("job:fail:job-3:download");
    simulation.tick(3);
    const auto removed = std::find_if(simulation.state().installations.begin(), simulation.state().installations.end(),
                                      [](const playden::homebrew::Installation& value) { return value.game_id == SteamId(900001); });
    check(removed != simulation.state().installations.end() && !removed->available && removed->saves_present,
          "T09 uninstall removes only the selected fixture install and preserves saves");
    check(simulation.action("reset"), "T09 reset is available");
    const auto after_reset = std::find_if(simulation.state().installations.begin(), simulation.state().installations.end(),
                                          [](const playden::homebrew::Installation& value) { return value.game_id == SteamId(900001); });
    check(after_reset != simulation.state().installations.end() && after_reset->saves_present,
          "T09 reset preserves fixture installs and saves");
    check(simulation.action("install:900001:external"), "T09 a completed uninstall does not block reinstall");
    check(std::any_of(simulation.state().jobs.begin(), simulation.state().jobs.end(),
                      [](const playden::homebrew::Job& job) { return job.game_id == SteamId(900001) && job.kind == "install" && job.state == playden::homebrew::JobState::Queued; }),
          "T09 reinstall creates a fresh stable job");
}

void t10_session_failure_running_stop_and_crash(const std::filesystem::path& fixtures,
                                                const std::filesystem::path& profile) {
    playden::homebrew::Simulation simulation(fixtures, profile);
    simulation.scenario("launch-failure");
    activate(simulation, "session-retry");
    simulation.tick(2);
    check(simulation.state().session == playden::homebrew::SessionState::Running, "T10 launch retry reaches running");
    check(simulation.action("session:quit"), "T10 session quit is controller reachable");
    simulation.action("inject:stop-failure");
    check(simulation.state().session == playden::homebrew::SessionState::Failed, "T10 stop failure is recoverable");
    simulation.action("inject:crash");
    activate(simulation, "session-logs");
    simulation.action("circle");
    check(simulation.state().navigation.route == playden::homebrew::Route::Details
              && simulation.state().navigation.detail_id == SteamId(900001),
          "T10 crash logs return to prior game details");
}

void t11_preferences_failure_and_controller_modal_recovery(const std::filesystem::path& fixtures,
                                                            const std::filesystem::path& profile) {
    playden::homebrew::Simulation simulation(fixtures, profile);
    simulation.action("route:settings");
    activate(simulation, "settings-motion");
    playden::homebrew::Simulation restored(fixtures, profile);
    check(restored.state().reduced_motion, "T11 reduced motion persists across restart");
    restored.scenario("save-failure");
    restored.action("route:library");
    restored.action("triangle");
    const auto focus = restored.state().navigation.focus_key;
    restored.action("inject:controller-disconnect");
    restored.action("inject:controller-reconnect");
    check(restored.view().surface == "filters" && restored.state().navigation.focus_key == focus,
          "T11 controller reconnect retains modal and visible focus");
    restored.action("circle"); restored.action("route:settings");
    activate(restored, "settings-notices"); activate(restored, "retry-save"); restored.action("circle");
    activate(restored, "settings-logs");
    check(restored.state().log_state == playden::homebrew::LogState::Loading, "T11 logs expose a loading state");
    restored.tick(1);
    check(restored.state().log_state == playden::homebrew::LogState::Ready && restored.state().log_lines.size() == 32,
          "T11 logs resolve deterministically to bounded contextual records");
    for (int index = 0; index < 20; ++index) restored.action("r2");
    check(restored.state().log_offset == 24 && restored.state().log_lines.size() <= 128,
          "T11 log scrolling clamps to the eight-row viewport");
    restored.action("inject:logs-failure"); activate(restored, "logs-retry"); restored.tick(1);
    check(restored.state().log_state == playden::homebrew::LogState::Ready, "T11 failed logs retry through normal actions");
    restored.action("circle"); activate(restored, "settings-account"); activate(restored, "account-switch");
    check(restored.state().navigation.modals.back().focused_action == 0, "T11 account switch defaults to Cancel");
    activate(restored, "confirm-destructive");
    check(restored.state().account().authentication == playden::homebrew::AuthState::Waiting
              && restored.state().account().id.empty() && restored.state().library.favorites.empty(),
          "T11 account switch clears identity and account-visible cache");
    restored.action("inject:qr-approved"); restored.tick(1);
    check(restored.state().account().authentication == playden::homebrew::AuthState::SignedIn
              && !restored.state().account().id.empty() && restored.state().account().last_sync > 0,
          "T11 account recovery restores a new fixture identity and last-sync state");
    restored.action("route:library"); restored.action("search"); restored.text("Night Orchard"); restored.action("done"); restored.action("square");
    const auto switched_id = restored.state().account().id;
    const auto switched_name = restored.state().account().display_name;
    const auto switched_sync = restored.state().account().last_sync;
    playden::homebrew::Simulation switched_restart(fixtures, profile);
    check(switched_restart.state().account().id == switched_id && switched_restart.state().account().display_name == switched_name
              && switched_restart.state().account().last_sync == switched_sync
              && switched_restart.state().library.query == "Night Orchard"
              && switched_restart.state().library.favorites.contains(SteamId(900010)),
          "T11 switched account identity and organization restore only from its profile");
    restored.action("route:settings"); activate(restored, "settings-about");
    check(restored.view().surface == "about" && restored.view().rows.size() == 5,
          "T11 About exposes version, build, kit, kit source, and Back rows");
    {
        const auto& rows = restored.view().rows;
        const auto kit = std::find_if(rows.begin(), rows.end(), [](const auto& row) { return row.key == "about-kit"; });
        check(kit != rows.end() && kit->title == "Runtime kit in use" && kit->value.rfind("playden-kit 0.13 · ", 0) == 0 &&
                  kit->subtitle.empty(),
              "T11 About names the runtime kit in use, with no note when it is the one Playden expects");
    }
}

// The family-sharing fixture marks a few games shared by a Steam family member and leaves the rest owned.
void family_sharing_scenario_marks_shared_games(const std::filesystem::path& fixtures,
                                                const std::filesystem::path& profile) {
    playden::homebrew::Simulation simulation(fixtures, profile);
    const auto count = simulation.state().games.size();
    check(simulation.scenario("family-sharing"), "family-sharing scenario loads");
    const auto& games = simulation.state().games;
    const auto shared = std::count_if(games.begin(), games.end(), [](const playden::homebrew::Game& game) { return game.family_shared; });
    check(games.size() == count && shared > 0 && static_cast<std::size_t>(shared) < count,
          "family-sharing marks some fixture games shared without adding games");
    check(simulation.snapshot().find("\"route\":\"library\"") != std::string::npos, "family-sharing opens the Library");
}

// The game update scenarios: Update or DLC available after Play with its sheet, the install sheet's options, and a
// queue of updates whose first one locks Play.
void game_update_scenarios(const std::filesystem::path& fixtures, const std::filesystem::path& profile) {
    playden::homebrew::Simulation simulation(fixtures, profile);
    const auto row = [&simulation](const std::string& key) -> const playden::homebrew::ViewRow* {
        for (const auto& item : simulation.view().rows) if (item.key == key) return &item;
        return nullptr;
    };
    check(simulation.scenario("update-available"), "update-available loads");
    check(row("details-update") && row("details-update")->title == "Update…" && row("details-update-note") &&
              row("details-update-note")->title == "Update available" && row("details-update-note")->value == "1.2 GB",
          "update-available offers Update with its size");
    check(simulation.action("cross") && simulation.view().surface == "update" && row("update-confirm") &&
              row("update-confirm")->enabled && row("update-confirm")->value == "1.2 GB",
          "Update opens the update sheet");
    check(simulation.action("circle"), "the sheet closes");
    check(simulation.scenario("dlc-available") && row("details-update")->title == "DLC available…",
          "dlc-available says DLC available");
    check(simulation.scenario("install-options") && simulation.view().surface == "install" && row("install-version") &&
              row("install-language") && row("install-dlc") && row("install-dlc")->value == "2 of 2 · 4.3 GB",
          "install-options shows Version, Language and DLC");
    check(simulation.action("focus:install-dlc") && simulation.action("cross") && simulation.view().surface == "install-dlc" &&
              row("install-dlc-900102") && row("install-dlc-900102")->title == "Texture Pack" &&
              row("install-dlc-900102")->value == "Free · 4.2 GB",
          "the DLC row opens the list");
    check(simulation.action("circle") && simulation.view().surface == "install", "Back returns to the sheet");
    check(simulation.scenario("updating") && simulation.state().navigation.route == playden::homebrew::Route::Downloads,
          "updating opens the queue");
    check(simulation.action("details:900001") && row("details-play") && !row("details-play")->enabled &&
              row("details-play")->value == "Finishing the update…" && !row("details-update"),
          "an update putting its files in place locks Play");
    check(simulation.scenario("moving") && simulation.state().navigation.route == playden::homebrew::Route::Downloads &&
              simulation.state().jobs.size() == 3 && simulation.state().jobs.front().move &&
              simulation.state().jobs.front().stage_text.rfind("Moving to ", 0) == 0,
          "moving opens the queue with its moves");
    check(simulation.action("details:900001") && row("details-play") && !row("details-play")->enabled &&
              row("details-play")->value == "Moving…",
          "a game that is moving can't be played");
}

void t12_stress_dataset_keeps_logical_focus(const std::filesystem::path& fixtures,
                                            const std::filesystem::path& profile) {
    playden::homebrew::Simulation simulation(fixtures, profile);
    simulation.scenario("delayed-art");
    const auto delayed_id = simulation.state().navigation.selected_game_id;
    const auto delayed_index = simulation.state().navigation.selected_index;
    const auto delayed_scroll = simulation.state().navigation.scroll_offsets.at("library-grid");
    const auto bytes = simulation.state().jobs.front().completed_bytes;
    const auto delayed = std::find_if(simulation.state().games.begin(), simulation.state().games.end(),
                                      [](const playden::homebrew::Game& game) { return game.id == SteamId(900007); });
    check(delayed != simulation.state().games.end() && delayed->art_pending, "T12 delayed art exposes a stable placeholder state");
    simulation.tick(1);
    check(!delayed->art_pending && simulation.state().navigation.selected_game_id == delayed_id
              && simulation.state().navigation.selected_index == delayed_index
              && simulation.state().navigation.scroll_offsets.at("library-grid") == delayed_scroll
              && simulation.state().jobs.front().completed_bytes > bytes,
          "T12 asynchronous art and job updates preserve focus and viewport");
    simulation.scenario("stress");
    check(simulation.state().games.size() == 1000, "T12 stress scenario loads all 1,000 logical games");
    check(simulation.view().rows.size() <= 24, "T12 library materializes a bounded window");
    simulation.action("select:100500");
    const auto focused = simulation.state().navigation.selected_game_id;
    simulation.tick(3);
    check(simulation.state().navigation.selected_game_id == focused,
          "T12 asynchronous art/job updates never steal stable game focus");
    check(simulation.snapshot().find("\"materialized_row_count\":24") != std::string::npos,
          "T12 snapshot records bounded library materialization");
}

void t13_settings_sections_rail(const std::filesystem::path& fixtures, const std::filesystem::path& profile) {
    playden::homebrew::Simulation simulation(fixtures, profile);
    simulation.action("route:settings");
    const auto section = [&simulation] {
        const auto& view = simulation.view();
        return view.rows[static_cast<std::size_t>(view.focused_row)].section;
    };
    check(section() == "Account", "T13 Settings opens on the Account section");
    simulation.action("left");
    check(simulation.state().navigation.settings_rail_focused, "T13 Left moves to the section rail");
    simulation.action("down");
    check(section() == "Library" && simulation.state().navigation.focus_key == "settings-import",
          "T13 Down on the rail switches to the next section's first row (Import games heads Library)");
    simulation.action("cross");
    check(!simulation.state().navigation.settings_rail_focused && simulation.state().navigation.focus_key == "settings-import" &&
              simulation.state().navigation.modals.empty(),
          "T13 Cross on the rail enters the section without activating its row");
    simulation.action("down");
    check(section() == "Library" && simulation.state().navigation.focus_key == "settings-import",
          "T13 Down on a section's last row stays in the section");
    simulation.action("left");
    simulation.action("down");
    check(section() == "Community", "T13 Community follows Library on the rail");
    simulation.action("down");
    simulation.action("right");
    check(section() == "Storage" && simulation.state().navigation.focus_key == "settings-storage",
          "T13 Storage starts with Games storage, Import games no longer in it");
    simulation.action("down");
    simulation.action("down");
    check(section() == "Storage" && simulation.state().navigation.focus_key == "settings-runtime",
          "T13 Down stops on the section's last row");
    simulation.action("up");
    check(section() == "Storage" && simulation.state().navigation.focus_key == "settings-storage" &&
              !simulation.state().navigation.tabs_focused, "T13 Up stops on the section's first row");
    simulation.action("up");
    check(simulation.state().navigation.focus_key == "settings-storage" && simulation.state().navigation.tabs_focused,
          "T13 Up on a section's first row goes to the tab bar, not the previous section");
    simulation.action("down");
    simulation.action("left");
    simulation.action("down");
    simulation.action("right");
    check(section() == "Display" && simulation.state().navigation.focus_key == "settings-motion",
          "T13 the rail moves to the next section");
    simulation.action("left");
    simulation.action("right");
    check(!simulation.state().navigation.settings_rail_focused && simulation.state().navigation.focus_key == "settings-motion",
          "T13 Right returns from the rail to the same row");

    // Quit Playden, as on macOS: the rail entry is the action, with no "Quit / Review" card.
    simulation.action("left");
    for (int step = 0; step < 12; ++step) simulation.action("down");
    check(section() == "Quit Playden" && simulation.state().navigation.focus_key == "settings-quit",
          "T13 the last rail entry is Quit Playden");
    simulation.action("right");
    check(simulation.state().navigation.settings_rail_focused, "T13 Right stays on the Quit Playden rail entry");
    simulation.action("cross");
    check(!simulation.state().navigation.modals.empty() && simulation.state().navigation.modals.back().id == "confirmation",
          "T13 Cross on Quit Playden asks to confirm quitting");
    simulation.action("circle");
    check(simulation.state().navigation.modals.empty() && simulation.state().navigation.settings_rail_focused,
          "T13 cancelling returns to the Quit Playden rail entry");
    simulation.action("up");
    simulation.action("right");
    for (int step = 0; step < 20; ++step) simulation.action("down");
    check(section() != "Quit Playden", "T13 moving down the rows never lands on the Quit Playden entry");
}

} // namespace

void t14_tab_bar_from_the_top_of_a_page(const std::filesystem::path& fixtures,
                                        const std::filesystem::path& profile) {
    playden::homebrew::Simulation simulation(fixtures, profile);
    simulation.scenario("default");
    simulation.action("r1");
    check(simulation.state().navigation.route == playden::homebrew::Route::Library, "T14 Library is open");
    for (int step = 0; step < 20 && !simulation.state().navigation.tabs_focused; ++step) simulation.action("up");
    check(simulation.state().navigation.tabs_focused && simulation.state().navigation.tab_focus == 1,
          "T14 Up from the top of a page focuses the current tab");
    simulation.action("right");
    simulation.action("right");
    check(simulation.state().navigation.tab_focus == 3 &&
              simulation.state().navigation.route == playden::homebrew::Route::Library,
          "T14 Left/Right move between tabs without opening them");
    simulation.action("right");
    simulation.action("right");
    check(simulation.state().navigation.tab_focus == 4, "T14 the last tab stays focused at the end");
    simulation.action("cross");
    check(simulation.state().navigation.route == playden::homebrew::Route::Settings &&
              !simulation.state().navigation.tabs_focused, "T14 Cross opens the focused tab and focuses its page");
    for (int step = 0; step < 20 && !simulation.state().navigation.tabs_focused; ++step) simulation.action("up");
    simulation.action("left");
    simulation.action("down");
    check(!simulation.state().navigation.tabs_focused &&
              simulation.state().navigation.route == playden::homebrew::Route::Settings,
          "T14 Down returns to the page without changing tab");
    for (int step = 0; step < 20 && !simulation.state().navigation.tabs_focused; ++step) simulation.action("up");
    simulation.action("circle");
    check(!simulation.state().navigation.tabs_focused, "T14 Circle returns to the page");
}

// Issue 52: every value a recipe sets has a row the player can change and reset.
void t52_recipe_values_in_game_settings(const std::filesystem::path& fixtures, const std::filesystem::path& profile) {
    namespace rt = playden::runtime;
    playden::homebrew::Simulation simulation(fixtures, profile);
    load_settings_data(simulation, fixtures);
    std::map<playden::GameId, playden::homebrew::LiveGameSetup> setups;
    auto& setup = setups[SteamId(900001)];
    setup.scanned = true;
    setup.programs = {{"bin/win64/hl2_win64.exe", "pe64", "", false, "d3d11 graphics=dxvk"},
                      {"Launcher.exe", "pe32", "This game needs .NET, which Playden's runtime kit doesn't include yet.", true, ""},
                      {"Stub.exe", "pe32", "This game's program has Steam DRM in a form Playden can't unpack yet.", false, ""}};
    // The programs show in an open Program picker as soon as the folder's scan arrives.
    simulation.action("details:900001");
    activate(simulation, "details-more");
    activate(simulation, "game-settings");
    activate(simulation, "setting-launch_option");
    check(!find_row(simulation, "program-0"), "T52 no programs before the scan");
    simulation.SetGameSetups(setups);
    check(find_row(simulation, "program-0") && simulation.state().navigation.modals.back().setting_id == "launch_option",
          "T52 the open picker lists the scanned programs");
    simulation.action("circle");
    simulation.action("circle");
    simulation.action("circle");
    const auto overrides = [&]() -> const std::map<std::string, std::string>& {
        return simulation.state().game_settings.at(SteamId(900001)).overrides;
    };
    simulation.action("details:900001");
    activate(simulation, "details-more");
    activate(simulation, "game-settings");
    activate(simulation, "section-advanced");
    check(find_row(simulation, "setting-launch_option")->title == "Program", "T52 the launch option is the Program row");

    // Program: Steam's entries are suggestions; any program in the game folder can start.
    activate(simulation, "setting-launch_option");
    check(find_row(simulation, "choice-1") && find_row(simulation, "program-0") && find_row(simulation, "value-reset"),
          "T52 the Program picker lists launch options and the folder's programs");
    check(!find_row(simulation, "program-2")->enabled && find_row(simulation, "program-1")->enabled,
          "T52 a hard refusal cannot be picked, a soft one can");
    activate(simulation, "program-0");
    check(overrides().at("program") == "bin/win64/hl2_win64.exe" && overrides().at("architecture") == "pe64",
          "T52 a picked program saves with its architecture");
    check(find_row(simulation, "setting-launch_option")->value == "hl2_win64.exe" &&
          find_row(simulation, "setting-architecture")->value == "64-bit" && !find_row(simulation, "setting-architecture")->enabled,
          "T52 the program type follows the program");
    check(find_row(simulation, "effective-command")->subtitle.rfind("bin/win64/hl2_win64.exe -game hl2", 0) == 0,
          "T52 the effective command starts the picked program");
    check(!find_row(simulation, "setting-start_check"), "T52 no known problem, no Try anyway");
    // A preset or a reset keeps the program, so a game that starts only with it keeps starting.
    check(simulation.action("focus:setting-architecture") && simulation.action("triangle"), "T52 triangle on program type");
    check(overrides().count("architecture"), "T52 the program type is not reset on its own");
    activate(simulation, "setting-resolution");
    activate(simulation, "choice-0");
    activate(simulation, "preset-picker");
    activate(simulation, "preset-mouse-driven");
    check(overrides().count("program") && overrides().count("architecture") && !overrides().count("resolution"),
          "T52 choosing a preset keeps the program");
    activate(simulation, "setting-resolution");
    activate(simulation, "choice-0");
    activate(simulation, "reset-settings");
    check(overrides().count("program") && !overrides().count("resolution"), "T52 reset keeps the program");
    check(!find_row(simulation, "reset-settings")->enabled, "T52 nothing else left to reset");
    activate(simulation, "setting-launch_option");
    activate(simulation, "value-reset");
    check(!overrides().count("program") && !overrides().count("architecture"), "T52 reset returns to the recipe's program");
    // A soft refusal: picking it means trying anyway, and the reason stays on the row.
    activate(simulation, "setting-launch_option");
    activate(simulation, "program-1");
    check(overrides().at("start_check") == "try", "T52 a soft-refused program tries anyway");
    activate(simulation, "section-compatibility");
    const auto* problems = find_row(simulation, "setting-start_check");
    check(problems && problems->value == "Try anyway" && problems->subtitle.find(".NET") != std::string::npos,
          "T52 Known problems shows the refusal");
    check(simulation.action("focus:setting-launch_option") && simulation.action("triangle"), "T52 triangle resets the program");
    check(!overrides().count("program") && !overrides().count("architecture"), "T52 triangle drops the picked program");
    check(!overrides().count("start_check"), "T52 Try anyway goes with the program it was set for");
    activate(simulation, "setting-launch_option");
    activate(simulation, "program-1");
    activate(simulation, "setting-launch_option");
    activate(simulation, "value-reset");
    check(!overrides().count("start_check") && !overrides().count("program"), "T52 reset to the recipe drops Try anyway");
    activate(simulation, "setting-launch_option");
    activate(simulation, "program-1");
    activate(simulation, "setting-launch_option");
    activate(simulation, "program-0");
    check(!overrides().count("start_check"), "T52 another program drops Try anyway");
    activate(simulation, "setting-launch_option");
    activate(simulation, "value-reset");

    // Required arguments: start as the recipe's, can be replaced and reset.
    activate(simulation, "setting-arguments");
    check(simulation.state().navigation.modals.back().id == "setting-value", "T52 required arguments open their actions");
    check(!find_row(simulation, "value-reset")->enabled, "T52 nothing to reset yet");
    activate(simulation, "value-edit");
    check(simulation.state().editor.value.rfind("-game hl2 -steam {resolution}", 0) == 0, "T52 the editor starts from the recipe");
    simulation.action("triangle");  // clears the editor
    simulation.text("-game hl2 {resolution}");
    simulation.action("done");
    check(overrides().at("arguments") == "-game hl2 {resolution}", "T52 required arguments save");
    check(find_row(simulation, "effective-command")->subtitle == "hl2.exe -game hl2 -w 1920 -h 1080", "T52 the command follows");
    activate(simulation, "setting-arguments");
    activate(simulation, "value-reset");
    check(!overrides().count("arguments"), "T52 required arguments reset");

    // Library overrides: the whole list, each entry removable, then added to and reset.
    activate(simulation, "setting-dll_overrides");
    check(find_row(simulation, "override-remove-0")->title == "d3d9=n" && !find_row(simulation, "override-remove-1"),
          "T52 the recipe's overrides are listed");
    activate(simulation, "override-remove-0");
    check(overrides().at("dll_overrides").empty(), "T52 a recipe override can be removed");
    activate(simulation, "override-add");
    simulation.text("dinput8=n,b");
    simulation.action("done");
    check(overrides().at("dll_overrides") == "dinput8=n,b", "T52 an override can be added");
    activate(simulation, "setting-dll_overrides");
    activate(simulation, "value-reset");
    check(!overrides().count("dll_overrides"), "T52 library overrides reset");

    // A refused value keeps the editor open with a notice: the setting first, then the whole reason, which
    // names the variables Environment variables accepts (#480). The row says which ones too.
    {
        playden::runtime::KitCapabilities kit;
        kit.environment = true;
        simulation.SetKitCapabilities(kit);
    }
    check(find_row(simulation, "setting-environment") != nullptr, "T52 Environment variables is listed");
    check(find_row(simulation, "setting-environment")->subtitle.find("DXVK_") != std::string::npos,
          "T52 Environment variables names the variables it accepts");
    activate(simulation, "setting-environment");
    check(simulation.state().editor.active, "T52 Environment variables opens the keyboard");
    simulation.text("PROTON_DISABLE_LSTEAMCLIENT=1");
    simulation.action("done");
    {
        const auto refused = std::find_if(simulation.state().notices.begin(), simulation.state().notices.end(),
                                          [](const auto& notice) { return notice.id == "setting-validation"; });
        check(simulation.state().editor.active && refused != simulation.state().notices.end() && refused->persistent,
              "T52 a refused variable keeps the editor open with a notice");
        check(refused != simulation.state().notices.end() && refused->message == "Environment variables" &&
                  refused->detail.rfind("PROTON_DISABLE_LSTEAMCLIENT is not an allowed variable.", 0) == 0 &&
                  refused->detail.find("DXVK_") != std::string::npos &&
                  refused->detail.find("WINE_CPU_TOPOLOGY") != std::string::npos,
              "T52 the notice gives the whole reason and the accepted names");
    }
    // Cancelled, the refusal goes with the editor and does not show in the next one.
    simulation.action("circle");
    check(!simulation.state().editor.active &&
              std::none_of(simulation.state().notices.begin(), simulation.state().notices.end(),
                           [](const auto& notice) { return notice.id == "setting-validation"; }),
          "T52 a cancelled editor takes its refusal with it");
    activate(simulation, "setting-environment");
    simulation.action("triangle");  // clears the editor
    simulation.text("PROTON_LOG=1");
    simulation.action("done");
    check(simulation.state().editor.active, "T52 refused again");
    simulation.action("triangle");  // clears the editor
    simulation.text("DXVK_ASYNC=1");
    simulation.action("done");
    check(overrides().at("environment") == "DXVK_ASYNC=1" &&
              std::none_of(simulation.state().notices.begin(), simulation.state().notices.end(),
                           [](const auto& notice) { return notice.id == "setting-validation"; }),
          "T52 an accepted variable saves and clears the notice");
    activate(simulation, "setting-environment");
    simulation.action("triangle");
    simulation.action("done");
    check(!overrides().count("environment") || overrides().at("environment").empty(), "T52 Environment variables clear");
    simulation.SetKitCapabilities({});

    // Locked rows: Change anyway, then reset to the recipe.
    activate(simulation, "setting-graphics");
    activate(simulation, "unlock-setting");
    check(simulation.state().navigation.modals.back().setting_id == "graphics", "T52 Change anyway opens the picker");
    activate(simulation, "choice-3");
    check(overrides().at("graphics") == "opengl" && overrides().count("unlock.graphics"), "T52 a locked row changes anyway");
    check(find_row(simulation, "setting-graphics")->badge == "Changed anyway" &&
          find_row(simulation, "setting-graphics")->subtitle.find("may not start") != std::string::npos,
          "T52 the warning stays on the row");
    activate(simulation, "setting-graphics");
    activate(simulation, "value-reset");
    check(!overrides().count("graphics") && !overrides().count("unlock.graphics"), "T52 reset locks the row again");

    // Export as recipe: a community recipe file that reads back.
    const auto directory = std::filesystem::temp_directory_path() / ("playden-t52-" + std::to_string(::getpid()));
    std::filesystem::remove_all(directory);
    simulation.SetRecipeExportTarget(directory, "13.60");
    playden::homebrew::LiveKitInfo kit_info;
    kit_info.kit_id = "hl2-fw1360-dev1";
    kit_info.version = "0.12-dev.20261022.1";
    simulation.SetKitInfo(kit_info);
    activate(simulation, "export-recipe");
    check(simulation.state().navigation.modals.back().title == "Recipe exported", "T52 export says where it went");
    check(simulation.state().navigation.modals.back().body.find("playden-ps5-recipes") != std::string::npos,
          "T52 export points at the recipes repository");
    rt::CommunityRecipe community;
    std::string reason;
    const auto exported_text = read_text(directory / "recipe-900001.json");
    check(rt::ParseCommunityRecipe(exported_text, &community, &reason) &&
              community.tested_firmware == "13.60" && community.executable == "hl2.exe" &&
              community.recipe.dll_overrides == "d3d9=n" && community.recipe.locked.empty(),
          "T52 the exported recipe reads back");
    // The kit version it was tested on, not the kit id, and no requirement; the author names it.
    check(community.tested_kit == "0.12-dev.20261022.1" && community.min_kit.empty() &&
              exported_text.find("hl2-fw1360-dev1") == std::string::npos,
          "T52 the export says which kit version it was tested on");
    check(community.recipe.id == "community-steam-900001" && community.title.empty() && community.contributors.empty() &&
              exported_text.find("\"contributors\": []") != std::string::npos,
          "T52 the export leaves the slug, title and contributors to the author");
    // Named by the author, it reads back with its slug and title.
    auto named = community;
    named.recipe.id = rt::CommunityRecipeIdFor(playden::GameRef::Steam(900001), "dxvk-mouse");
    named.title = "DXVK with the mouse preset";
    named.contributors = {"Bo"};
    rt::CommunityRecipe reread;
    check(rt::ParseCommunityRecipe(rt::SerializeCommunityRecipe(named), &reread, &reason) &&
              reread.recipe.id == "community-steam-900001-dxvk-mouse" && reread.title == named.title &&
              reread.contributors == named.contributors,
          "T52 a named export round-trips");
    // A kit before 0.10 has no version: tested_kit stays empty.
    kit_info.version.clear();
    simulation.SetKitInfo(kit_info);
    activate(simulation, "alert-ok");
    activate(simulation, "export-recipe");
    check(rt::ParseCommunityRecipe(read_text(directory / "recipe-900001.json"), &community, &reason) &&
              community.tested_kit.empty(),
          "T52 an older kit leaves tested_kit empty");
    std::filesystem::remove_all(directory);

    // A game Playden refused starts its sheet from the draft recipe.
    rt::GameRecipe draft;
    draft.id = rt::GeneratedRecipeId(900003);
    draft.version = 2;
    draft.app_id = 900003;
    setups[SteamId(900003)].draft = draft;
    setups[SteamId(900003)].refusal_soft = true;
    setups[SteamId(900003)].refusal = "Steam lists no Windows program this console can start for this game.";
    simulation.SetGameSetups(setups);
    rt::EffectiveSettings values;
    check(simulation.RecipeFor(SteamId(900003)).id == draft.id && simulation.EffectiveSettingsFor(SteamId(900003), &values) && !values.launch,
          "T52 a refused game's sheet has no program until the player picks one");
    check(simulation.known_problem(SteamId(900003)), "T52 a refused game shows Known problems");
}

void type_into(playden::homebrew::Simulation& simulation, const std::string& key, const std::string& text) {
    activate(simulation, key);
    check(simulation.state().editor.active, "sign-in field opens the keyboard");
    simulation.action("clear");
    simulation.text(text);
    simulation.action("done");
}

void t15_password_sign_in(const std::filesystem::path& fixtures, const std::filesystem::path& profile) {
    using playden::homebrew::SignInStep;
    playden::homebrew::Simulation simulation(fixtures, profile);
    simulation.scenario("signed-out");
    simulation.action("cross");
    check(simulation.state().sign_in.step == SignInStep::Qr, "T15 the QR code is the default method");
    check(find_row(simulation, "signin-password-mode") != nullptr, "T15 the QR screen offers the password form");
    activate(simulation, "signin-password-mode");
    check(simulation.state().sign_in.step == SignInStep::Password && find_row(simulation, "signin-account") &&
              find_row(simulation, "signin-password") && find_row(simulation, "signin-qr-mode"),
          "T15 the password form has its fields and the way back");
    check(!find_row(simulation, "signin-submit")->enabled, "T15 Sign in waits for both fields");
    type_into(simulation, "signin-account", "email");
    check(!find_row(simulation, "signin-submit")->enabled, "T15 Sign in still waits for the password");

    // The password shows as dots in the form, the keyboard and the snapshot; Show reveals it on the keyboard only.
    activate(simulation, "signin-password");
    check(simulation.state().editor.secret && !simulation.state().editor.revealed, "T15 the password field is secret");
    simulation.text("p\xc3\xa4ss");
    check(simulation.view().subtitle == "\u2022\u2022\u2022\u2022", "T15 the keyboard shows dots");
    check(simulation.snapshot().find("p\xc3\xa4ss") == std::string::npos &&
              simulation.snapshot().find("<redacted>") != std::string::npos,
          "T15 the snapshot redacts the password");
    for (int i = 0; i < 6; ++i) simulation.action("down");
    for (int i = 0; i < 9; ++i) simulation.action("left");
    for (int i = 0; i < 3; ++i) simulation.action("right");
    simulation.action("cross");
    check(simulation.state().editor.revealed && simulation.view().subtitle == "p\xc3\xa4ss", "T15 Show reveals the password");
    simulation.action("cross");
    check(!simulation.state().editor.revealed, "T15 Hide masks it again");
    simulation.action("right"); simulation.action("right");
    check(simulation.state().editor.focus_key == 5, "T15 a password's bottom row has six keys");
    simulation.action("done");
    check(find_row(simulation, "signin-password")->value == "\u2022\u2022\u2022\u2022", "T15 the form shows dots");
    simulation.action("r1");
    check(simulation.state().navigation.route == playden::homebrew::Route::SignIn &&
              !simulation.state().sign_in.password.empty(),
          "T15 R1 does not leave the password form");
    simulation.action("route:home");
    check(simulation.state().sign_in.password.empty(), "T15 leaving by route wipes the password");
    simulation.scenario("signed-out");
    simulation.action("cross");
    activate(simulation, "signin-password-mode");
    check(simulation.state().sign_in.account_name == "email", "T15 the account name stays");
    type_into(simulation, "signin-password", "p\xc3\xa4ss");
    check(simulation.snapshot().find("p\xc3\xa4ss") == std::string::npos, "T15 the form's password is not snapshotted");
    activate(simulation, "signin-password");
    check(!simulation.state().editor.revealed, "T15 reopening the field masks it");
    simulation.action("circle");
    check(find_row(simulation, "signin-submit")->enabled, "T15 Sign in is enabled with both fields");

    // Email code: a wrong code asks again; the right one signs in and is remembered.
    activate(simulation, "signin-submit");
    check(simulation.state().sign_in.step == SignInStep::EmailCode && simulation.state().sign_in.password.empty(),
          "T15 Steam asks for an email code and the password is gone");
    check(simulation.view().subtitle == "Enter the code sent to …@example.invalid.", "T15 the email prompt names the domain");
    type_into(simulation, "guard-code", "0 0-0000");
    check(simulation.state().sign_in.code == "00000", "T15 codes keep five letters or digits");
    activate(simulation, "guard-submit");
    check(simulation.state().sign_in.code_rejected && simulation.view().subtitle.rfind("That code was not right.", 0) == 0,
          "T15 a wrong code shows inline");
    type_into(simulation, "guard-code", "ab12c");
    activate(simulation, "guard-submit");
    check(simulation.state().account().authentication == playden::homebrew::AuthState::Syncing &&
              simulation.state().sign_in_password,
          "T15 the right code signs in and remembers the password method");

    // The next sign-in opens the form; the toggle goes back to QR.
    simulation.scenario("signed-out");
    simulation.action("focus:startup-connect"); simulation.action("cross");
    check(simulation.state().sign_in.step == SignInStep::Password, "T15 the remembered method opens");
    activate(simulation, "signin-qr-mode");
    check(simulation.state().sign_in.step == SignInStep::Qr &&
              simulation.state().account().authentication == playden::homebrew::AuthState::Waiting,
          "T15 Use QR code instead shows the QR code");
    activate(simulation, "signin-password-mode");

    // Authenticator code and approval with a code allowed.
    type_into(simulation, "signin-account", "mobile");
    type_into(simulation, "signin-password", "pw");
    activate(simulation, "signin-submit");
    check(simulation.state().sign_in.step == SignInStep::AuthenticatorCode &&
              simulation.view().subtitle == "Enter the code from the Steam Mobile app.",
          "T15 Steam asks for the app's code");
    activate(simulation, "guard-cancel");
    check(simulation.state().navigation.route == playden::homebrew::Route::Startup, "T15 Cancel leaves the sign-in");
    simulation.action("focus:startup-connect"); simulation.action("cross");
    type_into(simulation, "signin-account", "approve");
    type_into(simulation, "signin-password", "pw");
    activate(simulation, "signin-submit");
    check(simulation.state().sign_in.step == SignInStep::Approval && find_row(simulation, "guard-use-code") &&
              simulation.view().subtitle == "Approve the sign-in in the Steam Mobile app.",
          "T15 approval waits, with a code allowed");
    activate(simulation, "guard-use-code");
    check(find_row(simulation, "guard-code") && simulation.state().navigation.focus_key == "guard-code",
          "T15 Type a code instead opens the code field");

    // Cancel clears the password; a refused password returns to the form with the reason.
    simulation.action("circle");
    simulation.action("focus:startup-connect"); simulation.action("cross");
    type_into(simulation, "signin-account", "wrong");
    type_into(simulation, "signin-password", "secret");
    simulation.action("circle");
    check(simulation.state().sign_in.password.empty(), "T15 Cancel clears the password");
    simulation.action("focus:startup-connect"); simulation.action("cross");
    check(find_row(simulation, "signin-password")->value.empty() &&
              simulation.state().sign_in.account_name == "wrong",
          "T15 the form comes back with the name only");
    type_into(simulation, "signin-password", "secret");
    activate(simulation, "signin-submit");
    check(simulation.state().sign_in.step == SignInStep::Password && simulation.state().sign_in.password.empty() &&
              simulation.view().subtitle.find("did not accept") != std::string::npos,
          "T15 a refused password shows why");
}

// The PS5's system keyboard: each opened field is offered once, and its accepted text commits like Done.
void t16_system_keyboard_text(const std::filesystem::path& fixtures, const std::filesystem::path& profile) {
    using playden::homebrew::SignInStep;
    playden::homebrew::Simulation simulation(fixtures, profile);
    simulation.scenario("signed-out");
    simulation.action("cross");
    activate(simulation, "signin-password-mode");
    activate(simulation, "signin-account");
    const auto first = simulation.state().editor.serial;
    simulation.SubmitEditorText("email");
    check(!simulation.state().editor.active && simulation.state().sign_in.account_name == "email" &&
              simulation.state().navigation.focus_key == "signin-password",
          "T16 accepted text commits the field");
    activate(simulation, "signin-password");
    check(simulation.state().editor.serial != first && simulation.state().editor.secret,
          "T16 each field opens with a new serial and says it is secret");
    simulation.SubmitEditorText("hunter2");
    check(simulation.state().sign_in.password == "hunter2" && simulation.state().editor.value.empty(),
          "T16 the password is taken and the editor keeps no copy");
    activate(simulation, "signin-submit");
    activate(simulation, "guard-code");
    simulation.SubmitEditorText("ab 12c9");
    check(simulation.state().sign_in.code == "AB12C", "T16 codes are normalized from the system keyboard too");
    simulation.SubmitEditorText("ignored");
    check(simulation.state().sign_in.code == "AB12C", "T16 text with no open field is ignored");
}

// T17: the Import sheet opens from Startup, Home's empty state and Settings › Storage; it lists the
// drives' folders by drive with their size, says what to do when there are none, shows the scan,
// and adds a game from its program list.
static void t17_import_sheet(const std::filesystem::path& fixtures, const std::filesystem::path& profile) {
    using playden::homebrew::RowKind;
    using playden::homebrew::Route;
    playden::homebrew::Simulation simulation(fixtures, profile);
    const auto row = [&](const std::string& key) -> const playden::homebrew::ViewRow* {
        for (const auto& item : simulation.view().rows) if (item.key == key) return &item;
        return nullptr;
    };
    const auto keys = [&] {
        std::vector<std::string> result;
        for (const auto& item : simulation.view().rows) result.push_back(item.key);
        return result;
    };
    const auto press = [&](const std::string& key) {
        check(simulation.action("focus:" + key) && simulation.action("cross"), "T17 row activates");
    };
    // Startup: Import games sits between Connect Steam and Skip for now, and opens the sheet.
    simulation.scenario("signed-out");
    check((keys() == std::vector<std::string>{"startup-connect", "startup-import", "startup-skip"}),
          "T17 Startup offers Import games under Connect Steam");
    press("startup-import");
    check(simulation.view().surface == "import-games" && simulation.state().navigation.route == Route::Startup &&
              simulation.state().account().authentication == playden::homebrew::AuthState::SignedOut,
          "T17 Import games opens the sheet, not the sign-in");
    simulation.action("circle");
    check(simulation.state().navigation.modals.empty() && simulation.state().navigation.focus_key == "startup-import",
          "T17 Circle returns to Startup's Import games");
    // Home's empty state.
    press("startup-skip");
    check(row("home-import") && row("home-import")->title == "Import games", "T17 Home's empty state offers Import games");
    press("home-import");
    check(simulation.view().surface == "import-games", "T17 Home opens the sheet");

    // Empty: the instructions, Look for games, and a scan that fills the list.
    check(simulation.scenario("import-empty"), "T17 import-empty scenario");
    check(simulation.view().surface == "import-games" && simulation.state().navigation.route == Route::Settings,
          "T17 the scenario opens the sheet over Settings");
    check((keys() == std::vector<std::string>{"import-look", "cancel"}) &&
              simulation.view().subtitle.find("Copy game folders into playden/import on an exFAT drive") == 0 &&
              simulation.state().navigation.focus_key == "import-look",
          "T17 the empty sheet says where to copy games");
    check(simulation.view().rows.back().title == "Close", "T17 the sheet closes with Close");
    simulation.action("cross");
    check(row("import-look")->title == "Looking for games…" && row("import-look")->kind == RowKind::Status &&
              !row("import-look")->enabled && simulation.state().import_scanning,
          "T17 Look for games shows the scan");
    simulation.tick(1);
    check(!simulation.state().import_scanning && simulation.state().import_candidates.size() == 6,
          "T17 the scan finds the fixture folders");
    check(simulation.state().navigation.focus_key == "import-look", "T17 the focus stays on Look for games");
    check(row("import-group-usb0") && row("import-group-usb0")->kind == RowKind::Heading &&
              row("import-group-usb0")->title == "USB drive" && row("import-group-m2")->title == "M.2 SSD" &&
              row("import-group-internal")->title == "Console drop folder" && !row("import-group-usb1"),
          "T17 folders are grouped by drive");
    simulation.action("down");
    check(simulation.state().navigation.focus_key == "import-candidate-usb0:playden/import/Night Sky",
          "T17 Down steps over the drive heading");
    simulation.action("down"); simulation.action("down"); simulation.action("down");
    check(simulation.state().navigation.focus_key == "import-candidate-m2:playden/import/Delores",
          "T17 Down steps over the next drive's heading too");
    simulation.action("up");
    check(simulation.state().navigation.focus_key == "import-candidate-usb0:playden/import/Café Étoile",
          "T17 Up steps back over it");
    simulation.action("circle");
    check(simulation.state().navigation.modals.empty() && simulation.state().navigation.focus_key == "settings-import" &&
              row("settings-import")->value == "6 found",
          "T17 Settings › Library counts the folders found");

    // Candidates: sizes, Added, copy only, no program.
    check(simulation.scenario("import-candidates"), "T17 import-candidates scenario");
    check(simulation.state().navigation.focus_key == "import-candidate-usb0:playden/import/Old School Rally",
          "T17 the focus starts on the first game not added");
    check(row("import-candidate-usb0:playden/import/Night Sky")->value == "Added", "T17 an added folder says Added");
    const auto* rally = row("import-candidate-usb0:playden/import/Old School Rally");
    check(rally->value == "938 MB" && rally->subtitle == "/mnt/usb0/playden/import/Old School Rally", "T17 size and path");
    check(row("import-candidate-usb0:playden/import/Café Étoile")->subtitle.find(" · Copy only") != std::string::npos,
          "T17 a non-ASCII path can only be copied");
    const auto* tools = row("import-candidate-internal:playden-import/Tools");
    check(!tools->enabled && tools->subtitle == "No Windows program found", "T17 a folder without a program can't be added");
    check(simulation.view().subtitle.find("Games found in playden/import") == 0, "T17 the sheet's subtitle with games");

    // Copy only, with one program besides a redistributable: no program list; step 1 says which
    // program starts the game, and why it can't play from its drive.
    press("import-candidate-usb0:playden/import/Café Étoile");
    check(simulation.view().surface == "import-choice" &&
              simulation.view().subtitle == "Starts with CafeEtoile.exe, which you can change in Game settings. "
                                            "You can import it into Playden later from the game's page." &&
              !row("import-play-in-place")->enabled &&
              row("import-play-in-place")->subtitle == "This folder's name has characters the PS5 can't start a game from. "
                                                       "Copy it into Playden instead.",
          "T17 one clear program skips the list; a copy-only folder says why");
    simulation.action("circle");
    check(simulation.view().surface == "import-games", "T17 Back returns to the sheet");

    // Add: the program list, then the folder is in the library.
    press("import-candidate-usb0:playden/import/Old School Rally");
    check(simulation.view().surface == "import-programs" && simulation.view().title == "Old School Rally" &&
              row("import-program-0")->title == "OldSchoolRally.exe" && row("import-program-0")->subtitle.empty() &&
              row("import-program-1")->title == "RallyBench.exe" && row("import-program-1")->subtitle == "Benchmark" &&
              row("import-program-2")->subtitle.find("Installer or tool") != std::string::npos && simulation.view().rows.back().title == "Back" &&
              simulation.state().navigation.focus_key == "import-program-0",
          "T17 the program list puts the game first and labels helpers");
    simulation.action("cross");
    check(simulation.view().surface == "import-choice" && simulation.view().title == "Where will the game files live?" &&
              simulation.view().eyebrow == "Import games · Old School Rally" &&
              simulation.view().subtitle == "You can import it into Playden later from the game's page." &&
              row("import-play-in-place")->title == "Play from the USB drive" &&
              row("import-play-in-place")->subtitle == "Nothing is copied. Keep the drive plugged in." &&
              row("import-copy")->title == "Copy to storage…" && row("import-copy")->value == "938 MB" &&
              row("import-copy")->subtitle == "Copies the game. You pick the storage next." &&
              simulation.state().navigation.focus_key == "import-copy",
          "T17 choosing the program asks where the game files live, Import focused");
    press("import-play-in-place");
    check(simulation.state().navigation.route == Route::Details && simulation.state().navigation.modals.empty() &&
              simulation.state().navigation.detail_id && simulation.state().navigation.detail_id->store == playden::Store::kLocal,
          "T17 playing from the drive adds the game and opens its page");
    const auto& games = simulation.state().games;
    check(std::count_if(games.begin(), games.end(), [](const playden::homebrew::Game& game) {
              return game.id.store == playden::Store::kLocal; }) == 2,
          "T17 the library has both imported games");
    // An added folder opens its game's page.
    check(simulation.scenario("import-candidates"), "T17 import-candidates scenario again");
    press("import-candidate-usb0:playden/import/Night Sky");
    check(simulation.state().navigation.route == Route::Details && simulation.state().navigation.modals.empty() &&
              simulation.state().navigation.detail_id &&
              simulation.state().navigation.detail_id->store == playden::Store::kLocal,
          "T17 an added folder opens its game");
}

// T18: an imported game's page (Store "Imported", its drive, Copy and Remove from library), the
// copy's progress, removing it from the library, and a drop-folder game moved into Playden.
static void t18_imported_game_page(const std::filesystem::path& fixtures, const std::filesystem::path& profile) {
    using playden::homebrew::Route;
    playden::homebrew::Simulation simulation(fixtures, profile);
    const auto row = [&](const std::string& key) -> const playden::homebrew::ViewRow* {
        for (const auto& item : simulation.view().rows) if (item.key == key) return &item;
        return nullptr;
    };
    const auto press = [&](const std::string& key) {
        check(simulation.action("focus:" + key) && simulation.action("cross"), "T18 row activates");
    };
    check(simulation.scenario("import-details"), "T18 import-details scenario");
    check(simulation.state().navigation.route == Route::Details && row("details-play") && row("details-play")->title == "Play" &&
              row("details-copy")->title == "Copy to storage…" && row("details-remove")->title == "Remove from library…" &&
              row("details-folder")->title == "Plays from /mnt/usb0/playden/import/Night Sky" && !row("details-cloud"),
          "T18 an imported game's page plays in place and offers Import and Remove");
    // Copy to storage: the wizard's drive choice at once; Back closes it.
    press("details-copy");
    check(simulation.view().surface == "import-storage" && simulation.view().title == "Choose storage" &&
              simulation.view().subtitle == "412 MB will be copied there." &&
              simulation.view().eyebrow == "Import games · Night Sky" &&
              row("volume-external")->subtitle == "Default · Available" && row("volume-external")->enabled &&
              row("volume-internal")->subtitle == "Available" && row("volume-internal")->enabled &&
              !row("volume-readonly")->enabled && row("volume-readonly")->subtitle == "No write access" &&
              simulation.state().navigation.focus_key == "volume-external" &&
              simulation.view().legends.front() == "Cross Import here" && simulation.view().rows.back().title == "Back",
          "T18 Copy to storage from the page opens the drive choice, the default drive focused");
    simulation.action("circle");
    check(simulation.state().navigation.modals.empty() && simulation.state().navigation.route == Route::Details,
          "T18 Back from the page's drive choice closes it");
    press("details-copy");
    press("volume-readonly");
    check(simulation.view().surface == "import-storage" && simulation.state().jobs.back().game_id.store != playden::Store::kLocal,
          "T18 a drive that can't take the game does nothing");
    press("volume-internal");
    check(simulation.state().navigation.modals.empty() && simulation.state().jobs.back().volume_id == "internal" &&
              row("details-download") && row("details-download")->title == "Copying" && !row("details-copy"),
          "T18 the copy runs on the chosen drive and shows on the page");
    check(simulation.scenario("import-copying"), "T18 import-copying scenario");
    check(row("details-download")->progress > 0.39F && row("details-download")->progress < 0.41F, "T18 the copy is 40% done");
    // Cancel copy: asks, then the game plays in place again.
    check(row("details-cancel-copy") && row("details-remove") && !row("details-remove")->enabled, "T18 a copy offers Cancel copy");
    press("details-cancel-copy");
    check(simulation.view().surface == "confirmation", "T18 Cancel copy asks first");
    press("confirm-destructive");
    check(row("details-copy") && !row("details-download") && !row("details-cancel-copy") && row("details-remove")->enabled,
          "T18 a cancelled copy returns the game to playing in place");

    // Remove from library: the folder stays, and can be added again.
    check(simulation.scenario("import-details"), "T18 import-details again");
    press("details-remove");
    check(simulation.view().surface == "confirmation" && simulation.state().navigation.focus_key == "cancel" &&
              simulation.state().navigation.modals.back().body.find("stay where they are") != std::string::npos,
          "T18 Remove from library asks first and says the files stay");
    press("confirm-destructive");
    check(simulation.state().navigation.route == Route::Library && simulation.state().navigation.modals.empty() &&
              !simulation.state().import_candidates.front().imported.valid() &&
              std::none_of(simulation.state().games.begin(), simulation.state().games.end(),
                           [](const playden::homebrew::Game& game) { return game.title == "Night Sky"; }),
          "T18 the game leaves the library and its folder can be added again");

    // The drop folder: step 1, then console storage moves the folder at once.
    check(simulation.scenario("import-candidates"), "T18 import-candidates scenario");
    press("import-candidate-internal:playden-import/Hollow Knight");
    check(row("import-play-in-place")->title == "Play from the drop folder" &&
              row("import-play-in-place")->subtitle == "Nothing moves. The files stay in /data/playden-import." &&
              row("import-copy")->title == "Move to storage…" && row("import-copy")->value == "8.9 GB" &&
              row("import-copy")->subtitle == "Takes the game out of the drop folder. You pick the storage next." &&
              !row("import-move") && !row("storage-picker"),
          "T18 a drop-folder game plays from there or is imported");
    press("import-copy");
    check(simulation.view().surface == "import-storage" && simulation.view().subtitle == "8.9 GB. The game leaves the drop folder." &&
              row("volume-internal")->subtitle == "Instant · moves the folder" && row("volume-internal")->enabled &&
              row("volume-external")->subtitle == "Copies, then frees the drop folder",
          "T18 the drop folder's drive choice says what each drive does");
    simulation.action("circle");
    check(simulation.view().surface == "import-choice" && simulation.state().navigation.focus_key == "import-copy",
          "T18 Back from step 2 returns to step 1");
    press("import-copy");
    press("volume-internal");
    const auto id = simulation.state().navigation.detail_id.value_or(playden::GameId{});
    check(simulation.state().navigation.route == Route::Details && id.store == playden::Store::kLocal &&
              row("details-uninstall") && !row("details-remove") && !row("details-copy"),
          "T18 a moved game is in Playden at once and uninstalls");
    // To another drive (a fresh console): a copy, playing from the drop folder until it is done.
    playden::homebrew::Simulation fresh(fixtures, profile);
    const auto press_fresh = [&](const std::string& key) {
        check(fresh.action("focus:" + key) && fresh.action("cross"), "T18 row activates");
    };
    check(fresh.scenario("import-candidates"), "T18 import-candidates scenario on a fresh console");
    press_fresh("import-candidate-internal:playden-import/Hollow Knight");
    press_fresh("import-copy");
    press_fresh("volume-external");
    const auto copied = fresh.state().navigation.detail_id.value_or(playden::GameId{});
    check(fresh.state().navigation.route == Route::Details && copied.store == playden::Store::kLocal &&
              !fresh.state().jobs.empty() && fresh.state().jobs.back().game_id == copied &&
              fresh.state().jobs.back().volume_id == "external" && !fresh.state().external_games.empty(),
          "T18 a drop-folder game imported to another drive is copied there and plays in place meanwhile");
}


void synthetic_download_servers(const std::filesystem::path& fixtures, const std::filesystem::path& profile) {
    using playden::homebrew::Simulation;
    Simulation simulation(fixtures, profile);
    check(simulation.scenario("download-servers"), "the download-server scenario exists");
    const auto row = [&simulation](const std::string& key) -> const playden::homebrew::ViewRow* {
        for (const auto& item : simulation.view().rows) if (item.key == key) return &item;
        return nullptr;
    };
    const auto press = [&simulation](const std::string& key) {
        check(simulation.action("focus:" + key) && simulation.action("cross"), "synthetic server action exists");
    };
    check(row("settings-download-server") && row("settings-download-server")->value == "Steam default",
          "the synthetic scenario shows the Settings row");
    press("settings-download-server");
    check(row("download-server-6") && !row("download-server-7") && row("download-server-3")->subtitle == "HTTP" &&
              row("download-server-5")->title == "Steam (Valve) · cache5-fra1",
          "seven servers are shown, with HTTP and the host of repeated providers marked");
    press("download-server-test");
    check(row("download-server-test")->title == "Testing Akamai (1 of 7)…", "synthetic test starts with the first provider");
    simulation.tick(1);
    check(row("download-server-test")->title == "Testing Fastly (2 of 7)…" &&
              row("download-server-0")->value == "12.4 MB/s", "synthetic progress produces rates");
    simulation.action("circle");
    check(!simulation.state().navigation.modals.empty(), "Circle cancels the synthetic test first");
    simulation.action("circle");
    check(simulation.state().navigation.modals.empty(), "the next Circle closes the picker");
    press("settings-download-server");
    press("download-server-test");
    simulation.tick(7);
    check(row("download-server-1")->value == "41.2 MB/s" && row("download-server-1")->badge == "Fastest" &&
              row("download-server-3")->value == "Failed" && simulation.state().navigation.focus_key == "download-server-1" &&
              simulation.state().download_server.empty(), "synthetic completion focuses the fastest without choosing it");
    simulation.action("cross");
    check(simulation.state().download_server == "fastly.cdn.steampipe.steamcontent.com" &&
              simulation.TakeDownloadServerRequests().empty(), "the synthetic choice is saved without service requests");
    Simulation restarted(fixtures, profile);
    restarted.scenario("download-servers");
    check(restarted.state().download_server == simulation.state().download_server,
          "the simulation profile keeps the choice across restarts");
    check(restarted.action("focus:settings-download-server") && restarted.action("cross") &&
              restarted.state().navigation.focus_key == "download-server-1" &&
              restarted.action("focus:download-server-fastest") && restarted.action("cross"),
          "the synthetic picker offers Fastest");
    check(restarted.state().download_server == "fastest" && restarted.state().navigation.modals.empty(),
          "the synthetic Fastest choice is saved");
    for (const auto& item : restarted.view().rows)
        if (item.key == "settings-download-server") check(item.value == "Fastest", "the row shows Fastest");
    // Queue: the download names its server before the rate.
    // Queue: a game Playden refused to prepare offers Uninstall beside Try again.
    check(restarted.scenario("queue-refused"), "refused preparation scenario exists");
    {
        const auto& modals = restarted.state().navigation.modals;
        check(!modals.empty() && modals.back().id == "prep-actions" &&
                  std::find(modals.back().action_keys.begin(), modals.back().action_keys.end(), "uninstall") !=
                      modals.back().action_keys.end(),
              "the refused game's actions include Uninstall");
        check(restarted.action("focus:uninstall") && restarted.action("cross") &&
                  restarted.state().navigation.modals.back().id == "confirmation",
              "Uninstall asks first");
    }
    check(restarted.scenario("queue-one-download"), "queue scenario exists");
    check(restarted.state().jobs.front().server_name == "Fastly" &&
              playden::homebrew::FormatDownloadRate(restarted.state().jobs.front().bytes_per_second,
                                                    restarted.state().jobs.front().eta_seconds,
                                                    restarted.state().jobs.front().server_name) ==
                  "Fastly · 18.2 MB/s · ~3 min left",
          "the Queue row names the download server");
}

void leftover_titles_simulation_uses_controller_navigation(const std::filesystem::path& fixtures,
                                                          const std::filesystem::path& profile) {
    using namespace playden::homebrew;
    Simulation simulation(fixtures, profile);
    check(simulation.scenario("web-access"), "web access scenario exists");
    check(simulation.view().focused_row >= 0 &&
              simulation.view().rows[static_cast<std::size_t>(simulation.view().focused_row)].key == "settings-web-access",
          "the web access scenario opens Settings on Web access");
    {
        bool qr = false;
        for (const auto& row : simulation.view().rows) qr = qr || row.qr == "http://192.168.1.50:7031/";
        check(qr, "the simulation shows a made-up address with its QR code");
    }
    check(simulation.scenario("leftover-titles"), "leftover title scenario exists");
    check(simulation.state().leftover_titles.size() == 2 &&
              simulation.state().navigation.focus_key == "settings-leftovers", "the Leftover PS5 titles row starts focused");
    simulation.action("cross");
    check(simulation.state().navigation.focus_key == "leftover-select-all", "the manager opens on Select all");
    simulation.action("down");
    simulation.action("down");
    check(simulation.state().navigation.focus_key == "leftover-toggle-FAKE78002", "Down reaches the second title");
    simulation.action("cross");
    simulation.action("focus:leftover-selection-delete");
    simulation.action("cross");
    check(simulation.state().navigation.focus_key == "cancel", "deletion starts on Cancel");
    simulation.action("circle");
    check(simulation.state().navigation.modals.back().id == "leftover-titles" &&
              simulation.state().leftover_deletions.empty(), "Circle returns to the manager without deleting");
    simulation.action("focus:leftover-selection-delete");
    simulation.action("cross");
    simulation.action("focus:confirm-destructive");
    simulation.action("cross");
    simulation.action("l1");
    simulation.action("l1");
    check(simulation.state().navigation.route == Route::Downloads, "L1 reaches Queue");
    simulation.tick(1);
    check(simulation.state().leftover_deletions.front().step == "Removing PS5 title…", "Queue starts title removal");
    simulation.tick(5);
    check(simulation.state().leftover_titles.size() == 1 && simulation.state().leftover_titles.front().title_id == "FAKE78001" &&
              simulation.state().leftover_deletions.empty(), "only the confirmed title is removed");
}

// Community recipes (docs/community-recipes.md): the chooser's order, one base per game, the switch on
// an installed game, the Install sheet's Settings row, an updated recipe and the fallback reason.
std::string rev(char digit) { return std::string(64, digit); }

bool has_row(const playden::homebrew::Simulation& simulation, const std::string& key) { return find_row(simulation, key) != nullptr; }

void community_recipes_chooser(const std::filesystem::path& fixtures, const std::filesystem::path& profile) {
    namespace rt = playden::runtime;
    using playden::homebrew::CommunityRecipeOffer;
    playden::homebrew::Simulation simulation(fixtures, profile);
    load_settings_data(simulation, fixtures);
    check(simulation.scenario("recipes-three"), "recipes-three scenario loads");
    // Works before playable, then by votes (up − down): 14-1 before 3-0.
    const auto offers = simulation.CommunityOffersFor(SteamId(900002));
    check(offers.size() == 3 && offers[0].title == "120 Hz, pointer on left stick" && offers[1].title == "Gamepad layout" &&
              offers[2].title == "Windowed 1080p", "the chooser sorts by status, then votes");
    const auto& keys = simulation.state().navigation.modals.back().action_keys;
    check(keys.size() >= 8 && keys[0] == "preset-playden-default" && keys[1] == "chooser-heading-community" &&
              keys[2] == "recipe-" + rev('1') && keys[3] == "recipe-" + rev('2') && keys[4] == "recipe-" + rev('3') &&
              keys[5] == "recipes-search" && keys[6] == "chooser-heading-presets" && keys[7] == "preset-mouse-driven",
          "Playden default, the recipes in order, search by name, then the built-in presets");
    // Broken after playable, untested last; equal status and votes by title.
    playden::homebrew::CommunityCatalog catalog = simulation.state().community;
    auto make = [](std::string title, std::string status, std::uint32_t up, std::uint32_t down, char digit) {
        CommunityRecipeOffer offer;
        offer.id = "community-steam-900002-" + std::string(1, digit);
        offer.title = std::move(title);
        offer.status = std::move(status);
        offer.revision = rev(digit);
        offer.has_votes = up + down > 0;
        offer.votes_up = up;
        offer.votes_down = down;
        return offer;
    };
    catalog.games[playden::GameRef::Steam(900002)] = {make("Untested", "untested", 50, 0, 'a'), make("Broken", "broken", 9, 0, 'b'),
                                       make("Zeta", "works", 2, 1, 'c'), make("Alpha", "works", 2, 1, 'd'),
                                       make("Playable", "playable", 0, 0, 'e'), make("Liked", "works", 10, 0, 'f')};
    simulation.UpdateCommunityCatalog(catalog);
    const auto sorted = simulation.CommunityOffersFor(SteamId(900002));
    std::vector<std::string> titles;
    for (const auto& offer : sorted) titles.push_back(offer.title);
    check((titles == std::vector<std::string>{"Liked", "Alpha", "Zeta", "Playable", "Broken", "Untested"}),
          "works, playable, broken, untested; then votes; then title");

    // A recipe row: status, votes and author only; the side panel has where it was tested and what it changes.
    check(simulation.scenario("recipes-three"), "recipes-three scenario loads again");
    const auto* row = find_row(simulation, "recipe-" + rev('1'));
    check(row && row->title == "120 Hz, pointer on left stick" && row->badge == "Works · Verified" && row->value == "👍 14  👎 1" &&
              row->subtitle == "By leolobato \u2605" && row->author == "leolobato" && row->author_style == "trusted" &&
              row->lines.empty(),
          "a recipe row shows its status, Verified, votes and author; a trusted account's name has a star");
    {
        // The player's own vote shows in the counts until the API's totals have it (summary.json is cached and the copy
        // refreshes once a day), and a changed vote moves from one thumb to the other; never counted twice.
        const auto recipe_id = simulation.CommunityOffersFor(SteamId(900002)).front().id;
        auto dated = simulation.state().community;
        dated.votes_generated_at = 1'000;
        simulation.UpdateCommunityCatalog(dated);
        playden::community::VoteState memory;
        playden::community::CastVote first{recipe_id, rev('1'), "up", ""};
        first.at = 900;  // before the totals: in them
        memory.Record(first);
        simulation.UpdateVoteMemory(memory);
        check(find_row(simulation, "recipe-" + rev('1'))->value == "👍 14  👎 1", "a vote the totals have is not counted again");
        playden::community::CastVote changed{recipe_id, rev('1'), "down", "slow"};
        changed.at = 2'000;  // after the totals
        memory.Record(changed);
        simulation.UpdateVoteMemory(memory);
        check(find_row(simulation, "recipe-" + rev('1'))->value == "👍 13  👎 2",
              "a changed vote moves to the other thumb until the totals have it");
        check(simulation.CommunityOffersFor(SteamId(900002)).front().votes_down == 2, "the chooser's offers count it too");
        playden::community::VoteState fresh;
        playden::community::CastVote later{recipe_id, rev('1'), "up", ""};
        later.at = 2'000;
        fresh.Record(later);
        simulation.UpdateVoteMemory(fresh);
        check(find_row(simulation, "recipe-" + rev('1'))->value == "👍 15  👎 1", "a new vote is added to the totals");
        dated.votes_generated_at = 3'000;
        simulation.UpdateCommunityCatalog(dated);
        check(find_row(simulation, "recipe-" + rev('1'))->value == "👍 14  👎 1", "totals made after the vote already have it");
        simulation.UpdateVoteMemory({});
        check(simulation.scenario("recipes-three"), "recipes-three scenario loads once more");
    }
    check(find_row(simulation, "recipe-" + rev('2'))->subtitle == "By rina" &&
              find_row(simulation, "recipe-" + rev('2'))->author_style.empty() &&
              find_row(simulation, "recipe-" + rev('2'))->badge == "Works",
          "a typed name has no account colour, an unverified recipe no Verified");
    check(find_row(simulation, "recipe-" + rev('3'))->subtitle == "By tomas" &&
              find_row(simulation, "recipe-" + rev('3'))->author_style == "account",
          "an account's name is drawn in the account colour, without words for it or a star");
    simulation.action("focus:recipe-" + rev('1'));
    {
        const auto& comparison = simulation.view().comparison;
        check(comparison && comparison->column == "This recipe" &&
                  comparison->hint == "Verified · Community recipe by leolobato \u2605 · tested on runtime kit 0.12, firmware 10.01, 2026-10-20",
              "the side panel says where the focused recipe was tested");
        check(comparison && std::any_of(comparison->rows.begin(), comparison->rows.end(),
                                        [](const std::array<std::string, 3>& line) { return line[2] == "3840×2160"; }),
              "the side panel lists what the recipe changes");
    }
    simulation.action("focus:recipe-" + rev('3'));
    check(simulation.view().comparison &&
              std::any_of(simulation.view().comparison->starts.begin(), simulation.view().comparison->starts.end(),
                          [](const std::string& line) { return line.find("d3d9=n,b") != std::string::npos; }),
          "library overrides are listed apart, under Changes how the game starts");

    // A recipe the kit can't run stays listed, disabled, with the reason.
    check(simulation.scenario("recipes-refused"), "recipes-refused scenario loads");
    const auto* refused = find_row(simulation, "recipe-" + rev('5'));
    check(refused && !refused->enabled && !refused->lines.empty() && refused->lines.back() == "⚠ The recipe needs runtime kit version 0.13 or newer.",
          "a recipe that needs a newer kit is greyed out with the reason");
    simulation.action("focus:recipe-" + rev('5'));
    {
        const auto& comparison = simulation.view().comparison;
        check(comparison && comparison->note == "⚠ The recipe needs runtime kit version 0.13 or newer." &&
                  std::any_of(comparison->rows.begin(), comparison->rows.end(),
                              [](const std::array<std::string, 3>& line) { return line[2].find("3840×2160") != std::string::npos; }),
              "a refused recipe's comparison shows its own values, the refusal apart");
    }
    simulation.action("cross");
    check(simulation.state().navigation.modals.back().id == "preset-picker" &&
              !simulation.state().game_settings.count(SteamId(900005)),
          "a refused recipe can't be chosen");

    // A game without recipes, and the offline copy.
    check(simulation.scenario("recipes-none") && has_row(simulation, "recipes-none"), "a game with no recipes says so");
    check(simulation.scenario("recipes-offline"), "recipes-offline scenario loads");
    check(simulation.view().subtitle.find("Offline: community recipes as of 2026-10-18.") != std::string::npos &&
              has_row(simulation, "recipe-" + rev('1')),
          "an offline copy is still offered, with its date");
}

// An imported game gets the recipes of its Steam app ID; without one it gets none (no matching by name), and it still
// shares and exports: under its Steam app ID (made_on imported), else under the name key of its title.
// Search by game name and linking (docs/community-recipes.md): only games with recipes, by their names' words; another
// game's recipe links that game (saved), its updates show, Unlink keeps the choice; a recipe whose program isn't in this
// copy starts Playden's own, with a note.
void community_recipes_search_and_link(const std::filesystem::path& fixtures, const std::filesystem::path& profile) {
    namespace rt = playden::runtime;
    using playden::GameRef;
    const auto night_sky = playden::GameId::Local("6d1f3a52-0c55-4f3e-9b2a-5d1c7e0a9f11");
    const GameRef gog_night{"gog", "1453375253"}, imported_night{"imported", "night-sky"}, gog_creme{"gog", "1207658924"};
    const auto top = [](playden::homebrew::Simulation& simulation) -> const playden::homebrew::ModalEntry& {
        return simulation.state().navigation.modals.back();
    };
    const auto keys_of = [&](playden::homebrew::Simulation& simulation) { return top(simulation).action_keys; };
    const auto listed = [&](playden::homebrew::Simulation& simulation, const std::string& key) {
        const auto keys = keys_of(simulation);
        return std::find(keys.begin(), keys.end(), key) != keys.end();
    };
    playden::homebrew::Simulation simulation(fixtures, profile);
    load_settings_data(simulation, fixtures);
    check(simulation.scenario("recipes-imported-none"), "recipes-imported-none scenario loads");
    // No Steam app ID: none of its own, and the chooser offers the search.
    check(simulation.CommunityOffersFor(night_sky).empty() && has_row(simulation, "recipes-none") && has_row(simulation, "recipes-search") &&
              find_row(simulation, "recipes-none")->subtitle.find("Search by name") != std::string::npos,
          "an imported game without a Steam app ID has none of its own and is offered the search");
    // The rule: each query word begins a word of the name; only games with recipes; the game's own ref left out.
    auto results = simulation.SearchRecipeGames(night_sky, "night sky");
    check(results.size() == 2 && results[0].game == gog_night && results[0].recipes == 2 && results[0].name == "Night Sky" &&
              results[1].game == imported_night && results[1].recipes == 1,
          "the search finds the games named Night Sky, with their store and recipe count");
    check(simulation.SearchRecipeGames(night_sky, "NIGHT").size() == 3 && simulation.SearchRecipeGames(night_sky, "ight").empty() &&
              simulation.SearchRecipeGames(night_sky, "night sky deluxe").empty(),
          "every query word must begin a word of the name");
    const auto creme = simulation.SearchRecipeGames(SteamId(900002), "creme brulee");
    check(creme.size() == 1 && creme[0].game == gog_creme, "the searching game's own recipes are not a result");
    {
        auto catalog = simulation.state().community;
        catalog.names[GameRef{"gog", "99"}] = "Night Sky Without Recipes";
        catalog.games[GameRef{"gog", "99"}] = {};
        simulation.UpdateCommunityCatalog(catalog);
        check(simulation.SearchRecipeGames(night_sky, "night sky").size() == 2, "a game without recipes is never found");
    }
    // The keyboard opens prefilled with the game's name; Done lists the results.
    activate(simulation, "recipes-search");
    check(simulation.state().editor.active && simulation.state().editor.purpose == "Search recipes" &&
              simulation.state().editor.value == "Night Sky",
          "the search opens the keyboard with the game's name");
    simulation.SubmitEditorText("night sky");
    check(!simulation.state().editor.active && top(simulation).id == "preset-picker" &&
              listed(simulation, "search-game-gog/1453375253") && listed(simulation, "search-game-imported/night-sky") &&
              simulation.state().navigation.focus_key == "search-game-gog/1453375253" &&
              find_row(simulation, "search-game-gog/1453375253")->subtitle == "GOG · 2 recipes",
          "the results name each game, its store and its recipe count");
    check(find_row(simulation, "search-game-gog/1453375253")->tone.empty(),
          "a game in the results is no player's recipe: no Community tag");
    simulation.action("circle");
    check(top(simulation).id == "preset-picker" && listed(simulation, "recipes-none"), "Circle leaves the results for the game's own");
    activate(simulation, "recipes-search");
    simulation.SubmitEditorText("zzz");
    check(listed(simulation, "search-none") && simulation.state().navigation.focus_key == "recipes-search", "nothing found says so");
    check(find_row(simulation, "search-none")->title == "No game found", "nothing found names no game");
    check(simulation.SearchMatchesOwnGame(SteamId(900002), "creme") && !simulation.SearchMatchesOwnGame(SteamId(900002), "zzz") &&
              !simulation.SearchMatchesOwnGame(night_sky, "night sky"),
          "the search knows when the game's own recipes match, so an empty result can say they're on the first page");
    activate(simulation, "recipes-search");
    simulation.SubmitEditorText("Night Sky");
    // One result's recipes, with the program note for the one whose program isn't in this copy.
    activate(simulation, "search-game-gog/1453375253");
    check(listed(simulation, "recipe-" + rev('7')) && listed(simulation, "recipe-" + rev('8')) && !listed(simulation, "recipes-search"),
          "a result shows that game's recipes");
    check(simulation.ProgramNoteFor(night_sky, rev('7')) ==
              "Made for Night Sky · GOG: NightSky64.exe isn't in this copy, so Playden's program is used." &&
              simulation.ProgramNoteFor(night_sky, rev('8')).empty() && simulation.ProgramNoteFor(SteamId(900002), rev('1')).empty(),
          "another game's recipe whose program isn't in this copy gets the note; one whose program is, and own ones, don't");
    check(find_row(simulation, "recipe-" + rev('7'))->lines.back() == "NightSky64.exe isn't in this copy: Playden's program is used." &&
              simulation.view().comparison && simulation.view().comparison->note.rfind("Made for Night Sky", 0) == 0,
          "the row and the side panel say so");
    {
        // The side panel lists how the game starts here: Playden's program, said to be Playden's, not the recipe's.
        const auto& starts = simulation.view().comparison->starts;
        check(find_row(simulation, "recipe-" + rev('7'))->tone == "community" && !starts.empty() &&
                  starts.front() == "program: Game.exe, Playden's (NightSky64.exe isn't in this copy)" &&
                  std::none_of(starts.begin(), starts.end(),
                               [](const std::string& line) { return line.find("program: NightSky64.exe") != std::string::npos; }),
              "the side panel's program is the one used here, named as Playden's");
    }
    simulation.action("circle");
    check(simulation.state().navigation.focus_key == "search-game-gog/1453375253", "Circle goes back to the results, on that game");
    activate(simulation, "search-game-gog/1453375253");
    // Choosing it links the game: the choice names the game it is for, saved with the settings.
    activate(simulation, "recipe-" + rev('7'));
    const auto record = [&] { return simulation.state().game_settings.at(night_sky); };
    check(record().community_recipe.id == rt::CommunityRecipeIdFor(gog_night, "night-drive-pad") &&
              record().community_recipe.game == gog_night && record().linked_game == gog_night &&
              simulation.LinkedGameFor(night_sky) == gog_night,
          "choosing another game's recipe records that game and links it");
    check(simulation.RecipeFor(night_sky).launch_entries.front().executable == "Game.exe" &&
              simulation.RecipeFor(night_sky).defaults.count("pointer_stick"),
          "it applies with Playden's program and the recipe's settings");
    const auto preset_row = find_row(simulation, "preset-picker");
    check(preset_row && !preset_row->lines.empty() && preset_row->lines.back().rfind("Made for Night Sky · GOG: NightSky64.exe", 0) == 0,
          "the Preset row carries the note");
    {
        // Sharing it: another game's recipe is credited, never updated (the API refuses that across games): a new recipe
        // for this game, without a choice, changed or not.
        namespace cm = playden::community;
        const auto decision = simulation.ShareDecisionFor(night_sky);
        check(decision.kind == cm::ShareKind::kNewRecipe && decision.send_based_on && !decision.send_submission_id,
              "another game's recipe is shared as a new recipe for this game, crediting it");
        // The sheet offers no Suggest changes, and Send makes a new recipe for this game.
        if (!has_row(simulation, "share-recipe")) activate(simulation, "section-advanced");
        activate(simulation, "share-recipe");
        check(top(simulation).id == "share-recipe" && !has_row(simulation, "share-choice-update") &&
                  !has_row(simulation, "share-choice-new") && has_row(simulation, "share-send"),
              "the Share sheet doesn't ask to suggest changes to another game's recipe");
        activate(simulation, "share-send");
        if (has_row(simulation, "share-send-confirm")) activate(simulation, "share-send-confirm");
        check(simulation.state().game_settings.at(night_sky).shared_recipe.recipe_id.rfind("community-imported-night-sky-shared-", 0) == 0,
              "it is shared as a new recipe for this game");
        for (int i = 0; i < 4 && top(simulation).id != "game-settings"; ++i)
            if (has_row(simulation, "alert-ok")) activate(simulation, "alert-ok");
            else simulation.action("circle");
        check(top(simulation).id == "game-settings", "back to Game settings");
    }
    // The chooser lists the linked game's recipes after the game's own, with Unlink.
    activate(simulation, "preset-picker");
    check(listed(simulation, "chooser-heading-linked") && listed(simulation, "recipe-" + rev('8')) && listed(simulation, "recipes-unlink") &&
              simulation.state().navigation.focus_key == "recipe-" + rev('7'),
          "the linked game's recipes are listed, the chosen one focused");
    // A newer revision of the linked game's recipe shows as an update.
    {
        auto catalog = simulation.state().community;
        for (auto& offer : catalog.games.at(gog_night))
            if (offer.revision == rev('7')) offer.revision = rev('b');
        simulation.UpdateCommunityCatalog(catalog);
    }
    simulation.action("circle");
    check(has_row(simulation, "recipe-update"), "a newer revision of the linked game's recipe is an update");
    // Unlink: the game's own recipes only; the choice stays, and so does it in the runtime's eyes.
    activate(simulation, "preset-picker");
    activate(simulation, "recipes-unlink");
    check(!record().linked_game.valid() && record().community_recipe.game == gog_night && !listed(simulation, "chooser-heading-linked") &&
              listed(simulation, "recipe-" + rev('7')) && simulation.RecipeBaseFor(night_sky).has_value(),
          "Unlink drops the linked game's recipes, keeping the current choice");
    simulation.action("circle");
    check(!has_row(simulation, "recipe-update"), "an unlinked game's updates no longer show");
    // A choice of an unrelated game's recipe (no game recorded) is refused, as the runtime refuses it.
    check(simulation.RecipeFallbackFor(SteamId(900004)).empty(), "no choice, no fallback");
}

// A share saved before shares recorded their game counts as the Steam game's own (only Steam games could share then).
void community_old_share_is_the_steam_games(const std::filesystem::path& fixtures, const std::filesystem::path& profile) {
    namespace rt = playden::runtime;
    const auto night_sky = playden::GameId::Local("6d1f3a52-0c55-4f3e-9b2a-5d1c7e0a9f11");
    playden::homebrew::Simulation simulation(fixtures, profile);
    load_settings_data(simulation, fixtures);
    check(simulation.scenario("share-imported"), "share-imported scenario loads");
    // Saved before shares recorded their game: no "game" in shared_recipe.
    rt::GameSettingsRecord steam, imported;
    steam.overrides["resolution"] = "3840x2160";
    steam.shared_recipe = {"sub-old", "community-steam-900002-old", {}};
    imported.overrides["resolution"] = "3840x2160";
    imported.shared_recipe = {"sub-old-2", "community-imported-night-sky-old", {}};
    const auto settings_file = profile.string() + ".old-share.settings";
    std::string reason;
    check(rt::SaveGameSettingsFile(settings_file, {{SteamId(900002), steam}, {night_sky, imported}}, &reason), "seed old shares");
    simulation.SetGameSettingsFile(settings_file);
    check(!simulation.state().game_settings.at(SteamId(900002)).shared_recipe.game.valid(), "the old share reads back without a game");
    const auto decision = simulation.ShareDecisionFor(SteamId(900002));
    check(decision.kind == playden::community::ShareKind::kUpdateOwn && decision.send_submission_id,
          "an old share of a Steam game is the Steam game's: its next share updates it");
    const auto other = simulation.ShareDecisionFor(night_sky);
    check(other.kind == playden::community::ShareKind::kNewRecipe && !other.send_submission_id,
          "an old share of another game stays unknown: its next share is new");
    std::filesystem::remove(settings_file);
}

void community_recipes_imported(const std::filesystem::path& fixtures, const std::filesystem::path& profile) {
    namespace rt = playden::runtime;
    const auto night_sky = playden::GameId::Local("6d1f3a52-0c55-4f3e-9b2a-5d1c7e0a9f11");
    const auto top = [](playden::homebrew::Simulation& simulation) -> const playden::homebrew::ModalEntry& {
        return simulation.state().navigation.modals.back();
    };
    const auto directory = std::filesystem::temp_directory_path() / ("playden-imported-recipes-" + std::to_string(::getpid()));
    std::filesystem::remove_all(directory);
    const auto exported = [&](playden::homebrew::Simulation& simulation) {
        while (!simulation.state().navigation.modals.empty() && top(simulation).id != "game-settings") simulation.action("circle");
        if (!has_row(simulation, "export-recipe")) activate(simulation, "section-advanced");
        activate(simulation, "export-recipe");
        rt::CommunityRecipe community;
        std::string reason;
        const auto text = read_text(directory / ("recipe-" + playden::ToKey(night_sky) + ".json"));
        check(rt::ParseCommunityRecipe(text, &community, &reason), "the imported game's export reads back");
        return community;
    };
    {
        playden::homebrew::Simulation simulation(fixtures, profile);
        load_settings_data(simulation, fixtures);
        simulation.SetRecipeExportTarget(directory, "13.60");
        check(simulation.scenario("recipes-imported"), "recipes-imported scenario loads");
        // Matched by its Steam app ID (Game settings): 900002's three recipes, in the chooser.
        const auto offers = simulation.CommunityOffersFor(night_sky);
        check(offers.size() == 3 && offers[0].id == "community-steam-900002-120hz-left-stick-pointer",
              "an imported game gets the recipes of its Steam app ID");
        check(top(simulation).id == "preset-picker" && has_row(simulation, "recipe-" + rev('1')) &&
                  !has_row(simulation, "recipes-none"),
              "the chooser offers them");
        // Its share and export are for the Steam game, made on an imported copy.
        const auto community = exported(simulation);
        check(community.game == playden::GameRef::Steam(900002) && community.made_on == "imported" &&
                  community.game_name == "Night Sky" && community.recipe.id == "community-steam-900002",
              "an imported copy with a Steam app ID exports for that Steam game, made on imported");
        while (!simulation.state().navigation.modals.empty() && top(simulation).id != "game-settings") simulation.action("circle");
        check(has_row(simulation, "share-recipe"), "it can be shared");
    }
    {
        playden::homebrew::Simulation simulation(fixtures, profile);
        load_settings_data(simulation, fixtures);
        simulation.SetRecipeExportTarget(directory, "13.60");
        check(simulation.scenario("share-imported"), "share-imported scenario loads");
        // No Steam app ID: no recipes by name.
        check(simulation.CommunityOffersFor(night_sky).empty(), "without a Steam app ID an imported game matches nothing");
        check(top(simulation).id == "share-recipe", "an imported game without a Steam app ID shares");
        activate(simulation, "share-send");
        const auto shared = simulation.state().game_settings.at(night_sky).shared_recipe;
        check(shared.recipe_id.rfind("community-imported-night-sky-shared-", 0) == 0,
              "it is shared under the name key of its title");
        activate(simulation, "alert-ok");
        const auto community = exported(simulation);
        check(community.game == (playden::GameRef{"imported", "night-sky"}) && community.made_on.empty() &&
                  community.game_name == "Night Sky" && community.recipe.id == "community-imported-night-sky",
              "it exports under its name key");
        // The chooser says why there are none.
        while (!simulation.state().navigation.modals.empty() && top(simulation).id != "game-settings") simulation.action("circle");
        activate(simulation, "preset-picker");
        check(find_row(simulation, "recipes-none") &&
                  find_row(simulation, "recipes-none")->subtitle.find("by its Steam app ID") != std::string::npos,
              "the chooser says an imported game's recipes come by its Steam app ID");
        // The share was made for imported/night-sky; a Steam app ID from Game settings changes the game's ref, so the
        // next share is a new one, not an update of that submission.
        check(shared.game == (playden::GameRef{"imported", "night-sky"}), "a saved share records the game it was made for");
        const auto set_app = [&](const char* text) {
            while (!simulation.state().navigation.modals.empty() && top(simulation).id != "game-settings") simulation.action("circle");
            activate(simulation, "steam-app-id");
            simulation.SubmitEditorText(text);
        };
        namespace cm = playden::community;
        check(simulation.ShareDecisionFor(night_sky).kind == cm::ShareKind::kUpdateOwn, "still the same game: updates its share");
        set_app("123");
        auto decision = simulation.ShareDecisionFor(night_sky);
        check(decision.kind == cm::ShareKind::kNewRecipe && !decision.send_submission_id,
              "a changed game ref: the earlier share is not updated, the next is new");
    }
    std::filesystem::remove_all(directory);
}

void community_recipes_one_base(const std::filesystem::path& fixtures, const std::filesystem::path& profile) {
    namespace rt = playden::runtime;
    playden::homebrew::Simulation simulation(fixtures, profile);
    load_settings_data(simulation, fixtures);
    // Velvet Engines is not installed: choosing needs no confirmation.
    check(simulation.scenario("recipes-one"), "recipes-one scenario loads");
    simulation.action("circle");
    activate(simulation, "details-more");
    activate(simulation, "game-settings");
    activate(simulation, "preset-picker");
    activate(simulation, "preset-mouse-driven");
    check(simulation.state().game_settings.at(SteamId(900012)).preset_id == "mouse-driven", "a preset is the base");
    activate(simulation, "preset-picker");
    activate(simulation, "recipe-" + rev('4'));
    auto record = simulation.state().game_settings.at(SteamId(900012));
    check(record.preset_id.empty() && record.community_recipe.id == "community-steam-900012-mouse-on-right-stick" &&
              record.community_recipe.revision == rev('4'), "choosing a recipe clears the preset");
    check(simulation.RecipeFor(SteamId(900012)).id == "community-steam-900012-mouse-on-right-stick", "the recipe is the game's base");
    check(find_row(simulation, "preset-picker")->value == "Mouse on the right stick", "the Preset row names the recipe");
    check(simulation.RecipeBaseFor(SteamId(900012)) &&
              simulation.RecipeBaseFor(SteamId(900012))->revision == rev('4'), "Share sees the recipe the game is based on");
    activate(simulation, "preset-picker");
    activate(simulation, "preset-keyboard-on-controller");
    record = simulation.state().game_settings.at(SteamId(900012));
    check(record.preset_id == "keyboard-on-controller" && record.community_recipe.id.empty(), "choosing a preset clears the recipe");
    activate(simulation, "preset-picker");
    activate(simulation, "preset-playden-default");
    record = simulation.state().game_settings.at(SteamId(900012));
    check(record.preset_id.empty() && record.community_recipe.id.empty(), "Playden default clears both");
}

void community_recipes_switch(const std::filesystem::path& fixtures, const std::filesystem::path& profile) {
    playden::homebrew::Simulation simulation(fixtures, profile);
    load_settings_data(simulation, fixtures);
    // An installed game: the changes that stay and the dropped ones are listed before the switch.
    check(simulation.scenario("recipes-switch"), "recipes-switch scenario loads");
    const auto& modal = simulation.state().navigation.modals.back();
    check(modal.id == "confirmation" && modal.title == "Switch to “120 Hz, pointer on left stick”?",
          "a switch on an installed game asks first");
    check(modal.body.find("Your changes that stay: Volume.") != std::string::npos &&
              modal.body.find("No longer apply: Program (The recipe has no launch option") != std::string::npos &&
              modal.body.find("If the switch doesn't finish, the game keeps its current recipe.") != std::string::npos,
          "the confirmation lists what stays and what is dropped");
    check(find_row(simulation, "recipe-switch-confirm")->title == "Switch and prepare again",
          "a new program prepares the game again");
    check(simulation.state().game_settings.at(SteamId(900002)).community_recipe.id.empty(), "nothing changes before the answer");
    check(simulation.CommunityChoicesInUse() && simulation.CommunityChoicesInUse()->count(rev('1')),
          "the cache keeps the recipe a switch waits to confirm");
    activate(simulation, "recipe-switch-confirm");
    const auto& record = simulation.state().game_settings.at(SteamId(900002));
    check(record.community_recipe.revision == rev('1') && record.overrides.count("volume") && !record.overrides.count("launch_option"),
          "the switch keeps the changes that still apply and drops the rest");
    check(simulation.state().navigation.modals.back().id == "game-settings", "back on Game settings after the switch");
    // Reset all goes back to the base's values: the recipe's resolution.
    activate(simulation, "reset-settings");
    playden::runtime::EffectiveSettings settings;
    check(simulation.EffectiveSettingsFor(SteamId(900002), &settings) && settings.values.at("resolution").value == "3840x2160" &&
              simulation.state().game_settings.at(SteamId(900002)).overrides.empty(),
          "Reset all returns to the recipe's values");
    // Without changes of the player's, the confirmation says so rather than "none carry over".
    activate(simulation, "preset-picker");
    activate(simulation, "preset-playden-default");
    check(simulation.state().navigation.modals.back().id == "confirmation" &&
              simulation.state().navigation.modals.back().body.find("You haven't changed any settings for this game.") != std::string::npos &&
              simulation.state().navigation.modals.back().body.find("None of your changes") == std::string::npos,
          "a switch without the player's changes says there are none");
    simulation.action("circle");
    simulation.action("circle");
    check(simulation.state().navigation.modals.back().id == "game-settings", "cancelled, back on Game settings");
    // The settings toast names the new base.
    simulation.action("circle");
    const auto toast = std::find_if(simulation.state().notices.begin(), simulation.state().notices.end(),
                                    [](const playden::homebrew::Notice& notice) { return notice.id == "settings-saved"; });
    check(toast != simulation.state().notices.end() && toast->message == "Settings saved · Community: 120 Hz, pointer on left stick",
          "closing Game settings announces the recipe");
    // Changes of the player's that all stop applying: none carry over.
    check(simulation.scenario("recipes-switch"), "recipes-switch scenario loads again");
    simulation.action("circle");
    simulation.action("circle");
    check(simulation.state().navigation.modals.back().id == "game-settings", "back on Game settings");
    activate(simulation, "reset-settings");
    activate(simulation, "setting-launch_option");
    activate(simulation, "choice-1");
    check(simulation.state().game_settings.at(SteamId(900002)).overrides.size() == 1 &&
              simulation.state().game_settings.at(SteamId(900002)).overrides.at("launch_option") == "editor",
          "only the level editor is chosen");
    activate(simulation, "preset-picker");
    activate(simulation, "recipe-" + rev('1'));
    check(simulation.state().navigation.modals.back().id == "confirmation" &&
              simulation.state().navigation.modals.back().body.find("None of your changes carry over.") != std::string::npos &&
              simulation.state().navigation.modals.back().body.find("No longer apply: Program") != std::string::npos,
          "when every change is dropped, none carry over");
}

void community_recipes_install(const std::filesystem::path& fixtures, const std::filesystem::path& profile) {
    using namespace playden::homebrew;
    {
        // Fixtures: untouched, Install creates the job and saves no settings.
        Simulation simulation(fixtures, profile);
        load_settings_data(simulation, fixtures);
        check(simulation.scenario("recipes-one"), "recipes-one scenario loads");
        const auto* settings = find_row(simulation, "install-settings");
        check(settings && settings->value == "Playden default ›" && settings->subtitle == "★ 1 community recipe available",
              "the Install sheet offers the community recipes");
        check(simulation.state().navigation.focus_key == "install-confirm", "Install stays focused");
        activate(simulation, "install-confirm");
        check(simulation.state().navigation.route == Route::Downloads && !simulation.state().game_settings.count(SteamId(900012)),
              "Install without touching Settings installs as before");
    }
    {
        // Fixtures: a recipe chosen on the sheet is saved before the download.
        std::error_code ignored;
        std::filesystem::remove(profile, ignored);  // the job the block above queued
        Simulation simulation(fixtures, profile);
        load_settings_data(simulation, fixtures);
        check(simulation.scenario("recipes-one"), "recipes-one scenario loads");
        activate(simulation, "install-settings");
        check(simulation.state().navigation.modals.back().id == "preset-picker" &&
                  find_row(simulation, "preset-playden-default")->selected, "Playden's choice is selected in the chooser");
        activate(simulation, "recipe-" + rev('4'));
        check(simulation.state().navigation.modals.back().id == "install" &&
                  find_row(simulation, "install-settings")->value == "Community: Mouse on the right stick ›" &&
                  !simulation.state().game_settings.count(SteamId(900012)),
              "the choice waits on the sheet");
        activate(simulation, "install-confirm");
        check(simulation.state().game_settings.at(SteamId(900012)).community_recipe.revision == rev('4'),
              "Install saves the chosen recipe");
    }
    {
        // Live: every install opens the sheet; only a game with recipes gets its Settings row, whose choice is saved
        // before the request goes out.
        Simulation live({}, {}, ModelMode::Live);
        live.SetWallClock(1700000000);
        live.UpdateAccount({AuthState::SignedIn, "123", "Synthetic account", {}, {}, true});
        playden::GameSummary game{SteamId(42), "Recipe game", {}, {}, {}};
        check(live.UpdateLibrary({{game}, false, {}, false, false, {}}), "library loads");
        live.UpdateAppDetails(SteamId(42), 300'000'000, 600'000'000, true);
        live.UpdateVolumes({{"internal", "Console storage", true, 825'000'000'000ull, 512'000'000'000ull}}, "internal");
        check(live.action("details:42"), "details open");
        activate(live, "details-install");
        auto requests = live.TakeInstallRequests();
        check(requests.empty() && !live.state().navigation.modals.empty() && live.state().navigation.modals.back().id == "install" &&
                  find_row(live, "install-settings") == nullptr && live.TakeCommunityRefreshRequest(),
              "without recipes the Install sheet has no Settings row, and asks for a refresh");
        activate(live, "cancel");
        playden::runtime::CommunityRecipe object;
        object.recipe.id = "community-steam-42-pad";
        object.game = playden::GameRef::Steam(42);
        object.recipe.version = 1;
        object.recipe.app_id = 42;
        object.recipe.name = "Recipe game";
        object.recipe.launch_entries = {{"game", "Recipe game", "Game.exe", ""}};
        object.title = "Pad";
        object.status = "works";
        live.SetCommunityRecipeLoader([object](const std::string& revision) -> std::optional<playden::runtime::CommunityRecipe> {
            if (revision != rev('7')) return std::nullopt;
            return object;
        });
        playden::runtime::VerifiedKitView kit;
        kit.runtime.artifact_revision = "f729534-kit-0.12";
        live.SetKitView(kit);
        CommunityCatalog catalog;
        catalog.release_tag = "r1";
        CommunityRecipeOffer offer;
        offer.id = "community-steam-42-pad";
        offer.title = "Pad";
        offer.status = "works";
        offer.revision = rev('7');
        catalog.games[playden::GameRef::Steam(42)] = {offer};
        live.UpdateCommunityCatalog(catalog);
        activate(live, "details-install");
        check(live.TakeInstallRequests().empty() && live.state().navigation.modals.back().id == "install",
              "a game with recipes shows the Install sheet");
        activate(live, "install-confirm");
        requests = live.TakeInstallRequests();
        check(requests.size() == 1 && requests[0].kind == InstallRequestKind::Install && !live.state().game_settings.count(SteamId(42)),
              "untouched, Install sends the same request and saves nothing");
        activate(live, "details-install");
        activate(live, "install-settings");
        activate(live, "recipe-" + rev('7'));
        check(live.CommunityChoicesInUse() == std::set<std::string>{rev('7')} && !live.state().game_settings.count(SteamId(42)),
              "the cache keeps the recipe the open Install sheet chose, before anything is saved");
        live.action("circle");
        check(live.state().navigation.modals.empty() && live.CommunityChoicesInUse() == std::set<std::string>{},
              "a cancelled Install sheet keeps nothing");
        activate(live, "details-install");
        activate(live, "install-settings");
        activate(live, "recipe-" + rev('7'));
        activate(live, "install-confirm");
        requests = live.TakeInstallRequests();
        check(requests.size() == 1 && live.state().game_settings.at(SteamId(42)).community_recipe.id == "community-steam-42-pad",
              "the recipe chosen at Install is saved with the request");
        check(live.CommunityChoicesInUse() == std::set<std::string>{rev('7')}, "the cache keeps the saved choice");
        // Installed again later (uninstalling keeps a game's settings): the sheet shows the saved recipe, which the install
        // uses, and Playden default chosen there replaces it.
        live.SetSettingsData({{"playden-default", "Playden default", "Most games", "", {}}}, {});
        activate(live, "details-install");
        check(find_row(live, "install-settings") && find_row(live, "install-settings")->value == "Community: Pad ›",
              "the Install sheet shows the settings saved for the game");
        {
            const auto before = live.state().game_settings.at(SteamId(42)).revision;
            activate(live, "install-confirm");
            (void)live.TakeInstallRequests();
            check(live.state().game_settings.at(SteamId(42)).revision == before &&
                      live.state().game_settings.at(SteamId(42)).community_recipe.id == "community-steam-42-pad",
                  "installed with them untouched, the settings don't change");
            activate(live, "details-install");
        }
        activate(live, "install-settings");
        check(find_row(live, "recipe-" + rev('7')) && find_row(live, "recipe-" + rev('7'))->selected &&
                  find_row(live, "preset-playden-default") && !find_row(live, "preset-playden-default")->selected,
              "the chooser has the saved recipe selected");
        activate(live, "preset-playden-default");
        check(find_row(live, "install-settings") && find_row(live, "install-settings")->value == "Playden default ›",
              "Playden default is chosen on the sheet");
        activate(live, "install-confirm");
        (void)live.TakeInstallRequests();
        check(live.state().game_settings.count(SteamId(42)) && live.state().game_settings.at(SteamId(42)).community_recipe.id.empty(),
              "and saved at Install, replacing the recipe");
    }
    {
        // Game settings that can't be read: what is in use is unknown, so the cache keeps everything.
        const auto unreadable = profile.parent_path() / "unreadable-game-settings.json";
        std::ofstream(unreadable) << "{ not json";
        Simulation live({}, {}, ModelMode::Live);
        live.SetGameSettingsFile(unreadable);
        check(!live.CommunityChoicesInUse(), "unreadable game settings keep every recipe");
        std::error_code ignored;
        std::filesystem::remove(unreadable, ignored);
    }
}

void community_recipes_updates_and_fallback(const std::filesystem::path& fixtures, const std::filesystem::path& profile) {
    playden::homebrew::Simulation simulation(fixtures, profile);
    load_settings_data(simulation, fixtures);
    // A newer revision of the chosen recipe is offered, never applied silently.
    check(simulation.scenario("recipes-settings"), "recipes-settings scenario loads");
    const auto* update = find_row(simulation, "recipe-update");
    check(update && update->title == "Updated recipe available" && update->value == "Use it", "an updated recipe is offered");
    // The chooser lists the recipe once, at its newer revision: what that changes, marked Updated, not Current.
    activate(simulation, "preset-picker");
    check(!has_row(simulation, "recipe-" + rev('0')), "the older revision in use isn't listed beside the newer one");
    const auto* newer = find_row(simulation, "recipe-" + rev('1'));
    check(newer && !newer->selected && newer->badge.find("Updated") != std::string::npos &&
              newer->badge.find("Current") == std::string::npos && simulation.state().navigation.focus_key == "recipe-" + rev('1'),
          "the newer revision is marked Updated and focused");
    check(simulation.view().comparison &&
              std::any_of(simulation.view().comparison->rows.begin(), simulation.view().comparison->rows.end(),
                          [](const std::array<std::string, 3>& line) { return line[0] == "Button mapping"; }),
          "its comparison shows what the newer revision changes from the one in use");
    simulation.action("circle");
    check(simulation.RecipeFor(SteamId(900002)).revision == rev('0'), "the game keeps the revision it chose");
    activate(simulation, "recipe-update");
    check(simulation.state().navigation.modals.back().id == "confirmation", "using it on an installed game asks first");
    activate(simulation, "recipe-switch-confirm");
    check(simulation.state().game_settings.at(SteamId(900002)).community_recipe.revision == rev('1') &&
              simulation.state().game_settings.at(SteamId(900002)).overrides.count("volume") && !has_row(simulation, "recipe-update"),
          "Use it chooses the new revision and keeps the player's changes");
    // Export remembers the recipe it is based on, and the kit the title runs.
    const auto directory = std::filesystem::temp_directory_path() / ("playden-recipe-export-" + std::to_string(getpid()));
    std::filesystem::remove_all(directory);
    simulation.SetRecipeExportTarget(directory, "10.01");
    simulation.export_recipe(SteamId(900002));
    const auto exported = read_text(directory / "recipe-900002.json");
    check(exported.find("\"tested_kit\": \"0.12\"") != std::string::npos || exported.find("\"tested_kit\":\"0.12\"") != std::string::npos,
          "the export names the kit the title runs");
    check(simulation.ExportedRecipeBase(SteamId(900002)) && simulation.ExportedRecipeBase(SteamId(900002))->revision == rev('1'),
          "the export remembers the community recipe it is based on");
    std::filesystem::remove_all(directory);
    // A chosen recipe the kit can't run: Playden's own recipe is used and the sheet says why.
    check(simulation.scenario("recipes-fallback"), "recipes-fallback scenario loads");
    const auto* status = find_row(simulation, "recipe-status");
    check(status && status->title == "Playden's own recipe is used" && status->subtitle == "The recipe needs runtime kit version 0.13 or newer.",
          "the fallback reason is shown");
    check(simulation.RecipeFor(SteamId(900005)).id.rfind("community-", 0) != 0, "the game falls back to Playden's recipe");
    check(find_row(simulation, "preset-picker")->value == "OpenGL through Zink", "the Preset row keeps the choice");
}

// Sharing and votes in fixtures: the decision table on the Share sheet, a second share with a saved
// submission, the first share's notice, and the vote prompt once per revision (Not now counts).
void community_sharing(const std::filesystem::path& fixtures, const std::filesystem::path& profile) {
    namespace cm = playden::community;
    const auto game = SteamId(900002);
    playden::homebrew::Simulation simulation(fixtures, profile);
    load_settings_data(simulation, fixtures);
    const auto top = [&simulation]() -> const playden::homebrew::ModalEntry& { return simulation.state().navigation.modals.back(); };

    // No community recipe in use, nothing shared before: a new recipe, no question.
    check(simulation.scenario("share-new"), "share-new scenario loads");
    check(simulation.ShareDecisionFor(game).kind == cm::ShareKind::kNewRecipe && top().id == "share-recipe" && top().title == "Share recipe",
          "Share: a new recipe");
    check(!has_row(simulation, "share-choice-update") && !has_row(simulation, "share-update") &&
              find_row(simulation, "share-title")->value == "Optional",
          "a new recipe asks nothing, and its title is optional");
    check(find_row(simulation, "share-as")->value == "TodorPenchev" && find_row(simulation, "share-rating")->value == "Works",
          "the name and the Compatibility rating are shown");
    check(find_row(simulation, "share-what")->subtitle.find("Resolution 3840×2160") != std::string::npos,
          "what is sent lists the changes from Playden's default");
    check(top().focused_action >= 0 && top().action_keys[static_cast<std::size_t>(top().focused_action)] == "share-send",
          "with nothing to fill in, the sheet opens on Send");
    simulation.action("focus:share-title");
    simulation.action("cross");
    check(simulation.state().editor.active && simulation.state().editor.purpose == "Recipe title", "the title opens the keyboard");
    simulation.SubmitEditorText(std::string(61, 'x'));
    check(simulation.state().editor.active, "a title over 60 characters is refused");
    simulation.SubmitEditorText("4K at 120 Hz");
    check(!simulation.state().editor.active && find_row(simulation, "share-title")->value == "4K at 120 Hz", "the title is set");
    activate(simulation, "share-send");
    check(top().title == "Recipe sent for review" && top().link.find("/pull/") != std::string::npos &&
              top().link_caption == "Scan to follow the pull request",
          "sent: the pull request as a QR code");
    const auto first = simulation.state().game_settings.at(game).shared_recipe;
    check(!first.submission_id.empty() && !first.recipe_id.empty(), "the share is saved in the game's settings");
    // A second share of the game updates the saved submission (its pull request is in review).
    activate(simulation, "alert-ok");
    check(top().id == "game-settings", "OK goes back to Game settings");
    check(simulation.ShareDecisionFor(game).kind == cm::ShareKind::kUpdateOwn &&
              simulation.ShareDecisionFor(game).send_submission_id,
          "a saved submission: the next share updates it");
    activate(simulation, "section-advanced");
    activate(simulation, "share-recipe");
    check(find_row(simulation, "share-send") && find_row(simulation, "share-send")->title == "Update the pull request",
          "an update to a share in review says it updates the pull request");
    activate(simulation, "share-send");
    check(simulation.state().game_settings.at(game).shared_recipe == first, "the same submission is revised");

    // The first share explains what is sent before sending.
    check(simulation.scenario("share-privacy"), "share-privacy scenario loads");
    check(top().title == "Before you share" && top().body.find("No Steam ID or account name") != std::string::npos,
          "the first share says what is sent and what isn't");
    check(simulation.state().game_settings.at(game).shared_recipe.submission_id.empty(), "nothing is sent before Send");

    // Someone else's recipe with changes that keep program and graphics: ask, Suggest changes first.
    check(simulation.scenario("share-ask"), "share-ask scenario loads");
    auto decision = simulation.ShareDecisionFor(game);
    check(decision.kind == cm::ShareKind::kAskPlayer && decision.preselect == cm::ShareChoice::kUpdate,
          "based on someone else's recipe, changed: ask, Suggest changes pre-selected");
    check(find_row(simulation, "share-choice-update")->selected && !find_row(simulation, "share-choice-new")->selected &&
              find_row(simulation, "share-title")->value == "120 Hz, pointer on left stick",
          "Suggest changes is chosen and keeps the recipe's title");
    check(find_row(simulation, "share-what")->lines.front() == "Your changes: Resolution 1920×1080",
          "the sheet lists the player's changes against the recipe");
    activate(simulation, "share-choice-new");
    check(find_row(simulation, "share-choice-new")->selected && find_row(simulation, "share-title")->value == "Optional",
          "a new recipe's title is optional");
    // A different graphics choice pre-selects a new recipe.
    simulation.action("circle");
    check(top().id == "game-settings", "back to Game settings");
    for (const char* section : {"section-compatibility", "section-advanced"})
        if (!has_row(simulation, "setting-graphics")) activate(simulation, section);
    activate(simulation, "setting-graphics");
    for (const auto& row : simulation.view().rows)
        if (row.key.rfind("choice-", 0) == 0 && !row.selected && row.enabled) { activate(simulation, row.key); break; }
    decision = simulation.ShareDecisionFor(game);
    check(decision.kind == cm::ShareKind::kAskPlayer && decision.preselect == cm::ShareChoice::kNew,
          "another graphics choice pre-selects Share as a new recipe");

    // This console's own recipe, its pull request in review: an update, no question.
    check(simulation.scenario("share-own"), "share-own scenario loads");
    decision = simulation.ShareDecisionFor(game);
    check(decision.kind == cm::ShareKind::kUpdateOwn && decision.send_based_on && decision.send_submission_id,
          "based on this console's recipe: an update");
    check(find_row(simulation, "share-send")->title == "Update the pull request" &&
              simulation.view().subtitle.find("still in review") != std::string::npos,
          "it says the update replaces what is in review");

    // Someone else's recipe without changes: nothing to share, a thumbs up instead.
    check(simulation.scenario("share-nothing"), "share-nothing scenario loads");
    check(simulation.ShareDecisionFor(game).kind == cm::ShareKind::kNoChangesOfferVote && top().title == "Nothing new to share" &&
              has_row(simulation, "share-vote") && find_row(simulation, "cancel")->title == "Not now",
          "nothing new: Vote for it instead or Not now");
    activate(simulation, "share-vote");
    check(top().id == "vote-recipe" && find_row(simulation, "vote-up")->selected, "votes not asked yet: the vote sheet explains first");

    // Votes off: the offer explains how to turn them on; Playden still asks how the game ran.
    check(simulation.scenario("votes-off"), "votes-off scenario loads");
    check(find_row(simulation, "share-vote")->subtitle.find("Recipe votes are off") != std::string::npos, "the offer says votes are off");
    activate(simulation, "share-vote");
    check(top().title == "Recipe votes are off", "voting off: no vote, how to turn it on");
    while (!simulation.state().navigation.modals.empty()) simulation.action("circle");
    simulation.NotifySessionEnded(game, 3600);
    check(top().id == "feedback", "with votes off Playden still asks how the game ran");
    // Don't ask again stops the question only: votes stay as they were.
    check(simulation.scenario("feedback-prompt"), "feedback-prompt scenario loads for Don't ask again");
    const auto votes_before = simulation.state().recipe_votes;
    activate(simulation, "feedback-never");
    check(!simulation.state().ask_after_games && !simulation.ExportPreferences().ask_after_games &&
              simulation.state().recipe_votes == votes_before,
          "Don't ask again turns asking off and leaves votes alone");
    simulation.NotifySessionEnded(game, 3600);
    check(simulation.state().navigation.modals.empty(), "then nothing asks after play");

    // After a game: how it ran (any game), with the recipe in use; Not now counts, twice at most.
    check(simulation.scenario("feedback-prompt"), "feedback-prompt scenario loads");
    check(top().id == "feedback" && top().title.find("How did") == 0 &&
              simulation.view().subtitle.find("120 Hz, pointer on left stick") != std::string::npos &&
              has_row(simulation, "feedback-good") && has_row(simulation, "feedback-problems") && has_row(simulation, "feedback-never"),
          "after play: how did it run, and the answer rates the recipe in use");
    check(!simulation.state().vote_memory.feedback.empty() && simulation.state().vote_memory.feedback.back().prompts == 1,
          "the prompt is noted");
    simulation.action("circle");
    check(simulation.state().navigation.modals.empty(), "Not now closes it");
    simulation.NotifySessionEnded(game, 900);
    check(top().id == "feedback", "a second session asks once more");
    simulation.action("circle");
    simulation.NotifySessionEnded(game, 900);
    check(simulation.state().navigation.modals.empty(), "then Playden stops asking about these settings");
    check(simulation.scenario("feedback-start"), "feedback-start scenario loads");
    check(top().id == "feedback" && top().title.find("Did ") == 0 && has_row(simulation, "feedback-started") &&
              has_row(simulation, "feedback-no-start"),
          "a short session asks whether the game started");
    activate(simulation, "feedback-no-start");
    check(!has_row(simulation, "feedback-reason-slow") && !has_row(simulation, "feedback-result-partial") &&
              find_row(simulation, "feedback-logs")->value == "Included" &&
              has_row(simulation, "feedback-change-settings"),
          "it didn't start: logs are required, Change settings is offered");
    activate(simulation, "feedback-change-settings");
    check(top().id == "game-settings", "Change settings opens Game settings");
    // It ran well with the player's own changes: a report, and the offer to share them as a recipe.
    check(simulation.scenario("feedback-good"), "feedback-good scenario loads");
    check(has_row(simulation, "feedback-recipe") && !find_row(simulation, "feedback-logs")->selected,
          "changes beyond display settings offer a recipe; logs are optional");
    activate(simulation, "feedback-recipe");
    activate(simulation, "feedback-send");
    check(top().id == "share-recipe", "the Share sheet follows the report");
    check(simulation.scenario("feedback-problems"), "feedback-problems scenario loads");
    {
        // Result first, as two ticked choices, then what went wrong, then the report.
        const auto& keys = top().action_keys;
        const auto at = [&](const char* key) { return std::find(keys.begin(), keys.end(), key) - keys.begin(); };
        check(at("heading-result") < at("feedback-result-partial") && at("feedback-result-partial") < at("feedback-result-broken") &&
                  at("feedback-result-broken") < at("heading-reason") && at("heading-reason") < at("feedback-reason-crash") &&
                  at("feedback-reason-other") < at("heading-report"),
              "problems: Result, then What went wrong, then Report");
    }
    check(find_row(simulation, "feedback-reason-slow")->selected && find_row(simulation, "feedback-result-partial")->checked &&
              !find_row(simulation, "feedback-result-broken")->checked &&
              find_row(simulation, "feedback-send")->enabled,
          "problems: the reason chosen, Partial by default, Send ready");
    activate(simulation, "feedback-result-broken");
    check(find_row(simulation, "feedback-result-broken")->checked && !find_row(simulation, "feedback-result-partial")->checked,
          "choosing Broken ticks it instead of Partial");
    activate(simulation, "feedback-send");
    check(top().title == "Test report sent" && !top().link.empty(), "the report is sent, its issue as a QR code");
    const auto* down = simulation.state().vote_memory.Find(playden::runtime::CommunityRecipeIdFor(playden::GameRef::Steam(900002), "120hz-left-stick-pointer"), rev('1'));
    check(down && down->vote == "down" && down->reason == "slow", "the recipe's thumbs down goes with it");
    // Other: chosen at once, then the keyboard asks what it was; the text shows under it, cancelling keeps Other.
    check(simulation.scenario("feedback-problems"), "feedback-problems scenario loads for Other");
    activate(simulation, "feedback-reason-other");
    check(simulation.state().editor.active, "Other opens the keyboard");
    simulation.SubmitEditorText("Music stutters");
    check(!simulation.state().editor.active && find_row(simulation, "feedback-reason-other")->selected &&
              !find_row(simulation, "feedback-reason-slow")->selected &&
              find_row(simulation, "feedback-reason-other")->subtitle == "Music stutters",
          "Other is chosen and shows what was typed");
    activate(simulation, "feedback-reason-other");
    check(simulation.state().editor.active && simulation.state().editor.value == "Music stutters", "choosing Other again edits it");
    simulation.action("circle");
    check(!simulation.state().editor.active && find_row(simulation, "feedback-reason-other")->selected &&
              find_row(simulation, "feedback-reason-other")->subtitle == "Music stutters",
          "cancelling the keyboard keeps Other and its text");
    // A trusted account is never told so: no Verified pitch, and the row says whether shares credit the account.
    check(simulation.scenario("account-linked"), "account-linked scenario loads");
    {
        const auto* account = find_row(simulation, "settings-account-link");
        check(account && account->value == "Rina" && account->subtitle.find("Verified") == std::string::npos &&
                  account->subtitle.find("Share as") != std::string::npos,
              "a linked trusted account: no Verified, and Share as decides the credit");
        activate(simulation, "settings-share-as");
        check(find_row(simulation, "share-as-account") &&
                  find_row(simulation, "share-as-account")->subtitle.find("Verified") == std::string::npos,
              "the Share as picker doesn't mention Verified");
    }
    check(simulation.scenario("feedback-privacy"), "feedback-privacy scenario loads");
    check(top().title == "Before you send", "the first report says what it sends");
    // A send that got no answer: the sheet stays with the reason, and Send retries the same share or report.
    check(simulation.scenario("share-error"), "share-error scenario loads");
    check(top().id == "share-recipe" && find_row(simulation, "share-send")->title == "Try again" &&
              find_row(simulation, "share-send")->tone == "problem" &&
              find_row(simulation, "share-send")->subtitle.find("couldn't reach") != std::string::npos,
          "a failed share says why and offers Try again");
    check(simulation.scenario("feedback-error"), "feedback-error scenario loads");
    check(top().id == "feedback" && find_row(simulation, "feedback-send")->title == "Try again" &&
              find_row(simulation, "feedback-send")->subtitle.find("couldn't reach") != std::string::npos &&
              find_row(simulation, "feedback-send")->subtitle.find("second report") != std::string::npos &&
              find_row(simulation, "feedback-send")->subtitle.find("vote") == std::string::npos,
          "a failed report says why in a report's words and offers Try again");
    // Changes worth sharing put Share recipe by the preset, out of Advanced.
    check(simulation.scenario("share-new"), "share-new scenario loads for Game settings");
    simulation.action("circle");
    {
        const auto& keys = top().action_keys;
        const auto share = std::find(keys.begin(), keys.end(), "share-recipe");
        const auto advanced = std::find(keys.begin(), keys.end(), "section-advanced");
        check(top().id == "game-settings" && share != keys.end() && share < advanced, "Share recipe is at the top with changes to share");
    }
    // The prompt can come up away from the game's page: Change settings still opens that game's settings.
    check(simulation.scenario("share-new"), "share-new scenario loads to leave the game's page");
    while (!simulation.state().navigation.modals.empty()) simulation.action("circle");
    simulation.action("circle");
    check(simulation.state().navigation.route != playden::homebrew::Route::Details, "away from the game's page");
    simulation.NotifySessionEnded(game, 600);
    check(top().id == "feedback", "the prompt comes up there");
    activate(simulation, "feedback-problems");
    check(simulation.state().navigation.focus_key.rfind("heading-", 0) != 0 && simulation.state().navigation.focus_key != "divider",
          "after It had problems the focus is on a row it can take, not a heading");
    check(simulation.state().navigation.focus_key == "feedback-result-partial", "and that row is Partial, the first choice");
    activate(simulation, "feedback-change-settings");
    check(top().id == "game-settings" && simulation.state().navigation.selected_game_id == game && has_row(simulation, "preset-picker"),
          "Change settings opens the game's settings from anywhere");
    // Share feedback… in the game's actions opens the same sheet any time.
    check(simulation.scenario("share-new"), "share-new scenario loads for the game actions");
    while (!simulation.state().navigation.modals.empty()) simulation.action("circle");
    activate(simulation, "details-more");
    check(has_row(simulation, "share-feedback"), "Game actions offer Share feedback…");
    activate(simulation, "share-feedback");
    check(top().id == "feedback" && !has_row(simulation, "feedback-never"), "Share feedback… asks how it ran, without Don't ask again");

    // A thumbs down with a reason; votes are on.
    check(simulation.scenario("vote-reason"), "vote-reason scenario loads");
    check(top().id == "vote-recipe" && has_row(simulation, "vote-reason-no-start") &&
              find_row(simulation, "vote-reason-slow")->selected && !has_row(simulation, "vote-never"),
          "Your vote: a thumbs down, the reasons, Slow chosen; votes on asks nothing more");
    activate(simulation, "vote-send");
    const auto* cast = simulation.state().vote_memory.Find(playden::runtime::CommunityRecipeIdFor(playden::GameRef::Steam(900002), "120hz-left-stick-pointer"), rev('1'));
    check(cast && cast->vote == "down" && cast->reason == "slow", "the vote and its reason are remembered");
    check(simulation.snapshot().find("sharing-id") == std::string::npos, "the snapshot holds no sharing ID");
}


// My presets in Choose settings: their section between the community recipes and the built-in presets (only when
// there is one), each row's name, summary and other games, choosing one, and the Triangle menu (Rename…, Update from
// this game, Delete… with its question).
const std::string kMine1 = "mypreset-my-0000000000000001";
const std::string kMine2 = "mypreset-my-0000000000000002";
const std::string kMine3 = "mypreset-my-0000000000000003";

const playden::homebrew::ModalEntry& top_modal(const playden::homebrew::Simulation& simulation) {
    check(!simulation.state().navigation.modals.empty(), "a sheet or dialog is open");
    return simulation.state().navigation.modals.back();
}

bool has_legend(const playden::homebrew::Simulation& simulation, const std::string& legend) {
    const auto& legends = simulation.view().legends;
    return std::find(legends.begin(), legends.end(), legend) != legends.end();
}

std::string validation(const playden::homebrew::Simulation& simulation) {
    for (const auto& notice : simulation.state().notices)
        if (notice.id == "setting-validation") return notice.detail;
    return {};
}

void my_presets_chooser(const std::filesystem::path& fixtures, const std::filesystem::path& profile) {
    namespace rt = playden::runtime;
    playden::homebrew::Simulation simulation(fixtures, profile);
    load_settings_data(simulation, fixtures);
    check(simulation.scenario("recipes-three"), "recipes-three scenario loads");
    const auto& none = top_modal(simulation).action_keys;
    check(std::find(none.begin(), none.end(), "chooser-heading-mine") == none.end(),
          "my presets: no section in Choose settings without one");

    check(simulation.scenario("my-presets"), "my-presets scenario loads");
    check(simulation.MyPresets().size() == 3, "my presets: three made");
    const auto keys = top_modal(simulation).action_keys;
    const auto at = [&keys](const std::string& key) { return std::find(keys.begin(), keys.end(), key) - keys.begin(); };
    check(top_modal(simulation).id == "preset-picker" && at("recipes-search") + 1 == at("chooser-heading-mine") &&
              at("chooser-heading-mine") + 1 == at(kMine1) && at(kMine1) + 1 == at(kMine2) && at(kMine2) + 1 == at(kMine3) &&
              at(kMine3) + 1 == at("chooser-heading-presets") && at("preset-playden-default") == 0 &&
              at("chooser-heading-community") < at("recipes-search"),
          "my presets: Playden default, community recipes and search, my presets, then the built-in presets");
    check(find_row(simulation, "chooser-heading-mine") && find_row(simulation, "chooser-heading-mine")->title == "My presets · made by you",
          "my presets: the section's heading");
    const auto* row = find_row(simulation, kMine1);
    check(row && row->title == "Point and click" &&
              row->subtitle == rt::MyPresetSummary(simulation.MyPresets().front().preset.settings) && !row->subtitle.empty() &&
              row->value == "2 games" && row->selected && row->badge == "Current · changed",
          "my presets: the game's preset is current, with what it sets and the two other games using it");
    check(simulation.state().navigation.focus_key == kMine1, "my presets: Choose settings opens on the game's preset");
    row = find_row(simulation, kMine2);
    check(row && row->title == "Keyboard only" && row->value.empty() && !row->selected && row->badge.empty(),
          "my presets: an unused preset says no games");
    // Focused: the comparison beside the sheet, and Triangle for its menu.
    check(simulation.action("focus:" + kMine3), "focus a preset of mine");
    check(simulation.view().comparison && simulation.view().comparison->title == "Couch at 60 Hz" &&
              !simulation.view().comparison->rows.empty(),
          "my presets: the comparison shows what the preset changes");
    check(has_legend(simulation, "Triangle Manage"), "my presets: the footer offers Triangle on a preset of mine");
    check(simulation.action("focus:preset-mouse-driven") && !has_legend(simulation, "Triangle Manage"),
          "my presets: no Triangle on a built-in preset");
    check(simulation.action("triangle") && top_modal(simulation).id == "preset-picker", "Triangle on a built-in preset does nothing");

    // Triangle menu on another preset: no Update from this game.
    check(simulation.action("focus:" + kMine2) && simulation.action("triangle"), "Triangle on another preset");
    check(top_modal(simulation).id == "mypreset-menu" && top_modal(simulation).title == "Keyboard only" &&
              (top_modal(simulation).action_keys == std::vector<std::string>{"my-preset-rename", "my-preset-delete", "cancel"}),
          "my presets: a preset the game doesn't use offers Rename and Delete");
    check(simulation.action("circle") && top_modal(simulation).id == "preset-picker" &&
              simulation.state().navigation.focus_key == kMine2, "Circle goes back to the row");

    // On the game's preset: Update from this game, with the game's own change.
    check(simulation.action("focus:" + kMine1) && simulation.action("triangle"), "Triangle on the game's preset");
    check((top_modal(simulation).action_keys ==
           std::vector<std::string>{"my-preset-rename", "my-preset-update", "my-preset-delete", "cancel"}) &&
              find_row(simulation, "my-preset-update")->enabled && find_row(simulation, "my-preset-delete")->dangerous,
          "my presets: the game's preset can take its changes");

    // Rename…: the keyboard with the name; names that can't be used keep it open with the reason.
    check(simulation.action("focus:my-preset-rename") && simulation.action("cross"), "Rename…");
    check(simulation.state().editor.active && simulation.state().editor.purpose == "Preset name" &&
              simulation.state().editor.value == "Point and click",
          "my presets: Rename opens the keyboard with the name");
    simulation.SubmitEditorText("keyboard ONLY");
    check(simulation.state().editor.active && validation(simulation) == "You already have a preset with this name.",
          "my presets: a name another preset has is refused");
    simulation.SubmitEditorText(std::string(41, 'a'));
    check(simulation.state().editor.active && validation(simulation) == "Up to 40 characters.", "my presets: a long name is refused");
    simulation.SubmitEditorText("Point\u200band click");
    check(simulation.state().editor.active && validation(simulation) == "Letters, numbers and punctuation on one line.",
          "my presets: a name with an invisible character is refused");
    simulation.SubmitEditorText("   ");
    check(simulation.state().editor.active && validation(simulation) == "Type a name for the preset.", "my presets: an empty name is refused");
    simulation.SubmitEditorText("Adventures");
    check(!simulation.state().editor.active && validation(simulation).empty() && top_modal(simulation).id == "preset-picker" &&
              simulation.state().navigation.focus_key == kMine1 && find_row(simulation, kMine1)->title == "Adventures",
          "my presets: renamed, back on its row");

    // Update from this game: the game's change goes into the preset and off the game; the other games get a revision.
    const auto game = SteamId(900009);
    const auto other_revision = simulation.state().game_settings.at(SteamId(900010)).revision;
    check(simulation.action("triangle") && simulation.action("focus:my-preset-update") && simulation.action("cross"), "Update from this game");
    check(top_modal(simulation).id == "confirmation" && top_modal(simulation).title == "Updated “Adventures”",
          "my presets: the update says so");
    check(simulation.MyPresets().front().preset.settings.at("performance_overlay") == "on" &&
              simulation.state().game_settings.at(game).overrides.empty() &&
              simulation.state().game_settings.at(game).preset_id == simulation.MyPresets().front().preset.id &&
              simulation.state().game_settings.at(SteamId(900010)).revision == other_revision + 1,
          "my presets: the update moved the game's change into the preset");
    check(simulation.action("cross") && top_modal(simulation).id == "preset-picker", "OK goes back to Choose settings");
    check(simulation.action("focus:" + kMine1) && simulation.action("triangle") && !find_row(simulation, "my-preset-update")->enabled &&
              find_row(simulation, "my-preset-update")->subtitle == "No changes to add",
          "my presets: with no changes of its own, Update from this game says why it can't");

    // Delete…: asks first; Cancel keeps it.
    check(simulation.action("focus:my-preset-delete") && simulation.action("cross"), "Delete…");
    check(top_modal(simulation).id == "confirmation" && top_modal(simulation).title == "Delete “Adventures”?" &&
              top_modal(simulation).body == "Games using it go back to Playden default and keep their own changes." &&
              simulation.state().navigation.focus_key == "cancel",
          "my presets: Delete asks first, on Cancel");
    check(simulation.action("cross") && simulation.MyPresets().size() == 3 && top_modal(simulation).id == "mypreset-menu",
          "my presets: Cancel keeps the preset");
    auto with_change = simulation.state().game_settings.at(SteamId(900011));
    check(simulation.action("focus:my-preset-delete") && simulation.action("cross") && simulation.action("focus:my-preset-delete-confirm") &&
              simulation.action("cross"),
          "Delete confirmed");
    check(simulation.MyPresets().size() == 2 && top_modal(simulation).id == "preset-picker" && !find_row(simulation, kMine1) &&
              simulation.state().game_settings.at(game).preset_id.empty() &&
              simulation.state().game_settings.at(SteamId(900011)).preset_id.empty() &&
              simulation.state().game_settings.at(SteamId(900011)).overrides == with_change.overrides &&
              simulation.state().navigation.focus_key == "preset-playden-default" && find_row(simulation, "preset-playden-default")->selected,
          "my presets: deleted; its games are on Playden default, which is focused");

    // Choosing one of mine: like a built-in preset, it clears the game's changes and closes.
    check(simulation.scenario("my-presets"), "my-presets scenario loads again");
    check(simulation.action("focus:" + kMine3) && simulation.action("cross"), "choose a preset of mine");
    check(top_modal(simulation).id == "game-settings" &&
              simulation.state().game_settings.at(game).preset_id == "my-0000000000000003" &&
              simulation.state().game_settings.at(game).overrides.empty() &&
              find_row(simulation, "preset-picker")->value == "Couch at 60 Hz",
          "my presets: chosen like a built-in preset");
    check(simulation.action("focus:preset-picker") && simulation.action("cross") &&
              simulation.state().navigation.focus_key == kMine3 && find_row(simulation, kMine3)->badge == "Current",
          "my presets: the chosen preset is current");

    // The Install sheet's chooser lists them too, and keeps the choice for the install; no Update in its menu.
    check(simulation.scenario("recipes-one") && simulation.action("focus:install-settings") && simulation.action("cross"),
          "the Install sheet's chooser");
    check(top_modal(simulation).id == "preset-picker" && find_row(simulation, kMine1) && !find_row(simulation, kMine1)->selected &&
              find_row(simulation, kMine1)->badge.empty(),
          "my presets: the Install sheet's chooser lists them");
    check(simulation.action("focus:" + kMine1) && simulation.action("triangle") &&
              (top_modal(simulation).action_keys == std::vector<std::string>{"my-preset-rename", "my-preset-delete", "cancel"}) &&
              simulation.action("circle"),
          "my presets: no Update from this game for an install");
    check(simulation.action("cross") && top_modal(simulation).id == "install", "choose one for the install");
    check(simulation.action("focus:install-settings") && simulation.action("cross") && find_row(simulation, kMine1)->selected &&
              find_row(simulation, kMine1)->badge == "Selected",
          "my presets: the install keeps the chosen preset");

    // Deleting the pending install choice must also forget its id before Install saves it.
    const auto install_game = simulation.state().navigation.selected_game_id;
    check(simulation.action("triangle") && simulation.action("focus:my-preset-delete") && simulation.action("cross") &&
              simulation.action("focus:my-preset-delete-confirm") && simulation.action("cross"), "delete the install choice");
    check(simulation.state().navigation.focus_key == "preset-playden-default" &&
              find_row(simulation, "preset-playden-default")->selected, "deleted install choice selects default");
    check(simulation.action("circle") && simulation.action("focus:install-confirm") && simulation.action("cross"),
          "install after deleting its selected preset");
    check(simulation.state().navigation.route == playden::homebrew::Route::Downloads, "installation was queued");
    const auto installed_settings = simulation.state().game_settings.find(install_game);
    check(installed_settings == simulation.state().game_settings.end() || installed_settings->second.preset_id.empty(),
          "install does not write the deleted preset id back into game settings");
}

// Game settings: Save as my preset… and Update “<name>”, what they say after, and why they can't.
void my_presets_game_settings(const std::filesystem::path& fixtures, const std::filesystem::path& profile) {
    playden::homebrew::Simulation simulation(fixtures, profile);
    load_settings_data(simulation, fixtures);
    // Update “<name>” on a game with a change of its own on one of the player's presets.
    check(simulation.scenario("my-presets") && simulation.action("circle"), "back to Game settings");
    const auto keys = top_modal(simulation).action_keys;
    check(top_modal(simulation).id == "game-settings" && keys.size() > 2 && keys[0] == "preset-picker" &&
              std::find(keys.begin(), keys.end(), "update-my-preset") != keys.end() &&
              std::find(keys.begin(), keys.end(), "save-my-preset") == keys.end(),
          "my presets: Game settings offers Update, not Save, on a preset of mine with own changes");
    const auto* row = find_row(simulation, "update-my-preset");
    check(row && row->title == "Update “Point and click”" && row->subtitle == "Every game using it changes too.",
          "my presets: the Update row names the preset");
    check(simulation.action("focus:update-my-preset") && simulation.action("cross"), "Update");
    check(top_modal(simulation).title == "Updated “Point and click”" && simulation.action("cross") &&
              !find_row(simulation, "update-my-preset") && !find_row(simulation, "save-my-preset"),
          "my presets: updated; the game has no changes left, so neither row");
    {
        // The row that opened the result is gone: focus lands on a row that exists, the sheet's focused one.
        const auto& sheet = top_modal(simulation);
        const auto& focus = simulation.state().navigation.focus_key;
        check(sheet.id == "game-settings" && std::find(sheet.action_keys.begin(), sheet.action_keys.end(), focus) != sheet.action_keys.end() &&
                  focus == sheet.action_keys[static_cast<std::size_t>(sheet.focused_action)],
              "my presets: after Update, focus is on a row of Game settings");
    }

    // Save as my preset…: the keyboard with the game's name; the game then uses the preset.
    check(simulation.scenario("my-presets-save"), "my-presets-save scenario loads");
    const auto game = SteamId(900012);
    row = find_row(simulation, "save-my-preset");
    check(row && row->title == "Save as my preset…" && simulation.state().navigation.focus_key == "save-my-preset" &&
              !find_row(simulation, "update-my-preset"),
          "my presets: Game settings offers Save on Playden default");
    check(simulation.action("cross") && simulation.state().editor.active && simulation.state().editor.purpose == "Preset name",
          "Save opens the keyboard");
    std::string title;
    for (const auto& known : simulation.state().games)
        if (known.id == game) title = known.title;
    check(simulation.state().editor.value == title, "my presets: the keyboard is prefilled with the game's name");
    simulation.SubmitEditorText("Point and click");
    check(simulation.state().editor.active && validation(simulation) == "You already have a preset with this name.",
          "my presets: Save refuses a name in use");
    simulation.SubmitEditorText("Overlay and 720p");
    check(!simulation.state().editor.active && top_modal(simulation).id == "confirmation" &&
              top_modal(simulation).title == "Saved “Overlay and 720p”" && top_modal(simulation).body.rfind("This game uses it now.", 0) == 0,
          "my presets: saved, and the game uses it");
    check(simulation.MyPresets().size() == 4 && simulation.state().game_settings.at(game).preset_id == simulation.MyPresets().back().preset.id &&
              simulation.state().game_settings.at(game).overrides.empty(),
          "my presets: the game moved onto the new preset");
    check(simulation.action("cross") && top_modal(simulation).id == "game-settings" && !find_row(simulation, "save-my-preset") &&
              find_row(simulation, "preset-picker")->value == "Overlay and 720p",
          "my presets: Game settings shows the new preset");

    // A game on a community recipe keeps it.
    check(simulation.scenario("recipes-settings"), "recipes-settings scenario loads");
    check(simulation.action("focus:save-my-preset") && simulation.action("cross"), "Save on a community recipe");
    simulation.SubmitEditorText("From a recipe");
    check(top_modal(simulation).title == "Saved “From a recipe”" &&
              top_modal(simulation).body.rfind("This game keeps its community recipe.", 0) == 0 &&
              !simulation.state().game_settings.at(SteamId(900002)).community_recipe.id.empty(),
          "my presets: a game on a community recipe keeps it");

    // Nothing to keep: said before the keyboard.
    check(simulation.scenario("recipes-none") && simulation.action("circle"), "a game on Playden default without changes");
    check(simulation.action("focus:save-my-preset") && simulation.action("cross"), "Save with nothing to keep");
    check(!simulation.state().editor.active && top_modal(simulation).title == "Nothing to save" &&
              top_modal(simulation).body == "This game uses only its own settings: change something first.",
          "my presets: nothing to keep says so, without the keyboard");

    // 50 presets: said before the keyboard.
    check(simulation.scenario("my-presets-full") && simulation.MyPresets().size() == 50, "my-presets-full scenario loads");
    check(simulation.action("cross"), "Save with 50 presets");
    check(!simulation.state().editor.active && top_modal(simulation).title == "The preset was not saved" &&
              top_modal(simulation).body == "You have 50 presets. Delete one to save another." && simulation.MyPresets().size() == 50,
          "my presets: the 50th is the last");
}

int main(int argc, char** argv) {
    check(argc == 2, "fixture directory argument is required");
    const std::filesystem::path fixtures(argv[1]);
    const auto profile = temp_profile();
    auto clean = [&] { std::error_code error; std::filesystem::remove(profile, error); };
    clean();
    synthetic_download_servers(fixtures, profile);
    clean();
    leftover_titles_simulation_uses_controller_navigation(fixtures, profile);
    t01_authentication_ignores_late_approval(fixtures, profile);
    clean();
    t02_focus_and_nested_modal_restoration(fixtures, profile);
    clean();
    t03_search_filters_and_focus_reconciliation(fixtures, profile);
    clean();
    t03_circle_leaves_an_applied_search(fixtures, profile);
    clean();
    t04_organization_persists_without_deleting_games(fixtures, profile);
    clean();
    t05_install_rechecks_storage_and_submits_once(fixtures, profile);
    clean();
    t06_queue_transitions_are_single_active_and_safe(fixtures, profile);
    clean();
    t07_interruption_and_verification_recovery(fixtures, profile);
    clean();
    t08_nested_settings_restore_openers(fixtures, profile);
    clean();
    t08b_settings_sheet_rules(fixtures, profile);
    settings_revision_exhaustion_preserves_record(fixtures, profile);
    settings_sheet_details(fixtures, profile);
    settings_save_failure_does_not_claim_success(fixtures, profile);
    clean();
    community_recipes_chooser(fixtures, profile);
    clean();
    community_recipes_imported(fixtures, profile);
    community_recipes_search_and_link(fixtures, profile);
    community_old_share_is_the_steam_games(fixtures, profile);
    clean();
    community_recipes_one_base(fixtures, profile);
    clean();
    community_recipes_switch(fixtures, profile);
    clean();
    community_recipes_install(fixtures, profile);
    clean();
    community_recipes_updates_and_fallback(fixtures, profile);
    clean();
    community_sharing(fixtures, profile);
    clean();
    my_presets_chooser(fixtures, profile);
    clean();
    my_presets_game_settings(fixtures, profile);
    clean();
    t52_recipe_values_in_game_settings(fixtures, profile);
    clean();
    t09_uninstall_and_reset_preserve_declared_data(fixtures, profile);
    clean();
    t10_session_failure_running_stop_and_crash(fixtures, profile);
    clean();
    t11_preferences_failure_and_controller_modal_recovery(fixtures, profile);
    clean();
    t12_stress_dataset_keeps_logical_focus(fixtures, profile);
    clean();
    family_sharing_scenario_marks_shared_games(fixtures, profile);
    clean();
    game_update_scenarios(fixtures, profile);
    clean();
    t13_settings_sections_rail(fixtures, profile);
    clean();
    t14_tab_bar_from_the_top_of_a_page(fixtures, profile);
    clean();
    t15_password_sign_in(fixtures, profile);
    clean();
    t16_system_keyboard_text(fixtures, profile);
    clean();
    t17_import_sheet(fixtures, profile);
    clean();
    t18_imported_game_page(fixtures, profile);
    clean();
    public_contract_and_snapshot(fixtures, profile);
    std::error_code error;
    std::filesystem::remove(profile, error);
    std::cout << "homebrew_simulation_test passed\n";
}
