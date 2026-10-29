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
    const std::vector<std::string> top(keys.begin(), keys.begin() + std::min<std::size_t>(keys.size(), 23));
    check(top == std::vector<std::string>{"preset-picker", "heading-game", "setting-launch_option", "setting-extra_arguments",
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
    check(simulation.state().navigation.focus_key == "preset-picker", "T08b Up steps over a heading");
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
    check(simulation.action("down"), "focus the next preset");
    const auto& comparison = *simulation.view().comparison;
    check(!comparison.rows.empty() && !comparison.note.empty() && !comparison.title.empty(),
          "the comparison lists settings and closes with what applies");
    const auto* community = find_row(simulation, "preset-community");
    check(community && !community->enabled && community->value == "Not available yet", "community presets wait for the index");
    simulation.action("focus:preset-community");
    simulation.action("cross");
    check(simulation.state().navigation.modals.back().id == "preset-picker", "the community entry does nothing yet");
    simulation.action("circle");
    simulation.action("focus:preset-picker");
    simulation.action("r1");
    check(simulation.state().navigation.modals.back().focused_action == 5, "R1 jumps five rows");
    simulation.action("l1");
    check(simulation.state().navigation.modals.back().focused_action == 0, "L1 jumps back");
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
    check(row("details-update") && row("details-update")->title == "Update" && row("details-update-note") &&
              row("details-update-note")->title == "Update available" && row("details-update-note")->value == "Up to 1.2 GB",
          "update-available offers Update with its size");
    check(simulation.action("cross") && simulation.view().surface == "update" && row("update-confirm") &&
              row("update-confirm")->enabled && row("update-confirm")->value == "Up to 1.2 GB",
          "Update opens the update sheet");
    check(simulation.action("circle"), "the sheet closes");
    check(simulation.scenario("dlc-available") && row("details-update")->title == "DLC available",
          "dlc-available says DLC available");
    check(simulation.scenario("install-options") && simulation.view().surface == "install" && row("install-version") &&
              row("install-language") && row("install-dlc") && row("install-dlc")->value == "2 DLC",
          "install-options shows Version, Language and Includes");
    check(simulation.scenario("updating") && simulation.state().navigation.route == playden::homebrew::Route::Downloads,
          "updating opens the queue");
    check(simulation.action("details:900001") && row("details-play") && !row("details-play")->enabled &&
              row("details-play")->value == "Finishing the update…" && !row("details-update"),
          "an update putting its files in place locks Play");
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
                      {"Launcher.exe", "pe32", "This game needs .NET, which Playden's runtime doesn't include yet.", true, ""},
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
    activate(simulation, "export-recipe");
    check(simulation.state().navigation.modals.back().title == "Recipe exported", "T52 export says where it went");
    rt::CommunityRecipe community;
    std::string reason;
    check(rt::ParseCommunityRecipe(read_text(directory / "recipe-900001.json"), &community, &reason) &&
              community.tested_firmware == "13.60" && community.executable == "hl2.exe" &&
              community.recipe.dll_overrides == "d3d9=n" && community.recipe.locked.count("graphics"),
          "T52 the exported recipe reads back");
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
              row("import-program-1")->title == "Benchmark/RallyBench.exe" && row("import-program-1")->subtitle.empty() &&
              row("import-program-2")->subtitle == "Installer or tool" && simulation.view().rows.back().title == "Back" &&
              simulation.state().navigation.focus_key == "import-program-0",
          "T17 the program list puts the game first and labels helpers");
    simulation.action("cross");
    check(simulation.view().surface == "import-choice" && simulation.view().title == "Where will the game files live?" &&
              simulation.view().eyebrow == "Import games · Old School Rally" &&
              simulation.view().subtitle == "You can import it into Playden later from the game's page." &&
              row("import-play-in-place")->title == "Play from the USB drive" &&
              row("import-play-in-place")->subtitle == "Nothing is copied. Keep the drive plugged in." &&
              row("import-copy")->title == "Import into Playden" && row("import-copy")->value == "938 MB" &&
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
              row("details-copy")->title == "Import into Playden" && row("details-remove")->title == "Remove from library" &&
              row("details-folder")->title == "Plays from /mnt/usb0/playden/import/Night Sky" && !row("details-cloud"),
          "T18 an imported game's page plays in place and offers Import and Remove");
    // Import into Playden: the wizard's drive choice at once; Back closes it.
    press("details-copy");
    check(simulation.view().surface == "import-storage" && simulation.view().title == "Choose storage" &&
              simulation.view().subtitle == "412 MB will be copied there." &&
              simulation.view().eyebrow == "Import games · Night Sky" &&
              row("volume-external")->subtitle == "Default · Available" && row("volume-external")->enabled &&
              row("volume-internal")->subtitle == "Available" && row("volume-internal")->enabled &&
              !row("volume-readonly")->enabled && row("volume-readonly")->subtitle == "No write access" &&
              simulation.state().navigation.focus_key == "volume-external" &&
              simulation.view().legends.front() == "Cross Import here" && simulation.view().rows.back().title == "Back",
          "T18 Import into Playden from the page opens the drive choice, the default drive focused");
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
              row("import-copy")->title == "Import into Playden" && row("import-copy")->value == "8.9 GB" &&
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
