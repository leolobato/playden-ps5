#include "library/query.h"
// simulation.cpp - Deterministic state transitions for the Playden UI comparison.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: MIT

#include "homebrew/simulation.hpp"
#include "runtime/game_probe.h"

#include "app/diagnostic_log.h"
#include "homebrew/browse.hpp"
#include "homebrew/json.hpp"
#include "homebrew/queue_layout.hpp"
#include "homebrew/storage_sim.hpp"
#include "steam/install_volumes.h"
#include "stores/local/scanner.h"
#include "steam/backend/json.h"
#include "steam/secret.h"

#include <cstring>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <ctime>
#include <fcntl.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <set>
#include <string_view>
#include <utility>
#include <type_traits>

namespace playden::homebrew {
std::string FormatDownloadRate(std::optional<std::uint64_t> rate, std::optional<int> eta) {
    if (!rate) return "—";
    char text[64];
    std::snprintf(text, sizeof(text), "%.1f MB/s", static_cast<double>(*rate) / (1024.0 * 1024.0));
    std::string result(text);
    result += " · ";
    if (!eta || *eta < 0) return result + "—";
    if (*eta < 60) return result + "~" + std::to_string(*eta) + " sec left";
    if (*eta < 3600) return result + "~" + std::to_string((*eta + 59) / 60) + " min left";
    return result + "~" + std::to_string((static_cast<std::int64_t>(*eta) + 3599) / 3600) + " hr left";
}

namespace {
// "Go back to Playden <previous>" is for development: release builds (NDEBUG) don't offer it.
#ifdef NDEBUG
constexpr bool kOfferGoBack = false;
#else
constexpr bool kOfferGoBack = true;
#endif

constexpr std::uint64_t kTransferPerTick = 67'108'864;
constexpr std::int64_t kFixtureClock = 1'760'000'000;
constexpr int kLiveNoticeSeconds = 6;  // how long a live notification stays on screen
constexpr std::size_t kOrganizationJournalEntries = 256;
constexpr std::size_t kOrganizationJournalBytes = 256 * 1024;

struct StringBuilder {
    std::string value;
    StringBuilder& operator<<(std::string_view text) { value.append(text); return *this; }
    StringBuilder& operator<<(const std::string& text) { value.append(text); return *this; }
    StringBuilder& operator<<(const char* text) { value.append(text); return *this; }
    StringBuilder& operator<<(char character) { value.push_back(character); return *this; }
    template <typename Integer>
        requires std::is_integral_v<Integer>
    StringBuilder& operator<<(Integer number) { value.append(std::to_string(number)); return *this; }
    // A game's key; no game is written as 0, as the numeric app ID was.
    StringBuilder& operator<<(const GameId& id) { value.append(id.valid() ? ToKey(id) : "0"); return *this; }
};

std::string read_file(const std::filesystem::path& path) {
    std::FILE* input = std::fopen(path.string().c_str(), "rb");
    if (!input) return {};
    std::string result;
    char buffer[4096];
    while (const auto count = std::fread(buffer, 1, sizeof(buffer), input)) result.append(buffer, count);
    std::fclose(input);
    return result;
}

std::string string_value(const json::Value* value, std::string fallback = {}) {
    if (value) if (const auto* text = value->string()) return *text;
    return fallback;
}

std::uint64_t uint_value(const json::Value* value, std::uint64_t fallback = 0) {
    if (value) if (const auto number = value->number()) return static_cast<std::uint64_t>(*number);
    return fallback;
}

// Fixture and snapshot files name games by Steam app ID.
GameId steam_value(const json::Value* value) {
    const auto number = uint_value(value);
    return number <= std::numeric_limits<std::uint32_t>::max() ? GameId::Steam(static_cast<std::uint32_t>(number)) : GameId{};
}

// The profile file names games by Steam app ID (0: none).
std::string app_text(const GameId& id) { return std::to_string(id.steam_app().value_or(0)); }

// The game a row key or action names; invalid (selects nothing) unless it is a canonical key.
GameId key_id(std::string_view key) { return ParseKey(key).value_or(GameId{}); }

bool bool_value(const json::Value* value, bool fallback = false) {
    if (value) if (const auto boolean = value->boolean()) return *boolean;
    return fallback;
}

std::optional<std::uint64_t> optional_uint(const json::Value* value) {
    if (!value) return std::nullopt;
    if (const auto number = value->number()) return static_cast<std::uint64_t>(*number);
    return std::nullopt;
}

std::string route_name(Route route) {
    switch (route) {
    case Route::Startup: return "startup";
    case Route::SignIn: return "qr";
    case Route::Home: return "home";
    case Route::Library: return "library";
    case Route::Downloads: return "downloads";
    case Route::Settings: return "settings";
    case Route::Details: return "details";
    case Route::Session: return "session";
    case Route::Saves: return "saves";
    case Route::SaveDetails: return "save-details";
    }
    return "home";
}

std::string auth_name(AuthState state) {
    switch (state) {
    case AuthState::SignedOut: return "signed-out";
    case AuthState::Restoring: return "restoring";
    case AuthState::Waiting: return "waiting";
    case AuthState::Expired: return "expired";
    case AuthState::Failed: return "failed";
    case AuthState::Cancelled: return "cancelled";
    case AuthState::Syncing: return "syncing";
    case AuthState::SigningOut: return "signing-out";
    case AuthState::SignedIn: return "signed-in";
    case AuthState::Offline: return "offline";
    }
    return "signed-out";
}

AuthState parse_auth_state(const std::string& state) {
    if (state == "waiting") return AuthState::Waiting;
    if (state == "expired") return AuthState::Expired;
    if (state == "failed") return AuthState::Failed;
    if (state == "cancelled") return AuthState::Cancelled;
    if (state == "syncing") return AuthState::Syncing;
    if (state == "signed-in") return AuthState::SignedIn;
    if (state == "offline") return AuthState::Offline;
    return AuthState::SignedOut;
}

std::string format_bytes(std::uint64_t bytes) {
    if (bytes == 0) return {};
    char text[32];
    if (bytes >= 1000ull * 1000 * 1000) std::snprintf(text, sizeof(text), "%.1f GB", static_cast<double>(bytes) / 1e9);
    else std::snprintf(text, sizeof(text), "%.0f MB", std::max(1.0, static_cast<double>(bytes) / 1e6));
    return text;
}

std::string job_state_name(JobState state) {
    switch (state) {
    case JobState::Queued: return "queued";
    case JobState::Downloading: return "downloading";
    case JobState::Pausing: return "pausing";
    case JobState::Paused: return "paused";
    case JobState::Verifying: return "verifying";
    case JobState::Downloaded: return "downloaded";
    case JobState::Failed: return "failed";
    case JobState::Cancelled: return "cancelled";
    case JobState::Interrupted: return "interrupted";
    }
    return "queued";
}

// job_state_name, but an imported game's copy says "copying".
std::string job_state_value(const Job& job) {
    if (job.state == JobState::Downloading && job.game_id.store == Store::kLocal) return "copying";
    return job_state_name(job.state);
}

std::string job_stage_name(JobStage stage) {
    switch (stage) {
    case JobStage::Download: return "download";
    case JobStage::Verify: return "verify";
    case JobStage::Install: return "install";
    case JobStage::Uninstall: return "uninstall";
    }
    return "download";
}

JobState parse_job_state(const std::string& state) {
    if (state == "downloading") return JobState::Downloading;
    if (state == "pausing") return JobState::Pausing;
    if (state == "paused") return JobState::Paused;
    if (state == "verifying") return JobState::Verifying;
    if (state == "downloaded" || state == "completed") return JobState::Downloaded;
    if (state == "failed") return JobState::Failed;
    if (state == "cancelled") return JobState::Cancelled;
    if (state == "interrupted") return JobState::Interrupted;
    return JobState::Queued;
}

JobStage parse_job_stage(const std::string& stage) {
    if (stage == "verify" || stage == "verifying") return JobStage::Verify;
    if (stage == "install") return JobStage::Install;
    if (stage == "uninstall") return JobStage::Uninstall;
    return JobStage::Download;
}

const Game* find_game(const State& state, GameId id) {
    const auto found = std::find_if(state.games.begin(), state.games.end(),
                                    [id](const Game& game) { return game.id == id; });
    return found == state.games.end() ? nullptr : &*found;
}

Installation* find_installation(State& state, GameId id) {
    const auto found = std::find_if(state.installations.begin(), state.installations.end(),
                                    [id](const Installation& installation) { return installation.game_id == id; });
    return found == state.installations.end() ? nullptr : &*found;
}

const Installation* find_installation(const State& state, GameId id) {
    return find_installation(const_cast<State&>(state), id);
}

bool is_installed(const State& state, GameId id) {
    const auto found = std::find_if(state.installations.begin(), state.installations.end(),
                                    [id](const Installation& installation) { return installation.game_id == id; });
    return found != state.installations.end() && found->installed_bytes > 0;
}

int next_queue_order(const State& state) {
    int order = -1;
    for (const auto& job : state.jobs) order = std::max(order, job.queue_order);
    return order + 1;
}

Job* find_job(State& state, const std::string& id) {
    const auto found = std::find_if(state.jobs.begin(), state.jobs.end(),
                                    [&id](const Job& job) { return job.id == id; });
    return found == state.jobs.end() ? nullptr : &*found;
}

Volume* find_volume(State& state, const std::string& id) {
    const auto found = std::find_if(state.volumes.begin(), state.volumes.end(),
                                    [&id](const Volume& volume) { return volume.id == id; });
    return found == state.volumes.end() ? nullptr : &*found;
}

std::string game_key(GameId id);

void clear_profile_customizations(State& state) { state.game_settings.clear(); }

namespace rt = playden::runtime;

std::string choice_label(const rt::SettingDefinition& definition, const std::string& value) {
    for (const auto& choice : definition.choices)
        if (choice.value == value) return choice.label;
    return value;
}

// Bindings offered for a controller button; index 0 inherits the recipe or preset value.
const std::vector<std::pair<std::string, std::string>>& common_bindings() {
    static const std::vector<std::pair<std::string, std::string>> bindings = {
        {"", "Inherit"}, {"none", "Nothing"}, {"escape", "Escape"}, {"enter", "Enter"}, {"space", "Space"},
        {"tab", "Tab"}, {"shift", "Shift"}, {"ctrl", "Ctrl"}, {"alt", "Alt"}, {"e", "E"}, {"f", "F"}, {"q", "Q"},
        {"r", "R"}, {"1", "1"}, {"2", "2"}, {"3", "3"}, {"4", "4"}, {"f5", "F5 (quick save)"}, {"f9", "F9 (quick load)"},
        {"mouse_left", "Left click"}, {"mouse_right", "Right click"}, {"mouse_middle", "Middle click"}};
    return bindings;
}

std::string binding_label(const std::string& value) {
    if (value.empty()) return "Not set";
    for (const auto& [binding, label] : common_bindings())
        if (binding == value) return label;
    return value;
}

const char* source_label(rt::ValueSource source) {
    switch (source) {
    case rt::ValueSource::kRecipe: return "From the game";
    case rt::ValueSource::kPreset: return "From preset";
    case rt::ValueSource::kUser: return "Your choice";
    default: return "Playden default";
    }
}

void reconcile_library_selection(State& state) {
    if (state.navigation.library_rail_focused) return;
    const auto games = filtered_games(state);
    const auto current = std::find_if(games.begin(), games.end(), [&state](const Game* game) {
        return game->id == state.navigation.selected_game_id;
    });
    if (current != games.end()) {
        state.navigation.selected_index = static_cast<int>(std::distance(games.begin(), current));
        state.navigation.focus_key = game_key(state.navigation.selected_game_id);
    } else if (!games.empty()) {
        state.navigation.selected_index = std::clamp(state.navigation.selected_index, 0, static_cast<int>(games.size()) - 1);
        state.navigation.selected_game_id = games[static_cast<std::size_t>(state.navigation.selected_index)]->id;
        state.navigation.focus_key = game_key(state.navigation.selected_game_id);
    } else {
        state.navigation.selected_index = 0;
        state.navigation.selected_game_id = {};
        state.navigation.focus_key = state.library.query.empty() ? "library-empty" : "clear-search";
    }
    state.navigation.scroll_offsets["library-grid"] = state.navigation.selected_index / 6;
}

std::string game_key(GameId id) { return "game-" + ToKey(id); }

std::vector<std::string> context_actions(bool live) {
    if (live) return {"favorite", "collection", "note", "hide", "uninstall", "cancel"};
    return {"favorite", "collection", "note", "hide", "verify", "uninstall", "logs", "cancel"};
}

constexpr const char* uninstall_body_kept = "The game's files and its PS5 title are removed from this console. Its saves and "
                                            "settings are kept, and installing the game again brings them back.";
constexpr const char* uninstall_body_kept_imported = "The game's files and its PS5 title are removed from this console. Its saves and "
                                                     "settings are kept, and adding the game again brings them back.";
constexpr const char* uninstall_body_deleted = "The game's files, its PS5 title and its saves and settings on this console are "
                                               "deleted.";
constexpr const char* uninstall_batch_body_kept = "Their files and PS5 titles are removed from this console. Their saves and "
                                                  "settings are kept, and installing a game again brings them back.";
constexpr const char* uninstall_batch_body_deleted = "Their files, PS5 titles and saves and settings on this console are deleted.";

// The name kept game data is shown with: the game's name, else the library's, else its app id.
std::string kept_name(const State& state, const LiveKeptBottle& kept) {
    if (!kept.name.empty()) return kept.name;
    if (const auto* game = find_game(state, kept.game_id); game && !game->title.empty()) return game->title;
    return "Steam app " + ToKey(kept.game_id);
}

std::string kept_count(std::size_t count) {
    return std::to_string(count) + (count == 1 ? " game" : " games");
}

// Saves are small: "84 KB", "1.2 MB".
std::string save_size(std::uint64_t bytes) {
    char text[32];
    if (bytes == 0) return "0 KB";
    if (bytes >= 1000ull * 1000 * 1000) std::snprintf(text, sizeof(text), "%.1f GB", static_cast<double>(bytes) / 1e9);
    else if (bytes >= 1000ull * 1000) std::snprintf(text, sizeof(text), "%.1f MB", static_cast<double>(bytes) / 1e6);
    else std::snprintf(text, sizeof(text), "%.0f KB", std::max(1.0, static_cast<double>(bytes) / 1e3));
    return text;
}

// "just now", "5 min ago", "3 h ago", "yesterday", "4 days ago".
std::string time_ago(std::int64_t now, std::int64_t then) {
    if (then <= 0 || now <= 0) return {};
    const auto seconds = std::max<std::int64_t>(0, now - then);
    if (seconds < 60) return "just now";
    if (seconds < 3600) return std::to_string(seconds / 60) + " min ago";
    if (seconds < 86'400) return std::to_string(seconds / 3600) + " h ago";
    if (seconds < 2 * 86'400) return "yesterday";
    return std::to_string(seconds / 86'400) + " days ago";
}

constexpr std::array<const char*, 5> kSavesFilters = {"All", "Needs you", "In sync", "This console only", "Not installed"};

// What the Saves tab says about one game's saves.
struct SaveFacts {
    std::string label;   // status
    std::string tone;    // good, busy, attention, problem, quiet
    std::string reason;  // the line under the name
    bool busy = false;   // checking, waiting in a job or moving saves: no actions
    bool needs_you = false;
    bool in_sync = false;
    bool local_only = false;
};

SaveFacts save_facts(const LiveCloudGame* cloud, bool installed) {
    SaveFacts facts;
    const auto status = cloud ? cloud->status : CloudStatus::None;
    if (status == CloudStatus::Checking) {
        facts = {"Checking…", "busy", "", true};
    } else if (status == CloudStatus::Syncing) {
        const auto direction = cloud->downloads > 0 && cloud->uploads == 0 ? "Downloading"
                               : cloud->uploads > 0 && cloud->downloads == 0 ? "Uploading" : "Syncing";
        facts = {std::string(direction) + (cloud->total_bytes > 0
                     ? " " + std::to_string(cloud->moved_bytes * 100 / cloud->total_bytes) + "%" : std::string("…")),
                 "busy", "", true};
    } else if (cloud && cloud->after_play) {
        facts = {"Will sync after play", "busy", "The game may be running"};
        facts.in_sync = true;
    } else {
        switch (status) {
        case CloudStatus::Synced:
        case CloudStatus::UpToDate: facts = {"In sync", "good", ""}; facts.in_sync = true; break;
        case CloudStatus::Conflict:
            facts = cloud->first_sync        ? SaveFacts{"Choose saves", "attention", "First sync · both sides already have saves"}
                    : cloud->first_overwrite ? SaveFacts{"Choose saves", "attention", "First upload would replace Cloud saves"}
                                             : SaveFacts{"Conflict", "problem", "Both changed since the last sync"};
            facts.needs_you = true;
            break;
        case CloudStatus::NeedsAccount:
            facts = {"Not in Cloud yet", "attention", "Saves here were never uploaded to this Steam account"};
            facts.needs_you = true;
            break;
        case CloudStatus::Failed: facts = {"Couldn't sync", "problem", cloud->message}; facts.needs_you = true; break;
        case CloudStatus::Unsupported: facts = {"This console only", "quiet", "No Steam Cloud for this game"}; facts.local_only = true; break;
        case CloudStatus::Waiting:
            facts = {installed ? "Cloud has saves" : "Cloud is newer", "quiet",
                     installed ? cloud->message : "They come down when you reinstall or press Sync now"};
            break;
        default: facts = {"Not checked yet", "quiet", ""}; break;
        }
        // A game further down a running check keeps its last known state; only the game being
        // checked or synced shows that. Its actions still wait for the check.
        if (cloud && cloud->queued) facts.busy = true;
    }
    if (!installed) facts.reason = facts.reason.empty() ? "Not installed" : "Not installed · " + facts.reason;
    return facts;
}

// The player's choices that decide whether the game starts at all: the program from the game
// folder, its type and folder, and "Try anyway". A preset or a reset keeps them, so a game
// that starts only with them keeps starting; the Program row resets them.
bool start_choice(const std::string& id) {
    return id == "program" || id == "architecture" || id == "program_folder" || id == "start_check";
}
// A different program, or none, makes the old program's folder, arguments and "Try anyway" moot.
void forget_program_choices(std::map<std::string, std::string>* overrides) {
    for (const char* key : {"program", "architecture", "program_folder", "program_arguments", "start_check"})
        overrides->erase(key);
}
bool has_resettable(const std::map<std::string, std::string>& overrides) {
    return std::any_of(overrides.begin(), overrides.end(), [](const auto& entry) { return !start_choice(entry.first); });
}
// A game-relative path as the bottle config and the Program picker compare it: '/' separated, lower
// case, "." for none.
std::string BottlePath(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    std::transform(path.begin(), path.end(), path.begin(),
                   [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    return path == "." ? std::string() : path;
}

void clear_keeping_start_choices(std::map<std::string, std::string>* overrides) {
    for (auto it = overrides->begin(); it != overrides->end();) it = start_choice(it->first) ? std::next(it) : overrides->erase(it);
}

// "a=n;b=n,b" as its entries, in order.
std::vector<std::string> split_overrides(const std::string& list) {
    std::vector<std::string> entries;
    std::size_t start = 0;
    while (start < list.size()) {
        const auto end = std::min(list.find(';', start), list.size());
        if (end > start) entries.push_back(list.substr(start, end - start));
        start = end + 1;
    }
    return entries;
}

std::string action_label(const std::string& key) {
    static const std::map<std::string, std::string> labels = {
        {"cancel", "Cancel"}, {"confirm-destructive", "Confirm"}, {"ready-later", "Later"}, {"alert-ok", "OK"}, {"alert-settings", "Game settings"}, {"runtime-cleanup-confirm", "Clean up"}, {"quit-to-home", "Play"}, {"favorite", "Favorite"},
        {"collection", "Collections"}, {"note", "Compatibility and note"}, {"verify", "Verify files"},
        {"uninstall", "Uninstall"}, {"import-remove", "Remove from library"}, {"uninstall-delete-saves", "Delete saves too"}, {"logs", "Open logs"}, {"logs-back", "Back"},
        {"logs-pick", "Logs"}, {"log-install", "Install log"}, {"log-auth", "Sign-in log"},
        {"log-launch", "Launch log"}, {"log-cloud", "Cloud saves log"}, {"logs-refresh", "Refresh"},
        {"cloud-sync", "Cloud saves"}, {"cloud-choose", "Cloud saves"}, {"cloud-use-cloud", "Use Cloud saves"},
        {"cloud-use-local", "Use console saves"}, {"cloud-confirm-account", "Use this account"},
        {"cloud-confirm-account-all", "Upload all"},
        {"saves-delete-confirm", "Delete saves"}, {"manage-saves", "Manage saves"}, {"saves-leftover-confirm", "Remove"}, {"saves-quit-anyway", "Play without syncing"},
        {"install-confirm", "Install"}, {"storage-picker", "Change storage"}, {"installed-close", "Close"},
        {"value-edit", "Edit…"}, {"value-reset", "Reset to recipe"}, {"override-add", "Add an override…"},
        {"unlock-setting", "Change anyway"}, {"export-recipe", "Export as recipe"}, {"program-scan", "Looking for programs…"},
        {"preset-picker", "Preset"}, {"game-settings", "Game settings"}, {"game-maintenance", "Maintenance"}, {"runtime-check-again", "Check again"}, {"reset-settings", "Reset all to preset"}, {"clear-shader-cache", "Clear shader cache"},
        {"section-compatibility", "Compatibility"}, {"section-advanced", "Advanced"}, {"heading-game", "Game"},
        {"heading-display", "Display"}, {"heading-controls", "Controls"}, {"heading-sound", "Sound"},
        {"heading-graphics", "Graphics"}, {"heading-windows", "Windows"},
        {"sort-name", "Name"}, {"sort-recent-played", "Recently played"}, {"sort-recent-added", "Recently added"},
        {"sort-playtime", "Playtime"}, {"sort-install-size", "Install size"}, {"filter-installed", "Installed"},
        {"filter-genre-adventure", "Adventure"}, {"filter-controller-full", "Full controller support"},
        {"filter-compat-playable", "Playable"}, {"filter-hide-demos", "Demos"}, {"filters-reset", "Reset filters"}, {"refresh-library", "Refresh library"}, {"advance-clock", "Advance fixture clock"},
        {"inject-download-failed", "Inject download failure"}, {"inject-verification-failed", "Inject verification failure"},
        {"inject-uninstall-failed", "Inject uninstall failure"},
        {"inject-storage-disconnect", "Disconnect storage"}, {"inject-controller-disconnect", "Disconnect controller"},
        {"inject-launch-failure", "Inject launch failure"}, {"inject-crash", "Inject session crash"},
        {"runtime-retry", "Retry runtime check"}, {"inject-qr-approved", "Approve current challenge"},
        {"inject-qr-expired", "Expire current challenge"}, {"inject-qr-failure", "Inject network failure"},
        {"inject-qr-denied", "Deny current challenge"}, {"logs-retry", "Retry loading logs"},
        {"inject-logs-empty", "Inject empty logs"}, {"inject-logs-failure", "Inject log failure"},
        {"inject-logs-ready", "Inject ready logs"}, {"account-auth", "Authentication"},
        {"account-last-sync", "Last library sync"}, {"account-sync", "Sync now"},
        {"account-recover", "Reconnect account"}, {"account-switch", "Switch account"},
        {"account-reauth", "Sign in again"}, {"job-resume-all", "Resume all"}, {"job-clear-completed", "Clear completed"},
        {"prep-clear-completed", "Clear completed"}, {"storage-close", "Close"}, {"collection-new", "New collection"}, {"collection-manage", "Manage collections"},
        {"about-version", "Version"}, {"self-update-install", "Download and install"},
        {"self-update-restart", "Restart Playden"}, {"self-update-cancel", "Cancel update"},
        {"kit-install", "Download"}, {"kit-cancel", "Cancel download"},
        {"self-update-back-confirm", "Go back"}, {"release-notes-ok", "OK"}, {"successor-open", "Open the new Playden"},
        {"about-build", "Build"}, {"about-framework", "Framework"}, {"about-platform", "Platform"},
        {"about-kit", "Runtime kit"}, {"about-kit-source", "Built from"},
        {"about-back", "Back"}, {"inject-library-refresh", "Refresh library fixture"},
        {"inject-startup-loading", "Show startup loading"}};
    if (const auto found = labels.find(key); found != labels.end()) return found->second;
    // Per-job actions carry the job id after the verb.
    for (const auto& [prefix, label] : std::initializer_list<std::pair<const char*, const char*>>{
             {"job-pause-", "Pause download"}, {"job-resume-", "Resume download"}, {"job-retry-", "Retry download"},
             {"job-cancel-", "Cancel download"}, {"job-now-", "Download now"}, {"job-up-", "Move up"}, {"job-down-", "Move down"},
             {"job-open-", "Open game"}, {"job-dismiss-", "Dismiss"}, {"prep-next-", "Prepare next"},
             {"prep-open-", "Game details"}, {"prep-retry-", "Try again"}, {"prep-recreate-", "Recreate bottle"},
             {"prep-dismiss-", "Dismiss"},
             {"uninstall-dismiss-", "Dismiss"}})
        if (key.rfind(prefix, 0) == 0) return label;
    std::string title = key;
    std::replace(title.begin(), title.end(), '-', ' ');
    if (!title.empty()) title.front() = static_cast<char>(std::toupper(static_cast<unsigned char>(title.front())));
    return title;
}

// Recreate bottle asks first, from Maintenance and for a setup an earlier kit left (Queue, the game page).
ModalEntry recreate_confirmation(const std::string& title, std::string opener) {
    ModalEntry confirmation{"confirmation", "Recreate the bottle?", std::move(opener), {"cancel", "confirm-destructive"}, 1, false};
    confirmation.body = title + "'s Windows environment is made again from the runtime kit, with its saves and settings brought back. "
                        "The PS5 title is removed and prepared again, so the game then waits for the PS5 Home screen once more.";
    return confirmation;
}

std::string trimmed(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::size_t snapshot_size(const playden::OrganizationSnapshot& value) {
    std::size_t bytes = sizeof(value) + value.query.size() + value.scope.size() + value.sort.size();
    bytes += (value.favorites.size() + value.hidden.size()) * sizeof(GameId);
    bytes += value.ratings.size() * (sizeof(GameId) + sizeof(playden::CompatibilityRating));
    for (const auto& [id, note] : value.notes) bytes += sizeof(id) + note.size();
    for (const auto& filter : value.filters) bytes += filter.size();
    for (const auto& collection : value.collections)
        bytes += collection.id.size() + collection.name.size() + collection.members.size() * sizeof(GameId);
    return bytes;
}

bool snapshot_within_model_bounds(const playden::OrganizationSnapshot& value) {
    return playden::OrganizationSnapshotIsValid(value);
}

template <typename Value>
void replay_set_delta(std::set<Value>& base, const std::set<Value>& before,
                      const std::set<Value>& after) {
    for (const auto& value : before) if (!after.contains(value)) base.erase(value);
    for (const auto& value : after) if (!before.contains(value)) base.insert(value);
}

template <typename Key, typename Value>
void replay_map_delta(std::map<Key, Value>& base, const std::map<Key, Value>& before,
                      const std::map<Key, Value>& after) {
    for (const auto& [key, value] : before) {
        (void)value;
        if (!after.contains(key)) base.erase(key);
    }
    for (const auto& [key, value] : after) {
        const auto old = before.find(key);
        if (old == before.end() || old->second != value) base[key] = value;
    }
}

const playden::OrganizationCollection* find_collection(
    const std::vector<playden::OrganizationCollection>& collections, std::string_view id) {
    const auto found = std::find_if(collections.begin(), collections.end(), [id](const auto& value) {
        return value.id == id;
    });
    return found == collections.end() ? nullptr : &*found;
}

std::string free_collection_id(const playden::OrganizationSnapshot& base,
                               const playden::OrganizationSnapshot& local) {
    for (std::uint64_t serial = 1;; ++serial) {
        const auto candidate = "collection-" + std::to_string(serial);
        if (!find_collection(base.collections, candidate) &&
            !find_collection(local.collections, candidate)) return candidate;
    }
}

void replay_organization_delta(playden::OrganizationSnapshot& base,
                               const playden::OrganizationSnapshot& before,
                               const playden::OrganizationSnapshot& after,
                               bool reset, bool selected) {
    if (reset) {
        base = after;
        return;
    }
    replay_set_delta(base.favorites, before.favorites, after.favorites);
    replay_set_delta(base.hidden, before.hidden, after.hidden);
    replay_map_delta(base.ratings, before.ratings, after.ratings);
    replay_map_delta(base.notes, before.notes, after.notes);
    replay_set_delta(base.filters, before.filters, after.filters);

    for (const auto& old : before.collections) {
        if (find_collection(after.collections, old.id)) continue;
        base.collections.erase(std::remove_if(base.collections.begin(), base.collections.end(),
                                              [&old](const auto& value) { return value.id == old.id; }),
                               base.collections.end());
    }
    for (const auto& local : after.collections) {
        const auto* prior = find_collection(before.collections, local.id);
        auto current = std::find_if(base.collections.begin(), base.collections.end(),
                                    [&local](const auto& value) { return value.id == local.id; });
        if (!prior) {
            if (current != base.collections.end()) {
                const auto replacement = free_collection_id(base, after);
                if (base.scope == current->id) base.scope = replacement;
                current->id = replacement;
            }
            base.collections.push_back(local);
        } else if (*prior != local) {
            if (current == base.collections.end()) base.collections.push_back(local);
            else *current = local;
        }
    }
    if (before.query != after.query) base.query = after.query;
    if (before.scope != after.scope) base.scope = after.scope;
    if (before.sort != after.sort) base.sort = after.sort;
    if (selected || before.selected_game_id != after.selected_game_id)
        base.selected_game_id = after.selected_game_id;
}

// The Import sheet's drive groups, in the order it lists them (ImportRoots' volume ids).
constexpr std::array<std::pair<const char*, const char*>, 5> kImportGroups = {{
    {"usb0", "USB drive"}, {"usb1", "USB drive 2"}, {"m2", "M.2 SSD"}, {"extended", "Extended storage"},
    {"internal", "Console drop folder"}}};
constexpr const char* kImportEmptyText =
    "Copy game folders into playden/import on an exFAT drive, or into /data/playden-import over FTP, then choose "
    "Look for games.";
constexpr const char* kImportCopyOnlyText =
    "This folder's name has characters the PS5 can't start a game from. Copy it into Playden instead.";

constexpr const char* kImportDropVolume = "internal";
// The wizard's step 1 title, and what it says about importing later.
constexpr const char* kImportChoiceTitle = "Where will the game files live?";
constexpr const char* kImportChoiceText = "You can import it into Playden later from the game's page.";


// A drive's name as the Import sheet groups it ("USB drive", "Console drop folder").
std::string import_volume_name(const std::string& volume) {
    for (const auto& [id, name] : kImportGroups)
        if (volume == id) return name;
    return volume;
}

std::string import_candidate_key(const LiveImportCandidate& candidate) {
    return "import-candidate-" + candidate.volume_id + ":" + candidate.folder;
}

// Fixture mode: what Look for games finds (one game added already, one with two programs to choose from,
// one copy only, one without a program).
std::vector<LiveImportCandidate> fixture_import_candidates() {
    std::vector<LiveImportCandidate> candidates;
    const auto add = [&](std::string volume, std::string folder, std::string path, std::string title,
                         std::uint64_t bytes, bool ascii_ok, std::vector<LiveImportProgram> programs,
                         std::uint64_t files = 0) {
        LiveImportCandidate candidate;
        candidate.volume_id = std::move(volume); candidate.folder = std::move(folder); candidate.path = std::move(path);
        candidate.title = std::move(title); candidate.total_bytes = bytes; candidate.ascii_ok = ascii_ok;
        candidate.programs = std::move(programs); candidate.file_count = files;
        candidates.push_back(std::move(candidate));
    };
    add("usb0", "playden/import/Night Sky", "/mnt/usb0/playden/import/Night Sky", "Night Sky", 412'000'000, true,
        {{"NightSky.exe", 18'000'000, false}}, 120);
    add("usb0", "playden/import/Old School Rally", "/mnt/usb0/playden/import/Old School Rally", "Old School Rally",
        938'000'000, true, {{"OldSchoolRally.exe", 640'000'000, false}, {"Benchmark/RallyBench.exe", 12'000'000, false},
         {"UnityCrashHandler64.exe", 1'400'000, true}}, 2400);
    add("usb0", "playden/import/Café Étoile", "/mnt/usb0/playden/import/Café Étoile", "Café Étoile", 1'250'000'000,
        false, {{"CafeEtoile.exe", 96'000'000, false}, {"redist/vcredist_x64.exe", 14'000'000, true}}, 300);
    add("m2", "playden/import/Delores", "/mnt/ext1/playden/import/Delores", "Delores", 230'000'000, true,
        {{"Delores.exe", 9'000'000, false}}, 80);
    add("internal", "playden-import/Hollow Knight", "/data/playden-import/Hollow Knight", "Hollow Knight",
        8'900'000'000, true, {{"hollow_knight.exe", 650'000, false}}, 1500);
    add("internal", "playden-import/Tools", "/data/playden-import/Tools", "Tools", 3'000'000, true, {});
    return candidates;
}

// The fixture's imported game (Night Sky, already added from the USB drive).
GameId fixture_imported_game() { return GameId::Local("6d1f3a52-0c55-4f3e-9b2a-5d1c7e0a9f11"); }

struct StartupProblem {
    std::string title;
    std::string subtitle;
    bool show_detail = true;  // the error line, for reports; left out where it adds nothing for the player
};

// What the startup error means for the player: the step that failed and how to fix it. The error
// itself stays on the screen as the detail line, for reports.
StartupProblem describe_startup_problem(const std::string& detail, bool granted) {
    const auto has = [&detail](const char* text) { return detail.find(text) != std::string::npos; };
    const auto stage = [&detail](std::initializer_list<const char*> stages) {
        for (const char* name : stages)
            if (detail.find(std::string("helper stage=") + name + " ") != std::string::npos) return true;
        return false;
    };
    const StartupProblem incomplete{"Playden's app folder is incomplete",
        "Some of Playden's files in /data/homebrew are missing or can't be read. Copy the whole app folder from the "
        "release zip again (or use tools/console_title_deploy.py), then open Playden again."};
    constexpr const char* kElsewhere = "launcher is not installed in ";
    if (const auto at = detail.find(kElsewhere); at != std::string::npos) {
        const auto from = at + std::strlen(kElsewhere);
        const auto folder = detail.substr(from, detail.find(';', from) - from);
        const auto parent = folder.substr(0, folder.rfind('/'));
        // ProsperoStore can install to other folders and drives; its labels are Settings > Install location and
        // Uninstall (held) on the app's page.
        return {"Install Playden on console storage",
                "Playden runs only from " + folder + ", on the console's own storage. From ProsperoStore: set "
                "Settings > Install location to " + parent + ", hold Uninstall on Playden's page, then install it "
                "again. If you copied it by hand, move the app folder to " + parent + ".",
                false};
    }
    if (granted) {
        if (has("stable launcher source is invalid")) return incomplete;
        if (has("stable launcher source identity does not match"))
            return {"Playden can't start",
                    "The Playden that started is not the copy in /data/homebrew. If you just copied a new version, "
                    "restart the PS5; if it stays, copy the whole app folder to /data/homebrew again."};
        return {"Playden can't start", "Playden has root access but can't use its data in /data/playden. "
                                       "Restart Playden; if it stays, check that folder's permissions over FTP."};
    }
    if (has("launcher source is invalid") || has("launcher source identity is unavailable") ||
        stage({"find-helper", "validate-helper", "open-helper", "stat-helper"}))
        return incomplete;
    if (stage({"network", "socket", "socket-options"}))
        return {"Playden can't start", "The console's network did not start, and Playden needs it to ask for root "
                                       "access. Restart the PS5, jailbreak it again, then open Playden again."};
    if (stage({"stream-helper"}))
        return {"Root access is missing",
                "The ELF loader on port 9021 stopped while Playden sent it the root helper. Load the ELF loader "
                "again, then open Playden again."};
    if (stage({"connect"}))
        return {"The ELF loader isn't running",
                "Playden gets root access through the ELF loader (elfldr) on port 9021, and nothing answered there. "
                "Load the ELF loader (most autoloaders start it), then open Playden again."};
    if (has("helper stage="))
        return {"Root access is missing",
                "The ELF loader ran Playden's root helper, but the helper could not give Playden access. "
                "Load kstuff and the autoloader, then open Playden again. If it stays, report the startup error "
                "below with your firmware version."};
    return {"Root access is missing", "Without it, Playden can't reach its data, so it can't show your games, "
                                      "download or play. Load kstuff and the autoloader, then open Playden again."};
}

} // namespace

std::string JobStateText(const Job& job) {
    if (!job.state_text.empty()) return job.state_text;
    switch (job.state) {
    case JobState::Queued: return "Queued";
    case JobState::Downloading: return job.game_id.store == Store::kLocal ? "Copying" : "Downloading";
    case JobState::Pausing: return "Pausing";
    case JobState::Paused: return "Paused";
    case JobState::Verifying: return "Verifying";
    case JobState::Downloaded: return "Complete";
    case JobState::Failed: return "Failed";
    case JobState::Cancelled: return "Cancelled";
    case JobState::Interrupted: return "Interrupted";
    }
    return "Transfer";
}

Simulation::Simulation(std::filesystem::path fixture_directory, std::filesystem::path profile_path, ModelMode mode)
    : fixture_directory_(std::move(fixture_directory)), profile_path_(std::move(profile_path)), mode_(mode) {
    reset_default_state();
    if (live()) {
        rebuild_view();
        return;
    }
    load_fixtures();
    scenario("default");
    load_profile();
    rebuild_view();
}

void Simulation::reset_default_state() {
    state_ = {};
    state_.simulation_mode = !live();
    state_.clock_ticks = live() ? 0 : kFixtureClock;
    state_.navigation.scroll_offsets["library-grid"] = 0;
    state_.navigation.scroll_offsets["home"] = 0;
    state_.navigation.scroll_offsets["details"] = 0;
    state_.navigation.scroll_offsets["details-content"] = 0;
    selected_volume_id_ = "external";
    editing_setting_id_.clear();
    next_job_serial_ = 1;
    last_error_.clear();
    captured_organization_ = {};
    persisted_organization_ = {};
    captured_preferences_ = {};
}

void Simulation::SetControllerConnected(bool connected) {
    const bool changed = state_.controller_connected != connected;
    state_.controller_connected = connected;
    // The pad reads as disconnected for the first frames after start: only a change after the
    // controller has been seen once is a real disconnect or reconnect.
    const bool announce = live() && changed && controller_seen_;
    if (connected) controller_seen_ = true;
    if (announce) {
        if (!connected) {
            push_notice({"controller", "Controller disconnected", "", true, 0, true, "Reconnect it to keep playing"});
        } else {
            state_.notices.erase(std::remove_if(state_.notices.begin(), state_.notices.end(),
                                                [](const Notice& notice) { return notice.id == "controller"; }),
                                 state_.notices.end());
            push_notice({"controller-connected", "Controller connected", "", false, kLiveNoticeSeconds, false, ""});
        }
    }
    rebuild_view();
}

void Simulation::SetWallClock(std::int64_t unix_seconds) {
    if (!live() || state_.clock_ticks == unix_seconds) return;
    // Notifications only count down while they can be seen, so none expires behind a dialog.
    const auto elapsed = state_.clock_ticks == 0 ? 0 : std::clamp<std::int64_t>(unix_seconds - state_.clock_ticks, 0, 60);
    state_.clock_ticks = unix_seconds;
    if (elapsed > 0 && live_notices_visible()) {
        for (auto& notice : state_.notices)
            if (!notice.persistent) notice.remaining_ticks -= static_cast<int>(elapsed);
        state_.notices.erase(std::remove_if(state_.notices.begin(), state_.notices.end(),
                                            [](const Notice& notice) { return !notice.persistent && notice.remaining_ticks <= 0; }),
                             state_.notices.end());
    }
    rebuild_view();
}

void Simulation::push_notice(Notice notice) {
    // A failure the player has to see waits in a dialog until they press OK; a corner
    // notification goes unnoticed and leaves on its own.
    if (live() && notice.failure && notice.alert && !notice.persistent) {
        show_alert(std::move(notice.message), std::move(notice.detail));
        return;
    }
    state_.notices.erase(std::remove_if(state_.notices.begin(), state_.notices.end(),
                                        [&notice](const Notice& existing) { return existing.id == notice.id; }),
                         state_.notices.end());
    state_.notices.push_back(std::move(notice));
    // Keep at most a few transient notifications; the oldest give way.
    constexpr std::size_t kMaxTransient = 8;
    std::size_t transient = 0;
    for (const auto& existing : state_.notices) transient += existing.persistent ? 0 : 1;
    for (auto it = state_.notices.begin(); transient > kMaxTransient && it != state_.notices.end();) {
        if (!it->persistent) { it = state_.notices.erase(it); --transient; }
        else ++it;
    }
}

bool Simulation::live_notices_visible() const noexcept {
    return state_.navigation.modals.empty() && !state_.editor.active &&
           state_.navigation.route != Route::Startup && state_.navigation.route != Route::SignIn;
}

std::string mask_secret(std::string_view value) {
    std::string masked;
    for (const char c : value)
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) masked += "\u2022";
    return masked;
}

std::optional<SignInCredentials> Simulation::TakeSignInCredentials() {
    auto credentials = std::move(pending_credentials_);
    pending_credentials_.reset();
    return credentials;
}

std::optional<std::string> Simulation::TakeGuardCode() {
    auto code = std::move(pending_guard_code_);
    pending_guard_code_.reset();
    return code;
}

std::vector<ServiceRequest> Simulation::TakeServiceActions() {
    auto actions = std::move(pending_service_actions_);
    pending_service_actions_.clear();
    return actions;
}

std::vector<InstallRequest> Simulation::TakeInstallRequests() {
    auto requests = std::move(pending_install_requests_);
    pending_install_requests_.clear();
    return requests;
}

std::vector<CloudRequest> Simulation::TakeCloudRequests() {
    auto requests = std::move(pending_cloud_requests_);
    pending_cloud_requests_.clear();
    return requests;
}

std::vector<GameId> Simulation::TakeRuntimePrepareRequests() {
    auto requests = std::move(pending_runtime_prepare_requests_);
    pending_runtime_prepare_requests_.clear();
    return requests;
}

void Simulation::UpdateKeptBottles(std::vector<LiveKeptBottle> kept) {
    state_.kept_bottles = std::move(kept);
    std::erase_if(kept_selected_, [this](GameId id) {
        return std::none_of(state_.kept_bottles.begin(), state_.kept_bottles.end(), [id](const LiveKeptBottle& item) { return item.game_id == id; });
    });
    // An open manager lists what is kept now; with nothing left it closes.
    auto& modals = state_.navigation.modals;
    for (std::size_t index = 0; index < modals.size(); ++index) {
        auto& modal = modals[index];
        if (modal.id != "kept-data") continue;
        if (state_.kept_bottles.empty()) {
            if (index + 1 == modals.size()) close_modal();
            break;
        }
        const auto focused = modal.action_keys[static_cast<std::size_t>(modal.focused_action)];
        modal.action_keys = kept_manager_actions();
        const auto found = std::find(modal.action_keys.begin(), modal.action_keys.end(), focused);
        modal.focused_action = found == modal.action_keys.end()
            ? std::min(modal.focused_action, static_cast<int>(modal.action_keys.size()) - 1)
            : static_cast<int>(found - modal.action_keys.begin());
        if (index + 1 == modals.size()) state_.navigation.focus_key = modal.action_keys[static_cast<std::size_t>(modal.focused_action)];
    }
    rebuild_view();
}

void Simulation::UpdateExternalGames(std::vector<LiveExternalGame> games) {
    if (!live() || games == state_.external_games) return;
    state_.external_games = std::move(games);
    rebuild_view();
}

void Simulation::UpdateImportCandidates(std::vector<LiveImportCandidate> candidates, bool scanning) {
    if (!live() || (candidates == state_.import_candidates && scanning == state_.import_scanning)) return;
    state_.import_candidates = std::move(candidates);
    state_.import_scanning = scanning;
    refresh_import_sheet();
    settle_pending_imports();
    rebuild_view();
}

void Simulation::RequestImport(ImportRequest request) {
    if (live()) pending_import_requests_.push_back(std::move(request));
}

std::vector<ImportRequest> Simulation::TakeImportRequests() { return std::exchange(pending_import_requests_, {}); }

std::vector<std::string> Simulation::import_sheet_actions() const {
    std::vector<std::string> actions{"import-look"};
    for (const auto& [volume, name] : kImportGroups) {
        bool heading = false;
        for (const auto& candidate : state_.import_candidates) {
            if (candidate.volume_id != volume) continue;
            if (!heading) { actions.push_back(std::string("import-group-") + volume); heading = true; }
            actions.push_back(import_candidate_key(candidate));
        }
    }
    actions.push_back("cancel");
    return actions;
}

const LiveImportCandidate* Simulation::import_candidate(const std::string& key) const {
    for (const auto& candidate : state_.import_candidates)
        if (import_candidate_key(candidate) == key) return &candidate;
    return nullptr;
}

void Simulation::open_import_sheet() {
    auto actions = import_sheet_actions();
    // The first game not added yet, else the first game, else Look for games.
    int focus = 0, first = -1;
    for (int index = 0; index < static_cast<int>(actions.size()); ++index) {
        const auto* candidate = import_candidate(actions[static_cast<std::size_t>(index)]);
        if (!candidate) continue;
        if (first < 0) first = index;
        if (!candidate->imported.valid()) { focus = index; break; }
    }
    if (focus == 0 && first >= 0) focus = first;
    open_modal({"import-games", "Import games", state_.navigation.focus_key, std::move(actions), focus, false});
}

void Simulation::refresh_import_sheet() {
    auto& modals = state_.navigation.modals;
    for (std::size_t index = 0; index < modals.size(); ++index) {
        auto& modal = modals[index];
        if (modal.id == "import-games") {
            const auto focused = modal.action_keys[static_cast<std::size_t>(modal.focused_action)];
            modal.action_keys = import_sheet_actions();
            const auto found = std::find(modal.action_keys.begin(), modal.action_keys.end(), focused);
            modal.focused_action = found == modal.action_keys.end() ? 0 : static_cast<int>(found - modal.action_keys.begin());
            if (index + 1 == modals.size()) state_.navigation.focus_key = modal.action_keys[static_cast<std::size_t>(modal.focused_action)];
        } else if ((modal.id == "import-programs" || modal.id == "import-choice" ||
                    (modal.id == "import-storage" && !import_choice_game_.valid())) &&
                   !import_candidate(import_programs_key_)) {
            // The folder went away in a new scan: its program list closes.
            modals.erase(modals.begin() + static_cast<std::ptrdiff_t>(index));
            if (index == modals.size() && !modals.empty())
                state_.navigation.focus_key = modals.back().action_keys[static_cast<std::size_t>(modals.back().focused_action)];
            --index;
        }
    }
}

void Simulation::move_import_sheet(int delta) {
    auto& modal = state_.navigation.modals.back();
    if (delta == 0) return;
    const int step = delta > 0 ? 1 : -1;
    for (int next = modal.focused_action + step; next >= 0 && next < static_cast<int>(modal.action_keys.size()); next += step) {
        if (modal.action_keys[static_cast<std::size_t>(next)].rfind("import-group-", 0) == 0) continue;
        modal.focused_action = next;
        break;
    }
    state_.navigation.focus_key = modal.action_keys[static_cast<std::size_t>(modal.focused_action)];
}

void Simulation::look_for_games() {
    if (state_.import_scanning) return;
    // The sheet shows the scan at once, so a second press before the store answers asks nothing more.
    state_.import_scanning = true;
    // Fixture mode: the scan runs until the next tick, which fills in the fixture's folders.
    if (live()) RequestImport({ImportRequestKind::LookForGames});
}

const LiveExternalGame* Simulation::external_game(GameId id) const {
    for (const auto& external : state_.external_games)
        if (external.game_id == id) return &external;
    return nullptr;
}

const Job* Simulation::import_copy_job(GameId id) const {
    if (id.store != Store::kLocal) return nullptr;
    for (const auto& job : state_.jobs)
        if (job.game_id == id && job.kind == "install" && job.state != JobState::Downloaded && job.state != JobState::Cancelled)
            return &job;
    return nullptr;
}

Simulation::ImportSubject Simulation::import_subject() const {
    ImportSubject subject;
    if (import_choice_game_.valid()) {
        const auto* game = find_game(state_, import_choice_game_);
        if (game) subject.title = game->title;
        if (game && game->install_bytes) subject.bytes = *game->install_bytes;
        // An in-place game's size comes from the import scan when the library has none.
        for (const auto& candidate : state_.import_candidates)
            if (candidate.imported == import_choice_game_ && !subject.bytes) subject.bytes = candidate.total_bytes;
        if (const auto* external = external_game(import_choice_game_)) subject.volume = external->volume_id;
    } else if ((subject.candidate = import_candidate(import_programs_key_))) {
        subject.title = subject.candidate->title;
        subject.volume = subject.candidate->volume_id;
        subject.bytes = subject.candidate->total_bytes;
        subject.ascii_ok = subject.candidate->ascii_ok;
    }
    return subject;
}

std::string Simulation::import_volume_blocker(const ImportSubject& subject, const Volume& volume) const {
    if (!volume.available) return "Disconnected";
    if (!volume.writable) return "No write access";
    const bool drop = subject.volume == kImportDropVolume;
    // A drop-folder game moves within the console storage: no extra space.
    if (drop && volume.id == kImportDropVolume) return {};
    if (!drop && volume.id == subject.volume) return "The game is on this drive";
    if (volume.capacity_known) {
        const auto unused = volume.capacity_bytes - std::min(volume.used_bytes, volume.capacity_bytes);
        const auto free = unused - std::min(volume.reserved_bytes, unused);
        const std::uint64_t margin = live() ? steam::kInstallSpaceMargin : 0;
        if (free < margin || subject.bytes > free - margin) return "Not enough space";
    }
    return {};
}

void Simulation::open_import_choice(std::size_t program) {
    import_program_index_ = program;
    import_choice_game_ = {};
    import_program_chosen_ = false;
    const auto* candidate = import_candidate(import_programs_key_);
    if (!candidate || program >= candidate->programs.size()) return;
    // Play from the folder (disabled, with why, when the PS5 can't start a game from it), then
    // Import into Playden (step 2 picks the drive), then Back. Import has the focus.
    open_modal({"import-choice", kImportChoiceTitle, state_.navigation.focus_key,
                {"import-play-in-place", "import-copy", "cancel"}, 1, false});
}

void Simulation::open_import_storage(GameId game) {
    if (game.valid()) {
        if (!find_game(state_, game) || !external_game(game)) return;
        import_choice_game_ = game;
    }
    const auto subject = import_subject();
    if (!subject.candidate && !import_choice_game_.valid()) return;
    // The install volumes, with the focus on the default drive when it can take the game, else on
    // the first that can; then Back.
    std::vector<std::string> actions;
    int focus = -1, first = -1;
    for (const auto& volume : state_.volumes) {
        const int index = static_cast<int>(actions.size());
        actions.push_back("volume-" + volume.id);
        if (!import_volume_blocker(subject, volume).empty()) continue;
        if (first < 0) first = index;
        if (volume.id == selected_volume_id_) focus = index;
    }
    actions.push_back("cancel");
    if (focus < 0) focus = first >= 0 ? first : static_cast<int>(actions.size()) - 1;
    open_modal({"import-storage", "Choose storage", state_.navigation.focus_key, std::move(actions), focus, false});
}

void Simulation::open_game_page(GameId id) {
    if (!find_game(state_, id)) return;
    state_.navigation.modals.clear();
    if (live()) force_organization_selection_ = true;
    // From Startup (no library page yet) Circle goes Home.
    if (state_.navigation.route != Route::Details)
        state_.navigation.detail_origin = state_.navigation.route == Route::Startup ? Route::Home : state_.navigation.route;
    state_.navigation.detail_id = id;
    state_.navigation.selected_game_id = id;
    state_.navigation.route = Route::Details;
    details_primary_pending_ = live();
    state_.navigation.focus_key = live() ? "details-favorite" : "details-play";
}

void Simulation::start_import_transfer(GameId id, ImportRequestKind kind, const std::string& volume_id) {
    if (live()) {
        RequestImport({kind, id, kind == ImportRequestKind::CopyIntoPlayden ? volume_id : std::string()});
        import_transfers_starting_[id] = kind;
        return;
    }
    // Fixture mode: a copy is a job in Queue (its drive, or the default); a move is instant.
    const auto* game = find_game(state_, id);
    const auto bytes = game && game->install_bytes ? *game->install_bytes : std::uint64_t{0};
    if (kind == ImportRequestKind::CopyIntoPlayden) {
        if (import_copy_job(id)) return;
        state_.jobs.push_back({next_job_id(), id, "install", next_queue_order(state_), JobStage::Download, JobState::Queued, 0,
                               std::max<std::uint64_t>(bytes, 1), std::nullopt, std::nullopt, {}, JobStage::Download,
                               volume_id.empty() ? selected_volume_id_ : volume_id, 0, bytes, true, false});
    } else {
        std::erase_if(state_.external_games, [id](const LiveExternalGame& external) { return external.game_id == id; });
        if (!find_installation(state_, id))
            state_.installations.push_back({id, kImportDropVolume, std::max<std::uint64_t>(bytes, 1), true, true, true});
        reconcile_storage(state_);
    }
}

void Simulation::choose_import(const std::string& action) {
    // Step 1: Import into Playden goes on to the drive choice.
    if (action == "import-copy") { open_import_storage(); return; }
    if (action != "import-play-in-place") return;
    const auto* candidate = import_candidate(import_programs_key_);
    if (!candidate || import_program_index_ >= candidate->programs.size() || candidate->imported.valid()) return;
    if (!candidate->ascii_ok) return;
    const auto key = import_programs_key_;
    add_import_candidate(key, std::nullopt, {});
}

void Simulation::choose_import_volume(const std::string& volume_id) {
    const auto subject = import_subject();
    const auto* volume = find_volume(state_, volume_id);
    if (!volume || !import_volume_blocker(subject, *volume).empty()) return;
    // The drop folder into the console storage is a rename; anything else a copy to that drive.
    const auto kind = subject.volume == kImportDropVolume && volume_id == kImportDropVolume ? ImportRequestKind::MoveIntoPlayden
                                                                                              : ImportRequestKind::CopyIntoPlayden;
    if (import_choice_game_.valid()) {
        // A game already added (its page's Import into Playden): the transfer starts on its page.
        const auto id = import_choice_game_;
        start_import_transfer(id, kind, volume_id);
        open_game_page(id);
        return;
    }
    const auto* candidate = subject.candidate;
    if (!candidate || import_program_index_ >= candidate->programs.size() || candidate->imported.valid()) return;
    add_import_candidate(import_programs_key_, kind, volume_id);
}

void Simulation::add_import_candidate(const std::string& key, std::optional<ImportRequestKind> then, const std::string& volume_id) {
    const auto* candidate = import_candidate(key);
    if (!candidate) return;
    // Played in place, it is prepared once added; a copy or move chosen with it prepares it once it is
    // in Playden instead.
    RequestImport({ImportRequestKind::Add, {}, candidate->volume_id, candidate->folder, candidate->programs[import_program_index_].path,
                   !then});
    pending_imports_[key] = {then, volume_id, state_.navigation.route};
    import_failures_.erase(key);
    if (!live()) {
        // Fixture mode: the folder becomes a library game at once.
        Game game;
        // A UUID made from the folder, so adding it again finds the same game.
        const auto hash = static_cast<unsigned long long>(std::hash<std::string>{}(key));
        char uuid[40];
        std::snprintf(uuid, sizeof(uuid), "%08llx-%04llx-4%03llx-8%03llx-%012llx", hash & 0xffffffffULL,
                      (hash >> 32) & 0xffffULL, (hash >> 48) & 0xfffULL, (hash >> 20) & 0xfffULL, hash & 0xffffffffffffULL);
        game.id = GameId::Local(uuid);
        game.title = candidate->title;
        game.source = "Imported";
        game.install_bytes = candidate->total_bytes;
        if (!find_game(state_, game.id)) state_.games.push_back(game);
        if (!external_game(game.id)) state_.external_games.push_back({game.id, candidate->volume_id, true, ""});
        for (auto& entry : state_.import_candidates)
            if (import_candidate_key(entry) == key) entry.imported = game.id;
    }
    // Back on the Import sheet until the game is in the library; then its page opens.
    while (!state_.navigation.modals.empty() && state_.navigation.modals.back().id != "import-games") close_modal();
    settle_pending_imports();
}

void Simulation::settle_pending_imports() {
    for (auto it = pending_imports_.begin(); it != pending_imports_.end();) {
        const auto* candidate = import_candidate(it->first);
        // The folder went away in a new scan: nothing waits on it any more.
        if (!candidate) { it = pending_imports_.erase(it); continue; }
        if (!candidate->imported.valid()) { ++it; continue; }
        const auto id = candidate->imported;
        if (it->second.then) {
            // The copy or move chosen with the add goes once the game has its ID.
            const auto kind = *it->second.then;
            it->second.then.reset();
            start_import_transfer(id, kind, it->second.volume_id);
        }
        // The library lists it: its page opens over the Import sheet, or where the sheet was when the
        // library's arrival closed it (Startup gives way to Home); not once the player went elsewhere.
        if (!find_game(state_, id)) { ++it; continue; }
        const auto from = it->second.from;
        it = pending_imports_.erase(it);
        const auto& modals = state_.navigation.modals;
        const auto route = state_.navigation.route;
        if ((!modals.empty() && modals.back().id == "import-games") ||
            (modals.empty() && !state_.editor.active && (route == from || (from == Route::Startup && route == Route::Home))))
            open_game_page(id);
    }
}

void Simulation::NotifyImportFailed(const ImportRequest& request, const std::string& message) {
    std::string title, verb;
    switch (request.kind) {
    case ImportRequestKind::LookForGames: verb = "Can't look for games"; break;
    case ImportRequestKind::Add: verb = "Can't add"; break;
    case ImportRequestKind::Remove: verb = "Can't remove"; break;
    case ImportRequestKind::CopyIntoPlayden: verb = "Can't copy"; break;
    case ImportRequestKind::MoveIntoPlayden: verb = "Can't move"; break;
    }
    if (request.kind == ImportRequestKind::Add) {
        for (const auto& candidate : state_.import_candidates)
            if (candidate.volume_id == request.volume_id && candidate.folder == request.folder) {
                title = candidate.title;
                pending_imports_.erase(import_candidate_key(candidate));
                import_failures_[import_candidate_key(candidate)] = message;
            }
        if (title.empty()) title = request.folder.substr(request.folder.find_last_of('/') + 1);
    } else if (const auto* game = find_game(state_, request.game_id)) {
        title = game->title;
    }
    if (request.kind == ImportRequestKind::LookForGames) state_.import_scanning = false;
    if (request.kind == ImportRequestKind::CopyIntoPlayden || request.kind == ImportRequestKind::MoveIntoPlayden)
        import_transfers_starting_.erase(request.game_id);
    push_notice({"import-" + (request.game_id.valid() ? ToKey(request.game_id) : request.volume_id + ":" + request.folder),
                 verb + (title.empty() ? std::string() : " " + title), "", false, kLiveNoticeSeconds * 2, true, message});
    rebuild_view();
}

void Simulation::open_steam_programs(GameId id) {
    const auto setup = game_setups_.find(id);
    const auto* game = find_game(state_, id);
    if (!game || setup == game_setups_.end() || setup->second.steam_choices.empty()) return;
    steam_programs_shown_ = setup->second.steam_choices;
    // Installers and tools last, each group in Steam's order (its default option first).
    std::stable_partition(steam_programs_shown_.begin(), steam_programs_shown_.end(),
                          [](const LaunchProgram& program) { return !program.helper; });
    steam_programs_game_ = id;
    std::vector<std::string> actions;
    for (std::size_t index = 0; index < steam_programs_shown_.size(); ++index)
        actions.push_back("steam-program-" + std::to_string(index));
    actions.push_back("cancel");
    open_modal({"steam-programs", game->title, state_.navigation.focus_key, std::move(actions), 0, false});
}

bool Simulation::SeedGameProgram(GameId id, const std::string& program) {
    if (!live() || !id.valid() || settings_load_failed_ || game_settings_file_.empty()) return false;
    if (!rt::ValidateSettingValue(rt::GameRecipe{}, "program", program, nullptr)) return false;
    // Only a record never saved: once the player changed anything (a Program reset included), the
    // choice is theirs.
    if (const auto found = state_.game_settings.find(id);
        found != state_.game_settings.end() && (found->second.revision != 0 || !found->second.overrides.empty()))
        return false;
    auto& record = state_.game_settings[id];
    record.overrides["program"] = program;
    ++record.revision;
    save_profile();
    rebuild_view();
    return saved_game_settings_ == state_.game_settings;
}

std::vector<std::string> Simulation::collection_sheet_actions(const std::string& sheet) const {
    std::vector<std::string> actions;
    if (sheet == "collections") {
        for (const auto& collection : state_.library.collections) actions.push_back("collection-toggle-" + collection.id);
        actions.push_back("collection-new");
        if (!state_.library.collections.empty()) actions.push_back("collection-manage");
    } else {
        for (const auto& collection : state_.library.collections) {
            actions.push_back("collection-rename-" + collection.id);
            actions.push_back("collection-pin-" + collection.id);
            actions.push_back("collection-delete-" + collection.id);
        }
    }
    actions.push_back("cancel");
    return actions;
}

// Open collection sheets list the collections as they are now; `focus` moves the focus to a row.
void Simulation::refresh_collection_sheets(const std::string& focus) {
    auto& modals = state_.navigation.modals;
    for (std::size_t index = 0; index < modals.size(); ++index) {
        auto& modal = modals[index];
        if (modal.id != "collections" && modal.id != "collections-manage") continue;
        const auto focused = focus.empty() && modal.focused_action < static_cast<int>(modal.action_keys.size())
            ? modal.action_keys[static_cast<std::size_t>(modal.focused_action)] : focus;
        modal.action_keys = collection_sheet_actions(modal.id);
        const auto found = std::find(modal.action_keys.begin(), modal.action_keys.end(), focused);
        modal.focused_action = found == modal.action_keys.end()
            ? std::min(modal.focused_action, static_cast<int>(modal.action_keys.size()) - 1)
            : static_cast<int>(found - modal.action_keys.begin());
        if (index + 1 == modals.size()) state_.navigation.focus_key = modal.action_keys[static_cast<std::size_t>(modal.focused_action)];
    }
}

std::vector<std::string> Simulation::kept_manager_actions() const {
    std::vector<std::string> actions{"kept-select-all"};
    for (const auto& kept : state_.kept_bottles) actions.push_back("kept-toggle-" + ToKey(kept.game_id));
    actions.push_back("kept-selection-delete");
    actions.push_back("cancel");
    return actions;
}

std::vector<GameId> Simulation::installed_list() const {
    struct Entry { GameId id; std::uint64_t bytes; std::optional<std::int64_t> played; std::string title; };
    std::vector<Entry> entries;
    const auto add = [&](GameId id, std::uint64_t bytes) {
        const auto* game = find_game(state_, id);
        entries.push_back({id, bytes, game ? game->last_played : std::nullopt,
                           game ? (game->folded_title.empty() ? game->title : game->folded_title) : ToKey(id)});
    };
    for (const auto& installation : state_.installations) add(installation.game_id, installation.installed_bytes);
    // As on macOS: largest first, or never played first and then the longest unplayed; ties by title.
    std::sort(entries.begin(), entries.end(), [this](const Entry& left, const Entry& right) {
        if (installed_by_played_) {
            if (left.played.has_value() != right.played.has_value()) return !left.played.has_value();
            if (left.played && *left.played != *right.played) return *left.played < *right.played;
        } else if (left.bytes != right.bytes) {
            return left.bytes > right.bytes;
        }
        return left.title != right.title ? left.title < right.title : left.id < right.id;
    });
    std::vector<GameId> ids;
    for (const auto& entry : entries) ids.push_back(entry.id);
    return ids;
}

std::string Simulation::installed_blocker(GameId id) const {
    const auto runtime = state_.runtime_games.find(id);
    const LiveRuntimeGame* game = runtime == state_.runtime_games.end() ? nullptr : &runtime->second;
    if (game && !game->uninstall_status.empty()) return game->uninstall_status;
    if (pending_uninstalls_.contains(id)) return "Uninstalling…";
    if (game && (game->activity == TitleActivity::Running || game->play_status == "Playing" || game->play_status == "Starting…"))
        return "Playing";
    if (std::any_of(state_.jobs.begin(), state_.jobs.end(), [id](const Job& job) {
            return job.game_id == id && job.state != JobState::Downloaded && job.state != JobState::Cancelled;
        }))
        return "In Queue";
    if (game && (game->status == "Preparing" || game->maintenance_running)) return "Busy";
    return {};
}

// The list: Sort and Select all, the games, then the footer's Uninstall selected and Close.
std::vector<std::string> Simulation::installed_manager_actions() const {
    std::vector<std::string> actions = {"installed-sort", "installed-select-all"};
    for (const auto id : installed_list()) actions.push_back("installed-toggle-" + ToKey(id));
    actions.push_back("installed-selection-uninstall");
    actions.push_back("installed-close");
    return actions;
}

// Where a key of the Installed games list sits: 0 Sort and Select all, 1 the games, 2 the footer.
static int installed_part(const std::string& key) {
    if (key == "installed-sort" || key == "installed-select-all") return 0;
    if (key.rfind("installed-toggle-", 0) == 0) return 1;
    return 2;
}

// The footer's buttons go side by side: Left/Right move between them, Up returns to the game the
// focus left, and Down from the last game reaches them.
void Simulation::move_installed_manager(int delta_x, int delta_y) {
    auto& modal = state_.navigation.modals.back();
    const auto& keys = modal.action_keys;
    const int current = std::clamp(modal.focused_action, 0, static_cast<int>(keys.size()) - 1);
    const int part = installed_part(keys[static_cast<std::size_t>(current)]);
    int first_footer = static_cast<int>(keys.size());
    for (int index = 0; index < static_cast<int>(keys.size()); ++index)
        if (installed_part(keys[static_cast<std::size_t>(index)]) == 2) { first_footer = index; break; }
    int target = current;
    if (part == 2) {
        if (delta_x != 0) target = std::clamp(current + (delta_x > 0 ? 1 : -1), first_footer, static_cast<int>(keys.size()) - 1);
        else if (delta_y < 0 && first_footer > 0) {
            const auto back = std::find(keys.begin(), keys.end(), installed_list_focus_);
            target = back != keys.end() ? static_cast<int>(back - keys.begin()) : first_footer - 1;
        }
    } else if (delta_y != 0) {
        target = current + delta_y;
        if (target >= first_footer) {
            installed_list_focus_ = keys[static_cast<std::size_t>(current)];
            target = current == first_footer - 1 ? first_footer : first_footer - 1;
        }
        target = std::clamp(target, 0, static_cast<int>(keys.size()) - 1);
    }
    modal.focused_action = target;
    state_.navigation.focus_key = keys[static_cast<std::size_t>(target)];
}

// The open Installed games list follows the installs, jobs and runtime: games that are gone or
// can't be picked now leave the selection, and with no game left the list closes.
void Simulation::refresh_installed_manager() {
    std::erase_if(installed_selected_, [this](GameId id) { return !find_installation(state_, id) || !installed_blocker(id).empty(); });
    auto& modals = state_.navigation.modals;
    for (std::size_t index = 0; index < modals.size(); ++index) {
        auto& modal = modals[index];
        if (modal.id != "installed-games") continue;
        auto actions = installed_manager_actions();
        if (actions.size() == 4) {
            if (index + 1 == modals.size()) close_modal();
            break;
        }
        const auto focused = modal.action_keys[static_cast<std::size_t>(modal.focused_action)];
        modal.action_keys = std::move(actions);
        const auto found = std::find(modal.action_keys.begin(), modal.action_keys.end(), focused);
        if (found != modal.action_keys.end()) {
            modal.focused_action = static_cast<int>(found - modal.action_keys.begin());
        } else {
            // The focused game went away: the nearest game left takes the focus, else Close.
            int first = -1, last = -1;
            for (int key = 0; key < static_cast<int>(modal.action_keys.size()); ++key)
                if (installed_part(modal.action_keys[static_cast<std::size_t>(key)]) == 1) { if (first < 0) first = key; last = key; }
            const auto close = std::find(modal.action_keys.begin(), modal.action_keys.end(), "installed-close");
            modal.focused_action = first >= 0 ? std::clamp(modal.focused_action, first, last)
                                              : static_cast<int>(close - modal.action_keys.begin());
        }
        if (index + 1 == modals.size()) state_.navigation.focus_key = modal.action_keys[static_cast<std::size_t>(modal.focused_action)];
    }
}

std::vector<GameId> Simulation::TakeKeptDeleteRequests() {
    auto requests = std::move(pending_kept_delete_requests_);
    pending_kept_delete_requests_.clear();
    return requests;
}

std::vector<std::pair<GameId, MaintenanceKind>> Simulation::TakeMaintenanceRequests() {
    auto requests = std::move(pending_maintenance_requests_);
    pending_maintenance_requests_.clear();
    return requests;
}

std::vector<GameId> Simulation::TakeActivityRequests() {
    auto requests = std::move(pending_activity_requests_);
    pending_activity_requests_.clear();
    return requests;
}

void Simulation::request_uninstall(GameId id, bool delete_saves, bool batch) {
    pending_install_requests_.push_back({InstallRequestKind::Uninstall, id, {}, delete_saves});
    pending_uninstalls_.insert(id);
    if (const auto* game = find_game(state_, id); game && !game->title.empty()) uninstall_titles_[id] = game->title;
    // Asked again: how it ended before is no longer listed, so the game shows once, as it goes now.
    std::erase_if(state_.ended_uninstalls, [id](const EndedUninstall& ended) { return ended.game_id == id; });
    previous_account_uninstalls_.erase(id);
    if (batch) batch_uninstalls_.insert(id);
    else batch_uninstalls_.erase(id);
}

// Queue keeps how this session's uninstalls ended, newest first: a game's latest stop, and every finish.
void Simulation::end_uninstall(GameId id, const std::string& stopped) {
    // It is no longer at work: Queue lists it as ended at once, not also as running until the
    // controller's next list.
    std::erase_if(state_.preparation_queue.uninstalls, [id](const LiveUninstall& item) { return item.game_id == id; });
    pending_uninstalls_.erase(id);
    if (previous_account_uninstalls_.erase(id)) {
        uninstall_titles_.erase(id);
        return;
    }
    auto& ended = state_.ended_uninstalls;
    std::erase_if(ended, [id](const EndedUninstall& entry) { return entry.game_id == id && !entry.stopped.empty(); });
    std::string title;
    if (const auto* game = find_game(state_, id); game && !game->title.empty()) title = game->title;
    else if (const auto known = uninstall_titles_.find(id); known != uninstall_titles_.end()) title = known->second;
    ended.insert(ended.begin(), {id, std::move(title), stopped, state_.clock_ticks});
    uninstall_titles_.erase(id);
}

const EndedUninstall* Simulation::uninstall_stop(GameId id) const {
    for (const auto& ended : state_.ended_uninstalls)
        if (ended.game_id == id && !ended.stopped.empty()) return &ended;
    return nullptr;
}

void Simulation::NotifyUninstallStopped(GameId id, const std::string& reason) {
    end_uninstall(id, reason);
    const auto* game = find_game(state_, id);
    const auto text = (game ? game->title : std::string("A game")) + " · " + reason;
    // A game of a batch is followed in Queue, which keeps the reason: a corner notice is enough there.
    const bool batch = batch_uninstalls_.erase(id) > 0;
    push_notice({"uninstall-" + ToKey(id), "Uninstall stopped", "", false, kLiveNoticeSeconds * 2, true, text, !batch});
    rebuild_view();
}

void Simulation::NotifyUninstallFinished(GameId id) {
    end_uninstall(id, {});
    batch_uninstalls_.erase(id);
    rebuild_view();
}

void Simulation::NotifyPlayStopped(GameId id, const std::string& reason) {
    // A start from Home is reported as Playden starts, before its store's library: wait for the name.
    if (live() && !find_game(state_, id) && !catalog_sources_.count(id.store)) {
        pending_play_stops_[id] = reason;
        return;
    }
    tell_play_stopped(id, reason);
}

void Simulation::tell_play_stopped(GameId id, const std::string& reason) {
    const auto* game = find_game(state_, id);
    rechosen_after_refusal_.erase(id);
    const auto text = (game ? game->title : std::string("A game")) + " · " + reason;
    // A refused Prepare that another program or "Try anyway" may get past offers Game settings.
    if (live() && game_setups_.count(id) && game_setups_.at(id).draft) {
        show_alert("Can't play", text, id);
        rebuild_view();
        return;
    }
    push_notice({"play-" + ToKey(id), "Can't play", "", false, kLiveNoticeSeconds * 2, true, text});
    rebuild_view();
}

void Simulation::RequestRuntimePrepare(GameId id) {
    if (id.valid()) pending_runtime_prepare_requests_.push_back(id);
}

void Simulation::UpdatePreparationQueue(PreparationQueue queue) {
    // A dismissal or Prepare next the runtime has not carried out yet keeps showing as asked; once the
    // runtime's list no longer has the entry, or has the game first, it is done.
    std::erase_if(prepared_dismissed_, [&queue](const LiveCompletedPreparation& done) {
        return std::find(queue.completed.begin(), queue.completed.end(), done) == queue.completed.end();
    });
    for (const auto& done : prepared_dismissed_) std::erase(queue.completed, done);
    if (prepare_next_asked_) {
        const auto found = std::find(queue.waiting.begin(), queue.waiting.end(), *prepare_next_asked_);
        if (found == queue.waiting.end() || found == queue.waiting.begin()) prepare_next_asked_.reset();
        else std::rotate(queue.waiting.begin(), found, found + 1);
    }
    // Uninstalled games may leave the library (imported ones): their names are kept for Queue.
    for (const auto& uninstall : queue.uninstalls)
        if (const auto* game = find_game(state_, uninstall.game_id); game && !game->title.empty())
            uninstall_titles_[uninstall.game_id] = game->title;
    if (queue == state_.preparation_queue) return;
    state_.preparation_queue = std::move(queue);
    rebuild_view();
}

std::vector<GameId> Simulation::TakePrepareNextRequests() { return std::exchange(pending_prepare_next_requests_, {}); }

std::vector<LiveCompletedPreparation> Simulation::TakePreparedDismissals() {
    return std::exchange(pending_prepared_dismissals_, {});
}

bool Simulation::TakePreparedClear() { return std::exchange(pending_prepared_clear_, false); }

// Moves a waiting game to the front of the preparation queue; the game being prepared now finishes first.
// Focus stays on the game, now first.
void Simulation::prepare_next(GameId id) {
    auto& waiting = state_.preparation_queue.waiting;
    const auto found = std::find(waiting.begin(), waiting.end(), id);
    if (found == waiting.end() || found == waiting.begin()) return;
    std::rotate(waiting.begin(), found, found + 1);
    if (live()) {
        pending_prepare_next_requests_.push_back(id);
        prepare_next_asked_ = id;
    }
    state_.queue_expanded = kQueuePreparation;
    state_.navigation.focus_key = "prep-" + ToKey(id);
    const auto* game = find_game(state_, id);
    const auto& active = state_.preparation_queue.active;
    const auto* current = active ? find_game(state_, *active) : nullptr;
    push_notice({"prepare-next", (game ? game->title : std::string("The game")) + " is up next", "", false, kLiveNoticeSeconds,
                 false, current ? "After " + current->title : "It starts next"});
}

std::vector<GameId> Simulation::TakeRuntimePlayRequests() {
    auto requests = std::move(pending_runtime_play_requests_);
    pending_runtime_play_requests_.clear();
    return requests;
}

bool Simulation::TakeRuntimeCleanupRequest() {
    const bool requested = pending_runtime_cleanup_;
    pending_runtime_cleanup_ = false;
    return requested;
}

void Simulation::SetRuntimeLeftovers(std::size_t count) {
    if (!live() || state_.runtime_leftover_requests == count) return;
    state_.runtime_leftover_requests = count;
    rebuild_view();
}

void Simulation::SetKitInfo(LiveKitInfo kit) {
    if (!live()) return;
    state_.kit = std::move(kit);
    rebuild_view();
}

void Simulation::UpdateRuntimeProblem(const std::string& raw) {
    if (!live()) return;
    std::string words;
    if (raw.find("data root") != std::string::npos)
        words = "Playden can't use its data folder /data/playden. Make it writable (chmod 777 over FTP) and restart Playden.";
    else if (raw.find("directory boundary") != std::string::npos)
        words = "The console's /data is mounted in a way Playden can't use. ShadowMountPlus 1.7beta4 or later does this: "
                "load 1.7beta2 and restart Playden.";
    else if (!raw.empty())
        words = "The game runtime couldn't start (" + raw + "). Restart Playden; if it stays, reinstall it.";
    if (words == state_.runtime_problem) return;
    state_.runtime_problem = words;
    state_.notices.erase(std::remove_if(state_.notices.begin(), state_.notices.end(),
                                        [](const Notice& notice) { return notice.id == "runtime"; }),
                         state_.notices.end());
    if (!words.empty())
        state_.notices.push_back({"runtime", "Games can't start · see Runtime status in Settings", "", true, 0});
    rebuild_view();
}

void Simulation::ShowLayoutNotice(const std::string& text) {
    if (!live() || text.empty()) return;
    push_notice({"layout", text, "", false, kLiveNoticeSeconds * 2, false, ""});
    rebuild_view();
}

void Simulation::ShowRootAccessMissing(const std::string& raw, bool granted) {
    if (!live()) return;
    // The whole failure and the helper's diagnostic, which the screen wraps: they are what the
    // player reports. Only a runaway message is cut. The unused fallback launcher's path check is left out.
    constexpr std::size_t kDetailLimit = 1000;
    std::string detail = raw;
    if (const auto from = detail.find("; launcher paths unavailable: "); from != std::string::npos) {
        const auto to = detail.find("; ", from + 2);
        detail.erase(from, to == std::string::npos ? std::string::npos : to - from);
    }
    if (detail.size() > kDetailLimit) detail = detail.substr(0, kDetailLimit - 1) + "…";
    state_.root_access_missing = true;
    state_.root_access_granted = granted;
    state_.root_access_detail = std::move(detail);
    state_.notices.clear();
    state_.navigation.modals.clear();
    state_.editor.active = false;
    state_.navigation.route = Route::Startup;
    state_.navigation.focus_key = "root-quit";
    rebuild_view();
}

void Simulation::UpdateSelfUpdate(const LiveSelfUpdate& update) {
    if (!live()) return;
    const LiveSelfUpdate before = state_.self_update;
    const bool changed = before.phase != update.phase || before.done != update.done || before.total != update.total ||
                         before.available != update.available || before.error != update.error ||
                         before.part != update.part || before.current != update.current ||
                         before.bytes_per_second != update.bytes_per_second || before.previous != update.previous ||
                         before.quiet != update.quiet || before.current_notes_url != update.current_notes_url ||
                         before.whats_new != update.whats_new || before.moved_to != update.moved_to ||
                         before.kit_phase != update.kit_phase || before.kit_done != update.kit_done ||
                         before.kit_total != update.kit_total || before.kit_error != update.kit_error ||
                         !update.finished_message.empty();
    if (!changed) {
        show_update_prompt();
        return;
    }
    state_.self_update = update;
    if (!update.finished_message.empty() && update.whats_new) {
        // Updated: the release notes open once, as a dialog, when nothing else is open.
        pending_update_prompt_ = "whats-new";
        state_.self_update.finished_message.clear();
        self_update_requests_.push_back("acknowledge");
    } else if (!update.finished_message.empty()) {
        push_notice({"self-update-finished", update.finished_message, "", false, kLiveNoticeSeconds * 2,
                     update.finished_failed, "", false});
        state_.self_update.finished_message.clear();
        self_update_requests_.push_back("acknowledge");
    }
    if (!update.moved_to.empty() && before.moved_to.empty()) {
        pending_update_prompt_ = "moved";
    } else if (update.phase == "available" && update.quiet) {
        self_update_prompted_ = "available " + update.available;  // Settings still lists it
    } else if (update.phase == "available" && self_update_prompted_ != "available " + update.available) {
        self_update_prompted_ = "available " + update.available;
        pending_update_prompt_ = "available";
    } else if (update.phase == "staged" && self_update_prompted_ != "staged " + update.available) {
        self_update_prompted_ = "staged " + update.available;
        pending_update_prompt_ = "staged";
    } else if (update.phase == "failed" && before.phase != "failed") {
        // A check that ran on its own fails quietly; what the player started says why it stopped.
        if (update.user_check || before.phase == "downloading" || before.phase == "unpacking")
            show_alert("Playden update stopped", update.error);
    } else if (update.phase == "up-to-date" && before.phase == "checking" && update.user_check) {
        push_notice({"self-update-check", "Playden is up to date", "", false, kLiveNoticeSeconds, false,
                     "Version " + update.current});
    }
    // The kit-only install. An update on offer or under way brings the kit itself, so it asks first.
    const bool update_brings_kit = update.phase == "available" || update.phase == "downloading" ||
                                   update.phase == "unpacking" || update.phase == "staged";
    if (update.kit_phase == "checking") kit_prompted_ = false;
    if (update.kit_phase == "available" && !kit_prompted_ && !update_brings_kit && pending_update_prompt_.empty()) {
        kit_prompted_ = true;
        pending_update_prompt_ = "kit";
    } else if (update.kit_phase == "failed" && before.kit_phase != "failed" &&
               (before.kit_phase == "downloading" || before.kit_phase == "unpacking")) {
        show_alert("Runtime kit download stopped", update.kit_error);
    } else if (update.kit_phase == "installed" && before.kit_phase != "installed") {
        push_notice({"kit-installed", "Runtime kit installed", "", false, kLiveNoticeSeconds, false,
                     "Games can be prepared and played now"});
    }
    sync_self_update_job();
    show_update_prompt();
    rebuild_view();
}

std::vector<std::string> Simulation::TakeSelfUpdateRequests() {
    auto requests = std::move(self_update_requests_);
    self_update_requests_.clear();
    return requests;
}

// The update's prompts wait, like alerts, until no other dialog, sheet or keyboard is open.
void Simulation::show_update_prompt() {
    const auto route = state_.navigation.route;
    // The kit's offer shows before sign-in too (Connect Steam and the sign-in screens, not while the
    // saved account loads): it downloads while the player signs in.
    const bool kit = pending_update_prompt_ == "kit";
    const bool before_sign_in = (route == Route::Startup && !state_.startup_loading) || route == Route::SignIn;
    if (pending_update_prompt_.empty() || !state_.navigation.modals.empty() || state_.editor.active ||
        route == Route::Session || (route == Route::Startup && !(kit && before_sign_in)) ||
        (route == Route::SignIn && !kit))
        return;
    const auto& update = state_.self_update;
    if (kit && update.kit_phase == "available") {
        ModalEntry prompt{"confirmation", update.kit_replaces ? "Replace the runtime kit?" : "Download the runtime kit?",
                          state_.navigation.focus_key, {"kit-install", "ready-later"}, 0, false};
        const std::string size = update.kit_download_bytes ? "The download is " + format_bytes(update.kit_download_bytes) + ". "
                                                           : std::string();
        prompt.body = (update.kit_replaces
                           ? "The installed runtime kit isn't the one Playden " + update.current +
                                 " was made for, so games may not run as this version expects. Playden can put the "
                                 "right one in its place. "
                           : std::string("Playden needs its runtime kit to prepare and play Windows games. ")) +
                      size + "You can sign in and keep using Playden while it downloads.";
        open_modal(std::move(prompt));
    } else if (pending_update_prompt_ == "available" && update.phase == "available") {
        const std::string beta = update.available.find("-beta.") != std::string::npos ? " (Beta)" : "";
        // The same version as a new app: the step that follows an update from 0.7.1-beta.2 or older.
        const bool same = update.moves && update.available == update.current;
        ModalEntry prompt{"confirmation",
                          same ? std::string("Move Playden to its new app")
                               : "Playden " + update.available + beta + " is available",
                          state_.navigation.focus_key, {"self-update-install", "ready-later"}, 0, false};
        const std::string move = "Playden " + update.available + " installs as a new app next to this one, so "
                                 "ProsperoStore can install and update it. Your sign-in, games and settings stay, and "
                                 "this app is removed once the new one has started.\n\n";
        prompt.body = (same ? std::string() : update.notes.empty() ? std::string() : update.notes + "\n\n") +
                      (update.moves ? move : std::string()) + "The download is " +
                      format_bytes(update.download_bytes) +
                      (update.kit_needed ? ", with a new runtime kit." : ".") +
                      " You can keep using Playden while it downloads.";
        prompt.link = update.notes_url;
        open_modal(std::move(prompt));
    } else if (pending_update_prompt_ == "staged" && update.phase == "staged") {
        ModalEntry prompt{"confirmation", "Playden " + update.available + " is ready", state_.navigation.focus_key,
                          {"self-update-restart", "ready-later"}, 0, false};
        prompt.body = "Restart Playden to finish the update. It closes and opens again by itself; if it doesn't, "
                      "open it from the Home screen.";
        open_modal(std::move(prompt));
    } else if (pending_update_prompt_ == "moved" && !update.moved_to.empty()) {
        // This title updated to the new one, which hasn't started yet: its tile may be missing.
        ModalEntry prompt{"confirmation", "Playden " + update.moved_to + " is a new app", state_.navigation.focus_key,
                          {"successor-open", "ready-later"}, 0, false};
        prompt.body = "Playden " + update.moved_to + " was installed as a new app, and this one no longer updates. "
                      "Open the new Playden from Home.\n\nIf it isn't on Home, choose Open the new Playden: this "
                      "Playden closes, asks the PS5 to add the new one, and opens it. If this Playden opens again "
                      "instead, restart the PS5 and look on Home.";
        open_modal(std::move(prompt));
    } else if (pending_update_prompt_ == "whats-new" || pending_update_prompt_ == "release-notes") {
        open_release_notes(pending_update_prompt_ == "whats-new");
    }
    pending_update_prompt_.clear();
}

// The update shows on Downloads while it downloads, unpacks and waits for the restart.
void Simulation::sync_self_update_job() {
    state_.jobs.erase(std::remove_if(state_.jobs.begin(), state_.jobs.end(),
                                     [](const Job& job) { return job.kind == "self-update"; }),
                      state_.jobs.end());
    const auto& update = state_.self_update;
    if (update.kit_phase == "downloading" || update.kit_phase == "unpacking") {
        Job job;
        job.id = "kit-install";
        job.kind = "self-update";
        job.queue_order = -1;
        job.title = "Runtime kit";
        const bool downloading = update.kit_phase == "downloading";
        job.state = downloading ? JobState::Downloading : JobState::Verifying;
        job.stage_text = downloading ? "Downloading the runtime kit" : "Unpacking the runtime kit";
        job.state_text = downloading ? "Downloading" : "Unpacking";
        if (downloading) {
            job.bytes_per_second = update.bytes_per_second;
            job.eta_seconds = update.eta_seconds;
        }
        job.completed_bytes = update.kit_done;
        job.total_bytes = update.kit_total;
        state_.jobs.push_back(std::move(job));
    }
    if (update.phase != "downloading" && update.phase != "unpacking" && update.phase != "staged") return;
    const bool kit = update.part == "kit";
    Job job;
    job.id = "playden-update";
    job.kind = "self-update";
    job.queue_order = -1;
    job.title = "Playden " + update.available;
    if (update.phase == "downloading") {
        job.state = JobState::Downloading;
        job.stage_text = kit ? "Downloading the runtime kit" : "Downloading Playden";
        job.state_text = "Downloading";
        job.bytes_per_second = update.bytes_per_second;
        job.eta_seconds = update.eta_seconds;
    } else if (update.phase == "unpacking") {
        job.state = JobState::Verifying;
        job.stage_text = kit ? "Unpacking the runtime kit" : "Unpacking Playden";
        job.state_text = "Unpacking";
    } else {
        job.state = JobState::Downloaded;
        job.stage_text = "Restart Playden to finish";
        job.state_text = "Ready · restart Playden";
    }
    job.completed_bytes = update.phase == "staged" ? update.download_bytes : update.done;
    job.total_bytes = update.phase == "staged" ? update.download_bytes : update.total;
    state_.jobs.push_back(std::move(job));
}

// The running version's release notes, with its release page as a QR code: once after an update
// ("What's new"), and from Settings › About › Release notes.
// The selected game changed under the dialogs: the ones about a game close, Playden's own update
// dialogs (often opened at startup, before the library arrives) stay.
void Simulation::close_game_modals() {
    auto& modals = state_.navigation.modals;
    modals.erase(std::remove_if(modals.begin(), modals.end(), [](const ModalEntry& modal) {
        return std::none_of(modal.action_keys.begin(), modal.action_keys.end(), [](const std::string& key) {
            return key == "self-update-install" || key == "self-update-restart" || key == "release-notes-ok" ||
                   key == "kit-install" || key == "successor-open";
        });
    }), modals.end());
}

void Simulation::open_release_notes(bool after_update) {
    const auto& update = state_.self_update;
    ModalEntry notes{"confirmation",
                     (after_update ? "What's new in Playden " : "Playden ") + update.current +
                         (update.current.find("-beta.") != std::string::npos ? " (Beta)" : "") +
                         (after_update ? std::string() : std::string(" release notes")),
                     state_.navigation.focus_key, {"release-notes-ok"}, 0, false};
    notes.body = (after_update ? "Playden was updated to " + update.current + ".\n\n" : std::string()) +
                 (update.current_notes.empty() ? std::string("What changed in this version is on its release page.")
                                               : update.current_notes);
    notes.link = update.current_notes_url;
    open_modal(std::move(notes));
}

void Simulation::request_self_update_restart(const std::string& request) {
    const bool busy = std::any_of(state_.runtime_games.begin(), state_.runtime_games.end(), [](const auto& entry) {
        const auto& game = entry.second;
        return game.status == "Preparing" || !game.uninstall_status.empty() || game.maintenance_running;
    });
    if (busy && request == "open-successor") {
        show_alert("Open the new Playden later", "A game is being prepared or removed. When it is done, close Playden "
                                                  "and open the new one from Home, or restart the PS5 if it isn't there.");
        return;
    }
    if (busy) {
        show_alert("Restart Playden later", "A game is being prepared or removed. Restart Playden from Settings when it is done.");
        return;
    }
    self_update_requests_.push_back(request);
    state_.quit_requested = true;
}

void Simulation::UpdateRuntime(RuntimeState runtime, const std::vector<LiveRuntimeGame>& games) {
    if (!live()) return;
    state_.runtime = runtime;
    std::map<GameId, LiveRuntimeGame> next;
    for (const auto& game : games) next[game.game_id] = game;
    for (const auto& [id, game] : next) {
        if (game.status != "Not prepared") rechosen_after_refusal_.erase(id);
        const auto before = state_.runtime_games.find(id);
        // Launch outcomes are separate from setup readiness. Surface a new actionable
        // refusal once through the existing notice path, keeping diagnostics off Details.
        if (!game.play_detail.empty() &&
            (before == state_.runtime_games.end() || before->second.play_detail != game.play_detail)) {
            if (game.play_detail.rfind("Could not start:", 0) == 0 || game.play_detail == "The game could not start.")
                NotifyPlayStopped(id, "The game could not start. Open Logs in Settings for details.");
            else if (game.play_detail == "The game's PS5 title may still be running. Close it, then press Play." ||
                     game.play_detail == "Choose which Cloud saves to keep, or press Play again to start without syncing." ||
                     game.play_detail == "Cloud saves could not be checked. Press Play again to start without syncing.")
                NotifyPlayStopped(id, game.play_detail);
        }
        if (before == state_.runtime_games.end()) continue;
        const auto* known = find_game(state_, id);
        const std::string title = known ? known->title : "The game";
        // A finished maintenance request says how it went; applying settings speaks only when it fails.
        if (before->second.maintenance_running && !game.maintenance_running && game.maintenance) {
            const bool apply = *game.maintenance == MaintenanceKind::ApplySettings;
            const bool deferred = game.maintenance_message == "Changes apply the next time Playden starts the game.";
            if (!apply || (!game.maintenance_ok && !deferred))
                push_notice({"maintenance-" + ToKey(id), game.maintenance_ok ? title + ": done" : title + ": not done", "",
                             false, kLiveNoticeSeconds * (game.maintenance_ok ? 1 : 2), !game.maintenance_ok, game.maintenance_message});
        }
        // A Playden session ended: verbose or input logging lasts one session and goes back to Normal,
        // and settings saved while the game ran are applied now, so a start from Home uses them.
        if (before->second.play_status == "Playing" && game.play_status != "Playing") {
            const auto record = state_.game_settings.find(id);
            if (record != state_.game_settings.end() && record->second.overrides.count("logging") &&
                record->second.overrides.at("logging") != "normal" && can_change_game_settings()) {
                record->second.overrides.erase("logging");
                ++record->second.revision;
                save_profile();
                push_notice({"verbose-logging-" + ToKey(id), "Detailed logging is off again", "", false,
                             kLiveNoticeSeconds, false, title + " logs at the Normal level from its next start"});
            }
            if (game.prepared && record != state_.game_settings.end() && record->second.revision > game.settings_revision)
                pending_maintenance_requests_.push_back({id, MaintenanceKind::ApplySettings});
        }
    }
    state_.runtime_games = std::move(next);
    // A quit to Home waiting for a preparation goes once nothing is prepared; Cancel drops it.
    if (state_.quit_waiting) {
        const bool waiting = !state_.navigation.modals.empty() && state_.navigation.modals.back().title == quit_wait_title_;
        if (!waiting) {
            state_.quit_waiting = false;
        } else if (std::none_of(state_.runtime_games.begin(), state_.runtime_games.end(),
                                [](const auto& entry) { return entry.second.status == "Preparing"; })) {
            state_.quit_waiting = false;
            state_.navigation.modals.clear();
            state_.quit_requested = true;
        }
    }
    for (auto& installation : state_.installations) {
        const auto found = state_.runtime_games.find(installation.game_id);
        installation.runtime_ready = found != state_.runtime_games.end() && found->second.ready;
    }
    show_ready_prompt();
    open_settings_after_prepare();
    refresh_installed_manager();
    rebuild_view();
}

void Simulation::open_settings_after_prepare() {
    for (auto it = settings_after_prepare_.begin(); it != settings_after_prepare_.end();) {
        const auto setup = game_setups_.find(*it);
        if (!game_prepared(*it) && (setup == game_setups_.end() || !setup->second.draft)) { ++it; continue; }
        // Only over the same game's page, with nothing else open; otherwise More offers it.
        if (state_.navigation.selected_game_id == *it && state_.navigation.modals.empty()) open_game_settings();
        it = settings_after_prepare_.erase(it);
    }
}

void Simulation::NotifyReadyToPlay(GameId id) {
    if (!live()) return;
    if (std::find(ready_prompts_.begin(), ready_prompts_.end(), id) == ready_prompts_.end()) ready_prompts_.push_back(id);
    show_ready_prompt();
    rebuild_view();
}

// "<game> is ready to play": a dialog, because notifications take no input. It waits
// for its own game's page with no dialog, sheet or keyboard open, and is dropped
// once the game no longer waits for the PS5. Play quits to Home without asking again.
void Simulation::show_ready_prompt() {
    if (!live() || ready_prompts_.empty()) return;
    std::erase_if(ready_prompts_, [this](GameId id) {
        const auto found = state_.runtime_games.find(id);
        return found == state_.runtime_games.end() || !found->second.awaiting_registration;
    });
    const auto route = state_.navigation.route;
    if (ready_prompts_.empty() || !state_.navigation.modals.empty() || state_.editor.active ||
        route != Route::Details)
        return;
    const auto id = state_.navigation.detail_id.value_or(state_.navigation.selected_game_id);
    const auto pending = std::find(ready_prompts_.begin(), ready_prompts_.end(), id);
    if (pending == ready_prompts_.end()) return;
    ready_prompts_.erase(pending);
    ready_prompt_game_ = id;
    const auto* game = find_game(state_, id);
    ModalEntry prompt{"confirmation", (game ? game->title : std::string("Your game")) + " is ready to play",
                      state_.navigation.focus_key, {"ready-later", "quit-to-home"}, 1, false};
    prompt.body = "Play quits Playden to the PS5 Home screen, where the game appears. Start it from there.";
    open_modal(std::move(prompt));
}

void Simulation::show_alert(std::string title, std::string body, GameId settings_game) {
    const bool queued = std::any_of(alert_prompts_.begin(), alert_prompts_.end(), [&](const AlertPrompt& alert) {
        return alert.title == title && alert.body == body;
    });
    const bool showing = !state_.navigation.modals.empty() && state_.navigation.modals.back().title == title &&
                         state_.navigation.modals.back().body == body;
    if (!queued && !showing) alert_prompts_.push_back({std::move(title), std::move(body), settings_game});
    show_alert_prompt();
}

// A failure dialog with OK. Like the ready-to-play prompt it waits until no other
// dialog, sheet or keyboard is open, so it never covers what the player is doing.
void Simulation::show_alert_prompt() {
    const auto route = state_.navigation.route;
    if (alert_prompts_.empty() || !state_.navigation.modals.empty() || state_.editor.active ||
        route == Route::Startup || route == Route::SignIn || route == Route::Session)
        return;
    auto alert = std::move(alert_prompts_.front());
    alert_prompts_.erase(alert_prompts_.begin());
    alert_settings_game_ = alert.settings_game;
    std::vector<std::string> actions = {"alert-ok"};
    if (alert.settings_game.valid()) actions = {"alert-ok", "alert-settings"};
    ModalEntry prompt{"confirmation", std::move(alert.title), state_.navigation.focus_key, std::move(actions), 0, false};
    prompt.body = alert.body.empty() ? std::string(" ") : std::move(alert.body);
    open_modal(std::move(prompt));
}

void Simulation::UpdateCloud(const std::vector<LiveCloudGame>& games) {
    if (!live()) return;
    for (const auto& game : games) {
        const auto previous = state_.cloud.find(game.game_id);
        const auto before = previous == state_.cloud.end() ? CloudStatus::None : previous->second.status;
        // The game Play is waiting for speaks through its own dialog.
        if (before != game.status && game.game_id != quit_sync_game_) {
            const auto* known = find_game(state_, game.game_id);
            const std::string title = known ? known->title : "A game";
            const auto id = "cloud-" + ToKey(game.game_id);
            if (game.status == CloudStatus::Synced && before != CloudStatus::None)
                push_notice({id, "Cloud saves synced", "", false, kLiveNoticeSeconds, false, title + " · " + game.message});
            // A check the player did not start never opens a dialog: the Saves tab shows it.
            else if (game.status == CloudStatus::Failed && !game.background)
                push_notice({id, "Cloud sync failed", "", false, kLiveNoticeSeconds * 2, true, title + " · " + game.message});
            // A choice is announced when it is new, not each time a check finds it again.
            else if ((game.status == CloudStatus::Conflict || game.status == CloudStatus::NeedsAccount) && before != CloudStatus::None &&
                     settled_cloud_[game.game_id] != game.status)
                push_notice({id, "Cloud saves need a choice", "", false, kLiveNoticeSeconds * 2, false,
                             title + (game.background ? " · open Saves" : " · open its details")});
        }
        if (game.status != CloudStatus::Checking && game.status != CloudStatus::Syncing && !game.queued)
            settled_cloud_[game.game_id] = game.status;
        state_.cloud[game.game_id] = game;
    }
    rebuild_view();
}

void Simulation::UpdateInstalls(const std::vector<LiveInstallJob>& jobs, const std::vector<LiveInstalledGame>& installed,
                                const std::vector<LiveCompletedDownload>& completed) {
    if (!live()) return;
    std::map<GameId, JobState> job_states;
    for (const auto& job : jobs) job_states[job.game_id] = job.state;
    std::set<GameId> installed_ids;
    for (const auto& game : installed) installed_ids.insert(game.game_id);
    // A copy's job, or a move's finished install, ends its starting state.
    std::erase_if(import_transfers_starting_, [&](const auto& entry) {
        return job_states.count(entry.first) || installed_ids.count(entry.first);
    });
    if (live_installs_seen_) {
        const auto title = [this](GameId id) {
            const auto* game = find_game(state_, id);
            return game ? game->title : std::string("A game");
        };
        for (const auto& [id, previous] : live_job_states_) {
            if (job_states.contains(id) || !installed_ids.contains(id)) continue;
            push_notice({"install-" + ToKey(id), "Download complete", "", false, kLiveNoticeSeconds, false, title(id)});
        }
        for (const auto& job : jobs) {
            const auto previous = live_job_states_.find(job.game_id);
            if (job.state != JobState::Failed || (previous != live_job_states_.end() && previous->second == JobState::Failed)) continue;
            // The queue carries on with the next game and Downloads keeps the failure with Resume, so the
            // player is told in a corner instead of a dialog that stops what they are doing.
            push_notice({"install-" + ToKey(job.game_id), "Download failed", "", false, kLiveNoticeSeconds * 2, true,
                         title(job.game_id) + (job.failure.empty() ? std::string() : " · " + job.failure), false});
        }
        for (const auto id : live_installed_ids_) {
            if (installed_ids.contains(id) || job_states.contains(id) || !pending_uninstalls_.contains(id)) continue;
            // A batch's games are followed in Queue instead.
            if (batch_uninstalls_.contains(id)) continue;
            push_notice({"install-" + ToKey(id), "Game uninstalled", "", false, kLiveNoticeSeconds, false, title(id)});
        }
    }
    for (auto it = pending_uninstalls_.begin(); it != pending_uninstalls_.end();)
        it = installed_ids.contains(*it) ? std::next(it) : pending_uninstalls_.erase(it);
    live_installs_seen_ = true;
    live_job_states_ = std::move(job_states);
    live_installed_ids_ = std::move(installed_ids);
    std::vector<Job> projected;
    int order = 0;
    for (const auto& live_job : jobs) {
        Job job;
        job.id = ToKey(live_job.game_id);
        job.game_id = live_job.game_id;
        job.queue_order = order++;
        job.state = live_job.state;
        job.completed_bytes = live_job.done_bytes;
        job.total_bytes = live_job.total_bytes;
        job.held_bytes = live_job.held_bytes;
        job.checking_licenses = live_job.state == JobState::Downloading && live_job.checking_licenses;
        job.failure = live_job.failure;
        job.volume_id = live_job.volume_id;
        if (live_job.state == JobState::Downloading) {
            job.bytes_per_second = live_job.bytes_per_second;
            job.eta_seconds = live_job.eta_seconds;
        }
        projected.push_back(std::move(job));
    }
    // Finished downloads follow the queue, newest first, until the player dismisses them.
    for (const auto& finished : completed) {
        if (live_job_states_.contains(finished.game_id) || !live_installed_ids_.contains(finished.game_id)) continue;
        Job job;
        job.id = ToKey(finished.game_id);
        job.game_id = finished.game_id;
        job.queue_order = order++;
        job.state = JobState::Downloaded;
        job.completed_bytes = job.total_bytes = finished.install_bytes;
        job.volume_id = finished.volume_id;
        job.finished_at = finished.finished_at;
        job.completion_applied = true;
        projected.push_back(std::move(job));
    }
    std::vector<Installation> installations;
    for (const auto& game : installed)
        installations.push_back({game.game_id, game.volume_id, game.install_bytes, game.available,
                                 state_.runtime_games.count(game.game_id) && state_.runtime_games.at(game.game_id).ready, false});
    state_.jobs = std::move(projected);
    sync_self_update_job();
    state_.installations = std::move(installations);
    apply_live_volume_usage();
    refresh_installed_manager();
    rebuild_view();
}

void Simulation::UpdateVolumes(const std::vector<LiveVolume>& volumes, const std::string& default_volume_id,
                               bool default_volume_saved) {
    if (!live()) return;
    std::vector<Volume> projected;
    for (const auto& live_volume : volumes) {
        Volume volume;
        volume.id = live_volume.id;
        volume.label = live_volume.label;
        volume.available = live_volume.available;
        volume.writable = live_volume.available && live_volume.writable;
        volume.capacity_known = live_volume.total_bytes.has_value() && live_volume.free_bytes.has_value();
        if (volume.capacity_known) {
            volume.capacity_bytes = *live_volume.total_bytes;
            volume.base_used_bytes = *live_volume.total_bytes - std::min(*live_volume.free_bytes, *live_volume.total_bytes);
        }
        projected.push_back(std::move(volume));
    }
    state_.volumes = std::move(projected);
    selected_volume_id_ = default_volume_id;
    state_.default_volume_id = default_volume_id;
    // Said once each time saving starts failing, not on every refresh.
    if (!default_volume_saved && default_volume_saved_) {
        const auto* volume = find_volume(state_, default_volume_id);
        push_notice({"default-volume", "Install drive not saved", "", false, kLiveNoticeSeconds * 2, true,
                     (volume ? volume->label : std::string("This drive")) +
                         " is used until Playden closes · Console storage may be full"});
    }
    default_volume_saved_ = default_volume_saved;
    apply_live_volume_usage();
    rebuild_view();
}

namespace {
// Live installs go ahead when space fits, when the size is checked later by the downloader, or
// when the console cannot read free space at all.
bool live_install_allowed(InstallReadiness readiness, bool free_space_supported) {
    return readiness == InstallReadiness::Ready || readiness == InstallReadiness::UnknownSize ||
           (readiness == InstallReadiness::UnknownCapacity && !free_space_supported);
}

}  // namespace

// The Steam option is kept as Steam's (its arguments, folder and the launcher and Unreal rules
// preparation applies), only first; Game settings › Program still lists the others.
void Simulation::choose_steam_launch(GameId id, const std::string& program) {
    if (!live() || !id.valid()) return;
    state_.game_settings[id].steam_launch = program;
    save_profile();
    // Answered: the row waits for the new setup, which no longer asks.
    if (auto setup = game_setups_.find(id); setup != game_setups_.end()) setup->second.steam_choices.clear();
    pending_runtime_prepare_requests_.push_back(id);
    rebuild_view();
}


// Used space is what the drive reports, which already includes what downloads have written or
// preallocated; reserved space is the rest of each download's install estimate, as the install
// service reserves it when admitting a download.
void Simulation::apply_live_volume_usage() {
    for (auto& volume : state_.volumes) {
        volume.games_bytes = 0;
        volume.reserved_bytes = 0;
        for (const auto& installation : state_.installations)
            if (installation.volume_id == volume.id) volume.games_bytes += installation.installed_bytes;
        for (const auto& job : state_.jobs) {
            if (job.volume_id != volume.id || job.state == JobState::Failed ||
                job.state == JobState::Cancelled || job.state == JobState::Downloaded) continue;
            const auto* game = find_game(state_, job.game_id);
            if (!game || !game->install_bytes) continue;
            const auto pending = steam::PendingInstallBytes(game->install_bytes, job.held_bytes);
            const auto maximum = std::numeric_limits<std::uint64_t>::max();
            volume.reserved_bytes = pending > maximum - volume.reserved_bytes ? maximum : volume.reserved_bytes + pending;
        }
        volume.used_bytes = volume.capacity_known ? volume.base_used_bytes : volume.games_bytes;
    }
}

void Simulation::queue_service_action(ServiceAction action, Store store) {
    constexpr std::size_t kMaxPendingActions = 32;
    const bool barrier = action == ServiceAction::SignOut || action == ServiceAction::SwitchAccount;
    if (barrier && std::find_if(pending_service_actions_.begin(), pending_service_actions_.end(), [&](const ServiceRequest& pending) {
            return pending.action == action && pending.store == store;
        }) != pending_service_actions_.end()) return;
    if (pending_service_actions_.size() >= kMaxPendingActions) {
        if (!barrier) return;
        const auto disposable = std::find_if(pending_service_actions_.begin(), pending_service_actions_.end(), [](const ServiceRequest& pending) {
            return pending != ServiceAction::SignOut && pending != ServiceAction::SwitchAccount;
        });
        if (disposable == pending_service_actions_.end()) return;
        pending_service_actions_.erase(disposable);
    }
    pending_service_actions_.push_back({action, store});
}

bool Simulation::beyond_steam() const {
    for (const auto& [store, account] : state_.accounts)
        if (store != Store::kSteam &&
            (account.authentication == AuthState::SignedIn || account.authentication == AuthState::Offline)) return true;
    return std::any_of(state_.games.begin(), state_.games.end(),
                       [](const Game& game) { return game.id.store != Store::kSteam; });
}

void Simulation::steam_restore_notice() {
    const auto& error = state_.account().error;
    push_notice({"steam-restore", "Steam isn't signed in", "", false, kLiveNoticeSeconds * 2, true,
                 error.empty() ? std::string("Sign in to Steam from Settings.") : error, false});
}

void Simulation::leave_steam_sign_in() {
    if (beyond_steam()) {
        state_.navigation.route = Route::Home;
        state_.navigation.focus_key = state_.games.empty() ? "home-library" : game_key(state_.games.front().id);
    } else {
        state_.navigation.route = Route::Startup;
        state_.navigation.focus_key = "startup-connect";
    }
}

void Simulation::clear_account_specific_state() {
    ++state_.account_generation;
    state_.account().challenge_url.clear();
    state_.account().challenge_expiry = 0;
    state_.account().last_sync = 0;
    state_.account().cache_available = false;
    state_.account().error.clear();
    // Only Steam's games go; other stores' games stay.
    state_.games.erase(std::remove_if(state_.games.begin(), state_.games.end(),
                                      [](const Game& game) { return game.id.store == Store::kSteam; }),
                       state_.games.end());
    catalog_sources_.erase(Store::kSteam);
    loading_sources_.erase(Store::kSteam);
    library_catalog_received_ = !catalog_sources_.empty();
    // Live organization (favorites, collections, ratings, notes) belongs to the device and stays.
    if (live()) state_.library.view_sort.clear();
    else state_.library = {};
    // Steam's installs and downloads go with its account; other stores' stay listed. So do its ended
    // uninstalls; one still running may finish, but how it ends is not listed for the next account.
    std::erase_if(state_.installations, [](const Installation& item) { return item.game_id.store == Store::kSteam; });
    std::erase_if(state_.ended_uninstalls, [](const EndedUninstall& ended) { return ended.game_id.store == Store::kSteam; });
    for (const auto& uninstall : state_.preparation_queue.uninstalls)
        if (uninstall.game_id.store == Store::kSteam) previous_account_uninstalls_.insert(uninstall.game_id);
    for (const auto id : pending_uninstalls_)
        if (id.store == Store::kSteam) previous_account_uninstalls_.insert(id);
    std::erase_if(pending_uninstalls_, [](GameId id) { return id.store == Store::kSteam; });
    std::erase_if(batch_uninstalls_, [](GameId id) { return id.store == Store::kSteam; });
    std::erase_if(uninstall_titles_, [](const auto& entry) { return entry.first.store == Store::kSteam; });
    std::erase_if(state_.jobs, [](const Job& job) { return job.game_id.store == Store::kSteam; });
    state_.volumes.clear();
    state_.cloud.clear();
    // Bottle settings belong to the installed game, independently of Steam sign-in.
    if (!live()) state_.game_settings.clear();
    state_.navigation.detail_id.reset();
    // A game of another store that is still listed stays selected.
    if (std::none_of(state_.games.begin(), state_.games.end(),
                     [this](const Game& game) { return game.id == state_.navigation.selected_game_id; }))
        state_.navigation.selected_game_id = {};
    state_.navigation.selected_index = 0;
    state_.navigation.home_shelf = 0;
    state_.navigation.home_shelf_focus.clear();
    state_.navigation.scroll_offsets.clear();
    state_.navigation.modals.clear();
    state_.editor = {};
    state_.session = SessionState::Idle;
    state_.session_game_id = {};
    state_.session_error.clear();
    // The live runtime belongs to the console: the controller projects it only when it changes, so a
    // Steam sign-in or sign-out must not reset it (games of other stores still play).
    if (!live()) state_.runtime = RuntimeState::Unavailable;
    state_.library_loading = !loading_sources_.empty();
    state_.library_syncing = state_.library_loading;
    state_.notices.erase(std::remove_if(state_.notices.begin(), state_.notices.end(),
                                        [](const Notice& notice) { return notice.id != "persistence" && notice.id != "runtime"; }),
                         state_.notices.end());
    state_.log_state = LogState::Ready;
    state_.log_lines.clear();
    state_.log_offset = 0;
    state_.log_context.clear();
    pending_destructive_action_.clear();
    selected_volume_id_.clear();
    editing_setting_id_.clear();
    editing_target_id_.clear();
    update_persistence_notice();
}

void Simulation::UpdateAccount(AccountUpdate update) {
    if (!live()) return;
    if (update.clear_library) clear_account_specific_state();
    // The saved sign-in is retried every few seconds; a repeat of the same failure leaves the
    // screen, its focus and any open list alone.
    const bool repeat = update.authentication == AuthState::Failed && state_.account().authentication == AuthState::Failed &&
                        update.sign_in == SignInStep::Qr && state_.sign_in.step == SignInStep::Qr &&
                        update.error == state_.account().error && update.storage_full == state_.account().storage_full;
    state_.account().storage_full = update.authentication == AuthState::Failed && update.storage_full;
    state_.account().authentication = update.authentication;
    state_.account().id = std::move(update.id);
    state_.account().display_name = std::move(update.display_name);
    state_.account().challenge_url = std::move(update.challenge_url);
    state_.account().error = std::move(update.error);
    state_.account().challenge_expiry = 0;
    state_.library_syncing = update.authentication == AuthState::Syncing;

    if (update.signed_in_with_password && *update.signed_in_with_password != state_.sign_in_password) {
        state_.sign_in_password = *update.signed_in_with_password;
        save_profile();
    }
    if (repeat) {
        // Nothing changed.
    } else if (update.authentication == AuthState::Failed && update.restoring && beyond_steam()) {
        // Another store's games are there to play: a Steam restore that failed doesn't take the screen.
        if (state_.navigation.route == Route::Startup || state_.navigation.route == Route::SignIn) leave_steam_sign_in();
    } else if (update.authentication == AuthState::Waiting || update.authentication == AuthState::Expired || update.authentication == AuthState::Failed) {
        state_.navigation.route = Route::SignIn;
        auto& form = state_.sign_in;
        if (update.sign_in == SignInStep::Qr) {
            if (form.step != SignInStep::Qr) clear_sign_in_secrets();
            form.step = SignInStep::Qr;
            form.error.clear();
            set_route_focus(update.authentication == AuthState::Expired ? "qr-refresh"
                            : update.authentication != AuthState::Failed ? "qr-cancel"
                            : state_.account().storage_full && !state_.installations.empty() ? "signin-installed" : "qr-retry");
        } else if (update.authentication != AuthState::Waiting) {
            // A failed password sign-in returns to the form, with the reason and the account name kept.
            clear_sign_in_secrets();
            form.step = SignInStep::Password;
            form.code_entry = false;
            form.error = state_.account().error;
            state_.account().error.clear();
            set_route_focus(form.account_name.empty() ? "signin-account" : "signin-password");
        } else {
            // A code typed instead of approving was wrong: approval in the app still works, so say both.
            const bool approval = update.code_rejected && form.step == SignInStep::Approval && form.code_entry;
            form.step = approval ? SignInStep::Approval : update.sign_in;
            form.error.clear();
            if (form.step == SignInStep::Password) {
                const auto& focus = state_.editor.active ? state_.editor.opener_focus_key : state_.navigation.focus_key;
                if (focus.rfind("signin-", 0) != 0 || focus == "signin-cancel")
                    set_route_focus(form.account_name.empty() ? "signin-account" : "signin-password");
            } else if (form.step == SignInStep::Submitting) {
                set_route_focus("signin-cancel");
            } else {
                steam::ZeroizeSecret(form.code);
                form.email_domain = update.email_domain;
                form.code_allowed = update.code_allowed;
                form.code_is_email = update.code_is_email;
                form.code_allowed = form.code_allowed || approval;
                form.code_entry = approval;
                form.code_sent = false;
                form.code_rejected = update.code_rejected;
                set_route_focus(form.step == SignInStep::Approval && !form.code_entry
                                    ? (form.code_allowed ? "guard-use-code" : "guard-cancel") : "guard-code");
            }
        }
    } else if (update.authentication == AuthState::SignedIn || update.authentication == AuthState::Offline) {
        if (state_.navigation.route == Route::Startup || state_.navigation.route == Route::SignIn) state_.navigation.route = Route::Home;
        state_.navigation.focus_key = state_.games.empty() ? "home-library" : game_key(state_.games.front().id);
    } else if (update.authentication == AuthState::SignedOut || update.authentication == AuthState::Cancelled) {
        leave_steam_sign_in();
    } else if (update.authentication == AuthState::Restoring) {
        // Another store's games stay on screen while Steam restores; with none, Startup waits for it.
        if (!beyond_steam() || state_.navigation.route == Route::Startup || state_.navigation.route == Route::SignIn) {
            state_.navigation.route = Route::Startup;
            state_.navigation.focus_key = "startup-loading";
        }
    }
    if (!repeat) restore_failed_sign_in_ = update.authentication == AuthState::Failed && update.restoring &&
                                           state_.navigation.route == Route::SignIn;
    // Steam's saved sign-in failed while another store's games are on screen: a notification says why;
    // the Account row keeps the error.
    if (!repeat && update.restoring && state_.navigation.route != Route::SignIn && beyond_steam())
        steam_restore_notice();
    if (state_.navigation.route != Route::SignIn) {
        // Leaving the sign-in screen: a sign-in field still open closes, and nothing secret stays.
        if (state_.editor.active && state_.editor.opener_focus_key.rfind("signin-", 0) == 0) state_.editor.active = false;
        if (state_.editor.active && state_.editor.opener_focus_key.rfind("guard-", 0) == 0) state_.editor.active = false;
        clear_sign_in_secrets();
    }
    rebuild_view();
}

bool Simulation::UpdateAppDetails(GameId id, std::optional<std::uint64_t> download_bytes,
                                  std::optional<std::uint64_t> install_bytes,
                                  std::optional<bool> windows_installable) {
    if (!live()) return false;
    const auto game = std::find_if(state_.games.begin(), state_.games.end(),
                                   [id](const Game& candidate) { return candidate.id == id; });
    if (game == state_.games.end()) return false;
    game->download_bytes = download_bytes;
    game->install_bytes = install_bytes;
    game->windows_installable = windows_installable;
    apply_live_volume_usage();
    rebuild_view();
    return true;
}

void Simulation::UpdateStoreDetails(GameId id, std::string description, std::vector<std::string> genres,
                                    std::string controller_support) {
    if (!live()) return;
    auto& facts = store_facts_[id];
    facts = {std::move(description), std::move(genres), std::move(controller_support)};
    for (auto& game : state_.games) {
        if (game.id != id) continue;
        if (!facts.description.empty()) game.description = facts.description;
        game.genres = facts.genres;
        game.controller_support = facts.controller_support;
    }
    rebuild_view();
}

bool Simulation::UpdateLibrary(LibraryUpdate update) {
    if (!live()) return false;

    std::set<GameId> identifiers;
    for (const auto& summary : update.games) {
        if (!summary.id.valid() || trimmed(summary.title).empty()
            || !identifiers.insert(summary.id).second
            || (summary.last_played_unix_seconds
                && *summary.last_played_unix_seconds
                    > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            || (summary.added_at_unix_seconds
                && *summary.added_at_unix_seconds
                    > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))) {
            return false;
        }
        if (summary.id.store != update.source) return false;
    }
    if (update.retrieved_at && *update.retrieved_at < 0) return false;

    if (!update.loading && update.error.empty()) {
        library_catalog_received_ = true;
        catalog_sources_.insert(update.source);
        const auto prior_visible = visible_games();
        const GameId prior_id = state_.navigation.selected_game_id;
        const auto prior_detail_id = state_.navigation.detail_id;
        const std::string prior_focus = state_.navigation.focus_key;
        const bool prior_overlay = state_.editor.active || !state_.navigation.modals.empty();
        const bool prior_catalog_empty = state_.games.empty();
        int prior_index = state_.navigation.selected_index;
        const auto prior_selected = std::find_if(prior_visible.begin(), prior_visible.end(), [prior_id](const Game* game) {
            return game->id == prior_id;
        });
        if (prior_selected != prior_visible.end())
            prior_index = static_cast<int>(std::distance(prior_visible.begin(), prior_selected));

        std::vector<Game> projected;
        projected.reserve(update.games.size());
        for (auto& summary : update.games) {
            Game game;
            game.id = summary.id;
            game.title = std::move(summary.title);
            game.folded_title = FoldLibraryText(game.title);
            game.description = std::move(summary.description);
            game.source = std::string(Traits(update.source).name);
            game.artwork_source = std::move(summary.artwork_source);
            game.playtime_minutes = summary.playtime_minutes;
            if (summary.last_played_unix_seconds)
                game.last_played = static_cast<std::int64_t>(*summary.last_played_unix_seconds);
            if (summary.added_at_unix_seconds)
                game.added_at = static_cast<std::int64_t>(*summary.added_at_unix_seconds);
            game.demo = summary.demo;
            if (const auto facts = store_facts_.find(game.id); facts != store_facts_.end()) {
                if (!facts->second.description.empty()) game.description = facts->second.description;
                game.genres = facts->second.genres;
                game.controller_support = facts->second.controller_support;
            }
            projected.push_back(std::move(game));
        }
        // Each store's games replace only that store's: the list stays grouped by store, in store order.
        std::vector<Game> merged;
        merged.reserve(state_.games.size() + projected.size());
        for (auto& game : state_.games)
            if (game.id.store < update.source) merged.push_back(std::move(game));
        for (auto& game : projected) merged.push_back(std::move(game));
        for (auto& game : state_.games)
            if (game.id.store > update.source) merged.push_back(std::move(game));
        state_.games = std::move(merged);

        const auto current_visible = visible_games();
        const auto retained = std::find_if(current_visible.begin(), current_visible.end(), [prior_id](const Game* game) {
            return game->id == prior_id;
        });
        bool identity_retained = false;
        if (retained != current_visible.end()) {
            identity_retained = true;
            state_.navigation.selected_game_id = prior_id;
            state_.navigation.selected_index = static_cast<int>(std::distance(current_visible.begin(), retained));
        } else if (prior_catalog_empty && prior_id.valid() && find_game(state_, prior_id)) {
            identity_retained = true;
            state_.navigation.selected_game_id = prior_id;
            state_.navigation.selected_index = 0;
        } else if (!current_visible.empty()) {
            state_.navigation.selected_index = std::clamp(prior_index, 0, static_cast<int>(current_visible.size()) - 1);
            state_.navigation.selected_game_id = current_visible[static_cast<std::size_t>(state_.navigation.selected_index)]->id;
            close_game_modals();
            state_.editor.active = false;
        } else {
            state_.navigation.selected_index = 0;
            state_.navigation.selected_game_id = {};
            close_game_modals();
            state_.editor.active = false;
        }

        if (state_.navigation.route == Route::Details && prior_detail_id) {
            const auto retained_detail = std::find_if(state_.games.begin(), state_.games.end(), [prior_detail_id](const Game& game) {
                return game.id == *prior_detail_id;
            });
            if (retained_detail != state_.games.end()) {
                state_.navigation.detail_id = *prior_detail_id;
                state_.navigation.selected_game_id = *prior_detail_id;
                const auto visible_detail = std::find_if(current_visible.begin(), current_visible.end(), [prior_detail_id](const Game* game) {
                    return game->id == *prior_detail_id;
                });
                if (visible_detail != current_visible.end())
                    state_.navigation.selected_index = static_cast<int>(std::distance(current_visible.begin(), visible_detail));
                identity_retained = true;
            } else {
                state_.navigation.detail_id = !state_.navigation.selected_game_id.valid()
                    ? std::nullopt : std::optional<GameId>{state_.navigation.selected_game_id};
                identity_retained = false;
            }
        }

        if (state_.navigation.route == Route::Details && !state_.navigation.selected_game_id.valid()) {
            state_.navigation.route = state_.navigation.detail_origin == Route::Home
                ? Route::Home : Route::Library;
            state_.navigation.detail_id.reset();
        }

        if (prior_overlay && identity_retained) {
            state_.navigation.focus_key = prior_focus;
        } else if (state_.navigation.route == Route::Library && state_.navigation.library_rail_focused) {
            state_.navigation.focus_key = prior_focus;
        } else if (state_.navigation.route == Route::Home || state_.navigation.route == Route::Library) {
            state_.navigation.focus_key = !state_.navigation.selected_game_id.valid()
                ? (state_.library.query.empty() ? "library-empty" : "clear-search")
                : game_key(state_.navigation.selected_game_id);
        } else if (state_.navigation.route == Route::Details) {
            if (identity_retained) state_.navigation.focus_key = prior_focus;
            else {
                details_primary_pending_ = live();
                state_.navigation.focus_key = live() ? "details-favorite"
                                                     : (is_installed(state_, state_.navigation.selected_game_id)
                                                            ? "details-play" : "details-install");
            }
        } else {
            state_.navigation.focus_key = prior_focus;
        }
        // An update dialog that stayed open keeps the focus; the route's new focus is where it returns to.
        if (prior_overlay && !identity_retained && !state_.navigation.modals.empty()) {
            state_.navigation.modals.front().opener_focus_key = state_.navigation.focus_key;
            state_.navigation.focus_key = prior_focus;
        }

        const auto projected_shelves = shelves(state_);
        for (std::size_t shelf_index = 0; shelf_index < projected_shelves.size(); ++shelf_index) {
            const auto& shelf = projected_shelves[shelf_index];
            const auto selected = std::find(shelf.game_ids.begin(), shelf.game_ids.end(), state_.navigation.selected_game_id);
            if (selected == shelf.game_ids.end()) continue;
            state_.navigation.home_shelf = static_cast<int>(shelf_index);
            state_.navigation.home_shelf_focus[shelf.key] = static_cast<int>(std::distance(shelf.game_ids.begin(), selected));
            break;
        }
    }

    if (update.loading) loading_sources_.insert(update.source);
    else loading_sources_.erase(update.source);
    state_.library_loading = !loading_sources_.empty();
    state_.library_syncing = state_.library_loading;
    if (update.source == Store::kSteam) state_.library_offline = update.stale;
    auto& account = state_.account(update.source);
    account.cache_available = update.cache_available;
    account.error = std::move(update.error);
    if (update.retrieved_at) account.last_sync = *update.retrieved_at;
    // Launches that stopped before this store's library arrived are told now, by name. A library that
    // could not load still tells them, without the name.
    if (!update.loading) {
        for (auto stop = pending_play_stops_.begin(); stop != pending_play_stops_.end();) {
            if (stop->first.store != update.source) { ++stop; continue; }
            const auto [id, reason] = *stop;
            stop = pending_play_stops_.erase(stop);
            tell_play_stopped(id, reason);
        }
    }
    // A game just added from the Import sheet is listed now: its page opens (from Startup too).
    if (!pending_imports_.empty()) settle_pending_imports();
    // A store without an account brought games while nothing is signed in (or Steam's saved sign-in
    // failed to restore): they open on Home.
    const bool waiting_route = state_.navigation.route == Route::Startup ||
                               (state_.navigation.route == Route::SignIn && restore_failed_sign_in_);
    if (waiting_route && !state_.startup_loading && !state_.root_access_missing &&
        state_.account().authentication != AuthState::Restoring && beyond_steam()) {
        const bool failed_restore = restore_failed_sign_in_;
        restore_failed_sign_in_ = false;
        state_.navigation.route = Route::Home;
        state_.navigation.focus_key = state_.games.empty() ? "home-library" : game_key(state_.games.front().id);
        if (failed_restore) steam_restore_notice();
    }
    captured_organization_.selected_game_id = state_.navigation.selected_game_id;
    rebuild_view();
    return true;
}

playden::OrganizationSnapshot Simulation::ExportOrganization() const {
    playden::OrganizationSnapshot result;
    result.favorites = state_.library.favorites;
    result.hidden = state_.library.hidden;
    result.query = state_.library.query;
    result.scope = state_.library.scope;
    result.sort = state_.library.sort;
    result.filters = state_.library.filters;
    result.selected_game_id = state_.navigation.selected_game_id;
    result.collections.reserve(state_.library.collections.size());
    for (const auto& value : state_.library.collections)
        result.collections.push_back({value.id, value.name, value.pinned, value.members});
    for (const auto& [id, rating] : state_.library.ratings)
        result.ratings[id] = static_cast<playden::CompatibilityRating>(rating);
    result.notes = state_.library.notes;
    return result;
}

playden::Preferences Simulation::ExportPreferences() const {
    return {state_.reduced_motion, state_.title_store_suffix, state_.cloud_saves_auto, state_.sign_in_password,
            state_.downloads_slow_network, state_.stay_signed_in, state_.beta_updates, state_.content_language};
}

void Simulation::apply_organization_snapshot(const playden::OrganizationSnapshot& snapshot) {
    const auto prior_selected_id = state_.navigation.selected_game_id;
    state_.library.favorites = snapshot.favorites;
    state_.library.hidden = snapshot.hidden;
    state_.library.query = snapshot.query;
    state_.library.scope = snapshot.scope;
    state_.library.sort = snapshot.sort;
    state_.library.filters = snapshot.filters;
    state_.library.collections.clear();
    for (const auto& value : snapshot.collections)
        state_.library.collections.push_back({value.id, value.name, value.pinned, value.members});
    state_.library.ratings.clear();
    for (const auto& [id, rating] : snapshot.ratings)
        state_.library.ratings[id] = static_cast<Compatibility>(rating);
    state_.library.notes = snapshot.notes;
    state_.navigation.selected_game_id = snapshot.selected_game_id;
    reconcile_applied_organization_selection(prior_selected_id);
}

void Simulation::reconcile_applied_organization_selection(GameId prior_selected_id) {
    if (state_.games.empty()) {
        if (!library_catalog_received_) return;
        state_.navigation.selected_game_id = {};
        state_.navigation.selected_index = 0;
        if (state_.navigation.route == Route::Home) state_.navigation.focus_key = "home-library";
        else if (state_.navigation.route == Route::Library && !state_.navigation.library_rail_focused)
            state_.navigation.focus_key = state_.library.query.empty() ? "library-empty" : "clear-search";
        return;
    }
    if (state_.navigation.route == Route::Details &&
        state_.navigation.detail_id == state_.navigation.selected_game_id &&
        find_game(state_, state_.navigation.selected_game_id)) return;

    const auto visible = visible_games();
    const auto visible_candidate = [&](GameId wanted) -> std::vector<const Game*>::const_iterator {
        auto found = std::find_if(visible.begin(), visible.end(), [wanted](const Game* game) {
            return game->id == wanted;
        });
        if (found != visible.end()) return found;
        found = std::find_if(visible.begin(), visible.end(), [prior_selected_id](const Game* game) {
            return game->id == prior_selected_id;
        });
        if (found != visible.end() || visible.empty()) return found;
        return visible.begin() + std::clamp(state_.navigation.selected_index, 0,
                                             static_cast<int>(visible.size()) - 1);
    };
    const auto library_item = visible_candidate(state_.navigation.selected_game_id);
    const GameId library_candidate = library_item == visible.end() ? GameId{} : (*library_item)->id;
    if (library_item != visible.end()) {
        const int index = static_cast<int>(std::distance(visible.begin(), library_item));
        state_.navigation.selected_index = index;
        state_.navigation.library_grid_column = index % 6;
        state_.navigation.scroll_offsets["library-grid"] = index / 6;
    }

    GameId home_candidate;
    std::string home_more_key;  // the focused "More" cell, kept across library updates
    const auto projected = shelves(state_);
    auto locate_home = [&projected](GameId id) -> std::optional<std::pair<int, int>> {
        for (std::size_t shelf_index = 0; shelf_index < projected.size(); ++shelf_index) {
            const auto& ids = projected[shelf_index].game_ids;
            const auto item = std::find(ids.begin(), ids.end(), id);
            if (item != ids.end())
                return std::pair<int, int>{static_cast<int>(shelf_index),
                                           static_cast<int>(std::distance(ids.begin(), item))};
        }
        return std::nullopt;
    };
    auto home_location = locate_home(state_.navigation.selected_game_id);
    if (!home_location) home_location = locate_home(prior_selected_id);
    if (!home_location && !projected.empty()) {
        const int shelf_index = std::clamp(state_.navigation.home_shelf, 0,
                                           static_cast<int>(projected.size()) - 1);
        const auto& shelf = projected[static_cast<std::size_t>(shelf_index)];
        const int item_index = std::clamp(state_.navigation.home_shelf_focus[shelf.key], 0,
                                          playden::homebrew::shelf_cells(shelf) - 1);
        home_location = std::pair<int, int>{shelf_index, item_index};
    }
    if (home_location) {
        const auto [shelf_index, item_index] = *home_location;
        const auto& shelf = projected[static_cast<std::size_t>(shelf_index)];
        if (item_index == static_cast<int>(shelf.game_ids.size())) home_more_key = playden::homebrew::shelf_more_key(shelf);
        else home_candidate = shelf.game_ids[static_cast<std::size_t>(item_index)];
        state_.navigation.home_shelf = shelf_index;
        state_.navigation.home_shelf_focus[shelf.key] = item_index;
        state_.navigation.scroll_offsets["home"] = shelf_index;
    } else {
        home_candidate = library_candidate;
    }

    if (state_.navigation.route == Route::Library) {
        state_.navigation.selected_game_id = library_candidate;
        if (!state_.navigation.library_rail_focused)
            state_.navigation.focus_key = !library_candidate.valid()
                ? (state_.library.query.empty() ? "library-empty" : "clear-search")
                : game_key(library_candidate);
    } else if (state_.navigation.route == Route::Home ||
               state_.navigation.route == Route::Settings ||
               state_.navigation.route == Route::Downloads) {
        state_.navigation.selected_game_id = home_candidate;
        if (state_.navigation.route == Route::Home)
            state_.navigation.focus_key = !home_more_key.empty() ? home_more_key
                                          : !home_candidate.valid() ? "home-library" : game_key(home_candidate);
    }
}

PersistenceLoadToken Simulation::BeginOrganizationLoad(std::uint64_t account) {
    if (!live() || account == 0) return {};
    if (organization_account_ != account) {
        organization_account_ = account;
        organization_revision_ = 0;
        organization_saved_revision_ = 0;
        organization_journal_.clear();
        organization_journal_bytes_ = 0;
        captured_organization_ = ExportOrganization();
        persisted_organization_ = {};
        organization_save_suppressed_ = false;
        organization_journal_full_ = false;
    } else if (OrganizationDirty() && organization_journal_.empty()) {
        const auto bytes = snapshot_size(persisted_organization_) + snapshot_size(captured_organization_);
        if (bytes <= kOrganizationJournalBytes) {
            organization_journal_.push_back({persisted_organization_, captured_organization_, false, false, bytes});
            organization_journal_bytes_ = bytes;
        } else {
            organization_journal_full_ = true;
        }
    }
    organization_load_token_ = {++next_persistence_load_token_, organization_revision_};
    organization_load_pending_ = true;
    return organization_load_token_;
}

bool Simulation::ApplyOrganization(std::uint64_t account, PersistenceLoadToken token,
                                   std::optional<playden::OrganizationSnapshot> loaded) {
    if (!live() || account == 0 || account != organization_account_ ||
        !organization_load_pending_ || !(token == organization_load_token_)) return false;
    auto loaded_base = loaded.value_or(playden::OrganizationSnapshot{});
    auto merged = loaded_base;
    const bool had_local_edits = !organization_journal_.empty();
    const bool had_local_selection = std::any_of(
        organization_journal_.begin(), organization_journal_.end(), [](const auto& entry) {
            return entry.reset || entry.selected ||
                   entry.before.selected_game_id != entry.after.selected_game_id;
        });
    for (const auto& entry : organization_journal_)
        replay_organization_delta(merged, entry.before, entry.after, entry.reset, entry.selected);
    if (!had_local_selection && !loaded_base.selected_game_id.valid())
        merged.selected_game_id = captured_organization_.selected_game_id;
    if (!snapshot_within_model_bounds(merged)) {
        organization_load_pending_ = false;
        organization_load_failed_ = true;
        update_persistence_notice();
        rebuild_view();
        return false;
    }
    apply_organization_snapshot(merged);
    merged.selected_game_id = state_.navigation.selected_game_id;
    captured_organization_ = merged;
    persisted_organization_ = std::move(loaded_base);
    organization_journal_.clear();
    organization_journal_bytes_ = 0;
    organization_load_pending_ = false;
    organization_load_failed_ = false;
    organization_journal_full_ = false;
    if (had_local_edits) {
        if (organization_revision_ == 0) organization_revision_ = 1;
        organization_saved_revision_ = 0;
    } else {
        organization_saved_revision_ = organization_revision_;
    }
    update_persistence_notice();
    rebuild_view();
    return true;
}

bool Simulation::FailOrganizationLoad(std::uint64_t account, PersistenceLoadToken token,
                                      playden::StorageError error) {
    if (!live() || error == playden::StorageError::kNone || account != organization_account_ ||
        !organization_load_pending_ || !(token == organization_load_token_)) return false;
    organization_load_pending_ = false;
    organization_load_failed_ = true;
    update_persistence_notice();
    rebuild_view();
    return true;
}

void Simulation::OrganizationCleared(std::uint64_t account) {
    if (!live() || account == 0 || account != organization_account_) return;
    organization_load_token_ = {};
    organization_load_pending_ = false;
    organization_load_failed_ = false;
    organization_save_failed_ = false;
    organization_journal_full_ = false;
    organization_journal_.clear();
    organization_journal_bytes_ = 0;
    organization_save_suppressed_ = true;
    organization_saved_revision_ = organization_revision_;
    captured_organization_ = ExportOrganization();
    persisted_organization_ = {};
    update_persistence_notice();
    rebuild_view();
}

PersistenceLoadToken Simulation::BeginPreferencesLoad() {
    if (!live()) return {};
    preferences_load_token_ = {++next_persistence_load_token_, preferences_revision_};
    preferences_load_pending_ = true;
    preferences_load_may_apply_ = !PreferencesDirty();
    return preferences_load_token_;
}

bool Simulation::ApplyPreferences(PersistenceLoadToken token,
                                  const playden::PreferencesLoadResult& loaded) {
    if (!live() || !preferences_load_pending_ || !(token == preferences_load_token_)) return false;
    preferences_load_pending_ = false;
    preferences_load_failed_ = loaded.status == playden::PreferencesLoadStatus::kDefaultCorrupt ||
                               loaded.status == playden::PreferencesLoadStatus::kDefaultReadError;
    if (preferences_load_may_apply_ && preferences_revision_ == token.baseline_revision) {
        state_.reduced_motion = loaded.preferences.reduce_motion;
        state_.title_store_suffix = loaded.preferences.title_store_suffix;
        state_.cloud_saves_auto = loaded.preferences.cloud_saves_auto;
        state_.sign_in_password = loaded.preferences.steam_sign_in_password;
        state_.beta_updates = loaded.preferences.beta_updates;
        state_.downloads_slow_network = loaded.preferences.downloads_slow_network;
        state_.stay_signed_in = loaded.preferences.steam_stay_signed_in;
        state_.content_language = loaded.preferences.content_language;
        captured_preferences_ = loaded.preferences;
        preferences_saved_revision_ = preferences_revision_;
    }
    update_persistence_notice();
    rebuild_view();
    return true;
}

bool Simulation::OrganizationDirty() const noexcept {
    return !organization_save_suppressed_ && organization_revision_ > organization_saved_revision_;
}

bool Simulation::PreferencesDirty() const noexcept {
    return preferences_revision_ > preferences_saved_revision_;
}

bool Simulation::OrganizationReadyForSave() const noexcept {
    return live() && organization_account_ != 0 && OrganizationDirty() &&
           !organization_load_pending_ && !organization_load_failed_ && !organization_journal_full_;
}

void Simulation::OrganizationSaveResult(std::uint64_t account, std::uint64_t revision,
                                        playden::StorageError error) {
    if (!live() || account != organization_account_ || revision > organization_revision_) return;
    if (error == playden::StorageError::kNone) {
        organization_saved_revision_ = std::max(organization_saved_revision_, revision);
        if (revision == organization_revision_) {
            organization_save_failed_ = false;
            persisted_organization_ = captured_organization_;
        }
    } else {
        organization_save_failed_ = true;
    }
    update_persistence_notice();
    rebuild_view();
}

void Simulation::PreferencesSaveResult(std::uint64_t revision, playden::StorageError error) {
    if (!live() || revision > preferences_revision_) return;
    if (error == playden::StorageError::kNone) {
        preferences_saved_revision_ = std::max(preferences_saved_revision_, revision);
        if (revision == preferences_revision_) preferences_save_failed_ = false;
    } else {
        preferences_save_failed_ = true;
    }
    update_persistence_notice();
    rebuild_view();
}

bool Simulation::TakePersistenceRetryRequest() {
    const bool requested = persistence_retry_requested_;
    persistence_retry_requested_ = false;
    return requested;
}

void Simulation::update_persistence_notice() {
    const bool failed = organization_load_failed_ || organization_save_failed_ ||
                        organization_journal_full_ || preferences_load_failed_ ||
                        preferences_save_failed_;
    state_.persistence_available = !failed;
    state_.notices.erase(std::remove_if(state_.notices.begin(), state_.notices.end(),
                                        [](const Notice& notice) { return notice.id == "persistence"; }),
                         state_.notices.end());
    if (failed)
        state_.notices.push_back({"persistence", "Saved UI data is unavailable", "retry-save", true, 0});
}

bool Simulation::record_live_persistence() {
    auto after_organization = ExportOrganization();
    auto after_preferences = ExportPreferences();
    const bool organization_changed = force_organization_reset_ || force_organization_selection_ ||
                                      !(after_organization == captured_organization_);
    const bool preferences_changed = force_preferences_reset_ ||
                                     (after_preferences.reduce_motion != captured_preferences_.reduce_motion ||
                                      after_preferences.title_store_suffix != captured_preferences_.title_store_suffix ||
                                      after_preferences.cloud_saves_auto != captured_preferences_.cloud_saves_auto ||
                                      after_preferences.steam_sign_in_password != captured_preferences_.steam_sign_in_password ||
                                      after_preferences.downloads_slow_network != captured_preferences_.downloads_slow_network ||
                                      after_preferences.beta_updates != captured_preferences_.beta_updates ||
                                      after_preferences.content_language != captured_preferences_.content_language ||
                                      after_preferences.steam_stay_signed_in != captured_preferences_.steam_stay_signed_in);

    if (organization_changed && !snapshot_within_model_bounds(after_organization)) {
        apply_organization_snapshot(captured_organization_);
        state_.notices.erase(std::remove_if(state_.notices.begin(), state_.notices.end(),
                                            [](const Notice& notice) { return notice.id == "persistence-validation"; }),
                             state_.notices.end());
        if (live()) show_alert("This change is too large to save", "Playden undid it, so your library is as it was.");
        else state_.notices.push_back({"persistence-validation", "This change is too large to save", "", true, 0});
        force_organization_reset_ = false;
        return false;
    }
    if (organization_changed && organization_account_ != 0) {
        if (organization_load_pending_ || organization_load_failed_) {
            const auto bytes = snapshot_size(captured_organization_) + snapshot_size(after_organization);
            if (organization_journal_.size() >= kOrganizationJournalEntries ||
                bytes > kOrganizationJournalBytes - std::min(organization_journal_bytes_, kOrganizationJournalBytes)) {
                apply_organization_snapshot(captured_organization_);
                state_.reduced_motion = captured_preferences_.reduce_motion;
                state_.title_store_suffix = captured_preferences_.title_store_suffix;
                state_.cloud_saves_auto = captured_preferences_.cloud_saves_auto;
                state_.sign_in_password = captured_preferences_.steam_sign_in_password;
                state_.beta_updates = captured_preferences_.beta_updates;
                state_.downloads_slow_network = captured_preferences_.downloads_slow_network;
                state_.stay_signed_in = captured_preferences_.steam_stay_signed_in;
                state_.content_language = captured_preferences_.content_language;
                organization_journal_full_ = true;
                force_organization_reset_ = false;
                force_organization_selection_ = false;
                force_preferences_reset_ = false;
                update_persistence_notice();
                return false;
            }
            organization_journal_.push_back({captured_organization_, after_organization,
                                             force_organization_reset_, force_organization_selection_, bytes});
            organization_journal_bytes_ += bytes;
        }
        captured_organization_ = std::move(after_organization);
        ++organization_revision_;
        organization_save_suppressed_ = false;
    }
    if (preferences_changed) {
        captured_preferences_ = after_preferences;
        ++preferences_revision_;
    }
    force_organization_reset_ = false;
    force_organization_selection_ = false;
    force_preferences_reset_ = false;
    update_persistence_notice();
    return true;
}

void Simulation::load_fixtures() {
    if (live()) return;
    const auto source = read_file(fixture_directory_ / "catalog.json");
    const auto root = json::parse(source);
    if (!root || !root->object()) {
        last_error_ = "Could not read fixtures/ui-comparison/catalog.json";
        state_.persistence_available = false;
        return;
    }
    state_.clock_ticks = static_cast<std::int64_t>(uint_value(root->get("clock"), kFixtureClock));
    if (const auto* values = root->get("games"); values && values->array()) {
        for (const auto& item : *values->array()) {
            Game game;
            game.id = steam_value(item.get("id"));
            game.title = string_value(item.get("title"));
            game.description = string_value(item.get("description"), "Description unavailable");
            game.source = string_value(item.get("source"));
            if (const auto* art = item.get("art")) {
                game.cover_key = string_value(art->get("cover"));
                game.hero_key = string_value(art->get("hero"));
                game.logo_key = string_value(art->get("logo"));
                game.art_failed = string_value(art->get("state")) == "failed";
            }
            if (const auto* genres = item.get("genres"); genres && genres->array())
                for (const auto& genre : *genres->array()) game.genres.push_back(string_value(&genre));
            game.controller_support = string_value(item.get("controller_support"), "unknown");
            game.download_bytes = optional_uint(item.get("download_bytes"));
            game.install_bytes = optional_uint(item.get("installed_bytes"));
            if (const auto added = optional_uint(item.get("added_at"))) game.added_at = static_cast<std::int64_t>(*added);
            if (const auto played = optional_uint(item.get("last_played"))) game.last_played = static_cast<std::int64_t>(*played);
            game.playtime_minutes = optional_uint(item.get("playtime_minutes"));
            game.demo = bool_value(item.get("demo"));
            state_.games.push_back(std::move(game));
        }
    }
    if (const auto* account = root->get("account")) {
        state_.account().id = string_value(account->get("id"));
        state_.account().display_name = string_value(account->get("display_name"));
        state_.account().authentication = AuthState::SignedIn;
        state_.account().last_sync = static_cast<std::int64_t>(uint_value(account->get("last_sync")));
        state_.account().cache_available = bool_value(account->get("cache_available"));
    }
    if (const auto* organization = root->get("organization")) {
        if (const auto* favorites = organization->get("favorites"); favorites && favorites->array())
            for (const auto& id : *favorites->array()) state_.library.favorites.insert(steam_value(&id));
        if (const auto* hidden = organization->get("hidden"); hidden && hidden->array())
            for (const auto& id : *hidden->array()) state_.library.hidden.insert(steam_value(&id));
        if (const auto* collections = organization->get("collections"); collections && collections->array()) {
            for (const auto& item : *collections->array()) {
                Collection collection;
                collection.id = string_value(item.get("id"));
                collection.name = string_value(item.get("name"));
                collection.pinned = bool_value(item.get("pinned"));
                if (const auto* ids = item.get("game_ids"); ids && ids->array())
                    for (const auto& id : *ids->array()) collection.members.insert(steam_value(&id));
                state_.library.collections.push_back(std::move(collection));
                ++next_collection_serial_;
            }
        }
        state_.library.query = string_value(organization->get("query"));
        state_.library.scope = string_value(organization->get("scope"), "all");
        state_.library.sort = string_value(organization->get("sort"), "name");
    }
    if (const auto* values = root->get("installations"); values && values->array()) {
        for (const auto& item : *values->array()) {
            Installation installation;
            installation.game_id = steam_value(item.get("game_id"));
            installation.volume_id = string_value(item.get("volume_id"));
            installation.installed_bytes = uint_value(item.get("installed_bytes"));
            installation.available = bool_value(item.get("available"), true);
            installation.runtime_ready = bool_value(item.get("runtime_ready"), false);
            installation.saves_present = bool_value(item.get("saves_preserved"), true);
            state_.installations.push_back(std::move(installation));
        }
    }
    if (const auto* values = root->get("volumes"); values && values->array()) {
        for (const auto& item : *values->array()) {
            Volume volume;
            volume.id = string_value(item.get("id"));
            volume.label = string_value(item.get("label"));
            volume.capacity_bytes = uint_value(item.get("capacity_bytes"));
            volume.used_bytes = uint_value(item.get("used_bytes"));
            volume.reserved_bytes = uint_value(item.get("reserved_bytes"));
            volume.available = bool_value(item.get("available"), true);
            volume.writable = bool_value(item.get("writable"), true);
            volume.capacity_known = bool_value(item.get("capacity_known"), true);
            volume.base_used_bytes = volume.used_bytes;
            volume.base_reserved_bytes = volume.reserved_bytes;
            state_.volumes.push_back(std::move(volume));
        }
        for (auto& volume : state_.volumes) {
            std::uint64_t fixture_install_bytes = 0;
            for (const auto& installation : state_.installations) if (installation.volume_id == volume.id)
                fixture_install_bytes += installation.installed_bytes;
            volume.base_used_bytes = volume.used_bytes > fixture_install_bytes ? volume.used_bytes - fixture_install_bytes : 0;
        }
    }
    if (const auto* values = root->get("jobs"); values && values->array()) {
        for (const auto& item : *values->array()) {
            Job job;
            job.id = string_value(item.get("id"));
            job.game_id = steam_value(item.get("game_id"));
            job.volume_id = string_value(item.get("volume_id"));
            job.kind = string_value(item.get("kind"), "install");
            job.queue_order = static_cast<int>(uint_value(item.get("order")));
            job.stage = parse_job_stage(string_value(item.get("stage")));
            job.state = parse_job_state(string_value(item.get("state")));
            job.completed_bytes = uint_value(item.get("completed_bytes"));
            job.total_bytes = uint_value(item.get("total_bytes"));
            job.bytes_per_second = optional_uint(item.get("rate_bytes_per_second"));
            if (const auto eta = optional_uint(item.get("eta_seconds"))) job.eta_seconds = static_cast<int>(*eta);
            job.failure = string_value(item.get("failure"));
            job.retry_stage = parse_job_stage(string_value(item.get("retry_target")));
            state_.jobs.push_back(std::move(job));
        }
    }
    for (const auto& job : state_.jobs) {
        if (job.id.rfind("job-", 0) != 0) continue;
        std::uint64_t serial = 0;
        const auto text = job.id.substr(4);
        const auto result = std::from_chars(text.data(), text.data() + text.size(), serial);
        if (result.ec == std::errc{} && result.ptr == text.data() + text.size()) next_job_serial_ = std::max(next_job_serial_, serial + 1);
    }
}

void Simulation::load_profile() {
    if (live()) return;
    const auto source = read_file(profile_path_);
    if (source.empty()) return;
    const auto root = json::parse(source);
    if (!root || !root->object()) {
        state_.persistence_available = false;
        state_.notices.push_back({"persistence", "Saved UI state is corrupt", "retry-save", true, 0});
        return;
    }
    const auto saved_account = string_value(root->get("account_id"));
    const auto saved_auth = string_value(root->get("authentication"));
    const auto restored_auth = parse_auth_state(saved_auth);
    if (saved_account != state_.account().id) {
        state_.library = {};
        state_.game_settings.clear();
    }
    state_.account().id = saved_account;
    state_.account().display_name = saved_account.empty() ? std::string{} : string_value(root->get("account_display_name"), "Connected fixture account");
    state_.reduced_motion = bool_value(root->get("reduced_motion"));
    if (const auto saved = string_value(root->get("content_language")); playden::steam::IsContentLanguage(saved))
        state_.content_language = saved;
    if (saved_auth == "signed-out" || saved_auth == "cancelled" || (saved_auth == "waiting" && saved_account.empty())) {
        state_.account().authentication = AuthState::SignedOut; state_.account().id.clear(); state_.account().display_name.clear();
        state_.account().cache_available = false; state_.account().challenge_expiry = 0; state_.navigation.route = Route::Startup; state_.navigation.focus_key = "startup-connect";
    } else if (saved_auth == "syncing") {
        state_.account().authentication = AuthState::Failed; state_.account().cache_available = false; state_.account().challenge_expiry = 0;
        state_.account().error = "Account sync was interrupted"; state_.navigation.route = Route::SignIn; state_.navigation.focus_key = "qr-retry";
    } else if (saved_auth == "waiting" || saved_auth == "expired") {
        state_.account().authentication = AuthState::Expired; state_.account().cache_available = false; state_.account().challenge_expiry = 0;
        state_.account().error = "Challenge expired during restart"; state_.navigation.route = Route::SignIn; state_.navigation.focus_key = "qr-refresh";
    } else if (saved_auth == "failed") {
        state_.account().authentication = AuthState::Failed; state_.account().cache_available = false; state_.account().challenge_expiry = 0;
        state_.account().error = "Account connection needs retry"; state_.navigation.route = Route::SignIn; state_.navigation.focus_key = "qr-retry";
    } else if (!saved_auth.empty()) {
        state_.account().authentication = restored_auth; state_.account().cache_available = restored_auth == AuthState::SignedIn || restored_auth == AuthState::Offline;
    }
    state_.account().last_sync = static_cast<std::int64_t>(uint_value(root->get("account_last_sync"), static_cast<std::uint64_t>(state_.account().last_sync)));
    state_.library.query = string_value(root->get("query"), state_.library.query);
    state_.library.scope = string_value(root->get("scope"), state_.library.scope);
    if (const auto* favorites = root->get("favorites"); favorites && favorites->array()) {
        state_.library.favorites.clear();
        for (const auto& id : *favorites->array()) state_.library.favorites.insert(steam_value(&id));
    }
    if (const auto* hidden = root->get("hidden"); hidden && hidden->array()) {
        state_.library.hidden.clear();
        for (const auto& id : *hidden->array()) state_.library.hidden.insert(steam_value(&id));
    }
    state_.library.sort = string_value(root->get("sort"), state_.library.sort);
    if (const auto* filters = root->get("filters"); filters && filters->array()) {
        state_.library.filters.clear();
        for (const auto& filter : *filters->array()) state_.library.filters.insert(string_value(&filter));
    }
    if (const auto* collections = root->get("collections"); collections && collections->array()) {
        state_.library.collections.clear();
        for (const auto& item : *collections->array()) {
            Collection collection;
            collection.id = string_value(item.get("id")); collection.name = string_value(item.get("name")); collection.pinned = bool_value(item.get("pinned"));
            if (const auto* members = item.get("members"); members && members->array()) for (const auto& id : *members->array()) collection.members.insert(steam_value(&id));
            state_.library.collections.push_back(std::move(collection));
            ++next_collection_serial_;
        }
    }
    if (const auto* ratings = root->get("ratings"); ratings && ratings->array()) {
        state_.library.ratings.clear();
        for (const auto& item : *ratings->array()) state_.library.ratings[steam_value(item.get("id"))] = static_cast<Compatibility>(uint_value(item.get("value")));
    }
    if (const auto* notes = root->get("notes"); notes && notes->array()) {
        state_.library.notes.clear();
        for (const auto& item : *notes->array()) state_.library.notes[steam_value(item.get("id"))] = string_value(item.get("value"));
    }
    // Keep revision numbers lexical: the fixture reader's doubles cannot represent every uint64.
    playden::steam::json::Limits settings_limits;
    settings_limits.max_bytes = 16u * 1024u * 1024u;
    settings_limits.max_values = 262144;
    const auto exact_profile = playden::steam::json::Parse(source, settings_limits);
    const auto* settings = exact_profile ? exact_profile->Find("game_settings") : nullptr;
    if (settings && settings->is_array()) {
        state_.game_settings.clear();
        for (const auto& item : settings->array()) {
            const auto* identity = item.Find("game_id");
            const auto app = identity ? identity->AsUint64() : std::nullopt;
            const auto* value = item.Find("settings");
            if (!app || *app == 0 || *app > std::numeric_limits<std::uint32_t>::max() || !value || !value->is_object()) continue;
            rt::GameSettingsRecord record;
            if (const auto* preset = value->Find("preset")) {
                if (!preset->is_string()) continue;
                record.preset_id = preset->text();
            }
            if (const auto* revision = value->Find("revision")) {
                const auto parsed = revision->AsUint64();
                if (!parsed) continue;
                record.revision = *parsed;
            }
            if (const auto* overrides = value->Find("overrides"); overrides && overrides->is_object())
                for (const auto& [id, text] : overrides->object())
                    if (text.is_string() && rt::FindSetting(id)) record.overrides[id] = text.text();
            state_.game_settings[GameId::Steam(static_cast<std::uint32_t>(*app))] = std::move(record);
        }
    }
    if (const auto* jobs = root->get("jobs"); jobs && jobs->array()) {
        state_.jobs.clear();
        for (const auto& item : *jobs->array()) {
            Job job;
            job.id = string_value(item.get("id")); job.kind = string_value(item.get("kind"), "install"); job.game_id = steam_value(item.get("game_id")); job.queue_order = static_cast<int>(uint_value(item.get("order")));
            job.stage = static_cast<JobStage>(uint_value(item.get("stage"))); job.state = static_cast<JobState>(uint_value(item.get("state")));
            job.completed_bytes = uint_value(item.get("completed")); job.total_bytes = uint_value(item.get("total")); job.volume_id = string_value(item.get("volume"));
            job.failure = string_value(item.get("failure")); job.retry_stage = static_cast<JobStage>(uint_value(item.get("retry"))); job.stage_ticks = static_cast<int>(uint_value(item.get("stage_ticks")));
            job.bytes_per_second = optional_uint(item.get("rate"));
            if (const auto eta = optional_uint(item.get("eta"))) job.eta_seconds = static_cast<int>(*eta);
            job.reserved_bytes = uint_value(item.get("reserved"));
            job.reservation_held = bool_value(item.get("reservation_held"));
            job.completion_applied = bool_value(item.get("completion_applied"));
            if (job.state == JobState::Downloading || job.state == JobState::Verifying || job.state == JobState::Pausing) job.state = JobState::Interrupted;
            state_.jobs.push_back(std::move(job));
        }
    }
    next_job_serial_ = std::max(next_job_serial_, uint_value(root->get("next_job_serial"), next_job_serial_));
    if (const auto* installations = root->get("installations"); installations && installations->array()) {
        state_.installations.clear();
        for (const auto& item : *installations->array()) state_.installations.push_back({steam_value(item.get("game_id")), string_value(item.get("volume")), uint_value(item.get("bytes")), bool_value(item.get("available"), true), bool_value(item.get("runtime")), bool_value(item.get("saves"), true)});
    }
    if (const auto* volumes = root->get("volumes"); volumes && volumes->array()) {
        for (const auto& item : *volumes->array()) {
            const auto id = string_value(item.get("id"));
            const auto found = std::find_if(state_.volumes.begin(), state_.volumes.end(), [&id](const Volume& value) { return value.id == id; });
            if (found == state_.volumes.end()) continue;
            found->used_bytes = uint_value(item.get("used"), found->used_bytes);
            found->reserved_bytes = uint_value(item.get("reserved"), found->reserved_bytes);
            found->available = bool_value(item.get("available"), found->available);
        }
    }
    if (state_.account().authentication == AuthState::SignedOut) {
        state_.library = {};
        state_.game_settings.clear();
    }
    reconcile_storage(state_);
}

bool Simulation::save_profile() {
    settings_save_failed_ = true;
    if (live() && !settings_load_failed_ && !game_settings_file_.empty() && state_.game_settings != saved_game_settings_) {
        std::string reason;
        if (rt::SaveGameSettingsFile(game_settings_file_.string(), state_.game_settings, &reason)) saved_game_settings_ = state_.game_settings;
        else push_notice({"settings-save-failed", "Game settings were not saved", "", false, kLiveNoticeSeconds, true, reason});
    }
    if (live()) {
        settings_save_failed_ = settings_load_failed_ || game_settings_file_.empty() || state_.game_settings != saved_game_settings_;
        const bool saved = record_live_persistence();
        return saved && !settings_save_failed_;
    }
    if (!state_.persistence_available) {
        state_.notices.push_back({"save-failed", "Changes cannot be saved", "retry-save", true, 0});
        return false;
    }
    std::error_code error;
    if (!profile_path_.parent_path().empty()) std::filesystem::create_directories(profile_path_.parent_path(), error);
    std::string output = "{\"account_id\":\"" + json::escape(state_.account().id) + "\",\"account_display_name\":\"" + json::escape(state_.account().display_name)
        + "\",\"authentication\":\"" + auth_name(state_.account().authentication) + "\",\"account_last_sync\":" + std::to_string(state_.account().last_sync)
        + ",\"next_job_serial\":" + std::to_string(next_job_serial_) + ",\"reduced_motion\":"
        + (state_.reduced_motion ? "true" : "false") + ",\"content_language\":\"" + json::escape(state_.content_language)
        + "\",\"query\":\"" + json::escape(state_.library.query)
        + "\",\"scope\":\"" + json::escape(state_.library.scope) + "\",\"favorites\":[";
    bool first = true;
    for (const auto id : state_.library.favorites) {
        if (!first) output += ',';
        first = false;
        output += app_text(id);
    }
    output += "],\"hidden\":[";
    first = true;
    for (const auto id : state_.library.hidden) { if (!first) output += ','; first = false; output += app_text(id); }
    output += "],\"sort\":\"" + json::escape(state_.library.sort) + "\",\"filters\":[";
    first = true;
    for (const auto& filter : state_.library.filters) { if (!first) output += ','; first = false; output += '"' + json::escape(filter) + '"'; }
    output += "],\"collections\":[";
    first = true;
    for (const auto& collection : state_.library.collections) {
        if (!first) output += ','; first = false;
        output += "{\"id\":\"" + json::escape(collection.id) + "\",\"name\":\"" + json::escape(collection.name)
            + "\",\"pinned\":" + (collection.pinned ? "true" : "false") + ",\"members\":[";
        bool first_member = true;
        for (const auto id : collection.members) { if (!first_member) output += ','; first_member = false; output += app_text(id); }
        output += "]}";
    }
    output += "],\"ratings\":[";
    first = true;
    for (const auto& [id, rating] : state_.library.ratings) { if (!first) output += ','; first = false; output += "{\"id\":" + app_text(id) + ",\"value\":" + std::to_string(static_cast<int>(rating)) + '}'; }
    output += "],\"notes\":[";
    first = true;
    for (const auto& [id, note] : state_.library.notes) { if (!first) output += ','; first = false; output += "{\"id\":" + app_text(id) + ",\"value\":\"" + json::escape(note) + "\"}"; }
    output += "],\"game_settings\":[";
    first = true;
    for (const auto& [id, record] : state_.game_settings) {
        if (!first) output += ','; first = false;
        output += "{\"game_id\":" + app_text(id) + ",\"settings\":" + rt::SerializeGameSettingsRecord(record) + "}";
    }
    output += "],\"jobs\":[";
    first = true;
    for (const auto& job : state_.jobs) {
        if (!first) output += ','; first = false;
        output += "{\"id\":\"" + json::escape(job.id) + "\",\"kind\":\"" + json::escape(job.kind) + "\",\"game_id\":" + app_text(job.game_id)
            + ",\"order\":" + std::to_string(job.queue_order) + ",\"stage\":" + std::to_string(static_cast<int>(job.stage))
            + ",\"state\":" + std::to_string(static_cast<int>(job.state)) + ",\"completed\":" + std::to_string(job.completed_bytes)
            + ",\"total\":" + std::to_string(job.total_bytes) + ",\"volume\":\"" + json::escape(job.volume_id)
            + "\",\"failure\":\"" + json::escape(job.failure) + "\",\"retry\":" + std::to_string(static_cast<int>(job.retry_stage))
            + ",\"rate\":" + (job.bytes_per_second ? std::to_string(*job.bytes_per_second) : "null")
            + ",\"eta\":" + (job.eta_seconds ? std::to_string(*job.eta_seconds) : "null")
            + ",\"stage_ticks\":" + std::to_string(job.stage_ticks) + ",\"reserved\":" + std::to_string(job.reserved_bytes)
            + ",\"reservation_held\":" + (job.reservation_held ? "true" : "false")
            + ",\"completion_applied\":" + (job.completion_applied ? "true" : "false") + '}';
    }
    output += "],\"installations\":[";
    first = true;
    for (const auto& installation : state_.installations) {
        if (!first) output += ','; first = false;
        output += "{\"game_id\":" + app_text(installation.game_id) + ",\"volume\":\"" + json::escape(installation.volume_id)
            + "\",\"bytes\":" + std::to_string(installation.installed_bytes) + ",\"available\":" + (installation.available ? "true" : "false")
            + ",\"runtime\":" + (installation.runtime_ready ? "true" : "false") + ",\"saves\":" + (installation.saves_present ? "true" : "false") + '}';
    }
    output += "],\"volumes\":[";
    first = true;
    for (const auto& volume : state_.volumes) {
        if (!first) output += ','; first = false;
        output += "{\"id\":\"" + json::escape(volume.id) + "\",\"used\":" + std::to_string(volume.used_bytes)
            + ",\"reserved\":" + std::to_string(volume.reserved_bytes) + ",\"available\":" + (volume.available ? "true" : "false") + '}';
    }
    output += "]}";
    std::FILE* file = std::fopen(profile_path_.string().c_str(), "wb");
    if (!file) {
        state_.persistence_available = false;
        state_.notices.push_back({"save-failed", "Changes cannot be saved", "retry-save", true, 0});
        return false;
    }
    const bool written = std::fwrite(output.data(), 1, output.size(), file) == output.size();
    const bool closed = std::fclose(file) == 0;
    settings_save_failed_ = !(written && closed);
    return written && closed;
}

bool Simulation::scenario(const std::string& name) {
    if (live()) return false;
    if (name == "default") {
        state_.account().authentication = AuthState::SignedIn;
        state_.navigation.route = Route::Home;
        state_.navigation.selected_index = 0;
        state_.navigation.selected_game_id = state_.games.empty() ? GameId{} : state_.games.front().id;
        state_.navigation.focus_key = state_.games.empty() ? "home-library" : game_key(state_.navigation.selected_game_id);
        const auto projected = playden::homebrew::shelves(state_);
        for (std::size_t index = 0; index < projected.size(); ++index) {
            const auto game = std::find(projected[index].game_ids.begin(), projected[index].game_ids.end(), state_.navigation.selected_game_id);
            if (game != projected[index].game_ids.end()) {
                state_.navigation.home_shelf = static_cast<int>(index);
                state_.navigation.home_shelf_focus[projected[index].key] = static_cast<int>(std::distance(projected[index].game_ids.begin(), game));
                break;
            }
        }
    } else if (name == "first-run" || name == "signed-out") {
        state_.account().authentication = AuthState::SignedOut;
        state_.account().id.clear();
        state_.account().display_name.clear();
        state_.account().cache_available = false;
        state_.navigation.route = Route::Startup;
        state_.navigation.focus_key = "startup-connect";
    } else if (name == "saves" || name == "saves-sync-all") {
        // The Saves tab with one game in each state, as in the mockup.
        state_.account().authentication = AuthState::SignedIn;
        state_.navigation.route = Route::Saves;
        state_.navigation.focus_key = "saves-filter-0";
        state_.clock_ticks = 1'800'000'000;
        state_.installations.clear();
        state_.cloud.clear();
        state_.kept_bottles.clear();
        const std::int64_t now = state_.clock_ticks;
        struct Seed { CloudStatus status; bool first; std::size_t files; std::uint64_t bytes; std::int64_t last; bool after_play; };
        const std::vector<Seed> seeds = {
            {CloudStatus::Conflict, false, 2, 84'000, now - 2 * 86'400, false},
            {CloudStatus::Conflict, true, 2, 900'000, 0, false},
            {CloudStatus::Failed, false, 5, 2'400'000, now - 86'400, false},
            {CloudStatus::UpToDate, false, 4, 312'000, now - 4 * 3600, true},
            {CloudStatus::Synced, false, 4, 3'100'000, now - 2 * 3600, false},
            {CloudStatus::UpToDate, false, 1, 12'000, now - 30 * 3600, false},
            {CloudStatus::Unsupported, false, 3, 640'000, 0, false},
            {CloudStatus::UpToDate, false, 6, 420'000, now - 21 * 86'400, false},
        };
        for (std::size_t index = 0; index < seeds.size() && index < state_.games.size(); ++index) {
            const auto id = state_.games[index].id;
            const auto& seed = seeds[index];
            if (index + 1 < seeds.size()) state_.installations.push_back({id, "internal", 4'000'000'000, true, true, true});
            else state_.kept_bottles.push_back({id, state_.games[index].title, now - 21 * 86'400, 1'900'000'000});
            LiveCloudGame cloud{id, seed.status, seed.status == CloudStatus::Failed ? "Steam is not reachable. Try again." : "",
                                seed.files, seed.status == CloudStatus::Unsupported ? 0 : seed.files + (seed.first ? 1 : 0),
                                seed.bytes, seed.status == CloudStatus::Unsupported ? 0 : seed.bytes + 7'000,
                                now - 3 * 3600, now - 5 * 3600};
            cloud.first_sync = seed.first;
            cloud.last_sync = seed.last;
            cloud.after_play = seed.after_play;
            if (seed.status == CloudStatus::Unsupported) { cloud.local_bytes = 0; state_.save_sizes[id] = seed.bytes; }
            state_.cloud[id] = cloud;
        }
        if (name == "saves-sync-all" && state_.games.size() >= 6) {
            state_.saves_job = {SavesJobKind::SyncAll, 4, 7, state_.games[5].id, false};
            auto& moving = state_.cloud[state_.games[5].id];
            moving.status = CloudStatus::Syncing;
            moving.downloads = 1;
            moving.moved_bytes = 4'000;
            moving.total_bytes = 12'000;
            state_.cloud[state_.games[6 % state_.games.size()].id].queued = true;
            state_.navigation.focus_key = "saves-sync-all";
        }
    } else if (name.rfind("queue-", 0) == 0) {
        // Queue in the states of its mockup: each workflow empty, one game, or one running and three waiting,
        // with or without finished entries.
        static const std::map<std::string, std::array<int, 3>> shapes = {
            {"queue-preparation-empty", {4, 0, 0}}, {"queue-downloads-empty", {0, 4, 0}}, {"queue-one-preparation", {4, 1, 0}},
            {"queue-one-download", {1, 4, 0}}, {"queue-both-full", {4, 4, 0}}, {"queue-all-empty", {0, 0, 0}},
            {"queue-active-history", {4, 4, 1}}, {"queue-completed-only", {0, 0, 1}}};
        const auto shape = shapes.find(name);
        if (shape == shapes.end() || state_.games.size() < 13) return false;
        const auto [downloads, preparations, history] = shape->second;
        state_.account().authentication = AuthState::SignedIn;
        state_.navigation.route = Route::Downloads;
        state_.navigation.modals.clear();
        state_.clock_ticks = 1'800'000'000;
        const std::int64_t now = state_.clock_ticks;
        state_.jobs.clear();
        state_.runtime_games.clear();
        state_.preparation_queue = {};
        state_.queue_expanded.clear();
        const auto job = [&](std::size_t game, int order, JobState job_state, std::uint64_t total, std::uint64_t done) {
            Job item;
            item.id = "queue-" + ToKey(state_.games[game].id);
            item.game_id = state_.games[game].id;
            item.queue_order = order;
            item.state = job_state;
            item.total_bytes = total;
            item.completed_bytes = done;
            item.volume_id = "internal";
            if (job_state == JobState::Downloading) { item.bytes_per_second = 18'200'000; item.eta_seconds = 152; }
            if (job_state == JobState::Downloaded) { item.finished_at = now - 3600; item.completion_applied = true; }
            state_.jobs.push_back(item);
        };
        for (int index = 0; index < downloads; ++index)
            job(static_cast<std::size_t>(index), index, index == 0 ? JobState::Downloading : JobState::Queued,
                7'600'000'000 + static_cast<std::uint64_t>(index) * 1'100'000'000, index == 0 ? 4'900'000'000 : 0);
        if (history) {
            job(8, 20, JobState::Downloaded, 549'000'000, 549'000'000);
            job(9, 21, JobState::Downloaded, 367'000'000, 367'000'000);
            job(10, 22, JobState::Downloaded, 1'700'000'000, 1'700'000'000);
        }
        auto& queue = state_.preparation_queue;
        for (int index = 0; index < preparations; ++index) {
            const auto id = state_.games[static_cast<std::size_t>(4 + index)].id;
            LiveRuntimeGame runtime;
            runtime.game_id = id;
            if (index == 0) {
                queue.active = id;
                runtime.status = "Preparing";
                runtime.detail = "Creating game files…";
            } else {
                queue.waiting.push_back(id);
                runtime.status = "Not prepared";
                runtime.queued = true;
            }
            state_.runtime_games[id] = runtime;
        }
        if (history)
            queue.completed = {{state_.games[11].id, now - 20 * 60}, {state_.games[12].id, now - 35 * 60}};
        state_.navigation.focus_key = preparations > 1 || (preparations && !downloads) ? "queue-preparation"
                                    : downloads || history ? "queue-downloads" : "";
        if (state_.navigation.focus_key.empty() && !state_.volumes.empty())
            state_.navigation.focus_key = "storage-" + state_.volumes.front().id;
    } else if (name == "startup-loading") {
        state_.startup_loading = true; state_.navigation.route = Route::Startup; state_.navigation.focus_key = "startup-loading";
    } else if (name == "persistence-corrupt") {
        state_.persistence_available = false;
        state_.navigation.route = Route::Startup;
        state_.notices.push_back({"persistence", "Saved state is unavailable", "retry-save", true, 0});
    } else if (name == "qr-waiting") {
        state_.navigation.route = Route::SignIn;
        state_.account().authentication = AuthState::Waiting;
        state_.account().challenge_expiry = state_.clock_ticks + 30;
        ++challenge_generation_;
        state_.navigation.focus_key = "qr-cancel";
    } else if (name == "qr-expired") {
        state_.navigation.route = Route::SignIn;
        state_.account().authentication = AuthState::Expired;
        state_.navigation.focus_key = "qr-refresh";
    } else if (name == "qr-failure" || name == "qr-denied") {
        state_.navigation.route = Route::SignIn;
        state_.account().authentication = AuthState::Failed;
        state_.account().error = name == "qr-denied" ? "Challenge denied" : "Network unavailable";
        state_.navigation.focus_key = "qr-retry";
    } else if (name == "qr-approved") {
        if (state_.account().authentication != AuthState::Cancelled && challenge_generation_ != cancelled_challenge_generation_) {
            if (state_.account().id.empty()) { state_.account().id = "fixture-connected"; state_.account().display_name = "Connected fixture account"; }
            if (state_.sign_in.step != SignInStep::Qr) state_.sign_in_password = true;
            clear_sign_in_secrets();
            state_.sign_in.step = SignInStep::Qr;
            state_.account().authentication = AuthState::Syncing;
            state_.navigation.route = Route::SignIn;
            state_.navigation.focus_key = "sync-status";
        }
    } else if (name == "late-approval") {
        if (state_.account().authentication != AuthState::Cancelled) state_.account().authentication = AuthState::Syncing;
    } else if (name == "syncing") {
        state_.account().authentication = AuthState::Syncing;
        state_.library_syncing = true;
        state_.navigation.route = Route::Home;
    } else if (name == "empty") {
        state_.navigation.route = Route::Library;
        state_.navigation.selected_game_id = {};
        state_.navigation.focus_key = "library-empty";
        state_.library.query.clear();
        state_.library.filters.insert("empty-fixture");
    } else if (name == "offline") {
        state_.navigation.route = Route::Library;
        state_.account().authentication = AuthState::Offline;
        state_.library_offline = true;
        state_.notices.push_back({"offline", "Offline — showing cached library", "retry-sync", true, 0});
    } else if (name == "no-results") {
        state_.navigation.route = Route::Library;
        state_.library.query = "zzz-no-matches";
        state_.navigation.selected_game_id = {};
        state_.navigation.focus_key = "clear-search";
    } else if (name == "delayed-art" || name == "failed-art") {
        state_.navigation.route = Route::Library;
        state_.navigation.selected_game_id = name == "delayed-art" ? GameId::Steam(900007) : GameId::Steam(900008);
        state_.navigation.focus_key = game_key(state_.navigation.selected_game_id);
        if (auto* game = const_cast<Game*>(find_game(state_, state_.navigation.selected_game_id))) {
            game->art_pending = name == "delayed-art";
            game->art_failed = name == "failed-art";
        }
    } else if (name == "install-ready" || name == "unknown-size" || name == "insufficient-space") {
        state_.navigation.route = Route::Details;
        state_.navigation.detail_id = name == "unknown-size" ? GameId::Steam(900006) : GameId::Steam(900012);
        state_.navigation.selected_game_id = *state_.navigation.detail_id;
        state_.navigation.focus_key = "details-install";
        if (name == "insufficient-space") {
            selected_volume_id_ = "internal";
            if (auto* volume = find_volume(state_, "internal")) volume->used_bytes = volume->capacity_bytes - 1'073'741'824;
        }
        open_modal({"install", "Install game", "details-install", {"install-confirm", "install-estimate-retry", "storage-picker", "cancel"}, 0, false});
    } else if (name == "storage-disconnect") {
        state_.navigation.route = Route::Downloads;
        if (auto* volume = find_volume(state_, "external")) volume->available = false;
        if (auto* job = find_job(state_, "job-1")) { job->state = JobState::Interrupted; job->failure = "Storage disconnected"; }
        state_.notices.push_back({"storage", "USB fixture library disconnected", "storage-retry", true, 0});
    } else if (name == "paused" || name == "download-failed" || name == "verification-failed" || name == "restart-recovery") {
        state_.navigation.route = Route::Downloads;
        if (auto* job = find_job(state_, "job-1")) {
            if (name == "paused") job->state = JobState::Paused;
            else if (name == "restart-recovery") job->state = JobState::Interrupted;
            else { job->state = JobState::Failed; job->failure = name == "verification-failed" ? "Verification failed" : "Download failed"; }
            job->stage = name == "verification-failed" ? JobStage::Verify : JobStage::Download;
            job->retry_stage = job->stage;
        }
        state_.navigation.focus_key = "job-job-1";
    } else if (name == "save-failure") {
        state_.persistence_available = false;
        state_.notices.push_back({"save-failed", "Changes cannot be saved", "retry-save", true, 0});
    } else if (name == "runtime-unavailable") {
        state_.runtime = RuntimeState::Unavailable;
        state_.navigation.route = Route::Details;
        state_.navigation.detail_id = GameId::Steam(900001);
        state_.navigation.selected_game_id = GameId::Steam(900001);
        if (auto* installation = find_installation(state_, GameId::Steam(900001))) installation->runtime_ready = false;
        state_.navigation.focus_key = "details-play";
    } else if (name == "launch-failure" || name == "crash" || name == "stop-failure") {
        state_.navigation.route = Route::Session;
        state_.session_game_id = GameId::Steam(900001);
        state_.session = name == "crash" ? SessionState::Crashed : SessionState::Failed;
        state_.session_error = name == "launch-failure" ? "Simulated launch failed" : (name == "crash" ? "Game exited unexpectedly" : "Game did not stop");
        state_.navigation.focus_key = name == "crash" ? "session-logs" : "session-retry";
    } else if (name == "controller-disconnected") {
        state_.controller_connected = false;
        state_.notices.push_back({"controller", "Controller disconnected", "controller-test", true, 0});
    } else if (name == "controller-reconnected") {
        state_.controller_connected = true;
        state_.notices.erase(std::remove_if(state_.notices.begin(), state_.notices.end(),
                                            [](const Notice& notice) { return notice.id == "controller"; }), state_.notices.end());
    } else if (name == "reduced-motion") {
        state_.reduced_motion = true;
        save_profile();
    } else if (name == "demos") {
        // A demo next to the full game, as Steam lists one after "Download demo".
        state_.navigation.route = Route::Library;
        Game demo;
        demo.id = GameId::Steam(900025);
        demo.title = "Tiny Astronaut Demo";
        demo.description = "A synthetic demo. UI fixture only; this is not an owned game.";
        demo.source = "Steam";
        demo.controller_support = "full";
        demo.download_bytes = demo.install_bytes = 536870912;
        demo.demo = true;
        if (const auto* full = find_game(state_, GameId::Steam(900021))) {
            demo.cover_key = full->cover_key, demo.hero_key = full->hero_key, demo.logo_key = full->logo_key;
            demo.genres = full->genres;
        }
        if (!find_game(state_, demo.id)) state_.games.push_back(std::move(demo));
        reconcile_library_selection(state_);
    } else if (name == "stress") {
        const auto source = read_file(fixture_directory_.parent_path() / "library.json");
        const auto root = json::parse(source);
        if (!root || !root->array()) { last_error_ = "Could not read 1,000-game stress fixture"; return false; }
        state_.games.clear();
        state_.games.reserve(root->array()->size());
        for (const auto& item : *root->array()) {
            Game game;
            game.id = steam_value(item.get("id"));
            game.title = string_value(item.get("title"));
            game.description = string_value(item.get("description"));
            game.source = "Steam / Windows";
            game.cover_key = string_value(item.get("artwork_source"));
            state_.games.push_back(std::move(game));
        }
        state_.library = {};
        state_.navigation.route = Route::Library;
        state_.navigation.selected_index = 0;
        state_.navigation.selected_game_id = state_.games.empty() ? GameId{} : state_.games.front().id;
        state_.navigation.focus_key = game_key(state_.navigation.selected_game_id);
    } else if (name == "art-ready") {
        if (auto found = std::find_if(state_.games.begin(), state_.games.end(), [](const Game& game) { return game.id == GameId::Steam(900007); }); found != state_.games.end()) {
            found->art_pending = false;
            found->art_failed = false;
        }
    } else if (name == "sync-complete") {
        state_.library_syncing = false;
        state_.account().authentication = AuthState::SignedIn;
        state_.navigation.route = Route::Home;
    } else if (name == "import-empty" || name == "import-candidates" || name == "import-programs" || name == "import-choice" ||
               name == "import-storage" || name == "import-drop-choice" || name == "import-drop-storage" ||
               name == "import-details" || name == "import-copying" || name == "import-library") {
        // The Import sheet over Settings › Library: nothing found yet, or the fixture's folders; then
        // a folder's programs, the wizard's two steps (a USB drive's game, a drop-folder game), and
        // the imported game's page (in place, then copying).
        state_.account().authentication = AuthState::SignedIn;
        state_.import_scanning = false;
        state_.import_candidates.clear();
        pending_imports_.clear();
        state_.navigation.modals.clear();
        if (name != "import-empty") {
            const auto id = fixture_imported_game();
            if (!find_game(state_, id)) {
                Game game;
                game.id = id;
                game.title = "Night Sky";
                game.source = "Imported";
                game.install_bytes = 412'000'000;
                game.description = "A quiet night drive under the stars.";
                state_.games.push_back(std::move(game));
            }
            if (!external_game(id)) state_.external_games.push_back({id, "usb0", true, ""});
            std::erase_if(state_.jobs, [id](const Job& job) { return job.game_id == id; });
            state_.import_candidates = fixture_import_candidates();
            state_.import_candidates.front().imported = id;
        }
        if (name == "import-details" || name == "import-copying") {
            const auto id = fixture_imported_game();
            state_.navigation.route = Route::Library;
            open_game_page(id);
            if (name == "import-copying") {
                start_import_transfer(id, ImportRequestKind::CopyIntoPlayden, {});
                auto& job = state_.jobs.back();
                job.state = JobState::Downloading;
                job.completed_bytes = job.total_bytes * 2 / 5;
            }
            rebuild_view();
            return true;
        }
        if (name == "import-library") {
            // The Library's Imported scope: imported games' tiles carry their title, no store art.
            state_.library.scope = "imported";
            state_.navigation.route = Route::Library;
            state_.navigation.library_rail_focused = false;
            reconcile_library_selection(state_);
            rebuild_view();
            return true;
        }
        state_.navigation.route = Route::Settings;
        state_.navigation.settings_rail_focused = false;
        state_.navigation.tabs_focused = false;
        state_.navigation.focus_key = "settings-import";
        rebuild_view();
        open_import_sheet();
        if (name == "import-storage" || name == "import-drop-storage") {
            // The console's drives as the wizard's mockup has them: the game's USB drive among them.
            state_.volumes = {{"internal", "Console storage", 673'900'000'000, 562'400'000'000, 0},
                              {"m2", "M.2 SSD", 1'000'200'000'000, 668'100'000'000, 0},
                              {"usb0", "USB drive", 1'000'200'000'000, 372'900'000'000, 0}};
            for (auto& volume : state_.volumes) volume.base_used_bytes = volume.used_bytes;
            selected_volume_id_ = "internal";
        }
        if (name != "import-empty" && name != "import-candidates") {
            const bool drop = name == "import-drop-choice" || name == "import-drop-storage";
            rebuild_view();
            action(drop ? "focus:import-candidate-internal:playden-import/Hollow Knight"
                        : "focus:import-candidate-usb0:playden/import/Old School Rally");
            action("cross");
            // A folder with one clear program skips its list (ClearGameProgram).
            if (name != "import-programs" && state_.navigation.modals.back().id == "import-programs") action("cross");
            if (name == "import-storage" || name == "import-drop-storage") {
                action("focus:import-copy");
                action("cross");
            }
        }
    } else {
        last_error_ = "Unknown scenario: " + name;
        return false;
    }
    rebuild_view();
    return true;
}

std::vector<const Game*> Simulation::visible_games() const {
    if (state_.editor.active && state_.editor.purpose == "Search") {
        auto preview = state_;
        preview.library.query = state_.editor.value;
        const auto filtered = filtered_games(preview);
        std::vector<const Game*> result;
        result.reserve(filtered.size());
        for (const auto* game : filtered) {
            const auto index = static_cast<std::size_t>(game - preview.games.data());
            result.push_back(&state_.games[index]);
        }
        return result;
    }
    return filtered_games(state_);
}

void Simulation::rebuild_view() {
    // The Library left for another tab: the order "More" showed ends with that visit.
    if (state_.navigation.route != Route::Library && state_.navigation.route != Route::Details)
        state_.library.view_sort.clear();
    const bool has_overlay = state_.editor.active || !state_.navigation.modals.empty();
    if (has_overlay) {
        // Only the flag is set aside: a copy of the editor would keep a password in memory.
        const bool editor_active = state_.editor.active;
        const auto modals = state_.navigation.modals;
        const auto focus = state_.navigation.focus_key;
        const int selected_index = state_.navigation.selected_index;
        const GameId selected_game_id = state_.navigation.selected_game_id;
        state_.editor.active = false;
        // Settings under a sheet shows the section of the row that opened it.
        if (state_.navigation.route == Route::Settings && !modals.empty() &&
            modals.front().opener_focus_key.rfind("settings-", 0) == 0)
            state_.navigation.focus_key = modals.front().opener_focus_key;
        state_.navigation.modals.clear();
        rebuilding_overlay_ = true;
        rebuild_view();
        rebuilding_overlay_ = false;
        route_view_ = view_;
        state_.editor.active = editor_active;
        state_.navigation.modals = modals;
        state_.navigation.focus_key = focus;
        state_.navigation.selected_index = selected_index;
        state_.navigation.selected_game_id = selected_game_id;
    }
    view_ = {};
    if (state_.navigation.route != Route::Downloads) queue_shown_ = false;
    view_.surface = route_name(state_.navigation.route);
    view_.show_simulation_label = state_.simulation_mode;
    if (state_.editor.active) view_.surface = "keyboard";
    else if (!state_.navigation.modals.empty()) view_.surface = state_.navigation.modals.back().id;

    auto row = [this](std::string key, RowKind kind, std::string title, std::string action) -> ViewRow& {
        view_.rows.push_back({});
        auto& result = view_.rows.back();
        result.key = std::move(key); result.kind = kind; result.title = std::move(title); result.action = std::move(action);
        return result;
    };

    if (state_.editor.active) {
        view_.title = state_.editor.purpose;
        view_.subtitle = state_.editor.secret && !state_.editor.revealed ? mask_secret(state_.editor.value) : state_.editor.value;
        if (state_.editor.purpose == "Search") view_.subtitle += " · " + std::to_string(visible_games().size()) + " results";
        row("keyboard-done", RowKind::Action, "Done", "editor-done");
        row("keyboard-cancel", RowKind::Action, "Cancel", "editor-cancel");
        view_.legends = {"Cross Select", "Options Done", "Square Backspace", "L1/R1 Move caret", "Triangle Clear", "Circle Cancel"};
    } else if (!state_.navigation.modals.empty()) {
        const auto& modal = state_.navigation.modals.back();
        view_.title = modal.title;
        if (modal.id == "logs") {
            view_.subtitle = state_.log_context;
            if (state_.log_state == LogState::Loading) row("logs-loading", RowKind::Status, "Loading fixture logs…", "").enabled = false;
            else if (state_.log_state == LogState::Failed) {
                auto& status = row("logs-error", RowKind::Status, "Logs unavailable", ""); status.subtitle = live() ? state_.log_context : "Simulated log service failure"; status.enabled = false;
            } else if (state_.log_lines.empty()) {
                row("logs-empty", RowKind::Status, "No log entries", "").enabled = false;
            } else {
                constexpr int visible_lines = 8;
                const int begin = std::clamp(state_.log_offset, 0, std::max(0, static_cast<int>(state_.log_lines.size()) - visible_lines));
                const int end = std::min(static_cast<int>(state_.log_lines.size()), begin + visible_lines);
                for (int index = begin; index < end; ++index) {
                    auto& line = row("log-line-" + std::to_string(index), RowKind::Text, state_.log_lines[static_cast<std::size_t>(index)], "");
                    line.enabled = false;
                }
            }
        }
        const GameId settings_game = state_.navigation.selected_game_id;
        const bool settings_modal = modal.id == "game-settings" || modal.id == "preset-picker" || modal.id == "button-mapping" ||
                                    modal.id == "controller-tuning" ||
                                    modal.id == "game-maintenance" || !modal.setting_id.empty();
        rt::EffectiveSettings settings_values;
        const bool settings_ready = settings_modal && EffectiveSettingsFor(settings_game, &settings_values);
        const auto settings_record = state_.game_settings.find(settings_game);
        const bool settings_custom = settings_record != state_.game_settings.end() && !settings_record->second.overrides.empty();
        // A picker marks the value the game would have without the player's choice.
        rt::EffectiveValue inherited;
        if (settings_ready && !modal.setting_id.empty()) {
            auto overrides = settings_record == state_.game_settings.end() ? std::map<std::string, std::string>{} : settings_record->second.overrides;
            overrides.erase(modal.setting_id);
            rt::EffectiveSettings base;
            rt::SettingsError ignored;
            if (rt::BuildEffectiveSettings(RecipeFor(settings_game), preset_for(settings_game), overrides, kit_for(settings_game),
                                           &base, &ignored))
                inherited = base.values.at(modal.setting_id);
        }
        if (modal.id == "game-settings" || modal.id == "game-maintenance") {
            if (const auto* game = find_game(state_, settings_game)) view_.subtitle = game->title;
        } else if (!modal.setting_id.empty()) {
            if (const auto* definition = rt::FindSetting(modal.setting_id)) {
                // A launch option by the name the picker shows, not its id.
                const auto launch_label = [this](GameId game, const std::string& id) {
                    for (const auto& entry : RecipeFor(game).launch_entries)
                        if (entry.id == id) return entry.label.empty() ? entry.executable : entry.label;
                    return id;
                };
                view_.subtitle = definition->effect;
                view_.subtitle += settings_custom || inherited.source != rt::ValueSource::kPlayden
                    ? std::string(" ") + source_label(inherited.source) + ": " + (modal.setting_id == "launch_option" ? launch_label(settings_game, inherited.value) : choice_label(*definition, rt::ChosenValue(inherited))) + "."
                    : "";
                const auto closed = !game_closed(settings_game) && definition->apply == rt::ApplyMode::kGameClosed;
                // A game that was never prepared gets its bottle from these values on the next Play.
                const auto runtime_game = state_.runtime_games.find(settings_game);
                const bool prepared = runtime_game != state_.runtime_games.end() && runtime_game->second.prepared;
                view_.subtitle += definition->apply == rt::ApplyMode::kPreparesBottle && live()
                    ? (prepared ? " Applies after Maintenance > Recreate bottle." : " Applies when Play prepares the game.")
                    : closed ? " Quit the game first." : " Applies on next launch.";
            }
        } else if (modal.id == "preset-picker") {
            view_.subtitle = "A preset sets several settings at once. Choosing one clears your changes for this game.";
        }
        if (modal.id == "install" && live()) {
            // Shown when the game does not fit as is: say why, and where it would go.
            const auto id = state_.navigation.detail_id.value_or(state_.navigation.selected_game_id);
            const auto assessment = assess_install(state_, id, install_target(), steam::kInstallSpaceMargin);
            const auto* volume = find_volume(state_, install_target());
            const auto* fallback = find_volume(state_, selected_volume_id_);
            view_.subtitle = "To " + (volume ? volume->label : std::string("unavailable storage")) +
                (install_volume_id_.empty() ? "" : " (this game only)") + ". ";
            view_.subtitle += assessment.free_bytes ? format_bytes(*assessment.free_bytes) + " free. " : "";
            view_.subtitle += assessment.installed_bytes
                ? "Needs about " + format_bytes(*assessment.installed_bytes) + ". "
                : "Its size is checked when the download starts. ";
            if (assessment.readiness == InstallReadiness::Insufficient) view_.subtitle += "Not enough space: choose another drive or free up space.";
            else if (assessment.readiness == InstallReadiness::Unavailable) view_.subtitle += "This drive is disconnected.";
            else if (assessment.readiness == InstallReadiness::ReadOnly) view_.subtitle += "This drive is read-only.";
            else if (assessment.readiness == InstallReadiness::UnknownCapacity)
                view_.subtitle += free_space_supported_ ? "Free space on this drive can't be read: choose another drive."
                                                        : "Free space can't be checked on this console.";
            if (!install_volume_id_.empty() && fallback)
                view_.subtitle += (view_.subtitle.back() == ' ' ? "" : " ") + std::string("Other games still install to ") +
                                  fallback->label + ".";
        }
        if (modal.id == "storage" && live() && picking_install_volume_) {
            const auto* fallback = find_volume(state_, selected_volume_id_);
            view_.subtitle = "For this game only." + (fallback ? " Other games still install to " + fallback->label + "." : std::string());
        }
        if (modal.id == "installed-games") {
            view_.subtitle = !installed_message_.empty() ? installed_message_
                : "Choose games to uninstall. Their saves and settings are kept unless you choose to delete them.";
            std::uint64_t bytes = 0;
            for (const auto& installation : state_.installations)
                if (installed_selected_.contains(installation.game_id)) bytes += installation.installed_bytes;
            view_.footer = installed_selected_.empty() ? std::string("Select games to uninstall")
                : std::to_string(installed_selected_.size()) + " selected · " + format_bytes(bytes);
        }
        if (modal.id == "import-games")
            view_.subtitle = state_.import_candidates.empty() ? kImportEmptyText
                : "Games found in playden/import on your drives and in /data/playden-import. Choose one to add it.";
        const auto* import_programs = modal.id == "import-programs" ? import_candidate(import_programs_key_) : nullptr;
        if (import_programs)
            view_.subtitle = import_programs->ascii_ok ? "Choose the program that starts the game." : kImportCopyOnlyText;
        if (modal.id == "steam-programs")
            view_.subtitle = "Steam lists more than one program for this game. Choose the one that starts it; you can change it later in Game settings.";
        // The import wizard: what it imports, where from, and how big it is.
        const bool import_wizard = modal.id == "import-choice" || modal.id == "import-storage";
        const auto import_wizard_subject = import_wizard ? import_subject() : ImportSubject{};
        const bool import_from_drop = import_wizard_subject.volume == kImportDropVolume;
        if (import_wizard) {
            view_.eyebrow = "Import games · " + import_wizard_subject.title;
            const auto size = format_bytes(import_wizard_subject.bytes);
            if (modal.id == "import-choice") {
                const auto* chosen = import_program_chosen_ ? import_candidate(import_programs_key_) : nullptr;
                if (chosen && import_program_index_ < chosen->programs.size()) {
                    const auto& path = chosen->programs[import_program_index_].path;
                    view_.subtitle = "Starts with " + path.substr(path.find_last_of('/') + 1) +
                                     ", which you can change in Game settings. " + kImportChoiceText;
                } else {
                    view_.subtitle = kImportChoiceText;
                }
            }
            else if (import_from_drop) view_.subtitle = (size.empty() ? std::string("The game") : size + ". The game") + " leaves the drop folder.";
            else view_.subtitle = (size.empty() ? std::string("The game") : size) + " will be copied there.";
        }
        if (modal.id == "kept-data")
            view_.subtitle = "Saves and settings kept when a game was uninstalled. Installing a game again brings its saves "
                             "back. Saves in Steam Cloud stay in Steam Cloud.";
        if (modal.id == "content-language")
            view_.subtitle = "New downloads are in this language when the game has it, and in English when it does not. "
                             "Games already installed keep the language they were downloaded in.";
        for (const auto& action_key : modal.action_keys) {
            auto& item = row(action_key, RowKind::Action, action_label(action_key), action_key);
            item.dangerous = modal.destructive && action_key != "cancel";
            // The game-language picker: every language Steam has, with the one in use ticked.
            if (modal.id == "content-language" && action_key.rfind("content-language-", 0) == 0) {
                const auto code = action_key.substr(17);
                item.kind = RowKind::Choice;
                item.title = playden::steam::LanguageName(code);
                item.selected = code == state_.content_language;
                if (item.selected) item.value = "In use";
            }
            // Demos is on unless the player hid them.
            if (action_key == "filter-hide-demos") {
                item.kind = RowKind::Toggle;
                item.selected = !state_.library.filters.contains("hide-demos");
                item.value = item.selected ? "On" : "Off";
            }
            // The menu's Favorite says what it will do for this game.
            if (modal.id == "context" && action_key == "favorite")
                item.title = state_.library.favorites.contains(state_.navigation.selected_game_id) ? "Remove from Favorites"
                                                                                                 : "Add to Favorites";
            if (settings_ready && action_key == "preset-picker") {
                item.kind = RowKind::Profile;
                const auto* preset = preset_for(settings_game);
                const std::string name = preset ? preset->name : "Playden default";
                item.value = settings_custom ? "Custom · from " + name : name;
                item.subtitle = "A ready-made bundle of the settings below.";
            } else if (settings_ready && modal.id == "preset-picker" && action_key.rfind("preset-", 0) == 0 && action_key != "preset-community") {
                item.kind = RowKind::Profile;
                const auto id = action_key.substr(7);
                const auto* current = preset_for(settings_game);
                for (const auto& preset : settings_presets_)
                    if (preset.id == id) { item.title = preset.name; item.subtitle = preset.try_when; item.value = preset.short_hint; }
                item.selected = current ? current->id == id : id == "playden-default";
                if (item.selected) item.badge = settings_custom ? "Current · changed" : "Current";
            } else if (settings_ready && action_key.rfind("setting-", 0) == 0) {
                const auto id = action_key.substr(8);
                const auto* definition = rt::FindSetting(id);
                const auto& value = settings_values.values.at(id);
                item.kind = definition->kind == rt::SettingKind::kText ? RowKind::Text : RowKind::Choice;
                item.title = definition->label;
                item.subtitle = value.available ? definition->effect : value.reason;
                // A saved choice that cannot apply now (Frame limit 120 at 60 Hz) fell back; the row says why.
                if (value.available && !value.locked && !value.reason.empty()) item.subtitle = value.reason;
                if (modal.id == "button-mapping") item.subtitle.clear();  // the sheet title says it once
                if (id == "graphics" && value.source == rt::ValueSource::kRecipe) {
                    const auto detected = rt::DetectedLabel(RecipeFor(settings_game).detected);
                    if (!detected.empty()) item.subtitle = "Chosen from the game's files: " + detected + ".";
                }
                if (id == "launch_option") {
                    item.value = settings_values.launch ? settings_values.launch->label : "Choose a program";
                    if (settings_values.launch && settings_values.launch->id == rt::kProgramEntryId)
                        item.subtitle = "From the game folder: " + settings_values.launch->executable + ".";
                    else if (!settings_values.launch) item.subtitle = "Pick the game's program from its folder.";
                }
                else if (id.rfind("button.", 0) == 0) item.value = binding_label(value.value);
                else if (id == "program_folder")
                    item.value = value.value.empty() ? "Program's folder" : value.value == "." ? "Game folder" : value.value;
                else if (definition->kind == rt::SettingKind::kText) item.value = value.value.empty() ? "Empty" : value.value;
                // A choice that waits for the kit update shows as chosen; the subtitle says when it applies.
                else item.value = choice_label(*definition, rt::ChosenValue(value));
                item.badge = value.unlocked ? "Changed anyway" : value.locked ? "Required by this game"
                    : definition->apply == rt::ApplyMode::kGameClosed ? "Game must be closed"
                    : definition->apply == rt::ApplyMode::kPreparesBottle ? "Prepares bottle" : "";
                // A locked row opens "Change anyway"; the program type only follows the program.
                // A row the kit's runtime takes can be chosen before the title's update (it applies then).
                item.enabled = (value.available || value.waits_for_update) && id != "architecture";
                item.modified = value.source == rt::ValueSource::kUser || value.unlocked;
                if (value.unlocked) item.subtitle = value.reason;
                if (id == "architecture") item.subtitle = "Follows the program you choose.";
                if (id == "start_check") {
                    const auto setup = game_setups_.find(settings_game);
                    std::string problem = setup != game_setups_.end() && setup->second.draft ? setup->second.refusal : std::string();
                    if (setup != game_setups_.end() && settings_values.launch)
                        for (const auto& program : setup->second.programs)
                            if (program.path == settings_values.launch->executable && !program.refusal.empty()) problem = program.refusal;
                    if (!problem.empty()) item.subtitle = problem;
                }
                std::string closed_reason;
                if (item.enabled && definition->apply == rt::ApplyMode::kGameClosed && !game_closed(settings_game, &closed_reason)) {
                    item.enabled = false;
                    item.subtitle = closed_reason;
                }
                // The technical names show on the focused row only, so more rows fit.
                const bool focused_row = modal.focused_action >= 0 &&
                    static_cast<std::size_t>(modal.focused_action) < modal.action_keys.size() &&
                    modal.action_keys[static_cast<std::size_t>(modal.focused_action)] == action_key;
                if (focused_row && !definition->also_called.empty() && modal.id != "button-mapping")
                    item.subtitle += " · Also called " + definition->also_called;
            } else if (settings_ready && action_key == "steam-app-id") {
                // Saved in the game's settings record; the game's own steam_appid.txt wins over it.
                item.kind = RowKind::Text;
                item.title = "Steam app ID";
                const auto record = state_.game_settings.find(settings_game);
                const std::uint32_t saved = record == state_.game_settings.end() ? 0 : record->second.steam_app_id;
                const auto runtime_game = state_.runtime_games.find(settings_game);
                const bool from_file = runtime_game != state_.runtime_games.end() && runtime_game->second.steam_app_from_game_file &&
                                       runtime_game->second.steam_app_id != 0;
                item.value = from_file ? std::to_string(runtime_game->second.steam_app_id)
                           : saved ? std::to_string(saved) : std::string("None");
                item.subtitle = from_file ? "From the game's steam_appid.txt" : "Only for games that need Steam. Most run without it.";
                item.modified = saved != 0;
            } else if (settings_ready && action_key == "controller-tuning") {
                item.kind = RowKind::Choice;
                item.title = "Controller tuning";
                item.subtitle = "Pointer stick, dead zones, rumble and invert.";
                int changed = 0;
                for (const auto& definition : rt::SettingsCatalogue())
                    if (definition.group == rt::SettingGroup::kControllerTuning &&
                        settings_values.values.at(definition.id).source == rt::ValueSource::kUser)
                        ++changed;
                item.value = changed == 0 ? "Default" : std::to_string(changed) + " changed";
                item.modified = changed > 0;
            } else if (settings_ready && action_key == "button-mapping") {
                item.kind = RowKind::Choice;
                item.title = "Button mapping";
                item.subtitle = "Keys and mouse clicks the controller buttons send.";
                int bound = 0;
                bool changed = false;
                for (const auto& button : rt::ProsperoButtons()) {
                    const auto& value = settings_values.values.at("button." + button);
                    bound += value.value.empty() || value.value == "none" ? 0 : 1;
                    changed = changed || value.source == rt::ValueSource::kUser;
                }
                item.value = bound == 0 ? "None" : std::to_string(bound) + (bound == 1 ? " button" : " buttons");
                item.modified = changed;
            } else if (settings_ready && (action_key == "section-compatibility" || action_key == "section-advanced")) {
                item.kind = RowKind::Heading;
                const bool advanced = action_key == "section-advanced";
                int count = 0;
                for (const auto& definition : rt::SettingsCatalogue()) {
                    const bool inside = advanced ? definition.group == rt::SettingGroup::kAdvanced
                                                 : definition.group == rt::SettingGroup::kCompatibility ||
                                                       definition.group == rt::SettingGroup::kGraphics ||
                                                       definition.group == rt::SettingGroup::kWindows;
                    if (inside && game_settings_row_shown(definition, settings_game)) ++count;
                }
                item.value = expanded_settings_sections_.count(action_key) ? "Hide" : "Show " + std::to_string(count);
            } else if (settings_ready && action_key.rfind("heading-", 0) == 0) {
                // Names the rows below it; not focusable (move_game_settings skips it).
                item.kind = RowKind::Separator;
                item.action.clear();
                item.enabled = false;
            } else if (settings_ready && !modal.setting_id.empty() && action_key.rfind("choice-", 0) == 0 && action_key != "choice-custom") {
                item.kind = RowKind::Choice;
                const auto index = static_cast<std::size_t>(std::stoul(action_key.substr(7)));
                const auto& recipe = RecipeFor(settings_game);
                std::string value;
                if (modal.setting_id == "launch_option") {
                    value = recipe.launch_entries[index].id;
                    item.title = recipe.launch_entries[index].label;
                } else if (modal.setting_id.rfind("button.", 0) == 0) {
                    value = common_bindings()[index].first;
                    item.title = common_bindings()[index].second;
                    if (index == 0) item.subtitle = "Use the preset or game binding: " + binding_label(inherited.value) + ".";
                } else {
                    const auto& choice = rt::FindSetting(modal.setting_id)->choices[index];
                    value = choice.value;
                    item.title = choice.label;
                    item.subtitle = choice.explanation;
                    // A choice the other settings rule out (Frame limit 120 without 120 Hz output) says why.
                    if (auto why = rt::SettingChoiceUnavailable(settings_values, modal.setting_id, value); !why.empty()) {
                        item.subtitle = std::move(why);
                        item.enabled = false;
                    } else if (rt::SettingChoiceWaitsForUpdate(settings_values, modal.setting_id, value)) {
                        item.subtitle = rt::kAppliesAfterKitUpdate;
                    }
                }
                const bool button = modal.setting_id.rfind("button.", 0) == 0;
                const auto& current = settings_values.values.at(modal.setting_id);
                // A folder pick of the file a Steam entry names shows on that entry.
                const bool picked_here = modal.setting_id == "launch_option" && settings_values.launch &&
                    settings_values.launch->id == rt::kProgramEntryId &&
                    BottlePath(settings_values.launch->executable) == BottlePath(recipe.launch_entries[index].executable);
                item.selected = button && index == 0 ? current.source != rt::ValueSource::kUser
                    : (value == rt::ChosenValue(current) || picked_here) && !(button && current.source != rt::ValueSource::kUser);
                item.value = button ? (item.selected && index != 0 ? "Your choice" : "")
                    : value == rt::ChosenValue(inherited) ? source_label(inherited.source) : item.selected ? "Your choice" : "";
            } else if (settings_ready && action_key == "choice-custom") {
                item.kind = RowKind::Text;
                item.title = "Other key…";
                item.subtitle = "Type a key name, a letter, F1–F24 or vk:0xNN.";
            } else if (settings_ready && action_key == "reset-settings") {
                item.enabled = settings_record != state_.game_settings.end() && has_resettable(settings_record->second.overrides);
            } else if (settings_ready && action_key == "effective-command") {
                item.kind = RowKind::Text;
                item.title = "Effective command";
                item.subtitle = settings_values.launch ? settings_values.launch->executable +
                    (settings_values.arguments.empty() ? "" : " " + settings_values.arguments) : std::string("No program chosen yet");
                item.value = "Read only";
            } else if (settings_ready && action_key == "export-recipe") {
                item.kind = RowKind::Action;
                item.title = "Export as recipe";
                item.subtitle = "Saves these settings as a community recipe to share with Playden.";
                item.enabled = settings_values.launch != nullptr;
            } else if (settings_ready && !modal.setting_id.empty() && action_key.rfind("program-", 0) == 0 && action_key != "program-scan") {
                // A program from the game folder, in the Program picker.
                item.kind = RowKind::Choice;
                const auto setup = game_setups_.find(settings_game);
                const auto index = static_cast<std::size_t>(std::stoul(action_key.substr(8)));
                if (setup != game_setups_.end() && index < setup->second.programs.size()) {
                    const auto& program = setup->second.programs[index];
                    item.title = program.path;
                    const auto detected = rt::DetectedLabel(program.detected);
                    item.subtitle = program.architecture.empty() ? "Not a Windows program the PS5 can start."
                        : program.refusal.empty() ? (program.architecture == "pe64" ? "64-bit" : "32-bit") +
                                                     (detected.empty() ? std::string() : " · " + detected)
                        : program.refusal_soft ? program.refusal + " Choosing it also sets Known problems to Try anyway."
                                               : program.refusal;
                    item.enabled = !program.architecture.empty() && (program.refusal.empty() || program.refusal_soft);
                    item.selected = settings_values.launch && settings_values.launch->executable == program.path;
                    item.value = item.selected ? "Your choice" : "In the game folder";
                }
            } else if (action_key == "program-scan") {
                item.kind = RowKind::Status;
                item.subtitle = "Playden is listing the programs in the game folder.";
                item.enabled = false;
            } else if (settings_ready && !modal.setting_id.empty() && action_key == "value-reset") {
                const auto& current = settings_values.values.at(modal.setting_id);
                item.subtitle = modal.setting_id == "launch_option" ? "Starts the program the recipe or Steam names."
                                                                    : "Uses the value from the game's recipe.";
                item.enabled = current.source == rt::ValueSource::kUser || current.unlocked;
                if (!item.enabled) item.subtitle = "Already the recipe's value.";
            } else if (settings_ready && !modal.setting_id.empty() && action_key == "value-edit") {
                item.kind = RowKind::Text;
                const auto& current = settings_values.values.at(modal.setting_id);
                item.subtitle = current.value.empty() ? "Empty" : current.value;
            } else if (settings_ready && action_key.rfind("override-remove-", 0) == 0) {
                const auto entries = split_overrides(settings_values.dll_overrides);
                const auto index = static_cast<std::size_t>(std::stoul(action_key.substr(16)));
                if (index < entries.size()) {
                    item.title = entries[index];
                    if (rt::IsGraphicsDllOverride(RecipeFor(settings_game), entries[index])) {
                        item.subtitle = "Follows Graphics: on while Graphics is DXVK.";
                        item.enabled = false;
                    } else {
                        item.subtitle = "Remove it: Wine's own library is used again.";
                        item.value = "Remove";
                    }
                }
            } else if (settings_ready && action_key == "override-add") {
                item.subtitle = "Like dinput8=n,b: n uses the game's copy, b Wine's.";
            } else if (settings_ready && action_key == "unlock-setting") {
                item.dangerous = true;
            } else if (settings_ready && modal.id == "preset-picker" && action_key == "preset-community") {
                item.kind = RowKind::Profile;
                item.title = "Community recipe";
                item.subtitle = "Settings shared through prospero-win-profiles. Waits for the shared recipe index.";
                item.value = "Not available yet";
                item.enabled = false;
            } else if (modal.id == "game-maintenance" && action_key.rfind("maintenance-", 0) == 0) {
                const auto runtime_game = state_.runtime_games.find(settings_game);
                const LiveRuntimeGame* runtime = live() && runtime_game != state_.runtime_games.end() ? &runtime_game->second : nullptr;
                const auto kind = action_key == "maintenance-repair" ? MaintenanceKind::Repair
                                : action_key == "maintenance-shader" ? MaintenanceKind::ClearShaderCache
                                : action_key == "maintenance-backup" ? MaintenanceKind::BackupSaves
                                : action_key == "maintenance-recreate" ? MaintenanceKind::RecreateBottle
                                : action_key == "maintenance-update" ? MaintenanceKind::UpdateRuntime
                                                                       : MaintenanceKind::ApplySettings;
                std::string closed_reason;
                const bool closed = game_closed(settings_game, &closed_reason);
                const bool busy = runtime && runtime->maintenance_running;
                if (action_key == "maintenance-reset") {
                    item.title = "Reset settings";
                    item.subtitle = "Removes your changes; the preset and the program you chose stay.";
                    item.enabled = settings_record != state_.game_settings.end() && has_resettable(settings_record->second.overrides);
                    if (!item.enabled)
                        item.subtitle = settings_record != state_.game_settings.end() && !settings_record->second.overrides.empty()
                            ? "Nothing else to reset. The program you chose stays; change it in the Program row."
                            : "Nothing to reset: the settings follow the preset.";
                } else if (kind == MaintenanceKind::UpdateRuntime) {
                    // What Play does first for a title on an older kit's runtime ("Kit update available"), alone.
                    const bool stale = runtime && (runtime->status == "Kit update available" || runtime->status == "Update available");
                    item.title = "Update runtime";
                    item.subtitle = "Moves the game to the installed kit's runtime without starting it.";
                    item.badge = "Game must be closed";
                    item.enabled = stale && runtime->can_prepare && closed && !busy;
                    // While it runs the title is being prepared, no longer "Kit update available".
                    const bool updating = busy && runtime->maintenance == MaintenanceKind::UpdateRuntime;
                    // A setup an earlier kit left has no title to update: it is made again.
                    if (!stale && !updating)
                        item.subtitle = runtime && runtime->status == "No kit" ? "Needs a usable runtime kit."
                                      : runtime && runtime->setup_other_kit
                                          ? "Setup was started with an earlier kit. Use Recreate bottle to finish it; saves are kept."
                                          : "Already up to date.";
                    else if (!updating && !closed) item.subtitle = closed_reason;
                    else if (!updating && !runtime->can_prepare && !busy) item.subtitle = "Wait for the game's setup to finish.";
                } else {
                    item.title = kind == MaintenanceKind::Repair ? "Repair"
                               : kind == MaintenanceKind::ClearShaderCache ? "Clear shader cache"
                               : kind == MaintenanceKind::BackupSaves ? "Back up saves" : "Recreate bottle";
                    item.subtitle = kind == MaintenanceKind::Repair ? "Fixes the files the game needs to start and writes its settings again."
                                  : kind == MaintenanceKind::ClearShaderCache ? "Shaders compile again on the next start."
                                  : kind == MaintenanceKind::BackupSaves ? "Copies the Windows user folders aside."
                                                                          : "Makes the Windows environment again, keeping saves and settings.";
                    item.badge = kind == MaintenanceKind::RecreateBottle ? "Prepares bottle" : "Game must be closed";
                    item.enabled = closed && !busy;
                    if (!closed) item.subtitle = closed_reason;
                    if (kind == MaintenanceKind::BackupSaves && runtime && runtime->save_backups > 0)
                        item.value = std::to_string(runtime->save_backups) + (runtime->save_backups == 1 ? " backup" : " backups") +
                                     " · newest " + playden::app::FormatLogTimestamp(runtime->latest_save_backup).substr(0, 10);
                }
                if (runtime && runtime->maintenance == kind && kind != MaintenanceKind::ApplySettings) {
                    if (runtime->maintenance_running) item.value = "Working…";
                    else if (!runtime->maintenance_message.empty()) item.subtitle = runtime->maintenance_message;
                }
                // A title waiting for recovery: only Recreate bottle (or the title's Resume) gets it out.
                if ((kind == MaintenanceKind::Repair || kind == MaintenanceKind::ClearShaderCache ||
                     kind == MaintenanceKind::BackupSaves) && runtime && runtime->status == "Needs recovery") {
                    item.enabled = false;
                    item.subtitle = "Finish the PS5 title's recovery first, or use Recreate bottle.";
                }
            } else if (action_key.rfind("volume-", 0) == 0) {
                item.kind = RowKind::Volume;
                if (const auto* volume = find_volume(state_, action_key.substr(7))) {
                    item.title = volume->label;
                    const auto unused = volume->capacity_bytes - std::min(volume->used_bytes, volume->capacity_bytes);
                    const auto free = unused - std::min(volume->reserved_bytes, unused);
                    if (volume->capacity_known)
                        item.value = format_bytes(free) + " free";
                    else
                        item.value = "Free space unknown";
                    item.subtitle = !volume->available ? "Disconnected" : !volume->writable ? "No write access" : "Available";
                    item.enabled = volume->available && volume->writable;
                    item.selected = volume->id == (live() && picking_install_volume_ ? install_target() : selected_volume_id_);
                    if (modal.id == "import-storage") {
                        // The wizard's drive choice: what happens on each drive, or why it can't.
                        const auto blocker = import_volume_blocker(import_wizard_subject, *volume);
                        item.selected = false;
                        item.enabled = blocker.empty();
                        item.tone = blocker == "Not enough space" || blocker == "Disconnected" || blocker == "No write access"
                                        ? "warn" : "";
                        if (!blocker.empty()) item.subtitle = blocker;
                        else if (import_from_drop)
                            item.subtitle = volume->id == kImportDropVolume ? "Instant · moves the folder" : "Copies, then frees the drop folder";
                        else item.subtitle = volume->id == selected_volume_id_ ? "Default · Available" : "Available";
                    }
                }
            }
            else if (action_key == "quit-to-home") item.dangerous = true;  // the dialog's primary action
            else if (action_key == "saves-delete-confirm" || action_key == "saves-leftover-confirm") item.dangerous = true;
            else if (action_key == "refresh-library") item.enabled = !state_.library_loading && !state_.library_syncing;
            else if (action_key == "installed-sort") {
                item.title = "Sort by";
                item.value = installed_by_played_ ? "Last played" : "Size";
            }
            else if (action_key == "installed-select-all") {
                std::size_t choosable = 0, chosen = 0;
                for (const auto id : installed_list())
                    if (find_installation(state_, id) && installed_blocker(id).empty()) {
                        ++choosable;
                        chosen += installed_selected_.contains(id) ? 1 : 0;
                    }
                item.title = choosable > 0 && chosen == choosable ? "Clear selection" : "Select all";
                item.enabled = choosable > 0;
            }
            else if (action_key.rfind("installed-toggle-", 0) == 0) {
                const auto id = key_id(std::string_view(action_key).substr(17));
                const auto* game = find_game(state_, id);
                const auto* installation = find_installation(state_, id);
                item.kind = RowKind::Toggle;
                item.game_id = id;
                item.title = game && !game->title.empty() ? game->title : "Steam app " + ToKey(id);
                item.selected = installed_selected_.contains(id);
                item.value = installation ? format_bytes(installation->installed_bytes) : std::string();
                const auto blocker = installed_blocker(id);
                if (!blocker.empty()) {
                    item.subtitle = blocker;
                    item.enabled = false;
                } else if (const auto* stopped = uninstall_stop(id)) {
                    // Its uninstall stopped: the reason stays until it goes again or is dismissed in Queue.
                    item.subtitle = "Stopped: " + stopped->stopped;
                    item.tone = "problem";
                } else {
                    const auto when = game && game->last_played ? time_ago(state_.clock_ticks, *game->last_played) : std::string();
                    item.subtitle = !game || !game->last_played ? "Never played" : when.empty() ? "Played" : "Played " + when;
                    // With games on more than one drive, each row names its drive.
                    std::set<std::string> drives;
                    for (const auto& other : state_.installations) drives.insert(other.volume_id);
                    if (drives.size() > 1 && installation)
                        if (const auto* volume = find_volume(state_, installation->volume_id))
                            item.subtitle = volume->label + " · " + item.subtitle;
                }
            }
            else if (action_key == "installed-selection-uninstall") {
                item.title = "Uninstall selected";
                item.dangerous = true;
                item.enabled = !installed_selected_.empty();
            }
            else if (modal.id == "import-games" && action_key == "import-look") {
                item.title = state_.import_scanning ? "Looking for games…" : "Look for games";
                if (state_.import_scanning) { item.kind = RowKind::Status; item.enabled = false; }
            }
            else if (modal.id == "import-games" && action_key.rfind("import-group-", 0) == 0) {
                item.kind = RowKind::Heading;
                item.action.clear();
                item.enabled = false;
                for (const auto& [volume, name] : kImportGroups)
                    if (action_key == std::string("import-group-") + volume) item.title = name;
            }
            else if (modal.id == "import-games" && action_key.rfind("import-candidate-", 0) == 0) {
                if (const auto* candidate = import_candidate(action_key)) {
                    item.title = candidate->title;
                    item.value = candidate->imported.valid() ? std::string("Added") : format_bytes(candidate->total_bytes);
                    item.subtitle = candidate->path;
                    if (candidate->imported.valid()) item.game_id = candidate->imported;
                    else if (candidate->programs.empty()) { item.subtitle = "No Windows program found"; item.enabled = false; }
                    else if (!candidate->ascii_ok) item.subtitle += " · Copy only";
                    // An add on its way, or why the last one was refused.
                    if (!candidate->imported.valid() && pending_imports_.count(action_key)) {
                        item.value = "Adding…";
                        item.enabled = false;
                    } else if (const auto failure = import_failures_.find(action_key);
                               !candidate->imported.valid() && failure != import_failures_.end()) {
                        item.subtitle = "Not added · " + failure->second;
                    }
                }
            }
            else if (modal.id == "steam-programs" && action_key.rfind("steam-program-", 0) == 0) {
                const auto index = static_cast<std::size_t>(std::stoul(action_key.substr(14)));
                if (index < steam_programs_shown_.size()) {
                    item.title = steam_programs_shown_[index].path;
                    if (steam_programs_shown_[index].helper) item.subtitle = "Installer or tool";
                }
            }
            else if (import_programs && action_key.rfind("import-program-", 0) == 0) {
                const auto index = static_cast<std::size_t>(std::stoul(action_key.substr(15)));
                if (index < import_programs->programs.size()) {
                    const auto& program = import_programs->programs[index];
                    item.title = program.path;
                    item.value = format_bytes(program.bytes);
                    if (program.helper) item.subtitle = "Installer or tool";

                }
            }
            else if (modal.id == "import-choice" && action_key == "import-play-in-place") {
                std::string place = import_volume_name(import_wizard_subject.volume);
                if (place == import_wizard_subject.volume)
                    if (const auto* volume = find_volume(state_, place)) place = volume->label;
                if (place == "Extended storage") place = "extended storage";
                item.title = import_from_drop ? "Play from the drop folder" : "Play from the " + place;
                item.subtitle = !import_wizard_subject.ascii_ok ? kImportCopyOnlyText
                    : import_from_drop ? "Nothing moves. The files stay in /data/playden-import."
                                       : "Nothing is copied. Keep the drive plugged in.";
                item.enabled = import_wizard_subject.ascii_ok;
            }
            else if (modal.id == "import-choice" && action_key == "import-copy") {
                item.title = "Import into Playden";
                item.value = format_bytes(import_wizard_subject.bytes);
                item.subtitle = import_from_drop ? "Takes the game out of the drop folder. You pick the storage next."
                                                 : "Copies the game. You pick the storage next.";
            }
            else if ((modal.id == "import-games" || modal.id == "import-programs" || import_wizard) && action_key == "cancel") {
                item.title = modal.id == "import-games" ? "Close" : "Back";
            }
            else if (action_key == "kept-select-all") {
                const bool all = !state_.kept_bottles.empty() && kept_selected_.size() == state_.kept_bottles.size();
                item.title = all ? "Clear selection" : "Select all";
                item.enabled = !state_.kept_bottles.empty();
            }
            else if (action_key.rfind("kept-toggle-", 0) == 0) {
                const auto id = key_id(std::string_view(action_key).substr(12));
                for (const auto& kept : state_.kept_bottles) {
                    if (kept.game_id != id) continue;
                    item.kind = RowKind::Toggle;
                    item.title = kept_name(state_, kept);
                    item.value = format_bytes(kept.bytes);
                    // The console has no gmtime_r: the date is the log timestamp's day.
                    if (kept.kept_at > 0) item.subtitle = "Kept " + playden::app::FormatLogTimestamp(kept.kept_at).substr(0, 10);
                    item.selected = kept_selected_.contains(id);
                }
            }
            else if (action_key == "kept-selection-delete") {
                std::uint64_t bytes = 0;
                for (const auto& kept : state_.kept_bottles) if (kept_selected_.contains(kept.game_id)) bytes += kept.bytes;
                item.title = "Delete selected";
                item.dangerous = true;
                item.enabled = !kept_selected_.empty();
                if (!kept_selected_.empty()) item.value = kept_count(kept_selected_.size()) + " · " + format_bytes(bytes);
            }
            else if (action_key == "uninstall-delete-saves") {
                item.kind = RowKind::Toggle;
                item.dangerous = false;
                item.selected = uninstall_delete_saves_;
                item.subtitle = "Deletes the game's private Windows folder instead of keeping it. Saves in Steam Cloud stay there.";
            }
            else if (action_key == "game-settings") {
                const auto id = state_.navigation.selected_game_id;
                const auto record = state_.game_settings.find(id);
                const auto* preset = preset_for(id);
                const std::string name = preset ? preset->name : "Playden default";
                item.value = record != state_.game_settings.end() && !record->second.overrides.empty() ? "Custom · from " + name : name;
                if (live()) {
                    const auto runtime_game = state_.runtime_games.find(id);
                    if (record != state_.game_settings.end() && runtime_game != state_.runtime_games.end() &&
                        record->second.revision > runtime_game->second.settings_revision)
                        item.subtitle = "Changes apply on next launch";
                }
            } else if (action_key == "game-maintenance") {
                if (live())
                    if (const auto runtime_game = state_.runtime_games.find(state_.navigation.selected_game_id);
                        runtime_game != state_.runtime_games.end() && runtime_game->second.maintenance_running)
                        item.value = "Working…";
            }
            else if (action_key == "verify" || action_key == "uninstall") {
                if (!is_installed(state_, state_.navigation.selected_game_id)) { item.enabled = false; item.subtitle = "Game is not installed"; }
            } else if (action_key == "hide") item.title = state_.library.hidden.contains(state_.navigation.selected_game_id) ? "Unhide" : "Hide";
            else if (action_key == "runtime-retry") item.subtitle = state_.runtime == RuntimeState::Ready ? "Ready — simulated" : state_.runtime == RuntimeState::Checking ? "Checking — simulated" : state_.runtime == RuntimeState::Failed ? "Failed — retry available" : "Unavailable — retry available";
            else if (action_key == "account-auth") { item.kind = RowKind::Status; item.value = auth_name(state_.account().authentication); item.subtitle = state_.account().error; item.enabled = false; }
            else if (action_key == "account-last-sync") { item.kind = RowKind::Status; item.value = state_.account().last_sync == 0 ? "Never" : std::to_string(state_.account().last_sync); item.enabled = false; }
            else if (action_key.rfind("open-game:", 0) == 0) item.title = "Open game";
            else if (action_key == "about-version") { item.kind = RowKind::Status; item.value = live() ? "Playden " + state_.self_update.current +
                (state_.self_update.current.find("-beta.") != std::string::npos ? " (Beta)" : "") : "Simulated"; item.enabled = false; }
            else if (action_key == "about-build") { item.kind = RowKind::Status; item.value = live() ? "Live service model" : "UI comparison fixture"; item.enabled = false; }
            else if (action_key == "about-framework") { item.kind = RowKind::Status; item.value = "Homebrew native model + Hui"; item.enabled = false; }
            else if (action_key == "about-platform") { item.kind = RowKind::Status; item.value = "Host / PS5 homebrew"; item.enabled = false; }
            else if (action_key == "about-kit" || action_key == "about-kit-source") {
                // The kit in use, and the one this build was tested with when they differ.
                LiveKitInfo kit = state_.kit;
                if (!live()) kit = {"playden-kit", std::string(64, 'b'), std::string(64, 'b'), std::string(40, '2'), false, {}};
                const auto short_hash = [](const std::string& hash) { return hash.substr(0, 12); };
                item.kind = RowKind::Status; item.enabled = false;
                if (kit.kit_id.empty()) {
                    item.value = action_key == "about-kit" ? "None" : "—";
                    if (action_key == "about-kit") item.subtitle = kit.unavailable_reason.empty() ? "Checking" : kit.unavailable_reason;
                } else if (action_key == "about-kit") {
                    item.value = kit.kit_id + " · " + short_hash(kit.manifest_sha256);
                    item.subtitle = kit.manifest_sha256 == kit.expected_manifest_sha256
                        ? "The kit this version of Playden was tested with"
                        : "Not the kit this version was tested with (" + short_hash(kit.expected_manifest_sha256) + ")";
                } else {
                    item.value = "prospero-win " + kit.prospero_win_revision.substr(0, 7);
                    if (kit.source_dirty) item.subtitle = "Includes uncommitted changes";
                }
            }
            else if (action_key == "install-confirm" || action_key == "install-estimate-retry") {
                const auto id = live() ? state_.navigation.detail_id.value_or(state_.navigation.selected_game_id)
                                       : state_.navigation.selected_game_id;
                const auto assessment = assess_install(state_, id, live() ? install_target() : selected_volume_id_,
                                                       live() ? steam::kInstallSpaceMargin : 0);
                if (action_key == "install-estimate-retry") item.enabled = assessment.readiness == InstallReadiness::UnknownSize;
                else if (live()) {
                    item.enabled = live_install_allowed(assessment.readiness, free_space_supported_);
                    item.subtitle = assessment.readiness == InstallReadiness::Insufficient ? "Not enough space"
                        : assessment.readiness == InstallReadiness::Unavailable ? "Drive disconnected"
                        : assessment.readiness == InstallReadiness::ReadOnly ? "Drive is read-only"
                        : assessment.readiness == InstallReadiness::UnknownCapacity && free_space_supported_ ? "Free space unknown"
                        : "Starts the download";
                } else {
                    item.enabled = assessment.readiness == InstallReadiness::Ready && assessment.download_bytes.has_value();
                    item.subtitle = assessment.readiness == InstallReadiness::UnknownSize ? "Fetching size estimate" : assessment.readiness == InstallReadiness::Insufficient ? "Insufficient space" : assessment.readiness == InstallReadiness::Unavailable ? "Storage disconnected" : assessment.readiness == InstallReadiness::ReadOnly ? "Storage is read-only" : "Creates one download job";
                }
            }
            else if (action_key.rfind("rating-", 0) == 0) {
                item.kind = RowKind::Choice;
                const auto value = action_key.substr(7);
                item.title = value == "works" ? "Works" : value == "playable" ? "Playable" : value == "broken" ? "Broken" : "Untested";
                const auto rating = state_.library.ratings.find(state_.navigation.selected_game_id);
                const auto current = rating == state_.library.ratings.end() ? Compatibility::Untested : rating->second;
                item.selected = (value == "works" && current == Compatibility::Works) || (value == "playable" && current == Compatibility::Playable)
                    || (value == "broken" && current == Compatibility::Broken) || (value == "untested" && current == Compatibility::Untested);
            } else if (action_key.rfind("collection-", 0) == 0 && action_key != "collection-new" && action_key != "collection-manage") {
                // A game's sheet: one checkbox per collection. Manage: rename, pin and delete each one.
                const char* prefixes[] = {"collection-toggle-", "collection-rename-", "collection-pin-", "collection-delete-"};
                for (std::size_t index = 0; index < 4; ++index) if (action_key.rfind(prefixes[index], 0) == 0) {
                    const auto id = action_key.substr(std::char_traits<char>::length(prefixes[index]));
                    const auto found = std::find_if(state_.library.collections.begin(), state_.library.collections.end(), [&id](const Collection& value) { return value.id == id; });
                    if (found == state_.library.collections.end()) continue;
                    const auto count = found->members.size();
                    const std::string games = std::to_string(count) + (count == 1 ? " game" : " games");
                    if (index == 0) {
                        item.kind = RowKind::Toggle;
                        item.title = found->name;
                        item.value = games;
                        item.selected = found->members.contains(state_.navigation.selected_game_id);
                    } else if (index == 1) {
                        item.title = "Rename \u201c" + found->name + "\u201d";
                        item.value = games;
                    } else if (index == 2) {
                        item.title = "Pin \u201c" + found->name + "\u201d on Home";
                        item.selected = found->pinned;
                        item.value = found->pinned ? "On" : "Off";
                    } else {
                        item.title = "Delete \u201c" + found->name + "\u201d";
                        item.dangerous = true;
                    }
                }
            }
        }
        view_.focused_row = modal.focused_action;
        if (settings_ready && modal.id == "preset-picker") {
            const auto& focused = modal.action_keys[static_cast<std::size_t>(modal.focused_action)];
            if (focused.rfind("preset-", 0) == 0) build_preset_comparison(settings_game, focused.substr(7));
        }
        view_.legends = {"Cross Select", "Circle Cancel"};
        if (modal.id == "game-settings") view_.legends = {"Cross Change", "Triangle Reset", "L1/R1 Jump", "Circle Close"};
        else if (modal.id == "preset-picker") view_.legends = {"Cross Use this preset", "L1/R1 Jump", "Circle Back"};
        else if (modal.id == "button-mapping") view_.legends = {"Cross Change", "L1/R1 Jump", "Circle Back"};
        else if (modal.id == "controller-tuning") view_.legends = {"Cross Change", "Triangle Reset", "L1/R1 Jump", "Circle Back"};
        else if (modal.id == "game-maintenance") view_.legends = {"Cross Select", "L1/R1 Jump", "Circle Cancel"};
        else if (modal.id == "import-games") view_.legends = {"Cross Select", "Circle Close"};
        else if (modal.id == "import-programs" || modal.id == "import-choice") view_.legends = {"Cross Select", "Circle Back"};
        else if (modal.id == "steam-programs") view_.legends = {"Cross Choose", "Circle Cancel"};
        else if (modal.id == "content-language") view_.legends = {"Cross Use this language", "L1/R1 Jump", "Circle Back"};
        else if (modal.id == "import-storage") view_.legends = {"Cross Import here", "Circle Back"};
        else if (modal.id == "installed-games") view_.legends = {"Cross Select", "Triangle Uninstall", "Circle Close"};
        else if (!modal.setting_id.empty()) view_.legends = {"Cross Select", "Triangle Reset", "L1/R1 Jump", "Circle Back"};
    } else if (state_.navigation.route == Route::Startup && state_.root_access_missing) {
        auto problem = describe_startup_problem(state_.root_access_detail, state_.root_access_granted);
        view_.title = std::move(problem.title);
        view_.subtitle = std::move(problem.subtitle);
        if (problem.show_detail && !state_.root_access_detail.empty())
            row("root-detail", RowKind::Status, "Startup error: " + state_.root_access_detail, "").enabled = false;
        row("root-quit", RowKind::Action, "Quit to Home", "root-quit");
        view_.legends = {"Cross Quit to Home"};
    } else if (state_.navigation.route == Route::Startup) {
        view_.title = "Welcome to Playden";
        view_.subtitle = live() ? "Connect a store or import games to fill your library." : "Browse the local UI fixture or connect a simulated Steam account.";
        if (state_.startup_loading || state_.account().authentication == AuthState::Restoring) {
            row("startup-loading", RowKind::Status, live() ? "Restoring your accounts…" : "Loading saved fixture state…", "").enabled = false;
            if (!live()) row("startup-advance", RowKind::Action, "Advance fixture clock", "advance-clock");
        }
        else if (!state_.persistence_available) {
            row("startup-retry-save", RowKind::Action, "Retry saved state", "retry-save");
            row("startup-continue", RowKind::Action, "Continue without saving", "continue-without-saving");
        } else {
            row("startup-connect", RowKind::Action, "Connect Steam", "connect");
            // Games from a drive or the drop folder need no store account.
            row("startup-import", RowKind::Action, "Import games", "import-manage");
            row("startup-skip", RowKind::Action, "Skip for now", "skip");
        }
    } else if (state_.navigation.route == Route::SignIn && state_.sign_in.step != SignInStep::Qr &&
               state_.account().authentication != AuthState::Syncing) {
        view_.title = "Connect Steam";
        const auto& form = state_.sign_in;
        const bool code_step = form.step == SignInStep::EmailCode || form.step == SignInStep::AuthenticatorCode ||
                               (form.step == SignInStep::Approval && form.code_entry);
        if (form.step == SignInStep::Password) {
            view_.subtitle = form.error.empty() ? "Sign in with your Steam account name and password." : form.error;
            row("signin-account", RowKind::Action, "Account name", "signin-account").value = form.account_name;
            row("signin-password", RowKind::Action, "Password", "signin-password").value = mask_secret(form.password);
            row("signin-submit", RowKind::Action, "Sign in", "signin-submit").enabled =
                !form.account_name.empty() && !form.password.empty();
            row("signin-qr-mode", RowKind::Action, "Use QR code instead", "signin-qr-mode");
            row("signin-stay", RowKind::Toggle, "Stay signed in", "signin-stay").selected = state_.stay_signed_in;
        } else if (form.step == SignInStep::Submitting) {
            view_.subtitle = "Signing in to Steam…";
            row("signin-cancel", RowKind::Action, "Cancel", "signin-cancel");
        } else if (code_step) {
            const bool email = form.step == SignInStep::EmailCode || (form.step == SignInStep::Approval && form.code_is_email);
            const std::string prompt = !email ? "Enter the code from the Steam Mobile app."
                : form.email_domain.empty() ? "Enter the code Steam sent to your email."
                : "Enter the code sent to …@" + form.email_domain + ".";
            view_.subtitle = form.code_sent ? "Checking the code…" : form.code_rejected ? "That code was not right. " + prompt : prompt;
            if (form.step == SignInStep::Approval && !form.code_sent)
                view_.subtitle += " You can also approve the sign-in in the Steam Mobile app.";
            row("guard-code", RowKind::Action, "Code", "guard-code").value = form.code;
            row("guard-submit", RowKind::Action, "Submit", "guard-submit").enabled = form.code.size() == 5 && !form.code_sent;
            row("guard-cancel", RowKind::Action, "Cancel", "guard-cancel");
        } else {
            view_.subtitle = !form.email_domain.empty() && !form.code_allowed
                ? "Approve the sign-in from the email Steam sent to …@" + form.email_domain + "."
                : "Approve the sign-in in the Steam Mobile app.";
            if (form.code_allowed) row("guard-use-code", RowKind::Action, "Type a code instead", "guard-use-code");
            row("guard-cancel", RowKind::Action, "Cancel", "guard-cancel");
        }
        if (!live() && (form.step == SignInStep::Approval || form.step == SignInStep::Submitting))
            row("qr-fixture-controls", RowKind::Action, "QR fixture controls", "qr-fixture-controls");
    } else if (state_.navigation.route == Route::SignIn && state_.account().authentication == AuthState::Failed &&
               state_.account().storage_full) {
        // The saved sign-in keeps being retried; uninstalling games here gives it the room it needs.
        view_.title = "Console storage is full";
        view_.subtitle = state_.account().error;
        auto& installed = row("signin-installed", RowKind::Action, "Installed games", "installed-manage");
        installed.subtitle = "Uninstall games you no longer play to free up space";
        if (state_.installations.empty()) {
            installed.kind = RowKind::Status; installed.action.clear(); installed.enabled = false; installed.value = "None";
            installed.subtitle = "Free up space in the PS5's Settings > Storage";
        } else {
            std::uint64_t bytes = 0;
            for (const auto& item : state_.installations) bytes += item.installed_bytes;
            installed.value = kept_count(state_.installations.size()) + " · " + format_bytes(bytes);
        }
        row("qr-retry", RowKind::Action, "Retry", "qr-retry");
    } else if (state_.navigation.route == Route::SignIn) {
        view_.title = "Connect Steam";
        view_.subtitle = state_.account().authentication == AuthState::Waiting
            ? (live() ? "Scan the QR code with the Steam mobile app." : "Simulated QR challenge — not a working Steam code")
            : state_.account().authentication == AuthState::Syncing ? "Loading your library…" : state_.account().error;
        if (state_.account().authentication == AuthState::Syncing) {
            if (live()) row("sync-status", RowKind::Status, "Loading library…", "").enabled = false;
            else row("sync-advance", RowKind::Action, "Advance fixture sync", "advance-clock");
        }
        else if (state_.account().authentication == AuthState::Expired) row("qr-refresh", RowKind::Action, "Refresh code", "qr-refresh");
        else if (state_.account().authentication == AuthState::Failed) row("qr-retry", RowKind::Action, "Retry", "qr-retry");
        else row("qr-cancel", RowKind::Action, "Cancel", "qr-cancel");
        if (state_.account().authentication != AuthState::Syncing) {
            row("signin-password-mode", RowKind::Action, "Sign in with account name and password", "signin-password-mode");
            row("signin-stay", RowKind::Toggle, "Stay signed in", "signin-stay").selected = state_.stay_signed_in;
        }
        if (!live() && state_.account().authentication != AuthState::Syncing) row("qr-fixture-controls", RowKind::Action, "QR fixture controls", "qr-fixture-controls");
    } else if (state_.navigation.route == Route::Home) {
        view_.title = "Home";
        view_.subtitle = state_.account().authentication == AuthState::SignedIn
            ? (live() ? (state_.account().error.empty() ? "Your library" : state_.account().error) : "Fixture history — simulated")
            : state_.account().authentication == AuthState::Offline
                ? (state_.account().error.empty() ? "Offline — showing available library" : state_.account().error)
                : "Signed out";
        bool steam_shown = false;
        for (const auto* game : visible_games()) {
            if (view_.rows.size() >= 12) break;
            auto& item = row(game_key(game->id), RowKind::Game, game->title, "open-game");
            item.game_id = game->id;
            item.badge = state_.library.favorites.contains(game->id) ? "Favorite" : "";
            steam_shown = steam_shown || game->id.store == Store::kSteam;
        }
        // Signed out with only other stores' games (imported ones) listed, Steam is still offered, and
        // Home is about those games rather than the account.
        const bool connect = state_.account().authentication != AuthState::SignedIn && !steam_shown;
        if (connect && !view_.rows.empty() && state_.account().authentication != AuthState::Offline) view_.subtitle = "Your library";
        if (view_.rows.empty()) {
            row("home-library", RowKind::Action, "Open Library", "library");
            row("home-import", RowKind::Action, "Import games", "import-manage");
        }
        if (connect) row("home-connect", RowKind::Action, "Connect a store", "connect");
        auto add_shelf = [this](const playden::homebrew::Shelf& source, int shelf_index) {
            ViewData::Shelf shelf;
            shelf.key = source.key; shelf.title = source.title;
            for (const auto id : source.game_ids) if (const auto* game = find_game(state_, id)) {
                ViewRow item;
                item.key = "shelf-" + shelf.key + "-game-" + ToKey(id); item.kind = RowKind::Game; item.title = game->title; item.action = "open-game"; item.game_id = id;
                shelf.items.push_back(std::move(item));
            }
            if (source.more > 0 && !shelf.items.empty()) {
                ViewRow more;
                more.key = playden::homebrew::shelf_more_key(source); more.kind = RowKind::Action;
                more.title = "See all in Library"; more.value = "+" + std::to_string(source.more); more.action = "shelf-more";
                shelf.items.push_back(std::move(more));
            }
            if (!shelf.items.empty()) {
                const auto saved = state_.navigation.home_shelf_focus.find(shelf.key);
                if (shelf_index == state_.navigation.home_shelf)
                    shelf.focused_item = saved == state_.navigation.home_shelf_focus.end() ? 0 : std::clamp(saved->second, 0, static_cast<int>(shelf.items.size()) - 1);
                view_.shelves.push_back(std::move(shelf));
            }
        };
        int shelf_index = 0;
        for (const auto& shelf : playden::homebrew::shelves(state_)) add_shelf(shelf, shelf_index++);
        view_.legends = {"Cross Open", "Square Favorite", "Options Actions", "L1/R1 Sections"};
    } else if (state_.navigation.route == Route::Library) {
        view_.title = "Library";
        const auto games = visible_games();
        view_.total_count = games.size();
        view_.subtitle = std::to_string(games.size()) + " games";
        if (state_.library_loading) view_.subtitle += live() ? " · Refreshing library…" : " · Refreshing fixture library…";
        else if (live() && state_.library_offline) view_.subtitle += " · Cached library";
        if (live() && !state_.account().error.empty()) view_.subtitle += " · " + state_.account().error;
        const std::size_t window_size = 24;
        const std::size_t selected = static_cast<std::size_t>(std::max(0, state_.navigation.selected_index));
        view_.first_index = games.size() <= window_size ? 0 : std::min(selected > window_size / 2 ? selected - window_size / 2 : 0, games.size() - window_size);
        const std::size_t end = std::min(games.size(), view_.first_index + window_size);
        for (std::size_t index = view_.first_index; index < end; ++index) {
            const auto* game = games[index];
            auto& item = row(game_key(game->id), RowKind::Game, game->title, "open-game");
            item.game_id = game->id;
            if (state_.library.favorites.contains(game->id)) item.badge = "Favorite";
            if (game->art_failed) item.subtitle = "Artwork unavailable";
        }
        // An empty grid offers the way back to the games it hides; Connect a store is only for a missing catalog.
        if (view_.rows.empty()) {
            if (!state_.library.query.empty()) row("clear-search", RowKind::Action, "Clear search", "clear-search");
            else if (!state_.library.filters.empty()) row("library-empty", RowKind::Action, "Reset filters", "filters-reset");
            else if (!state_.games.empty() && state_.library.scope != "all") row("library-empty", RowKind::Action, "Show all games", "library-show-all");
            else row("library-empty", RowKind::Action, "Connect a store", "connect");
        }
        // The footer shows five legends; with a search applied, Circle clears it in place of Square Favorite.
        view_.legends = state_.library.query.empty()
            ? std::vector<std::string>{"Cross Open", "Square Favorite", "Options Actions", "Triangle Filters", "Touchpad Search"}
            : std::vector<std::string>{"Cross Open", "Options Actions", "Triangle Filters", "Touchpad Search", "Circle Clear search"};
    } else if (state_.navigation.route == Route::Details) {
        const auto* game = find_game(state_, state_.navigation.detail_id.value_or(state_.navigation.selected_game_id));
        view_.title = game ? game->title : "Game unavailable";
        view_.subtitle = game ? game->description : "The selected fixture no longer exists.";
        Installation* installation = game ? find_installation(state_, game->id) : nullptr;
        // A game playing in place from its own folder (an imported game) has the same Play and setup
        // rows as an installed one; it is never in Installed games.
        std::optional<Installation> in_place;
        std::string in_place_reason;
        if (!installation && game)
            if (const auto* external = external_game(game->id)) {
                const auto runtime_game = state_.runtime_games.find(game->id);
                // Fixture mode: a folder there plays at once (its size stands for the install).
                in_place = Installation{game->id, external->volume_id, live() ? 0 : std::max<std::uint64_t>(game->install_bytes.value_or(1), 1),
                                        external->available && external->reason.empty(),
                                        live() ? runtime_game != state_.runtime_games.end() && runtime_game->second.ready : true, false};
                in_place_reason = external->reason;
                installation = &*in_place;
            }
        const bool imported = game && game->id.store == Store::kLocal;
        const Job* copying = imported && in_place ? import_copy_job(game->id) : nullptr;
        // Live installations are only finished installs; their size may be unknown on an absent drive.
        const bool installed = installation && (live() || installation->installed_bytes > 0);
        if (!live()) {
            auto& primary = row(installed ? "details-play" : "details-install", RowKind::Action,
                                installed ? (installation->runtime_ready ? "Play" : "Open Settings") : "Install",
                                installed ? (installation->runtime_ready ? "launch" : "details-kit-settings") : "install");
            primary.enabled = true;
        }
        if (live() && game) {
            // Real install state: an install job, a finished install, or what Steam allows.
            const auto* job = find_job(state_, ToKey(game->id));
            if (installed) {
                if (!installation->available && !in_place_reason.empty()) {
                    auto& status = row("details-installed", RowKind::Status, "Can't play from this folder", "");
                    status.subtitle = in_place_reason;
                    status.enabled = false;
                } else if (!installation->available) {
                    const auto* volume = find_volume(state_, installation->volume_id);
                    auto& status = row("details-installed", RowKind::Status, "Storage disconnected", "");
                    status.subtitle = "Connect " + (volume ? volume->label : std::string("its storage")) + " to use this game";
                    status.enabled = false;
                }
                if (installation->available) {
                    // The primary action says what pressing does; setup facts and a single
                    // player-facing hint carry its state without repeating technical diagnostics.
                    const auto play_game = state_.runtime_games.find(game->id);
                    if (play_game != state_.runtime_games.end()) {
                        const auto& value = play_game->second;
                        const bool kit_update = value.status == "Kit update available" || value.status == "Update available";
                        const bool no_kit = value.status == "No kit";
                        const bool prepares_first = !value.ready && value.can_prepare &&
                            (value.status == "Not prepared" || kit_update || value.status == "Needs repair");
                        const bool recovery = value.status == "Needs recovery" || value.status == "Failed";
                        const bool removing = value.status == "Removal incomplete";
                        const bool pending = value.status == "Registration pending";
                        const bool attention = value.can_prepare && (recovery || removing || pending);
                        const bool enabled = no_kit || value.can_play || prepares_first || value.awaiting_registration || attention;
                        auto& play = row("details-play", RowKind::Action,
                            no_kit ? "Open Settings" : removing ? "Finish removing" : pending ? "Check again"
                            : recovery || value.awaiting_registration ? "Finish setup" : "Play",
                            no_kit ? "details-kit-settings" : value.awaiting_registration ? "quit-to-home"
                            : attention ? (removing ? "runtime-remove-retry" : "runtime-prepare")
                            : enabled ? "runtime-play" : "");
                        play.value = value.awaiting_registration ? "Quits to Home"
                            : !value.play_status.empty() ? value.play_status
                            : prepares_first ? (kit_update ? "Kit update first" : value.status == "Needs repair" ? "Repairs first" : "Sets up first")
                            : value.status == "Preparing" ? value.detail
                            : value.status == "Checking" || value.status == "Removing" || value.status == "Unavailable" ? value.status
                            : std::string{};
                        play.enabled = enabled && value.uninstall_status.empty();
                        // Setting up waits for the player to choose the game's program.
                        if (const auto setup = game_setups_.find(game->id); setup != game_setups_.end() &&
                            !setup->second.steam_choices.empty() && !value.ready && value.status != "Preparing") {
                            play.title = "Choose program";
                            play.action = "steam-programs";
                            play.value.clear();
                            play.enabled = value.uninstall_status.empty();
                        }
                        // Finish setup would fail as before over a title or bottle an earlier kit made: make it again.
                        if (value.setup_other_kit) {
                            play.title = "Recreate bottle";
                            play.action = "prep-recreate-" + ToKey(game->id);
                            play.value = value.maintenance_running ? "Working…" : std::string{};
                            play.enabled = !value.maintenance_running;
                        }
                        if (!value.uninstall_status.empty()) {
                            play.value = value.uninstall_status;
                            play.action.clear();
                        }
                        if (value.queued && value.uninstall_status.empty()) {
                            play.value = "Queued";
                            play.enabled = false;
                            play.action.clear();
                        } else if (!play.enabled && value.uninstall_status.empty() && value.status != "Preparing") {
                            const auto other = std::find_if(state_.runtime_games.begin(), state_.runtime_games.end(),
                                [&](const auto& entry) { return entry.first != game->id && entry.second.status == "Preparing"; });
                            if (other != state_.runtime_games.end()) play.value = "Waiting";
                        }
                        if (value.progress) {
                            play.progress = std::max(*value.progress, 0.001F);
                            if (!play.value.empty())
                                play.value = trimmed(play.value.substr(0, play.value.find("…"))) + " " +
                                             std::to_string(static_cast<int>(*value.progress * 100.0F)) + "%";
                        }
                    }
                    // Steam Cloud saves: sync on demand; a pending choice opens its dialog. Only stores
                    // with Cloud saves have the row.
                    if (Traits(game->id.store).cloud_saves) {
                        const auto cloud = state_.cloud.find(game->id);
                        const auto cloud_status = cloud == state_.cloud.end() ? CloudStatus::None : cloud->second.status;
                        const bool busy = cloud_status == CloudStatus::Checking || cloud_status == CloudStatus::Syncing;
                        const bool choice = cloud_status == CloudStatus::Conflict || cloud_status == CloudStatus::NeedsAccount;
                        // Stays a button while it works, disabled, so it keeps its place in the row.
                        auto& saves = row("details-cloud", RowKind::Action, "Cloud saves",
                                          busy ? "" : choice ? "cloud-choose" : "cloud-sync");
                        saves.value = cloud_status == CloudStatus::None        ? "Sync"
                                      : busy                                  ? (cloud_status == CloudStatus::Checking ? "Checking…" : "Syncing…")
                                      : choice                                ? "Choose"
                                      : cloud_status == CloudStatus::Failed   ? "Retry"
                                      : cloud_status == CloudStatus::Unsupported ? "Unavailable"
                                                                              : "Synced";
                        if (cloud != state_.cloud.end()) saves.subtitle = cloud->second.message;
                        saves.enabled = !busy;
                    }
                }
            } else if (job) {
                auto& status = row("details-download", RowKind::Action, "Download", "open-downloads");
                status.value = job_state_value(*job);
                status.progress = job->total_bytes == 0 ? 0.0F : static_cast<float>(job->completed_bytes) / static_cast<float>(job->total_bytes);
            } else if (game->windows_installable.value_or(false)) {
                auto& install = row("details-install", RowKind::Action, "Install", "live-install");
                install.value = game->install_bytes ? format_bytes(*game->install_bytes) : "Size unknown";
                if (const auto* volume = find_volume(state_, selected_volume_id_)) {
                    const auto assessment = assess_install(state_, game->id, selected_volume_id_, steam::kInstallSpaceMargin);
                    install.subtitle = "To " + volume->label + (assessment.free_bytes
                        ? " · " + format_bytes(*assessment.free_bytes) + " free" : std::string());
                }
            } else if (game->windows_installable.has_value()) {
                row("details-unavailable", RowKind::Status, "No Windows version to install", "").enabled = false;
            } else {
                // Install keeps its place while Steam answers: disabled, then enabled with the size.
                auto& install = row("details-install", RowKind::Action, "Install", "");
                install.value = "…";  // narrower than any size, so the button keeps its width
                install.enabled = false;
                row("details-checking", RowKind::Status, "Checking Steam…", "").enabled = false;
            }
            // Saves kept when the game was uninstalled: Install brings them back.
            if (!installed)
                for (const auto& kept : state_.kept_bottles) {
                    if (kept.game_id != game->id) continue;
                    auto& saves = row("details-kept", RowKind::Status, "Saves from a previous install", "");
                    saves.value = format_bytes(kept.bytes);
                    saves.subtitle = "Installing the game again brings them back.";
                    saves.enabled = false;
                    row("details-kept-delete", RowKind::Action, "Delete saves", "kept-delete-" + ToKey(game->id)).dangerous = true;
                }
        }
        if (imported) {
            // An imported game: Import into Playden while it plays from its folder (the copy's
            // progress while it runs), then Remove from library; a copy in Playden uninstalls.
            const auto starting = copying || !in_place ? import_transfers_starting_.end()
                                                       : import_transfers_starting_.find(game->id);
            if (starting != import_transfers_starting_.end()) {
                // Asked for, not listed yet (the copy is planned first): disabled, like Install while Steam answers.
                auto& status = row("details-download", RowKind::Action,
                                   starting->second == ImportRequestKind::MoveIntoPlayden ? "Moving" : "Copying", "");
                status.value = "…";
                status.enabled = false;
            } else if (copying) {
                auto& status = row("details-download", RowKind::Action, "Copying", "import-open-copy");
                status.value = job_state_value(*copying);
                status.progress = copying->total_bytes == 0 ? 0.0F
                    : static_cast<float>(copying->completed_bytes) / static_cast<float>(copying->total_bytes);
                // A copy that failed (or runs too long) can be cancelled here; the game plays in place again.
                row("details-cancel-copy", RowKind::Action, "Cancel copy", "import-cancel-copy");
            } else if (const auto* external = in_place ? external_game(game->id) : nullptr; external && external->available) {
                // Its drive is there: a folder the PS5 can't play from (its name) can still be copied.
                row("details-copy", RowKind::Action, "Import into Playden",
                    "import-copy-game");
            }
            if (in_place) {
                row("details-remove", RowKind::Action, "Remove from library", "import-remove").enabled =
                    !copying && starting == import_transfers_starting_.end();
                // Where it plays from, under the buttons.
                for (const auto& candidate : state_.import_candidates)
                    if (candidate.imported == game->id && installation->available) {
                        auto& folder = row("details-folder", RowKind::Status, "Plays from " + candidate.path, "");
                        folder.enabled = false;
                    }
            } else if (installed) {
                row("details-uninstall", RowKind::Action, "Uninstall", "uninstall");
            }
        }
        if (installed) {
            const auto found = state_.runtime_games.find(game->id);
            const auto* runtime = found == state_.runtime_games.end() ? nullptr : &found->second;
            const std::string status = !installation->available ? "Unavailable"
                : runtime && !runtime->uninstall_status.empty() ? "Removing"
                : runtime ? runtime->status : !live() ? (installation->runtime_ready ? "Ready" : "No kit") : "Checking";
            const bool waiting = runtime && runtime->awaiting_registration && installation->available && runtime->uninstall_status.empty();
            const bool kit_update = status == "Kit update available" || status == "Update available";
            const std::string tone = waiting || kit_update ? "busy" : status == "Ready" ? "good"
                : status == "Failed" || status == "No kit" ? "problem"
                : status == "Needs repair" || status == "Needs recovery" || status == "Registration pending" ||
                  status == "Removal incomplete" ? "attention" : "quiet";
            // The game right ahead of this one in the preparation queue (the one being set up now when
            // this one is next), and where this one waits.
            std::string ahead;
            const auto& queue = state_.preparation_queue;
            const auto place = std::find(queue.waiting.begin(), queue.waiting.end(), game->id);
            const auto position = place == queue.waiting.end() ? 0 : static_cast<std::size_t>(place - queue.waiting.begin()) + 1;
            if (runtime && runtime->queued) {
                if (position > 1) {
                    if (const auto* other_game = find_game(state_, queue.waiting[position - 2])) ahead = other_game->title;
                } else {
                    for (const auto& [other_id, other] : state_.runtime_games)
                        if (other_id != game->id && other.status == "Preparing")
                            if (const auto* other_game = find_game(state_, other_id)) ahead = other_game->title;
                }
            }
            auto& setup = row("details-setup", RowKind::Status, "Setup", "");
            setup.value = waiting ? "Waiting for the PS5" : kit_update ? "Kit update available"
                : status == "Not prepared" ? "Not set up" : status == "Needs recovery" ? "Interrupted"
                : status == "Registration pending" ? "Not confirmed" : status == "Removal incomplete" ? "Removing"
                : status == "Preparing" && runtime && !runtime->detail.empty() ? runtime->detail : status;
            setup.tone = tone;
            setup.enabled = false;
            if (runtime && runtime->queued && runtime->uninstall_status.empty()) {
                setup.value = ahead.empty() ? "Queued" : "Waiting for " + ahead;
                setup.tone = "busy";
            }
            std::string hint;
            if (runtime && runtime->queued && runtime->uninstall_status.empty()) {
                hint = ahead.empty() ? "Setup starts when the game being set up is ready."
                                     : "Setup starts when " + ahead + " is ready.";
                if (position > 0 && queue.waiting.size() > 1)
                    hint = std::to_string(position) + " of " + std::to_string(queue.waiting.size()) + " waiting. " + hint;
            }
            else if (waiting && imported) hint = "The PS5 adds the game to Home when Playden quits.";
            else if (waiting) hint = "Quit Playden and " + game->title + " appears on the PS5 Home screen.";
            else if (status == "Registration pending") hint = "The PS5 hasn't confirmed " + game->title + " yet.";
            else if (status == "Removal incomplete") hint = "Removing " + game->title + " from the PS5 Home screen stopped partway.";
            else if (status == "Needs repair") hint = "Some of " + game->title + "'s setup files changed.";
            else if (status == "No kit") hint = "Playden needs its runtime kit to set up games.";
            else if (runtime && runtime->setup_other_kit)
                hint = "Setup started with an earlier kit. Recreate bottle makes it again; saves and settings are kept.";
            else if (runtime && (status == "Needs recovery" || status == "Failed")) {
                const bool stopped = status == "Needs recovery";
                if (runtime->failure_code.rfind("kit-", 0) == 0 ||
                    runtime->detail == "kit source is missing, linked, or has wrong mode")
                    hint = "The runtime kit is damaged. Reinstall it.";
                else if (runtime->failure_code == "space-unavailable" ||
                         runtime->detail == "insufficient verified free space for package copy")
                    hint = stopped ? "Setup stopped early: console storage was full." : "Setup failed: console storage was full.";
                else hint = stopped ? "Setup stopped early." : "Setup failed.";
            }
            if (!hint.empty()) {
                auto& note = row("details-hint", RowKind::Status, hint, "");
                note.tone = setup.tone;
                note.enabled = false;
            }
            // Waiting behind other games: move it to the front; the game being set up now finishes first.
            if (runtime && runtime->queued && runtime->uninstall_status.empty() && position > 1)
                row("details-prepare-next", RowKind::Action, "Prepare next", "details-prepare-next").game_id = game->id;
        }
        row("details-favorite", RowKind::Toggle, "Favorite", "favorite").selected = game && state_.library.favorites.contains(game->id);
        if (!live()) row("details-simulate-launch", RowKind::Action, "Simulate launch", "launch");
        row("details-more", RowKind::Action, "More", "context");
        view_.legends = {"Cross Select", "Options Actions", "Circle Back"};
    } else if (state_.navigation.route == Route::Downloads) {
        // Queue: Downloads, then Preparation, one expanded at a time, then the drives. Rows come in screen
        // order (heading, card, the expanded section's rows), so Up and Down follow what is on screen.
        view_.title = "Queue";
        // Each visit starts from the automatic choice (or the focused row's section); then the open section
        // stays open while focus moves and the queues change, until the player opens the other one.
        if (!queue_shown_) state_.queue_expanded.clear();
        queue_shown_ = true;
        const auto layout = BuildQueueLayout(state_);
        view_.subtitle = layout.summary;
        if (layout.downloads.expanded) state_.queue_expanded = kQueueDownloads;
        else if (layout.preparation.expanded) state_.queue_expanded = kQueuePreparation;
        const auto entry_row = [&](const QueueEntry& entry, bool card) {
            if (entry.job) {
                const auto& job = *entry.job;
                auto& item = row(entry.key, RowKind::Job, entry.title, "job-action");
                item.game_id = job.game_id;
                item.subtitle = job.failure;
                item.value = !job.state_text.empty() ? job.state_text : job_state_value(job);
                item.progress = job.total_bytes == 0 ? 0.0F : static_cast<float>(job.completed_bytes) / static_cast<float>(job.total_bytes);
                return;
            }
            auto& item = row(entry.key, RowKind::Job, entry.title, "prep-action");
            item.game_id = entry.game_id;
            item.subtitle = entry.note;
            item.value = entry.status;
            item.tone = entry.tone;
            item.section = kQueuePreparation;
            if (card) item.badge = "card";
        };
        for (const auto* section : {&layout.downloads, &layout.preparation}) {
            if (section->empty()) continue;
            auto& heading = row(std::string("queue-") + section->id, RowKind::Heading, section->name, "queue-expand");
            heading.value = section->summary;
            heading.selected = section->expanded;
            heading.section = section->id;
            if (section->active) entry_row(*section->active, true);
            if (section->expanded)
                for (const auto& entry : section->rows) entry_row(entry, false);
        }
        for (const auto& volume : state_.volumes) {
            auto& drive = row("storage-" + volume.id, RowKind::Volume, volume.label, "storage-details");
            drive.section = "storage";
        }
        view_.legends = {"Cross Select", "Options Actions", "Circle Back", "L1/R1 Sections"};
    } else if (state_.navigation.route == Route::Saves) {
        view_.title = "Saves";
        const auto all = save_games(false);
        const auto installed = [this](GameId id) {
            return std::any_of(state_.installations.begin(), state_.installations.end(),
                               [id](const Installation& item) { return item.game_id == id; });
        };
        const auto cloud_of = [this](GameId id) -> const LiveCloudGame* {
            const auto found = state_.cloud.find(id);
            return found == state_.cloud.end() ? nullptr : &found->second;
        };
        std::array<std::size_t, kSavesFilters.size()> counts{};
        std::uint64_t total = 0;
        for (const auto id : all) {
            const auto facts = save_facts(cloud_of(id), installed(id));
            ++counts[0];
            counts[1] += facts.needs_you;
            counts[2] += facts.in_sync;
            counts[3] += facts.local_only;
            counts[4] += !installed(id);
            if (const auto* cloud = cloud_of(id); cloud && cloud->status != CloudStatus::Unsupported) total += cloud->local_bytes;
            else if (const auto size = state_.save_sizes.find(id); size != state_.save_sizes.end()) total += size->second;
        }
        const auto& job = state_.saves_job;
        if (job.kind != SavesJobKind::None) {
            const auto* current = find_game(state_, job.current);
            view_.subtitle = std::string(job.stopping ? "Stopping after this game" : job.kind == SavesJobKind::SyncAll ? "Syncing" : "Checking") +
                             " " + std::to_string(std::min(job.done + 1, job.total)) + " of " + std::to_string(job.total) +
                             (job.kind == SavesJobKind::QuickCheck ? " games that may have changed" : "") +
                             (current ? " · " + current->title : std::string());
        } else {
            view_.subtitle = std::to_string(all.size()) + (all.size() == 1 ? " game · " : " games · ") + save_size(total) + " of saves";
        }
        for (std::size_t index = 0; index < kSavesFilters.size(); ++index) {
            auto& chip = row("saves-filter-" + std::to_string(index), RowKind::Choice, kSavesFilters[index], "saves-filter");
            chip.value = std::to_string(counts[index]);
            chip.selected = static_cast<int>(index) == state_.navigation.saves_filter;
        }
        if (const auto awaiting = saves_awaiting_account(); job.kind == SavesJobKind::None && !awaiting.empty())
            row("saves-upload-all", RowKind::Action, "Upload all", "saves-upload-all").value = std::to_string(awaiting.size());
        auto& sync_all = row("saves-sync-all", RowKind::Action, job.kind == SavesJobKind::None ? "Sync all" : "Stop", "saves-sync-all");
        sync_all.selected = job.kind != SavesJobKind::None;
        sync_all.enabled = !job.stopping && (job.kind != SavesJobKind::None || counts[0] > counts[3]);
        if (job.kind != SavesJobKind::None && job.total > 0) {
            float done = static_cast<float>(job.done);
            if (const auto* cloud = cloud_of(job.current); cloud && cloud->total_bytes > 0)
                done += static_cast<float>(cloud->moved_bytes) / static_cast<float>(cloud->total_bytes);
            sync_all.progress = std::clamp(done / static_cast<float>(job.total), 0.0F, 1.0F);
        }
        for (const auto id : save_games(true)) {
            const auto* game = find_game(state_, id);
            const auto* cloud = cloud_of(id);
            const bool here = installed(id);
            const auto facts = save_facts(cloud, here);
            std::string name = game ? game->title : std::string();
            if (name.empty())
                for (const auto& kept : state_.kept_bottles) if (kept.game_id == id) name = kept_name(state_, kept);
            auto& item = row("save-" + ToKey(id), RowKind::Action, name.empty() ? "Steam app " + ToKey(id) : name, "save-open");
            item.game_id = id;
            item.subtitle = facts.reason;
            item.value = facts.label;
            item.tone = facts.tone;
            if (cloud && cloud->status == CloudStatus::Syncing && cloud->total_bytes > 0)
                item.progress = std::max(0.001F, static_cast<float>(cloud->moved_bytes) / static_cast<float>(cloud->total_bytes));
            // A Cloud game's saves are the files its check found; a game without Steam Cloud has only its
            // Windows user folders to measure.
            if (cloud && cloud->status != CloudStatus::Unsupported) {
                if (cloud->local_bytes > 0) item.aside = save_size(cloud->local_bytes);
            } else if (const auto size = state_.save_sizes.find(id); size != state_.save_sizes.end() && size->second > 0) {
                item.aside = save_size(size->second);
            }
            const auto now = state_.clock_ticks;
            if (cloud && cloud->after_play) item.badge = "";
            else if (cloud && cloud->status == CloudStatus::Failed) item.badge = cloud->last_sync > 0 ? "Last sync " + time_ago(now, cloud->last_sync) : "";
            else if (cloud && cloud->last_sync > 0) item.badge = "Synced " + time_ago(now, cloud->last_sync);
            else if (cloud && cloud->status == CloudStatus::Conflict && cloud->first_sync) item.badge = "Never synced";
        }
        if (view_.rows.size() == kSavesFilters.size() + 1)
            row("saves-empty", RowKind::Status, all.empty() ? "No saves on this console yet" : "Nothing here", "").enabled = false;
        view_.legends = {"Cross Open", job.kind == SavesJobKind::None ? "Triangle Sync all" : "Triangle Stop", "Circle Back", "L1/R1 Sections"};
        if (job.kind == SavesJobKind::None && !saves_awaiting_account().empty())
            view_.legends.insert(view_.legends.begin() + 2, "Square Upload all");
    } else if (state_.navigation.route == Route::SaveDetails) {
        const auto id = state_.navigation.save_id;
        const auto* game = find_game(state_, id);
        const LiveKeptBottle* kept = nullptr;
        for (const auto& item : state_.kept_bottles) if (item.game_id == id) kept = &item;
        const bool here = std::any_of(state_.installations.begin(), state_.installations.end(),
                                      [id](const Installation& item) { return item.game_id == id; });
        view_.title = game ? game->title : kept ? kept_name(state_, *kept) : "Steam app " + ToKey(id);
        const auto found = state_.cloud.find(id);
        const auto* cloud = found == state_.cloud.end() ? nullptr : &found->second;
        const auto facts = save_facts(cloud, here);
        view_.subtitle = facts.label;
        view_.tone = facts.tone;
        const auto status = cloud ? cloud->status : CloudStatus::None;
        const auto day = [this](std::int64_t when) { return when > 0 ? time_ago(state_.clock_ticks, when) : std::string(); };
        const auto add = [&](std::string key, std::string title, std::string value = {}) -> ViewRow& {
            auto& added = row(key, RowKind::Action, std::move(title), key);
            added.value = std::move(value);
            added.game_id = id;
            added.enabled = !facts.busy;
            return added;
        };
        if (status == CloudStatus::Conflict) {
            add("saves-use-cloud", "Use Cloud saves", cloud->remote_latest > 0 ? "newest " + day(cloud->remote_latest) : "");
            add("saves-use-local", "Use this console's", cloud->local_latest > 0 ? "newest " + day(cloud->local_latest) : "");
        } else if (status == CloudStatus::NeedsAccount) {
            add("saves-confirm-account", "Use this account");
        } else if (status != CloudStatus::Unsupported) {
            add("saves-sync", status == CloudStatus::Failed ? "Try again" : "Sync now",
                status == CloudStatus::Waiting ? (here ? "downloads Cloud saves" : "into the kept data") : "");
        }
        if (!here && kept) {
            add("saves-leftover", "Remove leftover data", format_bytes(kept->bytes) + " with saves").dangerous = true;
        } else if (here && status != CloudStatus::Unsupported && status != CloudStatus::None) {
            add("saves-delete", "Delete saves on this console", "Cloud keeps its copy").dangerous = true;
        }
        if (view_.rows.empty()) row("saves-nothing", RowKind::Status, "Nothing to do", "").enabled = false;
        view_.legends = {"Cross Select", "Circle Back"};
    } else if (state_.navigation.route == Route::Settings) {
        view_.title = "Settings";
        if (live()) view_.subtitle = state_.account().error;
        // Rows come grouped by the macOS sections; the rail shows the focused row's section.
        std::string section = "Account";
        const auto add = [&](std::string key, RowKind kind, std::string title, std::string action) -> ViewRow& {
            auto& added = row(std::move(key), kind, std::move(title), std::move(action));
            added.section = section;
            return added;
        };
        auto& account = add("settings-account", RowKind::Action, "Account", "account");
        account.value = state_.account().display_name;
        account.subtitle = state_.account().authentication == AuthState::Offline ? "Cached library · Offline"
            : state_.account().authentication == AuthState::SignedIn
                ? (!live() || state_.account().last_sync > 0 ? "Steam connected · Synced" : "Steam connected")
                : "Connect Steam";
        if (state_.account().authentication != AuthState::SignedOut)
            add("settings-signout", RowKind::Action, "Sign out", "signout-confirm").dangerous = true;
        section = "Library";
        {
            // Windows games in a drive's playden/import or the console drop folder: the Import sheet.
            auto& entry = add("settings-import", RowKind::Action, "Import games", "import-manage");
            entry.subtitle = "Add Windows games from a drive or the console drop folder";
            const auto found = std::count_if(state_.import_candidates.begin(), state_.import_candidates.end(),
                                             [](const LiveImportCandidate& candidate) { return !candidate.imported.valid(); });
            if (found > 0) entry.value = std::to_string(found) + " found";
        }
        if (live()) {
            // Steam ships localisations as separate depots, so the language has to be chosen
            // before a game is downloaded.
            auto& content = add("settings-content-language", RowKind::Action, "Game language", "content-language-picker");
            content.value = playden::steam::LanguageName(state_.content_language);
            content.subtitle = "Which language new downloads are in, when the game has it. Games already installed keep theirs.";
            auto& suffix = add("settings-title-store", RowKind::Toggle, "Store name in PS5 titles", "toggle-title-store");
            suffix.selected = state_.title_store_suffix;
            suffix.subtitle = "Name game titles like \"Half-Life 2 (Steam)\". Prepared games change with their next Update.";
            auto& cloud = add("settings-cloud-saves", RowKind::Toggle, "Sync Cloud saves automatically", "toggle-cloud-saves");
            cloud.selected = state_.cloud_saves_auto;
            cloud.subtitle = state_.cloud_saves_auto ? "Before Play, after each game and when Playden starts."
                                                     : "Off: saves move only when you choose Sync in Saves.";
        }
        section = "Storage";
        auto& storage = add("settings-storage", RowKind::Action, "Library and storage", "storage-picker");
        if (live()) {
            const auto* volume = find_volume(state_, selected_volume_id_);
            storage.title = "Games storage";
            storage.value = volume ? volume->label : "";
            storage.subtitle = "Where new games are installed";
            if (state_.volumes.empty()) { storage.kind = RowKind::Status; storage.action.clear(); storage.enabled = false; storage.subtitle = "Storage is unavailable"; }
        }
        if (live()) {
            // Installed games: one row that opens the list, sorted by size or last played, where
            // the player picks games to uninstall together.
            auto& installed = add("settings-installed", RowKind::Action, "Installed games", "installed-manage");
            installed.subtitle = "Uninstall games you no longer play to free up space";
            if (state_.installations.empty()) {
                installed.kind = RowKind::Status; installed.action.clear(); installed.enabled = false; installed.value = "None";
            } else {
                std::uint64_t bytes = 0;
                for (const auto& item : state_.installations) bytes += item.installed_bytes;
                installed.value = kept_count(state_.installations.size()) + " · " + format_bytes(bytes);
            }
        }
        if (live()) {
            // Saves and settings kept from uninstalled games: one row that opens the manager,
            // where the player picks which to delete to reclaim their space.
            auto& kept = add("settings-kept", RowKind::Action, "Kept game data", "kept-manage");
            kept.subtitle = "Saves and settings kept when a game is uninstalled";
            if (state_.kept_bottles.empty()) {
                kept.kind = RowKind::Status; kept.action.clear(); kept.enabled = false; kept.value = "None";
            } else {
                std::uint64_t bytes = 0;
                for (const auto& item : state_.kept_bottles) bytes += item.bytes;
                kept.value = kept_count(state_.kept_bottles.size()) + " · " + format_bytes(bytes);
            }
        }
        if (live() && state_.runtime_leftover_requests > 0) {
            // Explicit recovery: settings requests no game is using any more.
            auto& leftovers = add("settings-runtime-cleanup", RowKind::Action, "Unused game settings data", "runtime-cleanup");
            leftovers.value = std::to_string(state_.runtime_leftover_requests);
            leftovers.subtitle = "Left by interrupted or finished preparations. Clean up removes only what no game is using.";
        }
        auto& runtime = add("settings-runtime", RowKind::Action, "Runtime status", "settings-runtime");
        runtime.value = state_.runtime == RuntimeState::Ready ? (live() ? "Ready" : "Ready — simulated") : state_.runtime == RuntimeState::Checking ? (live() ? "Checking" : "Checking — simulated") : state_.runtime == RuntimeState::Failed ? (live() ? "Failed" : "Failed — simulated") : (live() ? "Unavailable" : "Unavailable — simulated");
        if (live()) {
            runtime.subtitle = state_.runtime_problem.empty() ? "Games run through prospero-win" : state_.runtime_problem;
            runtime.kind = RowKind::Status; runtime.action.clear(); runtime.enabled = false;
        }
        if (live()) {
            auto& slow = add("settings-downloads-slow", RowKind::Toggle, "Downloads for slow networks", "toggle-downloads-slow");
            slow.selected = state_.downloads_slow_network;
            slow.subtitle = state_.downloads_slow_network
                ? "8 pieces at a time. Turn it off if downloads got slower."
                : "Off: 4 pieces at a time. Turn it on if downloads stay near 1 MB/s.";
        }
        section = "Display";
        auto& motion = add("settings-motion", RowKind::Toggle, "Reduced motion", "toggle-motion");
        motion.selected = state_.reduced_motion;
        motion.subtitle = "Keep the focus ring; turn off scaling and transitions";
        // The controller test is hidden on the console until it reads the D-pad and sticks correctly
        // (Gitea issue #31).
        if (!live()) {
            section = "Controller";
            auto& controller = add("settings-controller", RowKind::Action, "Controller", "controller-test");
            controller.value = state_.controller_connected ? "Connected" : "Disconnected";
            controller.subtitle = "Test the buttons and sticks";
        }
        section = "Updates";
        if (live()) {
            auto& beta = add("settings-beta-updates", RowKind::Toggle, "Beta updates", "toggle-beta-updates");
            beta.selected = state_.beta_updates;
            beta.subtitle = "Get test versions before they are released. They can have new bugs.";
            const auto& update = state_.self_update;
            auto& item = add("settings-update", RowKind::Action, "Playden updates", "self-update");
            if (update.phase == "checking") {
                item.value = "Checking…";
                item.subtitle = "Looking for a newer Playden on GitHub";
            } else if (update.phase == "available") {
                item.value = update.moves && update.available == update.current ? "New app available"
                                                                                  : update.available + " available";
                item.subtitle = (update.moves ? "Installs as a new app · download " : "Download ") +
                                format_bytes(update.download_bytes);
            } else if (update.phase == "downloading" || update.phase == "unpacking") {
                item.value = update.total ? std::to_string(update.done * 100 / update.total) + "%" : "Working…";
                item.subtitle = update.phase == "downloading" ? "Downloading Playden " + update.available
                                                               : "Unpacking Playden " + update.available;
            } else if (update.phase == "staged") {
                item.value = update.available + " ready";
                item.subtitle = "Restart Playden to finish the update";
            } else if (update.phase == "failed") {
                item.value = "Check again";
                item.subtitle = update.error;
            } else {
                item.value = "Check now";
                item.subtitle = update.phase == "up-to-date" ? "No newer update for version " + update.current
                                                              : "Version " + update.current;
            }
        }
        if (live() && !state_.beta_updates && state_.self_update.current.find("-beta.") != std::string::npos) {
            auto& note = add("settings-beta-wait", RowKind::Status, "You have a beta", "");
            note.subtitle = "Playden moves to the next stable version when it is out.";
            note.enabled = false;
        }
        // The kit-only install, while it has something to offer or say.
        if (live()) {
            const auto& update = state_.self_update;
            const auto& phase = update.kit_phase;
            // An update on offer or under way brings the kit itself: its own row says so.
            const bool update_brings_kit = update.phase == "available" || update.phase == "downloading" ||
                                           update.phase == "unpacking" || update.phase == "staged";
            if ((phase == "available" && !update_brings_kit) || phase == "checking" || phase == "downloading" ||
                phase == "unpacking" || phase == "failed") {
                auto& kit = add("settings-kit", RowKind::Action, "Runtime kit", "kit");
                if (phase == "available") {
                    kit.value = "Download";
                    kit.subtitle = update.kit_replaces ? "The installed kit isn't the one this version was made for"
                                                       : "Needed to prepare and play games";
                } else if (phase == "checking") {
                    kit.value = "Checking…";
                    kit.subtitle = "Looking for this version's kit on GitHub";
                } else if (phase == "failed") {
                    kit.value = "Try again";
                    kit.subtitle = update.kit_error;
                } else {
                    kit.value = update.kit_total ? std::to_string(update.kit_done * 100 / update.kit_total) + "%" : "Working…";
                    kit.subtitle = phase == "downloading" ? "Downloading the runtime kit" : "Unpacking the runtime kit";
                }
            }
        }
        if (kOfferGoBack && live() && !state_.self_update.previous.empty() &&
            state_.self_update.phase != "downloading" && state_.self_update.phase != "unpacking") {
            auto& back = add("settings-update-back", RowKind::Action, "Go back to Playden " + state_.self_update.previous,
                             "self-update-back");
            back.subtitle = "Put the previous version and its runtime kit back";
        }
        if (live() && !state_.self_update.current_notes_url.empty()) {
            auto& notes = add("settings-release-notes", RowKind::Action, "Release notes", "self-update-notes");
            notes.subtitle = "What changed in Playden " + state_.self_update.current;
        }
        section = "About";
        add("settings-about", RowKind::Action, "About", "about").subtitle = "Playden for PlayStation 5";
        auto& logs = add("settings-logs", RowKind::Action, "Logs", live() ? "logs-pick" : "logs");
        if (live()) {
            logs.subtitle = "Install, Cloud saves, sign-in and launch diagnostics";
            if (log_directory_.empty()) { logs.kind = RowKind::Status; logs.action.clear(); logs.enabled = false; logs.subtitle = "No logs on this device"; }
        }
        if (!state_.notices.empty()) add("settings-notices", RowKind::Action, "Warnings", "notices").value = std::to_string(state_.notices.size());
        if (!live()) {
            add("settings-simulation", RowKind::Action, "Simulation controls", "simulation-controls");
            add("settings-scenarios", RowKind::Action, "Fixture scenarios", "scenario-picker");
        }
        auto& reset = add("settings-reset", RowKind::Action, "Reset UI data", "reset-confirm");
        reset.dangerous = true;
        reset.subtitle = "Forget favorites, collections and view state on this console";
        section = "Quit Playden";
        // As on macOS, the rail entry itself quits: the section has no rows to review.
        auto& quit = add("settings-quit", RowKind::Action, "Quit Playden", "quit-confirm");
        quit.dangerous = true;
        quit.rail_only = true;
    } else if (state_.navigation.route == Route::Session) {
        view_.title = state_.session == SessionState::Running ? "Game running" : "Simulated session";
        view_.subtitle = state_.session_error;
        if (state_.session == SessionState::Running) {
            row("session-return", RowKind::Action, "Return to game", "return-game");
            row("session-quit", RowKind::Action, "Quit game", "quit-game");
        } else {
            if (state_.session_error == "Game did not stop") row("session-force-stop", RowKind::Action, "Force stop", "force-stop").dangerous = true;
            else row("session-retry", RowKind::Action, "Retry", "launch");
            row("session-logs", RowKind::Action, "Open logs", "logs");
            row("session-back", RowKind::Action, "Back to details", "details-back");
        }
        row("session-fixture-controls", RowKind::Action, "Session fixture controls", "session-fixture-controls");
    }
    // Live Details opens on Favorite while Steam's app info is still loading; once it arrives
    // focus moves to the first action (Install, the download, or Cloud saves), as on macOS
    // where the primary action is focused. An installed game waits for Play: while the runtime
    // is still checking, Play is absent or disabled, and focus moves to it once it is enabled.
    // Only until the user moves.
    if (details_primary_pending_ && state_.navigation.route == Route::Details && !has_overlay) {
        // Never a destructive action (Delete saves): a second Cross must not land on it.
        const auto first = std::find_if(view_.rows.begin(), view_.rows.end(), [](const ViewRow& item) {
            return item.enabled && !item.dangerous && (item.kind == RowKind::Action || item.kind == RowKind::Toggle);
        });
        if (first != view_.rows.end()) state_.navigation.focus_key = first->key;
        const auto has = [this](const char* key) {
            return std::any_of(view_.rows.begin(), view_.rows.end(), [key](const ViewRow& item) { return item.key == key; });
        };
        const auto play = std::find_if(view_.rows.begin(), view_.rows.end(), [](const ViewRow& item) { return item.key == "details-play"; });
        const bool awaiting_play = has("details-setup") && (play == view_.rows.end() || !play->enabled);
        if (!has("details-checking") && !awaiting_play) details_primary_pending_ = false;
    }
    auto focused = std::find_if(view_.rows.begin(), view_.rows.end(), [this](const ViewRow& item) { return item.key == state_.navigation.focus_key; });
    // Details: Install becomes Download, and Download becomes Play; focus follows the primary action.
    if (focused == view_.rows.end() && !has_overlay && state_.navigation.route == Route::Details &&
        (state_.navigation.focus_key == "details-install" || state_.navigation.focus_key == "details-download")) {
        focused = std::find_if(view_.rows.begin(), view_.rows.end(), [](const ViewRow& item) {
            return item.key == "details-install" || item.key == "details-download" || item.key == "details-play";
        });
        if (focused != view_.rows.end()) state_.navigation.focus_key = focused->key;
    }
    // Queue: the focused row went away (dismissed, started, or its section closed). Focus the nearest
    // row left in its section, else that section's heading, so focus never jumps across the screen.
    if (focused == view_.rows.end() && !has_overlay && state_.navigation.route == Route::Downloads && !view_.rows.empty()) {
        const auto section = QueueSectionOf(state_.navigation.focus_key);
        const int last = static_cast<int>(view_.rows.size()) - 1;
        const int origin = std::clamp(queue_focus_index_, 0, last);
        int best = -1;
        for (int distance = 0; distance <= last && best < 0 && !section.empty(); ++distance)
            for (const int candidate : {origin + distance, origin - distance}) {
                if (candidate < 0 || candidate > last) continue;
                const auto& key = view_.rows[static_cast<std::size_t>(candidate)].key;
                if (QueueSectionOf(key) == section && key.rfind("queue-", 0) != 0) { best = candidate; break; }
            }
        if (best < 0)
            for (int index = 0; index <= last; ++index)
                if (view_.rows[static_cast<std::size_t>(index)].key == "queue-" + section) best = index;
        if (best >= 0) {
            state_.navigation.selected_index = best;
            state_.navigation.focus_key = view_.rows[static_cast<std::size_t>(best)].key;
            focused = view_.rows.begin() + best;
        }
    }
    // Settings: the focused row went away (a dialog deleted it, or its data changed). Focus
    // the nearest row left in the same section, never the rail-only Quit entry.
    if (focused == view_.rows.end() && !has_overlay && state_.navigation.route == Route::Settings &&
        !state_.navigation.settings_rail_focused && !settings_focus_section_.empty() && !view_.rows.empty() &&
        state_.navigation.focus_key.rfind("settings-", 0) == 0) {
        const int last = static_cast<int>(view_.rows.size()) - 1;
        const int origin = std::clamp(settings_focus_index_, 0, last);
        int best = -1;
        // A row that can be chosen first, then any row of the same section.
        for (const bool need_enabled : {true, false})
            for (int distance = 0; distance <= last && best < 0; ++distance)
                for (const int candidate : {origin - distance, origin + distance}) {
                    if (candidate < 0 || candidate > last) continue;
                    const auto& item = view_.rows[static_cast<std::size_t>(candidate)];
                    if (item.section == settings_focus_section_ && !item.rail_only && (item.enabled || !need_enabled)) {
                        best = candidate;
                        break;
                    }
                }
        for (int distance = 0; distance <= last && best < 0; ++distance)
            for (const int candidate : {origin - distance, origin + distance})
                if (candidate >= 0 && candidate <= last && !view_.rows[static_cast<std::size_t>(candidate)].rail_only) { best = candidate; break; }
        if (best >= 0) {
            state_.navigation.selected_index = best;
            state_.navigation.focus_key = view_.rows[static_cast<std::size_t>(best)].key;
            focused = view_.rows.begin() + best;
        }
    }
    if (focused != view_.rows.end()) view_.focused_row = static_cast<int>(std::distance(view_.rows.begin(), focused));
    else if (!view_.rows.empty() && !(state_.navigation.route == Route::Library && state_.navigation.library_rail_focused)
             && !(state_.navigation.route == Route::Home && !view_.shelves.empty())) {
        view_.focused_row = std::clamp(state_.navigation.selected_index, 0, static_cast<int>(view_.rows.size()) - 1);
        if (state_.navigation.route == Route::Library && view_.first_index > 0)
            view_.focused_row = std::clamp(state_.navigation.selected_index - static_cast<int>(view_.first_index), 0, static_cast<int>(view_.rows.size()) - 1);
        state_.navigation.focus_key = view_.rows[static_cast<std::size_t>(view_.focused_row)].key;
        if (!has_overlay && view_.rows[static_cast<std::size_t>(view_.focused_row)].game_id.valid())
            state_.navigation.selected_game_id = view_.rows[static_cast<std::size_t>(view_.focused_row)].game_id;
    }
    if (!has_overlay && state_.navigation.route == Route::Downloads && focused != view_.rows.end())
        queue_focus_index_ = view_.focused_row;
    if (!has_overlay && state_.navigation.route == Route::Settings && view_.focused_row >= 0 &&
        static_cast<std::size_t>(view_.focused_row) < view_.rows.size()) {
        settings_focus_index_ = view_.focused_row;
        settings_focus_section_ = view_.rows[static_cast<std::size_t>(view_.focused_row)].section;
    }
    if (!has_overlay) route_view_ = view_;
}

void Simulation::open_modal(ModalEntry modal) {
    if (!state_.navigation.modals.empty()) modal.opener_focus_key = state_.navigation.modals.back().action_keys[static_cast<std::size_t>(state_.navigation.modals.back().focused_action)];
    state_.navigation.modals.push_back(std::move(modal));
    state_.navigation.focus_key = state_.navigation.modals.back().action_keys[static_cast<std::size_t>(state_.navigation.modals.back().focused_action)];
}

void Simulation::close_modal() {
    if (state_.navigation.modals.empty()) return;
    const auto opener = state_.navigation.modals.back().opener_focus_key;
    // Step 2 opened from a game's page (no step 1 under it) was for that game only.
    if (state_.navigation.modals.back().id == "import-storage" &&
        (state_.navigation.modals.size() < 2 || state_.navigation.modals[state_.navigation.modals.size() - 2].id != "import-choice"))
        import_choice_game_ = {};
    state_.navigation.modals.pop_back();
    state_.navigation.focus_key = opener;
}

void Simulation::start_logs() {
    state_.log_state = LogState::Loading;
    state_.log_lines.clear();
    state_.log_offset = 0;
    if (state_.navigation.route == Route::Session || state_.session != SessionState::Idle)
        state_.log_context = "Session fixture · game " + ToKey(state_.session_game_id);
    else if (state_.navigation.route == Route::Downloads)
        state_.log_context = "Transfer fixture · game " + ToKey(state_.navigation.selected_game_id);
    else if (state_.navigation.selected_game_id.valid())
        state_.log_context = "Game fixture · " + ToKey(state_.navigation.selected_game_id);
    else state_.log_context = "Playden fixture diagnostics";
}

void Simulation::SetLogDirectory(std::filesystem::path directory) {
    log_directory_ = std::move(directory);
    rebuild_view();
}

void Simulation::open_live_log(const std::string& topic, GameId game_id) {
    live_log_topic_ = topic;
    live_log_game_ = game_id;
    std::vector<std::string> actions;
    if (game_id.valid())
        if (const auto* job = find_job(state_, ToKey(game_id)); job && job->state == JobState::Failed)
            actions.push_back("job-retry-" + job->id);
    actions.push_back("logs-refresh");
    actions.push_back("logs-back");
    const std::string title = topic == "auth" ? "Sign-in log" : topic == "launch" ? "Launch log"
                              : topic == "cloud"                       ? "Cloud saves log"
                                                                       : "Install log";
    load_live_log();
    open_modal({"logs", title, state_.navigation.focus_key, std::move(actions), 0, false});
}

// Reads the newest lines of the chosen log; a game's view keeps only that game's install lines.
void Simulation::load_live_log() {
    state_.log_lines.clear();
    state_.log_offset = 0;
    if (log_directory_.empty() || live_log_topic_.empty()) {
        state_.log_state = LogState::Failed;
        state_.log_context = "Logs are not available on this device";
        return;
    }
    const auto tail = playden::app::ReadLogTail((log_directory_ / (live_log_topic_ + ".log")).string());
    if (tail.read_error) {
        state_.log_state = LogState::Failed;
        state_.log_context = "The log file could not be read";
        return;
    }
    const std::string marker = "app=" + ToKey(live_log_game_) + " ";
    for (const auto& line : tail.lines)
        if (!live_log_game_.valid() || line.find(marker) != std::string::npos) state_.log_lines.push_back(line);
    state_.log_state = LogState::Ready;
    std::string context;
    if (live_log_game_.valid()) {
        const auto* game = find_game(state_, live_log_game_);
        context = (game ? game->title : "This game") + std::string(" · ");
    }
    context += state_.log_lines.empty() ? std::string("nothing logged yet")
                                        : std::to_string(state_.log_lines.size()) + " lines, newest last";
    if (tail.truncated) context += " · older lines omitted";
    state_.log_context = std::move(context);
    // Open at the newest lines.
    state_.log_offset = std::max(0, static_cast<int>(state_.log_lines.size()) - 8);
}

void Simulation::finish_logs() {
    state_.log_state = LogState::Ready;
    state_.log_lines.clear();
    state_.log_lines.reserve(32);
    for (int index = 0; index < 32; ++index) {
        state_.log_lines.push_back("[fixture " + std::to_string(index + 1) + "] deterministic event for " + state_.log_context);
    }
    if (state_.log_lines.size() > 128) state_.log_lines.erase(state_.log_lines.begin(), state_.log_lines.end() - 128);
    state_.log_offset = 0;
}

std::string Simulation::next_job_id() {
    for (;;) {
        const auto id = "job-" + std::to_string(next_job_serial_++);
        if (!find_job(state_, id)) return id;
    }
}

void Simulation::start_editor(std::string purpose, std::string value, bool secret) {
    const auto opener = state_.navigation.focus_key;
    editing_setting_id_.clear();
    editing_append_ = false;
    state_.editor = {};
    state_.editor.active = true;
    state_.editor.secret = secret;
    state_.editor.serial = ++editor_serial_;
    state_.editor.system_keyboard = system_keyboard_available_;
    state_.editor.purpose = std::move(purpose);
    state_.editor.value = value;
    state_.editor.original = std::move(value);
    state_.editor.opener_focus_key = opener;
    state_.editor.caret = state_.editor.value.size();
    state_.navigation.focus_key = "keyboard-done";
}

void Simulation::set_route_focus(std::string key) {
    if (state_.editor.active) state_.editor.opener_focus_key = std::move(key);
    else state_.navigation.focus_key = std::move(key);
}

void Simulation::clear_sign_in_secrets() {
    steam::ZeroizeSecret(state_.sign_in.password);
    steam::ZeroizeSecret(state_.sign_in.code);
    if (state_.editor.secret) {
        steam::ZeroizeSecret(state_.editor.value);
        steam::ZeroizeSecret(state_.editor.original);
        state_.editor.caret = 0;
    }
}

void Simulation::normalize_editor() {
    if (state_.editor.purpose != "Steam Guard code") return;
    std::string code;
    for (const char c : state_.editor.value) {
        if (std::isalnum(static_cast<unsigned char>(c)) && code.size() < 5)
            code += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    state_.editor.value = std::move(code);
    state_.editor.caret = std::min(state_.editor.caret, state_.editor.value.size());
}

void Simulation::open_sign_in(bool reauthenticate) {
    const bool password = state_.sign_in_password;
    clear_sign_in_secrets();
    const auto name = state_.sign_in.account_name;
    state_.sign_in = {};
    state_.sign_in.account_name = name;
    state_.sign_in.step = password ? SignInStep::Password : SignInStep::Qr;
    state_.account().authentication = AuthState::Waiting;
    state_.account().challenge_url.clear();
    state_.account().error.clear();
    state_.navigation.route = Route::SignIn;
    state_.navigation.focus_key = password ? (name.empty() ? "signin-account" : "signin-password") : "qr-cancel";
    if (live()) queue_service_action(reauthenticate ? ServiceAction::Reauthenticate
                                     : password ? ServiceAction::UsePassword : ServiceAction::BeginQr);
    else if (!password) scenario("qr-waiting");
}

bool Simulation::sign_in_action(const std::string& action_name) {
    auto& form = state_.sign_in;
    if (action_name == "signin-password-mode" || action_name == "signin-qr-mode") {
        const bool password = action_name == "signin-password-mode";
        clear_sign_in_secrets();
        const auto name = form.account_name;
        form = {};
        form.account_name = name;
        form.step = password ? SignInStep::Password : SignInStep::Qr;
        state_.account().authentication = AuthState::Waiting;
        state_.account().challenge_url.clear();
        state_.account().error.clear();
        state_.navigation.focus_key = password ? (name.empty() ? "signin-account" : "signin-password") : "qr-cancel";
        if (live()) queue_service_action(password ? ServiceAction::UsePassword : ServiceAction::UseQr);
        else if (!password) scenario("qr-waiting");
    } else if (action_name == "signin-stay") {
        // In force at once: a sign-in that completes from now on follows the new choice.
        state_.stay_signed_in = !state_.stay_signed_in;
        save_profile();
        if (live()) queue_service_action(ServiceAction::StaySignedIn);
    } else if (action_name == "signin-account") {
        start_editor("Account name", form.account_name);
    } else if (action_name == "signin-password") {
        start_editor("Password", form.password, true);
    } else if (action_name == "signin-submit") {
        if (form.step != SignInStep::Password || form.account_name.empty() || form.password.empty()) return true;
        form.error.clear();
        if (!live()) { submit_fixture_sign_in(); return true; }
        if (pending_credentials_) steam::ZeroizeSecret(pending_credentials_->password);
        pending_credentials_ = SignInCredentials{form.account_name, form.password};
        steam::ZeroizeSecret(form.password);
        form.step = SignInStep::Submitting;
        state_.account().authentication = AuthState::Waiting;
        state_.account().error.clear();
        state_.navigation.focus_key = "signin-cancel";
        queue_service_action(ServiceAction::BeginPassword);
    } else if (action_name == "guard-code") {
        start_editor("Steam Guard code", form.code);
    } else if (action_name == "guard-use-code") {
        form.code_entry = true;
        steam::ZeroizeSecret(form.code);
        state_.navigation.focus_key = "guard-code";
    } else if (action_name == "guard-submit") {
        if (form.code.size() != 5 || form.code_sent) return true;
        form.code_rejected = false;
        if (!live()) { submit_fixture_sign_in(); return true; }
        if (pending_guard_code_) steam::ZeroizeSecret(*pending_guard_code_);
        pending_guard_code_ = form.code;
        form.code_sent = true;
        state_.navigation.focus_key = "guard-cancel";
        queue_service_action(ServiceAction::SubmitCode);
    } else if (action_name == "signin-cancel" || action_name == "guard-cancel") {
        cancel();
    } else {
        return false;
    }
    return true;
}

// Fixture mode scripts Steam's answers by account name: "wrong" (password refused), "limited" (rate
// limit), "email" and "mobile" (a code; 00000 is refused), "approve" (approval in the app, or a code);
// any other name signs in.
void Simulation::submit_fixture_sign_in() {
    auto& form = state_.sign_in;
    auto succeed = [this] {
        clear_sign_in_secrets();
        state_.sign_in_password = true;
        scenario("qr-approved");
    };
    if (form.step == SignInStep::Password) {
        const auto& name = form.account_name;
        if (name == "wrong" || name == "limited") {
            clear_sign_in_secrets();
            state_.account().authentication = AuthState::Failed;
            form.error = name == "wrong" ? "Steam did not accept the account name or password. Check them and try again."
                                         : "Steam is limiting sign-in attempts. Wait a while before you try again, or use the QR code.";
            state_.navigation.focus_key = "signin-password";
        } else if (name == "email" || name == "mobile" || name == "approve") {
            steam::ZeroizeSecret(form.password);
            form.step = name == "email" ? SignInStep::EmailCode : name == "mobile" ? SignInStep::AuthenticatorCode : SignInStep::Approval;
            form.email_domain = name == "email" ? "example.invalid" : "";
            form.code_allowed = true;
            form.code_is_email = name == "email";
            state_.navigation.focus_key = form.step == SignInStep::Approval ? "guard-use-code" : "guard-code";
        } else {
            succeed();
        }
        return;
    }
    if (form.code == "00000") {
        steam::ZeroizeSecret(form.code);
        form.code_rejected = true;
        if (form.step == SignInStep::Approval) form.step = form.code_is_email ? SignInStep::EmailCode : SignInStep::AuthenticatorCode;
        form.code_entry = false;
        state_.navigation.focus_key = "guard-code";
        return;
    }
    succeed();
}

void Simulation::commit_editor() {
    if (state_.navigation.route == Route::SignIn &&
        (state_.editor.purpose == "Account name" || state_.editor.purpose == "Password" ||
         state_.editor.purpose == "Steam Guard code")) {
        auto& form = state_.sign_in;
        std::string next;
        if (state_.editor.purpose == "Account name") {
            form.account_name = trimmed(state_.editor.value);
            next = form.account_name.empty() ? "signin-account" : "signin-password";
        } else if (state_.editor.purpose == "Password") {
            steam::ZeroizeSecret(form.password);
            form.password = state_.editor.value;
            next = form.password.empty() ? "signin-password" : form.account_name.empty() ? "signin-account" : "signin-submit";
        } else {
            form.code = state_.editor.value;
            next = form.code.size() == 5 ? "guard-submit" : "guard-code";
        }
        if (state_.editor.secret) { steam::ZeroizeSecret(state_.editor.value); steam::ZeroizeSecret(state_.editor.original); }
        state_.editor.active = false;
        state_.editor.secret = false;
        state_.editor.revealed = false;
        state_.navigation.focus_key = next;
        return;
    }
    if (state_.editor.purpose == "Search") { state_.library.query = state_.editor.value; reconcile_library_selection(state_); }
    else if (state_.editor.purpose == "Steam app ID") {
        // A Steam app ID in plain decimal ("440"), or nothing to clear it.
        if (!can_change_game_settings()) return;
        const auto value = trimmed(state_.editor.value);
        const auto parsed = value.empty() ? std::optional<std::uint32_t>{0} : rt::ParseSteamAppId(value);
        if (!parsed || (!value.empty() && std::to_string(*parsed) != value)) {
            state_.notices.push_back({"setting-validation", "Steam app ID: use the number in the game's Steam store address",
                                      "", true, 0, true});
            return;
        }
        std::erase_if(state_.notices, [](const Notice& notice) { return notice.id == "setting-validation"; });
        auto& record = state_.game_settings[state_.navigation.selected_game_id];
        if (record.steam_app_id != *parsed) {
            record.steam_app_id = *parsed;
            ++record.revision;
        }
        state_.editor.active = false;
        state_.navigation.focus_key = state_.editor.opener_focus_key;
        refresh_game_settings();
        save_profile();
        return;
    }
    else if (!editing_setting_id_.empty()) {
        if (!can_change_game_settings()) return;
        auto value = trimmed(state_.editor.value);
        // "Add an override" puts one entry after the current list.
        if (editing_append_) {
            rt::EffectiveSettings current;
            if (!value.empty() && EffectiveSettingsFor(state_.navigation.selected_game_id, &current) && !current.dll_overrides.empty())
                value = current.dll_overrides + ";" + value;
            else if (value.empty() && EffectiveSettingsFor(state_.navigation.selected_game_id, &current))
                value = current.dll_overrides;
        }
        std::string reason;
        if (!value.empty() && !rt::ValidateSettingValue(RecipeFor(state_.navigation.selected_game_id), editing_setting_id_, value, &reason)) {
            state_.notices.push_back({"setting-validation", state_.editor.purpose + ": " + reason, "", true, 0, true});
            return;
        }
        state_.notices.erase(std::remove_if(state_.notices.begin(), state_.notices.end(),
                                            [](const Notice& notice) { return notice.id == "setting-validation"; }),
                             state_.notices.end());
        set_game_setting(editing_setting_id_, value);
        editing_setting_id_.clear();
        editing_append_ = false;
        state_.editor.active = false;
        state_.navigation.focus_key = state_.editor.opener_focus_key;
        refresh_game_settings();
        save_profile();
        return;
    }
    else if (state_.editor.purpose == "Note") state_.library.notes[state_.navigation.selected_game_id] = state_.editor.value;
    else if (state_.editor.purpose == "Collection name") {
        const auto name = trimmed(state_.editor.value);
        auto duplicate = std::find_if(state_.library.collections.begin(), state_.library.collections.end(), [&name](const Collection& value) { return value.name == name; });
        if (name.empty() || duplicate != state_.library.collections.end()) {
            state_.notices.push_back({"collection-validation", "Collection names must be non-empty and unique", "", true, 0});
            return;
        }
        std::string id;
        do { id = "collection-" + std::to_string(next_collection_serial_++); }
        while (std::any_of(state_.library.collections.begin(), state_.library.collections.end(), [&id](const Collection& value) { return value.id == id; }));
        // Made from a game's Collections sheet, the new collection holds that game.
        const bool from_game = std::any_of(state_.navigation.modals.begin(), state_.navigation.modals.end(),
                                           [](const ModalEntry& modal) { return modal.id == "collections"; });
        std::set<GameId> members;
        if (from_game && state_.navigation.selected_game_id.valid()) members.insert(state_.navigation.selected_game_id);
        const auto new_id = id;
        state_.library.collections.push_back({std::move(id), name, false, std::move(members)});
        state_.editor.active = false;
        state_.navigation.focus_key = state_.editor.opener_focus_key;
        refresh_collection_sheets("collection-toggle-" + new_id);
        save_profile();
        return;
    } else if (state_.editor.purpose == "Rename collection") {
        const auto name = trimmed(state_.editor.value);
        const auto duplicate = std::find_if(state_.library.collections.begin(), state_.library.collections.end(), [this, &name](const Collection& value) { return value.name == name && value.id != editing_target_id_; });
        const auto target = std::find_if(state_.library.collections.begin(), state_.library.collections.end(), [this](const Collection& value) { return value.id == editing_target_id_; });
        if (name.empty() || duplicate != state_.library.collections.end() || target == state_.library.collections.end()) {
            state_.notices.push_back({"collection-validation", "Collection names must be non-empty and unique", "", true, 0});
            return;
        }
        target->name = name;
        state_.editor.active = false;
        state_.navigation.focus_key = state_.editor.opener_focus_key;
        refresh_collection_sheets();
        save_profile();
        return;
    }
    state_.editor.active = false;
    state_.navigation.focus_key = state_.editor.opener_focus_key;
    save_profile();
}

void Simulation::cancel() {
    if (tab_bar_shown() && state_.navigation.tabs_focused) {
        state_.navigation.tabs_focused = false;
        return;
    }
    if (state_.navigation.route == Route::Settings && state_.navigation.settings_rail_focused &&
        state_.navigation.modals.empty() && !state_.editor.active) {
        state_.navigation.settings_rail_focused = false;
        return;
    }
    if (state_.editor.active) {
        if (state_.editor.secret) { steam::ZeroizeSecret(state_.editor.value); steam::ZeroizeSecret(state_.editor.original); }
        state_.editor.active = false;
        state_.editor.revealed = false;
        state_.navigation.focus_key = state_.editor.opener_focus_key;
        return;
    }
    // An applied search is undone before Circle leaves the Library.
    if (state_.navigation.route == Route::Library && state_.navigation.modals.empty() && !state_.library.query.empty()) {
        state_.library.query.clear();
        reconcile_library_selection(state_);
        return;
    }
    if (!state_.navigation.modals.empty()) {
        const bool crash_logs = state_.navigation.modals.back().id == "logs" && state_.session == SessionState::Crashed;
        const bool settings_sheet = state_.navigation.modals.back().id == "game-settings";
        close_modal();
        if (settings_sheet) announce_settings_saved();
        if (crash_logs) {
            state_.navigation.route = Route::Details;
            state_.navigation.detail_id = state_.session_game_id;
            state_.navigation.selected_game_id = state_.session_game_id;
            state_.navigation.focus_key = "details-simulate-launch";
        }
        return;
    }
    if (state_.navigation.route == Route::SignIn) {
        if (live()) queue_service_action(ServiceAction::CancelQr);
        clear_sign_in_secrets();
        if (pending_credentials_) steam::ZeroizeSecret(pending_credentials_->password);
        pending_credentials_.reset();
        if (pending_guard_code_) steam::ZeroizeSecret(*pending_guard_code_);
        pending_guard_code_.reset();
        state_.sign_in.error.clear();
        cancelled_challenge_generation_ = challenge_generation_;
        state_.account().authentication = AuthState::Cancelled;
        leave_steam_sign_in();
    } else if (state_.navigation.route == Route::SaveDetails) {
        state_.navigation.route = Route::Saves;
        state_.navigation.focus_key = "save-" + ToKey(state_.navigation.save_id);
    } else if (state_.navigation.route == Route::Details || state_.navigation.route == Route::Session) {
        state_.navigation.route = state_.navigation.route == Route::Session ? Route::Details : state_.navigation.detail_origin;
        state_.navigation.focus_key = state_.navigation.route == Route::Details ? "details-play" : game_key(state_.navigation.selected_game_id);
    } else if (state_.navigation.route != Route::Home) {
        state_.navigation.route = Route::Home;
        state_.navigation.focus_key = game_key(state_.navigation.selected_game_id);
    }
}

namespace {
int route_tab(Route route) {
    return route == Route::Library ? 1 : route == Route::Downloads ? 2 : route == Route::Saves ? 3 : route == Route::Settings ? 4 : 0;
}
}  // namespace

void Simulation::move(int delta_x, int delta_y) {
    if (state_.editor.active) {
        state_.editor.focus_row = std::clamp(state_.editor.focus_row + delta_y, 0, 4);
        // A password's bottom row has a Show key between space and backspace.
        const int max_key = state_.editor.focus_row == 4 ? (state_.editor.symbols ? 3 : 4) + (state_.editor.secret ? 1 : 0) : 9;
        state_.editor.focus_key = std::clamp(state_.editor.focus_key + delta_x, 0, max_key);
        return;
    }
    if (!state_.navigation.modals.empty()) {
        auto& modal = state_.navigation.modals.back();
        if (modal.id == "logs" && state_.log_state == LogState::Ready && !state_.log_lines.empty()) {
            const int delta = delta_y != 0 ? delta_y : delta_x;
            state_.log_offset = std::clamp(state_.log_offset + delta, 0, std::max(0, static_cast<int>(state_.log_lines.size()) - 8));
            return;
        }
        if (modal.id == "installed-games") { move_installed_manager(delta_x, delta_y); return; }
        if (modal.id == "import-games") { move_import_sheet(delta_y != 0 ? delta_y : delta_x); return; }
        const int delta = delta_y != 0 ? delta_y : delta_x;
        const int last = static_cast<int>(modal.action_keys.size()) - 1;
        modal.focused_action = std::clamp(modal.focused_action + delta, 0, last);
        // Game settings' topic headings only label the rows below them: focus goes on past them.
        if (modal.id == "game-settings" && delta != 0) {
            const auto heading = [&modal](int index) { return modal.action_keys[static_cast<std::size_t>(index)].rfind("heading-", 0) == 0; };
            const int step = delta > 0 ? 1 : -1;
            while (heading(modal.focused_action) && modal.focused_action + step >= 0 && modal.focused_action + step <= last)
                modal.focused_action += step;
            while (heading(modal.focused_action) && modal.focused_action - step >= 0 && modal.focused_action - step <= last)
                modal.focused_action -= step;
        }
        state_.navigation.focus_key = modal.action_keys[static_cast<std::size_t>(modal.focused_action)];
        return;
    }
    // The tab bar, as on the PS5 Home screen: Up from the top of a page focuses the current tab,
    // Left/Right move between tabs and Down returns to the page. Cross opens the focused tab.
    if (tab_bar_shown()) {
        auto& navigation = state_.navigation;
        if (navigation.tabs_focused) {
            if (delta_x != 0) navigation.tab_focus = std::clamp(navigation.tab_focus + (delta_x > 0 ? 1 : -1), 0, 4);
            else if (delta_y > 0) navigation.tabs_focused = false;
            return;
        }
        if (delta_y < 0) {
            const auto focus = navigation.focus_key;
            const auto index = navigation.selected_index;
            const auto shelf = navigation.home_shelf;
            const bool library_rail = navigation.library_rail_focused, settings_rail = navigation.settings_rail_focused;
            move_content(delta_x, delta_y);
            if (navigation.focus_key == focus && navigation.selected_index == index && navigation.home_shelf == shelf &&
                navigation.library_rail_focused == library_rail && navigation.settings_rail_focused == settings_rail) {
                navigation.tabs_focused = true;
                navigation.tab_focus = route_tab(navigation.route);
            }
            return;
        }
    }
    move_content(delta_x, delta_y);
}

bool Simulation::tab_bar_shown() const {
    const auto route = state_.navigation.route;
    return state_.navigation.modals.empty() && !state_.editor.active &&
           (route == Route::Home || route == Route::Library || route == Route::Downloads || route == Route::Saves ||
            route == Route::Settings);
}

void Simulation::move_content(int delta_x, int delta_y) {
    if (move_browser(state_, delta_x, delta_y)) return;
    if (state_.navigation.route == Route::Settings && move_settings(delta_x, delta_y)) return;
    if (state_.navigation.route == Route::Saves && move_saves(delta_x, delta_y)) return;
    const int columns = state_.navigation.route == Route::Library ? 6 : 1;
    const int delta = delta_y != 0 ? delta_y * columns : delta_x;
    if (view_.rows.empty()) return;
    // The game page draws only its buttons in a row; status notes between them are not stops.
    if (state_.navigation.route == Route::Details && delta != 0) {
        const auto button = [](const ViewRow& item) { return item.kind == RowKind::Action || item.kind == RowKind::Toggle; };
        int target = view_.focused_row;
        for (int next = view_.focused_row + delta; next >= 0 && next < static_cast<int>(view_.rows.size()); next += delta)
            if (button(view_.rows[static_cast<std::size_t>(next)])) { target = next; break; }
        if (target == view_.focused_row) return;
        state_.navigation.selected_index = target;
        state_.navigation.focus_key = view_.rows[static_cast<std::size_t>(target)].key;
        return;
    }
    state_.navigation.selected_index = std::clamp(view_.focused_row + delta, 0, static_cast<int>(view_.rows.size()) - 1);
    const auto& selected = view_.rows[static_cast<std::size_t>(state_.navigation.selected_index)];
    state_.navigation.focus_key = selected.key;
    if (selected.game_id.valid()) state_.navigation.selected_game_id = selected.game_id;
    if (state_.navigation.route == Route::Library) state_.navigation.scroll_offsets["library-grid"] = state_.navigation.selected_index / columns;
    if (state_.navigation.route == Route::Home) state_.navigation.scroll_offsets["home"] = state_.navigation.selected_index;
}

// Settings sections, as on macOS: Left moves to the section rail, Up/Down there switches
// section (focusing its first row), Right or Cross returns to the rows. Up/Down on the rows
// stay in the focused section; Up on its first row goes to the tab bar.
bool Simulation::move_settings(int delta_x, int delta_y) {
    if (view_.rows.empty()) return true;
    const int current = std::clamp(view_.focused_row, 0, static_cast<int>(view_.rows.size()) - 1);
    if (!state_.navigation.settings_rail_focused) {
        if (delta_x < 0) { state_.navigation.settings_rail_focused = true; return true; }
        // Up and Down stay in the focused section: the rail (or the tab bar above the first row)
        // is the way to another one. A rail-only section (Quit Playden) has no card to move onto.
        if (delta_y != 0) {
            const auto& section = view_.rows[static_cast<std::size_t>(current)].section;
            int first = current, last = current;
            while (first > 0 && view_.rows[static_cast<std::size_t>(first - 1)].section == section) --first;
            while (last + 1 < static_cast<int>(view_.rows.size()) && view_.rows[static_cast<std::size_t>(last + 1)].section == section) ++last;
            const int target = std::clamp(current + delta_y, first, last);
            if (target != current && !view_.rows[static_cast<std::size_t>(target)].rail_only) {
                state_.navigation.selected_index = target;
                state_.navigation.focus_key = view_.rows[static_cast<std::size_t>(target)].key;
            }
            return true;
        }
        return delta_x > 0;
    }
    if (delta_x > 0) {
        if (!view_.rows[static_cast<std::size_t>(current)].rail_only) state_.navigation.settings_rail_focused = false;
        return true;
    }
    if (delta_y == 0) return true;
    std::vector<std::string> sections;
    for (const auto& item : view_.rows)
        if (sections.empty() || sections.back() != item.section) sections.push_back(item.section);
    const auto here = std::find(sections.begin(), sections.end(), view_.rows[static_cast<std::size_t>(current)].section);
    const int index = std::clamp(static_cast<int>(std::distance(sections.begin(), here)) + (delta_y > 0 ? 1 : -1), 0,
                                 static_cast<int>(sections.size()) - 1);
    const auto first = std::find_if(view_.rows.begin(), view_.rows.end(), [&](const ViewRow& item) {
        return item.section == sections[static_cast<std::size_t>(index)];
    });
    state_.navigation.selected_index = static_cast<int>(std::distance(view_.rows.begin(), first));
    state_.navigation.focus_key = first->key;
    return true;
}

void Simulation::change_route(int delta) {
    if (state_.editor.active || !state_.navigation.modals.empty()) return;
    // The password form and its Steam Guard steps are left with Cancel, which stops the sign-in and wipes them.
    if (state_.navigation.route == Route::SignIn && state_.sign_in.step != SignInStep::Qr) return;
    constexpr Route routes[] = {Route::Home, Route::Library, Route::Downloads, Route::Saves, Route::Settings};
    constexpr int count = static_cast<int>(std::size(routes));
    auto found = std::find(std::begin(routes), std::end(routes), state_.navigation.route);
    int index = found == std::end(routes) ? 0 : static_cast<int>(std::distance(std::begin(routes), found));
    index = ((index + delta) % count + count) % count;
    state_.navigation.route = routes[index];
    state_.navigation.selected_index = 0;
    state_.navigation.focus_key.clear();
    state_.navigation.settings_rail_focused = false;
    state_.navigation.tab_focus = index;
    if (state_.navigation.route == Route::Saves) open_saves();
}

void Simulation::open_saves() {
    // Opening the tab checks what may have changed; the controller limits how often.
    if (live()) pending_cloud_requests_.push_back({CloudRequestKind::QuickCheck, {}});
}

// Games whose saves were never uploaded to this Steam account and wait only for the player to
// confirm it ("Not in Cloud yet"); Upload all confirms them together.
std::vector<GameId> Simulation::saves_awaiting_account() const {
    std::vector<GameId> out;
    for (const auto id : save_games(false)) {
        const auto cloud = state_.cloud.find(id);
        if (cloud != state_.cloud.end() && cloud->second.status == CloudStatus::NeedsAccount && !cloud->second.queued &&
            !cloud->second.after_play)
            out.push_back(id);
    }
    return out;
}

void Simulation::ask_upload_all() {
    const auto count = saves_awaiting_account().size();
    if (count == 0 || state_.saves_job.kind != SavesJobKind::None) return;
    ModalEntry confirm{"confirmation", count == 1 ? "Upload saves to this Steam account?" : "Upload all saves to this Steam account?",
                       state_.navigation.focus_key, {"cloud-confirm-account-all", "cancel"}, 0, false};
    confirm.body = "Playden has not synced " + (count == 1 ? std::string("1 game's saves") : std::to_string(count) + " games' saves") +
                   " for " + (state_.account().display_name.empty() ? std::string("this account") : state_.account().display_name) +
                   " before. Confirm to upload this console's saves to its Steam Cloud.";
    open_modal(std::move(confirm));
}

std::vector<GameId> Simulation::save_games(bool filtered) const {
    std::set<GameId> ids;
    for (const auto& installation : state_.installations) ids.insert(installation.game_id);
    for (const auto& kept : state_.kept_bottles) ids.insert(kept.game_id);
    if (!live()) for (const auto& [id, cloud] : state_.cloud) ids.insert(id);
    // Saves lists Cloud saves: games of a store without them are not there.
    std::erase_if(ids, [](const GameId& id) { return !Traits(id.store).cloud_saves; });
    std::vector<std::pair<std::string, GameId>> named;
    for (const auto id : ids) {
        const auto* game = find_game(state_, id);
        std::string name = game ? game->title : std::string();
        if (name.empty())
            for (const auto& kept : state_.kept_bottles) if (kept.game_id == id) name = kept_name(state_, kept);
        if (name.empty()) name = "Steam app " + ToKey(id);
        if (filtered && state_.navigation.saves_filter != 0) {
            const auto cloud = state_.cloud.find(id);
            const bool installed = std::any_of(state_.installations.begin(), state_.installations.end(),
                                               [id](const Installation& item) { return item.game_id == id; });
            const auto facts = save_facts(cloud == state_.cloud.end() ? nullptr : &cloud->second, installed);
            const int filter = state_.navigation.saves_filter;
            if ((filter == 1 && !facts.needs_you) || (filter == 2 && !facts.in_sync) || (filter == 3 && !facts.local_only) ||
                (filter == 4 && installed))
                continue;
        }
        std::string folded = name;
        for (auto& value : folded) value = static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
        named.emplace_back(std::move(folded), id);
    }
    std::sort(named.begin(), named.end());
    std::vector<GameId> out;
    for (const auto& [name, id] : named) out.push_back(id);
    return out;
}

// The Saves tab: the filter chips and Sync all form one row above the games. Left/Right move along
// it, Down enters the list; Up from the first game returns to the current filter, Up from the chips
// reaches the tab bar.
bool Simulation::move_saves(int delta_x, int delta_y) {
    if (view_.rows.empty()) return true;
    const auto chip = [](const ViewRow& item) {
        return item.key.rfind("saves-filter-", 0) == 0 || item.key == "saves-upload-all" || item.key == "saves-sync-all";
    };
    const int current = std::clamp(view_.focused_row, 0, static_cast<int>(view_.rows.size()) - 1);
    int chips = 0;
    while (chips < static_cast<int>(view_.rows.size()) && chip(view_.rows[static_cast<std::size_t>(chips)])) ++chips;
    int target = current;
    if (current < chips) {
        if (delta_x != 0) target = std::clamp(current + (delta_x > 0 ? 1 : -1), 0, chips - 1);
        else if (delta_y > 0 && chips < static_cast<int>(view_.rows.size())) target = chips;
    } else {
        if (delta_y < 0 && current == chips) target = std::clamp(state_.navigation.saves_filter, 0, chips - 1);
        else if (delta_y != 0) target = std::clamp(current + delta_y, chips, static_cast<int>(view_.rows.size()) - 1);
    }
    if (target == current) return true;
    state_.navigation.selected_index = target;
    state_.navigation.focus_key = view_.rows[static_cast<std::size_t>(target)].key;
    return true;
}

void Simulation::UpdateSavesJob(LiveSavesJob job) {
    if (!live()) return;
    const bool finished = state_.saves_job.kind == SavesJobKind::SyncAll && job.kind == SavesJobKind::None;
    state_.saves_job = job;
    if (finished) {
        std::size_t in_sync = 0, need = 0;
        for (const auto id : save_games(false)) {
            const auto cloud = state_.cloud.find(id);
            if (cloud == state_.cloud.end()) continue;
            const auto facts = save_facts(&cloud->second, true);
            in_sync += facts.in_sync ? 1 : 0;
            need += facts.needs_you ? 1 : 0;
        }
        push_notice({"saves-sync-all", "Sync all finished", "", false, kLiveNoticeSeconds, false,
                     std::to_string(in_sync) + (in_sync == 1 ? " game" : " games") + " in sync" +
                         (need > 0 ? " · " + std::to_string(need) + " need you" : std::string())});
    }
    rebuild_view();
}

void Simulation::UpdateSaveSizes(std::map<GameId, std::uint64_t> sizes) {
    if (!live() || sizes == state_.save_sizes) return;
    state_.save_sizes = std::move(sizes);
    rebuild_view();
}

// Quitting stops the game runtime, so a preparation still running would be cut off and its half-made
// title left on the Home screen, where it fails to start. The quit waits for it in a dialog instead.
void Simulation::quit_to_home() {
    const auto preparing = std::find_if(state_.runtime_games.begin(), state_.runtime_games.end(),
                                        [](const auto& entry) { return entry.second.status == "Preparing"; });
    // Games waiting in the queue hold too: between two preparations the next one may be about to start.
    const auto& waiting = state_.preparation_queue.waiting;
    if (preparing == state_.runtime_games.end() && waiting.empty()) {
        state_.quit_requested = true;
        return;
    }
    const auto* game = find_game(state_, preparing != state_.runtime_games.end() ? preparing->first : waiting.front());
    const std::string name = game ? game->title : std::string("A game");
    quit_wait_title_ = preparing != state_.runtime_games.end() ? "Waiting for " + name : std::string("Waiting for the queue");
    state_.quit_waiting = true;
    ModalEntry wait{"confirmation", quit_wait_title_, state_.navigation.focus_key, {"cancel"}, 0, false};
    wait.body = preparing != state_.runtime_games.end()
        ? name + " is being prepared. Playden quits to the PS5 Home screen once it is done, so it is not left half-made."
        : "Playden quits to the PS5 Home screen once the queued games are held, so none is left half-made.";
    open_modal(std::move(wait));
}

// Play on a game waiting for the PS5 quits to Home; first its saves sync. Quits once they are in
// step; a choice, a failure or a refusal lets the player play without syncing.
void Simulation::settle_quit_sync(GameId id, std::optional<CloudStatus> status) {
    if (!quit_sync_game_.valid() || id != quit_sync_game_) return;
    const bool waiting = !state_.navigation.modals.empty() &&
                         state_.navigation.modals.back().action_keys.size() == 2 &&
                         state_.navigation.modals.back().action_keys.front() == "saves-quit-anyway";
    quit_sync_game_ = {};
    if (!waiting) return;  // the player cancelled
    if (status && (*status == CloudStatus::Synced || *status == CloudStatus::UpToDate || *status == CloudStatus::Unsupported)) {
        state_.navigation.modals.clear();
        quit_to_home();
        return;
    }
    auto& modal = state_.navigation.modals.back();
    const auto cloud = state_.cloud.find(id);
    if (!status) {
        modal.title = "The game may still be running";
        modal.body = "Its saves can't sync while it runs. Close it, or play now with this console's saves.";
    } else if (*status == CloudStatus::Failed) {
        modal.title = "Cloud saves could not sync";
        modal.body = (cloud == state_.cloud.end() ? std::string() : cloud->second.message + " ") +
                     "Play now with this console's saves, or cancel and try again from Saves.";
    } else {
        modal.title = "Cloud saves need a choice";
        modal.body = "Saves here and in Steam Cloud differ. Choose in Saves, or play now with this console's saves.";
    }
}

void Simulation::NotifyCloudAnswered(GameId id, CloudStatus status) {
    if (!live()) return;
    settle_quit_sync(id, status);
    rebuild_view();
}

void Simulation::NotifyCloudRefused(GameId id) {
    if (!live()) return;
    settle_quit_sync(id, std::nullopt);
    rebuild_view();
}


void Simulation::open_game_settings() {
    expanded_settings_sections_.clear();
    const auto opened = state_.game_settings.find(state_.navigation.selected_game_id);
    settings_opened_record_ = opened == state_.game_settings.end() ? rt::GameSettingsRecord{} : opened->second;
    // "Game must be closed" rows need to know whether the game runs now; an older answer is not trusted.
    if (live()) {
        pending_activity_requests_.push_back(state_.navigation.selected_game_id);
        if (const auto found = state_.runtime_games.find(state_.navigation.selected_game_id); found != state_.runtime_games.end())
            found->second.activity = TitleActivity::Unknown;
    }
    // From Game actions it opens on top of that sheet, so Circle returns there.
    open_modal({"game-settings", "Game settings", state_.navigation.focus_key, game_settings_actions(), 0, false});
}

void Simulation::activate() {
    if (tab_bar_shown() && state_.navigation.tabs_focused) {
        // Cross opens the focused tab and moves focus onto its page.
        const int target = std::clamp(state_.navigation.tab_focus, 0, 4);
        const int current = route_tab(state_.navigation.route);
        state_.navigation.tabs_focused = false;
        if (target != current) change_route(target - current);
        else if (state_.navigation.route == Route::Saves) open_saves();
        return;
    }
    if (state_.navigation.route == Route::Settings && state_.navigation.settings_rail_focused &&
        state_.navigation.modals.empty() && !state_.editor.active) {
        // Cross on a rail-only entry (Quit Playden) runs its action, as on macOS.
        const bool rail_action = !view_.rows.empty() &&
            view_.rows[static_cast<std::size_t>(std::clamp(view_.focused_row, 0, static_cast<int>(view_.rows.size()) - 1))].rail_only;
        if (!rail_action) {
            state_.navigation.settings_rail_focused = false;
            return;
        }
    }
    if (state_.editor.active) {
        static constexpr const char* letters[] = {"1234567890", "qwertyuiop", "asdfghjkl'", "zxcvbnm,.?"};
        static constexpr const char* symbols[] = {"1234567890", "!@#$%^&*()", "-_=+[]{}<>", "/\\|:;\"'~`?"};
        if (state_.editor.focus_row < 4) {
            char inserted = (state_.editor.symbols ? symbols : letters)[state_.editor.focus_row][state_.editor.focus_key];
            if (state_.editor.shifted && !state_.editor.symbols) {
                if (inserted >= 'a' && inserted <= 'z') inserted = static_cast<char>(inserted - 'a' + 'A');
                else if (state_.editor.focus_row == 2 && state_.editor.focus_key == 9) inserted = '"';
                else if (state_.editor.focus_row == 3 && state_.editor.focus_key == 7) inserted = ';';
                else if (state_.editor.focus_row == 3 && state_.editor.focus_key == 8) inserted = ':';
                else if (state_.editor.focus_row == 3 && state_.editor.focus_key == 9) inserted = '!';
            }
            state_.editor.value.insert(state_.editor.caret, 1, inserted); ++state_.editor.caret;
            if (state_.editor.shifted) state_.editor.shifted = false;
            normalize_editor();
        } else if ((!state_.editor.symbols && state_.editor.focus_key == 0)) state_.editor.shifted = !state_.editor.shifted;
        else {
            const int offset = state_.editor.symbols ? 0 : 1;
            int special = state_.editor.focus_key - offset;
            if (state_.editor.secret && special == 2) { state_.editor.revealed = !state_.editor.revealed; return; }
            if (state_.editor.secret && special > 2) --special;
            if (special == 0) state_.editor.symbols = !state_.editor.symbols;
            else if (special == 1) { state_.editor.value.insert(state_.editor.caret, " "); ++state_.editor.caret; normalize_editor(); }
            else if (special == 2 && state_.editor.caret > 0) {
                const auto prior = previous_caret(state_.editor.value, state_.editor.caret);
                state_.editor.value.erase(prior, state_.editor.caret - prior); state_.editor.caret = prior;
            }
            else if (special == 3) commit_editor();
        }
        return;
    }
    if (state_.navigation.route == Route::Library && state_.navigation.library_rail_focused) {
        const auto scope = state_.navigation.focus_key.rfind("rail-", 0) == 0 ? state_.navigation.focus_key.substr(5) : state_.library.scope;
        if (scope == "new-collection") { start_editor("Collection name", ""); return; }
        state_.library.scope = scope;
        state_.navigation.library_rail_focused = false;
        const auto games = visible_games();
        if (games.empty()) { state_.navigation.selected_game_id = {}; state_.navigation.focus_key = state_.library.query.empty() ? "library-empty" : "clear-search"; }
        else {
            state_.navigation.selected_index = std::min(state_.navigation.selected_index, static_cast<int>(games.size()) - 1);
            state_.navigation.selected_game_id = games[static_cast<std::size_t>(state_.navigation.selected_index)]->id;
            state_.navigation.focus_key = game_key(state_.navigation.selected_game_id);
        }
        save_profile(); return;
    }
    if (state_.navigation.route == Route::Home && state_.navigation.modals.empty() && !state_.navigation.selected_game_id.valid() &&
        state_.navigation.focus_key.rfind("shelf-", 0) == 0 && state_.navigation.focus_key.size() > 5 &&
        state_.navigation.focus_key.compare(state_.navigation.focus_key.size() - 5, 5, "-more") == 0) {
        // "More" at the end of Continue playing: every game in the Library, most recently played first.
        // Only this visit: the sort the player chose comes back when the Library is opened again.
        state_.library.scope = "all";
        state_.library.view_sort = "recent-played";
        state_.library.query.clear();
        state_.navigation.library_rail_focused = false;
        state_.navigation.route = Route::Library;
        state_.navigation.selected_index = 0;
        state_.navigation.library_grid_column = 0;
        state_.navigation.scroll_offsets["library-grid"] = 0;
        const auto games = visible_games();
        state_.navigation.selected_game_id = games.empty() ? GameId{} : games.front()->id;
        state_.navigation.focus_key = games.empty() ? "library-empty" : game_key(state_.navigation.selected_game_id);
        save_profile();
        return;
    }
    // Home opens the card under the focus ring, which is drawn from the focused
    // shelf and its remembered position; the selection can lag behind it after a
    // background profile update.
    if (state_.navigation.route == Route::Home && state_.navigation.modals.empty()) {
        const auto projected = playden::homebrew::shelves(state_);
        if (!projected.empty()) {
            const auto& shelf = projected[static_cast<std::size_t>(
                std::clamp(state_.navigation.home_shelf, 0, static_cast<int>(projected.size()) - 1))];
            const auto remembered = state_.navigation.home_shelf_focus.find(shelf.key);
            const int item = remembered == state_.navigation.home_shelf_focus.end() ? 0 : remembered->second;
            if (item >= 0 && item < static_cast<int>(shelf.game_ids.size()))
                state_.navigation.selected_game_id = shelf.game_ids[static_cast<std::size_t>(item)];
        }
    }
    // Home's empty state has only its buttons (Open Library, Import games, Connect a store): a game
    // selected before is not under the focus.
    if (state_.navigation.route == Route::Home && state_.navigation.modals.empty() && state_.navigation.selected_game_id.valid() &&
        state_.navigation.focus_key.rfind("home-", 0) != 0) {
        if (live()) force_organization_selection_ = true;
        state_.navigation.detail_origin = Route::Home;
        state_.navigation.detail_id = state_.navigation.selected_game_id;
        state_.navigation.route = Route::Details;
        details_primary_pending_ = live(); state_.navigation.focus_key = live() ? "details-favorite" : (is_installed(state_, state_.navigation.selected_game_id) ? "details-play" : "details-install");
        return;
    }
    if (view_.rows.empty()) return;
    const auto index = std::clamp(view_.focused_row, 0, static_cast<int>(view_.rows.size()) - 1);
    const ViewRow selected = view_.rows[static_cast<std::size_t>(index)];
    if (!selected.enabled) return;
    const auto& action_name = selected.action;
    const bool edits_settings = (action_name.rfind("preset-", 0) == 0 && action_name != "preset-picker") ||
                                (action_name.rfind("choice-", 0) == 0 && action_name != "choice-custom") ||
                                (action_name.rfind("program-", 0) == 0 && action_name != "program-scan") ||
                                action_name.rfind("override-remove-", 0) == 0 || action_name == "value-reset" ||
                                action_name == "unlock-setting" || action_name == "reset-settings";
    if (edits_settings && !can_change_game_settings()) return;
    if (sign_in_action(action_name)) return;
    if (action_name == "connect") open_sign_in();
    else if (action_name == "qr-retry" || action_name == "qr-refresh") {
        state_.sign_in.step = SignInStep::Qr;
        if (live()) {
            queue_service_action(ServiceAction::BeginQr);
            state_.account().authentication = AuthState::Waiting;
            state_.account().challenge_url.clear();
            state_.account().error.clear();
            state_.navigation.route = Route::SignIn;
            state_.navigation.focus_key = "qr-cancel";
        } else scenario("qr-waiting");
    }
    else if (action_name == "retry-save") {
        if (live()) persistence_retry_requested_ = true;
        else { state_.persistence_available = true; save_profile(); }
    }
    else if (action_name == "continue-without-saving") { state_.notices.erase(std::remove_if(state_.notices.begin(), state_.notices.end(), [](const Notice& notice) { return notice.id == "persistence"; }), state_.notices.end()); state_.navigation.route = Route::Home; }
    else if (action_name == "skip" || action_name == "library") { state_.navigation.route = action_name == "skip" ? Route::Home : Route::Library; state_.navigation.focus_key.clear(); }
    else if (action_name == "qr-cancel" || action_name == "editor-cancel" || action_name == "cancel") cancel();
    else if (action_name == "editor-done") commit_editor();
    else if (action_name == "clear-search") { state_.library.query.clear(); reconcile_library_selection(state_); }
    else if (action_name == "library-show-all") { state_.library.scope = "all"; reconcile_library_selection(state_); save_profile(); }
    else if (action_name == "open-game") {
        if (live()) force_organization_selection_ = true;
        state_.navigation.detail_origin = state_.navigation.route;
        state_.navigation.detail_id = selected.game_id;
        state_.navigation.selected_game_id = selected.game_id;
        state_.navigation.route = Route::Details;
        details_primary_pending_ = live(); state_.navigation.focus_key = live() ? "details-favorite" : (is_installed(state_, selected.game_id) ? "details-play" : "details-install");
    } else if (action_name == "favorite") {
        if (state_.library.favorites.contains(state_.navigation.selected_game_id)) state_.library.favorites.erase(state_.navigation.selected_game_id);
        else state_.library.favorites.insert(state_.navigation.selected_game_id);
        // From the More menu the change is done: the menu closes, as Hide does.
        if (!state_.navigation.modals.empty() && state_.navigation.modals.back().id == "context") close_modal();
        save_profile();
    } else if (action_name == "hide") {
        if (state_.library.hidden.contains(state_.navigation.selected_game_id)) state_.library.hidden.erase(state_.navigation.selected_game_id);
        else state_.library.hidden.insert(state_.navigation.selected_game_id);
        close_modal(); reconcile_library_selection(state_);
        const auto remaining = visible_games();
        if (remaining.empty()) state_.navigation.focus_key = state_.library.query.empty() ? "library-empty" : "clear-search";
        else {
            state_.navigation.selected_index = std::min(state_.navigation.selected_index, static_cast<int>(remaining.size()) - 1);
            state_.navigation.selected_game_id = remaining[static_cast<std::size_t>(state_.navigation.selected_index)]->id;
            state_.navigation.focus_key = game_key(state_.navigation.selected_game_id);
        }
    } else if (action_name == "context") open_modal({"context", "Game actions", "details-more", details_context_actions(), 0, false});
    else if (action_name == "install") open_modal({"install", "Install game", "details-install", {"install-confirm", "install-estimate-retry", "storage-picker", "cancel"}, 0, false});
    else if (action_name == "content-language-picker") {
        std::vector<std::string> actions;
        actions.reserve(playden::steam::ContentLanguages().size() + 1);
        for (const auto& language : playden::steam::ContentLanguages())
            actions.push_back("content-language-" + std::string(language.code));
        actions.push_back("cancel");
        // Opens on the language in use, so a list this long does not start at the top every time.
        int focus = 0;
        for (std::size_t index = 0; index < actions.size(); ++index)
            if (actions[index] == "content-language-" + state_.content_language) focus = static_cast<int>(index);
        open_modal({"content-language", "Game language", state_.navigation.focus_key, std::move(actions), focus, false});
    } else if (action_name.rfind("content-language-", 0) == 0) {
        const auto code = action_name.substr(17);
        if (playden::steam::IsContentLanguage(code)) {
            state_.content_language = code;
            save_profile();
        }
        close_modal();
        return;
    }
    else if (action_name == "storage-picker") {
        // From the install sheet the choice is for that install only; Settings changes the default.
        picking_install_volume_ = live() && !state_.navigation.modals.empty() && state_.navigation.modals.back().id == "install";
        std::vector<std::string> actions;
        for (const auto& volume : state_.volumes) actions.push_back("volume-" + volume.id);
        actions.push_back("cancel");
        open_modal({"storage", picking_install_volume_ ? "Install to" : "Choose storage",
                    state_.navigation.focus_key, std::move(actions), 0, false});
    } else if (action_name.rfind("volume-", 0) == 0) {
        const auto id = action_name.substr(7);
        if (!state_.navigation.modals.empty() && state_.navigation.modals.back().id == "import-storage") {
            choose_import_volume(id);
            return;
        }
        if (live() && picking_install_volume_) {
            const auto* volume = find_volume(state_, id);
            if (volume && volume->available) install_volume_id_ = id == selected_volume_id_ ? std::string() : id;
            picking_install_volume_ = false;
            close_modal();
            state_.navigation.focus_key = "install-confirm";  // back on the sheet, ready to install
            return;
        } else if (live()) {
            // The install service owns the choice; reflect it now and let its next snapshot confirm.
            const auto* volume = find_volume(state_, id);
            if (volume && volume->available && id != selected_volume_id_) {
                pending_install_requests_.push_back({InstallRequestKind::SetDefaultVolume, {}, id});
                selected_volume_id_ = id;
                state_.default_volume_id = id;
            }
        } else {
            selected_volume_id_ = id;
        }
        close_modal();
    } else if (action_name == "install-confirm") {
        const auto id = live() ? state_.navigation.detail_id.value_or(state_.navigation.selected_game_id)
                               : state_.navigation.selected_game_id;
        const auto assessment = assess_install(state_, id, live() ? install_target() : selected_volume_id_,
                                               live() ? steam::kInstallSpaceMargin : 0);
        if (live()) {
            if (live_install_allowed(assessment.readiness, free_space_supported_)) {
                if (id.valid()) pending_install_requests_.push_back({InstallRequestKind::Install, id, install_volume_id_});
                install_volume_id_.clear();
                close_modal();
            }
        } else if (assessment.readiness == InstallReadiness::UnknownSize || !assessment.download_bytes) {
            state_.notices.push_back({"estimate", "Download or install size remains unknown", "install-estimate-retry", true, 0});
        } else if (assessment.readiness != InstallReadiness::Ready) {
            state_.notices.push_back({"install-blocked", "Selected storage is unavailable or has insufficient space", "storage-picker", true, 0});
        } else {
            const auto existing = std::find_if(state_.jobs.begin(), state_.jobs.end(), [this](const Job& job) {
                return job.game_id == state_.navigation.selected_game_id && job.kind == "install"
                    && job.state != JobState::Cancelled && job.state != JobState::Downloaded && job.state != JobState::Failed;
            });
            if (existing == state_.jobs.end()) {
                const auto required = assessment.required_bytes.value_or(0);
                state_.jobs.push_back({next_job_id(), state_.navigation.selected_game_id, "install", next_queue_order(state_), JobStage::Download, JobState::Queued, 0, *assessment.download_bytes, std::nullopt, std::nullopt, {}, JobStage::Download, selected_volume_id_, 0, required, true, false});
                reconcile_storage(state_);
            }
            close_modal();
            state_.navigation.route = Route::Downloads;
            state_.navigation.focus_key = "job-" + state_.jobs.back().id;
        }
    } else if (action_name == "install-estimate-retry") {
        state_.notices.push_back({"estimate", "Fixture estimate is still unavailable", "install-estimate-retry", true, 0});
    } else if (action_name == "job-action" && selected.key == "job-kit-install") {
        open_modal({"job-actions", "Runtime kit", selected.key, {"kit-cancel", "cancel"}, 0, false});
    } else if (action_name == "job-action" && selected.key == "job-playden-update") {
        const auto& phase = state_.self_update.phase;
        std::vector<std::string> actions;
        if (phase == "staged") actions.push_back("self-update-restart");
        else actions.push_back("self-update-cancel");
        actions.push_back("cancel");
        open_modal({"job-actions", "Playden update", selected.key, std::move(actions), 0, false});
    } else if (action_name == "job-action") {
        const auto id = selected.key.substr(4);
        const auto* job = find_job(state_, id);
        std::vector<std::string> actions;
        if (job && (job->state == JobState::Downloading || job->state == JobState::Verifying)) actions.push_back("job-pause-" + id);
        if (job && (job->state == JobState::Paused || job->state == JobState::Interrupted)) actions.push_back("job-resume-" + id);
        const auto waiting = std::count_if(state_.jobs.begin(), state_.jobs.end(), [](const Job& other) {
            return other.state == JobState::Paused || other.state == JobState::Interrupted;
        });
        if (waiting > 1) actions.push_back("job-resume-all");
        if (job && job->state == JobState::Failed) actions.push_back("job-retry-" + id);
        // Another download is running or ahead in the queue: this one goes first, the running one waits behind it.
        const bool behind = job && std::any_of(state_.jobs.begin(), state_.jobs.end(), [job](const Job& other) {
            return &other != job && (other.state == JobState::Downloading || other.state == JobState::Verifying ||
                                     (other.state == JobState::Queued && other.queue_order < job->queue_order));
        });
        if (behind && (job->state == JobState::Queued || job->state == JobState::Paused || job->state == JobState::Interrupted))
            actions.insert(actions.begin(), "job-now-" + id);
        if (job && job->state == JobState::Queued) { actions.push_back("job-up-" + id); actions.push_back("job-down-" + id); }
        if (job && job->state == JobState::Downloaded) {
            actions.push_back("job-open-" + id); actions.push_back("job-dismiss-" + id);
            const auto finished = std::count_if(state_.jobs.begin(), state_.jobs.end(), [](const Job& other) { return other.state == JobState::Downloaded; });
            if (finished > 1) actions.push_back("job-clear-completed");
        }
        else if (job && job->state != JobState::Cancelled) actions.push_back("job-cancel-" + id);
        actions.push_back("logs"); actions.push_back("cancel");
        open_modal({"job-actions", "Download actions", selected.key, std::move(actions), 0, false});
    } else if (action_name == "details-prepare-next") {
        prepare_next(selected.game_id);
        state_.navigation.focus_key = selected.key;
    } else if (action_name == "queue-expand") {
        // The open section's heading closes it; the other's opens that one instead.
        state_.queue_expanded = state_.queue_expanded == selected.section ? kQueueCollapsed : selected.section;
    } else if (action_name == "prep-action") {
        // The same kind of menu as a download's: what this preparation allows, then Cancel.
        const auto& key = selected.key;
        const auto id = ToKey(selected.game_id);
        std::vector<std::string> actions;
        std::string title = "Preparation actions";
        // Clear completed is offered with more than one finished preparation or uninstall.
        const auto completed_count = [this] {
            return state_.preparation_queue.completed.size() +
                   static_cast<std::size_t>(std::count_if(state_.ended_uninstalls.begin(), state_.ended_uninstalls.end(),
                                                          [](const EndedUninstall& ended) { return ended.stopped.empty(); }));
        };
        if (key.rfind("prep-unin-", 0) == 0) {
            // An uninstall: its game's page while the library has it; one that ended leaves the list.
            if (find_game(state_, selected.game_id)) actions.push_back("prep-open-" + id);
            if (key.rfind("prep-unin-done-", 0) == 0) {
                actions.push_back("uninstall-dismiss-" + key.substr(std::strlen("prep-unin-done-")));
            } else if (const auto* stopped = uninstall_stop(selected.game_id); stopped && key.rfind("prep-unin-attn-", 0) == 0) {
                actions.push_back("uninstall-dismiss-" + id + "-" + std::to_string(stopped->finished_at));
            }
            if (key.rfind("prep-unin-done-", 0) == 0 && completed_count() > 1) actions.push_back("prep-clear-completed");
            title = "Uninstall actions";
        } else if (key.rfind("prep-done-", 0) == 0) {
            actions = {"prep-open-" + id, "prep-dismiss-" + key.substr(std::strlen("prep-done-"))};
            if (completed_count() > 1) actions.push_back("prep-clear-completed");
        } else if (key.rfind("prep-attn-", 0) == 0) {
            const auto runtime = state_.runtime_games.find(selected.game_id);
            // A setup an earlier kit left fails the same way on every Try again: the bottle is made again instead.
            if (runtime != state_.runtime_games.end() && runtime->second.setup_other_kit)
                actions.push_back("prep-recreate-" + id);
            else if (runtime != state_.runtime_games.end() && runtime->second.can_prepare && !runtime->second.awaiting_registration)
                actions.push_back("prep-retry-" + id);
            actions.push_back("prep-open-" + id);
        } else if (key.rfind("prep-active-", 0) == 0) {
            actions.push_back("prep-open-" + id);
        } else {
            // Prepare next waits for the game being prepared now; the first waiting game is next already.
            const auto& waiting = state_.preparation_queue.waiting;
            if (!waiting.empty() && waiting.front() != selected.game_id) actions.push_back("prep-next-" + id);
            actions.push_back("prep-open-" + id);
        }
        actions.push_back("cancel");
        open_modal({"prep-actions", std::move(title), key, std::move(actions), 0, false});
    } else if (action_name == "storage-details") {
        open_modal({"storage-details", selected.title, selected.key, {"storage-close"}, 0, false});
    } else if (action_name == "storage-close") {
        close_modal();
    } else if (action_name.rfind("prep-next-", 0) == 0) {
        close_modal();
        prepare_next(key_id(std::string_view(action_name).substr(std::strlen("prep-next-"))));
    } else if (action_name.rfind("prep-open-", 0) == 0) {
        const auto id = key_id(std::string_view(action_name).substr(std::strlen("prep-open-")));
        close_modal();
        state_.navigation.selected_game_id = id; state_.navigation.detail_id = id;
        state_.navigation.detail_origin = Route::Downloads; state_.navigation.route = Route::Details;
    } else if (action_name.rfind("prep-recreate-", 0) == 0) {
        // The same request as Maintenance › Recreate bottle. The runtime refuses to remove a title that runs,
        // so this doesn't wait for the console's answer on whether the game runs, as Maintenance's rows do.
        const auto id = key_id(std::string_view(action_name).substr(std::strlen("prep-recreate-")));
        const auto runtime = state_.runtime_games.find(id);
        if (!live() || runtime == state_.runtime_games.end() || !runtime->second.setup_other_kit ||
            runtime->second.maintenance_running)
            return;
        const auto* known = find_game(state_, id);
        if (!state_.navigation.modals.empty() && state_.navigation.modals.back().id == "prep-actions") close_modal();
        state_.navigation.selected_game_id = id;
        pending_destructive_action_ = "maintenance-recreate";
        open_modal(recreate_confirmation(known ? known->title : "the game", state_.navigation.focus_key));
    } else if (action_name.rfind("prep-retry-", 0) == 0) {
        const auto id = key_id(std::string_view(action_name).substr(std::strlen("prep-retry-")));
        close_modal();
        RequestRuntimePrepare(id);
    } else if (action_name.rfind("prep-dismiss-", 0) == 0) {
        // prep-dismiss-<game>-<finish time>: that finish only; preparing the game again lists it again.
        const auto rest = action_name.substr(std::strlen("prep-dismiss-"));
        const auto dash = rest.rfind('-');
        const LiveCompletedPreparation done{key_id(std::string_view(rest).substr(0, dash)),
                                            dash == std::string::npos ? 0 : std::strtoll(rest.c_str() + dash + 1, nullptr, 10)};
        close_modal();
        pending_prepared_dismissals_.push_back(done);
        prepared_dismissed_.push_back(done);
        std::erase(state_.preparation_queue.completed, done);
    } else if (action_name.rfind("uninstall-dismiss-", 0) == 0) {
        // uninstall-dismiss-<game>-<time>: that ended uninstall leaves Queue.
        const auto rest = action_name.substr(std::strlen("uninstall-dismiss-"));
        const auto dash = rest.rfind('-');
        const auto id = key_id(std::string_view(rest).substr(0, dash));
        const std::int64_t at = dash == std::string::npos ? 0 : std::strtoll(rest.c_str() + dash + 1, nullptr, 10);
        close_modal();
        std::erase_if(state_.ended_uninstalls, [&](const EndedUninstall& ended) { return ended.game_id == id && ended.finished_at == at; });
    } else if (action_name == "prep-clear-completed") {
        close_modal();
        pending_prepared_clear_ = true;
        auto& completed = state_.preparation_queue.completed;
        prepared_dismissed_.insert(prepared_dismissed_.end(), completed.begin(), completed.end());
        completed.clear();
        // Finished uninstalls go too; stopped ones still need the player.
        std::erase_if(state_.ended_uninstalls, [](const EndedUninstall& ended) { return ended.stopped.empty(); });
    } else if (action_name == "saves-filter") {
        state_.navigation.saves_filter = std::clamp(std::atoi(selected.key.c_str() + std::strlen("saves-filter-")), 0,
                                                    static_cast<int>(kSavesFilters.size()) - 1);
    } else if (live() && action_name == "saves-sync-all") {
        pending_cloud_requests_.push_back({state_.saves_job.kind == SavesJobKind::None ? CloudRequestKind::SyncAll
                                                                                     : CloudRequestKind::StopJob, {}});
    } else if (live() && action_name == "saves-upload-all") {
        ask_upload_all();
    } else if (live() && action_name == "cloud-confirm-account-all") {
        for (const auto id : saves_awaiting_account()) pending_cloud_requests_.push_back({CloudRequestKind::ConfirmAccount, id});
        close_modal();
    } else if (live() && action_name == "manage-saves") {
        const auto id = state_.navigation.detail_id.value_or(state_.navigation.selected_game_id);
        state_.navigation.modals.clear();
        state_.navigation.save_id = id;
        state_.navigation.route = Route::SaveDetails;
        state_.navigation.selected_index = 0;
        state_.navigation.focus_key.clear();
    } else if (action_name == "save-open") {
        state_.navigation.save_id = selected.game_id;
        state_.navigation.route = Route::SaveDetails;
        state_.navigation.selected_index = 0;
        state_.navigation.focus_key.clear();
    } else if (live() && (action_name == "saves-use-cloud" || action_name == "saves-use-local" ||
                          action_name == "saves-confirm-account" || action_name == "saves-sync")) {
        const auto kind = action_name == "saves-use-cloud" ? CloudRequestKind::UseCloud
                          : action_name == "saves-use-local" ? CloudRequestKind::UseLocal
                          : action_name == "saves-confirm-account" ? CloudRequestKind::ConfirmAccount : CloudRequestKind::Sync;
        if (state_.navigation.save_id.valid()) pending_cloud_requests_.push_back({kind, state_.navigation.save_id});
    } else if (live() && (action_name == "saves-delete" || action_name == "saves-leftover")) {
        const auto id = state_.navigation.save_id;
        const auto cloud = state_.cloud.find(id);
        const bool in_cloud = cloud != state_.cloud.end() && cloud->second.remote_files > 0;
        const auto* game = find_game(state_, id);
        std::string name = game ? game->title : std::string("this game");
        if (action_name == "saves-delete") {
            ModalEntry confirm{"confirmation", "Delete saves on this console?", state_.navigation.focus_key,
                               {"cancel", "saves-delete-confirm"}, 0, true};
            const std::size_t files = cloud == state_.cloud.end() ? 0 : cloud->second.local_files;
            confirm.body = "Deletes " + std::to_string(files) + (files == 1 ? " save file" : " save files") +
                           (cloud == state_.cloud.end() ? std::string() : " (" + save_size(cloud->second.local_bytes) + ")") +
                           " of " + name + ". The game stays installed and a backup is kept. " +
                           (in_cloud ? "Steam Cloud keeps its copy." : "Steam Cloud has no copy.");
            open_modal(std::move(confirm));
        } else {
            std::uint64_t bytes = 0;
            for (const auto& kept : state_.kept_bottles) if (kept.game_id == id) { bytes = kept.bytes; name = kept_name(state_, kept); }
            ModalEntry confirm{"confirmation", "Remove leftover data?", state_.navigation.focus_key,
                               {"cancel", "saves-leftover-confirm"}, 0, true};
            confirm.body = "Frees " + format_bytes(bytes) + ": " + name + "'s saves, Windows folder, settings and shader caches. " +
                           (in_cloud ? "Steam Cloud keeps a copy of its saves; reinstalling brings them back."
                                     : "Steam Cloud has no copy. These saves will be gone for good.");
            open_modal(std::move(confirm));
        }
    } else if (live() && (action_name == "saves-delete-confirm" || action_name == "saves-leftover-confirm")) {
        const auto id = state_.navigation.save_id;
        close_modal();
        if (id.valid())
            pending_cloud_requests_.push_back({action_name == "saves-delete-confirm" ? CloudRequestKind::RemoveLocal
                                                                                   : CloudRequestKind::RemoveLeftover, id});
        if (action_name == "saves-leftover-confirm") {
            state_.navigation.route = Route::Saves;
            state_.navigation.focus_key.clear();
        }
    } else if (live() && action_name == "saves-quit-anyway") {
        quit_sync_game_ = {};
        state_.navigation.modals.clear();
        quit_to_home();
    } else if (live() && action_name == "cloud-sync") {
        const auto id = state_.navigation.detail_id.value_or(state_.navigation.selected_game_id);
        if (id.valid() && Traits(id.store).cloud_saves) pending_cloud_requests_.push_back({CloudRequestKind::Sync, id});
    } else if (live() && action_name == "cloud-choose") {
        const auto id = state_.navigation.detail_id.value_or(state_.navigation.selected_game_id);
        const auto cloud = state_.cloud.find(id);
        if (cloud != state_.cloud.end() && cloud->second.status == CloudStatus::Conflict) {
            const auto& facts = cloud->second;
            const auto age = [this](std::int64_t when) -> std::string {
                if (when <= 0 || state_.clock_ticks <= 0) return "";
                const auto days = (state_.clock_ticks - when) / 86'400;
                return days <= 0 ? ", changed today" : days == 1 ? ", changed yesterday" : ", changed " + std::to_string(days) + " days ago";
            };
            // One side may have nothing (a fresh install): "no saves", not "0 files, ".
            const auto side = [&](std::size_t files, std::uint64_t bytes, std::int64_t latest) -> std::string {
                if (files == 0) return "no saves";
                const auto size = format_bytes(bytes);
                return std::to_string(files) + (files == 1 ? " file" : " files") + (size.empty() ? "" : ", " + size) + age(latest);
            };
            ModalEntry choice{"confirmation", "Which saves should Playden keep?", state_.navigation.focus_key,
                              {"cloud-use-cloud", "cloud-use-local", "cancel"}, 0, false};
            choice.body = "Steam Cloud: " + side(facts.remote_files, facts.remote_bytes, facts.remote_latest) +
                          ". This console: " + side(facts.local_files, facts.local_bytes, facts.local_latest) +
                          ". Saves that get replaced here are backed up first.";
            open_modal(std::move(choice));
        } else if (cloud != state_.cloud.end() && cloud->second.status == CloudStatus::NeedsAccount) {
            ModalEntry confirm{"confirmation", "Upload saves to this Steam account?", state_.navigation.focus_key,
                               {"cloud-confirm-account", "cancel"}, 0, false};
            confirm.body = "Playden has not synced this game's saves for " +
                           (state_.account().display_name.empty() ? std::string("this account") : state_.account().display_name) +
                           " before. Confirm to upload this console's saves to its Steam Cloud.";
            open_modal(std::move(confirm));
        }
    } else if (live() && (action_name == "cloud-use-cloud" || action_name == "cloud-use-local" || action_name == "cloud-confirm-account")) {
        const auto id = state_.navigation.detail_id.value_or(state_.navigation.selected_game_id);
        const auto kind = action_name == "cloud-use-cloud" ? CloudRequestKind::UseCloud
                          : action_name == "cloud-use-local" ? CloudRequestKind::UseLocal
                                                             : CloudRequestKind::ConfirmAccount;
        if (id.valid()) pending_cloud_requests_.push_back({kind, id});
        close_modal();
    } else if (live() && action_name == "runtime-check-again") {
        const auto id = state_.navigation.detail_id.value_or(state_.navigation.selected_game_id);
        close_modal();
        if (id.valid()) pending_runtime_prepare_requests_.push_back(id);
    } else if (live() && action_name == "runtime-update") {
        // The button goes once the update starts; Play shows its progress, so focus moves there.
        const auto id = state_.navigation.detail_id.value_or(state_.navigation.selected_game_id);
        if (id.valid()) pending_runtime_prepare_requests_.push_back(id);
        state_.navigation.focus_key = "details-play";
    } else if (live() && action_name == "runtime-prepare") {
        const auto id = state_.navigation.detail_id.value_or(state_.navigation.selected_game_id);
        if (id.valid()) pending_runtime_prepare_requests_.push_back(id);
    } else if (live() && action_name == "runtime-remove-retry") {
        // Continues the saved removal (with its first choice about saves), then removes the download.
        const auto id = state_.navigation.detail_id.value_or(state_.navigation.selected_game_id);
        if (id.valid()) request_uninstall(id, false, false);
    } else if (live() && action_name == "runtime-cleanup") {
        // Deleting data asks first, as the row's disclosure promises.
        const auto count = state_.runtime_leftover_requests;
        ModalEntry confirm{"confirmation", "Clean up unused game settings data?", state_.navigation.focus_key,
                           {"cancel", "runtime-cleanup-confirm"}, 0, true};
        confirm.body = std::to_string(count) + (count == 1 ? " request" : " requests") +
                       " left by interrupted or finished preparations. Playden removes only what no game is using; "
                       "games and saves are not touched.";
        open_modal(std::move(confirm));
    } else if (live() && action_name == "runtime-cleanup-confirm") {
        pending_runtime_cleanup_ = true;
        close_modal();
        push_notice({"runtime-cleanup", "Unused game settings data cleaned up", "", false, kLiveNoticeSeconds, false, ""});
    } else if (live() && action_name == "runtime-play") {
        const auto id = state_.navigation.detail_id.value_or(state_.navigation.selected_game_id);
        if (id.valid()) {
            // A new attempt may fail for the same reason; only background refreshes
            // should be deduplicated against the previous outcome.
            if (auto previous = state_.runtime_games.find(id); previous != state_.runtime_games.end())
                previous->second.play_detail.clear();
            pending_runtime_play_requests_.push_back(id);
        }
    } else if (live() && action_name == "live-install") {
        const auto id = state_.navigation.detail_id.value_or(state_.navigation.selected_game_id);
        // Install at once when the game fits the default drive (or its size is checked later);
        // otherwise the sheet says why and offers another drive for this install.
        install_volume_id_.clear();
        if (id.valid() && live_install_allowed(assess_install(state_, id, selected_volume_id_, steam::kInstallSpaceMargin).readiness,
                                            free_space_supported_))
            pending_install_requests_.push_back({InstallRequestKind::Install, id});
        else if (id.valid())
            open_modal({"install", "Install game", "details-install", {"install-confirm", "storage-picker", "cancel"}, 0, false});
    } else if (live() && action_name == "steam-programs") {
        open_steam_programs(state_.navigation.detail_id.value_or(state_.navigation.selected_game_id));
    } else if (live() && action_name.rfind("steam-program-", 0) == 0) {
        const auto index = static_cast<std::size_t>(std::stoul(action_name.substr(14)));
        if (index >= steam_programs_shown_.size()) return;
        const auto id = steam_programs_game_;
        close_modal();
        choose_steam_launch(id, steam_programs_shown_[index].path);
    } else if (live() && action_name == "open-downloads") {
        state_.navigation.route = Route::Downloads;
        state_.navigation.focus_key = "job-" + ToKey(state_.navigation.detail_id.value_or(GameId{}));
    } else if (action_name == "job-resume-all") {
        // Every paused download goes back in the queue, in its current order.
        for (auto& job : state_.jobs) {
            if (job.state != JobState::Paused && job.state != JobState::Interrupted) continue;
            if (live()) pending_install_requests_.push_back({InstallRequestKind::Resume, job.game_id});
            else job.state = JobState::Queued;
        }
        close_modal();
    } else if (live() && (action_name.rfind("job-pause-", 0) == 0 || action_name.rfind("job-resume-", 0) == 0 ||
                          action_name.rfind("job-retry-", 0) == 0)) {
        const auto dash = action_name.find('-', 4);
        if (const auto* job = find_job(state_, action_name.substr(dash + 1))) {
            const auto kind = action_name.rfind("job-pause-", 0) == 0 ? InstallRequestKind::Pause : InstallRequestKind::Resume;
            pending_install_requests_.push_back({kind, job->game_id});
        }
        // Retry from a job's log dialog also leaves the job's actions behind it.
        if (!state_.navigation.modals.empty() && state_.navigation.modals.back().id == "logs") close_modal();
        close_modal();
    } else if (action_name.rfind("job-pause-", 0) == 0) { if (auto* job = find_job(state_, action_name.substr(10))) job->state = JobState::Pausing; close_modal(); }
    else if (action_name.rfind("job-resume-", 0) == 0) {
        if (auto* job = find_job(state_, action_name.substr(11))) {
            for (auto& other : state_.jobs) if (&other != job && (other.state == JobState::Downloading || other.state == JobState::Verifying)) other.state = JobState::Queued;
            job->state = job->stage == JobStage::Download ? JobState::Downloading : JobState::Verifying;
        }
        close_modal();
    }
    else if (action_name.rfind("job-retry-", 0) == 0) {
        if (auto* job = find_job(state_, action_name.substr(10))) {
            for (auto& other : state_.jobs) if (&other != job && (other.state == JobState::Downloading || other.state == JobState::Verifying)) other.state = JobState::Queued;
            job->stage = job->retry_stage; job->state = job->stage == JobStage::Download ? JobState::Downloading : JobState::Verifying; job->failure.clear();
        }
        close_modal();
    }
    else if (action_name.rfind("job-now-", 0) == 0) {
        if (auto* job = find_job(state_, action_name.substr(8))) {
            if (live()) {
                pending_install_requests_.push_back({InstallRequestKind::DownloadNow, job->game_id});
            } else {
                for (auto& other : state_.jobs) {
                    if (&other == job) continue;
                    if (other.state == JobState::Downloading || other.state == JobState::Verifying) other.state = JobState::Queued;
                    if (other.queue_order < job->queue_order) ++other.queue_order;
                }
                job->queue_order = 0;
                for (auto& other : state_.jobs) if (&other != job && other.queue_order == 0) ++other.queue_order;
                job->state = job->stage == JobStage::Download ? JobState::Downloading : JobState::Verifying;
            }
        }
        close_modal();
    }
    else if (action_name.rfind("job-up-", 0) == 0 || action_name.rfind("job-down-", 0) == 0) {
        const bool up = action_name.rfind("job-up-", 0) == 0;
        const auto id = action_name.substr(up ? 7 : 9);
        if (auto* job = find_job(state_, id)) {
            if (live()) {
                pending_install_requests_.push_back({up ? InstallRequestKind::MoveUp : InstallRequestKind::MoveDown, job->game_id});
            } else {
                const int wanted = job->queue_order + (up ? -1 : 1);
                auto other = std::find_if(state_.jobs.begin(), state_.jobs.end(), [wanted](const Job& value) { return value.queue_order == wanted; });
                if (other != state_.jobs.end()) std::swap(job->queue_order, other->queue_order);
            }
        }
        close_modal();
    } else if (action_name.rfind("job-open-", 0) == 0) {
        if (const auto* job = find_job(state_, action_name.substr(9))) {
            state_.navigation.selected_game_id = job->game_id; state_.navigation.detail_id = job->game_id; state_.navigation.detail_origin = Route::Downloads; state_.navigation.route = Route::Details;
        }
        close_modal();
    } else if (action_name == "job-clear-completed") {
        close_modal();
        if (live()) pending_install_requests_.push_back({InstallRequestKind::ClearCompleted, {}});
        state_.jobs.erase(std::remove_if(state_.jobs.begin(), state_.jobs.end(), [](const Job& value) { return value.state == JobState::Downloaded; }), state_.jobs.end());
    } else if (action_name.rfind("job-dismiss-", 0) == 0) {
        const auto id = action_name.substr(12);
        close_modal();
        if (const auto* job = find_job(state_, id); live() && job && job->state == JobState::Downloaded)
            pending_install_requests_.push_back({InstallRequestKind::DismissCompleted, job->game_id});
        state_.jobs.erase(std::remove_if(state_.jobs.begin(), state_.jobs.end(), [&id](const Job& value) { return value.id == id && value.state == JobState::Downloaded; }), state_.jobs.end());
    } else if (action_name.rfind("job-cancel-", 0) == 0) {
        pending_destructive_action_ = action_name;
        ModalEntry confirmation{"confirmation", "Cancel download?", selected.key, {"cancel", "confirm-destructive"}, 0, true};
        confirmation.body = "Stops the download and deletes what was downloaded so far. Installing it again starts from the beginning.";
        open_modal(std::move(confirmation));
    } else if (action_name == "verify") {
        const auto duplicate = std::find_if(state_.jobs.begin(), state_.jobs.end(), [this](const Job& job) {
            return job.game_id == state_.navigation.selected_game_id && job.kind == "verify"
                && job.state != JobState::Downloaded && job.state != JobState::Cancelled && job.state != JobState::Failed;
        });
        if (duplicate == state_.jobs.end())
            state_.jobs.push_back({next_job_id(), state_.navigation.selected_game_id, "verify", next_queue_order(state_), JobStage::Verify, JobState::Queued, 0, 0, std::nullopt, std::nullopt, {}, JobStage::Verify, {}, 0, 0, false, false});
        while (!state_.navigation.modals.empty()) close_modal();
        state_.navigation.route = Route::Downloads;
    } else if (action_name == "uninstall" || action_name == "reset-confirm" || action_name == "signout-confirm" || action_name == "quit-confirm" || action_name == "quit-game") {
        pending_destructive_action_ = action_name;
        std::string title;
        if (action_name == "uninstall") title = "Uninstall this fixture game? Saves are preserved.";
        else if (action_name == "reset-confirm") title = "Reset preferences and organization? Installs, saves, and history remain.";
        else if (action_name == "signout-confirm") title = "Sign out? Account-visible cache will be cleared.";
        else if (action_name == "quit-confirm") title = "Quit Playden? Active jobs will recover after restart.";
        else title = "Quit the simulated game? Download jobs continue.";
        std::string body;
        if (live()) {
            // Live wording describes what actually happens; no simulated jobs or fixtures exist.
            if (action_name == "signout-confirm") {
                title = "Sign out of Steam?";
                body = "The saved sign-in, this account's library cache and its organization are removed from this console. Signing in again needs the Steam mobile app.";
            } else if (action_name == "quit-confirm") {
                title = "Quit Playden?";
                body = "Your sign-in, library and settings are kept for next time.";
            } else if (action_name == "reset-confirm") {
                body = "Favorites, collections, notes and display preferences return to their defaults.";
            } else if (action_name == "uninstall") {
                const auto id = state_.navigation.selected_game_id;
                const auto runtime_game = state_.runtime_games.find(id);
                const bool has_title = runtime_game != state_.runtime_games.end() && runtime_game->second.has_title;
                const auto cloud = state_.cloud.find(id);
                const bool no_cloud = !Traits(id.store).cloud_saves ||
                                      (cloud != state_.cloud.end() && cloud->second.status == CloudStatus::Unsupported);
                title = "Uninstall this game?";
                const bool steam = id.store == Store::kSteam;
                body = has_title ? (id.store == Store::kLocal ? uninstall_body_kept_imported : uninstall_body_kept)
                     : steam ? "The game's files are removed from this console. You can install it again from Steam."
                             : "The game's files are removed from this console.";
                // Only a store with Cloud saves is worth a word about them. With automatic sync off the
                // uninstall does not sync them (Controller::BeginUninstall).
                if (!Traits(id.store).cloud_saves) body += " Its saves exist only on this console.";
                else if (no_cloud) body += " This game has no Steam Cloud saves: its saves exist only on this console.";
                else if (state_.cloud_saves_auto) body += " Steam Cloud saves are synced first; if that fails, Playden asks before going on.";
                else body += " Automatic Cloud sync is off, so its saves are not synced first.";
            }
        }
        std::vector<std::string> actions{"cancel", "confirm-destructive"};
        if (live() && action_name == "uninstall") {
            uninstall_delete_saves_ = false;
            const auto runtime_game = state_.runtime_games.find(state_.navigation.selected_game_id);
            if (runtime_game != state_.runtime_games.end() && runtime_game->second.has_title)
                actions = {"cancel", "uninstall-delete-saves", "confirm-destructive"};
        }
        ModalEntry confirmation{"confirmation", std::move(title), state_.navigation.focus_key, std::move(actions), 0, true};
        confirmation.body = std::move(body);
        open_modal(std::move(confirmation));
    } else if (action_name == "uninstall-delete-saves") {
        uninstall_delete_saves_ = !uninstall_delete_saves_;
        // The dialog says what happens to the saves with the current choice.
        if (!state_.navigation.modals.empty()) {
            auto& body = state_.navigation.modals.back().body;
            const bool batch = pending_destructive_action_ == "installed-selection-uninstall";
            const std::string_view kept = batch ? uninstall_batch_body_kept
                : state_.navigation.selected_game_id.store == Store::kLocal ? uninstall_body_kept_imported : uninstall_body_kept;
            const std::string_view deleted = batch ? uninstall_batch_body_deleted : uninstall_body_deleted;
            const std::string_view from = uninstall_delete_saves_ ? kept : deleted;
            const std::string_view to = uninstall_delete_saves_ ? deleted : kept;
            if (const auto at = body.find(from); at != std::string::npos) body.replace(at, from.size(), to);
        }
    } else if (live() && action_name == "quit-to-home") {
        // Ready to play: ShadowMountPlus registers the title once Playden quits, and the
        // game appears on the Home screen. The dialog or Details already said so. Its saves
        // sync first, as before any Play, unless the game has no Steam Cloud or automatic sync is off.
        const GameId id = !state_.navigation.modals.empty() && ready_prompt_game_.valid()
            ? ready_prompt_game_ : state_.navigation.detail_id.value_or(state_.navigation.selected_game_id);
        const auto cloud = state_.cloud.find(id);
        // With automatic sync off, saves move only when the player chooses Sync in Saves.
        const bool syncs = id.valid() && Traits(id.store).cloud_saves && state_.cloud_saves_auto &&
                           state_.account().authentication == AuthState::SignedIn &&
                           (cloud == state_.cloud.end() || cloud->second.status != CloudStatus::Unsupported);
        state_.navigation.modals.clear();
        ready_prompt_game_ = {};
        if (!syncs) {
            quit_to_home();
        } else {
            quit_sync_game_ = id;
            pending_cloud_requests_.push_back({CloudRequestKind::Sync, id});
            ModalEntry wait{"confirmation", "Syncing Cloud saves…", state_.navigation.focus_key, {"saves-quit-anyway", "cancel"}, 1, false};
            wait.body = "Playden quits to the PS5 Home screen once this game's saves are in step with Steam Cloud.";
            open_modal(std::move(wait));
        }
    } else if (action_name == "ready-later" || action_name == "alert-ok") {
        close_modal();
    } else if (action_name == "alert-settings") {
        const auto game = alert_settings_game_;
        close_modal();
        if (game.valid() && find_game(state_, game)) {
            state_.navigation.selected_game_id = game;
            open_game_settings();
        }
    } else if (live() && action_name == "installed-manage") {
        if (state_.installations.empty()) return;
        installed_selected_.clear();
        installed_message_.clear();
        installed_list_focus_.clear();
        auto actions = installed_manager_actions();
        // Focus starts on the first game; Sort and Select all are above it.
        open_modal({"installed-games", "Installed games", state_.navigation.focus_key, std::move(actions), 2, false});
    } else if (action_name == "installed-close") {
        close_modal();
    } else if (live() && action_name == "installed-sort") {
        installed_by_played_ = !installed_by_played_;
        refresh_installed_manager();
    } else if (live() && action_name == "installed-select-all") {
        std::vector<GameId> choosable;
        for (const auto id : installed_list())
            if (find_installation(state_, id) && installed_blocker(id).empty()) choosable.push_back(id);
        const bool all = !choosable.empty() && std::all_of(choosable.begin(), choosable.end(),
                                                           [this](GameId id) { return installed_selected_.contains(id); });
        if (all) installed_selected_.clear();
        else installed_selected_.insert(choosable.begin(), choosable.end());
    } else if (live() && action_name.rfind("installed-toggle-", 0) == 0) {
        const auto id = key_id(std::string_view(action_name).substr(17));
        if (find_installation(state_, id) && installed_blocker(id).empty() && !installed_selected_.erase(id)) installed_selected_.insert(id);
    } else if (live() && action_name == "installed-selection-uninstall") {
        if (installed_selected_.empty()) return;
        std::vector<std::string> names;
        std::uint64_t bytes = 0;
        bool any_title = false, any_cloud = false, all_steam = true;
        for (const auto id : installed_list()) {
            if (!installed_selected_.contains(id)) continue;
            any_cloud = any_cloud || Traits(id.store).cloud_saves;
            all_steam = all_steam && id.store == Store::kSteam;
            const auto* game = find_game(state_, id);
            names.push_back(game && !game->title.empty() ? game->title : "Steam app " + ToKey(id));
            if (const auto* installation = find_installation(state_, id)) bytes += installation->installed_bytes;
            const auto runtime_game = state_.runtime_games.find(id);
            any_title = any_title || (runtime_game != state_.runtime_games.end() && runtime_game->second.has_title);
        }
        // "A, B and C", or "A, B, C and 2 more".
        std::string listed;
        const auto shown = names.size() > 3 ? std::size_t{3} : names.size();
        for (std::size_t index = 0; index < shown; ++index)
            listed += (index == 0 ? "" : index + 1 == shown && names.size() <= 3 ? " and " : ", ") + names[index];
        if (names.size() > 3) listed += " and " + std::to_string(names.size() - 3) + " more";
        pending_destructive_action_ = action_name;
        uninstall_delete_saves_ = false;
        ModalEntry confirmation{"confirmation", "Uninstall " + kept_count(names.size()) + "?", state_.navigation.focus_key,
                                any_title ? std::vector<std::string>{"cancel", "uninstall-delete-saves", "confirm-destructive"}
                                          : std::vector<std::string>{"cancel", "confirm-destructive"},
                                0, true};
        confirmation.body = listed + ". Frees about " + format_bytes(bytes) + ". ";
        confirmation.body += any_title ? uninstall_batch_body_kept
                           : all_steam ? "Their files are removed from this console. You can install them again from Steam."
                                       : "Their files are removed from this console.";
        if (any_cloud && state_.cloud_saves_auto)
            confirmation.body += " Steam Cloud saves are synced first. If that fails for a game, that game stays installed and Queue says why.";
        else if (any_cloud)
            confirmation.body += " Automatic Cloud sync is off, so their saves are not synced first.";
        open_modal(std::move(confirmation));
    } else if (action_name == "confirm-destructive" && live() && pending_destructive_action_ == "installed-selection-uninstall") {
        // In the list's order (its current sort), which is the order they run in.
        std::vector<GameId> started;
        installed_message_.clear();
        for (const auto id : installed_list()) {
            if (!installed_selected_.contains(id) || !installed_blocker(id).empty() || !find_installation(state_, id)) continue;
            request_uninstall(id, uninstall_delete_saves_, true);
            started.push_back(id);
        }
        installed_selected_.clear();
        uninstall_delete_saves_ = false;
        pending_destructive_action_.clear();
        if (started.empty()) {
            // Every checked game started running or downloading while the dialog was open.
            installed_message_ = "None of the selected games can be uninstalled now.";
            close_modal();  // back to the list, which says so
            refresh_installed_manager();
        } else {
            // Queue follows the uninstalls: the list closes, back to Settings.
            state_.navigation.focus_key = state_.navigation.modals.front().opener_focus_key;
            state_.navigation.modals.clear();
            const auto* game = started.size() == 1 ? find_game(state_, started.front()) : nullptr;
            push_notice({"uninstall-batch", "Uninstalling " + (game && !game->title.empty() ? game->title : kept_count(started.size())),
                         "", false, kLiveNoticeSeconds, false, started.size() == 1 ? "Follow it in Queue" : "Follow them in Queue"});
        }
    } else if (action_name == "import-manage") {
        open_import_sheet();
    } else if (action_name == "import-look") {
        look_for_games();
    } else if (action_name.rfind("import-candidate-", 0) == 0) {
        const auto* candidate = import_candidate(action_name);
        if (!candidate) return;
        if (candidate->imported.valid()) {
            // Added already: its page, with the sheets closed.
            open_game_page(candidate->imported);
            return;
        }
        if (pending_imports_.count(action_name)) return;  // being added
        if (candidate->programs.empty()) return;
        import_programs_key_ = action_name;
        // One program that isn't an installer or tool: no list, straight to where the files live.
        if (const auto clear = playden::stores::local::ClearGameProgram(candidate->programs)) {
            open_import_choice(*clear);
            import_program_chosen_ = true;
            return;
        }
        std::vector<std::string> actions;
        for (std::size_t index = 0; index < candidate->programs.size(); ++index)
            actions.push_back("import-program-" + std::to_string(index));
        actions.push_back("cancel");
        open_modal({"import-programs", candidate->title, state_.navigation.focus_key, std::move(actions), 0, false});
    } else if (action_name.rfind("import-program-", 0) == 0) {
        const auto* candidate = import_candidate(import_programs_key_);
        const auto index = static_cast<std::size_t>(std::stoul(action_name.substr(15)));
        if (!candidate || index >= candidate->programs.size()) return;
        // Where the game plays from: its folder, a copy or a move into Playden.
        open_import_choice(index);
    } else if (action_name == "import-play-in-place" || action_name == "import-copy") {
        choose_import(action_name);
    } else if (action_name == "import-copy-game") {
        // An imported game playing from its folder: Import into Playden opens the drive choice.
        const auto id = state_.navigation.detail_id.value_or(state_.navigation.selected_game_id);
        if (const auto* external = external_game(id); external && external->available && !import_copy_job(id)) open_import_storage(id);
    } else if (action_name == "import-cancel-copy") {
        // The same cancel as Queue's, from the game's page.
        const auto id = state_.navigation.detail_id.value_or(state_.navigation.selected_game_id);
        const auto* job = import_copy_job(id);
        if (!job) return;
        pending_destructive_action_ = "job-cancel-" + job->id;
        ModalEntry confirmation{"confirmation", "Cancel the copy?", state_.navigation.focus_key, {"cancel", "confirm-destructive"}, 0, true};
        confirmation.body = "Stops the copy and deletes what was copied so far. The game keeps playing from its folder.";
        open_modal(std::move(confirmation));
    } else if (action_name == "import-open-copy") {
        const auto id = state_.navigation.detail_id.value_or(state_.navigation.selected_game_id);
        state_.navigation.route = Route::Downloads;
        if (const auto* job = import_copy_job(id)) state_.navigation.focus_key = "job-" + job->id;
    } else if (action_name == "import-remove") {
        // Out of the library; the folder stays where it is.
        const auto id = state_.navigation.detail_id.value_or(state_.navigation.selected_game_id);
        const auto* game = find_game(state_, id);
        if (!game || !external_game(id)) return;
        pending_destructive_action_ = action_name;
        ModalEntry confirmation{"confirmation", "Remove " + game->title + " from the library?", state_.navigation.focus_key,
                                {"cancel", "confirm-destructive"}, 0, true};
        confirmation.body = "The game's folder and files stay where they are, and Import games can add it again. Its PS5 "
                            "title is removed; its saves and settings on this console are kept.";
        open_modal(std::move(confirmation));
    } else if (live() && action_name == "kept-manage") {
        if (state_.kept_bottles.empty()) return;
        kept_selected_.clear();
        open_modal({"kept-data", "Kept game data", state_.navigation.focus_key, kept_manager_actions(), 0, false});
    } else if (live() && action_name == "kept-select-all") {
        if (kept_selected_.size() == state_.kept_bottles.size()) kept_selected_.clear();
        else for (const auto& kept : state_.kept_bottles) kept_selected_.insert(kept.game_id);
    } else if (live() && action_name.rfind("kept-toggle-", 0) == 0) {
        const auto id = key_id(std::string_view(action_name).substr(12));
        if (!kept_selected_.erase(id)) kept_selected_.insert(id);
    } else if (live() && action_name == "kept-selection-delete") {
        if (kept_selected_.empty()) return;
        std::uint64_t bytes = 0;
        std::string only;
        for (const auto& kept : state_.kept_bottles)
            if (kept_selected_.contains(kept.game_id)) { bytes += kept.bytes; only = kept_name(state_, kept); }
        pending_destructive_action_ = action_name;
        ModalEntry confirmation{"confirmation", "Delete kept game data?", state_.navigation.focus_key, {"cancel", "confirm-destructive"}, 0, true};
        confirmation.body = "The saves and settings kept from " + (kept_selected_.size() == 1 ? only : kept_count(kept_selected_.size())) +
                            " (" + format_bytes(bytes) + ") are deleted from this console. Saves in Steam Cloud stay in Steam Cloud.";
        open_modal(std::move(confirmation));
    } else if (action_name == "confirm-destructive" && live() && pending_destructive_action_ == "kept-selection-delete") {
        for (const auto& kept : state_.kept_bottles)
            if (kept_selected_.contains(kept.game_id)) pending_kept_delete_requests_.push_back(kept.game_id);
        kept_selected_.clear();
        pending_destructive_action_.clear();
        state_.navigation.focus_key = state_.navigation.modals.front().opener_focus_key;
        state_.navigation.modals.clear();
    } else if (live() && action_name.rfind("kept-delete-", 0) == 0) {
        pending_destructive_action_ = action_name;
        const auto id = key_id(std::string_view(action_name).substr(12));
        std::string name;
        for (const auto& kept : state_.kept_bottles) if (kept.game_id == id) name = kept.name;
        if (name.empty()) if (const auto* game = find_game(state_, id)) name = game->title;
        ModalEntry confirmation{"confirmation", "Delete kept saves?", state_.navigation.focus_key, {"cancel", "confirm-destructive"}, 0, true};
        confirmation.body = "The saves and settings kept from " + (name.empty() ? std::string("this game") : name) +
                            " are deleted from this console. Saves in Steam Cloud stay in Steam Cloud.";
        open_modal(std::move(confirmation));
    } else if (action_name == "confirm-destructive" && pending_destructive_action_.rfind("maintenance-", 0) == 0) {
        const auto chosen = pending_destructive_action_;
        pending_destructive_action_.clear();
        close_modal();  // back to the maintenance sheet, which shows the progress
        const auto kind = chosen == "maintenance-repair" ? MaintenanceKind::Repair
                        : chosen == "maintenance-shader" ? MaintenanceKind::ClearShaderCache
                        : chosen == "maintenance-backup" ? MaintenanceKind::BackupSaves
                        : chosen == "maintenance-recreate" ? MaintenanceKind::RecreateBottle
                        : chosen == "maintenance-update" ? MaintenanceKind::UpdateRuntime
                                                           : MaintenanceKind::ApplySettings;
        if (chosen == "maintenance-reset") {
            auto& record = state_.game_settings[state_.navigation.selected_game_id];
            if (has_resettable(record.overrides) && can_change_game_settings()) {
                clear_keeping_start_choices(&record.overrides);
                ++record.revision;
                if (save_profile() || !live()) {
                    push_notice({"settings-reset", "Settings reset", "", false, kLiveNoticeSeconds, false,
                                 "The preset's values apply on next launch"});
                    if (live() && game_prepared(state_.navigation.selected_game_id))
                        pending_maintenance_requests_.push_back({state_.navigation.selected_game_id, MaintenanceKind::ApplySettings});
                }
            }
        } else {
            request_maintenance(state_.navigation.selected_game_id, kind);
        }
    } else if (action_name == "confirm-destructive" && pending_destructive_action_ == "import-remove") {
        const auto id = state_.navigation.detail_id.value_or(state_.navigation.selected_game_id);
        pending_destructive_action_.clear();
        state_.navigation.modals.clear();
        RequestImport({ImportRequestKind::Remove, id});
        if (!live()) {
            // Fixture mode: the game leaves the library at once and its folder can be added again.
            std::erase_if(state_.external_games, [id](const LiveExternalGame& external) { return external.game_id == id; });
            std::erase_if(state_.games, [id](const Game& game) { return game.id == id; });
            for (auto& candidate : state_.import_candidates)
                if (candidate.imported == id) candidate.imported = {};
        }
        // Back where the page was opened from; the library loses the game on its next refresh.
        state_.navigation.route = state_.navigation.detail_origin == Route::Details ? Route::Home : state_.navigation.detail_origin;
        state_.navigation.selected_game_id = {};
        state_.navigation.detail_id.reset();
        state_.navigation.focus_key.clear();
    } else if (action_name == "confirm-destructive" && live() && pending_destructive_action_.rfind("kept-delete-", 0) == 0) {
        pending_kept_delete_requests_.push_back(key_id(std::string_view(pending_destructive_action_).substr(12)));
        pending_destructive_action_.clear();
        state_.navigation.focus_key = state_.navigation.modals.front().opener_focus_key;
        state_.navigation.modals.clear();
    } else if (action_name == "confirm-destructive" && live() &&
               (pending_destructive_action_.rfind("job-cancel-", 0) == 0 || pending_destructive_action_ == "uninstall")) {
        if (pending_destructive_action_ == "uninstall") {
            request_uninstall(state_.navigation.selected_game_id, uninstall_delete_saves_, false);
            uninstall_delete_saves_ = false;
        } else if (const auto* job = find_job(state_, pending_destructive_action_.substr(11))) {
            pending_install_requests_.push_back({InstallRequestKind::Cancel, job->game_id});
        }
        pending_destructive_action_.clear();
        state_.navigation.modals.clear();
    } else if (action_name == "confirm-destructive") {
        if (pending_destructive_action_.rfind("job-cancel-", 0) == 0) {
            if (auto* job = find_job(state_, pending_destructive_action_.substr(11))) { job->state = JobState::Cancelled; job->reservation_held = false; }
            reconcile_storage(state_);
        }
        else if (pending_destructive_action_ == "uninstall") {
            if (const auto* installation = find_installation(state_, state_.navigation.selected_game_id)) {
                const auto duplicate = std::find_if(state_.jobs.begin(), state_.jobs.end(), [this](const Job& job) {
                    return job.game_id == state_.navigation.selected_game_id && job.kind == "uninstall"
                        && job.state != JobState::Downloaded && job.state != JobState::Cancelled;
                });
                const bool has_duplicate = duplicate != state_.jobs.end();
                const auto existing_id = has_duplicate ? duplicate->id : std::string{};
                if (!has_duplicate)
                    state_.jobs.push_back({next_job_id(), state_.navigation.selected_game_id, "uninstall", next_queue_order(state_), JobStage::Uninstall, JobState::Queued, 0, installation->installed_bytes, std::nullopt, std::nullopt, {}, JobStage::Uninstall, installation->volume_id, 0, 0, false, false});
                state_.navigation.route = Route::Downloads;
                state_.navigation.focus_key = "job-" + (has_duplicate ? existing_id : state_.jobs.back().id);
            }
        }
        else if (pending_destructive_action_ == "signout-confirm") {
            if (live()) { queue_service_action(ServiceAction::SignOut); state_.account().authentication = AuthState::SigningOut; }
            else { state_.account().authentication = AuthState::SignedOut; state_.account().id.clear(); state_.account().display_name.clear(); state_.account().cache_available = false; state_.library = {}; clear_profile_customizations(state_); state_.navigation.route = Route::Startup; }
        }
        else if (pending_destructive_action_ == "account-switch") {
            if (live()) { queue_service_action(ServiceAction::SwitchAccount); state_.account().authentication = AuthState::SigningOut; }
            else {
                state_.library = {}; clear_profile_customizations(state_); state_.account() = {};
                state_.account().authentication = AuthState::Waiting; state_.account().challenge_expiry = state_.clock_ticks + 30;
                ++challenge_generation_; state_.navigation.route = Route::SignIn; state_.navigation.focus_key = "qr-cancel";
            }
        }
        else if (pending_destructive_action_ == "reset-confirm") { const auto installs = state_.installations; state_.library = {}; state_.installations = installs; clear_profile_customizations(state_); state_.reduced_motion = false; if (live()) { force_organization_reset_ = true; force_preferences_reset_ = true; } }
        else if (pending_destructive_action_ == "quit-game") { state_.session = SessionState::Stopping; state_.navigation.route = Route::Session; }
        else if (pending_destructive_action_ == "force-stop") { state_.session = SessionState::Idle; state_.navigation.route = Route::Details; state_.navigation.detail_id = state_.session_game_id; }
        else if (pending_destructive_action_ == "quit-confirm") state_.quit_requested = true;
        else if (pending_destructive_action_.rfind("collection-delete-", 0) == 0) {
            const auto id = pending_destructive_action_.substr(18);
            state_.library.collections.erase(std::remove_if(state_.library.collections.begin(), state_.library.collections.end(), [&id](const Collection& value) { return value.id == id; }), state_.library.collections.end());
            if (state_.library.scope == id) {
                state_.library.scope = "all";
                state_.navigation.library_rail_focused = false;
                reconcile_library_selection(state_);
            }
            // Back to the collection sheets the delete came from, which list what is left.
            close_modal();
            pending_destructive_action_.clear();
            refresh_collection_sheets();
            save_profile();
            rebuild_view();
            return;
        }
        state_.navigation.modals.clear();
        pending_destructive_action_.clear();
    } else if (action_name.rfind("sort-", 0) == 0) {
        state_.library.sort = action_name.substr(5); state_.library.view_sort.clear(); close_modal(); reconcile_library_selection(state_); save_profile();
    } else if (action_name.rfind("filter-", 0) == 0) {
        const auto filter = action_name.substr(7);
        if (state_.library.filters.contains(filter)) state_.library.filters.erase(filter); else state_.library.filters.insert(filter);
        close_modal(); reconcile_library_selection(state_); save_profile();
    } else if (action_name == "filters-reset") {
        state_.library.filters.clear(); state_.library.sort = "name"; state_.library.view_sort.clear(); close_modal(); reconcile_library_selection(state_); save_profile();
    } else if (action_name == "game-settings") {
        const auto id = state_.navigation.selected_game_id;
        const auto setup = game_setups_.find(id);
        // Only a game with no PS5 title yet is checked first; one being set up or waiting for the PS5 has its recipe.
        const auto runtime = state_.runtime_games.find(id);
        const bool no_title = runtime == state_.runtime_games.end() || runtime->second.status == "Not prepared";
        if (live() && !game_prepared(id) && no_title && (setup == game_setups_.end() || !setup->second.draft)) {
            if (!state_.navigation.modals.empty() && state_.navigation.modals.back().id == "context") close_modal();
            pending_runtime_prepare_requests_.push_back(id);
            settings_after_prepare_.insert(id);
            const auto* game = find_game(state_, id);
            push_notice({"settings-" + ToKey(id), "Checking the game", "", false, kLiveNoticeSeconds, false,
                         (game ? game->title : std::string("The game")) + " · Game settings opens when Playden has looked at its files."});
        } else {
            open_game_settings();
        }
    } else if (action_name == "game-maintenance") {
        if (live()) {
            pending_activity_requests_.push_back(state_.navigation.selected_game_id);
            if (const auto found = state_.runtime_games.find(state_.navigation.selected_game_id); found != state_.runtime_games.end())
                found->second.activity = TitleActivity::Unknown;
        }
        // From Game actions it opens on top of that sheet, so Circle returns there.
        open_modal({"game-maintenance", "Maintenance", state_.navigation.focus_key, maintenance_actions(), 0, false});
    } else if (action_name.rfind("maintenance-", 0) == 0) {
        const auto game = state_.navigation.selected_game_id;
        std::string refusal;
        if (action_name != "maintenance-reset" && !game_closed(game, &refusal)) return;
        const auto* known = find_game(state_, game);
        const std::string title = known ? known->title : "the game";
        std::string heading, body;
        if (action_name == "maintenance-reset") {
            heading = "Reset settings?";
            body = "Your changes to " + title + "'s settings are removed. The chosen preset and program stay; saves are not touched.";
        } else if (action_name == "maintenance-update") {
            heading = "Update the runtime?";
            body = title + " moves to the installed runtime kit, and settings that need it apply. "
                   "The game doesn't start; saves are not touched.";
        } else if (action_name == "maintenance-repair") {
            heading = "Repair the game?";
            body = "Playden checks the files " + title + " needs to start, fixes their permissions and writes its settings again. Saves are not touched.";
        } else if (action_name == "maintenance-shader") {
            heading = "Clear the shader cache?";
            body = "DXVK's shader cache for " + title + " is deleted. Shaders compile again on the next start, which may stutter at first.";
        } else if (action_name == "maintenance-backup") {
            heading = "Back up saves?";
            body = "The Windows user folders of " + title + " are copied aside on this console. Playden keeps the newest 3 backups. Saves the game keeps in its own folder are not included.";
        } else if (action_name == "maintenance-recreate") {
            pending_destructive_action_ = action_name;
            open_modal(recreate_confirmation(title, action_name));
            return;
        } else {
            return;
        }
        pending_destructive_action_ = action_name;
        ModalEntry confirmation{"confirmation", std::move(heading), action_name, {"cancel", "confirm-destructive"}, 1, false};
        confirmation.body = std::move(body);
        open_modal(std::move(confirmation));
    } else if (action_name == "effective-command") {
        rt::EffectiveSettings values;
        if (EffectiveSettingsFor(state_.navigation.selected_game_id, &values) && values.launch) {
            ModalEntry command{"confirmation", "Effective command", action_name, {"cancel"}, 0, false};
            command.body = values.launch->executable + (values.arguments.empty() ? "" : " " + values.arguments);
            if (!values.dll_overrides.empty()) command.body += "\nLibrary overrides: " + values.dll_overrides;
            open_modal(std::move(command));
        }
    }
    else if (action_name == "preset-picker") {
        std::vector<std::string> actions;
        int focused = 0;
        const auto* current = preset_for(state_.navigation.selected_game_id);
        for (const auto& preset : settings_presets_) {
            if (current && preset.id == current->id) focused = static_cast<int>(actions.size());
            actions.push_back("preset-" + preset.id);
        }
        actions.push_back("preset-community");
        actions.push_back("cancel");
        open_modal({"preset-picker", "Choose a preset", "preset-picker", std::move(actions), focused, false});
    } else if (action_name.rfind("preset-", 0) == 0 && !state_.navigation.modals.empty() && state_.navigation.modals.back().id == "preset-picker") {
        const auto id = action_name.substr(7);
        if (std::none_of(settings_presets_.begin(), settings_presets_.end(), [&id](const rt::SettingsPreset& preset) { return preset.id == id; }) ||
            !can_change_game_settings())
            return;  // the Community entry waits for the shared recipe index
        auto& record = state_.game_settings[state_.navigation.selected_game_id];
        record.preset_id = id == "playden-default" ? std::string{} : id;
        clear_keeping_start_choices(&record.overrides);
        ++record.revision;
        close_modal(); refresh_game_settings(); save_profile();
    } else if (action_name.rfind("setting-", 0) == 0) {
        open_setting(action_name.substr(8));
    } else if (action_name == "steam-app-id") {
        const auto record = state_.game_settings.find(state_.navigation.selected_game_id);
        const std::uint32_t saved = record == state_.game_settings.end() ? 0 : record->second.steam_app_id;
        start_editor("Steam app ID", saved ? std::to_string(saved) : std::string());
        state_.editor.symbols = true;  // digits first
    } else if (action_name == "unlock-setting" && !state_.navigation.modals.empty()) {
        // "Change anyway" on a row the recipe locks: the player's value may now win.
        const auto id = state_.navigation.modals.back().setting_id;
        auto& record = state_.game_settings[state_.navigation.selected_game_id];
        record.overrides[rt::kUnlockPrefix + id] = "on";
        ++record.revision;
        save_profile();
        close_modal();
        refresh_game_settings();
        open_setting(id);
    } else if (action_name == "value-edit" && !state_.navigation.modals.empty()) {
        const auto id = state_.navigation.modals.back().setting_id;
        rt::EffectiveSettings values;
        const bool known = EffectiveSettingsFor(state_.navigation.selected_game_id, &values);
        close_modal();
        start_editor(rt::FindSetting(id)->label, known ? values.values.at(id).value : std::string{});
        editing_setting_id_ = id;
    } else if (action_name == "override-add" && !state_.navigation.modals.empty()) {
        close_modal();
        start_editor("Add a library override", "");
        editing_setting_id_ = "dll_overrides";
        editing_append_ = true;
    } else if (action_name.rfind("override-remove-", 0) == 0 && !state_.navigation.modals.empty()) {
        rt::EffectiveSettings values;
        if (EffectiveSettingsFor(state_.navigation.selected_game_id, &values)) {
            auto entries = split_overrides(values.dll_overrides);
            const auto index = static_cast<std::size_t>(std::stoul(action_name.substr(16)));
            if (index < entries.size()) {
                entries.erase(entries.begin() + static_cast<std::ptrdiff_t>(index));
                std::string list;
                for (const auto& entry : entries) list += (list.empty() ? "" : ";") + entry;
                set_game_setting("dll_overrides", list);
                save_profile();
                auto& modal = state_.navigation.modals.back();
                modal.action_keys = setting_value_actions("dll_overrides");
                modal.focused_action = std::min(modal.focused_action, static_cast<int>(modal.action_keys.size()) - 1);
                state_.navigation.focus_key = modal.action_keys[static_cast<std::size_t>(modal.focused_action)];
            }
        }
    } else if (action_name == "value-reset" && !state_.navigation.modals.empty()) {
        // Back to the recipe: the player's value goes, and with it a "Change anyway".
        const auto id = state_.navigation.modals.back().setting_id;
        auto& record = state_.game_settings[state_.navigation.selected_game_id];
        record.overrides.erase(id);
        record.overrides.erase(rt::kUnlockPrefix + id);
        if (id == "launch_option") forget_program_choices(&record.overrides);
        if (id == "dll_overrides") record.overrides.erase("library_overrides");
        ++record.revision;
        if (id == "refresh") follow_refresh_rate(state_.navigation.selected_game_id);
        close_modal(); refresh_game_settings(); save_profile();
    } else if (action_name.rfind("program-", 0) == 0 && action_name != "program-scan" && !state_.navigation.modals.empty()) {
        // A program from the game folder; its architecture comes with it.
        const auto setup = game_setups_.find(state_.navigation.selected_game_id);
        const auto index = static_cast<std::size_t>(std::stoul(action_name.substr(8)));
        if (setup != game_setups_.end() && index < setup->second.programs.size()) {
            const auto program = setup->second.programs[index];
            forget_program_choices(&state_.game_settings[state_.navigation.selected_game_id].overrides);
            set_game_setting("program", program.path);
            set_game_setting("architecture", program.architecture);
            if (program.refusal_soft) set_game_setting("start_check", "try");
        }
        close_modal(); refresh_game_settings(); save_profile();
    } else if (action_name == "export-recipe") {
        export_recipe(state_.navigation.selected_game_id);
    } else if (action_name == "choice-custom" && !state_.navigation.modals.empty() && !state_.navigation.modals.back().setting_id.empty()) {
        const auto id = state_.navigation.modals.back().setting_id;
        rt::EffectiveSettings values;
        const bool known = EffectiveSettingsFor(state_.navigation.selected_game_id, &values);
        close_modal();
        start_editor(rt::FindSetting(id)->label, known ? values.values.at(id).value : std::string{});
        editing_setting_id_ = id;
    } else if (action_name == "button-mapping") {
        std::vector<std::string> actions;
        for (const auto& button : rt::ProsperoButtons()) actions.push_back("setting-button." + button);
        actions.push_back("cancel");
        open_modal({"button-mapping", "Button mapping", "button-mapping", std::move(actions), 0, false});
    } else if (action_name == "controller-tuning") {
        std::vector<std::string> actions;
        for (const auto& definition : rt::SettingsCatalogue())
            if (definition.group == rt::SettingGroup::kControllerTuning) actions.push_back("setting-" + definition.id);
        actions.push_back("cancel");
        open_modal({"controller-tuning", "Controller tuning", "controller-tuning", std::move(actions), 0, false});
    } else if (action_name.rfind("choice-", 0) == 0 && !state_.navigation.modals.empty() && state_.navigation.modals.back().setting_id.rfind("button.", 0) == 0) {
        const auto id = state_.navigation.modals.back().setting_id;
        const auto index = static_cast<std::size_t>(std::stoul(action_name.substr(7)));
        auto& record = state_.game_settings[state_.navigation.selected_game_id];
        if (index == 0) record.overrides.erase(id);           // inherit the preset or recipe binding
        else if (index < common_bindings().size()) record.overrides[id] = common_bindings()[index].first;
        ++record.revision;
        close_modal(); refresh_game_settings(); save_profile();
    } else if (action_name.rfind("choice-", 0) == 0 && !state_.navigation.modals.empty() && !state_.navigation.modals.back().setting_id.empty()) {
        const auto id = state_.navigation.modals.back().setting_id;
        const auto index = static_cast<std::size_t>(std::stoul(action_name.substr(7)));
        const auto& recipe = RecipeFor(state_.navigation.selected_game_id);
        const auto* definition = rt::FindSetting(id);
        const bool launch = id == "launch_option";
        if (definition && index < (launch ? recipe.launch_entries.size() : definition->choices.size())) {
            // A launch option replaces a program the player picked from the game folder.
            if (launch) {
                forget_program_choices(&state_.game_settings[state_.navigation.selected_game_id].overrides);
            }
            set_game_setting(id, launch ? recipe.launch_entries[index].id : definition->choices[index].value);
        }
        close_modal(); refresh_game_settings(); save_profile();
    } else if (action_name == "section-compatibility" || action_name == "section-advanced") {
        if (!expanded_settings_sections_.erase(action_name)) expanded_settings_sections_.insert(action_name);
        refresh_game_settings();
    } else if (action_name == "reset-settings") {
        auto& record = state_.game_settings[state_.navigation.selected_game_id];
        if (has_resettable(record.overrides)) { clear_keeping_start_choices(&record.overrides); ++record.revision; }
        refresh_game_settings(); save_profile();
    }
    else if (action_name == "note") open_modal({"compatibility", "Compatibility and note", "note", {"rating-untested", "rating-works", "rating-playable", "rating-broken", "edit-note", "cancel"}, 0, false});
    else if (action_name.rfind("rating-", 0) == 0) {
        const auto value = action_name.substr(7);
        state_.library.ratings[state_.navigation.selected_game_id] = value == "works" ? Compatibility::Works : value == "playable" ? Compatibility::Playable : value == "broken" ? Compatibility::Broken : Compatibility::Untested;
        save_profile(); close_modal();
    }
    else if (action_name == "edit-note") start_editor("Note", state_.library.notes[state_.navigation.selected_game_id]);
    else if (action_name == "collection") open_modal({"collections", "Collections", "collection", collection_sheet_actions("collections"), 0, false});
    else if (action_name == "collection-manage")
        open_modal({"collections-manage", "Manage collections", "collection-manage", collection_sheet_actions("collections-manage"), 0, false});
    else if (action_name == "collection-new") { editing_target_id_.clear(); start_editor("Collection name", ""); }
    else if (action_name.rfind("collection-toggle-", 0) == 0) {
        const auto id = action_name.substr(18);
        auto found = std::find_if(state_.library.collections.begin(), state_.library.collections.end(), [&id](const Collection& value) { return value.id == id; });
        if (found != state_.library.collections.end()) {
            if (found->members.contains(state_.navigation.selected_game_id)) found->members.erase(state_.navigation.selected_game_id); else found->members.insert(state_.navigation.selected_game_id);
            save_profile();
        }
    }
    else if (action_name.rfind("collection-rename-", 0) == 0) {
        editing_target_id_ = action_name.substr(18);
        const auto found = std::find_if(state_.library.collections.begin(), state_.library.collections.end(), [this](const Collection& value) { return value.id == editing_target_id_; });
        if (found != state_.library.collections.end()) start_editor("Rename collection", found->name);
    }
    else if (action_name.rfind("collection-pin-", 0) == 0) {
        const auto id = action_name.substr(15);
        const auto found = std::find_if(state_.library.collections.begin(), state_.library.collections.end(), [&id](const Collection& value) { return value.id == id; });
        if (found != state_.library.collections.end()) { found->pinned = !found->pinned; save_profile(); }
        refresh_collection_sheets();
    }
    else if (action_name.rfind("collection-delete-", 0) == 0) {
        pending_destructive_action_ = action_name;
        open_modal({"confirmation", "Delete collection? Games stay in your library.", action_name, {"cancel", "confirm-destructive"}, 0, true});
    }
    else if (action_name == "toggle-motion") { state_.reduced_motion = !state_.reduced_motion; save_profile(); }
    else if (action_name == "toggle-title-store") { state_.title_store_suffix = !state_.title_store_suffix; save_profile(); }
    else if (action_name == "toggle-cloud-saves") { state_.cloud_saves_auto = !state_.cloud_saves_auto; save_profile(); }
    else if (action_name == "toggle-downloads-slow") {
        state_.downloads_slow_network = !state_.downloads_slow_network;
        save_profile();
    }
    else if (action_name == "launch") { state_.session_game_id = state_.navigation.selected_game_id; state_.session = SessionState::Preparing; state_.navigation.route = Route::Session; state_.navigation.focus_key = "session-return"; }
    else if (action_name == "runtime-status") open_modal({"runtime", "Runtime unavailable", "details-play", {"runtime-retry", "cancel"}, 0, false});
    else if (action_name == "return-game") state_.session = SessionState::Running;
    else if (action_name == "force-stop") { pending_destructive_action_ = "force-stop"; open_modal({"confirmation", "Force stop simulated session?", state_.navigation.focus_key, {"cancel", "confirm-destructive"}, 0, true}); }
    else if (action_name == "details-back") { state_.navigation.route = Route::Details; state_.navigation.detail_id = state_.session_game_id; }
    else if (live() && action_name == "logs") {
        // From a download's actions: that game's install log, with Retry when the download failed.
        GameId game_id;
        if (!state_.navigation.modals.empty() && state_.navigation.modals.back().id == "job-actions") {
            const auto& opener = state_.navigation.modals.back().opener_focus_key;
            if (opener.rfind("job-", 0) == 0) game_id = key_id(std::string_view(opener).substr(4));
        }
        open_live_log("install", game_id);
    }
    else if (live() && action_name == "logs-pick")
        open_modal({"diagnostics", "Logs", state_.navigation.focus_key, {"log-install", "log-cloud", "log-auth", "log-launch", "cancel"}, 0, false});
    else if (live() && (action_name == "log-install" || action_name == "log-cloud" || action_name == "log-auth" ||
                        action_name == "log-launch"))
        open_live_log(action_name.substr(4), {});
    else if (live() && action_name == "logs-refresh") load_live_log();
    else if (action_name == "logs") { start_logs(); open_modal({"logs", "Operation logs", state_.navigation.focus_key, {"logs-fixture-controls", "logs-back"}, 0, false}); }
    else if (action_name == "logs-retry") { start_logs(); state_.navigation.modals.back().action_keys = {"logs-fixture-controls", "logs-back"}; state_.navigation.modals.back().focused_action = 0; state_.navigation.focus_key = "logs-fixture-controls"; }
    else if (action_name == "logs-back") close_modal();
    else if (action_name == "logs-fixture-controls") open_modal({"logs-controls", "Log fixture controls", state_.navigation.focus_key,
        {"inject-logs-loading", "inject-logs-empty", "inject-logs-failure", "inject-logs-ready", "cancel"}, 0, false});
    else if (action_name == "controller-test") open_modal({"controller", "Controller diagnostics", state_.navigation.focus_key, {"cancel"}, 0, false});
    else if (action_name == "settings-runtime") open_modal({"runtime", "Runtime status — simulated", state_.navigation.focus_key, {"runtime-retry", "cancel"}, 0, false});
    else if (action_name == "runtime-retry") { state_.runtime = RuntimeState::Checking; state_.session_error.clear(); }
    else if (action_name == "qr-fixture-controls") open_modal({"qr-controls", "QR fixture controls", state_.navigation.focus_key,
        {"inject-qr-approved", "inject-qr-expired", "inject-qr-failure", "inject-qr-denied", "advance-clock", "cancel"}, 0, false});
    else if (action_name == "simulation-controls") open_modal({"simulation-controls", "Simulation controls", state_.navigation.focus_key,
        {"advance-clock", "inject-download-failed", "inject-verification-failed", "inject-uninstall-failed", "inject-storage-disconnect", "inject-storage-reconnect", "inject-controller-disconnect", "inject-controller-reconnect", "inject-launch-failure", "inject-stop-failure", "inject-runtime-failed", "inject-save-failure", "inject-logs-loading", "inject-logs-empty", "inject-logs-failure", "inject-logs-ready", "inject-delayed-art", "inject-failed-art", "inject-library-refresh", "inject-startup-loading", "cancel"}, 0, false});
    else if (action_name == "details-kit-settings") {
        state_.navigation.route = Route::Settings;
        state_.navigation.settings_rail_focused = false;
        state_.navigation.focus_key = "settings-about";
        settings_focus_section_ = "About";
        open_modal({"about", live() ? "About Playden" : "About Playden UI simulation", "settings-about",
            {"about-version", "about-build", "about-framework", "about-platform", "about-kit", "about-kit-source", "about-back"}, 4, false});
    }
    else if (action_name == "about") open_modal({"about", live() ? "About Playden" : "About Playden UI simulation", state_.navigation.focus_key,
        {"about-version", "about-build", "about-framework", "about-platform", "about-kit", "about-kit-source", "about-back"}, 0, false});
    else if (action_name == "about-back") close_modal();
    else if (live() && action_name == "toggle-beta-updates") {
        state_.beta_updates = !state_.beta_updates;
        self_update_requests_.push_back(state_.beta_updates ? "betas-on" : "betas-off");
    }
    else if (live() && action_name == "self-update") {
        const auto& phase = state_.self_update.phase;
        if (phase == "available") { pending_update_prompt_ = "available"; show_update_prompt(); }
        else if (phase == "staged") { pending_update_prompt_ = "staged"; show_update_prompt(); }
        else if (phase == "downloading" || phase == "unpacking") {
            state_.navigation.route = Route::Downloads;
            state_.navigation.focus_key = "job-playden-update";
        } else if (phase != "checking") self_update_requests_.push_back("check");
    }
    else if (live() && action_name == "self-update-install") { close_modal(); self_update_requests_.push_back("install"); }
    else if (live() && action_name == "kit") {
        const auto& phase = state_.self_update.kit_phase;
        if (phase == "available") { pending_update_prompt_ = "kit"; show_update_prompt(); }
        else if (phase == "downloading" || phase == "unpacking") {
            state_.navigation.route = Route::Downloads;
            state_.navigation.focus_key = "job-kit-install";
        } else if (phase == "failed" || phase == "idle") self_update_requests_.push_back("kit-check");
    }
    else if (live() && action_name == "kit-install") { close_modal(); self_update_requests_.push_back("kit-install"); }
    else if (live() && action_name == "kit-cancel") { close_modal(); self_update_requests_.push_back("kit-cancel"); }
    else if (live() && action_name == "self-update-cancel") { close_modal(); self_update_requests_.push_back("cancel"); }
    else if (live() && action_name == "self-update-restart") { close_modal(); request_self_update_restart(); }
    else if (live() && action_name == "successor-open") { close_modal(); request_self_update_restart("open-successor"); }
    else if (live() && action_name == "self-update-notes") open_release_notes(false);
    else if (action_name == "release-notes-ok") close_modal();
    else if (live() && action_name == "self-update-back") {
        ModalEntry confirm{"confirmation", "Go back to Playden " + state_.self_update.previous + "?",
                           state_.navigation.focus_key, {"self-update-back-confirm", "cancel"}, 1, false};
        confirm.body = "Playden closes, puts version " + state_.self_update.previous +
                       " and its runtime kit back, and opens again. If it doesn't open by itself, open it from the "
                       "Home screen.";
        open_modal(std::move(confirm));
    }
    else if (kOfferGoBack && live() && action_name == "self-update-back-confirm") { close_modal(); request_self_update_restart("rollback"); }
    else if (action_name == "scenario-picker") open_modal({"scenarios", "Fixture scenarios", state_.navigation.focus_key,
        {"scenario-default", "scenario-first-run", "scenario-signed-out", "scenario-qr-waiting", "scenario-offline", "scenario-empty", "scenario-no-results", "scenario-delayed-art", "scenario-failed-art", "scenario-install-ready", "scenario-unknown-size", "scenario-insufficient-space", "scenario-storage-disconnect", "scenario-paused", "scenario-download-failed", "scenario-verification-failed", "scenario-restart-recovery", "scenario-save-failure", "scenario-runtime-unavailable", "scenario-launch-failure", "scenario-crash", "scenario-stop-failure", "scenario-controller-disconnected", "scenario-reduced-motion", "scenario-stress", "scenario-import-empty", "scenario-import-candidates", "scenario-import-programs", "scenario-import-choice", "scenario-import-storage", "scenario-import-drop-choice", "scenario-import-drop-storage", "scenario-import-details", "scenario-import-copying", "scenario-import-library", "cancel"}, 0, false});
    else if (action_name.rfind("scenario-", 0) == 0) { state_.navigation.modals.clear(); scenario(action_name.substr(9)); }
    else if (action_name == "session-fixture-controls") open_modal({"session-controls", "Session fixture controls", state_.navigation.focus_key,
        {"advance-clock", "inject-stop-failure", "inject-crash", "cancel"}, 0, false});
    else if (action_name == "advance-clock") tick(1);
    else if (action_name.rfind("inject-", 0) == 0) {
        const auto injected = action_name.substr(7);
        if (!state_.navigation.modals.empty() && (state_.navigation.modals.back().id == "qr-controls" || state_.navigation.modals.back().id == "logs-controls" || state_.navigation.modals.back().id == "session-controls")) close_modal();
        action("inject:" + injected);
    }
    else if (action_name == "notices") {
        std::vector<std::string> actions;
        for (const auto& notice : state_.notices) if (!notice.action.empty()) actions.push_back(notice.action);
        actions.push_back("cancel"); open_modal({"notices", "Warnings", state_.navigation.focus_key, std::move(actions), 0, false});
    }
    else if (action_name.rfind("open-game:", 0) == 0) {
        const auto id = key_id(std::string_view(action_name).substr(10));
        if (id.valid() && find_game(state_, id)) {
            state_.navigation.modals.clear(); state_.navigation.selected_game_id = id; state_.navigation.detail_id = id;
            state_.navigation.detail_origin = Route::Downloads; state_.navigation.route = Route::Details;
            details_primary_pending_ = live(); state_.navigation.focus_key = live() ? "details-favorite" : (is_installed(state_, id) ? "details-play" : "details-install");
        }
    }
    else if (action_name == "storage-retry") action("inject:storage-reconnect");
    else if (action_name == "retry-save") {
        if (live()) persistence_retry_requested_ = true;
        else { state_.persistence_available = true; save_profile(); }
    }
    else if (action_name == "account") {
        std::vector<std::string> actions = {"account-auth", "account-last-sync"};
        if (state_.account().authentication == AuthState::SignedIn) actions.push_back("account-sync");
        else actions.push_back("account-recover");
        // A new QR sign-in for the same account: a fresh token, keeping the library and organization.
        if (live() && (state_.account().authentication == AuthState::SignedIn || state_.account().authentication == AuthState::Offline))
            actions.push_back("account-reauth");
        actions.push_back("account-switch");
        if (state_.account().authentication != AuthState::SignedOut) actions.push_back("signout-confirm");
        actions.push_back("cancel");
        open_modal({"account", state_.account().display_name.empty() ? "Account" : state_.account().display_name, state_.navigation.focus_key, std::move(actions), 0, false});
    }
    else if (action_name == "account-sync") {
        close_modal();
        if (!live()) state_.account().authentication = AuthState::Syncing;
        state_.library_syncing = true;
        state_.library_loading = true;
        state_.navigation.route = Route::Home;
        if (live()) queue_service_action(ServiceAction::RefreshLibrary);
    }
    else if (action_name == "refresh-library") {
        // As Sync now, but the Library stays on screen. Offline or with an expired sign-in, the
        // controller restores the saved sign-in first.
        close_modal();
        if (live()) {
            state_.library_syncing = true;
            state_.library_loading = true;
            queue_service_action(ServiceAction::RefreshLibrary);
        } else {
            state_.account().last_sync = state_.clock_ticks;
        }
    }
    else if (action_name == "account-recover") {
        state_.navigation.modals.clear();
        open_sign_in();
    }
    else if (action_name == "account-reauth") {
        state_.navigation.modals.clear();
        if (live()) open_sign_in(true);
    }
    else if (action_name == "account-switch") { pending_destructive_action_ = "account-switch"; open_modal({"confirmation", "Switch account? Current account cache will be cleared.", state_.navigation.focus_key, {"cancel", "confirm-destructive"}, 0, true}); }
}

bool Simulation::action(const std::string& logical_action) {
    // Any input settles Details focus where the user put it.
    details_primary_pending_ = false;
    state_.controller = {};
    if (logical_action == "up") state_.controller.left_y = -1.0F;
    else if (logical_action == "down") state_.controller.left_y = 1.0F;
    else if (logical_action == "left") state_.controller.left_x = -1.0F;
    else if (logical_action == "right") state_.controller.left_x = 1.0F;
    else if (logical_action == "cross") state_.controller.cross = true;
    else if (logical_action == "circle") state_.controller.circle = true;
    else if (logical_action == "square") state_.controller.square = true;
    else if (logical_action == "triangle") state_.controller.triangle = true;
    else if (logical_action == "l1") state_.controller.l1 = true;
    else if (logical_action == "r1") state_.controller.r1 = true;
    else if (logical_action == "l2") state_.controller.l2 = 1.0F;
    else if (logical_action == "r2") state_.controller.r2 = 1.0F;
    else if (logical_action == "options") state_.controller.options = true;
    // Without the root grant nothing else runs: Cross (or the action itself) quits to Home.
    if (state_.root_access_missing) {
        if (logical_action != "cross" && logical_action != "root-quit") return false;
        state_.quit_requested = true;
        return true;
    }
    if (live()) {
        if (logical_action == "connect") { open_sign_in(); rebuild_view(); return true; }
        if (logical_action == "qr-retry" || logical_action == "qr-refresh") {
            state_.sign_in.step = SignInStep::Qr;
            queue_service_action(ServiceAction::BeginQr);
            state_.account().authentication = AuthState::Waiting;
            state_.account().challenge_url.clear(); state_.account().error.clear();
            state_.navigation.route = Route::SignIn; state_.navigation.focus_key = "qr-cancel";
            rebuild_view(); return true;
        }
        if (logical_action == "qr-cancel") {
            queue_service_action(ServiceAction::CancelQr);
            state_.account().authentication = AuthState::Cancelled;
            leave_steam_sign_in();
            rebuild_view(); return true;
        }
        if (logical_action == "account-sync" || logical_action == "refresh-library") {
            queue_service_action(ServiceAction::RefreshLibrary);
            state_.library_syncing = true;
            state_.library_loading = true;
            rebuild_view(); return true;
        }
        if (logical_action == "signout" || logical_action == "account-switch") {
            queue_service_action(logical_action == "signout" ? ServiceAction::SignOut : ServiceAction::SwitchAccount);
            state_.account().authentication = AuthState::SigningOut;
            rebuild_view(); return true;
        }
        const bool synthetic = logical_action.rfind("scenario:", 0) == 0 || logical_action.rfind("inject:", 0) == 0
            || logical_action.rfind("route:", 0) == 0 || logical_action.rfind("install:", 0) == 0
            || logical_action.rfind("job:", 0) == 0 || logical_action.rfind("session:", 0) == 0
            || logical_action == "advance-clock" || logical_action == "simulation-controls"
            || logical_action == "scenario-picker" || logical_action == "session-fixture-controls"
            || logical_action == "qr-fixture-controls" || logical_action == "logs-fixture-controls"
            || logical_action == "launch" || logical_action == "runtime-retry";
        if (synthetic) return false;
    }
    if (logical_action.rfind("collection:create:", 0) == 0) {
        const auto name = trimmed(logical_action.substr(18));
        if (name.empty() || std::any_of(state_.library.collections.begin(), state_.library.collections.end(), [&name](const Collection& value) { return value.name == name; })) return false;
        std::string id;
        do { id = "collection-" + std::to_string(next_collection_serial_++); }
        while (std::any_of(state_.library.collections.begin(), state_.library.collections.end(), [&id](const Collection& value) { return value.id == id; }));
        state_.library.collections.push_back({std::move(id), name, false, {}});
        save_profile(); rebuild_view(); return true;
    }
    if (logical_action.rfind("collection:add:", 0) == 0) {
        const auto remainder = logical_action.substr(15); const auto separator = remainder.find(':');
        if (separator == std::string::npos) return false;
        const auto id = remainder.substr(0, separator);
        const auto game_id = key_id(std::string_view(remainder).substr(separator + 1));
        auto found = std::find_if(state_.library.collections.begin(), state_.library.collections.end(), [&id](const Collection& value) { return value.id == id; });
        if (!game_id.valid() || found == state_.library.collections.end()) return false;
        found->members.insert(game_id); save_profile(); rebuild_view(); return true;
    }
    if (logical_action.rfind("collection:rename:", 0) == 0) {
        const auto remainder = logical_action.substr(18); const auto separator = remainder.find(':');
        if (separator == std::string::npos) return false;
        const auto id = remainder.substr(0, separator); const auto name = remainder.substr(separator + 1);
        auto found = std::find_if(state_.library.collections.begin(), state_.library.collections.end(), [&id](const Collection& value) { return value.id == id; });
        if (name.empty() || found == state_.library.collections.end()) return false;
        found->name = name; save_profile(); rebuild_view(); return true;
    }
    if (logical_action.rfind("collection:pin:", 0) == 0 || logical_action.rfind("collection:delete:", 0) == 0) {
        const bool remove = logical_action.rfind("collection:delete:", 0) == 0;
        const auto id = logical_action.substr(remove ? 18 : 15);
        auto found = std::find_if(state_.library.collections.begin(), state_.library.collections.end(), [&id](const Collection& value) { return value.id == id; });
        if (found == state_.library.collections.end()) return false;
        if (remove) {
            state_.library.collections.erase(found);
            if (state_.library.scope == id) {
                state_.library.scope = "all";
                state_.navigation.library_rail_focused = false;
                reconcile_library_selection(state_);
            }
        } else found->pinned = !found->pinned;
        save_profile(); rebuild_view(); return true;
    }
    if (logical_action.rfind("rate:", 0) == 0) {
        const auto remainder = logical_action.substr(5); const auto separator = remainder.find(':'); if (separator == std::string::npos) return false;
        const auto id = key_id(std::string_view(remainder).substr(0, separator)); if (!id.valid()) return false;
        const auto rating = remainder.substr(separator + 1);
        state_.library.ratings[id] = rating == "works" ? Compatibility::Works : rating == "playable" ? Compatibility::Playable : rating == "broken" ? Compatibility::Broken : Compatibility::Untested;
        save_profile(); rebuild_view(); return true;
    }
    if (logical_action.rfind("note:", 0) == 0) {
        const auto remainder = logical_action.substr(5); const auto separator = remainder.find(':'); if (separator == std::string::npos) return false;
        const auto id = key_id(std::string_view(remainder).substr(0, separator)); if (!id.valid()) return false;
        state_.library.notes[id] = remainder.substr(separator + 1); save_profile(); rebuild_view(); return true;
    }
    if (logical_action.rfind("install:", 0) == 0) {
        const auto remainder = logical_action.substr(8); const auto separator = remainder.find(':'); if (separator == std::string::npos) return false;
        const auto id = key_id(std::string_view(remainder).substr(0, separator)); if (!id.valid()) return false;
        const auto volume_id = remainder.substr(separator + 1);
        const auto assessment = assess_install(state_, id, volume_id);
        if (assessment.readiness != InstallReadiness::Ready || !assessment.download_bytes) return false;
        const auto existing = std::find_if(state_.jobs.begin(), state_.jobs.end(), [id](const Job& value) {
            return value.game_id == id && value.kind == "install" && value.state != JobState::Cancelled
                && value.state != JobState::Downloaded && value.state != JobState::Failed;
        });
        if (existing == state_.jobs.end()) {
            state_.jobs.push_back({next_job_id(), id, "install", next_queue_order(state_), JobStage::Download, JobState::Queued, 0, *assessment.download_bytes, std::nullopt, std::nullopt, {}, JobStage::Download, volume_id, 0, assessment.required_bytes.value_or(0), true, false});
            reconcile_storage(state_);
        }
        if (live() || state_.persistence_available) save_profile();
        rebuild_view(); return true;
    }
    if (logical_action.rfind("job:move-up:", 0) == 0) {
        auto* job = find_job(state_, logical_action.substr(12)); if (!job) return false;
        const int wanted = job->queue_order - 1; auto other = std::find_if(state_.jobs.begin(), state_.jobs.end(), [wanted](const Job& value) { return value.queue_order == wanted; });
        if (other != state_.jobs.end()) std::swap(job->queue_order, other->queue_order); save_profile(); rebuild_view(); return true;
    }
    if (logical_action.rfind("job:cancel:", 0) == 0) {
        const auto id = logical_action.substr(11); if (!find_job(state_, id)) return false;
        pending_destructive_action_ = "job-cancel-" + id; open_modal({"confirmation", "Cancel download?", "job-" + id, {"cancel", "confirm-destructive"}, 0, true}); rebuild_view(); return true;
    }
    if (logical_action.rfind("job:fail:", 0) == 0) {
        const auto remainder = logical_action.substr(9); const auto separator = remainder.find(':'); if (separator == std::string::npos) return false;
        auto* job = find_job(state_, remainder.substr(0, separator)); if (!job) return false;
        job->retry_stage = remainder.substr(separator + 1) == "verify" ? JobStage::Verify : JobStage::Download; job->stage = job->retry_stage; job->state = JobState::Failed; job->failure = "Injected failure"; save_profile(); rebuild_view(); return true;
    }
    if (logical_action.rfind("job:retry:", 0) == 0) {
        auto* job = find_job(state_, logical_action.substr(10)); if (!job) return false;
        for (auto& other : state_.jobs) if (&other != job && (other.state == JobState::Downloading || other.state == JobState::Verifying)) other.state = JobState::Queued;
        job->stage = job->retry_stage; job->state = job->stage == JobStage::Download ? JobState::Downloading : JobState::Verifying; job->failure.clear(); save_profile(); rebuild_view(); return true;
    }
    if (logical_action == "reset") {
        const auto installs = state_.installations; state_.library = {}; state_.installations = installs; clear_profile_customizations(state_); state_.reduced_motion = false; if (live()) { force_organization_reset_ = true; force_preferences_reset_ = true; } save_profile(); rebuild_view(); return true;
    }
    if (logical_action == "session:quit") {
        if (state_.session != SessionState::Running) return false; state_.session = SessionState::Stopping; rebuild_view(); return true;
    }
    if (logical_action.rfind("route:", 0) == 0) {
        const auto route = logical_action.substr(6);
        if (route == "home") state_.navigation.route = Route::Home;
        else if (route == "library") state_.navigation.route = Route::Library;
        else if (route == "downloads") state_.navigation.route = Route::Downloads;
        else if (route == "settings") state_.navigation.route = Route::Settings;
        else if (route == "saves") state_.navigation.route = Route::Saves;
        else { last_error_ = "Unknown route: " + route; return false; }
        clear_sign_in_secrets();
        state_.navigation.modals.clear(); state_.editor.active = false; state_.navigation.focus_key.clear(); rebuild_view(); return true;
    }
    if (logical_action.rfind("select:", 0) == 0 || logical_action.rfind("details:", 0) == 0) {
        const bool details = logical_action.rfind("details:", 0) == 0;
        const auto text_id = logical_action.substr(details ? 8 : 7);
        const auto id = key_id(text_id);
        if (!id.valid() || !find_game(state_, id)) { last_error_ = "Unknown game: " + text_id; return false; }
        state_.navigation.selected_game_id = id; state_.navigation.focus_key = game_key(id);
        if (live()) force_organization_selection_ = true;
        const auto games = visible_games();
        const auto found = std::find_if(games.begin(), games.end(), [id](const Game* game) { return game->id == id; });
        if (found != games.end()) state_.navigation.selected_index = static_cast<int>(std::distance(games.begin(), found));
        if (!details && state_.navigation.route == Route::Home) {
            const auto projected = playden::homebrew::shelves(state_);
            int chosen_shelf = -1;
            if (state_.navigation.home_shelf >= 0 && state_.navigation.home_shelf < static_cast<int>(projected.size())) {
                const auto& current = projected[static_cast<std::size_t>(state_.navigation.home_shelf)];
                if (std::find(current.game_ids.begin(), current.game_ids.end(), id) != current.game_ids.end()) chosen_shelf = state_.navigation.home_shelf;
            }
            if (chosen_shelf < 0) for (std::size_t index = 0; index < projected.size(); ++index)
                if (std::find(projected[index].game_ids.begin(), projected[index].game_ids.end(), id) != projected[index].game_ids.end()) { chosen_shelf = static_cast<int>(index); break; }
            if (chosen_shelf >= 0) {
                state_.navigation.home_shelf = chosen_shelf;
                const auto& shelf = projected[static_cast<std::size_t>(chosen_shelf)];
                state_.navigation.home_shelf_focus[shelf.key] = static_cast<int>(std::distance(shelf.game_ids.begin(), std::find(shelf.game_ids.begin(), shelf.game_ids.end(), id)));
                state_.navigation.scroll_offsets["home"] = chosen_shelf;
            }
        }
        if (details) { state_.navigation.detail_origin = state_.navigation.route; state_.navigation.route = Route::Details; state_.navigation.detail_id = id; details_primary_pending_ = live(); state_.navigation.focus_key = live() ? "details-favorite" : (is_installed(state_, id) ? "details-play" : "details-install"); }
        else if (state_.navigation.route == Route::Downloads) {
            const auto job = std::find_if(state_.jobs.begin(), state_.jobs.end(), [id](const Job& value) { return value.game_id == id && value.state != JobState::Cancelled; });
            if (job != state_.jobs.end()) state_.navigation.focus_key = "job-" + job->id;
        }
        show_ready_prompt();
        save_profile(); rebuild_view(); return true;
    }
    if (logical_action.rfind("focus:", 0) == 0) {
        const auto key = logical_action.substr(6);
        const auto found = std::find_if(view_.rows.begin(), view_.rows.end(), [&key](const ViewRow& row) { return row.key == key; });
        if (found == view_.rows.end()) { last_error_ = "Unknown focus key: " + key; return false; }
        state_.navigation.focus_key = key; state_.navigation.selected_index = static_cast<int>(std::distance(view_.rows.begin(), found));
        if (!state_.navigation.modals.empty()) state_.navigation.modals.back().focused_action = state_.navigation.selected_index;
        save_profile(); rebuild_view(); return true;
    }
    if (logical_action.rfind("scenario:", 0) == 0) return scenario(logical_action.substr(9));
    if (logical_action == "advance-clock") { tick(1); return true; }
    if (logical_action == "done") { if (state_.editor.active) commit_editor(); rebuild_view(); return true; }
    if (logical_action == "backspace") {
        if (state_.editor.active && state_.editor.caret > 0) { const auto prior = previous_caret(state_.editor.value, state_.editor.caret); state_.editor.value.erase(prior, state_.editor.caret - prior); state_.editor.caret = prior; }
        rebuild_view(); return true;
    }
    if (logical_action == "clear") { if (state_.editor.active) { steam::ZeroizeSecret(state_.editor.value); state_.editor.caret = 0; } rebuild_view(); return true; }
    if (logical_action == "space") { if (state_.editor.active) { state_.editor.value.insert(state_.editor.caret, " "); ++state_.editor.caret; normalize_editor(); } rebuild_view(); return true; }
    if (logical_action == "reveal") { if (state_.editor.active && state_.editor.secret) state_.editor.revealed = !state_.editor.revealed; rebuild_view(); return true; }
    if (logical_action == "shift") { if (state_.editor.active) state_.editor.shifted = !state_.editor.shifted; rebuild_view(); return true; }
    if (logical_action == "symbols") { if (state_.editor.active) state_.editor.symbols = !state_.editor.symbols; rebuild_view(); return true; }
    if (logical_action == "caret-left") { if (state_.editor.active) state_.editor.caret = previous_caret(state_.editor.value, state_.editor.caret); rebuild_view(); return true; }
    if (logical_action == "caret-right") { if (state_.editor.active) state_.editor.caret = next_caret(state_.editor.value, state_.editor.caret); rebuild_view(); return true; }
    if (state_.editor.active && logical_action == "l1") { state_.editor.caret = previous_caret(state_.editor.value, state_.editor.caret); rebuild_view(); return true; }
    if (state_.editor.active && logical_action == "r1") { state_.editor.caret = next_caret(state_.editor.value, state_.editor.caret); rebuild_view(); return true; }
    if (state_.editor.active && logical_action == "triangle") { steam::ZeroizeSecret(state_.editor.value); state_.editor.caret = 0; rebuild_view(); return true; }
    // Options presses Done, so a game controller or a USB keyboard's Options key can finish the text without moving to Done.
    if (state_.editor.active && logical_action == "options") { commit_editor(); rebuild_view(); return true; }
    if (state_.editor.active && (logical_action == "l2" || logical_action == "r2" || logical_action == "search")) return true;
    if (!state_.editor.active && state_.navigation.modals.empty() && state_.navigation.route == Route::Details
        && (logical_action == "l2" || logical_action == "r2")) {
        auto& offset = state_.navigation.scroll_offsets["details-content"];
        const auto* game = find_game(state_, state_.navigation.detail_id.value_or(state_.navigation.selected_game_id));
        const auto page = DetailsDescriptionPage(state_.simulation_mode);
        const int last_page = game ? std::max(0, static_cast<int>((game->description.size() + page - 1) / page) - 1) : 0;
        offset = std::clamp(offset + (logical_action == "l2" ? -1 : 1), 0, last_page);
        rebuild_view(); return true;
    }
    if (logical_action == "up") move(0, -1);
    else if (logical_action == "down") move(0, 1);
    else if (logical_action == "left") move(-1, 0);
    else if (logical_action == "right") move(1, 0);
    else if (logical_action == "l2") move(0, -4);
    else if (logical_action == "r2") move(0, 4);
    else if ((logical_action == "l1" || logical_action == "r1") && !state_.navigation.modals.empty() &&
             (state_.navigation.modals.back().id == "game-settings" || state_.navigation.modals.back().id == "preset-picker" ||
              state_.navigation.modals.back().id == "game-maintenance" ||
              state_.navigation.modals.back().id == "content-language" ||
              state_.navigation.modals.back().id == "button-mapping" || state_.navigation.modals.back().id == "controller-tuning" ||
              !state_.navigation.modals.back().setting_id.empty()))
        move(0, logical_action == "l1" ? -5 : 5);  // L1/R1 jump five rows, as on macOS
    else if (logical_action == "l1") change_route(-1);
    else if (logical_action == "r1") change_route(1);
    else if (logical_action == "cross") activate();
    else if (logical_action == "circle") cancel();
    else if (logical_action == "triangle" && live() && !state_.editor.active && state_.navigation.modals.empty() &&
             state_.navigation.route == Route::Saves)
        pending_cloud_requests_.push_back({state_.saves_job.kind == SavesJobKind::None ? CloudRequestKind::SyncAll
                                                                                     : CloudRequestKind::StopJob, {}});
    else if (logical_action == "search") { if (!state_.editor.active && state_.navigation.route == Route::Library && state_.navigation.modals.empty()) start_editor("Search", state_.library.query); }
    else if (logical_action == "triangle" && !state_.editor.active && !state_.navigation.modals.empty() &&
             state_.navigation.modals.back().id == "installed-games") {
        // Triangle is Uninstall selected from anywhere in the list; the batch view has nothing to start.
        auto& list = state_.navigation.modals.back();
        const auto at = std::find(list.action_keys.begin(), list.action_keys.end(), "installed-selection-uninstall");
        if (live() && at != list.action_keys.end() && !installed_selected_.empty()) {
            const int was = list.focused_action;
            const auto key = state_.navigation.focus_key;
            list.focused_action = static_cast<int>(at - list.action_keys.begin());
            state_.navigation.focus_key = *at;
            rebuild_view();
            activate();
            // The dialog returns to the row the player was on.
            auto& modals = state_.navigation.modals;
            const auto list_at = std::find_if(modals.begin(), modals.end(), [](const ModalEntry& modal) { return modal.id == "installed-games"; });
            if (list_at != modals.end()) list_at->focused_action = was;
            if (modals.back().id == "confirmation") modals.back().opener_focus_key = key;
            else state_.navigation.focus_key = key;
        }
    }
    else if (logical_action == "triangle" && !state_.editor.active && !state_.navigation.modals.empty() &&
             (state_.navigation.modals.back().id == "game-settings" || state_.navigation.modals.back().id == "controller-tuning" ||
              !state_.navigation.modals.back().setting_id.empty())) {
        // Triangle returns the focused setting (or the open picker's setting) to the preset value.
        const auto& modal = state_.navigation.modals.back();
        const auto id = !modal.setting_id.empty() ? modal.setting_id
            : state_.navigation.focus_key.rfind("setting-", 0) == 0 ? state_.navigation.focus_key.substr(8) : std::string{};
        auto record = state_.game_settings.find(state_.navigation.selected_game_id);
        const auto* definition = id.empty() ? nullptr : rt::FindSetting(id);
        // The program type follows the program; Triangle on the Program row resets both.
        const bool closed_needed = (definition && definition->apply == rt::ApplyMode::kGameClosed &&
                                    !game_closed(state_.navigation.selected_game_id)) || id == "architecture";
        // With it go the keys that belong to the row: a "Change anyway", the picked program and its
        // architecture, and library overrides an earlier build kept as additions.
        std::vector<std::string> keys = {id, rt::kUnlockPrefix + id};
        if (id == "launch_option") keys.insert(keys.end(), {"program", "architecture", "program_folder", "program_arguments", "start_check"});
        if (id == "dll_overrides") keys.push_back("library_overrides");
        const auto present = [&] {
            return record != state_.game_settings.end() &&
                   std::any_of(keys.begin(), keys.end(), [&](const std::string& key) { return record->second.overrides.count(key) > 0; });
        };
        if (!id.empty() && !closed_needed && present() && can_change_game_settings()) {
            for (const auto& key : keys) record->second.overrides.erase(key);
            ++record->second.revision;
            if (id == "refresh") follow_refresh_rate(state_.navigation.selected_game_id);
            if (!modal.setting_id.empty()) close_modal();
            refresh_game_settings(); save_profile();
        }
    }
    else if (logical_action == "triangle") {
        if (!state_.editor.active && state_.navigation.route == Route::Library && state_.navigation.modals.empty()) {
            // Live mode offers only sorts and filters backed by facts the library actually has.
            // A restored "recent-added" sort stays listed so it is visible and can be changed.
            const bool dated = state_.library.sort == "recent-added" ||
                std::any_of(state_.games.begin(), state_.games.end(),
                            [](const Game& game) { return game.added_at.has_value(); });
            // Demos is listed while there are demos, or while they are hidden so they can be shown again.
            const bool demos = state_.library.filters.contains("hide-demos") ||
                std::any_of(state_.games.begin(), state_.games.end(), [](const Game& game) { return game.demo; });
            std::vector<std::string> actions = live()
            ? (dated ? std::vector<std::string>{"sort-name", "sort-recent-played", "sort-recent-added", "sort-playtime",
                                                "filter-compat-playable", "filters-reset", "refresh-library", "cancel"}
                     : std::vector<std::string>{"sort-name", "sort-recent-played", "sort-playtime", "filter-compat-playable",
                                                "filters-reset", "refresh-library", "cancel"})
            : std::vector<std::string>{"sort-name", "sort-recent-played", "sort-recent-added", "sort-playtime", "sort-install-size",
                                      "filter-installed", "filter-genre-adventure", "filter-controller-full", "filter-compat-playable",
                                      "filters-reset", "refresh-library", "cancel"};
            if (demos)
                actions.insert(std::find(actions.begin(), actions.end(), "filters-reset"), "filter-hide-demos");
        open_modal({"filters", "Sort and filters", state_.navigation.focus_key, std::move(actions), 0, false});
        }
    }
    else if (logical_action == "options") {
        if (!state_.editor.active && state_.navigation.route == Route::Details && state_.navigation.modals.empty()) {
            if (live()) force_organization_selection_ = true;
            open_modal({"context", "Game actions", "details-more", details_context_actions(), 0, false});
        } else if (!state_.editor.active && state_.navigation.route == Route::Home && state_.navigation.selected_game_id.valid() && state_.navigation.modals.empty()) {
            if (live()) force_organization_selection_ = true;
            open_modal({"context", "Game actions", state_.navigation.focus_key, context_actions(live()), 0, false});
        }
        else if (!state_.editor.active && state_.navigation.route == Route::Library && !state_.navigation.modals.empty()) {}
        else if (!state_.editor.active && state_.navigation.route == Route::Library && state_.navigation.selected_game_id.valid() && state_.navigation.modals.empty()
                 && state_.navigation.focus_key.rfind("game-", 0) == 0) {
            if (live()) force_organization_selection_ = true;
            open_modal({"context", "Game actions", state_.navigation.focus_key, {"favorite", "collection", "note", "hide", "cancel"}, 0, false});
        }
        else if (!state_.editor.active && state_.navigation.route == Route::Downloads && state_.navigation.modals.empty()) activate();
    } else if (logical_action == "square") {
        if (state_.editor.active) {
            if (state_.editor.caret > 0) { const auto prior = previous_caret(state_.editor.value, state_.editor.caret); state_.editor.value.erase(prior, state_.editor.caret - prior); state_.editor.caret = prior; }
        } else if (state_.navigation.modals.empty() && (state_.navigation.route == Route::Home || state_.navigation.route == Route::Library || state_.navigation.route == Route::Details) && state_.navigation.selected_game_id.valid()) {
            if (state_.library.favorites.contains(state_.navigation.selected_game_id)) state_.library.favorites.erase(state_.navigation.selected_game_id);
            else state_.library.favorites.insert(state_.navigation.selected_game_id);
            save_profile();
        } else if (live() && state_.navigation.modals.empty() && state_.navigation.route == Route::Saves) {
            ask_upload_all();
        }
    } else if (logical_action.rfind("inject:", 0) == 0) {
        auto injected = logical_action.substr(7);
        if (injected == "controller-disconnect") injected = "controller-disconnected";
        if (injected == "controller-reconnect") injected = "controller-reconnected";
        if (injected == "storage-disconnect") {
            if (auto* volume = find_volume(state_, "external")) volume->available = false;
            for (auto& installation : state_.installations) if (installation.volume_id == "external") installation.available = false;
            for (auto& job : state_.jobs) if (job.volume_id == "external" && (job.state == JobState::Downloading || job.state == JobState::Verifying)) { job.state = JobState::Interrupted; job.failure = "Storage disconnected"; }
            state_.notices.push_back({"storage", "USB fixture library disconnected", "storage-retry", true, 0});
        } else if (injected == "storage-reconnect") {
            if (auto* volume = find_volume(state_, "external")) volume->available = true;
            for (auto& installation : state_.installations) if (installation.volume_id == "external" && installation.installed_bytes > 0) installation.available = true;
            state_.notices.erase(std::remove_if(state_.notices.begin(), state_.notices.end(), [](const Notice& notice) { return notice.id == "storage"; }), state_.notices.end());
        } else if (injected == "controller-disconnected") {
            state_.controller_connected = false;
            state_.notices.push_back({"controller", "Controller disconnected", "controller-test", true, 0});
        } else if (injected == "controller-reconnected") {
            state_.controller_connected = true;
            state_.notices.erase(std::remove_if(state_.notices.begin(), state_.notices.end(), [](const Notice& notice) { return notice.id == "controller"; }), state_.notices.end());
        } else if (injected == "download-failed" || injected == "verification-failed") {
            Job* job = nullptr;
            const auto wanted_stage = injected == "verification-failed" ? JobStage::Verify : JobStage::Download;
            const auto active = std::find_if(state_.jobs.begin(), state_.jobs.end(), [wanted_stage](const Job& value) {
                return value.stage == wanted_stage && (value.state == JobState::Downloading || value.state == JobState::Verifying);
            });
            if (active != state_.jobs.end()) job = &*active;
            if (!job && state_.navigation.focus_key.rfind("job-", 0) == 0) job = find_job(state_, state_.navigation.focus_key.substr(4));
            if (!job) job = find_job(state_, "job-1");
            if (job) { job->state = JobState::Failed; job->retry_stage = wanted_stage; job->stage = job->retry_stage; job->failure = injected == "verification-failed" ? "Verification failed" : "Download failed"; }
        } else if (injected == "checking-licenses") {
            const auto active = std::find_if(state_.jobs.begin(), state_.jobs.end(), [](const Job& value) {
                return value.stage == JobStage::Download && value.state != JobState::Downloaded;
            });
            if (active != state_.jobs.end()) {
                active->state = JobState::Downloading;
                active->checking_licenses = true;
                active->completed_bytes = 0;
                active->total_bytes = 0;
            }
        } else if (injected == "uninstall-failed") {
            const auto job = std::find_if(state_.jobs.begin(), state_.jobs.end(), [](const Job& value) { return value.kind == "uninstall" && value.state != JobState::Downloaded; });
            if (job != state_.jobs.end()) { job->state = JobState::Failed; job->stage = JobStage::Uninstall; job->retry_stage = JobStage::Uninstall; job->failure = "Uninstall failed"; }
        } else if (injected == "launch-failure" || injected == "stop-failure") {
            state_.session = SessionState::Failed; state_.session_error = injected == "launch-failure" ? "Simulated launch failed" : "Game did not stop"; state_.navigation.route = Route::Session; state_.navigation.focus_key = "session-retry";
        } else if (injected == "crash") {
            state_.session = SessionState::Crashed; state_.session_error = "Game exited unexpectedly"; state_.navigation.route = Route::Session; state_.navigation.focus_key = "session-logs";
        } else if (injected == "session-running") {
            state_.session = SessionState::Running; state_.session_error.clear(); state_.navigation.route = Route::Session; state_.navigation.focus_key = "session-return";
        } else if (injected == "runtime-ready") state_.runtime = RuntimeState::Ready;
        else if (injected == "runtime-failed") state_.runtime = RuntimeState::Failed;
        else if (injected == "logs-loading") start_logs();
        else if (injected == "logs-ready") finish_logs();
        else if (injected == "logs-empty") { state_.log_state = LogState::Ready; state_.log_lines.clear(); state_.log_offset = 0; }
        else if (injected == "logs-failure") {
            state_.log_state = LogState::Failed; state_.log_lines.clear(); state_.log_offset = 0;
            if (!state_.navigation.modals.empty() && state_.navigation.modals.back().id == "logs") {
                state_.navigation.modals.back().action_keys = {"logs-retry", "logs-fixture-controls", "logs-back"};
                state_.navigation.modals.back().focused_action = 0; state_.navigation.focus_key = "logs-retry";
            }
        } else if (injected == "delayed-art" || injected == "failed-art") {
            const GameId id = injected == "delayed-art" ? GameId::Steam(900007) : GameId::Steam(900008);
            auto game = std::find_if(state_.games.begin(), state_.games.end(), [id](const Game& value) { return value.id == id; });
            if (game != state_.games.end()) { game->art_pending = injected == "delayed-art"; game->art_failed = injected == "failed-art"; }
        } else if (injected == "library-refresh") state_.library_loading = true;
        else if (injected == "startup-loading") { state_.startup_loading = true; state_.navigation.route = Route::Startup; state_.navigation.focus_key = "startup-loading"; }
        else if (injected == "late-approval") {
            if (state_.account().authentication != AuthState::Cancelled && challenge_generation_ != cancelled_challenge_generation_) state_.account().authentication = AuthState::Syncing;
        } else {
            if (!scenario(injected)) return false;
            if (live() || state_.persistence_available) save_profile();
            return true;
        }
        if (live() || state_.persistence_available) save_profile();
        rebuild_view(); return true;
    }
    else { last_error_ = "Unknown action: " + logical_action; return false; }
    show_ready_prompt();  // a ready-to-play prompt waits for the player to close what was open
    show_alert_prompt();
    rebuild_view();
    if (live() || state_.persistence_available) save_profile();
    return true;
}

void Simulation::text(const std::string& utf8) {
    if (!state_.editor.active) return;
    state_.editor.value.insert(state_.editor.caret, utf8);
    state_.editor.caret += utf8.size();
    normalize_editor();
    rebuild_view();
}

void Simulation::SubmitEditorText(std::string utf8) {
    if (state_.editor.active) {
        if (state_.editor.secret) steam::ZeroizeSecret(state_.editor.value);
        state_.editor.value = utf8;
        state_.editor.caret = state_.editor.value.size();
        normalize_editor();
        action("done");
    }
    steam::ZeroizeSecret(utf8);
}

void Simulation::controller_sample(ControllerState sample) {
    state_.controller = sample;
}

void Simulation::advance_jobs() {
    for (auto& job : state_.jobs) if (job.state == JobState::Pausing) job.state = JobState::Paused;
    Job* active = nullptr;
    for (auto& job : state_.jobs) if (job.state == JobState::Downloading || job.state == JobState::Verifying) { active = &job; break; }
    if (!active) {
        auto queued = std::min_element(state_.jobs.begin(), state_.jobs.end(), [](const Job& left, const Job& right) {
            const int left_order = left.state == JobState::Queued ? left.queue_order : std::numeric_limits<int>::max();
            const int right_order = right.state == JobState::Queued ? right.queue_order : std::numeric_limits<int>::max();
            return left_order < right_order;
        });
        if (queued != state_.jobs.end() && queued->state == JobState::Queued)
            queued->state = queued->stage == JobStage::Download ? JobState::Downloading : JobState::Verifying;
        return;
    }
    if (active->state == JobState::Downloading) {
        const auto* volume = find_volume(state_, active->volume_id);
        if (!volume || !volume->available) { active->state = JobState::Interrupted; active->failure = "Storage disconnected"; return; }
        if (active->checking_licenses) return;  // an injected license check holds the transfer
        active->completed_bytes = std::min(active->total_bytes, active->completed_bytes + kTransferPerTick);
        if (active->completed_bytes >= active->total_bytes) { active->stage = JobStage::Verify; active->state = JobState::Verifying; active->stage_ticks = 0; }
    } else if (active->state == JobState::Verifying) {
        ++active->stage_ticks;
        if (active->stage_ticks >= 2) {
            active->state = JobState::Downloaded;
            active->completed_bytes = active->total_bytes;
            reconcile_storage(state_);
            const auto message = active->kind == "verify" ? "Verification complete"
                : active->kind == "uninstall" ? "Uninstall complete; saves preserved" : "Download complete";
            state_.notices.push_back({"job-complete-" + active->id, message, "open-game:" + ToKey(active->game_id), false, 5});
        }
    }
}

void Simulation::tick(int ticks) {
    if (live()) {
        rebuild_view();
        return;
    }
    for (int index = 0; index < std::max(0, ticks); ++index) {
        if (!state_.screenshot_mode) {
            ++state_.clock_ticks;
            advance_jobs();
            // A finished copy of an imported game: it plays from Playden's storage, not its folder.
            for (const auto& job : state_.jobs)
                if (job.kind == "install" && job.state == JobState::Downloaded && job.game_id.store == Store::kLocal &&
                    external_game(job.game_id)) {
                    const auto id = job.game_id;
                    std::erase_if(state_.external_games, [id](const LiveExternalGame& external) { return external.game_id == id; });
                    if (auto* installation = find_installation(state_, id)) installation->runtime_ready = true;
                }
            if (state_.account().authentication == AuthState::Waiting && state_.clock_ticks >= state_.account().challenge_expiry) {
                state_.account().authentication = AuthState::Expired;
                state_.navigation.focus_key = "qr-refresh";
            }
            if (state_.account().authentication == AuthState::Syncing) {
                state_.account().authentication = AuthState::SignedIn;
                state_.account().cache_available = true;
                state_.account().last_sync = state_.clock_ticks;
                state_.library_syncing = false;
                state_.navigation.route = Route::Home;
                state_.navigation.focus_key = game_key(state_.navigation.selected_game_id);
            }
            if (state_.session == SessionState::Preparing) state_.session = SessionState::Launching;
            else if (state_.session == SessionState::Launching) state_.session = SessionState::Running;
            else if (state_.session == SessionState::Stopping) { state_.session = SessionState::Idle; state_.navigation.route = Route::Details; state_.navigation.detail_id = state_.session_game_id; }
            if (state_.runtime == RuntimeState::Checking) state_.runtime = RuntimeState::Ready;
            if (state_.import_scanning) {
                // Folders added before stay added.
                auto found = fixture_import_candidates();
                for (auto& candidate : found)
                    for (const auto& known : state_.import_candidates)
                        if (import_candidate_key(known) == import_candidate_key(candidate)) candidate.imported = known.imported;
                if (find_game(state_, fixture_imported_game())) found.front().imported = fixture_imported_game();
                state_.import_candidates = std::move(found);
                state_.import_scanning = false;
                refresh_import_sheet();
            }
            if (state_.log_state == LogState::Loading) finish_logs();
            state_.startup_loading = false;
            state_.library_loading = false;
            for (auto& game : state_.games) if (game.art_pending) game.art_pending = false;
            for (auto& notice : state_.notices) if (!notice.persistent && notice.remaining_ticks > 0) --notice.remaining_ticks;
            state_.notices.erase(std::remove_if(state_.notices.begin(), state_.notices.end(), [](const Notice& notice) { return !notice.persistent && notice.remaining_ticks == 0; }), state_.notices.end());
        }
    }
    if (ticks > 0 && (live() || state_.persistence_available)) save_profile();
    rebuild_view();
}

std::string Simulation::snapshot() const {
    StringBuilder output;
    const auto visible = visible_games();
    output << "{\"simulation\":" << (state_.simulation_mode ? "true" : "false") << ",\"clock\":" << state_.clock_ticks << ",\"route\":\""
           << route_name(state_.navigation.route) << "\",\"surface\":\"" << json::escape(view_.surface)
           << "\",\"focus\":\"" << json::escape(state_.navigation.focus_key) << "\",\"selected_game_id\":"
           << state_.navigation.selected_game_id << ",\"account\":\"" << auth_name(state_.account().authentication)
           << "\",\"detail_id\":";
    if (state_.navigation.detail_id) output << *state_.navigation.detail_id; else output << "null";
    output << ",\"query\":\"" << json::escape(state_.library.query) << "\",\"scope\":\""
           << json::escape(state_.library.scope) << "\",\"sort\":\"" << json::escape(state_.library.sort)
           << "\",\"visible_count\":" << visible.size() << ",\"materialized_row_count\":" << view_.rows.size()
           << ",\"modal_depth\":" << state_.navigation.modals.size()
           << ",\"startup_loading\":" << (state_.startup_loading ? "true" : "false")
           << ",\"library_loading\":" << (state_.library_loading ? "true" : "false")
           << ",\"logs\":{\"state\":\"" << (state_.log_state == LogState::Loading ? "loading" : state_.log_state == LogState::Failed ? "failed" : "ready")
           << "\",\"context\":\"" << json::escape(state_.log_context) << "\",\"count\":" << state_.log_lines.size()
           << ",\"offset\":" << state_.log_offset << "}"
           << ",\"editor\":{\"active\":" << (state_.editor.active ? "true" : "false") << ",\"value\":\""
           << (state_.editor.secret ? std::string("<redacted>") : json::escape(state_.editor.value)) << "\",\"original\":\""
           << (state_.editor.secret ? std::string("<redacted>") : json::escape(state_.editor.original))
           << "\",\"caret\":" << state_.editor.caret << "},\"scroll_offsets\":{";
    bool first = true;
    for (const auto& [key, value] : state_.navigation.scroll_offsets) {
        if (!first) output << ','; first = false;
        output << '"' << json::escape(key) << "\":" << value;
    }
    output << "},\"filters\":[";
    first = true;
    for (const auto& filter : state_.library.filters) { if (!first) output << ','; first = false; output << '"' << json::escape(filter) << '"'; }
    output << "],\"jobs\":[";
    first = true;
    for (const auto& job : state_.jobs) {
        if (!first) output << ',';
        first = false;
        output << "{\"id\":\"" << json::escape(job.id) << "\",\"game_id\":" << job.game_id
               << ",\"state\":\"" << job_state_name(job.state) << "\",\"stage\":\"" << job_stage_name(job.stage)
               << "\",\"retry_stage\":\"" << job_stage_name(job.retry_stage) << "\",\"volume\":\"" << json::escape(job.volume_id)
               << "\",\"completed\":" << job.completed_bytes << ",\"total\":" << job.total_bytes
               << ",\"stage_ticks\":" << job.stage_ticks << ",\"reserved\":" << job.reserved_bytes
               << ",\"reservation_held\":" << (job.reservation_held ? "true" : "false")
               << ",\"completion_applied\":" << (job.completion_applied ? "true" : "false")
               << ",\"failure\":\"" << json::escape(job.failure) << "\"}";
    }
    output << "],\"installations\":[";
    first = true;
    for (const auto& installation : state_.installations) {
        if (!first) output << ','; first = false;
        output << "{\"game_id\":" << installation.game_id << ",\"volume\":\"" << json::escape(installation.volume_id)
               << "\",\"available\":" << (installation.available ? "true" : "false") << ",\"runtime_ready\":"
               << (installation.runtime_ready ? "true" : "false") << ",\"saves_present\":" << (installation.saves_present ? "true" : "false") << '}';
    }
    output << "],\"volumes\":[";
    first = true;
    for (const auto& volume : state_.volumes) {
        if (!first) output << ','; first = false;
        output << "{\"id\":\"" << json::escape(volume.id) << "\",\"used\":" << volume.used_bytes
               << ",\"reserved\":" << volume.reserved_bytes << ",\"capacity\":" << volume.capacity_bytes
               << ",\"available\":" << (volume.available ? "true" : "false") << '}';
    }
    output << "],\"game_settings\":[";
    first = true;
    for (const auto& [id, record] : state_.game_settings) {
        if (!first) output << ','; first = false;
        output << "{\"game_id\":" << id << ",\"settings\":" << rt::SerializeGameSettingsRecord(record) << '}';
    }
    output << "],\"collections\":[";
    first = true;
    for (const auto& collection : state_.library.collections) {
        if (!first) output << ','; first = false;
        output << "{\"id\":\"" << json::escape(collection.id) << "\",\"name\":\"" << json::escape(collection.name)
               << "\",\"pinned\":" << (collection.pinned ? "true" : "false") << ",\"members\":[";
        bool first_member = true;
        for (const auto id : collection.members) { if (!first_member) output << ','; first_member = false; output << id; }
        output << "]}";
    }
    output << "],\"reduced_motion\":" << (state_.reduced_motion ? "true" : "false")
           << ",\"controller_connected\":" << (state_.controller_connected ? "true" : "false")
           << ",\"controller\":{\"left_x_milli\":" << static_cast<int>(state_.controller.left_x * 1000.0F)
           << ",\"left_y_milli\":" << static_cast<int>(state_.controller.left_y * 1000.0F)
           << ",\"right_x_milli\":" << static_cast<int>(state_.controller.right_x * 1000.0F)
           << ",\"right_y_milli\":" << static_cast<int>(state_.controller.right_y * 1000.0F)
           << ",\"l2_milli\":" << static_cast<int>(state_.controller.l2 * 1000.0F)
           << ",\"r2_milli\":" << static_cast<int>(state_.controller.r2 * 1000.0F) << "}}";
    return output.value;
}


void Simulation::SetSettingsData(std::vector<rt::SettingsPreset> presets, std::vector<rt::GameRecipe> recipes) {
    settings_presets_ = std::move(presets);
    recipes_.clear();
    for (auto& recipe : recipes) recipes_[rt::RecipeGame(recipe)] = std::move(recipe);
    simulated_recipes_.clear();
    rebuild_view();
}

void Simulation::SetGameSettingsFile(std::filesystem::path path) {
    game_settings_file_ = std::move(path);
    std::map<GameId, rt::GameSettingsRecord> records;
    std::string reason;
    settings_load_failed_ = !rt::LoadGameSettingsFile(game_settings_file_.string(), &records, &reason);
    if (settings_load_failed_) {
        push_notice({"settings-load-failed", "Game settings could not be read", "", false, kLiveNoticeSeconds, true, reason});
        rebuild_view();
        return;
    }
    state_.game_settings = std::move(records);
    saved_game_settings_ = state_.game_settings;
    rebuild_view();
}

void Simulation::SetKitCapabilities(rt::KitCapabilities kit) {
    kit_ = kit;
    refresh_game_settings();
    rebuild_view();
}

void Simulation::SetGeneratedRecipes(const std::vector<rt::GameRecipe>& recipes) {
    std::map<GameId, rt::GameRecipe> generated;
    for (const auto& recipe : recipes) generated[rt::RecipeGame(recipe)] = recipe;
    if (generated.size() == generated_recipes_.size() &&
        std::equal(generated.begin(), generated.end(), generated_recipes_.begin(), [](const auto& a, const auto& b) {
            return a.first == b.first && rt::SerializeGameRecipe(a.second) == rt::SerializeGameRecipe(b.second);
        }))
        return;
    generated_recipes_ = std::move(generated);
    refresh_game_settings();
    rebuild_view();
}

void Simulation::SetGameSetups(std::map<GameId, LiveGameSetup> setups) {
    const auto same = [](const LiveGameSetup& a, const LiveGameSetup& b) {
        if (a.scanned != b.scanned || a.refusal_soft != b.refusal_soft || a.refusal != b.refusal ||
            a.programs.size() != b.programs.size() || a.draft.has_value() != b.draft.has_value() ||
            a.steam_choices.size() != b.steam_choices.size())
            return false;
        for (std::size_t index = 0; index < a.steam_choices.size(); ++index)
            if (a.steam_choices[index].path != b.steam_choices[index].path) return false;
        for (std::size_t index = 0; index < a.programs.size(); ++index)
            if (a.programs[index].path != b.programs[index].path ||
                a.programs[index].architecture != b.programs[index].architecture ||
                a.programs[index].refusal != b.programs[index].refusal)
                return false;
        return !a.draft || rt::SerializeGameRecipe(*a.draft) == rt::SerializeGameRecipe(*b.draft);
    };
    if (setups.size() == game_setups_.size() &&
        std::equal(setups.begin(), setups.end(), game_setups_.begin(),
                   [&](const auto& a, const auto& b) { return a.first == b.first && same(a.second, b.second); }))
        return;
    game_setups_ = std::move(setups);
    for (const auto& [id, setup] : game_setups_)
        if (setup.scanned) requested_program_scans_.erase(id);
    // A setup that now waits for its program asks once: the sheet over the game's own page, otherwise
    // a notification; the Play row asks until then.
    // Asked until the runtime no longer waits: a setup still waiting right after the player chose
    // (the runtime hasn't taken the new request yet) doesn't ask again.
    std::erase_if(steam_programs_asked_, [&](GameId id) {
        const auto found = game_setups_.find(id);
        return found == game_setups_.end() || found->second.steam_choices.empty();
    });
    for (const auto& [id, setup] : game_setups_) {
        if (setup.steam_choices.empty() || !steam_programs_asked_.insert(id).second) continue;
        if (state_.navigation.route == Route::Details && state_.navigation.detail_id == id && state_.navigation.modals.empty()) {
            open_steam_programs(id);
        } else if (const auto* game = find_game(state_, id)) {
            push_notice({"steam-programs-" + ToKey(id), "Choose the program for " + game->title, "", false,
                         kLiveNoticeSeconds * 2, false, "Steam lists more than one. Choose Program on the game's page."});
        }
    }
    refresh_game_settings();
    open_settings_after_prepare();
    // An open Program picker shows the scan's programs as soon as they arrive.
    if (!state_.navigation.modals.empty() && state_.navigation.modals.back().id == "choice-picker" &&
        state_.navigation.modals.back().setting_id == "launch_option") {
        close_modal();
        open_setting("launch_option");
    }
    rebuild_view();
}

std::vector<GameId> Simulation::TakeProgramScanRequests() {
    auto requests = std::move(pending_program_scans_);
    pending_program_scans_.clear();
    for (const auto id : requests) requested_program_scans_.erase(id);
    return requests;
}

void Simulation::SetRecipeExportTarget(std::filesystem::path directory, std::string firmware) {
    recipe_export_directory_ = std::move(directory);
    console_firmware_ = std::move(firmware);
}

const rt::GameRecipe& Simulation::RecipeFor(GameId id) const {
    if (const auto found = recipes_.find(id); found != recipes_.end()) return found->second;
    if (const auto found = generated_recipes_.find(id); found != generated_recipes_.end())
        return found->second;
    // A game Playden refused: the sheet starts from the draft so the player can choose a program.
    if (const auto found = game_setups_.find(id); found != game_setups_.end() && found->second.draft)
        return *found->second.draft;
    // Games not prepared yet get a generic one so the sheet can be shown; preparing a real
    // install generates the game's own from Steam's launch options.
    auto& recipe = simulated_recipes_[id];
    if (recipe.launch_entries.empty()) {
        const auto* game = find_game(state_, id);
        recipe.id = "simulated-" + ToKey(id);
        recipe.version = 1;
        recipe.app_id = id.steam_app().value_or(0);
        if (!id.steam_app()) recipe.game = id;
        recipe.name = game ? game->title : "Game " + ToKey(id);
        recipe.launch_entries = {{"game", recipe.name, "game.exe", ""}};
        recipe.arguments = "{resolution}";
        recipe.graphics = "dxvk";
    }
    return recipe;
}

rt::KitCapabilities Simulation::kit_for(GameId id) const {
    const auto runtime = state_.runtime_games.find(id);
    return rt::KitForTitleRuntime(kit_, runtime == state_.runtime_games.end() || runtime->second.runtime_current);
}

const rt::SettingsPreset* Simulation::preset_for(GameId id) const {
    const auto record = state_.game_settings.find(id);
    const std::string wanted = record == state_.game_settings.end() || record->second.preset_id.empty()
        ? std::string("playden-default") : record->second.preset_id;
    for (const auto& preset : settings_presets_)
        if (preset.id == wanted) return &preset;
    return nullptr;
}

bool Simulation::EffectiveSettingsFor(GameId id, rt::EffectiveSettings* settings) const {
    const auto record = state_.game_settings.find(id);
    static const std::map<std::string, std::string> none;
    rt::SettingsError error;
    return rt::BuildEffectiveSettings(RecipeFor(id), preset_for(id), record == state_.game_settings.end() ? none : record->second.overrides,
                                      kit_for(id), settings, &error);
}

std::vector<std::string> Simulation::game_settings_actions() const {
    using G = rt::SettingGroup;
    const auto game = state_.navigation.selected_game_id;
    std::vector<std::string> actions = {"preset-picker"};
    const auto rows = [&](G group) {
        for (const auto& definition : rt::SettingsCatalogue())
            if (definition.group == group && game_settings_row_shown(definition, game)) actions.push_back("setting-" + definition.id);
    };
    // Rows by topic under headings that only label them; Compatibility and Advanced start collapsed.
    actions.push_back("heading-game");
    rows(G::kGame);
    // Another store's game may use Steamworks: the Steam app ID its emulator is given.
    if (game.store == Store::kLocal) {
        const auto program = std::find(actions.begin(), actions.end(), "setting-launch_option");
        actions.insert(program == actions.end() ? actions.end() : program + 1, "steam-app-id");
    }
    actions.push_back("heading-display");
    rows(G::kDisplay);
    actions.push_back("heading-controls");
    rows(G::kControls);
    actions.push_back("button-mapping");
    actions.push_back("controller-tuning");
    actions.push_back("heading-sound");
    rows(G::kSound);
    actions.push_back("section-compatibility");
    if (expanded_settings_sections_.count("section-compatibility")) {
        rows(G::kCompatibility);
        actions.push_back("heading-graphics");
        rows(G::kGraphics);
        actions.push_back("heading-windows");
        rows(G::kWindows);
    }
    actions.push_back("section-advanced");
    if (expanded_settings_sections_.count("section-advanced")) {
        rows(G::kAdvanced);
        actions.push_back("effective-command");
        actions.push_back("export-recipe");
    }
    actions.push_back("reset-settings");
    actions.push_back("cancel");
    return actions;
}

// Whether the sheet lists a setting's row: Known problems only for a game that has one.
bool Simulation::game_settings_row_shown(const rt::SettingDefinition& definition, GameId game) const {
    return rt::SettingShown(definition, kit_) && (definition.id != "start_check" || known_problem(game));
}

// Whether Game settings shows "Known problems": Playden refused the game for a reason a run
// could get past, the chosen program has one, or the player already chose to try anyway.
bool Simulation::known_problem(GameId id) const {
    const auto record = state_.game_settings.find(id);
    if (record != state_.game_settings.end() && record->second.overrides.count("start_check")) return true;
    const auto setup = game_setups_.find(id);
    if (setup == game_setups_.end()) return false;
    if (setup->second.draft && setup->second.refusal_soft && !setup->second.refusal.empty()) return true;
    rt::EffectiveSettings values;
    if (!EffectiveSettingsFor(id, &values) || !values.launch) return false;
    for (const auto& program : setup->second.programs)
        if (program.refusal_soft && program.path == values.launch->executable) return true;
    return false;
}

std::vector<std::string> Simulation::details_context_actions() const {
    auto actions = context_actions(live());
    // A game Playden could not prepare opens Game settings too, to choose a program or try anyway.
    const auto setup = game_setups_.find(state_.navigation.selected_game_id);
    // A hard refusal blocks that program, not the picker: another program in the folder may start.
    const bool refused = live() && setup != game_setups_.end() && setup->second.draft &&
                         !game_prepared(state_.navigation.selected_game_id);
    // An installed game not set up yet (a refusal from before Playden restarted is not kept) sets
    // itself up first, so Game settings is there before Play is pressed once (#46).
    // Maintenance is there for every installed game, whatever its state (a title waiting for
    // recovery gets out through Recreate bottle); rows that can't run say why.
    const auto selected = state_.navigation.selected_game_id;
    // An imported game playing from its folder is "installed" there: its settings apply, and it
    // leaves the library rather than being uninstalled.
    const bool in_place = external_game(selected) != nullptr;
    if (in_place) std::replace(actions.begin(), actions.end(), std::string("uninstall"), std::string("import-remove"));
    const bool installed = (live() && is_installed(state_, selected)) || in_place;
    if (installed || game_prepared(state_.navigation.selected_game_id))
        actions.insert(actions.begin(), {"game-settings", "game-maintenance"});
    else if (refused)
        actions.insert(actions.begin(), "game-settings");
    // A title waiting for the PS5 is ready to play from Home; asking the console again is a side action.
    const auto runtime = state_.runtime_games.find(state_.navigation.selected_game_id);
    if (live() && runtime != state_.runtime_games.end() && runtime->second.awaiting_registration &&
        runtime->second.can_prepare)
        actions.insert(actions.begin(), "runtime-check-again");
    // The game's saves in the Saves tab, after Game settings and Maintenance; imported games have no link.
    if (live() && is_installed(state_, state_.navigation.selected_game_id) && selected.store != Store::kLocal) {
        const auto maintenance = std::find(actions.begin(), actions.end(), "game-maintenance");
        actions.insert(maintenance == actions.end() ? actions.begin() : maintenance + 1, "manage-saves");
    }
    return actions;
}

std::vector<std::string> Simulation::maintenance_actions() const {
    return {"maintenance-reset", "maintenance-update", "maintenance-repair", "maintenance-shader", "maintenance-backup",
            "maintenance-recreate", "cancel"};
}

bool Simulation::game_prepared(GameId id) const {
    if (!live()) return find_game(state_, id) != nullptr;
    const auto found = state_.runtime_games.find(id);
    return found != state_.runtime_games.end() && found->second.prepared;
}

bool Simulation::game_closed(GameId id, std::string* reason) const {
    const auto* game = find_game(state_, id);
    const std::string title = game ? game->title : "the game";
    if (!live()) {
        const bool closed = state_.session == SessionState::Idle || state_.session_game_id != id;
        if (!closed && reason) *reason = "Quit " + title + " to change this.";
        return closed;
    }
    const auto found = state_.runtime_games.find(id);
    if (found == state_.runtime_games.end()) { if (reason) *reason = "Prepare the game first."; return false; }
    const auto& value = found->second;
    if (value.activity == TitleActivity::Running || value.play_status == "Playing" || value.play_status == "Starting…") {
        if (reason) *reason = "Quit " + title + " to change this.";
        return false;
    }
    if (value.activity == TitleActivity::Unknown) {
        if (reason) *reason = "Checking whether " + title + " is running…";
        return false;
    }
    return true;
}

void Simulation::request_maintenance(GameId id, MaintenanceKind kind) {
    if (!live()) {
        // Fixture mode only describes what the real action does.
        const char* words = kind == MaintenanceKind::ClearShaderCache ? "Shader cache cleared"
                          : kind == MaintenanceKind::Repair ? "Repaired"
                          : kind == MaintenanceKind::BackupSaves ? "Saves backed up"
                          : kind == MaintenanceKind::RecreateBottle ? "Bottle recreated"
                          : kind == MaintenanceKind::UpdateRuntime ? "Runtime updated" : "Settings applied";
        push_notice({"maintenance-" + ToKey(id), words, "", false, kLiveNoticeSeconds, false, "Simulated"});
        return;
    }
    pending_maintenance_requests_.push_back({id, kind});
    auto& game = state_.runtime_games[id];
    game.maintenance = kind;
    game.maintenance_running = true;
    game.maintenance_message.clear();
}

std::map<std::string, std::string> Simulation::kept_start_choices(GameId id) const {
    const auto record = state_.game_settings.find(id);
    if (record == state_.game_settings.end()) return {};
    auto overrides = record->second.overrides;
    clear_keeping_start_choices(&overrides);
    return overrides;
}

void Simulation::build_preset_comparison(GameId id, const std::string& preset_id) {
    const rt::SettingsPreset* chosen = nullptr;
    for (const auto& preset : settings_presets_)
        if (preset.id == preset_id) chosen = &preset;
    rt::EffectiveSettings current, after;
    rt::SettingsError error;
    if (!chosen || !EffectiveSettingsFor(id, &current) ||
        !rt::BuildEffectiveSettings(RecipeFor(id), chosen, kept_start_choices(id), kit_for(id), &after, &error))
        return;
    ViewData::Comparison comparison;
    comparison.title = chosen->name;
    comparison.hint = chosen->try_when;
    const auto label = [&](const rt::SettingDefinition& definition, const rt::EffectiveSettings& values) {
        const auto& value = values.values.at(definition.id);
        if (definition.id == "launch_option") return values.launch ? values.launch->label : value.value;
        if (definition.kind == rt::SettingKind::kText) return value.value.empty() ? std::string("Empty") : value.value;
        auto text = choice_label(definition, rt::ChosenValue(value));
        if (value.locked || (definition.id == "graphics" && value.source == rt::ValueSource::kRecipe)) text += " (game)";
        return text;
    };
    int changes = 0, buttons = 0;
    for (const auto& definition : rt::SettingsCatalogue()) {
        const bool differs = rt::ChosenValue(current.values.at(definition.id)) != rt::ChosenValue(after.values.at(definition.id));
        if (definition.id.rfind("button.", 0) == 0) { buttons += differs ? 1 : 0; continue; }
        const bool key = definition.id == "controller" || definition.id == "resolution" ||
                         definition.id == "performance_overlay" || definition.id == "graphics";
        if (differs) ++changes;
        if ((differs || key) && comparison.rows.size() < 8)
            comparison.rows.push_back({definition.label, label(definition, current), label(definition, after)});
    }
    if (buttons > 0) {
        changes += buttons;
        if (comparison.rows.size() < 8)
            comparison.rows.push_back({"Button mapping", "", std::to_string(buttons) + (buttons == 1 ? " button" : " buttons") + " change"});
    }
    const auto record = state_.game_settings.find(id);
    const bool custom = record != state_.game_settings.end() && !record->second.overrides.empty();
    comparison.note = changes == 0 ? (custom ? "Clears your changes; the values stay the same." : "Nothing changes.")
                    : std::to_string(changes) + (changes == 1 ? " setting changes" : " settings change") +
                      ". All apply on next launch. What the game requires stays.";
    view_.comparison = std::move(comparison);
}

void Simulation::open_setting(const std::string& id) {
    const auto* definition = rt::FindSetting(id);
    const auto action_name = "setting-" + id;
    rt::EffectiveSettings values;
    if (!definition || !EffectiveSettingsFor(state_.navigation.selected_game_id, &values)) return;
    const auto& value = values.values.at(id);
    if ((!value.available && !value.waits_for_update) || id == "architecture" ||
        (definition->apply == rt::ApplyMode::kGameClosed && !game_closed(state_.navigation.selected_game_id))) {
        // Disabled rows stay focusable so their reason can be read; Cross does nothing.
    } else if (value.locked && !value.unlocked) {
        ModalEntry confirmation{"confirmation", "Change anyway?", action_name, {"cancel", "unlock-setting"}, 0, false, "", id};
        confirmation.body = "This game's recipe requires " + definition->label + ". If you change it, the game may not start. "
                            "Reset to recipe brings the required value back.";
        open_modal(std::move(confirmation));
    } else if (id.rfind("button.", 0) == 0) {
        std::vector<std::string> actions;
        int focused = 0;
        for (std::size_t index = 0; index < common_bindings().size(); ++index) {
            if (value.source == rt::ValueSource::kUser && common_bindings()[index].first == value.value) focused = static_cast<int>(index);
            actions.push_back("choice-" + std::to_string(index));
        }
        actions.push_back("choice-custom");
        actions.push_back("cancel");
        open_modal({"choice-picker", definition->label, action_name, std::move(actions), focused, false, "", id});
    } else if (id == "dll_overrides" || id == "arguments" || id == "program_folder" || id == "program_arguments") {
        // Values the recipe starts: edit them, or go back to the recipe's.
        open_modal({"setting-value", definition->label, action_name, setting_value_actions(id), 0, false, "", id});
    } else if (definition->kind == rt::SettingKind::kText) {
        start_editor(definition->label, value.value);
        editing_setting_id_ = id;
    } else {
        const auto& recipe = RecipeFor(state_.navigation.selected_game_id);
        const bool launch = id == "launch_option";
        const std::size_t count = launch ? recipe.launch_entries.size() : definition->choices.size();
        std::vector<std::string> actions;
        int focused = 0;
        const auto setup = launch ? game_setups_.find(state_.navigation.selected_game_id) : game_setups_.end();
        // Each program is listed once: a folder program Steam names shows as Steam's entry, and the
        // recipe's entry for the player's earlier pick as the folder program it is.
        const auto in_folder = [&](const std::string& executable) {
            if (setup == game_setups_.end()) return false;
            for (const auto& program : setup->second.programs)
                if (BottlePath(program.path) == BottlePath(executable)) return true;
            return false;
        };
        const auto steam_names = [&](const std::string& path) {
            for (const auto& entry : recipe.launch_entries)
                if (entry.id != rt::kProgramEntryId && BottlePath(entry.executable) == BottlePath(path)) return true;
            return false;
        };
        const bool picked = launch && values.launch && values.launch->id == rt::kProgramEntryId;
        for (std::size_t index = 0; index < count; ++index) {
            if (launch && recipe.launch_entries[index].id == rt::kProgramEntryId &&
                in_folder(recipe.launch_entries[index].executable))
                continue;
            const auto& choice = launch ? recipe.launch_entries[index].id : definition->choices[index].value;
            if (choice == rt::ChosenValue(value) ||
                (picked && BottlePath(recipe.launch_entries[index].executable) == BottlePath(values.launch->executable)))
                focused = static_cast<int>(actions.size());
            actions.push_back("choice-" + std::to_string(index));
        }
        if (launch) {
            // Steam's launch options are suggestions; any program in the game folder can start.
            if (setup != game_setups_.end())
                for (std::size_t index = 0; index < setup->second.programs.size(); ++index) {
                    if (steam_names(setup->second.programs[index].path)) continue;
                    if (picked && BottlePath(setup->second.programs[index].path) == BottlePath(values.launch->executable))
                        focused = static_cast<int>(actions.size());
                    actions.push_back("program-" + std::to_string(index));
                }
            // Every opening scans again, so programs a game update added show up and a scan that was
            // dropped is asked for once more; the last list shows meanwhile.
            if (live()) {
                if (requested_program_scans_.insert(state_.navigation.selected_game_id).second)
                    pending_program_scans_.push_back(state_.navigation.selected_game_id);
                if (setup == game_setups_.end() || !setup->second.scanned) actions.push_back("program-scan");
            }
        }
        if (launch || value.unlocked) actions.push_back("value-reset");
        actions.push_back("cancel");
        open_modal({"choice-picker", definition->label, action_name, std::move(actions), focused, false, "", id});
    }
}

std::vector<std::string> Simulation::setting_value_actions(const std::string& id) const {
    std::vector<std::string> actions;
    if (id == "dll_overrides") {
        rt::EffectiveSettings values;
        if (EffectiveSettingsFor(state_.navigation.selected_game_id, &values))
            for (std::size_t index = 0; index < split_overrides(values.dll_overrides).size(); ++index)
                actions.push_back("override-remove-" + std::to_string(index));
        actions.push_back("override-add");
    }
    actions.push_back("value-edit");
    actions.push_back("value-reset");
    actions.push_back("cancel");
    return actions;
}

void Simulation::export_recipe(GameId id) {
    // Community recipes name a Steam app (steam_appid); other games have none to share.
    if (!id.steam_app()) return;
    rt::EffectiveSettings values;
    if (!EffectiveSettingsFor(id, &values) || !values.launch) return;
    auto community = rt::BuildCommunityRecipe(RecipeFor(id), values);
    const auto* game = find_game(state_, id);
    if (game) community.recipe.name = game->title;
    community.min_kit = state_.kit.kit_id;
    community.tested_firmware = console_firmware_;
    community.tested_date = playden::app::FormatLogTimestamp(static_cast<std::int64_t>(std::time(nullptr))).substr(0, 10);
    // The player's rating and note for the game are what they found.
    const auto rating = state_.library.ratings.find(id);
    community.status = rating == state_.library.ratings.end() ? "untested"
                     : rating->second == Compatibility::Works ? "works"
                     : rating->second == Compatibility::Playable ? "playable"
                     : rating->second == Compatibility::Broken ? "broken" : "untested";
    if (const auto note = state_.library.notes.find(id); note != state_.library.notes.end()) community.notes = note->second;
    const auto text = rt::SerializeCommunityRecipe(community);
    if (recipe_export_directory_.empty()) {
        push_notice({"recipe-exported", "Recipe exported", "", false, kLiveNoticeSeconds, false, "Simulated"});
        return;
    }
    const auto path = recipe_export_directory_ / ("recipe-" + ToKey(id) + ".json");
    const auto temporary = recipe_export_directory_ / ("recipe-" + ToKey(id) + ".json.tmp");
    std::error_code error;
    std::filesystem::create_directories(recipe_export_directory_, error);
    // POSIX calls, not iostreams: the console's C library lacks the locale functions they need.
    bool written = false;
    const int file = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (file >= 0) {
        std::size_t done = 0;
        while (done < text.size()) {
            const auto count = ::write(file, text.data() + done, text.size() - done);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) break;
            done += static_cast<std::size_t>(count);
        }
        written = done == text.size() && ::fsync(file) == 0;
        written = ::close(file) == 0 && written;
    }
    if (written) std::filesystem::rename(temporary, path, error);
    if (!written || error) {
        std::filesystem::remove(temporary, error);
        push_notice({"recipe-exported", "The recipe could not be saved", "", false, kLiveNoticeSeconds, true, path.string()});
        return;
    }
    ModalEntry done{"confirmation", "Recipe exported", "export-recipe", {"alert-ok"}, 0, false};
    done.body = "Saved to " + path.string() + ". Add your name as author, then send it as a pull request to Playden's "
                "repository (docs/community-recipes.md).";
    open_modal(std::move(done));
}

void Simulation::refresh_game_settings() {
    for (auto& modal : state_.navigation.modals) {
        if (modal.id != "game-settings") continue;
        const auto focused = modal.action_keys[static_cast<std::size_t>(modal.focused_action)];
        modal.action_keys = game_settings_actions();
        const auto found = std::find(modal.action_keys.begin(), modal.action_keys.end(), focused);
        modal.focused_action = found == modal.action_keys.end() ? 0 : static_cast<int>(found - modal.action_keys.begin());
        if (&modal == &state_.navigation.modals.back()) state_.navigation.focus_key = modal.action_keys[static_cast<std::size_t>(modal.focused_action)];
    }
}

void Simulation::announce_settings_saved() {
    if (!settings_opened_record_) return;
    const auto before = *settings_opened_record_;
    settings_opened_record_.reset();
    if (settings_save_failed_ || settings_load_failed_) return;
    const auto found = state_.game_settings.find(state_.navigation.selected_game_id);
    const auto after = found == state_.game_settings.end() ? rt::GameSettingsRecord{} : found->second;
    // Count settings whose stored choice differs; a preset change counts once.
    std::set<std::string> ids;
    for (const auto& [id, value] : before.overrides) ids.insert(id);
    for (const auto& [id, value] : after.overrides) ids.insert(id);
    int changes = before.preset_id != after.preset_id ? 1 : 0;
    for (const auto& id : ids) {
        const auto old_value = before.overrides.find(id);
        const auto new_value = after.overrides.find(id);
        if ((old_value == before.overrides.end()) != (new_value == after.overrides.end()) ||
            (old_value != before.overrides.end() && old_value->second != new_value->second))
            ++changes;
    }
    if (changes == 0) return;
    const auto game = state_.navigation.selected_game_id;
    const auto* preset = preset_for(game);
    const std::string name = preset ? preset->name : "Playden default";
    // Say when the changes apply: a running game gets them from its next start in Playden.
    std::string when = std::to_string(changes) + (changes == 1 ? " change applies" : " changes apply");
    // Only a game known to run waits for its next start from Playden; an unanswered check
    // most likely means a stopped game, and the apply below finds out.
    const auto runtime_game = state_.runtime_games.find(game);
    const bool running = live() ? runtime_game != state_.runtime_games.end() &&
                                      (runtime_game->second.activity == TitleActivity::Running ||
                                       runtime_game->second.play_status == "Playing")
                                : !game_closed(game);
    when += running ? " the next time Playden starts the game" : " on next launch";
    push_notice({"settings-saved", "Settings saved · " + (after.overrides.empty() ? name : "Custom · from " + name), "", false,
                 kLiveNoticeSeconds, false, when});
    // A prepared title gets the new settings now when it is stopped, a new program or folder
    // included (RuntimeHost::RepublishSettings); otherwise Play applies them.
    if (live() && game_prepared(game)) pending_maintenance_requests_.push_back({game, MaintenanceKind::ApplySettings});
}

bool Simulation::can_change_game_settings() {
    const auto found = state_.game_settings.find(state_.navigation.selected_game_id);
    if (found == state_.game_settings.end() || found->second.revision != std::numeric_limits<std::uint64_t>::max()) return true;
    push_notice({"settings-revision-exhausted", "Game settings could not be changed", "", false,
                 kLiveNoticeSeconds, true, "The settings revision has reached its limit."});
    return false;
}

void Simulation::set_game_setting(const std::string& id, const std::string& value) {
    if (!can_change_game_settings()) return;
    const auto game = state_.navigation.selected_game_id;
    auto& record = state_.game_settings[game];
    if (start_choice(id) || id == "launch_option") rechosen_after_refusal_.insert(game);
    // The full library override list replaces what earlier builds kept as additions.
    if (id == "dll_overrides") record.overrides.erase("library_overrides");
    auto overrides = record.overrides;
    overrides.erase(id);
    rt::EffectiveSettings base;
    rt::SettingsError error;
    // Choosing the inherited value removes the override, so the row is no longer "changed".
    const bool inherited = rt::BuildEffectiveSettings(RecipeFor(game), preset_for(game), overrides, kit_for(game), &base, &error)
        && rt::ChosenValue(base.values.at(id)) == value;
    if (inherited) record.overrides.erase(id);
    else record.overrides[id] = value;
    ++record.revision;
    if (id == "refresh") follow_refresh_rate(game);
}

// Frame limit follows Refresh rate: a cap of 120 or 40 the player chose that the new rate can't pace goes back to Off,
// and the toast says so, instead of a saved choice that no longer applies. Runs after every change to Refresh rate.
void Simulation::follow_refresh_rate(GameId game) {
    const auto found = state_.game_settings.find(game);
    if (found == state_.game_settings.end()) return;
    auto& record = found->second;
    const auto limit = record.overrides.find("frame_limit");
    if (limit == record.overrides.end() || (limit->second != "120" && limit->second != "40")) return;
    rt::EffectiveSettings after;
    rt::SettingsError error;
    if (!rt::BuildEffectiveSettings(RecipeFor(game), preset_for(game), record.overrides, kit_for(game), &after, &error)) return;
    if (rt::SettingChoiceUnavailable(after, "frame_limit", limit->second).empty()) return;
    const auto cap = limit->second;
    // Off is saved unless the game would have it anyway: a recipe or preset cap that only falls back to Off at 60 Hz
    // would come back at 120 Hz.
    auto overrides = record.overrides;
    overrides.erase("frame_limit");
    rt::EffectiveSettings base;
    const bool inherited_off = rt::BuildEffectiveSettings(RecipeFor(game), preset_for(game), overrides, kit_for(game), &base, &error) &&
                               rt::ChosenValue(base.values.at("frame_limit")) == "off" && base.values.at("frame_limit").reason.empty();
    if (inherited_off) record.overrides.erase("frame_limit");
    else record.overrides["frame_limit"] = "off";
    ++record.revision;
    push_notice({"frame-limit-off", "Frame limit set to Off", "", false, kLiveNoticeSeconds, false,
                 "A cap of " + cap + " needs Refresh rate 120 Hz.", false});
}

// Player words for a preparation journal step; never the step's identifier.
std::string PreparationStepWords(const std::string& step) {
    if (step == "stage-title") return "Setting up the game";
    if (step == "copy-template") return "Copying the Windows environment";
    if (step == "validate-artifacts") return "Checking the game's files";
    if (step == "publish-profile" || step == "capture-settings" || step == "capture-update" ||
        step == "initialize-control" || step == "control-ready")
        return "Applying settings";
    if (step == "query-native" || step == "register" || step == "await-native") return "Waiting for the PS5";
    return step.empty() ? std::string{} : "Preparing";
}

int PreparationStepNumber(const std::string& step) {
    if (step == "stage-title") return 2;
    if (step == "copy-template") return 3;
    if (step == "validate-artifacts") return 4;
    if (step == "publish-profile" || step == "capture-settings" || step == "capture-update" ||
        step == "initialize-control" || step == "control-ready")
        return 5;
    if (step == "query-native" || step == "register" || step == "await-native") return 6;
    return 1;
}

std::string PreparationProgressWords(bool has_record, const std::string& step) {
    const auto words = has_record ? PreparationStepWords(step) : std::string("Reserving a PS5 title");
    if (words.empty() || words == "Preparing") return words;
    return "Step " + std::to_string(has_record ? PreparationStepNumber(step) : 1) + " of " +
           std::to_string(kPreparationSteps) + " · " + words;
}

} // namespace playden::homebrew
