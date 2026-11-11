// homebrew_sharing_test.cpp - Sharing recipes and votes, live: the Share sheet hands the whole request
// to the sharing worker (no sharing ID in the model), the worker sends it with the same
// Idempotency-Key on every retry and the sharing ID only in Authorization, the answer is saved per
// game, a second share updates it, duplicates and errors read plainly, Game settings refreshes the
// shared recipe's state, and the vote prompt asks once per recipe revision after five minutes of play.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "community/install_secret.h"
#include "community/recipe_release.h"
#include "community/recipe_store.h"
#include "homebrew/controller.hpp"
#include "homebrew/sharing_service.hpp"
#include "homebrew/simulation.hpp"
#include "runtime/game_settings.h"

#include <strings.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace {

using playden::GameId;
using playden::homebrew::AuthState;
using playden::homebrew::Compatibility;
using playden::homebrew::ModelMode;
using playden::homebrew::RuntimeState;
using playden::homebrew::SharingRequest;
using playden::homebrew::SharingRequestKind;
using playden::homebrew::SharingResult;
using playden::homebrew::Simulation;
namespace cm = playden::community;
namespace rt = playden::runtime;

void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

const GameId kGame = GameId::Steam(274520);
const playden::GameRef kGameRef = playden::GameRef::Steam(274520);
const std::string kRecipeId = "community-steam-274520-darkwood-120";
const std::string kRevision(64, 'c');
const std::string kNewerRevision(64, 'd');

std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

// Game settings has two pages: the summary, and over it the list that Change settings… opens. Tests name rows on
// either page; this goes to the page that has the row, as a player would, and stays put when neither has it.
bool settings_page_with(playden::homebrew::Simulation& simulation, const std::string& key) {
    const auto shown = [&] {
        for (const auto& row : simulation.view().rows)
            if (row.key == key) return true;
        return false;
    };
    if (shown()) return true;
    const auto& modals = simulation.state().navigation.modals;
    if (modals.empty()) return false;
    // A row on neither page leaves the page and its focus as they were.
    const std::string start_id = modals.back().id;
    const std::string start_focus = simulation.state().navigation.focus_key;
    const auto restore = [&] {
        if (!simulation.state().navigation.modals.empty() && simulation.state().navigation.modals.back().id == start_id)
            simulation.action("focus:" + start_focus);
        return false;
    };
    if (modals.back().id == "game-settings-list") {
        simulation.action("circle");
        if (shown()) return true;
        simulation.action("focus:change-settings");
        simulation.action("cross");
    } else if (modals.back().id == "game-settings" && simulation.action("focus:change-settings") && simulation.action("cross")) {
        if (shown()) return true;
        simulation.action("circle");
    } else {
        return false;
    }
    return shown() || restore();
}

const playden::homebrew::ViewRow* find_row(Simulation& model, const std::string& key) {
    settings_page_with(model, key);
    for (const auto& row : model.view().rows)
        if (row.key == key) return &row;
    return nullptr;
}

void activate(Simulation& model, const std::string& key) {
    settings_page_with(model, key);
    if (!model.action("focus:" + key)) {
        std::fprintf(stderr, "missing row %s on %s\n", key.c_str(), model.view().surface.c_str());
        check(false, "requested row exists");
    }
    check(model.action("cross"), "requested row activates");
}

std::string header_of(const playden::steam::HttpRequest& request, const char* name) {
    for (const auto& [key, value] : request.headers)
        if (strcasecmp(key.c_str(), name) == 0) return value;
    return {};
}

playden::steam::HttpResponse answer(long status, const std::string& body,
                                    std::vector<std::pair<std::string, std::string>> headers = {}) {
    playden::steam::HttpResponse response;
    response.status_code = status;
    response.body.assign(body.begin(), body.end());
    response.headers = std::move(headers);
    return response;
}

playden::steam::HttpResponse lost() {
    // Connected, then the answer never came: the worker tries again with the same key.
    playden::steam::HttpResponse response;
    response.error = playden::steam::HttpError::kTimeout;
    response.timing.connected = true;
    return response;
}

playden::steam::HttpResponse unreachable() {
    // Nothing answered the connection: no retry, the player gets Try again at once.
    playden::steam::HttpResponse response;
    response.error = playden::steam::HttpError::kTimeout;
    return response;
}

// The API as the worker sees it: every request kept, answers given in order (the last repeats).
struct FakeApi {
    std::mutex mutex;
    std::vector<playden::steam::HttpRequest> requests;
    std::vector<playden::steam::HttpResponse> answers;
    void answer_with(std::vector<playden::steam::HttpResponse> next) {
        std::lock_guard<std::mutex> lock(mutex);
        answers = std::move(next);
        requests.clear();
    }
    playden::update::Fetch fetch() {
        return [this](const playden::steam::HttpRequest& request, const std::atomic<bool>&) {
            std::lock_guard<std::mutex> lock(mutex);
            requests.push_back(request);
            if (answers.empty()) return playden::steam::HttpResponse{};
            const auto next = answers.front();
            if (answers.size() > 1) answers.erase(answers.begin());
            return next;
        };
    }
};

rt::CommunityRecipe community_recipe(const std::string& revision) {
    rt::CommunityRecipe community;
    auto& recipe = community.recipe;
    recipe.id = kRecipeId;
    recipe.version = 1;
    community.game = kGameRef;
    community.game_name = "Darkwood";
    recipe.name = "Darkwood";
    recipe.launch_entries = {{"game", "Darkwood", "Darkwood.exe", ""}};
    recipe.arguments = "{resolution}";
    recipe.resolution_arguments = "-width {w} -height {h}";
    recipe.architecture = "pe64";
    recipe.graphics = "dxvk";
    recipe.defaults = {{"resolution", "3840x2160"}};
    recipe.revision = revision;
    community.executable = "Darkwood.exe";
    community.title = "Darkwood at 4K";
    community.status = "works";
    community.author = "rina";
    community.tested_kit = "0.12";
    community.tested_firmware = "10.01";
    community.tested_date = "2026-10-20";
    return community;
}

playden::homebrew::CommunityRecipeOffer offer(const std::string& revision) {
    playden::homebrew::CommunityRecipeOffer value;
    value.id = kRecipeId;
    value.title = "Darkwood at 4K";
    value.status = "works";
    value.author = "rina";
    value.tested_kit = "0.12";
    value.revision = revision;
    return value;
}

// A live model with Darkwood installed and prepared, its title stopped, a kit and (with_catalog) one community recipe.
void set_up(Simulation& model, const std::filesystem::path& base, bool with_catalog = true) {
    model.SetGameSettingsFile(base / "game-settings.json");
    model.SetSettingsData({{"playden-default", "Playden default", "Most games", "", {}}}, {});
    model.SetKitCapabilities(rt::KitCapabilitiesFromManifest({"dxvk", "display-120hz"}, true));
    rt::VerifiedKitView kit;
    kit.kit_id = "kit-test";
    kit.runtime.artifact_revision = "f729534-kit-0.12";
    kit.template_audio_ps5 = true;
    model.SetKitView(kit);
    playden::homebrew::LiveKitInfo info;
    info.version = "0.12";
    model.SetKitInfo(info);
    model.SetSharingContext("10.01", "0.6.0");
    model.SetWallClock(1'000'000);
    model.UpdateAccount({AuthState::SignedIn, "account", "Owner", {}, {}, false});
    playden::GameSummary game;
    game.id = kGame;
    game.title = "Darkwood";
    check(model.UpdateLibrary({{game}, false, {}, false, true, 100}), "library delivered");
    model.UpdateInstalls({}, {{kGame, 1024, "internal", true}});
    playden::homebrew::LiveRuntimeGame runtime;
    runtime.game_id = kGame;
    runtime.status = "Ready";
    runtime.prepared = runtime.has_title = runtime.ready = true;
    runtime.launch_entry_id = "game";
    runtime.activity = playden::homebrew::TitleActivity::Stopped;
    model.UpdateRuntime(RuntimeState::Ready, {runtime});
    if (!with_catalog) return;
    playden::homebrew::CommunityCatalog catalog;
    catalog.release_tag = "r20261022.1";
    catalog.games[kGameRef] = {offer(kRevision)};
    model.UpdateCommunityCatalog(catalog);
    model.SetCommunityRecipeLoader([](const std::string& revision) -> std::optional<rt::CommunityRecipe> {
        if (revision == kRevision || revision == kNewerRevision) return community_recipe(revision);
        return std::nullopt;
    });
}

void open_game_settings(Simulation& model) {
    while (!model.state().navigation.modals.empty()) model.action("circle");
    check(model.action("details:274520"), "Darkwood's page opens");
    activate(model, "details-more");
    activate(model, "game-settings");
}

void open_share(Simulation& model) {
    open_game_settings(model);
    activate(model, "section-advanced");
    activate(model, "share-recipe");
    check(model.view().surface == "share-recipe", "the Share sheet opens");
}

// Runs the model's requests through the worker and hands the answers back, like Controller::TickSharing.
void pump(Simulation& model, playden::homebrew::SharingService& service, std::vector<SharingRequest>* sent = nullptr) {
    auto requests = model.TakeSharingRequests();
    if (sent) *sent = requests;
    for (auto& request : requests) service.Submit(std::move(request));
    service.Wait();
    for (const auto& result : service.TakeResults()) model.ApplySharingResult(result);
    if (auto memory = service.Memory()) model.UpdateVoteMemory(*memory);
}

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

