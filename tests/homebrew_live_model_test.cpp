// homebrew_live_model_test.cpp - Live homebrew model boundary tests.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: MIT

#include "homebrew/simulation.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>
#include "steam_id.h"

namespace {

using playden::homebrew::AccountUpdate;
using playden::homebrew::AuthState;
using playden::homebrew::ModelMode;
using playden::homebrew::Route;
using playden::homebrew::ServiceAction;
using playden::homebrew::Simulation;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void activate(Simulation& model, const std::string& key) {
    check(model.action("focus:" + key), "requested live row must exist");
    check(model.action("cross"), "requested live row must activate");
}

bool has_action(const std::vector<playden::homebrew::ServiceRequest>& actions, ServiceAction expected) {
    return std::find(actions.begin(), actions.end(), expected) != actions.end();
}

Simulation make_live(const std::filesystem::path& base) {
    return Simulation(base / "fixtures-that-do-not-exist", base / "missing" / "profile.json", ModelMode::Live);
}

void live_starts_clean_and_never_reads_or_writes_fixtures(const std::filesystem::path& base) {
    auto model = make_live(base);
    const auto& state = model.state();
    check(!state.simulation_mode && state.account().authentication == AuthState::SignedOut,
          "live model starts signed out and identifies itself as live");
    check(state.games.empty() && state.jobs.empty() && state.installations.empty() && state.volumes.empty(),
          "live model does not inherit fixture library or operation state");
    check(state.runtime == playden::homebrew::RuntimeState::Unavailable && state.clock_ticks == 0,
          "live model starts without synthetic runtime readiness or fixture time");
    check(!model.view().show_simulation_label && model.snapshot().find("\"simulation\":false") != std::string::npos,
          "live presentation and diagnostics do not claim simulation mode");
    check(model.last_error().empty(), "missing fixture and profile paths are not consulted in live mode");

    const auto clock = state.clock_ticks;
    check(!model.scenario("default"), "scenario API is unavailable in live mode");
    check(!model.action("scenario:qr-approved") && !model.action("inject:qr-approved")
              && !model.action("route:library") && !model.action("install:1:external")
              && !model.action("runtime-retry"),
          "synthetic scenario, injection, route, install and runtime actions are unavailable live");
    model.tick(100);
    check(model.state().clock_ticks == clock && model.state().account().authentication == AuthState::SignedOut,
          "manual ticks cannot advance live authentication or fixture time");
    check(!std::filesystem::exists(base / "missing" / "profile.json"),
          "live interactions do not create the comparison profile");

    model.SetControllerConnected(false);
    check(!model.state().controller_connected, "production controller connection state has an explicit setter");
}

void live_dispatches_bounded_service_effects_and_replaces_challenges(const std::filesystem::path& base) {
    auto model = make_live(base);
    activate(model, "startup-connect");
    auto actions = model.TakeServiceActions();
    check(actions.size() == 1 && actions.front() == ServiceAction::BeginQr,
          "connect dispatches one QR begin effect");
    check(model.TakeServiceActions().empty(), "taking service actions drains the queue");

    model.UpdateAccount({AuthState::Waiting, {}, {}, "https://example.invalid/first-secret", {}, false});
    check(model.state().account().challenge_url == "https://example.invalid/first-secret",
          "external challenge is accepted for presentation");
    model.UpdateAccount({AuthState::Waiting, {}, {}, "https://example.invalid/replacement-secret", {}, false});
    check(model.state().account().challenge_url == "https://example.invalid/replacement-secret",
          "new external challenge replaces the prior challenge");
    check(model.snapshot().find("replacement-secret") == std::string::npos,
          "diagnostic snapshot omits the private challenge URL");
    model.UpdateAccount({AuthState::Expired, {}, {}, {}, "Code expired", false});
    check(model.state().account().challenge_url.empty() && model.view().subtitle == "Code expired",
          "external update can clear the challenge and expose a static recovery error");

    activate(model, "qr-refresh");
    model.action("circle");
    actions = model.TakeServiceActions();
    check(actions.size() == 2 && actions[0] == ServiceAction::BeginQr && actions[1] == ServiceAction::CancelQr,
          "retry and cancel dispatch ordered service effects without synthetic approval");

    for (int index = 0; index < 40; ++index) {
        activate(model, "startup-connect");
        model.action("circle");
    }
    actions = model.TakeServiceActions();
    check(actions.size() == 32, "pending service effects are bounded to 32 entries");
}

void failed_signout_preserves_account_until_external_clear(const std::filesystem::path& base) {
    auto model = make_live(base);
    model.UpdateAccount({AuthState::SignedIn, "76561198000000001", "Synthetic Tester", {}, {}, false});
    check(model.state().navigation.route == Route::Home && !model.state().account().cache_available,
          "authentication success does not invent a delivered cache");
    check(model.snapshot().find("76561198000000001") == std::string::npos
              && model.snapshot().find("Synthetic Tester") == std::string::npos,
          "live snapshot omits account identity");

    model.action("l1");
    check(model.state().navigation.route == Route::Settings, "controller navigation reaches account settings");
    activate(model, "settings-signout");
    activate(model, "confirm-destructive");
    auto actions = model.TakeServiceActions();
    check(has_action(actions, ServiceAction::SignOut) && model.state().account().authentication == AuthState::SigningOut,
          "confirmed signout dispatches a barrier and waits for the service result");
    check(model.state().account().id == "76561198000000001",
          "pending signout does not clear account state before durable service completion");

    model.UpdateAccount({AuthState::Failed, "76561198000000001", "Synthetic Tester", {}, "Sign out failed", false});
    check(model.state().account().authentication == AuthState::Failed
              && model.state().account().id == "76561198000000001"
              && model.state().account().error == "Sign out failed"
              && model.view().subtitle == "Sign out failed",
          "failed signout preserves the account and shows the external error without fake success");

    model.UpdateAccount({AuthState::SignedOut, {}, {}, {}, {}, true});
    check(model.state().account().id.empty() && model.state().account().display_name.empty()
              && model.state().games.empty() && model.state().navigation.modals.empty()
              && model.state().navigation.route == Route::Startup,
          "confirmed external account clear removes account-specific state and restores safe startup focus");
}

void account_commands_dispatch_refresh_and_switch_barriers(const std::filesystem::path& base) {
    auto refresh = make_live(base);
    refresh.UpdateAccount({AuthState::SignedIn, "76561198000000002", "Refresh Tester", {}, {}, false});
    refresh.action("l1");
    activate(refresh, "settings-account");
    activate(refresh, "account-sync");
    auto actions = refresh.TakeServiceActions();
    check(actions.size() == 1 && actions.front() == ServiceAction::RefreshLibrary
              && refresh.state().account().authentication == AuthState::SignedIn
              && refresh.state().library_loading && refresh.state().library_syncing,
          "account sync dispatches refresh and remains pending external completion");

    auto switching = make_live(base);
    switching.UpdateAccount({AuthState::SignedIn, "76561198000000003", "Switch Tester", {}, {}, false});
    switching.action("l1");
    activate(switching, "settings-account");
    activate(switching, "account-switch");
    activate(switching, "confirm-destructive");
    actions = switching.TakeServiceActions();
    check(actions.size() == 1 && actions.front() == ServiceAction::SwitchAccount
              && switching.state().account().authentication == AuthState::SigningOut
              && switching.state().account().id == "76561198000000003",
          "confirmed account switch dispatches a barrier without prematurely clearing identity");
}

void settings_survive_account_restore_and_cache_action_is_honest(const std::filesystem::path& base) {
    std::filesystem::create_directories(base);
    std::filesystem::permissions(base, std::filesystem::perms::owner_all);
    const auto file = base / "game-settings.json";
    playden::runtime::GameSettingsRecord stored;
    stored.revision = 3;
    stored.overrides["resolution"] = "1280x720";
    std::string reason;
    check(playden::runtime::SaveGameSettingsFile(file.string(), {{SteamId(220), stored}}, &reason), "seed live settings");
    auto model = make_live(base);
    model.SetGameSettingsFile(file);
    model.UpdateAccount({AuthState::Restoring, {}, {}, {}, {}, true});
    check(model.state().game_settings.count(SteamId(220)) && model.state().game_settings.at(SteamId(220)) == stored,
          "initial account restore retains device game settings");
    model.UpdateAccount({AuthState::SignedOut, {}, {}, {}, {}, true});
    model.SetWallClock(100);
    std::map<playden::GameId, playden::runtime::GameSettingsRecord> loaded;
    check(playden::runtime::LoadGameSettingsFile(file.string(), &loaded, &reason) && loaded.at(SteamId(220)) == stored,
          "sign-out does not erase settings from disk");
    model.UpdateAccount({AuthState::SignedIn, "account", "Owner", {}, {}, false});
    playden::GameSummary game;
    game.id = SteamId(220);
    game.title = "Half-Life 2";
    check(model.UpdateLibrary({{game}, false, {}, false, true, 100}), "settings game library delivered");
    {
        std::ofstream corrupt(file);
        corrupt << "malformed settings";
    }
    model.SetGameSettingsFile(file);
    check(model.state().game_settings.count(SteamId(220)) && model.state().game_settings.at(SteamId(220)) == stored,
          "failed settings reload preserves the current records");
    model.action("r1");
    check(!model.state().navigation.modals.empty() &&
              model.state().navigation.modals.back().title == "Game settings could not be read",
          "a settings load failure waits in a dialog");
    activate(model, "alert-ok");
    std::ifstream unchanged(file);
    std::string contents;
    std::getline(unchanged, contents);
    check(contents == "malformed settings", "load failure blocks destructive automatic save");
    check(model.action("details:220"), "open settings game details");
    activate(model, "details-more");
    check(std::none_of(model.view().rows.begin(), model.view().rows.end(),
                       [](const auto& row) { return row.key == "game-settings"; }),
          "live settings wait until the game has a prepared PS5 title");
}

const playden::homebrew::ViewRow* find_row(const Simulation& model, const std::string& key) {
    const auto& rows = model.view().rows;
    const auto found = std::find_if(rows.begin(), rows.end(), [&key](const auto& row) { return row.key == key; });
    return found == rows.end() ? nullptr : &*found;
}

bool has_notice(const Simulation& model, const std::string& id) {
    const auto& notices = model.state().notices;
    return std::any_of(notices.begin(), notices.end(), [&id](const auto& notice) { return notice.id == id; });
}

// 120 Hz output and the Frame limit choices it brings can be chosen while the game's title waits for its kit
// update: they are saved, show as chosen and say they apply after the update; Reset takes them back.
void live_120hz_waits_for_the_title_update(const std::filesystem::path& base) {
    std::filesystem::create_directories(base);
    auto model = make_live(base);
    model.SetGameSettingsFile(base / "display-settings.json");
    model.SetSettingsData({{"playden-default", "Playden default", "Most games", "", {}}}, {});
    model.SetKitCapabilities(playden::runtime::KitCapabilitiesFromManifest({"dxvk", "display-120hz"}, true));
    model.UpdateAccount({AuthState::SignedIn, "account", "Owner", {}, {}, false});
    playden::GameSummary game;
    game.id = SteamId(220);
    game.title = "Half-Life 2";
    check(model.UpdateLibrary({{game}, false, {}, false, true, 100}), "display game library delivered");
    model.UpdateInstalls({}, {{SteamId(220), 1024, "internal", true}});
    playden::homebrew::LiveRuntimeGame runtime;
    runtime.game_id = SteamId(220);
    runtime.status = "Kit update available";
    runtime.prepared = runtime.has_title = true;
    runtime.runtime_current = false;
    runtime.launch_entry_id = "game";
    runtime.activity = playden::homebrew::TitleActivity::Stopped;
    model.UpdateRuntime(playden::homebrew::RuntimeState::Ready, {runtime});
    check(model.action("details:220"), "open the display game's details");
    activate(model, "details-more");
    activate(model, "game-settings");
    const std::string after_update = playden::runtime::kAppliesAfterKitUpdate;
    const auto says_after_update = [&](const char* key) {
        const auto* row = find_row(model, key);
        return row && row->enabled && row->subtitle.rfind(after_update, 0) == 0;
    };
    check(says_after_update("setting-refresh") && find_row(model, "setting-refresh")->value == "60 Hz",
          "120 Hz output can be chosen before the title's update and says when it applies");
    activate(model, "setting-frame_limit");
    const auto* limit_120 = find_row(model, "choice-1");
    check(limit_120 && limit_120->title == "120" && !limit_120->enabled && limit_120->subtitle == "Only with Refresh rate 120 Hz.",
          "Frame limit 120 needs 120 Hz output first");
    check(find_row(model, "choice-2") && find_row(model, "choice-2")->enabled, "Frame limit 60 stays available");
    check(model.action("circle"), "close the Frame limit picker");

    activate(model, "setting-refresh");
    activate(model, "choice-1");
    check(says_after_update("setting-refresh") && find_row(model, "setting-refresh")->value == "120 Hz" &&
              find_row(model, "setting-refresh")->modified,
          "the saved 120 Hz output shows as the player's choice, waiting for the update");
    activate(model, "setting-frame_limit");
    limit_120 = find_row(model, "choice-1");
    check(limit_120 && limit_120->enabled && limit_120->subtitle == after_update,
          "with 120 Hz output chosen, Frame limit 120 can be chosen and waits for the update");
    check(find_row(model, "choice-3") && find_row(model, "choice-3")->enabled, "Frame limit 40 can be chosen too");
    check(find_row(model, "choice-2")->subtitle.empty(), "Frame limit 60 applies now, as DXVK's cap");
    activate(model, "choice-1");
    check(says_after_update("setting-frame_limit") && find_row(model, "setting-frame_limit")->value == "120",
          "the saved Frame limit 120 shows as chosen, waiting for the update");
    const auto& saved = model.state().game_settings.at(SteamId(220)).overrides;
    check(saved.count("refresh") && saved.at("refresh") == "120" && saved.count("frame_limit") && saved.at("frame_limit") == "120",
          "both choices are saved at once");
    check(find_row(model, "preset-picker")->value == "Custom · from Playden default", "the sheet marks the settings as changed");
    activate(model, "setting-frame_limit");
    check(find_row(model, "choice-1")->selected && find_row(model, "choice-1")->value == "Your choice" &&
              !find_row(model, "choice-0")->selected,
          "the picker marks the saved choice, not the value that applies until the update");
    check(model.action("circle"), "close the Frame limit picker again");

    runtime.status = "Ready";
    runtime.ready = true;
    runtime.runtime_current = true;
    model.UpdateRuntime(playden::homebrew::RuntimeState::Ready, {runtime});
    const auto* refresh = find_row(model, "setting-refresh");
    check(refresh && refresh->enabled && refresh->value == "120 Hz" && refresh->subtitle.find(after_update) == std::string::npos,
          "an updated title applies the saved 120 Hz output");
    const auto* limit = find_row(model, "setting-frame_limit");
    check(limit && limit->value == "120" && limit->subtitle.find(after_update) == std::string::npos,
          "an updated title applies the saved Frame limit");
    activate(model, "setting-frame_limit");
    check(find_row(model, "choice-1")->enabled && find_row(model, "choice-1")->subtitle.empty(), "Frame limit 120 applies now");
    check(model.action("circle"), "close the Frame limit picker on the updated title");

    // Frame limit follows Refresh rate: back at 60 Hz, a cap of 120 goes to Off and the toast says so; 60 stays.
    activate(model, "setting-refresh");
    activate(model, "choice-0");
    const auto& after_60 = model.state().game_settings.at(SteamId(220)).overrides;
    check(!after_60.count("refresh") && !after_60.count("frame_limit") &&
              find_row(model, "setting-frame_limit")->value == "Off",
          "Refresh rate 60 Hz sets a cap of 120 back to Off");
    const auto toast = std::find_if(model.state().notices.begin(), model.state().notices.end(),
                                    [](const playden::homebrew::Notice& notice) { return notice.id == "frame-limit-off"; });
    check(toast != model.state().notices.end() && toast->message == "Frame limit set to Off" &&
              toast->detail == "A cap of 120 needs Refresh rate 120 Hz.",
          "the toast says why the cap went to Off");
    activate(model, "setting-refresh");
    activate(model, "choice-1");
    activate(model, "setting-frame_limit");
    activate(model, "choice-2");
    activate(model, "setting-refresh");
    activate(model, "choice-0");
    check(model.state().game_settings.at(SteamId(220)).overrides.count("frame_limit") &&
              model.state().game_settings.at(SteamId(220)).overrides.at("frame_limit") == "60",
          "a cap of 60 stays at 60 Hz");
    // Triangle on Refresh rate takes 120 Hz back, and a cap of 40 with it.
    activate(model, "setting-refresh");
    activate(model, "choice-1");
    activate(model, "setting-frame_limit");
    activate(model, "choice-3");
    check(model.action("focus:setting-refresh") && model.action("triangle"), "reset Refresh rate with Triangle");
    check(!model.state().game_settings.at(SteamId(220)).overrides.count("refresh") &&
              !model.state().game_settings.at(SteamId(220)).overrides.count("frame_limit"),
          "resetting Refresh rate sets a cap of 40 back to Off");

    // Reset takes the waiting choices back.
    runtime.status = "Kit update available";
    runtime.ready = false;
    runtime.runtime_current = false;
    model.UpdateRuntime(playden::homebrew::RuntimeState::Ready, {runtime});
    activate(model, "reset-settings");
    check(model.state().game_settings.at(SteamId(220)).overrides.empty(), "Reset removes the waiting choices");
    check(find_row(model, "setting-refresh")->value == "60 Hz" && find_row(model, "setting-frame_limit")->value == "Off",
          "after Reset the rows show the preset's values");
}

// A preset's cap of 120 falls back to Off at 60 Hz without being Off: a player's 40 set back by Refresh rate 60 Hz is
// saved as Off, so 120 Hz later does not bring the preset's cap back.
void live_refresh_rate_keeps_off_over_a_preset_cap(const std::filesystem::path& base) {
    std::filesystem::create_directories(base);
    auto model = make_live(base);
    model.SetGameSettingsFile(base / "capped-settings.json");
    model.SetSettingsData({{"playden-default", "Playden default", "Most games", "", {{"refresh", "120"}, {"frame_limit", "120"}}}}, {});
    model.SetKitCapabilities(playden::runtime::KitCapabilitiesFromManifest({"dxvk", "display-120hz"}, true));
    model.UpdateAccount({AuthState::SignedIn, "account", "Owner", {}, {}, false});
    playden::GameSummary game;
    game.id = SteamId(220);
    game.title = "Half-Life 2";
    check(model.UpdateLibrary({{game}, false, {}, false, true, 100}), "capped game library delivered");
    model.UpdateInstalls({}, {{SteamId(220), 1024, "internal", true}});
    playden::homebrew::LiveRuntimeGame runtime;
    runtime.game_id = SteamId(220);
    runtime.status = "Ready";
    runtime.prepared = runtime.has_title = runtime.ready = runtime.runtime_current = true;
    runtime.launch_entry_id = "game";
    runtime.activity = playden::homebrew::TitleActivity::Stopped;
    model.UpdateRuntime(playden::homebrew::RuntimeState::Ready, {runtime});
    check(model.action("details:220"), "open the capped game's details");
    activate(model, "details-more");
    activate(model, "game-settings");
    check(find_row(model, "setting-frame_limit")->value == "120", "the preset caps the game at 120");
    activate(model, "setting-frame_limit");
    activate(model, "choice-3");
    activate(model, "setting-refresh");
    activate(model, "choice-0");
    const auto& overrides = model.state().game_settings.at(SteamId(220)).overrides;
    check(overrides.count("frame_limit") && overrides.at("frame_limit") == "off",
          "a cap set back over the preset's 120 is saved as Off");
    activate(model, "setting-refresh");
    activate(model, "choice-1");
    check(find_row(model, "setting-frame_limit")->value == "Off", "120 Hz again keeps Off, not the preset's 120");
}

// Maintenance › Update runtime runs the update Play does first for a title on an older kit's runtime, without
// starting the game: only while the title waits for it and the game is closed.
void live_update_runtime_from_maintenance(const std::filesystem::path& base) {
    using playden::homebrew::MaintenanceKind;
    using playden::homebrew::TitleActivity;
    using Requests = std::vector<std::pair<playden::homebrew::GameId, MaintenanceKind>>;
    std::filesystem::create_directories(base);
    auto model = make_live(base);
    model.SetGameSettingsFile(base / "update-settings.json");
    model.SetSettingsData({{"playden-default", "Playden default", "Most games", "", {}}}, {});
    model.SetKitCapabilities(playden::runtime::KitCapabilitiesFromManifest({"dxvk", "display-120hz"}, true));
    model.UpdateAccount({AuthState::SignedIn, "account", "Owner", {}, {}, false});
    playden::GameSummary game;
    game.id = SteamId(220);
    game.title = "Half-Life 2";
    check(model.UpdateLibrary({{game}, false, {}, false, true, 100}), "update game library delivered");
    model.UpdateInstalls({}, {{SteamId(220), 1024, "internal", true}});
    playden::homebrew::LiveRuntimeGame runtime;
    runtime.game_id = SteamId(220);
    runtime.status = "Kit update available";
    runtime.prepared = runtime.has_title = runtime.can_prepare = true;
    runtime.runtime_current = false;
    runtime.launch_entry_id = "game";
    runtime.activity = TitleActivity::Stopped;
    model.UpdateRuntime(playden::homebrew::RuntimeState::Ready, {runtime});
    check(model.action("details:220"), "open the stale game's details");
    activate(model, "details-more");
    activate(model, "game-maintenance");
    check(!find_row(model, "maintenance-update")->enabled, "Update runtime waits for the console's answer");
    model.TakeActivityRequests();
    model.UpdateRuntime(playden::homebrew::RuntimeState::Ready, {runtime});
    const auto* update = find_row(model, "maintenance-update");
    check(update && update->enabled && update->title == "Update runtime" &&
              update->subtitle == "Moves the game to the installed kit's runtime without starting it." &&
              update->badge == "Game must be closed",
          "Update runtime is offered for a closed game whose title waits for its kit update");

    // Not while the game runs.
    runtime.activity = TitleActivity::Running;
    model.UpdateRuntime(playden::homebrew::RuntimeState::Ready, {runtime});
    update = find_row(model, "maintenance-update");
    check(update && !update->enabled && update->subtitle == "Quit Half-Life 2 to change this.", "Update runtime waits while the game runs");
    (void)model.action("focus:maintenance-update");
    (void)model.action("cross");
    check(model.TakeMaintenanceRequests().empty(), "a running game's title is not updated");
    // Nor while its setup is queued or running.
    runtime.activity = TitleActivity::Stopped;
    runtime.can_prepare = false;
    model.UpdateRuntime(playden::homebrew::RuntimeState::Ready, {runtime});
    update = find_row(model, "maintenance-update");
    check(update && !update->enabled && update->subtitle == "Wait for the game's setup to finish.", "Update runtime waits for setup");
    runtime.can_prepare = true;
    model.UpdateRuntime(playden::homebrew::RuntimeState::Ready, {runtime});

    activate(model, "maintenance-update");
    check(model.state().navigation.modals.back().title == "Update the runtime?", "Update runtime asks first");
    activate(model, "confirm-destructive");
    check(model.TakeMaintenanceRequests() == Requests{{SteamId(220), MaintenanceKind::UpdateRuntime}},
          "a confirmed Update runtime goes to the runtime");
    check(model.TakeRuntimePlayRequests().empty(), "Update runtime does not start the game");
    check(find_row(model, "maintenance-update")->value == "Working…" && !find_row(model, "maintenance-repair")->enabled,
          "the update shows its progress and holds the other actions");
    runtime.maintenance = MaintenanceKind::UpdateRuntime;
    runtime.maintenance_running = true;
    runtime.status = "Preparing";
    runtime.can_prepare = false;
    model.UpdateRuntime(playden::homebrew::RuntimeState::Ready, {runtime});
    update = find_row(model, "maintenance-update");
    check(update && !update->enabled && update->value == "Working…" &&
              update->subtitle == "Moves the game to the installed kit's runtime without starting it.",
          "the running update keeps saying what it does");
    runtime.can_prepare = true;
    runtime.maintenance_running = false;
    runtime.maintenance_ok = true;
    runtime.maintenance_message = "Runtime updated.";
    runtime.status = "Ready";
    runtime.ready = true;
    runtime.runtime_current = true;
    model.UpdateRuntime(playden::homebrew::RuntimeState::Ready, {runtime});
    check(has_notice(model, "maintenance-220"), "the finished update reports its outcome");
    update = find_row(model, "maintenance-update");
    check(update && !update->enabled && update->subtitle == "Runtime updated.", "the row shows the outcome");
    runtime.maintenance.reset();
    model.UpdateRuntime(playden::homebrew::RuntimeState::Ready, {runtime});
    update = find_row(model, "maintenance-update");
    check(update && !update->enabled && update->subtitle == "Already up to date.", "a current title has nothing to update");
}

// A prepared game's settings and maintenance: rows that need the game closed wait for the
// console's answer, a closed sheet asks for the settings to be applied, and maintenance
// requests go to the runtime after a confirmation.
void live_settings_follow_the_prepared_title(const std::filesystem::path& base) {
    using playden::homebrew::MaintenanceKind;
    using playden::homebrew::TitleActivity;
    std::filesystem::create_directories(base);
    auto model = make_live(base);
    model.SetGameSettingsFile(base / "prepared-settings.json");
    model.SetSettingsData({{"playden-default", "Playden default", "Most games", "", {}},
                           {"mouse", "Mouse-driven game", "Point and click", "", {{"controller", "keyboard"}}}}, {});
    model.SetKitCapabilities(playden::runtime::KitCapabilitiesFromManifest({"settings-dxvk-config"}, true));
    model.UpdateAccount({AuthState::SignedIn, "account", "Owner", {}, {}, false});
    playden::GameSummary game;
    game.id = SteamId(220);
    game.title = "Half-Life 2";
    check(model.UpdateLibrary({{game}, false, {}, false, true, 100}), "prepared game library delivered");
    model.UpdateInstalls({}, {{SteamId(220), 1024, "internal", true}});
    playden::homebrew::LiveRuntimeGame runtime;
    runtime.game_id = SteamId(220);
    runtime.status = "Ready";
    runtime.ready = runtime.prepared = runtime.has_title = true;
    runtime.launch_entry_id = "game";
    model.UpdateRuntime(playden::homebrew::RuntimeState::Ready, {runtime});
    check(model.action("details:220"), "open prepared game details");
    activate(model, "details-more");
    check(find_row(model, "game-settings") && find_row(model, "game-maintenance"),
          "a prepared game's More menu offers Game settings and Maintenance");
    // A title waiting for recovery keeps Maintenance: Recreate bottle is the way out.
    {
        auto recovering = runtime;
        recovering.status = "Needs recovery";
        recovering.ready = recovering.prepared = false;
        recovering.activity = TitleActivity::Stopped;
        model.UpdateRuntime(playden::homebrew::RuntimeState::Ready, {recovering});
        activate(model, "cancel");
        activate(model, "details-more");
        check(find_row(model, "game-maintenance") != nullptr, "a title needing recovery still offers Maintenance");
        activate(model, "game-maintenance");
        model.TakeActivityRequests();
        model.UpdateRuntime(playden::homebrew::RuntimeState::Ready, {recovering});  // the console's answer
        const auto* recreate = find_row(model, "maintenance-recreate");
        check(recreate && recreate->enabled, "Recreate bottle can run while the title waits for recovery");
        for (const char* key : {"maintenance-repair", "maintenance-shader", "maintenance-backup"}) {
            const auto* blocked = find_row(model, key);
            check(blocked && !blocked->enabled &&
                      blocked->subtitle == "Finish the PS5 title's recovery first, or use Recreate bottle.",
                  "Repair, Clear shader cache and Back up saves wait for the title's recovery");
        }
        for (const char* key : {"maintenance-repair", "maintenance-shader", "maintenance-backup"}) {
            (void)model.action(std::string("focus:") + key);
            (void)model.action("cross");
        }
        check(model.TakeMaintenanceRequests().empty(), "the rows waiting for recovery start nothing");
        const auto* reset = find_row(model, "maintenance-reset");
        check(reset && reset->subtitle.find("recovery") == std::string::npos, "Reset settings is unchanged during recovery");
        activate(model, "cancel");
        model.UpdateRuntime(playden::homebrew::RuntimeState::Ready, {runtime});
    }

    activate(model, "game-settings");
    check(model.TakeActivityRequests() == std::vector<playden::homebrew::GameId>{SteamId(220)}, "the sheet asks whether the game runs");
    activate(model, "section-compatibility");
    const auto* version = find_row(model, "setting-windows_version");
    check(version && !version->enabled && version->subtitle == "Checking whether Half-Life 2 is running…",
          "game-closed rows wait for the console's answer");
    runtime.activity = TitleActivity::Running;
    model.UpdateRuntime(playden::homebrew::RuntimeState::Ready, {runtime});
    check(find_row(model, "setting-windows_version")->subtitle == "Quit Half-Life 2 to change this.",
          "game-closed rows are disabled while the game runs");
    runtime.activity = TitleActivity::Stopped;
    model.UpdateRuntime(playden::homebrew::RuntimeState::Ready, {runtime});
    check(find_row(model, "setting-windows_version")->enabled, "game-closed rows open once the game is stopped");
    check(find_row(model, "setting-locale") && !find_row(model, "setting-locale")->enabled,
          "unbuilt rows stay listed and disabled");
    check(model.action("focus:setting-frame_limit") &&
              find_row(model, "setting-frame_limit")->subtitle.find("Also called [display] frame_rate · dxgi.maxFrameRate") != std::string::npos,
          "the focused row names its technical keys");

    activate(model, "setting-resolution");
    activate(model, "choice-0");
    check(model.action("circle"), "close the settings sheet");
    check(model.TakeMaintenanceRequests() ==
              std::vector<std::pair<playden::homebrew::GameId, MaintenanceKind>>{{SteamId(220), MaintenanceKind::ApplySettings}},
          "closing the sheet asks for the settings to be applied");
    check(has_notice(model, "settings-saved"), "closing the sheet announces the change");
    check(model.state().navigation.modals.back().id == "context", "closing Game settings returns to Game actions");
    check(find_row(model, "game-settings")->subtitle == "Changes apply on next launch",
          "the More menu says unpublished changes wait for the next launch");

    activate(model, "game-maintenance");
    check(model.TakeActivityRequests() == std::vector<playden::homebrew::GameId>{SteamId(220)}, "maintenance asks whether the game runs");
    check(!find_row(model, "maintenance-backup")->enabled, "maintenance waits for the console's answer");
    model.UpdateRuntime(playden::homebrew::RuntimeState::Ready, {runtime});
    check(find_row(model, "maintenance-backup")->enabled, "maintenance is open while the game is stopped");
    activate(model, "maintenance-backup");
    activate(model, "confirm-destructive");
    check(model.TakeMaintenanceRequests() ==
              std::vector<std::pair<playden::homebrew::GameId, MaintenanceKind>>{{SteamId(220), MaintenanceKind::BackupSaves}},
          "a confirmed maintenance action goes to the runtime");
    check(find_row(model, "maintenance-backup")->value == "Working…", "the running action shows its progress");
    runtime.maintenance = MaintenanceKind::BackupSaves;
    runtime.maintenance_running = true;
    model.UpdateRuntime(playden::homebrew::RuntimeState::Ready, {runtime});
    runtime.maintenance_running = false;
    runtime.maintenance_ok = true;
    runtime.maintenance_message = "Saves backed up.";
    runtime.save_backups = 1;
    runtime.latest_save_backup = 1760000000;
    model.UpdateRuntime(playden::homebrew::RuntimeState::Ready, {runtime});
    check(has_notice(model, "maintenance-220"), "a finished maintenance action reports its outcome");
    check(find_row(model, "maintenance-backup")->value.rfind("1 backup", 0) == 0, "the backup row counts the backups");
    runtime.activity = TitleActivity::Running;
    model.UpdateRuntime(playden::homebrew::RuntimeState::Ready, {runtime});
    check(!find_row(model, "maintenance-shader")->enabled, "maintenance waits while the game runs");
}


// A new program applies like any other setting: closing Game settings asks the runtime to apply
// it (RuntimeHost::RepublishSettings switches the prepared title's program), with no dialog, and
// Play starts at once. Reset to recipe goes back to Steam's launch option.
void live_program_change_applies_like_other_settings(const std::filesystem::path& base) {
    using playden::homebrew::GameId;
    using playden::homebrew::MaintenanceKind;
    using playden::homebrew::TitleActivity;
    using Requests = std::vector<std::pair<GameId, MaintenanceKind>>;
    std::filesystem::create_directories(base);
    auto model = make_live(base);
    model.SetGameSettingsFile(base / "program-settings.json");
    model.SetSettingsData({{"playden-default", "Playden default", "Most games", "", {}}}, {});
    model.UpdateAccount({AuthState::SignedIn, "account", "Owner", {}, {}, false});
    playden::GameSummary game;
    game.id = SteamId(22380);
    game.title = "Fallout: New Vegas";
    check(model.UpdateLibrary({{game}, false, {}, false, true, 100}), "program: library delivered");
    model.UpdateInstalls({}, {{SteamId(22380), 1024, "internal", true}});
    // Prepared for a program picked earlier, so the recipe lists it first, as on the console.
    playden::runtime::GameRecipe recipe;
    recipe.id = playden::runtime::GeneratedRecipeId(22380);
    recipe.version = 2;
    recipe.app_id = 22380;
    recipe.name = game.title;
    recipe.launch_entries = {{"program", "FalloutNV.exe", "FalloutNV.exe", "", ""},
                             {"launch-1", "Fallout: New Vegas", "FalloutNVLauncher.exe", "", ""}};
    model.SetGeneratedRecipes({recipe});
    std::map<GameId, playden::homebrew::LiveGameSetup> setups;
    setups[SteamId(22380)].scanned = true;
    setups[SteamId(22380)].programs = {{"FalloutNV.exe", "pe32", "", false, ""},
                              {"FalloutNVLauncher.exe", "pe32", "", false, ""},
                              {"Redists/vcredist_x86.exe", "pe32", "", false, ""}};
    model.SetGameSetups(setups);
    playden::homebrew::LiveRuntimeGame runtime;
    runtime.game_id = SteamId(22380);
    runtime.status = "Ready";
    runtime.ready = runtime.prepared = runtime.has_title = runtime.can_play = true;
    runtime.launch_entry_id = "launch-1";
    runtime.activity = TitleActivity::Stopped;
    model.UpdateRuntime(playden::homebrew::RuntimeState::Ready, {runtime});
    check(model.action("details:22380"), "program: open details");

    // The Program picker lists each program once: Steam's launcher as its entry, the earlier pick as
    // the folder program it is, and the subtitle names the default by its label.
    activate(model, "details-more");
    activate(model, "game-settings");
    (void)model.TakeActivityRequests();
    activate(model, "setting-launch_option");
    check(!find_row(model, "choice-0") && find_row(model, "choice-1") && find_row(model, "program-0") &&
              !find_row(model, "program-1") && find_row(model, "program-2"),
          "picker: no program is listed twice");
    check(model.view().subtitle.find("Recreate") == std::string::npos, "program: no Recreate bottle is needed");
    activate(model, "program-0");
    check(find_row(model, "setting-launch_option")->badge.empty() &&
              find_row(model, "setting-launch_option")->value == "FalloutNV.exe",
          "program: the Program row is an ordinary setting");
    check(model.action("circle") && model.state().navigation.modals.size() == 1 &&
              model.state().navigation.modals.back().id == "context",
          "program: closing the sheet asks nothing");
    check(model.TakeMaintenanceRequests() == Requests{{SteamId(22380), MaintenanceKind::ApplySettings}},
          "program: closing the sheet applies the new program");
    model.action("circle");
    activate(model, "details-play");
    check(model.state().navigation.modals.empty() && model.TakeRuntimePlayRequests() == std::vector<GameId>{SteamId(22380)},
          "program: Play starts at once");

    // Reset to recipe goes back to Steam's launch option, not the program the recipe lists first.
    activate(model, "details-more");
    activate(model, "game-settings");
    activate(model, "setting-launch_option");
    check(model.view().subtitle.find("Your choice: FalloutNV.exe. Applies on next launch.") != std::string::npos,
          "picker: the choice is named by its program, not its id, and applies on next launch");
    activate(model, "value-reset");
    check(find_row(model, "setting-launch_option")->value == "Fallout: New Vegas", "program: reset goes back to Steam's");
    check(model.action("circle") && model.state().navigation.modals.back().id == "context" &&
              model.TakeMaintenanceRequests() == Requests{{SteamId(22380), MaintenanceKind::ApplySettings}},
          "program: the reset applies the same way");
}

void live_download_telemetry() {
    using namespace playden::homebrew;
    Simulation model({}, {}, ModelMode::Live);
    LiveInstallJob job; job.game_id = SteamId(275390); job.state = JobState::Downloading;
    job.done_bytes = 100000000; job.total_bytes = 1000000000;
    job.bytes_per_second = 40055603; job.eta_seconds = 300;
    model.UpdateInstalls({job}, {});
    check(model.state().jobs.size() == 1 && model.state().jobs[0].bytes_per_second == 40055603 &&
          model.state().jobs[0].eta_seconds == 300, "live transfer telemetry reaches the Downloads model");
    check(FormatDownloadRate(job.bytes_per_second, job.eta_seconds) == "38.2 MB/s · ~5 min left", "rate and ETA formatting");
    check(FormatDownloadRate(0, {}) == "0.0 MB/s · —", "stalled rate has no invented ETA");
    check(FormatDownloadRate(1048576, {}) == "1.0 MB/s · —", "unknown total keeps rate but hides ETA");
    check(FormatDownloadRate({}, 300) == "—", "unknown rate hides stale ETA");
    check(FormatDownloadRate(1048576, 59) == "1.0 MB/s · ~59 sec left", "short ETA");
    check(FormatDownloadRate(1048576, 61) == "1.0 MB/s · ~2 min left", "ETA rounds up");
    for (const auto state : {JobState::Paused, JobState::Queued, JobState::Failed}) {
        job.state = state; // stale incoming fields must never appear outside an active transfer
        model.UpdateInstalls({job}, {});
        check(!model.state().jobs[0].bytes_per_second && !model.state().jobs[0].eta_seconds, "inactive telemetry clears");
    }
    // An imported game's copy into Playden says "Copying" (tiles, Queue) where a store's download says
    // "Downloading"; other states and a job's own words read the same for both.
    const auto imported = playden::GameId::Local("6d1f3a52-0c55-4f3e-9b2a-5d1c7e0a9f11");
    LiveInstallJob copy; copy.game_id = imported; copy.state = JobState::Downloading;
    copy.done_bytes = 1; copy.total_bytes = 4;
    job.state = JobState::Downloading;
    model.UpdateInstalls({job, copy}, {});
    const auto find_job = [&](playden::GameId id) -> const Job& {
        for (const auto& value : model.state().jobs) if (value.game_id == id) return value;
        check(false, "job listed");
        return model.state().jobs.front();
    };
    check(JobStateText(find_job(SteamId(275390))) == "Downloading", "a store's download says Downloading");
    check(JobStateText(find_job(imported)) == "Copying", "an imported game's copy says Copying");
    for (int i = 0; i < 6 && model.state().navigation.route != Route::Downloads; ++i) model.action("r1");
    check(model.state().navigation.route == Route::Downloads, "R1 reaches the Queue");
    bool copying_row = false, downloading_row = false;
    for (const auto& row : model.view().rows) {
        if (row.kind != RowKind::Job) continue;
        copying_row = copying_row || (row.game_id == imported && row.value == "copying");
        downloading_row = downloading_row || (row.game_id == SteamId(275390) && row.value == "downloading");
    }
    check(copying_row && downloading_row, "Queue rows: copying for the import, downloading for the store's");
    copy.state = JobState::Paused;
    model.UpdateInstalls({copy}, {});
    check(JobStateText(find_job(imported)) == "Paused", "a paused copy says Paused");
    Job update; update.state = JobState::Downloading; update.game_id = imported; update.state_text = "Unpacking";
    check(JobStateText(update) == "Unpacking", "a job's own words come first");
}


// The kit-only install: offered before sign-in, downloaded while the player signs in, and listed on
// the Queue and in Settings until it is in place.
void live_kit_install(const std::filesystem::path& base) {
    using namespace playden::homebrew;
    std::filesystem::create_directories(base);
    auto model = make_live(base);
    check(model.state().navigation.route == Route::Startup, "kit: a first start shows Connect Steam");
    LiveSelfUpdate update;
    update.current = "0.6.0";
    update.kit_phase = "checking";
    model.UpdateSelfUpdate(update);
    check(model.state().navigation.modals.empty(), "kit: nothing is offered while it is looked for");
    update.kit_phase = "available";
    update.kit_download_bytes = 300000000;
    model.UpdateSelfUpdate(update);
    check(!model.state().navigation.modals.empty() &&
              model.state().navigation.modals.back().title == "Download the runtime kit?" &&
              model.state().navigation.modals.back().body.find("sign in") != std::string::npos,
          "kit: offered before sign-in, with the download size");
    check(model.action("cross") && model.state().navigation.modals.empty() &&
              model.TakeSelfUpdateRequests() == std::vector<std::string>{"kit-install"},
          "kit: Download asks for the kit");

    update.kit_phase = "downloading";
    update.kit_done = 100;
    update.kit_total = 400;
    model.UpdateSelfUpdate(update);
    // Cancelled: the kit is available again, but not offered again until the next check.
    update.kit_phase = "available";
    model.UpdateSelfUpdate(update);
    check(model.state().navigation.modals.empty(), "kit: a cancelled download isn't offered again");
    update.kit_phase = "downloading";
    model.UpdateSelfUpdate(update);
    const auto job = std::find_if(model.state().jobs.begin(), model.state().jobs.end(),
                                  [](const Job& item) { return item.id == "kit-install"; });
    check(job != model.state().jobs.end() && job->title == "Runtime kit" && job->completed_bytes == 100 &&
              job->stage_text == "Downloading the runtime kit",
          "kit: the download shows on the Queue");

    // Signed in while it downloads: Settings shows the progress.
    model.UpdateAccount({AuthState::SignedIn, "account", "Owner", {}, {}, false});
    check(model.UpdateLibrary({{}, false, {}, false, true, 100}), "kit: library delivered");
    for (int i = 0; i < 6 && model.state().navigation.route != Route::Settings; ++i) model.action("r1");
    check(find_row(model, "settings-kit") && find_row(model, "settings-kit")->value == "25%",
          "kit: Settings shows the progress");

    update.kit_phase = "installed";
    model.UpdateSelfUpdate(update);
    check(std::none_of(model.state().jobs.begin(), model.state().jobs.end(),
                       [](const Job& item) { return item.id == "kit-install"; }) &&
              !find_row(model, "settings-kit") &&
              std::any_of(model.state().notices.begin(), model.state().notices.end(),
                          [](const Notice& notice) { return notice.id == "kit-installed"; }),
          "kit: once in place, the job and row go and a notice says so");

    // Another kit is installed: the offer says it replaces it; a failed download says why.
    auto other = make_live(base / "other");
    LiveSelfUpdate replace;
    replace.current = "0.6.0";
    replace.kit_phase = "available";
    replace.kit_replaces = true;
    other.UpdateSelfUpdate(replace);
    check(!other.state().navigation.modals.empty() &&
              other.state().navigation.modals.back().title == "Replace the runtime kit?",
          "kit: replacing another kit is said");
    other.action("circle");
    replace.kit_phase = "downloading";
    other.UpdateSelfUpdate(replace);
    replace.kit_phase = "failed";
    replace.kit_error = "The kit download failed.";
    other.UpdateSelfUpdate(replace);
    // Like every alert, it waits for sign-in.
    other.UpdateAccount({AuthState::SignedIn, "account", "Owner", {}, {}, false});
    check(other.UpdateLibrary({{}, false, {}, false, true, 100}), "kit: library delivered");
    other.action("r1");  // a queued alert shows after the next press
    check(!other.state().navigation.modals.empty() &&
              other.state().navigation.modals.back().title == "Runtime kit download stopped",
          "kit: a failed download says why");

    // An update on offer brings the kit itself: the kit isn't offered on top of it.
    auto both = make_live(base / "both");
    LiveSelfUpdate newer;
    newer.current = "0.6.0";
    newer.phase = "available";
    newer.available = "0.7.0";
    newer.kit_phase = "available";
    both.UpdateSelfUpdate(newer);
    check(both.state().navigation.modals.empty(), "kit: no kit offer while a newer Playden is on offer");
}

// Playden's own update: the prompts, the Downloads job, the Settings row and the requests.
void live_self_update(const std::filesystem::path& base) {
    using namespace playden::homebrew;
    std::filesystem::create_directories(base);
    auto model = make_live(base);
    model.UpdateAccount({AuthState::SignedIn, "account", "Owner", {}, {}, false});
    check(model.UpdateLibrary({{}, false, {}, false, true, 100}), "update: library delivered");
    for (int i = 0; i < 6 && model.state().navigation.route != Route::Settings; ++i) model.action("r1");
    check(model.state().navigation.route == Route::Settings, "update: settings open");
    check(find_row(model, "settings-beta-updates") && !find_row(model, "settings-beta-updates")->selected,
          "beta updates default off");
    activate(model, "settings-beta-updates");
    check(find_row(model, "settings-beta-updates")->selected, "beta updates turn on");
    check(model.ExportPreferences().beta_updates, "beta opt-in exported for persistence");
    check(model.TakeSelfUpdateRequests() == std::vector<std::string>{"betas-on"}, "enabling betas switches the track");
    activate(model, "settings-beta-updates");
    check(model.TakeSelfUpdateRequests() == std::vector<std::string>{"betas-off"}, "disabling betas switches back");
    LiveSelfUpdate update;
    update.current = "0.7.1-beta.1";
    model.UpdateSelfUpdate(update);
    check(find_row(model, "settings-beta-wait") &&
          find_row(model, "settings-beta-wait")->subtitle ==
              "Playden moves to the next stable version when it is out.",
          "opting out on a beta explains waiting for stable");
    update.current = "0.2.0";
    model.UpdateSelfUpdate(update);
    check(find_row(model, "settings-update") && find_row(model, "settings-update")->value == "Check now",
          "update: Settings offers a check");
    activate(model, "settings-update");
    check(model.TakeSelfUpdateRequests() == std::vector<std::string>{"check"}, "update: the row asks for a check");

    update.phase = "checking";
    update.user_check = true;
    model.UpdateSelfUpdate(update);
    update.phase = "up-to-date";
    update.available = "0.2.0";
    model.UpdateSelfUpdate(update);
    check(std::any_of(model.state().notices.begin(), model.state().notices.end(),
                      [](const Notice& notice) { return notice.id == "self-update-check"; }),
          "update: a check from Settings says Playden is up to date");

    // A newer release: one prompt, with the download size.
    update.phase = "available";
    update.available = "0.3.0";
    update.download_bytes = 500000000;
    update.kit_needed = true;
    update.notes = "New things.";
    model.UpdateSelfUpdate(update);
    check(!model.state().navigation.modals.empty() &&
              model.state().navigation.modals.back().title == "Playden 0.3.0 is available" &&
              model.state().navigation.modals.back().body.find("runtime kit") != std::string::npos,
          "update: a newer release is offered");
    check(model.action("cross") && model.state().navigation.modals.empty() &&
              model.TakeSelfUpdateRequests() == std::vector<std::string>{"install"},
          "update: Download and install asks for the install");
    model.UpdateSelfUpdate(update);
    check(model.state().navigation.modals.empty(), "update: the same release is offered once");

    // Downloading and unpacking show on Downloads.
    update.phase = "downloading";
    update.part = "kit";
    update.done = 100;
    update.total = 400;
    model.UpdateSelfUpdate(update);
    const auto job = std::find_if(model.state().jobs.begin(), model.state().jobs.end(),
                                  [](const Job& item) { return item.id == "playden-update"; });
    check(job != model.state().jobs.end() && job->title == "Playden 0.3.0" &&
              job->stage_text == "Downloading the runtime kit" && job->completed_bytes == 100,
          "update: the download shows on Downloads");
    check(find_row(model, "settings-update")->value == "25%", "update: Settings shows the progress");
    model.UpdateInstalls({}, {});
    check(std::any_of(model.state().jobs.begin(), model.state().jobs.end(),
                      [](const Job& item) { return item.id == "playden-update"; }),
          "update: game downloads refreshing keep the update job");
    update.phase = "unpacking";
    update.part = "app";
    model.UpdateSelfUpdate(update);
    check(model.state().jobs.back().stage_text == "Unpacking Playden", "update: unpacking shows too");

    // Staged: the restart prompt; Restart asks to apply at quit and quits.
    update.phase = "staged";
    model.UpdateSelfUpdate(update);
    check(!model.state().navigation.modals.empty() &&
              model.state().navigation.modals.back().title == "Playden 0.3.0 is ready",
          "update: a staged update asks for a restart");
    check(model.action("cross") && model.state().quit_requested &&
              model.TakeSelfUpdateRequests() == std::vector<std::string>{"restart"},
          "update: Restart Playden applies the update at quit");

    // Go back: offered when a previous version is kept, confirmed, then Playden quits for it.
    auto going_back = make_live(base);
    going_back.UpdateAccount({AuthState::SignedIn, "account", "Owner", {}, {}, false});
    check(going_back.UpdateLibrary({{}, false, {}, false, true, 100}), "update: library delivered");
    for (int i = 0; i < 6 && going_back.state().navigation.route != Route::Settings; ++i) going_back.action("r1");
    LiveSelfUpdate kept;
    kept.current = "0.3.0";
    kept.previous = "0.2.0";
    going_back.UpdateSelfUpdate(kept);
    check(find_row(going_back, "settings-update-back") &&
              find_row(going_back, "settings-update-back")->title == "Go back to Playden 0.2.0",
          "update: Settings offers going back");
    activate(going_back, "settings-update-back");
    check(!going_back.state().navigation.modals.empty() &&
              going_back.state().navigation.modals.back().title == "Go back to Playden 0.2.0?" &&
              going_back.state().navigation.modals.back().focused_action == 1,
          "update: going back asks first, with Cancel focused");
    check(going_back.action("left") && going_back.action("cross") && going_back.state().quit_requested &&
              going_back.TakeSelfUpdateRequests() == std::vector<std::string>{"rollback"},
          "update: Go back puts the previous version back at quit");

    // A version the player went back from is listed in Settings but not offered in a dialog.
    auto declined = make_live(base);
    declined.UpdateAccount({AuthState::SignedIn, "account", "Owner", {}, {}, false});
    check(declined.UpdateLibrary({{}, false, {}, false, true, 100}), "update: library delivered");
    LiveSelfUpdate quiet;
    quiet.current = "0.2.0";
    quiet.phase = "available";
    quiet.available = "0.2.1";
    quiet.quiet = true;
    declined.UpdateSelfUpdate(quiet);
    check(declined.state().navigation.modals.empty(), "update: no dialog for a version gone back from");

    // After an update: "What's new" with the notes and the release page as a QR link, once; the notes
    // stay in Settings › Release notes.
    auto updated = make_live(base);
    updated.UpdateAccount({AuthState::SignedIn, "account", "Owner", {}, {}, false});
    check(updated.UpdateLibrary({{}, false, {}, false, true, 100}), "update: library delivered");
    for (int i = 0; i < 6 && updated.state().navigation.route != Route::Settings; ++i) updated.action("r1");
    LiveSelfUpdate fresh;
    fresh.current = "0.3.0";
    fresh.current_notes = "Faster downloads.";
    fresh.current_notes_url = "https://github.com/o/r/releases/tag/v0.3.0";
    fresh.finished_message = "Playden was updated to 0.3.0.";
    fresh.whats_new = true;
    updated.UpdateSelfUpdate(fresh);
    check(!updated.state().navigation.modals.empty() &&
              updated.state().navigation.modals.back().title == "What's new in Playden 0.3.0" &&
              updated.state().navigation.modals.back().body.find("Faster downloads.") != std::string::npos &&
              updated.state().navigation.modals.back().link == fresh.current_notes_url,
          "update: what's new opens with the notes and the release page");
    check(updated.TakeSelfUpdateRequests() == std::vector<std::string>{"acknowledge"}, "update: what's new is acknowledged");
    check(updated.action("cross") && updated.state().navigation.modals.empty(), "update: OK closes what's new");
    fresh.finished_message.clear();
    fresh.whats_new = false;
    updated.UpdateSelfUpdate(fresh);
    check(updated.state().navigation.modals.empty(), "update: what's new shows once");
    check(find_row(updated, "settings-release-notes") != nullptr, "update: Settings lists the release notes");
    activate(updated, "settings-release-notes");
    check(!updated.state().navigation.modals.empty() &&
              updated.state().navigation.modals.back().title == "Playden 0.3.0 release notes" &&
              updated.state().navigation.modals.back().link == fresh.current_notes_url,
          "update: the release notes open from Settings");

    // A failed install says why; the previous run's result shows once.
    auto failing = make_live(base);
    failing.UpdateAccount({AuthState::SignedIn, "account", "Owner", {}, {}, false});
    check(failing.UpdateLibrary({{}, false, {}, false, true, 100}), "update: library delivered");
    for (int i = 0; i < 6 && failing.state().navigation.route != Route::Settings; ++i) failing.action("r1");
    LiveSelfUpdate step;
    step.current = "0.2.0";
    step.phase = "downloading";
    failing.UpdateSelfUpdate(step);
    step.phase = "failed";
    step.error = "The kit download failed.";
    failing.UpdateSelfUpdate(step);
    check(!failing.state().navigation.modals.empty() &&
              failing.state().navigation.modals.back().title == "Playden update stopped" &&
              failing.state().navigation.modals.back().body == "The kit download failed.",
          "update: a failed install is reported");
    check(failing.action("cross") && failing.state().navigation.modals.empty(), "update: OK closes the report");
    step.phase = "idle";
    step.finished_message = "Playden was updated to 0.3.0.";
    failing.UpdateSelfUpdate(step);
    check(failing.TakeSelfUpdateRequests() == std::vector<std::string>{"acknowledge"} &&
              std::any_of(failing.state().notices.begin(), failing.state().notices.end(),
                          [](const Notice& notice) { return notice.id == "self-update-finished"; }),
          "update: the finished update is announced once");
}

// Without the root grant only "Root access is missing" shows; its one action quits to Home.
void live_root_access_missing() {
    Simulation model({}, {}, ModelMode::Live);
    const std::string error = "native data grant failed (code 2); helper stage=request status=unavailable error=2";
    model.ShowRootAccessMissing(error);
    const auto& view = model.view();
    check(model.state().navigation.route == Route::Startup && view.title == "Root access is missing",
          "root access: the screen shows its title");
    check(view.subtitle.find("Load kstuff and the autoloader, then open Playden again.") != std::string::npos,
          "root access: the body says how to fix it");
    const auto detail = std::find_if(view.rows.begin(), view.rows.end(),
                                     [](const auto& row) { return row.key == "root-detail"; });
    check(detail != view.rows.end() && detail->title == "Startup error: " + error && !detail->enabled,
          "root access: the startup error is a detail line");
    check(view.focused_row >= 0 && view.rows[static_cast<std::size_t>(view.focused_row)].key == "root-quit" &&
              view.rows[static_cast<std::size_t>(view.focused_row)].title == "Quit to Home",
          "root access: Quit to Home has focus");
    // Nothing else is reachable: no sign-in, Settings, search or tabs.
    for (const char* name : {"connect", "options", "circle", "triangle", "r1", "search", "route:settings", "down"})
        check(!model.action(name), "root access: other actions do nothing");
    check(model.TakeServiceActions().empty() && !model.state().quit_requested && !model.state().editor.active &&
              model.state().navigation.modals.empty() && model.state().navigation.route == Route::Startup,
          "root access: no service action, dialog or route change");
    check(model.action("cross") && model.state().quit_requested, "root access: Cross quits to Home");

    Simulation long_error({}, {}, ModelMode::Live);
    long_error.ShowRootAccessMissing(std::string(400, 'x'));
    const auto& rows = long_error.view().rows;
    const auto whole = std::find_if(rows.begin(), rows.end(), [](const auto& row) { return row.key == "root-detail"; });
    check(whole != rows.end() && whole->title.find(std::string(400, 'x')) != std::string::npos,
          "root access: a startup error with its diagnostics is shown whole");
    Simulation runaway_error({}, {}, ModelMode::Live);
    runaway_error.ShowRootAccessMissing(std::string(3000, 'x'));
    const auto& runaway_rows = runaway_error.view().rows;
    const auto shortened = std::find_if(runaway_rows.begin(), runaway_rows.end(), [](const auto& row) { return row.key == "root-detail"; });
    check(shortened != runaway_rows.end() && shortened->title.size() < 1100 && shortened->title.find("…") != std::string::npos,
          "root access: a runaway startup error is shortened");

    // Without the grant the FTP-made stubs fail the fallback's check too: that clause is left out.
    Simulation stubs({}, {}, ModelMode::Live);
    stubs.ShowRootAccessMissing("native data grant failed (code 13); launcher paths unavailable: existing Steam state "
                                "root is inaccessible; helper stage=target_preflight status=16 error=16");
    check(stubs.state().root_access_detail ==
              "native data grant failed (code 13); helper stage=target_preflight status=16 error=16",
          "root access: the helper diagnostic survives, the fallback path check does not");

    // The grant ran but its data failed: same screen, other words, no kstuff advice.
    Simulation granted({}, {}, ModelMode::Live);
    const std::string data_error = "managed runtime data root is inaccessible";
    granted.ShowRootAccessMissing(data_error, true);
    const auto& data_view = granted.view();
    check(data_view.title == "Playden can't start", "root access: a data failure after the grant has its own title");
    check(data_view.subtitle == "Playden has root access but can't use its data in /data/playden. "
                                "Restart Playden; if it stays, check that folder's permissions over FTP.",
          "root access: the body points at /data/playden");
    check(data_view.subtitle.find("kstuff") == std::string::npos, "root access: no kstuff advice after the grant");
    const auto data_detail = std::find_if(data_view.rows.begin(), data_view.rows.end(),
                                          [](const auto& row) { return row.key == "root-detail"; });
    check(data_detail != data_view.rows.end() && data_detail->title == "Startup error: " + data_error,
          "root access: the data failure keeps its detail line");
    check(data_view.focused_row >= 0 && data_view.rows[static_cast<std::size_t>(data_view.focused_row)].key == "root-quit",
          "root access: the data failure keeps Quit to Home");
    check(!granted.action("connect") && granted.action("cross") && granted.state().quit_requested,
          "root access: the data failure only quits to Home");

    Simulation fixture({}, {}, ModelMode::Simulation);
    fixture.ShowRootAccessMissing(error);
    check(!fixture.state().root_access_missing, "root access: the fixture model ignores it");
}

const playden::homebrew::ViewRow* find_view_row(const Simulation& model, const std::string& key) {
    for (const auto& row : model.view().rows) if (row.key == key) return &row;
    return nullptr;
}

// An imported game's Game settings: Program, and a "Steam app ID" that takes plain decimal only,
// shows the game's own steam_appid.txt when that one wins, and clears when emptied.
void live_imported_game_steam_app_id(const std::filesystem::path& base) {
    std::filesystem::create_directories(base);
    auto model = make_live(base);
    model.SetGameSettingsFile(base / "imported-settings.json");
    model.SetSettingsData({{"playden-default", "Playden default", "Most games", "", {}}}, {});
    const auto id = playden::GameId::Local("6d1f3a52-0c55-4f3e-9b2a-5d1c7e0a9f11");
    playden::GameSummary game;
    game.id = id;
    game.title = "Night Sky";
    playden::homebrew::LibraryUpdate update{{game}, false, {}, false, true, 100};
    update.source = playden::Store::kLocal;
    check(model.UpdateLibrary(update), "imported library delivered");
    model.UpdateExternalGames({{id, "usb0", true, ""}});
    playden::homebrew::LiveRuntimeGame runtime;
    runtime.game_id = id;
    runtime.status = "Ready";
    runtime.ready = runtime.prepared = runtime.has_title = true;
    model.UpdateRuntime(playden::homebrew::RuntimeState::Ready, {runtime});
    check(model.action("details:" + playden::ToKey(id)), "imported game details");
    activate(model, "details-more");
    check(find_view_row(model, "game-settings") && find_view_row(model, "import-remove") &&
              !find_view_row(model, "uninstall") && !find_view_row(model, "manage-saves"),
          "an imported game playing in place has Game settings and Remove from library, no Saves link");
    activate(model, "game-settings");
    const auto* row = find_view_row(model, "steam-app-id");
    check(row && row->title == "Steam app ID" && row->value == "None" &&
              row->subtitle == "Only for games that need Steam. Most run without it." && find_view_row(model, "setting-launch_option"),
          "Game settings lists Program and Steam app ID");
    activate(model, "steam-app-id");
    check(model.state().editor.active && model.state().editor.purpose == "Steam app ID" && model.state().editor.symbols,
          "Steam app ID opens the keyboard on its digits");
    model.SubmitEditorText("0440");
    const auto saved = [&] {
        const auto found = model.state().game_settings.find(id);
        return found == model.state().game_settings.end() ? 0u : found->second.steam_app_id;
    };
    check(saved() == 0 && std::any_of(model.state().notices.begin(), model.state().notices.end(),
                                      [](const auto& notice) { return notice.id == "setting-validation"; }),
          "a number that is not plain decimal is refused");
    model.SubmitEditorText("abc");
    check(saved() == 0, "letters are refused");
    model.SubmitEditorText("440");
    check(saved() == 440 && !model.state().editor.active && find_view_row(model, "steam-app-id")->value == "440" &&
              std::none_of(model.state().notices.begin(), model.state().notices.end(),
                           [](const auto& notice) { return notice.id == "setting-validation"; }),
          "a plain Steam app ID is saved");
    runtime.steam_app_id = 620;
    runtime.steam_app_from_game_file = true;
    model.UpdateRuntime(playden::homebrew::RuntimeState::Ready, {runtime});
    row = find_view_row(model, "steam-app-id");
    check(row && row->value == "620" && row->subtitle == "From the game's steam_appid.txt",
          "the game's own steam_appid.txt shows when it wins");
    activate(model, "steam-app-id");
    model.SubmitEditorText("");
    check(saved() == 0, "an empty Steam app ID clears it");
}

// The program chosen when a game was imported goes into its Game settings only while it has none
// saved: a Program the player reset (or changed) in Game settings stays theirs at the next prepare.
void live_imported_program_seeds_once(const std::filesystem::path& base) {
    std::filesystem::create_directories(base);
    std::filesystem::permissions(base, std::filesystem::perms::owner_all);
    const auto file = base / "seed-settings.json";
    const auto fresh = playden::GameId::Local("1b4e28ba-2fa1-41d2-883f-0016d3cca427");
    const auto reset = playden::GameId::Local("6fa459ea-ee8a-4ca4-894e-db77e160355e");
    const auto chosen = playden::GameId::Local("16fd2706-8baf-433b-82eb-8c7fada847da");
    playden::runtime::GameSettingsRecord was_reset;  // Program reset to the recipe's: no override, saved
    was_reset.revision = 2;
    playden::runtime::GameSettingsRecord was_chosen;
    was_chosen.revision = 1;
    was_chosen.overrides["program"] = "Other.exe";
    std::string reason;
    check(playden::runtime::SaveGameSettingsFile(file.string(), {{reset, was_reset}, {chosen, was_chosen}}, &reason),
          "seed: settings written");
    auto model = make_live(base);
    model.SetGameSettingsFile(file);
    check(model.SeedGameProgram(fresh, "Bin/Game.exe") &&
              model.state().game_settings.at(fresh).overrides.at("program") == "Bin/Game.exe",
          "seed: a game with no saved settings gets its program");
    check(!model.SeedGameProgram(fresh, "Bin/Other.exe") &&
              model.state().game_settings.at(fresh).overrides.at("program") == "Bin/Game.exe",
          "seed: written once");
    check(!model.SeedGameProgram(reset, "Bin/Game.exe") && !model.state().game_settings.at(reset).overrides.count("program") &&
              model.state().game_settings.at(reset).revision == 2,
          "seed: a Program the player reset stays reset");
    check(!model.SeedGameProgram(chosen, "Bin/Game.exe") &&
              model.state().game_settings.at(chosen).overrides.at("program") == "Other.exe",
          "seed: a Program the player chose stays");
    std::map<playden::GameId, playden::runtime::GameSettingsRecord> loaded;
    check(playden::runtime::LoadGameSettingsFile(file.string(), &loaded, &reason) &&
              loaded.at(fresh).overrides.at("program") == "Bin/Game.exe" && !loaded.at(reset).overrides.count("program"),
          "seed: saved at once");
}

// Steam options only for other branches don't say which program starts the game: once the download is
// there, setting up waits for the player's choice. The sheet lists installers and tools last, and the
// choice is kept as Steam's option (steam_launch), not as a Program, and sets the game up again.
void live_setup_asks_for_a_steam_program(const std::filesystem::path& base) {
    using playden::homebrew::LiveGameSetup;
    std::filesystem::create_directories(base);
    auto model = make_live(base);
    model.SetGameSettingsFile(base / "steam-program-settings.json");
    model.UpdateAccount({AuthState::SignedIn, "account", "Owner", {}, {}, false});
    playden::GameSummary monster;
    monster.id = SteamId(449610);
    monster.title = "Monster Boy";
    playden::GameSummary other;
    other.id = SteamId(220);
    other.title = "Half-Life 2";
    check(model.UpdateLibrary({{monster, other}, false, {}, false, true, 100}), "steam program: library delivered");
    model.UpdateInstalls({}, {{SteamId(449610), 1024, "internal", true}});
    playden::homebrew::LiveRuntimeGame runtime;
    runtime.game_id = SteamId(449610);
    runtime.status = "Not prepared";
    runtime.can_prepare = true;
    model.UpdateRuntime(playden::homebrew::RuntimeState::Ready, {runtime});
    LiveGameSetup setup;
    setup.steam_choices = {{"x86/Setup.exe", true}, {"x64/Launcher64.exe", false}, {"x86/Launcher.exe", false}};

    // Elsewhere, a notification says so; the game's Play row asks.
    check(model.action("details:220"), "steam program: another page");
    model.SetGameSetups({{SteamId(449610), setup}});
    check(std::any_of(model.state().notices.begin(), model.state().notices.end(),
                      [](const auto& notice) { return notice.id.rfind("steam-programs-", 0) == 0; }) &&
              model.state().navigation.modals.empty(),
          "steam program: a notification away from the game's page");
    check(model.action("details:449610"), "steam program: its page");
    const auto play = std::find_if(model.view().rows.begin(), model.view().rows.end(),
                                   [](const auto& row) { return row.key == "details-play"; });
    check(play != model.view().rows.end() && play->title == "Choose program" && play->enabled, "steam program: Play asks");
    activate(model, "details-play");
    check(model.view().surface == "steam-programs" && model.state().navigation.modals.back().action_keys.size() == 4,
          "steam program: every program and Cancel");
    const auto title = [&](const std::string& key) {
        for (const auto& row : model.view().rows) if (row.action == key) return row.title;
        return std::string();
    };
    check(title("steam-program-0") == "x64/Launcher64.exe" && title("steam-program-2") == "x86/Setup.exe",
          "steam program: installers and tools last");
    activate(model, "steam-program-1");
    const auto& record = model.state().game_settings.at(SteamId(449610));
    check(record.steam_launch == "x86/Launcher.exe" && record.overrides.empty(),
          "steam program: kept as Steam's option, not as a Program");
    const auto prepares = model.TakeRuntimePrepareRequests();
    check(std::count(prepares.begin(), prepares.end(), SteamId(449610)) == 1, "steam program: the game is set up again");
    check(model.view().surface != "steam-programs", "steam program: the sheet closes");
    // The runtime still reports the old wait until it takes the new request: no second question.
    model.SetGameSetups({{SteamId(449610), setup}});
    check(model.view().surface != "steam-programs", "steam program: a stale wait doesn't ask again");

    // Over the game's own page, the sheet opens by itself.
    model.SetGameSetups({});
    model.SetGameSetups({{SteamId(449610), setup}});
    check(model.view().surface == "steam-programs", "steam program: the sheet opens over the game's page");
}

} // namespace

int main() {
    live_root_access_missing();
    live_download_telemetry();
    const auto base = std::filesystem::canonical(std::filesystem::temp_directory_path())
        / ("playden-homebrew-live-model-test-" + std::to_string(getpid()));
    std::error_code error;
    std::filesystem::remove_all(base, error);

    settings_survive_account_restore_and_cache_action_is_honest(base);
    live_settings_follow_the_prepared_title(base);
    live_120hz_waits_for_the_title_update(base / "display");
    live_refresh_rate_keeps_off_over_a_preset_cap(base / "capped");
    live_update_runtime_from_maintenance(base / "update-runtime");
    live_program_change_applies_like_other_settings(base);
    live_imported_game_steam_app_id(base);
    live_imported_program_seeds_once(base);
    live_setup_asks_for_a_steam_program(base);
    live_starts_clean_and_never_reads_or_writes_fixtures(base);
    live_dispatches_bounded_service_effects_and_replaces_challenges(base);
    failed_signout_preserves_account_until_external_clear(base);
    account_commands_dispatch_refresh_and_switch_barriers(base);
    live_self_update(base);
    live_kit_install(base / "kit");

    std::filesystem::remove_all(base, error);
    std::cout << "homebrew_live_model_test passed\n";
    return 0;
}
