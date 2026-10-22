#include "homebrew/simulation.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include "steam_id.h"

namespace {

using namespace playden;
using namespace playden::homebrew;

void Check(bool value, const char* message) {
    if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}

Simulation Live() {
    Simulation model("fixtures-not-used", "profile-not-used.json", ModelMode::Live);
    model.UpdateAccount({AuthState::SignedIn, "42", "Owner", {}, {}, false});
    return model;
}

GameSummary Game(GameId id, std::string title) {
    GameSummary game;
    game.id = id;
    game.title = std::move(title);
    return game;
}

void Activate(Simulation& model, const std::string& key) {
    Check(model.action("focus:" + key), "row to activate exists");
    Check(model.action("cross"), "row activates");
}

OrganizationSnapshot SavedOrganization() {
    OrganizationSnapshot saved;
    saved.favorites = {SteamId(20)};
    saved.hidden = {SteamId(30)};
    saved.collections = {{"collection-1", "Saved", true, {SteamId(20)}}};
    saved.ratings = {{SteamId(20), CompatibilityRating::kPlayable}};
    saved.notes = {{SteamId(20), "saved note"}};
    saved.query = "saved query";
    saved.scope = "collection-1";
    saved.sort = "playtime";
    saved.filters = {"compat-playable"};
    saved.selected_game_id = SteamId(20);
    return saved;
}

void RoundTripAndSelectionBeforeGames() {
    auto model = Live();
    const auto token = model.BeginOrganizationLoad(42);
    Check(token.id != 0 && token.baseline_revision == 0, "organization load token captures baseline");
    const auto saved = SavedOrganization();
    Check(model.ApplyOrganization(42, token, saved), "matching organization load applies");
    Check(model.ExportOrganization() == saved && !model.OrganizationDirty(),
          "saved organization projects and starts clean");
    Check(model.state().navigation.selected_game_id == SteamId(20),
          "saved selection is retained before the catalog arrives");
    Check(model.UpdateLibrary({{Game(SteamId(10), "Ten"), Game(SteamId(20), "Twenty"), Game(SteamId(30), "Thirty")},
                               false, {}, false, true, 1}),
          "catalog arrives after organization");
    Check(model.state().navigation.selected_game_id == SteamId(20),
          "catalog projection restores the saved Steam selection");

    const auto preferences_token = model.BeginPreferencesLoad();
    PreferencesLoadResult loaded;
    loaded.preferences.reduce_motion = true;
    Check(model.ApplyPreferences(preferences_token, loaded) && model.state().reduced_motion,
          "global preferences project into the live model");
    Check(model.ExportPreferences().reduce_motion && !model.PreferencesDirty(),
          "loaded preferences export cleanly");
}

void PendingEditsReplayAndCollectionCollisionKeepsLocalIdentity() {
    auto model = Live();
    model.UpdateLibrary({{Game(SteamId(10), "Ten"), Game(SteamId(20), "Twenty"), Game(SteamId(30), "Thirty")},
                         false, {}, false, true, 1});
    const auto token = model.BeginOrganizationLoad(42);
    Check(model.action("note:10:local note"), "pending local note accepted");
    Check(model.action("rate:10:works"), "pending local rating accepted");
    Check(model.action("collection:create:Local"), "pending local collection accepted");
    Check(model.action("collection:add:collection-1:10"), "pending local membership accepted");
    Check(model.action("select:10") && model.action("square"), "pending favorite accepted");
    Check(model.OrganizationRevision() > token.baseline_revision && !model.OrganizationReadyForSave(),
          "pending edits are dirty but not saved before load resolution");

    const auto saved = SavedOrganization();
    Check(model.ApplyOrganization(42, token, saved), "delayed saved organization merges");
    const auto merged = model.ExportOrganization();
    Check(merged.notes.at(SteamId(10)) == "local note" && merged.notes.at(SteamId(20)) == "saved note",
          "local note wins without losing unrelated saved note");
    Check(merged.favorites.count(SteamId(10)) && merged.favorites.count(SteamId(20)),
          "local and saved favorites merge semantically");
    const auto local = std::find_if(merged.collections.begin(), merged.collections.end(),
                                    [](const auto& value) { return value.name == "Local"; });
    const auto saved_collection = std::find_if(merged.collections.begin(), merged.collections.end(),
                                               [](const auto& value) { return value.name == "Saved"; });
    Check(local != merged.collections.end() && local->id == "collection-1" && local->members.count(SteamId(10)),
          "locally allocated collection identity survives a saved ID collision");
    Check(saved_collection != merged.collections.end() && saved_collection->id != "collection-1" &&
              merged.scope == saved_collection->id,
          "colliding saved collection and its scope are deterministically remapped");
    Check(model.OrganizationDirty() && model.OrganizationReadyForSave(),
          "merged pending edits become eligible for durable save");
}

void ResetSupersedesLoadedBase() {
    auto model = Live();
    model.UpdateLibrary({{Game(SteamId(10), "Ten")}, false, {}, false, true, 1});
    const auto token = model.BeginOrganizationLoad(42);
    Check(model.action("reset"), "pending reset accepted");
    Check(model.ApplyOrganization(42, token, SavedOrganization()), "saved base arrives after reset");
    const auto merged = model.ExportOrganization();
    Check(merged.favorites.empty() && merged.hidden.empty() && merged.collections.empty() &&
              merged.ratings.empty() && merged.notes.empty() && merged.query.empty() &&
              merged.scope == "all" && merged.sort == "name" && merged.filters.empty(),
          "explicit reset supersedes every stored organization field");
}

void FailedLoadRetainsEditsAndRetryDoesNotClaimSuccess() {
    auto model = Live();
    const auto token = model.BeginOrganizationLoad(42);
    Check(model.action("note:99:pending"), "edit before failed load accepted");
    const auto revision = model.OrganizationRevision();
    Check(!model.ApplyOrganization(41, token, SavedOrganization()), "wrong account load rejected");
    Check(!model.ApplyOrganization(42, {token.id + 1, token.baseline_revision}, SavedOrganization()),
          "stale token rejected");
    Check(model.FailOrganizationLoad(42, token, StorageError::kReadFailed),
          "matching failed load recorded");
    Check(model.ExportOrganization().notes.at(SteamId(99)) == "pending" &&
              model.OrganizationRevision() == revision && !model.OrganizationReadyForSave() &&
              !model.state().persistence_available,
          "failed load retains edits and exposes unavailable persistence");

    model.action("l1");
    Activate(model, "settings-notices");
    Activate(model, "retry-save");
    Check(model.TakePersistenceRetryRequest() && !model.state().persistence_available,
          "retry requests IO without prematurely claiming persistence success");
    Check(!model.TakePersistenceRetryRequest(), "retry request is edge-triggered");

    const auto retry = model.BeginOrganizationLoad(42);
    Check(model.ApplyOrganization(42, retry, SavedOrganization()), "retry load applies");
    Check(model.ExportOrganization().notes.at(SteamId(99)) == "pending" && model.OrganizationReadyForSave(),
          "retry replays the original edit over saved data");
    model.OrganizationSaveResult(42, model.OrganizationRevision(), StorageError::kWriteFailed);
    Check(model.OrganizationDirty() && !model.state().persistence_available,
          "save failure remains dirty and visible");
    model.OrganizationSaveResult(42, model.OrganizationRevision(), StorageError::kNone);
    Check(!model.OrganizationDirty() && model.state().persistence_available,
          "matching save acknowledgement alone marks organization durable");
}

void DelayedPreferencesCannotOverwriteLocalToggle() {
    auto model = Live();
    const auto token = model.BeginPreferencesLoad();
    model.action("l1");
    Activate(model, "settings-motion");
    Check(model.state().reduced_motion && model.PreferencesRevision() > token.baseline_revision,
          "local motion edit advances global preference revision");
    PreferencesLoadResult delayed;
    delayed.preferences.reduce_motion = false;
    Check(model.ApplyPreferences(token, delayed) && model.state().reduced_motion,
          "delayed preference load cannot overwrite local toggle");
    Check(model.PreferencesDirty(), "preserved local preference remains dirty");
    model.PreferencesSaveResult(model.PreferencesRevision(), StorageError::kNone);
    Check(!model.PreferencesDirty(), "matching preference save acknowledgement clears dirty state");
}

void FailedPreferenceRetryCannotOverwriteDirtyValue() {
    auto model = Live();
    const auto initial = model.BeginPreferencesLoad();
    PreferencesLoadResult failed;
    failed.status = PreferencesLoadStatus::kDefaultReadError;
    Check(model.ApplyPreferences(initial, failed), "initial preference failure is recorded");
    model.action("l1");
    Activate(model, "settings-motion");
    Check(model.state().reduced_motion && model.PreferencesDirty(),
          "preference edited after failed load remains dirty");
    const auto retry = model.BeginPreferencesLoad();
    PreferencesLoadResult stale_disk;
    stale_disk.preferences.reduce_motion = false;
    Check(model.ApplyPreferences(retry, stale_disk), "retry preference result is consumed");
    Check(model.state().reduced_motion && model.PreferencesDirty(),
          "retry cannot overwrite a value dirty before BeginPreferencesLoad");
}

void ResetBeforePreferenceLoadWinsEvenAtDefaultValue() {
    auto model = Live();
    const auto token = model.BeginPreferencesLoad();
    Check(model.action("reset"), "explicit reset is accepted while preferences load");
    PreferencesLoadResult stored;
    stored.preferences.reduce_motion = true;
    Check(model.ApplyPreferences(token, stored), "stored preference result is consumed");
    Check(!model.state().reduced_motion && model.PreferencesDirty(),
          "explicit false reset supersedes delayed stored true preference");
}

void RetryLoadPreservesEditsMadeAfterEarlierSuccessfulLoad() {
    auto model = Live();
    const auto initial = model.BeginOrganizationLoad(42);
    Check(model.ApplyOrganization(42, initial, SavedOrganization()), "retry baseline loads");
    Check(model.action("note:20:locally edited"), "post-load local edit is accepted");
    model.OrganizationSaveResult(42, model.OrganizationRevision(), StorageError::kWriteFailed);
    const auto retry = model.BeginOrganizationLoad(42);
    auto changed_disk = SavedOrganization();
    changed_disk.favorites.insert(SteamId(99));
    Check(model.ApplyOrganization(42, retry, changed_disk), "same-account retry load applies");
    const auto merged = model.ExportOrganization();
    Check(merged.notes.at(SteamId(20)) == "locally edited" && merged.favorites.contains(SteamId(99)),
          "retry merges post-load local edits over newly loaded unrelated fields");
}

void AcceptedClearSuppressesAutosaveUntilAnotherUserEdit() {
    auto model = Live();
    const auto token = model.BeginOrganizationLoad(42);
    Check(model.ApplyOrganization(42, token, SavedOrganization()), "clear baseline loads");
    model.OrganizationCleared(42);
    Check(!model.OrganizationDirty() && !model.OrganizationReadyForSave(),
          "accepted clear does not immediately recreate visible account data");
    Check(model.action("note:20:after clear"), "new edit after clear accepted");
    Check(model.OrganizationDirty() && model.OrganizationReadyForSave(),
          "new user edit after clear re-enables save");
}

void NavigationSelectionIsPersisted() {
    auto model = Live();
    const auto token = model.BeginOrganizationLoad(42);
    Check(model.ApplyOrganization(42, token, OrganizationSnapshot{}), "empty organization loads");
    Check(model.UpdateLibrary({{Game(SteamId(10), "Ten"), Game(SteamId(20), "Twenty")},
                               false, {}, false, true, 1}), "navigation catalog loads");
    Check(model.action("r1"), "library route is selected");
    const auto first = model.state().navigation.selected_game_id;
    Check(model.action("right"), "live directional navigation is accepted");
    const auto second = model.state().navigation.selected_game_id;
    Check(first.valid() && second.valid() && second != first && model.OrganizationDirty(),
          "directional selection advances the organization revision");
    Check(model.ExportOrganization().selected_game_id == second,
          "export retains the last user-selected Steam ID");
}

void OversizedAndSaturatedEditsAreRolledBackVisibly() {
    auto model = Live();
    const auto token = model.BeginOrganizationLoad(42);
    Check(model.action("note:10:" + std::string(4097, 'x')), "oversized note action is handled");
    Check(!model.ExportOrganization().notes.contains(SteamId(10)), "oversized note is rejected without truncation");
    Check(!model.state().navigation.modals.empty() &&
              model.state().navigation.modals.back().title == "This change is too large to save",
          "oversized edit has a visible rejection");
    Check(model.action("focus:alert-ok") && model.action("cross") && model.state().navigation.modals.empty(),
          "the rejection is dismissed with OK");

    bool saturated = false;
    std::string last_accepted;
    for (int index = 0; index < 400 && !saturated; ++index) {
        const auto candidate = std::to_string(index) + std::string(900, 'a' + index % 20);
        model.action("note:10:" + candidate);
        const auto snapshot = model.ExportOrganization();
        const auto actual = snapshot.notes.find(SteamId(10));
        if (actual != snapshot.notes.end() && actual->second == candidate)
            last_accepted = candidate;
        saturated = std::any_of(model.state().notices.begin(), model.state().notices.end(),
                                [](const Notice& value) { return value.id == "persistence"; });
    }
    Check(saturated && model.ExportOrganization().notes.at(SteamId(10)) == last_accepted,
          "journal saturation rolls back the untracked edit and reports the failure");
    Check(model.ApplyOrganization(42, token, SavedOrganization()),
          "bounded accepted journal can still merge after saturation");
}

void RealModalActionsAdvancePersistence() {
    {
        auto model = Live();
        auto saved = SavedOrganization();
        const auto token = model.BeginOrganizationLoad(42);
        Check(model.ApplyOrganization(42, token, saved), "clear-search baseline loads");
        model.action("r1");
        Activate(model, "clear-search");
        Check(model.ExportOrganization().query.empty() && model.OrganizationDirty(),
              "clear-search row records its organization mutation");
    }
    {
        auto model = Live();
        const auto token = model.BeginOrganizationLoad(42);
        Check(model.ApplyOrganization(42, token, OrganizationSnapshot{}), "hide baseline loads");
        model.UpdateLibrary({{Game(SteamId(10), "Ten")}, false, {}, false, true, 1});
        model.action("r1");
        model.action("focus:game-10");
        model.action("options");
        Activate(model, "hide");
        Check(model.ExportOrganization().hidden.contains(SteamId(10)) && model.OrganizationDirty(),
              "context hide records its organization mutation");
    }
    {
        auto model = Live();
        const auto token = model.BeginOrganizationLoad(42);
        Check(model.ApplyOrganization(42, token, SavedOrganization()), "reset baseline loads");
        model.action("l1");
        Activate(model, "settings-reset");
        Activate(model, "confirm-destructive");
        const auto reset = model.ExportOrganization();
        Check(reset.collections.empty() && reset.notes.empty() && model.OrganizationDirty(),
              "confirmed settings reset records a reset journal entry");
    }
    {
        auto model = Live();
        auto saved = SavedOrganization();
        saved.query.clear();
        const auto token = model.BeginOrganizationLoad(42);
        Check(model.ApplyOrganization(42, token, saved), "collection-delete baseline loads");
        model.UpdateLibrary({{Game(SteamId(20), "Twenty")}, false, {}, false, true, 1});
        model.action("options");
        Activate(model, "collection");
        Activate(model, "collection-manage");
        Activate(model, "collection-delete-collection-1");
        Activate(model, "confirm-destructive");
        const auto removed = model.ExportOrganization();
        Check(removed.collections.empty() && removed.scope == "all" && model.OrganizationDirty(),
              "deleting the active collection repairs scope and records the mutation");
    }
}

void MergeThatExceedsStoreBoundsKeepsLocalIntentForRetry() {
    auto model = Live();
    const auto token = model.BeginOrganizationLoad(42);
    Check(model.action("collection:create:Local"), "pending local collection is accepted");
    OrganizationSnapshot full;
    for (int index = 1; index <= 256; ++index) {
        full.collections.push_back({"collection-" + std::to_string(index),
                                    "Saved " + std::to_string(index), false, {}});
    }
    Check(!model.ApplyOrganization(42, token, full), "over-bound merge result is rejected");
    const auto retained = model.ExportOrganization();
    Check(retained.collections.size() == 1 && retained.collections.front().name == "Local" &&
              !model.state().persistence_available && !model.OrganizationReadyForSave(),
          "invalid 257-collection merge keeps local intent and exposes retry state");
}

void InitialMissingOrganizationPreservesActiveDetailsSelection() {
    auto model = Live();
    const auto token = model.BeginOrganizationLoad(42);
    Check(model.UpdateLibrary({{Game(SteamId(43), "Forty Three")}, false, {}, false, true, 1}),
          "library can arrive before organization");
    Check(model.action("cross") && model.state().navigation.route == Route::Details,
          "user enters details while organization load is pending");
    Check(model.ApplyOrganization(42, token, std::nullopt), "missing organization applies");
    Check(model.state().navigation.route == Route::Details &&
              model.state().navigation.selected_game_id == SteamId(43) &&
              model.state().navigation.detail_id == SteamId(43),
          "missing stored selection does not invalidate active details");
    Check(model.action("square") && model.ExportOrganization().favorites.contains(SteamId(43)),
          "favorite remains available after delayed missing organization");

    auto stored_selection = Live();
    const auto stored_token = stored_selection.BeginOrganizationLoad(42);
    stored_selection.UpdateLibrary({{Game(SteamId(43), "Forty Three"), Game(SteamId(99), "Ninety Nine")},
                                    false, {}, false, true, 1});
    Check(stored_selection.action("cross"), "explicit details navigation is recorded");
    OrganizationSnapshot saved;
    saved.selected_game_id = SteamId(99);
    Check(stored_selection.ApplyOrganization(42, stored_token, saved),
          "stored nonzero selection merges");
    Check(stored_selection.state().navigation.selected_game_id == SteamId(43) &&
              stored_selection.state().navigation.detail_id == SteamId(43),
          "explicit navigation wins over delayed stored nonzero selection");
}

void SelectionDoesNotConsumeAnnotationIdBudget() {
    auto model = Live();
    const auto token = model.BeginOrganizationLoad(42);
    Check(model.UpdateLibrary({{Game(SteamId(20000), "Selected")}, false, {}, false, true, 1}),
          "selection boundary catalog loads");
    OrganizationSnapshot full;
    for (std::uint32_t id = 1; id <= 10000; ++id) full.favorites.insert(SteamId(id));
    Check(model.ApplyOrganization(42, token, full),
          "maximum annotated IDs load with an independent projected selection");
    const auto restored = model.ExportOrganization();
    Check(restored.favorites.size() == 10000 && restored.favorites.contains(SteamId(1)) &&
              restored.favorites.contains(SteamId(10000)) && restored.selected_game_id == SteamId(20000),
          "all annotations and the independent selected Steam ID are preserved");
}

void DelayedStoredSelectionReconcilesVisibleNavigation() {
    {
        auto model = Live();
        const auto token = model.BeginOrganizationLoad(42);
        auto first = Game(SteamId(43), "Forty Three");
        auto second = Game(SteamId(99), "Ninety Nine");
        first.playtime_minutes = 1;
        second.playtime_minutes = 2;
        Check(model.UpdateLibrary({{std::move(first), std::move(second)},
                                   false, {}, false, true, 1}),
              "home selection catalog arrives before organization");
        OrganizationSnapshot saved;
        saved.selected_game_id = SteamId(99);
        Check(model.ApplyOrganization(42, token, saved), "stored Home selection applies");
        Check(model.state().navigation.selected_game_id == SteamId(99) &&
                  model.state().navigation.focus_key == "game-99",
              "stored Home selection reconciles visible focus");
        Check(model.action("cross") && model.state().navigation.route == Route::Details &&
                  model.state().navigation.detail_id == SteamId(99),
              "Home activation opens the visibly selected stored game");
    }
    {
        auto model = Live();
        const auto token = model.BeginOrganizationLoad(42);
        auto first = Game(SteamId(43), "Forty Three");
        auto second = Game(SteamId(99), "Ninety Nine");
        first.playtime_minutes = 1;
        second.playtime_minutes = 2;
        model.UpdateLibrary({{std::move(first), std::move(second)},
                             false, {}, false, true, 1});
        Check(model.action("r1"), "Library route opens before organization completes");
        OrganizationSnapshot saved;
        saved.selected_game_id = SteamId(99);
        Check(model.ApplyOrganization(42, token, saved), "stored Library selection applies");
        Check(model.state().navigation.selected_game_id == SteamId(99) &&
                  model.state().navigation.selected_index == 1 &&
                  model.state().navigation.focus_key == "game-99",
              "stored Library selection reconciles row index and focus");
        Check(model.action("cross") && model.state().navigation.detail_id == SteamId(99),
              "Library activation opens the visibly selected stored game");
    }
}

void UnrelatedPendingEditDoesNotSuppressProjectedSelectionFallback() {
    auto model = Live();
    const auto token = model.BeginOrganizationLoad(42);
    auto game = Game(SteamId(42), "Forty Two");
    game.playtime_minutes = 1;
    Check(model.UpdateLibrary({{std::move(game)}, false, {}, false, true, 1}),
          "favorite-race catalog arrives before organization");
    Check(model.action("square"), "pending favorite is accepted");
    Check(model.ApplyOrganization(42, token, std::nullopt),
          "missing organization merges unrelated pending favorite");
    Check(model.ExportOrganization().favorites.contains(SteamId(42)) &&
              model.state().navigation.selected_game_id == SteamId(42) &&
              model.state().navigation.focus_key == "game-42",
          "unrelated journal entry preserves projected selection and visible focus");
    Check(model.action("cross") && model.state().navigation.detail_id == SteamId(42),
          "activation opens projected game after unrelated merge");
}

void UnavailableStoredSelectionFallsBackToVisibleState() {
    {
        auto model = Live();
        const auto token = model.BeginOrganizationLoad(42);
        auto game = Game(SteamId(43), "Forty Three");
        game.playtime_minutes = 1;
        model.UpdateLibrary({{std::move(game)}, false, {}, false, true, 1});
        OrganizationSnapshot absent;
        absent.selected_game_id = SteamId(99);
        Check(model.ApplyOrganization(42, token, absent), "absent stored selection applies safely");
        Check(model.state().navigation.selected_game_id == SteamId(43) &&
                  model.state().navigation.focus_key == "game-43" &&
                  model.ExportOrganization().selected_game_id == SteamId(43),
              "absent stored ID falls back to the visible neighbor and captured snapshot");
        Check(model.action("cross") && model.state().navigation.detail_id == SteamId(43),
              "Home opens the visible fallback game");
    }
    {
        auto model = Live();
        const auto token = model.BeginOrganizationLoad(42);
        auto first = Game(SteamId(43), "Forty Three");
        auto second = Game(SteamId(99), "Ninety Nine");
        first.playtime_minutes = 1;
        second.playtime_minutes = 2;
        model.UpdateLibrary({{std::move(first), std::move(second)}, false, {}, false, true, 1});
        OrganizationSnapshot hidden;
        hidden.hidden.insert(SteamId(99));
        hidden.selected_game_id = SteamId(99);
        Check(model.ApplyOrganization(42, token, hidden), "hidden stored selection applies safely");
        Check(model.state().navigation.selected_game_id == SteamId(43) &&
                  model.state().navigation.focus_key == "game-43",
              "hidden stored ID falls back to a visible Home game");
    }
    {
        auto model = Live();
        const auto token = model.BeginOrganizationLoad(42);
        model.UpdateLibrary({{Game(SteamId(43), "Forty Three"), Game(SteamId(99), "Ninety Nine")},
                             false, {}, false, true, 1});
        model.action("r1");
        OrganizationSnapshot no_results;
        no_results.query = "no match";
        no_results.selected_game_id = SteamId(99);
        Check(model.ApplyOrganization(42, token, no_results), "no-result selection applies safely");
        Check(!model.state().navigation.selected_game_id.valid() &&
                  model.state().navigation.focus_key == "clear-search",
              "no visible Library result clears selection and focuses recovery action");
    }
}

void PendingGameEditorPinsItsTargetAcrossDelayedSelection() {
    auto model = Live();
    const auto token = model.BeginOrganizationLoad(42);
    model.UpdateLibrary({{Game(SteamId(43), "Forty Three"), Game(SteamId(99), "Ninety Nine")},
                         false, {}, false, true, 1});
    Check(model.action("options"), "game context opens while organization is pending");
    Check(model.state().navigation.selected_game_id == SteamId(43), "game context pins current game");
    Check(model.OrganizationRevision() > token.baseline_revision,
          "opening a game context records selection intent");
    Check(model.action("focus:note"), "note context row exists");
    Check(model.state().navigation.selected_game_id == SteamId(43), "focusing note keeps game target");
    Check(model.action("cross"), "note context row activates");
    Check(model.state().editor.active, "the Note item opens the note editor directly");
    Check(model.state().navigation.selected_game_id == SteamId(43), "note editor starts on game target");
    model.text("local note");
    OrganizationSnapshot saved;
    saved.selected_game_id = SteamId(99);
    Check(model.ApplyOrganization(42, token, saved), "delayed stored selection merges during editor");
    Check(model.state().editor.active && model.state().navigation.selected_game_id == SteamId(43),
          "active game editor keeps its explicit local target");
    Check(model.action("done"), "note editor commits");
    const auto result = model.ExportOrganization();
    Check(result.notes.contains(SteamId(43)) && result.notes.at(SteamId(43)) == "local note" &&
              !result.notes.contains(SteamId(99)),
          "editor commit writes only the originally targeted game");
}

void OffRouteSelectionRefreshesHomeCoordinates() {
    auto model = Live();
    const auto token = model.BeginOrganizationLoad(42);
    auto first = Game(SteamId(43), "Forty Three");
    auto second = Game(SteamId(99), "Ninety Nine");
    first.playtime_minutes = 1;
    second.playtime_minutes = 2;
    model.UpdateLibrary({{std::move(first), std::move(second)}, false, {}, false, true, 1});
    model.action("l1");
    Check(model.state().navigation.route == Route::Settings, "Settings is active during load completion");
    OrganizationSnapshot saved;
    saved.selected_game_id = SteamId(99);
    Check(model.ApplyOrganization(42, token, saved), "off-route stored selection applies");
    Check(model.state().navigation.home_shelf_focus.at("continue") == 1,
          "off-route apply refreshes cached Home shelf position");
    Check(model.action("r1") && model.state().navigation.route == Route::Home,
          "navigation returns to Home");
    Check(model.action("cross") && model.state().navigation.detail_id == SteamId(99),
          "Home activation matches the cached visible selection");
}

void KnownEmptyCatalogClearsStoredSelection() {
    auto model = Live();
    const auto token = model.BeginOrganizationLoad(42);
    Check(model.UpdateLibrary({{}, false, {}, false, true, 1}),
          "genuine empty catalog delivery is accepted");
    OrganizationSnapshot saved;
    saved.selected_game_id = SteamId(99);
    Check(model.ApplyOrganization(42, token, saved), "empty-catalog organization applies");
    Check(!model.state().navigation.selected_game_id.valid() &&
              model.state().navigation.focus_key == "home-library",
          "known empty catalog clears unavailable stored selection");
}

void HomeSelectionUsesActualShelfProjection() {
    {
        auto model = Live();
        const auto token = model.BeginOrganizationLoad(42);
        auto played = Game(SteamId(43), "Played");
        auto unplayed = Game(SteamId(99), "Unplayed");
        played.playtime_minutes = 1;
        unplayed.playtime_minutes = 0;
        model.UpdateLibrary({{std::move(played), std::move(unplayed)}, false, {}, false, true, 1});
        OrganizationSnapshot saved;
        saved.selected_game_id = SteamId(99);
        Check(model.ApplyOrganization(42, token, saved), "mixed-shelf organization applies");
        Check(model.state().navigation.selected_game_id == SteamId(43) &&
                  model.state().navigation.focus_key == "game-43" &&
                  model.state().navigation.home_shelf_focus.at("continue") == 0,
              "unplayed stored ID falls back to the actual visible shelf game");
        Check(model.action("cross") && model.state().navigation.detail_id == SteamId(43),
              "mixed shelf activation opens the visible played game");
    }
    {
        auto model = Live();
        const auto token = model.BeginOrganizationLoad(42);
        auto played = Game(SteamId(43), "Played");
        played.playtime_minutes = 1;
        model.UpdateLibrary({{std::move(played)}, false, {}, false, true, 1});
        OrganizationSnapshot saved;
        saved.query = "no library row matches";
        saved.selected_game_id = SteamId(99);
        Check(model.ApplyOrganization(42, token, saved), "Home query mismatch organization applies");
        Check(model.state().navigation.selected_game_id == SteamId(43) &&
                  model.state().navigation.focus_key == "game-43",
              "Home shelf remains selectable independently of Library query results");
        Check(model.action("cross") && model.state().navigation.detail_id == SteamId(43),
              "query-mismatched Home activation opens the visible shelf game");
    }
}

}  // namespace