void share_and_vote(const std::filesystem::path& base) {
    // Private, as Playden's user data is; the worker makes its own folder in it (0700).
    std::filesystem::create_directories(base);
    std::filesystem::permissions(base, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace);
    FakeApi api;
    std::vector<std::string> log;
    std::mutex log_mutex;
    playden::homebrew::SharingServiceConfig config;
    config.store_root = (base / "community-recipes").string();
    config.fetch = api.fetch();
    config.log = [&](const std::string& line) {
        std::lock_guard<std::mutex> lock(log_mutex);
        log.push_back(line);
    };
    std::vector<std::uint32_t> waits;
    config.sleep = [&waits](std::uint32_t milliseconds, const std::atomic<bool>&) { waits.push_back(milliseconds); };
    playden::homebrew::SharingService service(config);
    service.Start();
    service.Wait();
    check(service.Memory().has_value(), "the worker loads the vote memory first");

    Simulation model("fixtures-not-used", "profile-not-used.json", ModelMode::Live);
    set_up(model, base);
    pump(model, service);

    // A new recipe: Playden's own settings with the player's changes.
    open_game_settings(model);
    activate(model, "setting-resolution");
    std::string resolution_choice;
    for (const auto& row : model.view().rows)
        if (row.title == "3840×2160" || row.title.find("3840") != std::string::npos) resolution_choice = row.key;
    check(!resolution_choice.empty(), "a 4K resolution can be chosen");
    activate(model, resolution_choice);
    check(model.ShareDecisionFor(kGame).kind == cm::ShareKind::kNewRecipe, "no community recipe, no share: a new recipe");
    open_share(model);
    check(!find_row(model, "share-choice-update") && find_row(model, "share-title") && find_row(model, "share-what"),
          "a new recipe asks nothing: title, name, rating, what is sent");
    check(contains(find_row(model, "share-what")->subtitle, "Darkwood.exe") ||
              contains(find_row(model, "share-what")->subtitle, "Game.exe"),
          "what is sent names the program");
    const auto what_is_sent = find_row(model, "share-what")->subtitle;
    activate(model, "share-title");
    check(model.state().editor.active && model.state().editor.purpose == "Recipe title", "the title opens the keyboard");
    // Text the API would refuse stays in the editor: a zero-width space inside.
    model.SubmitEditorText("A\u200Bb");
    check(model.state().editor.active, "a title with a zero-width space is refused");
    model.SubmitEditorText("Darkwood 4K");
    check(!model.state().editor.active && find_row(model, "share-title")->value == "Darkwood 4K", "the system keyboard sets the title");
    // Shared as: a picker of no name, a typed name or the Playden account; its choice is the default afterwards.
    check(find_row(model, "share-as") && find_row(model, "share-as")->value == "No name", "a share credits nobody by default");
    activate(model, "share-as");
    check(model.view().surface == "share-as" && find_row(model, "share-as-none")->selected &&
              contains(model.view().subtitle, "Settings, under Community"),
          "the picker opens on No name and says where the default lives");
    activate(model, "share-as-name");
    model.SubmitEditorText("\u3000Todor\u3000");  // ideographic spaces survive the trim, the API refuses them
    check(model.state().editor.active && model.state().share_name.empty(), "a name padded with U+3000 is refused");
    model.SubmitEditorText("Todor");
    check(model.state().share_name == "Todor" && model.ExportPreferences().share_name == "Todor" &&
              model.ExportPreferences().share_as == playden::ShareAs::kName && find_row(model, "share-as-name")->selected,
          "typing a name chooses it, and both are remembered");
    model.action("circle");
    check(model.view().surface == "share-recipe" && find_row(model, "share-as")->value == "Todor", "the Share sheet shows the name");
    // The first share says what is sent before anything is.
    activate(model, "share-send");
    check(model.state().navigation.modals.back().title == "Before you share" && model.TakeSharingRequests().empty(),
          "the first share explains what is sent and sends nothing yet");
    check(contains(model.state().navigation.modals.back().body, "No Steam ID or account name"), "the notice says what isn't sent");
    activate(model, "share-send-confirm");
    check(model.state().sharing_explained && model.ExportPreferences().sharing_explained, "the explanation shows once");

    // The worker gets the whole request; the model has no sharing ID, the worker makes the key.
    api.answer_with({lost(), answer(503, R"({"error":"try_again","message":"again"})"),
                     answer(201, R"({"id":"sub-1","recipe_id":"community-steam-274520-darkwood-4k","state":"in-review",)"
                                 R"("pr_url":"https://github.com/leolobato/playden-ps5-recipes/pull/12"})")});
    std::vector<SharingRequest> sent;
    pump(model, service, &sent);
    check(sent.size() == 1 && sent[0].kind == SharingRequestKind::Submit, "Send hands one share to the worker");
    const auto& request = sent[0].submission;
    check(request.game == kGameRef && request.game_name == "Darkwood" && request.title == "Darkwood 4K" && request.name == "Todor" && request.kit == "0.12" &&
              request.firmware == "10.01" && request.playden_version == "0.6.0" && !request.based_on &&
              request.choice == cm::ShareChoice::kNone && request.submission_id.empty() && request.idempotency_key.empty(),
          "the request carries the recipe, title, name and versions; no base, no choice, no key yet");
    // The PR description gets what the sheet showed: what is sent (program, resolution); no base, no "Your changes".
    check(request.summary.size() == 1 && request.summary[0] == "What is sent: " + what_is_sent &&
              contains(request.summary[0], "3840"),
          "the request carries the sheet's What is sent line");
    auto keyed = request;
    keyed.idempotency_key = "checked-in-test";
    check(cm::SubmissionRequestProblem(keyed).empty(), "the request keeps the API's contract");
    {
        std::lock_guard<std::mutex> lock(api.mutex);
        check(api.requests.size() == 3, "a lost answer and a 503 are retried");
        const auto key = header_of(api.requests[0], "Idempotency-Key");
        check(cm::IsIdempotencyKey(key) && header_of(api.requests[1], "Idempotency-Key") == key &&
                  header_of(api.requests[2], "Idempotency-Key") == key,
              "every retry of one share sends the same Idempotency-Key");
    }
    check(waits.size() == 2, "the worker waits between retries");
    const auto secret = read_text(base / "community-recipes" / cm::kInstallSecretFileName).substr(0, 32);
    check(cm::IsInstallSecret(secret), "the sharing ID was made on the worker");
    {
        std::lock_guard<std::mutex> lock(api.mutex);
        for (const auto& sent_request : api.requests) {
            check(header_of(sent_request, "Authorization") == "Bearer " + secret, "the sharing ID goes in Authorization");
            const std::string body(sent_request.body.begin(), sent_request.body.end());
            check(!contains(body, secret) && !contains(sent_request.url, secret), "never in a body or a URL");
        }
    }
    check(model.state().navigation.modals.back().title == "Recipe sent for review" &&
              model.state().navigation.modals.back().link == "https://github.com/leolobato/playden-ps5-recipes/pull/12" &&
              model.state().navigation.modals.back().link_caption == "Scan to follow the pull request",
          "the sent dialog shows the pull request as a QR code");
    const auto& shared = model.state().game_settings.at(kGame).shared_recipe;
    check(shared.submission_id == "sub-1" && shared.recipe_id == "community-steam-274520-darkwood-4k", "the share is saved per game");
    std::map<GameId, rt::GameSettingsRecord> saved;
    std::string reason;
    check(rt::LoadGameSettingsFile((base / "game-settings.json").string(), &saved, &reason) &&
              saved.at(kGame).shared_recipe.submission_id == "sub-1",
          "the saved share reaches the settings file");

    // Game settings shows the shared recipe's state, asked again at most every few minutes.
    open_game_settings(model);
    auto status_requests = model.TakeSharingRequests();
    check(status_requests.empty(), "a share answered just now isn't asked again at once");
    model.SetWallClock(1'000'000 + 400);
    open_game_settings(model);
    status_requests = model.TakeSharingRequests();
    // The first clock tick also asks once whether an account is linked.
    std::erase_if(status_requests, [](const SharingRequest& request) { return request.kind == SharingRequestKind::AccountCheck; });
    check(status_requests.size() == 1 && status_requests[0].kind == SharingRequestKind::Status &&
              status_requests[0].submission_id == "sub-1",
          "opening Game settings later asks for the shared recipe's state");
    api.answer_with({answer(200, R"({"id":"sub-1","state":"published","recipe_id":"community-steam-274520-darkwood-4k",)"
                                 R"("pr_url":"https://github.com/leolobato/playden-ps5-recipes/pull/12"})")});
    for (auto& status : status_requests) service.Submit(std::move(status));
    service.Wait();
    for (const auto& result : service.TakeResults()) model.ApplySharingResult(result);
    check(find_row(model, "shared-recipe") && find_row(model, "shared-recipe")->value == "Published",
          "the Shared recipe row says it's published");
    {
        std::lock_guard<std::mutex> lock(api.mutex);
        check(api.requests.size() == 1 && header_of(api.requests[0], "Authorization").empty(), "the state needs no sharing ID");
    }
    open_game_settings(model);
    check(model.TakeSharingRequests().empty(), "asked just now: not again");

    // A second share of the same game updates the saved submission.
    activate(model, "section-advanced");
    activate(model, "share-recipe");
    check(model.ShareDecisionFor(kGame).kind == cm::ShareKind::kUpdateOwn, "a saved share: the next one updates it");
    check(find_row(model, "share-title") != nullptr, "the title row is there");
    if (find_row(model, "share-title")->value == "Optional") {
        activate(model, "share-title");
        model.SubmitEditorText("Darkwood 4K");
    }
    activate(model, "share-send");
    // A lost answer three times: the share fails; Try again sends the same key back.
    api.answer_with({lost()});
    pump(model, service, &sent);
    check(sent.size() == 1 && sent[0].submission.submission_id == "sub-1" && !sent[0].submission.based_on,
          "the second share names the saved submission");
    check(find_row(model, "share-send")->title == "Try again" && find_row(model, "share-send")->tone == "problem" &&
              contains(find_row(model, "share-send")->subtitle, "couldn't reach"),
          "a lost answer says so and offers Try again");
    std::string first_key;
    {
        std::lock_guard<std::mutex> lock(api.mutex);
        first_key = header_of(api.requests.front(), "Idempotency-Key");
    }
    api.answer_with({answer(429, R"({"error":"rate_limited","message":"slow down"})", {{"Retry-After", "600"}})});
    activate(model, "share-send");
    pump(model, service, &sent);
    check(sent.size() == 1 && sent[0].submission.idempotency_key == first_key, "Try again is the same share: the same key");
    check(contains(find_row(model, "share-send")->subtitle, "Try again in 10 minutes"), "a rate limit says when to try again");
    // The pull request already has exactly this: the API says unchanged, and so does Playden, not "sent for review".
    api.answer_with({answer(201, R"({"id":"sub-1","recipe_id":"community-steam-274520-darkwood-4k","state":"in-review",)"
                                 R"("pr_url":"https://github.com/leolobato/playden-ps5-recipes/pull/12","unchanged":true})")});
    activate(model, "share-send");
    pump(model, service, &sent);
    check(model.state().navigation.modals.back().title == "Nothing new to share" &&
              contains(model.state().navigation.modals.back().body, "already has these settings") &&
              model.state().navigation.modals.back().link == "https://github.com/leolobato/playden-ps5-recipes/pull/12",
          "a share that changed nothing says so, with the pull request");
    check(model.state().game_settings.at(kGame).shared_recipe.submission_id == "sub-1", "the saved share stays");
    while (!model.state().navigation.modals.empty()) model.action("circle");
    open_game_settings(model);
    activate(model, "share-recipe");
    // The API doesn't know the saved share any more: it is forgotten, and the next Send shares a new recipe.
    api.answer_with({answer(404, R"({"error":"unknown_submission","message":"no such submission"})")});
    activate(model, "share-send");
    pump(model, service, &sent);
    check(sent.size() == 1 && sent[0].submission.submission_id == "sub-1", "the share named the saved submission");
    check(model.state().game_settings.at(kGame).shared_recipe.submission_id.empty() &&
              model.ShareDecisionFor(kGame).kind != cm::ShareKind::kUpdateOwn,
          "a share the API doesn't know is forgotten");
    check(model.view().surface == "share-recipe" && contains(find_row(model, "share-send")->subtitle, "share it as a new recipe"),
          "the Share sheet says so");
    // A refused attempt starts over with a new key; this answer matches a published revision.
    api.answer_with({answer(200, R"({"state":"duplicate","recipe_id":")" + kRecipeId + R"(","revision":")" + kRevision + R"("})")});
    activate(model, "share-send");
    pump(model, service, &sent);
    check(sent.size() == 1 && sent[0].submission.idempotency_key.empty(), "after a refusal the next attempt gets a new key");
    check(model.state().navigation.modals.back().title == "Nothing new to share" && find_row(model, "share-vote"),
          "a duplicate of a published recipe offers a thumbs up");
    // Votes not asked yet: the thumbs up opens the vote sheet, which says what a vote sends.
    activate(model, "share-vote");
    check(model.view().surface == "vote-recipe" && contains(model.view().subtitle, "random sharing ID") &&
              find_row(model, "vote-up")->selected,
          "the first vote explains what is sent, thumbs up chosen");
    activate(model, "vote-send");
    check(model.state().recipe_votes == playden::RecipeVotesPreference::kOn, "sending the first vote turns votes on");
    api.answer_with({answer(204, "")});
    pump(model, service, &sent);
    check(sent.size() == 1 && sent[0].kind == SharingRequestKind::Vote && sent[0].vote.vote == "up" &&
              sent[0].vote.recipe_id == kRecipeId && sent[0].vote.revision == kRevision && sent[0].vote.reason.empty(),
          "the thumbs up goes to the worker");
    check(model.state().vote_memory.Find(kRecipeId, kRevision) && model.state().vote_memory.Find(kRecipeId, kRevision)->vote == "up",
          "the vote is remembered");
    check(model.state().vote_memory.Find(kRecipeId, kRevision)->at > 1'700'000'000,
          "with when the API took it (an answer without a Date: the console's clock), for the counts the chooser shows");
    check(contains(read_text(base / "community-recipes" / cm::kVoteStateFileName), kRevision), "and saved by the worker");

    // Playden can't tell the version of the runtime kit: a recipe must name the one it ran on (the API refuses
    // "unknown" there), so the Share sheet says so and nothing is sent.
    {
        playden::homebrew::LiveKitInfo unknown;
        model.SetKitInfo(unknown);
        open_share(model);
        if (find_row(model, "share-title")->value == "Optional") {
            activate(model, "share-title");
            model.SubmitEditorText("Darkwood 4K");
        }
        activate(model, "share-send");
        const auto refused = model.TakeSharingRequests();
        check(std::none_of(refused.begin(), refused.end(), [](const SharingRequest& r) { return r.kind == SharingRequestKind::Submit; }),
              "a recipe without a version of the runtime kit it ran on isn't sent");
        check(contains(find_row(model, "share-send")->subtitle, "version of the runtime kit"), "the Share sheet says why");
        while (!model.state().navigation.modals.empty()) model.action("circle");
        playden::homebrew::LiveKitInfo info;
        info.version = "0.12";
        model.SetKitInfo(info);
    }
    // Duplicate of another console's pull request: no vote, its QR code.
    open_share(model);
    if (find_row(model, "share-title")->value == "Optional") {
        activate(model, "share-title");
        model.SubmitEditorText("Darkwood 4K");
    }
    api.answer_with({answer(200, R"({"state":"duplicate","recipe_id":"community-steam-274520-x","pr_url":"https://github.com/leolobato/playden-ps5-recipes/pull/9"})")});
    activate(model, "share-send");
    pump(model, service);
    check(model.state().navigation.modals.back().title == "Already in review" &&
              model.state().navigation.modals.back().link == "https://github.com/leolobato/playden-ps5-recipes/pull/9" &&
              contains(model.state().navigation.modals.back().body, "already sent the same settings"),
          "a duplicate in review says so and shows its pull request");

    // Votes after play: a game prepared with the community recipe, once per revision, five minutes or more.
    while (!model.state().navigation.modals.empty()) model.action("circle");
    open_game_settings(model);
    activate(model, "preset-picker");
    activate(model, "recipe-" + kRevision);
    if (model.state().navigation.modals.back().id == "confirmation") activate(model, "recipe-switch-confirm");
    while (!model.state().navigation.modals.empty()) model.action("circle");
    playden::homebrew::LiveRuntimeGame runtime;
    runtime.game_id = kGame;
    runtime.status = "Ready";
    runtime.prepared = runtime.has_title = runtime.ready = true;
    runtime.launch_entry_id = "game";
    runtime.activity = playden::homebrew::TitleActivity::Stopped;
    runtime.recipe_id = kRecipeId;
    runtime.recipe_version = kNewerRevision.substr(0, 32);
    model.UpdateRuntime(RuntimeState::Ready, {runtime});
    check(!model.AppliedCommunityRecipe(kGame), "a revision the catalog and the choice don't name isn't guessed");
    runtime.recipe_version = kRevision.substr(0, 32);
    model.UpdateRuntime(RuntimeState::Ready, {runtime});
    check(model.AppliedCommunityRecipe(kGame) && model.AppliedCommunityRecipe(kGame)->revision == kRevision,
          "the title's 32-hex tag gives the full revision");
    // After a game: "Did it start?" for a short session, "How did it run?" after three minutes; two prompts at most
    // for the same settings, and none once a report was sent.
    (void)model.TakeSharingRequests();
    model.NotifySessionEnded(kGame, 120);
    check(model.view().surface == "feedback" && model.view().title == "Did Darkwood start?" &&
              find_row(model, "feedback-started") && find_row(model, "feedback-no-start"),
          "two minutes of play ask whether it started");
    auto seen = model.TakeSharingRequests();
    check(seen.size() == 1 && seen[0].kind == SharingRequestKind::FeedbackSeen && seen[0].feedback_game == "steam-274520" &&
              seen[0].feedback_kit == "0.12" && !seen[0].feedback_answered,
          "showing the prompt notes it for the worker");
    model.action("circle");
    for (auto& request : seen) service.Submit(std::move(request));
    service.Wait();
    if (auto memory = service.Memory()) model.UpdateVoteMemory(*memory);
    check(contains(read_text(base / "community-recipes" / cm::kVoteStateFileName), "\"game\":\"steam-274520\""),
          "the worker saves the prompt");
    model.NotifySessionEnded(kGame, 900);
    check(model.view().surface == "feedback" && model.view().title == "How did Darkwood run?" &&
              contains(model.view().subtitle, "Darkwood at 4K"),
          "ten minutes ask how it ran, and say the answer rates the recipe");
    check(!model.state().library.ratings.contains(kGame), "Darkwood starts out Untested");
    activate(model, "feedback-good");
    check(find_row(model, "feedback-send") && find_row(model, "feedback-logs") && !find_row(model, "feedback-logs")->selected &&
              !find_row(model, "feedback-recipe"),
          "it ran well: a report without logs, and no recipe to share for unchanged settings");
    activate(model, "feedback-details");
    model.SubmitEditorText("Played the first day, no problems.");
    activate(model, "feedback-send");
    check(model.state().navigation.modals.back().title == "Before you send" &&
              contains(model.state().navigation.modals.back().body, "you own this game"),
          "the first report says what it sends");
    check(!model.state().library.ratings.contains(kGame), "the rating waits for the report to go");
    activate(model, "feedback-send-confirm");
    check(model.state().reports_explained && model.ExportPreferences().reports_explained, "and only the first time");
    check(model.state().library.ratings.contains(kGame) && model.state().library.ratings.at(kGame) == Compatibility::Works,
          "a game that ran well and was Untested is rated Works");
    api.answer_with({answer(204, ""), answer(201, R"({"id":"r-1","state":"open","issue_url":"https://github.com/leolobato/playden-ps5/issues/801","updated":false})")});
    pump(model, service, &sent);
    check(sent.size() == 3 && sent[0].kind == SharingRequestKind::FeedbackSeen && sent[1].kind == SharingRequestKind::Vote &&
              sent[1].vote.vote == "up" && sent[1].vote.revision == kRevision && sent[2].kind == SharingRequestKind::Report,
          "the report goes with the recipe's thumbs up");
    {
        const auto& report = sent[2].report;
        check(report.store == "steam" && report.store_id == "274520" && report.name == "Darkwood" && report.result == "works" &&
                  report.details == "Played the first day, no problems." && report.based_on &&
                  report.based_on->revision == kRevision && report.installed_on == "console" && !sent[2].attach_logs,
              "the report names the game, the result, the recipe and where it is installed");
        check(report.origin == "prompt", "an answer to the after-game prompt says so");
        check(report.versions.empty(), "a game that isn't Online names no Steam client");
        std::lock_guard<std::mutex> lock(api.mutex);
        check(api.requests.size() == 2 && api.requests[1].url.find("/v1/reports") != std::string::npos,
              "a report that works sends no logs");
        const std::string body(api.requests[1].body.begin(), api.requests[1].body.end());
        check(contains(body, "\"result\":\"works\"") && contains(body, "\"store_id\":\"274520\"") && !contains(body, kRevision + "x"),
              "the body is the contract's");
    }
    check(model.state().navigation.modals.back().title == "Test report sent" &&
              model.state().navigation.modals.back().link == "https://github.com/leolobato/playden-ps5/issues/801",
          "the report's issue shows as a QR code");
    check(model.state().vote_memory.Find(kRecipeId, kRevision) && model.state().vote_memory.Find(kRecipeId, kRevision)->vote == "up",
          "the thumbs up is remembered");
    while (!model.state().navigation.modals.empty()) model.action("circle");
    model.NotifySessionEnded(kGame, 900);
    check(model.state().navigation.modals.empty(), "once a report was sent for these settings, Playden doesn't ask again");
    // A report with the recipe's vote, sent while nothing answers and left before it ends: one message, the report's.
    {
        check(model.action("details:274520"), "Darkwood's page opens for a failing report");
        activate(model, "details-more");
        activate(model, "share-feedback");
        activate(model, "feedback-problems");
        activate(model, "feedback-reason-slow");
        activate(model, "feedback-send");
        while (!model.state().navigation.modals.empty()) model.action("circle");
        api.answer_with({unreachable()});
        std::vector<SharingRequest> failing;
        pump(model, service, &failing);
        check(std::count_if(failing.begin(), failing.end(), [](const SharingRequest& r) { return r.kind == SharingRequestKind::Vote; }) == 1 &&
                  std::count_if(failing.begin(), failing.end(), [](const SharingRequest& r) { return r.kind == SharingRequestKind::Report; }) == 1,
              "the report goes with the recipe's vote");
        // Failure dialogs come one at a time: dismiss each and count them all.
        int messages = 0, report_messages = 0;
        for (int guard = 0; guard < 10 && !model.state().navigation.modals.empty(); ++guard) {
            const auto title = model.state().navigation.modals.back().title;
            if (contains(title, "wasn't sent")) ++messages;
            if (contains(title, "test report")) ++report_messages;
            model.action("circle");
        }
        for (const auto& notice : model.state().notices)
            if (contains(notice.message, "wasn't sent")) ++messages;
        check(messages == 1 && report_messages == 1, "a failed report and its vote say so once, as the report");
        model.TakeSharingRequests();
    }
    // A problem never changes the rating; a report that it ran well keeps a rating the player chose.
    {
        check(model.action("rate:274520:untested"), "the player sets Darkwood back to Untested");
        check(model.action("details:274520"), "Darkwood's page opens for a problem report");
        activate(model, "details-more");
        activate(model, "share-feedback");
        activate(model, "feedback-problems");
        activate(model, "feedback-reason-slow");
        activate(model, "feedback-send");
        check(model.state().library.ratings.at(kGame) == Compatibility::Untested, "a problem report leaves Untested");
        // Counted, with no issue: a plain thanks, and nothing to scan.
        const auto counted = model.TakeSharingRequests();
        const auto report = std::find_if(counted.begin(), counted.end(), [](const SharingRequest& r) { return r.kind == SharingRequestKind::Report; });
        check(report != counted.end() && report->report.origin == "manual", "a report from Share feedback says so");
        SharingResult waits;
        waits.kind = SharingRequestKind::Report;
        waits.ticket = report->ticket;
        waits.game_id = kGame;
        waits.report.ok = true;
        waits.report.id = "r-2";
        waits.report.state = "counted";
        model.ApplySharingResult(waits);
        check(!model.state().navigation.modals.empty() && model.state().navigation.modals.back().title == "Report received" &&
                  model.state().navigation.modals.back().link.empty() &&
                  contains(model.state().navigation.modals.back().body, "when another player reports the same game"),
              "a problem that waits for another player says so, with no link");
        while (!model.state().navigation.modals.empty()) model.action("circle");
        model.TakeSharingRequests();
        check(model.action("rate:274520:untested") && model.action("details:274520"), "Untested set again");
        activate(model, "details-more");
        activate(model, "share-feedback");
        activate(model, "feedback-good");
        activate(model, "feedback-send");
        check(model.state().library.ratings.at(kGame) == Compatibility::Works, "Untested set by hand also becomes Works");
        api.answer_with({answer(204, ""), answer(201, R"({"id":"r-3","state":"counted","issue_url":null,"updated":false})")});
        pump(model, service);
        check(!model.state().navigation.modals.empty() && model.state().navigation.modals.back().title == "Answer counted" &&
                  model.state().navigation.modals.back().link.empty() &&
                  contains(model.state().navigation.modals.back().body, "counts for the compatibility list"),
              "it ran well is counted, with no link");
        while (!model.state().navigation.modals.empty()) model.action("circle");
        model.TakeSharingRequests();
        check(model.action("rate:274520:playable") && model.action("details:274520"), "the player rates Darkwood Playable");
        activate(model, "details-more");
        activate(model, "share-feedback");
        activate(model, "feedback-good");
        activate(model, "feedback-send");
        check(model.state().library.ratings.at(kGame) == Compatibility::Playable, "a report that it ran well keeps Playable");
        while (!model.state().navigation.modals.empty()) model.action("circle");
        model.TakeSharingRequests();
    }
    // Other, in the player's words, goes with the report and with its vote.
    {
        check(model.action("details:274520"), "Darkwood's page opens for a report about Other");
        activate(model, "details-more");
        activate(model, "share-feedback");
        activate(model, "feedback-problems");
        activate(model, "feedback-reason-other");
        model.SubmitEditorText("The map screen flickers");
        activate(model, "feedback-send");
        const auto other = model.TakeSharingRequests();
        const auto vote = std::find_if(other.begin(), other.end(), [](const SharingRequest& r) { return r.kind == SharingRequestKind::Vote; });
        const auto report = std::find_if(other.begin(), other.end(), [](const SharingRequest& r) { return r.kind == SharingRequestKind::Report; });
        check(report != other.end() && report->report.reason_text == "The map screen flickers", "the report says what Other was");
        check(vote != other.end() && vote->vote.reason == "other" && vote->vote.reason_text == "The map screen flickers",
              "and so does its vote");
        while (!model.state().navigation.modals.empty()) model.action("circle");
    }
    // A newer revision: new settings ask again; Not now twice, then no more.
    auto catalog = model.state().community;
    catalog.games[kGameRef] = {offer(kNewerRevision)};
    model.UpdateCommunityCatalog(catalog);
    open_game_settings(model);
    activate(model, "recipe-update");
    if (!model.state().navigation.modals.empty() && model.state().navigation.modals.back().id == "confirmation")
        activate(model, "recipe-switch-confirm");
    while (!model.state().navigation.modals.empty()) model.action("circle");
    (void)model.TakeSharingRequests();
    runtime.recipe_version = kNewerRevision.substr(0, 32);
    model.UpdateRuntime(RuntimeState::Ready, {runtime});
    for (int prompt = 0; prompt < 2; ++prompt) {
        model.NotifySessionEnded(kGame, 900);
        check(model.view().surface == "feedback" && model.view().title == "How did Darkwood run?",
              "after ten minutes with a new revision Playden asks how it ran");
        model.action("circle");
        pump(model, service);
    }
    check(model.state().navigation.modals.empty(), "Not now closes the prompt");
    model.NotifySessionEnded(kGame, 900);
    check(model.state().navigation.modals.empty(), "two dismissed prompts don't ask about these settings again");
    // Game settings keeps the vote: a thumbs down with a reason.
    open_game_settings(model);
    check(find_row(model, "recipe-vote") && find_row(model, "recipe-vote")->value == "Not yet", "Your vote: not yet");
    activate(model, "recipe-vote");
    activate(model, "vote-down");
    check(find_row(model, "vote-reason-slow") && find_row(model, "vote-reason-crash"), "a thumbs down offers reasons");
    activate(model, "vote-reason-slow");
    activate(model, "vote-send");
    api.answer_with({answer(204, "")});
    pump(model, service, &sent);
    check(sent.size() == 1 && sent[0].vote.vote == "down" && sent[0].vote.reason == "slow" && sent[0].vote.revision == kNewerRevision,
          "the thumbs down and its reason go to the worker");
    check(find_row(model, "recipe-vote") && find_row(model, "recipe-vote")->value == "👎 Slow", "Your vote shows it");

    // A newer revision chosen but the game not prepared with it yet: Your vote stays on the revision
    // the title plays with. A failed send can be tried again from there.
    const std::string third(64, 'f');  // not 'e': the votes-off part below uses it
    model.SetCommunityRecipeLoader([third](const std::string& revision) -> std::optional<rt::CommunityRecipe> {
        if (revision == kRevision || revision == kNewerRevision || revision == third) return community_recipe(revision);
        return std::nullopt;
    });
    auto later = model.state().community;
    later.games[kGameRef] = {offer(third)};
    model.UpdateCommunityCatalog(later);
    open_game_settings(model);
    activate(model, "recipe-update");
    if (!model.state().navigation.modals.empty() && model.state().navigation.modals.back().id == "confirmation")
        activate(model, "recipe-switch-confirm");
    (void)model.TakeSharingRequests();
    check(model.state().game_settings.at(kGame).community_recipe.revision == third, "the newest revision is chosen");
    check(model.VoteTargetFor(kGame) && model.VoteTargetFor(kGame)->revision == kNewerRevision,
          "the vote is for the revision the game was prepared with");
    open_game_settings(model);
    check(find_row(model, "recipe-vote") && find_row(model, "recipe-vote")->value == "👎 Slow",
          "Your vote shows the vote for the prepared revision, not the chosen one");
    check(find_row(model, "recipe-vote") && find_row(model, "recipe-vote")->title == "How did this recipe run?" &&
              contains(find_row(model, "recipe-vote")->subtitle, "the recipe in Preset"),
          "Game settings asks how the recipe ran, tied to Preset");
    activate(model, "recipe-vote");
    activate(model, "vote-up");
    activate(model, "vote-send");
    check(model.view().surface == "vote-recipe" && find_row(model, "vote-send") &&
              find_row(model, "vote-send")->title == "Sending\u2026" && !find_row(model, "vote-send")->enabled,
          "the vote sheet waits while the vote sends");
    api.answer_with({answer(400, R"({"error":"rule","message":"no"})")});
    pump(model, service, &sent);
    check(sent.size() == 1 && sent[0].vote.revision == kNewerRevision, "the vote goes to the prepared revision");
    check(model.view().surface == "vote-recipe" && find_row(model, "vote-send")->title == "Try again" &&
              find_row(model, "vote-send")->tone == "problem" && !find_row(model, "vote-send")->subtitle.empty(),
          "a failed send stays on the sheet and says why on Try again");
    model.action("circle");
    check(find_row(model, "recipe-vote") && find_row(model, "recipe-vote")->value == "👎 Slow", "a failed send changes nothing");
    // Leaving while it sends: the row says Sending…, then Not sent; nothing answers, so it isn't tried again.
    activate(model, "recipe-vote");
    activate(model, "vote-up");
    activate(model, "vote-send");
    model.action("circle");
    check(find_row(model, "recipe-vote")->value == "Sending\u2026", "left while sending, the row says so");
    api.answer_with({unreachable()});
    {
        std::vector<SharingRequest> unsent;
        pump(model, service, &unsent);
        std::lock_guard<std::mutex> lock(api.mutex);
        check(api.requests.size() == 1, "a server that doesn't answer isn't tried again at once");
    }
    check(find_row(model, "recipe-vote")->value == "Not sent" && find_row(model, "recipe-vote")->tone == "problem",
          "then it says the vote wasn't sent");
    activate(model, "recipe-vote");
    check(model.view().surface == "vote-recipe", "the vote sheet opens again after a failed send");
    // Other: chosen at once, the keyboard asks what it was, and it goes with the vote.
    activate(model, "vote-down");
    activate(model, "vote-reason-other");
    check(model.state().editor.active, "Other opens the keyboard");
    model.SubmitEditorText("Music stutters");
    check(find_row(model, "vote-reason-other")->selected && find_row(model, "vote-reason-other")->subtitle == "Music stutters",
          "Other is chosen and shows what was typed");
    activate(model, "vote-send");
    api.answer_with({answer(204, "")});
    pump(model, service, &sent);
    check(sent.size() == 1 && sent[0].vote.reason == "other" && sent[0].vote.reason_text == "Music stutters",
          "the vote carries Other and what it was");
    {
        std::lock_guard<std::mutex> lock(api.mutex);
        const std::string body(api.requests.back().body.begin(), api.requests.back().body.end());
        check(contains(body, "\"reason_text\":\"Music stutters\""), "the body has the text");
    }
    open_game_settings(model);
    activate(model, "recipe-vote");
    activate(model, "vote-up");
    activate(model, "vote-send");
    api.answer_with({answer(204, "")});
    pump(model, service, &sent);
    check(sent.size() == 1 && sent[0].vote.revision == kNewerRevision && find_row(model, "recipe-vote")->value == "👍 Good",
          "sent again, the vote counts");
    // Not prepared (yet): the vote is for the chosen revision.
    runtime.prepared = false;
    model.UpdateRuntime(RuntimeState::Ready, {runtime});
    check(model.VoteTargetFor(kGame) && model.VoteTargetFor(kGame)->revision == third, "before preparing, the chosen revision");
    runtime.prepared = true;
    model.UpdateRuntime(RuntimeState::Ready, {runtime});

    // A status answer that echoes the sharing ID is refused whole; nothing of it reaches the model or a log.
    {
        const auto echoed = read_text(base / "community-recipes" / cm::kInstallSecretFileName).substr(0, 32);
        SharingRequest status;
        status.kind = SharingRequestKind::Status;
        status.ticket = 9001;
        status.game_id = kGame;
        status.submission_id = "sub-1";
        api.answer_with({answer(200, R"({"id":"sub-1","state":"published","recipe_id":"community-steam-274520-darkwood-4k",)"
                                     R"("pr_url":"https://github.com/leolobato/playden-ps5-recipes/pull/)" + echoed + R"("})")});
        service.Submit(std::move(status));
        service.Wait();
        const auto results = service.TakeResults();
        check(results.size() == 1 && !results[0].status.ok && results[0].status.error.code == "bad_response" &&
                  results[0].status.pr_url.empty(),
              "a status answer holding the sharing ID is refused");
        for (const auto& result : results) model.ApplySharingResult(result);
        check(!contains(model.snapshot(), echoed), "the echoed sharing ID never reaches the model");
    }

    // Votes off: no prompt, and a thumbs up offer explains how to turn them on.
    model.SetWallClock(1'000'000 + 2000);
    while (!model.state().navigation.modals.empty()) model.action("circle");
    model.action("circle");
    model.action("l1");
    check(model.state().navigation.route == playden::homebrew::Route::Settings, "Settings opens");
    check(model.action("focus:settings-recipe-votes") && model.action("cross"), "Settings › Recipe votes toggles");
    check(model.state().recipe_votes == playden::RecipeVotesPreference::kOff, "votes are off");
    check(model.ExportPreferences().recipe_votes == playden::RecipeVotesPreference::kOff, "the preference is saved");
    auto newest = model.state().community;
    newest.games[kGameRef] = {offer(std::string(64, 'e'))};
    runtime.recipe_version = std::string(32, 'e');
    model.UpdateCommunityCatalog(newest);
    model.UpdateRuntime(RuntimeState::Ready, {runtime});
    model.NotifySessionEnded(kGame, 3600);
    check(!model.state().navigation.modals.empty() && model.state().navigation.modals.back().id == "feedback",
          "with votes off Playden still asks how the game ran");
    model.action("circle");
    model.TakeSharingRequests();
    check(model.action("focus:settings-ask-after-games") && model.action("cross"), "Settings › Ask how games ran toggles");
    check(!model.ExportPreferences().ask_after_games, "asking off is saved");
    model.NotifySessionEnded(kGame, 3600);
    check(model.state().navigation.modals.empty() && model.TakeSharingRequests().empty(), "with asking off nothing asks or is sent");

    // Reset sharing ID: confirmed, then done on the worker.
    check(model.action("focus:settings-sharing-reset") && model.action("cross"), "Reset sharing ID asks first");
    check(contains(model.state().navigation.modals.back().body, "can no longer be changed"), "it says what a reset means");
    activate(model, "sharing-reset-confirm");
    pump(model, service);
    const auto fresh = read_text(base / "community-recipes" / cm::kInstallSecretFileName).substr(0, 32);
    check(cm::IsInstallSecret(fresh) && fresh != secret, "the worker made a new sharing ID");

    // Share feedback… still works with asking off: Other with what it was goes as reason_text; while the report is
    // sending, Other doesn't open the keyboard again (the sheet may close under it).
    while (!model.state().navigation.modals.empty()) model.action("circle");
    check(model.action("details:274520"), "Darkwood's page opens for Share feedback…");
    activate(model, "details-more");
    activate(model, "share-feedback");
    activate(model, "feedback-problems");
    activate(model, "feedback-reason-other");
    check(model.state().editor.active, "Other opens the keyboard");
    model.SubmitEditorText("Music stutters");
    activate(model, "feedback-send");
    check(find_row(model, "feedback-send") && find_row(model, "feedback-send")->title == "Sending\u2026", "the report is sending");
    activate(model, "feedback-reason-other");
    check(!model.state().editor.active, "while it sends, Other doesn't open the keyboard");
    {
        const auto requests = model.TakeSharingRequests();
        const auto report = std::find_if(requests.begin(), requests.end(),
                                         [](const SharingRequest& r) { return r.kind == SharingRequestKind::Report; });
        check(report != requests.end() && report->report.reason == "other" && report->report.reason_text == "Music stutters",
              "the report carries Other and what it was");
    }
    while (!model.state().navigation.modals.empty()) model.action("circle");

    // The sharing ID never reached the model, its snapshot, the settings file or a log line.
    for (const auto& id : {secret, fresh}) {
        check(!contains(model.snapshot(), id), "the snapshot has no sharing ID");
        check(!contains(read_text(base / "game-settings.json"), id), "the settings file has no sharing ID");
        std::lock_guard<std::mutex> lock(log_mutex);
        for (const auto& line : log) check(!contains(line, id), "no log line holds the sharing ID");
    }
    service.Shutdown();
}

// What the recipe cache keeps: nothing is collected while what is in use is unknown.
void community_keep(const std::filesystem::path& base) {
    using playden::homebrew::CommunityKeepRevisions;
    using playden::homebrew::RuntimeHostStatus;
    std::filesystem::create_directories(base);
    Simulation model("fixtures-not-used", "profile-not-used.json", ModelMode::Live);
    check(CommunityKeepRevisions(model, nullptr, false, true) == std::set<std::string>{},
          "the Mac host (never a runtime) keeps the choices alone");
    check(!CommunityKeepRevisions(model, nullptr, false, false), "the console without a runtime host keeps everything");
    playden::homebrew::RuntimeSnapshot runtime;
    runtime.status = RuntimeHostStatus::kStarting;
    runtime.community_references = {std::string(32, 'a')};
    check(!CommunityKeepRevisions(model, &runtime, true, false), "a starting runtime keeps everything");
    runtime.status = RuntimeHostStatus::kReady;
    runtime.installs_known = false;
    check(!CommunityKeepRevisions(model, &runtime, true, false), "installs not known yet keeps everything");
    runtime.installs_known = true;
    check(CommunityKeepRevisions(model, &runtime, true, false) == std::set<std::string>{std::string(32, 'a')},
          "a ready runtime adds the installed games' recipes");
    const auto unreadable = base / "unreadable-game-settings.json";
    std::ofstream(unreadable) << "{ not json";
    model.SetGameSettingsFile(unreadable);
    check(!CommunityKeepRevisions(model, &runtime, true, false) && !CommunityKeepRevisions(model, nullptr, false, true),
          "unreadable game settings keep everything");
}

// A settings-only recipe (community.format_version 2): it starts the program Playden finds, the chooser's panel says
// so, and a share based on it is a new recipe that credits it, never a suggested change to it.
void settings_only_base(const std::filesystem::path& base) {
    std::filesystem::create_directories(base);
    Simulation model("fixtures-not-used", "profile-not-used.json", ModelMode::Live);
    set_up(model, base, false);
    rt::GameRecipe own;
    own.id = "steam-274520-auto";
    own.version = 1;
    own.app_id = 274520;
    own.name = "Darkwood";
    own.launch_entries = {{"game", "Darkwood", "Darkwood.exe", ""}};
    own.arguments = "{resolution}";
    own.architecture = "pe64";
    own.graphics = "dxvk";
    model.SetGeneratedRecipes({own});
    // The copy's programs, as the folder's scan found them.
    std::map<GameId, playden::homebrew::LiveGameSetup> setups;
    setups[kGame].scanned = true;
    setups[kGame].programs = {{"Darkwood.exe", "pe64", "", false, ""}, {"Bin/Tool.exe", "pe64", "", false, ""}};
    model.SetGameSetups(setups);
    const auto file = [](const std::string& slug, const std::string& defaults) {
        return "{\"recipe_id\": \"community-steam-274520-" + slug + "\", \"recipe_version\": 1,"
               " \"game\": {\"store\": \"steam\", \"key\": \"274520\", \"name\": \"Darkwood\"}, \"name\": \"Darkwood\","
               " \"defaults\": {" + defaults + "}, \"community\": {\"format_version\": 2, \"title\": \"" + slug +
               "\", \"status\": \"works\", \"author\": \"rina\"}}";
    };
    const std::string plain_revision(64, 'e'), missing_revision(64, 'f'), tool_revision(64, 'a');
    std::map<std::string, rt::CommunityRecipe> objects;
    for (const auto& [revision, text] : {std::pair{plain_revision, file("mouse", "\"resolution\": \"1920x1080\"")},
                                         std::pair{missing_revision, file("gone", "\"program\": \"Bin/Gone.exe\"")},
                                         std::pair{tool_revision, file("tool", "\"program\": \"BIN\\\\tool.exe\"")}}) {
        rt::CommunityRecipe community;
        std::string reason;
        check(rt::ParseCommunityRecipe(text, &community, &reason), "the settings-only file reads");
        objects[revision] = community;
    }
    playden::homebrew::CommunityCatalog catalog;
    catalog.release_tag = "r20261108.5";
    for (const auto& [revision, object] : objects) {
        auto listed = offer(revision);
        listed.id = object.recipe.id;
        listed.title = object.title;
        listed.flags = rt::CommunityRecipeFlags(object);
        catalog.games[kGameRef].push_back(listed);
    }
    model.UpdateCommunityCatalog(catalog);
    model.SetCommunityRecipeLoader([objects](const std::string& revision) -> std::optional<rt::CommunityRecipe> {
        const auto found = objects.find(revision);
        if (found == objects.end()) return std::nullopt;
        return found->second;
    });

    // The chooser's panel: the program Playden finds, or the named one that isn't in this copy.
    open_game_settings(model);
    activate(model, "preset-picker");
    check(model.action("focus:recipe-" + plain_revision) && model.view().comparison, "the settings-only recipe is listed");
    {
        const auto& starts = model.view().comparison->starts;
        check(!starts.empty() && starts.front() == "Uses the program Playden finds" &&
                  std::none_of(starts.begin(), starts.end(), [](const std::string& line) { return contains(line, "Darkwood.exe"); }),
              "the panel says the recipe uses the program Playden finds, without listing Playden's program as the recipe's");
    }
    check(model.ProgramNoteFor(kGame, plain_revision).empty(), "no program note when the recipe names none");
    check(model.action("focus:recipe-" + missing_revision) && model.view().comparison, "the one naming a program is listed");
    check(model.view().comparison->starts.front() == "Uses the program Playden finds (Bin/Gone.exe isn't in this copy)" &&
              model.ProgramNoteFor(kGame, missing_revision) == "Bin/Gone.exe isn't in this copy, so Playden's program is used.",
          "a named program this copy hasn't: Playden's program, and the note says so without naming another game");
    // Chosen: Playden's program with the recipe's settings, matched by its game, id and revision alone.
    activate(model, "recipe-" + plain_revision);
    if (model.state().navigation.modals.back().id == "confirmation") activate(model, "recipe-switch-confirm");
    check(model.RecipeBaseFor(kGame) && model.RecipeBaseFor(kGame)->revision == plain_revision &&
              model.RecipeFallbackFor(kGame).empty(),
          "the settings-only recipe is the base and applies");
    const auto& recipe = model.RecipeFor(kGame);
    check(recipe.id == "community-steam-274520-mouse" && recipe.launch_entries.size() == 1 &&
              recipe.launch_entries[0].executable == "Darkwood.exe" && recipe.architecture == "pe64" &&
              recipe.defaults.at("resolution") == "1920x1080",
          "it runs with Playden's program and its own settings");
    // Playden with no program for the copy: the recipe doesn't apply, and Game settings says why.
    auto none = own;
    none.launch_entries.clear();
    model.SetGeneratedRecipes({none});
    check(model.RecipeFallbackFor(kGame) == rt::kCommunityRecipeNoProgram && !model.RecipeBaseFor(kGame),
          "without a program Playden can find, the recipe falls back with the reason");
    model.SetGeneratedRecipes({own});
    check(model.RecipeFallbackFor(kGame).empty(), "with one again, it applies");

    // Sharing from it with the program kept: a settings-only share, so Suggest changes keeps it settings-only.
    check(model.ShareDecisionFor(kGame).kind == cm::ShareKind::kNoChangesOfferVote, "unchanged, the share offers a vote");
    open_game_settings(model);
    activate(model, "setting-resolution");
    std::string choice;
    for (const auto& row : model.view().rows)
        if (row.title.find("1280") != std::string::npos) choice = row.key;
    check(!choice.empty(), "another resolution can be chosen");
    activate(model, choice);
    const auto resolution = model.state().game_settings.at(kGame).overrides.at("resolution");
    auto decision = model.ShareDecisionFor(kGame);
    check(decision.kind == cm::ShareKind::kAskPlayer && decision.settings_only && decision.preselect == cm::ShareChoice::kUpdate,
          "a settings change is a settings-only share, Suggest changes pre-selected");
    const auto send = [&]() -> std::optional<SharingRequest> {
        activate(model, "share-send");
        if (find_row(model, "share-send-confirm")) activate(model, "share-send-confirm");
        for (auto& request : model.TakeSharingRequests())
            if (request.kind == SharingRequestKind::Submit) return request;
        return std::nullopt;
    };
    open_share(model);
    check(find_row(model, "share-choice-update") && find_row(model, "share-choice-new") &&
              contains(find_row(model, "share-what")->subtitle, "the program Playden finds") &&
              !contains(find_row(model, "share-what")->subtitle, "Darkwood.exe"),
          "the Share sheet offers Suggest changes and says no program is sent");
    auto sent = send();
    check(sent.has_value(), "the settings-only share is sent");
    if (sent) {
        rt::CommunityRecipe shared;
        std::string reason;
        check(rt::ParseCommunityRecipe(sent->submission.recipe_json, &shared, &reason) && rt::IsSettingsOnlyRecipe(shared) &&
                  shared.recipe.launch_entries.empty() && shared.executable.empty() &&
                  shared.recipe.defaults.at("resolution") == resolution &&
                  !contains(sent->submission.recipe_json, "launch_entries") && !contains(sent->submission.recipe_json, "Darkwood.exe"),
              "it sends a settings-only recipe with the player's change");
        check(sent->submission.based_on && sent->submission.based_on->revision == plain_revision &&
                  sent->submission.choice == cm::ShareChoice::kUpdate,
              "as a suggested change to the settings-only recipe");
    }

    // The player picks a program: the share names it, and is always a new recipe that credits the settings-only one.
    while (!model.state().navigation.modals.empty()) model.action("circle");
    open_game_settings(model);
    activate(model, "setting-launch_option");
    activate(model, "program-1");
    check(model.state().game_settings.at(kGame).overrides.count("program"), "the player chose Bin/Tool.exe");
    decision = model.ShareDecisionFor(kGame);
    check(decision.kind == cm::ShareKind::kNewRecipe && !decision.settings_only && decision.send_based_on && decision.send_new &&
              cm::ChoiceToSend(decision, cm::ShareChoice::kNone) == cm::ShareChoice::kNew,
          "a changed program is shared as a new recipe crediting the settings-only one");
    open_share(model);
    check(!find_row(model, "share-choice-update") && !find_row(model, "share-choice-new") &&
              contains(find_row(model, "share-what")->subtitle, "Tool.exe"),
          "the Share sheet doesn't offer to suggest changes, and names the program");
    sent = send();
    check(sent.has_value(), "the program share is sent");
    if (sent) {
        rt::CommunityRecipe shared;
        std::string reason;
        check(rt::ParseCommunityRecipe(sent->submission.recipe_json, &shared, &reason) && shared.format_version == 1 &&
                  shared.executable == "Bin/Tool.exe" && sent->submission.choice == cm::ShareChoice::kNew,
              "it sends a format 1 recipe with its program, as new");
    }

    // A recipe naming BIN\tool.exe, and the player picking that same file from the folder (Bin/Tool.exe): the program
    // is the same, so the share stays settings-only.
    while (!model.state().navigation.modals.empty()) model.action("circle");
    open_game_settings(model);
    activate(model, "preset-picker");
    activate(model, "recipe-" + tool_revision);
    if (model.state().navigation.modals.back().id == "confirmation") activate(model, "recipe-switch-confirm");
    while (!model.state().navigation.modals.empty()) model.action("circle");
    check(model.RecipeBaseFor(kGame) && model.RecipeBaseFor(kGame)->revision == tool_revision, "the recipe naming tool.exe is the base");
    open_game_settings(model);
    activate(model, "setting-launch_option");
    activate(model, "program-1");
    activate(model, "setting-resolution");
    std::string other;
    for (const auto& row : model.view().rows)
        if (row.title.find("1280") != std::string::npos) other = row.key;
    if (!other.empty()) activate(model, other);
    check(model.state().game_settings.at(kGame).overrides.count("program"), "the player chose Bin/Tool.exe");
    decision = model.ShareDecisionFor(kGame);
    check(decision.settings_only && decision.kind == cm::ShareKind::kAskPlayer && !decision.send_new,
          "the same program, written differently, is no program change");
}

}  // namespace

// A report that didn't work: the worker uploads the game's logs, redacted, then the report naming them.
void report_with_logs(const std::filesystem::path& base) {
    std::filesystem::create_directories(base / "logs");
    std::filesystem::permissions(base, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace);
    const auto write = [](const std::filesystem::path& path, const std::string& text) {
        std::FILE* file = std::fopen(path.c_str(), "wb");
        std::fwrite(text.data(), 1, text.size(), file);
        std::fclose(file);
    };
    write(base / "logs" / "title-FAKE78011.log",
          "== session-1.log\nPW_REPORT/1 build=abc profile=playden-steam-274520 time=1000\nuser L\"SteamUser\" L\"leoaccount\"\n");
    write(base / "logs" / "title-FAKE78012.log", "== session-1.log\nPW_REPORT/1 profile=playden-steam-220 time=2000\nother game\n");
    write(base / "logs" / "playden.log", "signed in as leoaccount 76561198000000001\nconsole_firmware=13.60\n");
    // Not logs: a screenshot another tool left there stays out of a report.
    write(base / "logs" / "shot.png", std::string(1024, 'p'));
    // A rotated log too big for the API's 8 MB: the report goes without the rotated logs rather than without logs.
    write(base / "logs" / "runtime.log.1", std::string(9u << 20, 'r'));
    // A kit with many capabilities: the summary's fact is longer than a version row may be (200 characters).
    std::filesystem::create_directories(base / "kits" / "playden-kit");
    {
        std::string caps;
        for (int i = 0; i < 40; ++i) caps += std::string(caps.empty() ? "" : ",") + "\"capability-number-" + std::to_string(i) + "\"";
        write(base / "kits" / "playden-kit" / "manifest.json", "{\"runtime\":{\"artifact_revision\":\"abc\"},\"capabilities\":[" + caps + "]}");
    }
    FakeApi api;
    playden::homebrew::SharingServiceConfig config;
    config.store_root = (base / "community-recipes").string();
    config.fetch = api.fetch();
    config.sleep = [](std::uint32_t, const std::atomic<bool>&) {};
    config.logs = playden::web::LogsPaths{(base / "logs").string(), (base / "homebrew").string(), (base / "kits").string(),
                                          (base / "app").string()};
    config.now = [] { return std::int64_t{1'762'000'000}; };
    playden::homebrew::SharingService service(config);
    service.Start();
    service.SetAccount({"leoaccount", 76561198000000001ull});
    api.answer_with({answer(201, R"({"id":"att_9","url":"https://attachments.playden.club/att_9/playden-logs.zip"})"),
                     answer(201, R"({"id":"r_9","state":"open","issue_url":"https://github.com/leolobato/playden-ps5/issues/809"})")});
    SharingRequest request;
    request.kind = SharingRequestKind::Report;
    request.ticket = 1;
    request.game_id = kGame;
    request.attach_logs = true;
    request.logs_app = 274520;
    request.logs_name = "Darkwood";
    auto& report = request.report;
    report.store = "steam";
    report.store_id = "274520";
    report.name = "Darkwood";
    report.result = "no-start";
    report.reason = "no-start";
    report.installed_on = "console";
    report.origin = "manual";
    report.controls = {"dualsense"};
    report.kit = "0.19";
    report.firmware = "13.60";
    report.playden_version = "0.10.0";
    // A game set to Online names Valve's Steam client version: after the logs' facts.
    report.versions = {{"Steam client", "1792618285"}};
    service.Submit(std::move(request));
    service.Wait();
    const auto results = service.TakeResults();
    check(results.size() == 1 && results[0].report.ok && results[0].report.issue_url.find("/issues/809") != std::string::npos &&
              !results[0].idempotency_key.empty(),
          "the report with logs is sent");
    std::lock_guard<std::mutex> lock(api.mutex);
    check(api.requests.size() == 2 && api.requests[0].url.find("/v1/attachments") != std::string::npos &&
              api.requests[1].url.find("/v1/reports") != std::string::npos,
          "the logs go first, then the report");
    const std::string zip(api.requests[0].body.begin(), api.requests[0].body.end());
    check(zip.rfind("PK", 0) == 0 && contains(zip, "title-FAKE78011.log") && !contains(zip, "title-FAKE78012.log") &&
              contains(zip, "playden.log") && contains(zip, "summary.md"),
          "the zip holds Playden's logs and this game's, not another game's");
    check(!contains(zip, "shot.png") && !contains(zip, "runtime.log.1") && zip.size() < (8u << 20),
          "only logs go in the zip, and rotated logs stay out when it would be too large");
    check(!contains(zip, "leoaccount") && !contains(zip, "76561198000000001") && contains(zip, "<steam-user>"),
          "the account name and SteamID are removed");
    const std::string body(api.requests[1].body.begin(), api.requests[1].body.end());
    check(contains(body, "\"attachment\":\"att_9\"") && contains(body, "\"versions\":[[") && !contains(body, "leoaccount"),
          "the report names the logs and carries the versions");
    check(contains(body, "[\"Steam client\",\"1792618285\"]") && body.find("Playden version") < body.find("Steam client"),
          "the report's own row comes after the logs' facts");
    check(contains(body, "capability-number-1") && !contains(body, "capability-number-39") && contains(body, "\u2026"),
          "a long fact is shortened to fit a version row, not refused");
    service.Shutdown();
}

// Settings › Community › Playden account: a code and QR code, polled until approved; the token stays with the
// worker (0600), shares and reports carry it, and an ended link is forgotten.
void account_link(const std::filesystem::path& base) {
    std::filesystem::create_directories(base);
    std::filesystem::permissions(base, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace);
    FakeApi api;
    playden::homebrew::SharingServiceConfig config;
    config.store_root = (base / "community-recipes").string();
    config.fetch = api.fetch();
    config.sleep = [](std::uint32_t, const std::atomic<bool>&) {};
    playden::homebrew::SharingService service(config);
    service.Start();
    service.Wait();
    Simulation model("fixtures-not-used", "profile-not-used.json", ModelMode::Live);
    set_up(model, base);
    pump(model, service);
    model.SetWallClock(1'000'001);
    auto checked = model.TakeSharingRequests();
    check(checked.size() == 1 && checked[0].kind == SharingRequestKind::AccountCheck, "Playden asks once whether an account is linked");
    for (auto& request : checked) service.Submit(std::move(request));
    service.Wait();
    for (const auto& result : service.TakeResults()) model.ApplySharingResult(result);
    check(model.state().account_link.checked && model.state().account_link.state.empty(), "no account linked yet");
    // Choosing the account in the picker starts a link; cancelling it must not choose the account later.
    open_share(model);
    activate(model, "share-as");
    api.answer_with({answer(200, R"({"device_code":"dev_code_cancelled01","user_code":"AAAA-BBBB","verification_uri_complete":"https://account.playden.club/device?user_code=AAAA-BBBB","expires_in":900,"interval":5})")});
    activate(model, "share-as-account");
    pump(model, service);
    check(model.state().navigation.modals.back().title == "Link a Playden account", "the account choice opens linking");
    model.action("circle");
    check(model.state().account_link.state.empty(), "closing the dialog stops linking");
    while (!model.state().navigation.modals.empty()) model.action("circle");
    model.action("circle");
    model.action("l1");
    check(model.state().navigation.route == playden::homebrew::Route::Settings, "Settings opens");
    model.SetWallClock(1'000'002);
    const std::string token = "tok_rina_0123456789abcdef";
    // The API sends the code without its dash; Playden shows it as two groups of four.
    api.answer_with({answer(200, R"({"device_code":"dev_code_0123456789","user_code":"WDJBMJHT","verification_uri_complete":"https://account.playden.club/device?user_code=WDJB-MJHT","expires_in":900,"interval":5})")});
    check(model.action("focus:settings-account-link") && model.action("cross"), "Playden account opens linking");
    pump(model, service);
    {
        const auto& dialog = model.state().navigation.modals.back();
        check(dialog.title == "Link a Playden account" && contains(dialog.body, "WDJB-MJHT") &&
                  contains(dialog.link_caption, "code WDJB-MJHT") &&
                  dialog.link == "https://account.playden.club/device?user_code=WDJB-MJHT",
              "the dialog shows the code and its QR code");
    }
    api.answer_with({answer(400, R"({"error":"authorization_pending"})")});
    model.SetWallClock(1'000'004);
    check(model.TakeSharingRequests().empty(), "it waits the interval before polling");
    model.SetWallClock(1'000'008);
    pump(model, service);
    check(model.state().account_link.state == "linking", "still waiting for approval");
    api.answer_with({answer(200, R"({"access_token":")" + token + R"(","token_type":"Bearer","expires_in":7776000})"),
                     answer(200, R"({"name":"Rina","trusted":true})")});
    model.SetWallClock(1'000'014);
    pump(model, service);
    check(model.state().account_link.state == "linked" && model.state().account_link.name == "Rina" &&
              model.state().account_link.trusted && model.state().navigation.modals.empty(),
          "approved: linked to Rina, the dialog closed");
    check(read_text(base / "community-recipes" / cm::kAccountTokenFileName) == token + "\n" &&
              (std::filesystem::status(base / "community-recipes" / cm::kAccountTokenFileName).permissions() &
               std::filesystem::perms::group_all) == std::filesystem::perms::none,
          "the token is kept privately by the worker");
    check(find_row(model, "settings-account-link")->value == "Rina", "Settings shows the account");
    check(model.ExportPreferences().share_as == playden::ShareAs::kNone,
          "linking from Settings after a cancelled account choice doesn't share as the account");
    open_share(model);
    check(find_row(model, "share-as")->value == "No name", "linking alone doesn't change who a share credits");
    activate(model, "share-as");
    // A name typed first stays for when the account isn't linked any more.
    activate(model, "share-as-name");
    model.SubmitEditorText("Rina R");
    activate(model, "share-as-account");
    check(model.ExportPreferences().share_as == playden::ShareAs::kAccount, "choosing the linked account makes it the default");
    model.action("circle");
    check(find_row(model, "share-as") && find_row(model, "share-as")->value == "Rina · Playden account",
          "the Share sheet shows the account");
    while (!model.state().navigation.modals.empty()) model.action("circle");
    {
        // Playden starts while the API doesn't answer: the saved link still counts (shares and reports keep crediting
        // it; the API says when a link ended), and it is checked again later for its name.
        playden::homebrew::SharingService again(config);
        again.Start();
        again.Wait();
        Simulation restarted("fixtures-not-used", "profile-not-used.json", ModelMode::Live);
        set_up(restarted, base);
        pump(restarted, again);
        api.answer_with({answer(503, R"({"error":"try_again","message":"busy"})")});
        restarted.SetWallClock(2'000'000);
        pump(restarted, again);
        const auto& link = restarted.state().account_link;
        check(link.state == "linked" && link.unverified, "a saved link the API couldn't check still counts as linked");
        restarted.SetWallClock(2'000'100);
        check(restarted.TakeSharingRequests().empty(), "it isn't checked again at once");
        api.answer_with({answer(200, R"({"name":"Rina","trusted":true})")});
        restarted.SetWallClock(2'000'300);
        pump(restarted, again);
        check(link.state == "linked" && !link.unverified && link.name == "Rina", "checked again later: its name");
        again.Shutdown();
    }
    // A report carries the account; an ended link is forgotten and said.
    SharingRequest report;
    report.kind = SharingRequestKind::Report;
    report.ticket = 99;
    report.use_account = true;
    report.game_id = kGame;
    report.report.store = "steam";
    report.report.store_id = "274520";
    report.report.name = "Darkwood";
    report.report.result = "works";
    report.report.origin = "manual";
    report.report.installed_on = "console";
    report.report.kit = report.report.firmware = report.report.playden_version = "unknown";
    api.answer_with({answer(401, R"({"error":"account_unlinked","message":"ended"})")});
    service.Submit(report);
    service.Wait();
    {
        std::lock_guard<std::mutex> lock(api.mutex);
        check(!api.requests.empty() && [&] {
            for (const auto& [name, value] : api.requests.back().headers)
                if (name == "X-Playden-Account") return value == token;
            return false;
        }(), "the report carries the account");
    }
    for (const auto& result : service.TakeResults()) model.ApplySharingResult(result);
    check(model.state().account_link.state.empty() && !std::filesystem::exists(base / "community-recipes" / cm::kAccountTokenFileName),
          "an ended link is forgotten");
    // With the account no longer linked, shares credit the typed name, and the row says why.
    open_share(model);
    check(find_row(model, "share-as")->value == "Rina R" &&
              find_row(model, "share-as")->subtitle == "Your Playden account isn't linked, so this credits Rina R.",
          "an unlinked account falls back to the typed name");
    check(model.ExportPreferences().share_as == playden::ShareAs::kAccount, "the default stays the account, for when it's linked again");
    while (!model.state().navigation.modals.empty()) model.action("circle");
    service.Shutdown();
}

// A prompt after a game, at a start where the library arrives late: it waits for the game's name, and the library's
// own selection doesn't close it.
void feedback_prompt_waits_for_library(const std::filesystem::path& base) {
    std::filesystem::create_directories(base);
    std::filesystem::permissions(base, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace);
    Simulation model("fixtures-not-used", "profile-not-used.json", ModelMode::Live);
    model.SetGameSettingsFile(base / "game-settings.json");
    model.SetSettingsData({{"playden-default", "Playden default", "Most games", "", {}}}, {});
    model.SetKitCapabilities(rt::KitCapabilitiesFromManifest({"dxvk"}, true));
    playden::homebrew::LiveKitInfo info;
    info.version = "0.12";
    model.SetKitInfo(info);
    model.SetWallClock(1'000'000);
    model.UpdateAccount({AuthState::SignedIn, "account", "Owner", {}, {}, false});
    model.UpdateVoteMemory({});
    playden::homebrew::LiveRuntimeGame runtime;
    runtime.game_id = kGame;
    runtime.status = "Ready";
    runtime.prepared = runtime.has_title = runtime.ready = true;
    runtime.launch_entry_id = "game";
    runtime.activity = playden::homebrew::TitleActivity::Stopped;
    model.UpdateRuntime(RuntimeState::Ready, {runtime});
    model.NotifySessionEnded(kGame, 30);
    check(model.state().navigation.modals.empty(), "the prompt waits while the library doesn't list the game");
    playden::GameSummary other;
    other.id = GameId::Steam(70);
    other.title = "A Short Hike";
    playden::GameSummary game;
    game.id = kGame;
    game.title = "Darkwood";
    check(model.UpdateLibrary({{other, game}, false, {}, false, true, 100}), "library delivered");
    check(model.view().surface == "feedback" && model.view().title == "Did Darkwood start?",
          "once the library lists the game, the prompt names it");
    playden::GameSummary third;
    third.id = GameId::Steam(400);
    third.title = "Celeste";
    // The selected game leaves the list: the library selects another and closes the dialogs about the old one.
    const auto selected = model.state().navigation.selected_game_id;
    std::vector<playden::GameSummary> without_selected;
    for (const auto& item : {third, other, game})
        if (item.id != selected) without_selected.push_back(item);
    check(selected.valid() && selected != kGame && model.UpdateLibrary({without_selected, false, {}, false, true, 200}),
          "library delivered again without the selected game");
    check(model.state().navigation.selected_game_id != selected, "the selection moved");
    check(model.view().surface == "feedback" && model.view().title == "Did Darkwood start?",
          "a library update that moves the selection leaves the prompt open");
    // A game no longer in the delivered library has nothing to ask about, and doesn't hold up what comes after.
    while (!model.state().navigation.modals.empty()) model.action("circle");
    model.NotifyStartFailed(GameId::Steam(999), "");
    check(model.state().navigation.modals.empty(), "a failure for a game the library no longer lists is dropped");
    model.NotifyStartFailed(kGame, "");
    check(!model.state().navigation.modals.empty() && model.state().navigation.modals.back().title == "Darkwood couldn't start",
          "and the next failure still shows");
}

// "Couldn't start" at a start where the library arrives late: it waits for the game's name, and the runtime's own
// "Could not start" for it says nothing more, but only for that failure.
void start_failed_waits_for_library(const std::filesystem::path& base) {
    std::filesystem::create_directories(base);
    std::filesystem::permissions(base, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace);
    Simulation model("fixtures-not-used", "profile-not-used.json", ModelMode::Live);
    model.SetGameSettingsFile(base / "game-settings.json");
    model.SetSettingsData({{"playden-default", "Playden default", "Most games", "", {}}}, {});
    model.SetWallClock(1'000'000);
    model.UpdateAccount({AuthState::SignedIn, "account", "Owner", {}, {}, false});
    model.NotifyStartFailed(kGame, "");
    check(model.state().navigation.modals.empty(), "a failure waits while the library doesn't list the game");
    playden::GameSummary game;
    game.id = kGame;
    game.title = "Darkwood";
    check(model.UpdateLibrary({{game}, false, {}, false, true, 100}), "library delivered");
    check(!model.state().navigation.modals.empty() && model.state().navigation.modals.back().title == "Darkwood couldn't start",
          "once listed, the dialog names it");
    while (!model.state().navigation.modals.empty()) model.action("circle");
    // Much later, a refusal for the same game is a new outcome: it is said.
    model.SetWallClock(1'000'000 + 600);
    playden::homebrew::LiveRuntimeGame runtime;
    runtime.game_id = kGame;
    runtime.status = "Ready";
    runtime.prepared = runtime.has_title = runtime.ready = true;
    runtime.play_detail = "Could not start: the runtime kit is missing";
    model.UpdateRuntime(RuntimeState::Ready, {runtime});
    check(!model.state().navigation.modals.empty() && model.state().navigation.modals.back().title == "Can't play",
          "a later Could not start isn't swallowed by an old failure");
}

// A start that failed after the handoff: one dialog that says so, with what to do, and no "Did it start?" for it.
void start_failed(const std::filesystem::path& base) {
    std::filesystem::create_directories(base);
    std::filesystem::permissions(base, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace);
    Simulation model("fixtures-not-used", "profile-not-used.json", ModelMode::Live);
    set_up(model, base);
    model.UpdateVoteMemory({});
    const auto top = [&model]() -> const playden::homebrew::ModalEntry& { return model.state().navigation.modals.back(); };
    // The session's prompt is up first (the runtime reports the end before its reason): the failure replaces it.
    model.NotifySessionEnded(kGame, 20);
    check(model.view().surface == "feedback", "the short session's prompt is up");
    model.NotifyStartFailed(kGame, "Wine exited with code 3");
    check(model.state().navigation.modals.size() == 1 && top().id == "start-failed" && top().title == "Darkwood couldn't start" &&
              contains(top().body, "Wine exited with code 3.") &&
              top().action_keys == std::vector<std::string>{"start-failed-settings", "start-failed-report", "start-failed-logs", "alert-ok"} &&
              model.state().navigation.focus_key == "start-failed-settings",
          "the failure replaces the question: Change settings first, then Report the problem, Open logs, OK");
    // The runtime's own "Could not start" for it raises no second message.
    playden::homebrew::LiveRuntimeGame runtime;
    runtime.game_id = kGame;
    runtime.status = "Ready";
    runtime.prepared = runtime.has_title = runtime.ready = true;
    runtime.launch_entry_id = "game";
    runtime.activity = playden::homebrew::TitleActivity::Stopped;
    runtime.play_detail = "Could not start: Wine exited with code 3";
    model.UpdateRuntime(RuntimeState::Ready, {runtime});
    check(std::none_of(model.state().notices.begin(), model.state().notices.end(),
                       [](const auto& notice) { return notice.message == "Can't play"; }) &&
              model.state().navigation.modals.size() == 1,
          "no Can't play notice as well");
    // Open logs goes over the dialog; Back returns to it.
    activate(model, "start-failed-logs");
    check(top().id == "logs", "Open logs shows the launch log");
    activate(model, "logs-back");
    check(top().id == "start-failed", "Back returns to the dialog");
    // Report the problem: straight to the report, as after "No, it didn't start", logs included; Circle closes it.
    activate(model, "start-failed-report");
    check(model.view().surface == "feedback" && find_row(model, "feedback-send") && !find_row(model, "feedback-started") &&
              find_row(model, "feedback-logs") && find_row(model, "feedback-logs")->selected,
          "Report the problem opens the report with no question first, logs included");
    check(model.view().legends.size() == 2 && model.view().legends[1] == "Circle Back", "its footer says Back");
    model.action("circle");
    check(model.state().navigation.modals.size() == 1 && top().id == "start-failed", "Circle goes back to the dialog");
    // Change settings from the report leaves the dialog too.
    activate(model, "start-failed-report");
    activate(model, "feedback-change-settings");
    check(top().id == "game-settings-list" &&
              std::none_of(model.state().navigation.modals.begin(), model.state().navigation.modals.end(),
                           [](const auto& modal) { return modal.id == "start-failed" || modal.id == "feedback"; }),
          "Change settings from the report closes the report and the dialog");
    while (!model.state().navigation.modals.empty()) model.action("circle");
    // Change settings opens the game's settings; nothing asks "Did it start?" afterwards.
    model.NotifyStartFailed(kGame, "");
    check(top().id == "start-failed" && contains(top().body, "It stopped before it got going."), "without a reason it says so plainly");
    activate(model, "start-failed-settings");
    check(top().id == "game-settings-list" && model.state().navigation.selected_game_id == kGame,
          "Change settings opens the settings list");
    while (!model.state().navigation.modals.empty()) model.action("circle");
    check(model.state().navigation.modals.empty(), "and no question follows");
}


std::vector<std::string> top_keys(const Simulation& model) {
    return model.state().navigation.modals.empty() ? std::vector<std::string>{} : model.state().navigation.modals.back().action_keys;
}

bool has(const std::vector<std::string>& keys, const std::string& key) {
    return std::find(keys.begin(), keys.end(), key) != keys.end();
}

// Community recipes off (features.txt): no service, so no catalog and no requests. A game that chose a recipe while
// they were on keeps it, read from the cache already on disk; every way to share, vote, report or pick another is gone.
void community_off(const std::filesystem::path& base) {
    std::filesystem::create_directories(base);
    std::filesystem::permissions(base, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace);
    const std::string bytes =
        read_text(std::filesystem::path(PLAYDEN_COMMUNITY_FIXTURES) / "recipes/steam/274520/120hz-left-stick-pointer.json");
    check(!bytes.empty(), "the recipe fixture reads");
    const std::string id = "community-steam-274520-120hz-left-stick-pointer";
    const std::string revision = cm::Sha256Hex(bytes);
    const auto root = base / "community-recipes";

    // No cache yet: the readers find nothing and make no folder.
    check(!playden::homebrew::CommunityRecipeCacheLoader(std::make_shared<cm::RecipeStore>(root))(revision),
          "no cache: the model's loader reads nothing");
    check(!playden::homebrew::CommunityRecipeBytes(std::make_shared<cm::RecipeStore>(root), false)(revision),
          "no cache: the runtime's reader reads nothing");
    check(!std::filesystem::exists(root), "and neither makes the community recipes folder");

    // On: the service stored the recipe and the player chose it for Darkwood.
    {
        cm::RecipeStore writer(root);
        std::string error;
        check(writer.Prepare(&error) && writer.WriteObjects({bytes}, &error), "the cache is written");
    }
    {
        Simulation on("fixtures-not-used", "profile-not-used.json", ModelMode::Live);
        set_up(on, base, false);
        playden::homebrew::CommunityCatalog catalog;
        catalog.release_tag = "r20261107.1";
        auto listed = offer(revision);
        listed.id = id;
        listed.title = "120 Hz, pointer on left stick";
        catalog.games[kGameRef] = {listed};
        on.UpdateCommunityCatalog(catalog);
        on.SetCommunityRecipeLoader(playden::homebrew::CommunityRecipeCacheLoader(std::make_shared<cm::RecipeStore>(root)));
        open_game_settings(on);
        activate(on, "preset-picker");
        activate(on, "recipe-" + revision);
        if (on.state().navigation.modals.back().id == "confirmation") activate(on, "recipe-switch-confirm");
        check(on.RecipeBaseFor(kGame) && on.RecipeBaseFor(kGame)->id == id, "on: the recipe is Darkwood's base");
    }
    // And shared it once: the record names its submission (Game settings would ask its state while on).
    {
        const auto file = base / "game-settings.json";
        auto text = read_text(file);
        const auto at = text.find("\"community_recipe\"");
        check(at != std::string::npos, "the saved record names its community recipe");
        text.insert(at, "\"shared_recipe\":{\"submission_id\":\"sub-1\",\"recipe_id\":\"" + id + "\"},");
        std::ofstream(file, std::ios::binary | std::ios::trunc) << text;
    }

    // Off from the start, as Playden starts without the line: no catalog, the cache-only loader.
    Simulation model("fixtures-not-used", "profile-not-used.json", ModelMode::Live);
    model.SetCommunityRecipesEnabled(false);
    check(!model.CommunityRecipesEnabled(), "the model has community recipes off");
    set_up(model, base, false);
    model.SetCommunityRecipeLoader(playden::homebrew::CommunityRecipeCacheLoader(std::make_shared<cm::RecipeStore>(root)));
    // No service sends either while off; given anyway, a catalog with a newer revision and the vote memory change
    // nothing: the gates are the model's, not the missing data's.
    {
        playden::homebrew::CommunityCatalog catalog;
        catalog.release_tag = "r20261108.1";
        auto newer = offer(kNewerRevision);
        newer.id = id;
        catalog.games[kGameRef] = {newer};
        model.UpdateCommunityCatalog(catalog);
        model.UpdateVoteMemory({});
    }
    const auto runtime_bytes = playden::homebrew::CommunityRecipeBytes(std::make_shared<cm::RecipeStore>(root), false);
    check(runtime_bytes(revision) == bytes && runtime_bytes(revision.substr(0, 32)) == bytes,
          "the runtime still reads the game's recipe from the cache, by revision and by prefix");

    // The game keeps its recipe: same base, same values, nothing falls back.
    check(model.RecipeBaseFor(kGame) && model.RecipeBaseFor(kGame)->id == id && model.RecipeBaseFor(kGame)->revision == revision,
          "off: the recipe stays Darkwood's base");
    check(model.RecipeFallbackFor(kGame).empty(), "and it applies");
    rt::EffectiveSettings values;
    check(model.EffectiveSettingsFor(kGame, &values) && values.values["resolution"].value == "3840x2160" &&
              values.values["refresh"].value == "120",
          "its values come from the cached recipe");

    // Game details and Game actions: no recipe tag, no Share feedback.
    check(model.action("details:274520"), "Darkwood's page opens");
    check(!find_row(model, "details-recipe"), "no Recipe available or Recipe in use tag");
    check(!find_row(model, "details-compat"), "nothing from the community's compatibility list");
    activate(model, "details-more");
    check(has(top_keys(model), "game-settings") && !has(top_keys(model), "share-feedback"), "Game actions has no Share feedback");

    // Game settings: the base by its title; no update, vote, shared recipe or Share recipe; Export as file stays.
    activate(model, "game-settings");
    check(contains(find_row(model, "preset-picker")->value, "120 Hz, pointer on left stick"), "the base shows by its title");
    {
        const auto record = model.state().game_settings.find(kGame);
        check(record != model.state().game_settings.end() && record->second.shared_recipe.submission_id == "sub-1",
              "the record keeps its shared recipe");
    }
    activate(model, "section-advanced");
    for (const char* key : {"recipe-update", "recipe-vote", "shared-recipe", "share-recipe"})
        check(!has(top_keys(model), key), "Game settings has no community row");
    check(has(top_keys(model), "export-recipe"), "Export as file stays");
    check(model.TakeSharingRequests().empty(), "opening Game settings asks nothing");
    check(!model.state().submissions.count("sub-1") || !model.state().submissions.at("sub-1").checking,
          "and its shared recipe isn't left Checking…");

    // Choose settings: Playden default, the recipe in use (focused) and the presets; no search, link or other recipes.
    activate(model, "preset-picker");
    check(!contains(model.view().subtitle, "Players share"), "the chooser doesn't describe community recipes");
    check(top_keys(model) == std::vector<std::string>{"preset-playden-default", "chooser-heading-community", "recipe-" + revision,
                                                      "chooser-heading-presets", "cancel"},
          "the chooser lists only the recipe in use besides Playden's");
    check(model.state().navigation.focus_key == "recipe-" + revision, "focus is on the recipe in use");
    // Back to Playden default: the recipe row goes, as it can't be chosen again.
    activate(model, "preset-playden-default");
    if (model.state().navigation.modals.back().id == "confirmation") activate(model, "recipe-switch-confirm");
    check(!model.RecipeBaseFor(kGame), "Playden default replaces the recipe");
    while (!model.state().navigation.modals.empty() && model.state().navigation.modals.back().id != "game-settings")
        model.action("circle");
    activate(model, "preset-picker");
    check(!has(top_keys(model), "chooser-heading-community") && !has(top_keys(model), "recipes-search"),
          "with no recipe in use, the chooser has no community section");
    check(!contains(model.view().subtitle, "community"), "and its text doesn't mention community recipes");
    while (!model.state().navigation.modals.empty()) model.action("circle");

    // After a game: no question. A start failure: no Report the problem.
    model.NotifySessionEnded(kGame, 600);
    check(model.state().navigation.modals.empty(), "no question after a game");
    model.NotifyStartFailed(kGame, "Wine exited with code 3");
    check(top_keys(model) == std::vector<std::string>{"start-failed-settings", "start-failed-logs", "alert-ok"},
          "the start failure offers Change settings, Open logs and OK");
    while (!model.state().navigation.modals.empty()) model.action("circle");

    // Settings: no Community section. Time passing asks nothing (no account check).
    model.action("circle");
    model.action("l1");
    check(model.state().navigation.route == playden::homebrew::Route::Settings, "Settings opens");
    for (const char* key : {"settings-ask-after-games", "settings-recipe-votes", "settings-share-as", "settings-account-link",
                            "settings-sharing-reset"})
        check(!find_row(model, key), "Settings has no Community row");
    model.SetWallClock(1'000'000 + 3600);
    check(model.TakeSharingRequests().empty() && !model.TakeCommunityRefreshRequest(), "nothing is asked of the services");
    check(!model.state().account_link.waiting && !model.state().account_link.checked, "no account check was queued");

    // The Install sheet of a game with community recipes in the catalog: no Settings row, no catalog refresh.
    {
        playden::GameSummary darkwood;
        darkwood.id = kGame;
        darkwood.title = "Darkwood";
        playden::GameSummary other;
        other.id = GameId::Steam(220);
        other.title = "Half-Life 2";
        check(model.UpdateLibrary({{darkwood, other}, false, {}, false, true, 101}), "a second game is in the library");
        playden::homebrew::CommunityCatalog catalog;
        catalog.release_tag = "r20261108.2";
        auto listed = offer(kNewerRevision);
        listed.id = "community-steam-220-sample";
        catalog.games[playden::GameRef::Steam(220)] = {listed};
        model.UpdateCommunityCatalog(catalog);
        while (!model.state().navigation.modals.empty()) model.action("circle");
        check(model.action("details:220"), "Half-Life 2's page opens");
        check(model.UpdateAppDetails(GameId::Steam(220), 300'000'000, 600'000'000, true), "its details arrive");
        model.UpdateVolumes({{"internal", "Console storage", true, 2'000'000'000, 1'000'000'000}}, "internal");
        activate(model, "details-install");
        check(model.state().navigation.modals.back().id == "install", "the Install sheet opens");
        check(!has(top_keys(model), "install-settings"), "the Install sheet has no Settings row");
        check(!model.TakeCommunityRefreshRequest(), "and asks for no catalog refresh");
    }
}

// Community recipes off and the game's recipe is gone from the cache: nothing can fetch it again, so the status row
// says so and offers Playden default, keeping the player's changes. On, the row stays a status (the catalog can help).
void community_off_recipe_missing(const std::filesystem::path& base) {
    std::filesystem::create_directories(base);
    std::filesystem::permissions(base, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace);
    const auto nothing_cached = [](const std::string&) -> std::optional<rt::CommunityRecipe> { return std::nullopt; };
    std::string resolution;
    {
        // On: the player chose the recipe for Darkwood and changed its resolution.
        Simulation on("fixtures-not-used", "profile-not-used.json", ModelMode::Live);
        set_up(on, base);
        open_game_settings(on);
        activate(on, "preset-picker");
        activate(on, "recipe-" + kRevision);
        if (on.state().navigation.modals.back().id == "confirmation") activate(on, "recipe-switch-confirm");
        check(on.RecipeBaseFor(kGame) && on.RecipeBaseFor(kGame)->id == kRecipeId, "on: the recipe is Darkwood's base");
        activate(on, "setting-resolution");
        std::string choice;
        for (const auto& row : on.view().rows)
            if (row.title.find("1280") != std::string::npos) choice = row.key;
        check(!choice.empty(), "another resolution can be chosen");
        activate(on, choice);
        resolution = on.state().game_settings.at(kGame).overrides.at("resolution");
    }
    {
        // On with the recipe missing: the row only says why, as before.
        Simulation on("fixtures-not-used", "profile-not-used.json", ModelMode::Live);
        set_up(on, base, false);
        on.SetCommunityRecipeLoader(nothing_cached);
        open_game_settings(on);
        const auto* status = find_row(on, "recipe-status");
        check(status && status->kind == playden::homebrew::RowKind::Status && !status->enabled &&
                  status->subtitle == rt::kCommunityRecipeRemoved,
              "on: the status row says why and offers nothing");
        activate(on, "recipe-status");
        check(on.state().game_settings.at(kGame).community_recipe.id == kRecipeId, "on: the row changes nothing");
    }

    // Off, with nothing in the cache: Playden's own recipe runs, and the row offers Playden default.
    Simulation model("fixtures-not-used", "profile-not-used.json", ModelMode::Live);
    model.SetCommunityRecipesEnabled(false);
    set_up(model, base, false);
    model.SetCommunityRecipeLoader(nothing_cached);
    check(!model.RecipeBaseFor(kGame) && !model.RecipeFallbackFor(kGame).empty(), "off: the missing recipe falls back");
    open_game_settings(model);
    const auto* status = find_row(model, "recipe-status");
    check(status && status->title == "Playden's own recipe is used" && status->kind == playden::homebrew::RowKind::Choice &&
              status->enabled && status->value == "Use Playden default" &&
              status->subtitle == "The recipe isn't on this console.",
          "off: the status row says the recipe isn't here and offers Playden default");
    activate(model, "recipe-status");
    if (model.state().navigation.modals.back().id == "confirmation") {
        check(contains(model.state().navigation.modals.back().title, "Playden default"), "the switch names Playden default");
        activate(model, "recipe-switch-confirm");
    }
    const auto& record = model.state().game_settings.at(kGame);
    check(record.community_recipe.id.empty() && (record.preset_id.empty() || record.preset_id == "playden-default"),
          "Playden default replaces the recipe");
    check(record.overrides.count("resolution") && record.overrides.at("resolution") == resolution, "the player's change stays");
    check(model.state().navigation.modals.back().id == "game-settings" && !find_row(model, "recipe-status"),
          "Game settings shows no status row afterwards");
    check(model.TakeSharingRequests().empty() && !model.TakeCommunityRefreshRequest(), "nothing is asked of the services");
}

int main() {
    const auto base = std::filesystem::canonical(std::filesystem::temp_directory_path()) /
                      ("playden-sharing-test-" + std::to_string(getpid()));
    std::error_code ignored;
    std::filesystem::remove_all(base, ignored);
    share_and_vote(base);
    community_keep(base / "keep");
    report_with_logs(base / "report");
    account_link(base / "account");
    feedback_prompt_waits_for_library(base / "prompt");
    start_failed(base / "start-failed");
    start_failed_waits_for_library(base / "start-failed-late");
    community_off(base / "off");
    community_off_recipe_missing(base / "off-missing");
    settings_only_base(base / "settings-only");
    std::filesystem::remove_all(base, ignored);
    std::puts("homebrew_sharing_test passed");
    return 0;
}
