// Controller-only traversal of the v2 install and management journeys on the simulated backend.
// Each journey starts from a fixture scenario; every action after that is a d-pad, Cross, Circle,
// Options or L1/R1 press, so a row reachable only through replay setters fails here.
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
static bool has_row(Simulation& simulation, const std::string& key) {
    const auto& rows = simulation.view().rows;
    return std::any_of(rows.begin(), rows.end(), [&](const ViewRow& row) { return row.key == key; });
}
static void choose(Simulation& simulation, const std::string& key) {
    const auto& rows = simulation.view().rows;
    const auto target = std::find_if(rows.begin(), rows.end(), [&](const ViewRow& row) { return row.key == key; });
    if (target == rows.end()) {
        std::fprintf(stderr, "Unreachable controller row %s in %s; rows:", key.c_str(), simulation.view().surface.c_str());
        for (const auto& row : rows) std::fprintf(stderr, " %s", row.key.c_str());
        std::fprintf(stderr, "\n");
        std::exit(1);
    }
    const int index = static_cast<int>(std::distance(rows.begin(), target));
    for (int count = 0; count < 100 && simulation.view().focused_row != index; ++count)
        press(simulation, simulation.view().focused_row < index ? "down" : "up");
    if (simulation.state().navigation.focus_key != key)
        std::fprintf(stderr, "focus %s, wanted %s on %s\n", simulation.state().navigation.focus_key.c_str(), key.c_str(),
                     simulation.view().surface.c_str());
    require(simulation.state().navigation.focus_key == key, "navigation reaches requested visible row");
    press(simulation, "cross");
}
static void to_downloads(Simulation& simulation) {
    // Close any sheet first; R1/L1 change sections only from a section's root.
    for (int count = 0; count < 6 && !simulation.state().navigation.modals.empty(); ++count)
        press(simulation, "circle");
    for (int count = 0; count < 6 && simulation.state().navigation.route != Route::Downloads; ++count) {
        if (!simulation.action("r1")) std::fprintf(stderr, "r1 refused on %s\n", simulation.view().surface.c_str());
    }
    if (simulation.state().navigation.route != Route::Downloads)
        std::fprintf(stderr, "stuck on surface %s route %d\n", simulation.view().surface.c_str(),
                     static_cast<int>(simulation.state().navigation.route));
    require(simulation.state().navigation.route == Route::Downloads, "R1 reaches Downloads");
}
static const Job& job(Simulation& simulation, const std::string& id) {
    const auto& jobs = simulation.state().jobs;
    const auto found = std::find_if(jobs.begin(), jobs.end(), [&](const Job& job) { return job.id == id; });
    require(found != jobs.end(), "job exists");
    return *found;
}