int main() {
    RoundTripAndSelectionBeforeGames();
    PendingEditsReplayAndCollectionCollisionKeepsLocalIdentity();
    ResetSupersedesLoadedBase();
    FailedLoadRetainsEditsAndRetryDoesNotClaimSuccess();
    DelayedPreferencesCannotOverwriteLocalToggle();
    FailedPreferenceRetryCannotOverwriteDirtyValue();
    ResetBeforePreferenceLoadWinsEvenAtDefaultValue();
    RetryLoadPreservesEditsMadeAfterEarlierSuccessfulLoad();
    AcceptedClearSuppressesAutosaveUntilAnotherUserEdit();
    NavigationSelectionIsPersisted();
    OversizedAndSaturatedEditsAreRolledBackVisibly();
    RealModalActionsAdvancePersistence();
    MergeThatExceedsStoreBoundsKeepsLocalIntentForRetry();
    InitialMissingOrganizationPreservesActiveDetailsSelection();
    SelectionDoesNotConsumeAnnotationIdBudget();
    DelayedStoredSelectionReconcilesVisibleNavigation();
    UnrelatedPendingEditDoesNotSuppressProjectedSelectionFallback();
    UnavailableStoredSelectionFallsBackToVisibleState();
    PendingGameEditorPinsItsTargetAcrossDelayedSelection();
    OffRouteSelectionRefreshesHomeCoordinates();
    KnownEmptyCatalogClearsStoredSelection();
    HomeSelectionUsesActualShelfProjection();
    std::cout << "homebrew_persistence_model_test passed\n";
    return 0;
}