int main(int argc, char** argv) {
    require(argc == 2, "fixture argument");
    const auto profile = std::filesystem::temp_directory_path() /
        ("playden-hui-management-tour-test-" + std::to_string(getpid()) + ".json");
    std::error_code error;
    std::filesystem::remove(profile, error);
    Simulation simulation(argv[1], profile);

    // Install: storage choice, space review and one queued job.
    simulation.scenario("install-ready");
    require(simulation.view().surface == "install", "install sheet shows required space and storage");
    const auto jobs_before = simulation.state().jobs.size();
    choose(simulation, "storage-picker");
    require(simulation.view().surface == "storage" && has_row(simulation, "volume-readonly"),
            "storage picker lists every volume, including unusable ones");
    choose(simulation, "volume-external");
    require(simulation.view().surface == "install", "choosing storage returns to the install sheet");
    choose(simulation, "install-confirm");
    require(simulation.state().jobs.size() == jobs_before + 1, "confirming queues exactly one job");
    require(simulation.state().navigation.route == Route::Downloads, "the new job is shown in Downloads");

    // Insufficient space blocks submission.
    simulation.scenario("insufficient-space");
    const auto blocked_before = simulation.state().jobs.size();
    choose(simulation, "install-confirm");
    require(simulation.state().jobs.size() == blocked_before, "insufficient space never queues a job");

    // Queue: progress, pause/resume, safe cancel and reordering.
    simulation.scenario("default");
    to_downloads(simulation);
    choose(simulation, "job-job-1");
    if (has_row(simulation, "job-pause-job-1")) {
        choose(simulation, "job-pause-job-1");
        simulation.tick(5);  // Pausing settles into Paused at a safe point
        choose(simulation, "job-job-1");
    }
    require(job(simulation, "job-1").state == JobState::Paused, "a transfer can be paused");
    choose(simulation, "job-resume-job-1");
    const auto progress = job(simulation, "job-1").completed_bytes;
    simulation.tick(30);
    require(job(simulation, "job-1").completed_bytes > progress, "progress advances and is inspectable");
    std::string queued;
    for (const auto& entry : simulation.state().jobs)
        if (entry.state == JobState::Queued) { queued = entry.id; break; }
    require(!queued.empty(), "the default queue has a queued job");
    choose(simulation, "job-" + queued);
    choose(simulation, "job-cancel-" + queued);
    require(simulation.state().navigation.focus_key == "cancel", "cancel confirmation defaults to decline");
    press(simulation, "circle");
    require(job(simulation, queued).state != JobState::Cancelled, "declining keeps the job");
    while (!simulation.state().navigation.modals.empty()) press(simulation, "circle");
    choose(simulation, "job-job-4");
    if (has_row(simulation, "job-up-job-4")) {
        const int order = job(simulation, "job-4").queue_order;
        choose(simulation, "job-up-job-4");
        require(job(simulation, "job-4").queue_order < order, "a queued job moves up");
    } else {
        press(simulation, "circle");
    }

    // Transfer and verification failures expose Retry.
    simulation.scenario("download-failed");
    to_downloads(simulation);
    choose(simulation, "job-job-1");
    choose(simulation, "job-retry-job-1");
    require(job(simulation, "job-1").state == JobState::Downloading, "a failed transfer retries");
    simulation.scenario("verification-failed");
    to_downloads(simulation);
    choose(simulation, "job-job-1");
    choose(simulation, "job-retry-job-1");
    require(job(simulation, "job-1").state != JobState::Failed, "a failed verification retries");

    // Interrupted work after restart or storage loss resumes without invented completion.
    simulation.scenario("restart-recovery");
    to_downloads(simulation);
    require(job(simulation, "job-1").state != JobState::Downloaded, "restart never invents completion");
    choose(simulation, "job-job-1");
    choose(simulation, "job-resume-job-1");
    require(job(simulation, "job-1").state == JobState::Downloading, "interrupted work resumes");

    // Installed game: Verify queues once; Uninstall confirms and keeps declared data.
    simulation.scenario("default");
    for (int count = 0; count < 6 && !simulation.state().navigation.modals.empty(); ++count) press(simulation, "circle");
    for (int count = 0; count < 6 && simulation.state().navigation.route != Route::Home; ++count) press(simulation, "l1");
    require(simulation.state().navigation.route == Route::Home, "L1 reaches Home");
    press(simulation, "cross");
    require(simulation.view().surface == "details", "Home opens details");
    if (!has_row(simulation, "details-play")) {
        std::fprintf(stderr, "first Home game is not installed\n");
        return 1;
    }
    const auto game = *simulation.state().navigation.detail_id;
    const auto verifies = [&] {
        return std::count_if(simulation.state().jobs.begin(), simulation.state().jobs.end(),
            [game](const Job& entry) { return entry.kind == "verify" && entry.game_id == game; });
    };
    const auto verify_before = verifies();
    press(simulation, "options");
    choose(simulation, "verify");
    require(verifies() == verify_before + 1, "Verify queues one verification");
    // Verify shows the queue; come back to the same game.
    for (int count = 0; count < 6 && !simulation.state().navigation.modals.empty(); ++count) press(simulation, "circle");
    for (int count = 0; count < 6 && simulation.state().navigation.route != Route::Home; ++count) press(simulation, "l1");
    press(simulation, "cross");
    require(simulation.state().navigation.detail_id == game, "Home reopens the same game");
    press(simulation, "options");
    choose(simulation, "uninstall");
    require(simulation.state().navigation.focus_key == "cancel", "uninstall defaults to decline");
    choose(simulation, "confirm-destructive");
    require(std::count_if(simulation.state().jobs.begin(), simulation.state().jobs.end(),
                [game](const Job& entry) { return entry.kind == "uninstall" && entry.game_id == game; }) == 1,
            "confirming Uninstall queues one uninstall operation");

    // Import: Skip at Startup, Import games from Home, add a folder to play from its drive and Play
    // it; then import it into Playden, follow the copy, and uninstall the copy.
    // A console with nothing queued: the earlier journeys' jobs stay with their own model.
    std::filesystem::remove(profile, error);
    Simulation importing(argv[1], profile);
    importing.scenario("signed-out");
    choose(importing, "startup-skip");
    require(importing.state().navigation.route == Route::Home, "Skip for now reaches Home");
    choose(importing, "home-import");
    require(importing.view().surface == "import-games", "Home's Import games opens the sheet");
    if (importing.state().import_candidates.empty()) {
        choose(importing, "import-look");
        importing.tick(1);
    }
    choose(importing, "import-candidate-usb0:playden/import/Old School Rally");
    require(importing.view().surface == "import-programs", "a folder lists its programs");
    choose(importing, "import-program-0");
    require(importing.view().surface == "import-choice" && has_row(importing, "import-copy"),
            "the program leads to the choice of where the game files live");
    choose(importing, "import-play-in-place");
    const auto imported = importing.state().navigation.detail_id.value_or(playden::GameId{});
    require(importing.state().navigation.route == Route::Details && imported.store == playden::Store::kLocal &&
                importing.state().navigation.detail_origin == Route::Home,
            "adding the game opens its page, which returns Home");
    require(has_row(importing, "details-copy") && has_row(importing, "details-remove") && !has_row(importing, "details-cloud"),
            "an imported game playing from its drive offers Copy and Remove, no Cloud saves");
    choose(importing, "details-play");
    require(importing.state().navigation.route == Route::Session && importing.state().session_game_id == imported,
            "Play launches the imported game");
    importing.tick(2);
    require(importing.state().session == SessionState::Running, "the imported game runs");
    choose(importing, "session-quit");
    if (importing.view().surface == "confirmation") choose(importing, "confirm-destructive");
    importing.tick(1);
    require(importing.state().navigation.route == Route::Details && importing.state().session == SessionState::Idle,
            "quitting the game returns to its page");
    choose(importing, "details-copy");
    require(importing.view().surface == "import-storage" && has_row(importing, "volume-internal") &&
                !has_row(importing, "import-play-in-place"),
            "Copy to storage asks for the drive");
    choose(importing, "volume-internal");
    require(importing.state().navigation.route == Route::Details && has_row(importing, "details-download") &&
                !has_row(importing, "details-copy"),
            "the page shows the copy's progress");
    // The fixture queue runs its own downloads first, one at a time.
    for (int tick = 0; tick < 600 && has_row(importing, "details-download"); ++tick) importing.tick(1);
    require(!has_row(importing, "details-download") && has_row(importing, "details-uninstall") &&
                !has_row(importing, "details-remove") && has_row(importing, "details-play"),
            "the copy finishes: the game plays from Playden and uninstalls");
    choose(importing, "details-uninstall");
    require(importing.state().navigation.focus_key == "cancel", "uninstall defaults to decline");
    choose(importing, "confirm-destructive");
    require(std::count_if(importing.state().jobs.begin(), importing.state().jobs.end(),
                [imported](const Job& entry) { return entry.kind == "uninstall" && entry.game_id == imported; }) == 1,
            "uninstalling the copy queues one uninstall operation");

    std::filesystem::remove(profile, error);
    std::puts("PASS: install, storage, space, queue, pause/resume, cancel, retry, recovery, verify, uninstall, import by controller");
}
