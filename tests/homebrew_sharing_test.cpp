// homebrew_sharing_test.cpp - Sharing recipes and votes, live: the Share sheet hands the whole request
// to the sharing worker (no sharing ID in the model), the worker sends it with the same
// Idempotency-Key on every retry and the sharing ID only in Authorization, the answer is saved per
// game, a second share updates it, duplicates and errors read plainly, Game settings refreshes the
// shared recipe's state, and the vote prompt asks once per recipe revision after five minutes of play.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "community/install_secret.h"
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
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace {

using playden::GameId;
using playden::homebrew::AuthState;
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
const std::string kRecipeId = "community-274520-darkwood-120";
const std::string kRevision(64, 'c');
const std::string kNewerRevision(64, 'd');

std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

const playden::homebrew::ViewRow* find_row(const Simulation& model, const std::string& key) {
    for (const auto& row : model.view().rows)
        if (row.key == key) return &row;
    return nullptr;
}

void activate(Simulation& model, const std::string& key) {
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
    playden::steam::HttpResponse response;
    response.error = playden::steam::HttpError::kConnectionFailed;
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
    recipe.app_id = 274520;
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

// A live model with Darkwood installed and prepared, its title stopped, a kit and one community recipe.
void set_up(Simulation& model, const std::filesystem::path& base) {
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
    playden::homebrew::CommunityCatalog catalog;
    catalog.release_tag = "r20261022.1";
    catalog.games[kGame] = {offer(kRevision)};
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
                     answer(201, R"({"id":"sub-1","recipe_id":"community-274520-darkwood-4k","state":"in-review",)"
                                 R"("pr_url":"https://github.com/leolobato/playden-ps5-recipes/pull/12"})")});
    std::vector<SharingRequest> sent;
    pump(model, service, &sent);
    check(sent.size() == 1 && sent[0].kind == SharingRequestKind::Submit, "Send hands one share to the worker");
    const auto& request = sent[0].submission;
    check(request.app_id == 274520 && request.title == "Darkwood 4K" && request.name == "Todor" && request.kit == "0.12" &&
              request.firmware == "10.01" && request.playden_version == "0.6.0" && !request.based_on &&
              request.choice == cm::ShareChoice::kNone && request.submission_id.empty() && request.idempotency_key.empty(),
          "the request carries the recipe, title, name and versions; no base, no choice, no key yet");
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
    check(shared.submission_id == "sub-1" && shared.recipe_id == "community-274520-darkwood-4k", "the share is saved per game");
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
    api.answer_with({answer(200, R"({"id":"sub-1","state":"published","recipe_id":"community-274520-darkwood-4k",)"
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
    check(contains(read_text(base / "community-recipes" / cm::kVoteStateFileName), kRevision), "and saved by the worker");

    // Duplicate of another console's pull request: no vote, its QR code.
    open_share(model);
    if (find_row(model, "share-title")->value == "Optional") {
        activate(model, "share-title");
        model.SubmitEditorText("Darkwood 4K");
    }
    api.answer_with({answer(200, R"({"state":"duplicate","recipe_id":"community-274520-x","pr_url":"https://github.com/leolobato/playden-ps5-recipes/pull/9"})")});
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
    activate(model, "feedback-send-confirm");
    check(model.state().reports_explained && model.ExportPreferences().reports_explained, "and only the first time");
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
    // A newer revision: new settings ask again; Not now twice, then no more.
    auto catalog = model.state().community;
    catalog.games[kGame] = {offer(kNewerRevision)};
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
    later.games[kGame] = {offer(third)};
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
    activate(model, "recipe-vote");
    activate(model, "vote-up");
    activate(model, "vote-send");
    api.answer_with({answer(400, R"({"error":"rule","message":"no"})")});
    pump(model, service, &sent);
    check(sent.size() == 1 && sent[0].vote.revision == kNewerRevision, "the vote goes to the prepared revision");
    check(find_row(model, "recipe-vote") && find_row(model, "recipe-vote")->value == "👎 Slow", "a failed send changes nothing");
    // The failure waits in a dialog until Game settings closes.
    while (!model.state().navigation.modals.empty() && model.state().navigation.modals.back().title != "The vote wasn't sent")
        model.action("circle");
    check(!model.state().navigation.modals.empty(), "a failed send says so");
    open_game_settings(model);
    activate(model, "recipe-vote");
    check(model.view().surface == "vote-recipe", "Your vote opens again after a failed send");
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
        api.answer_with({answer(200, R"({"id":"sub-1","state":"published","recipe_id":"community-274520-darkwood-4k",)"
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
    newest.games[kGame] = {offer(std::string(64, 'e'))};
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
    report.controls = {"dualsense"};
    report.kit = "0.19";
    report.firmware = "13.60";
    report.playden_version = "0.10.0";
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
    check(!contains(zip, "leoaccount") && !contains(zip, "76561198000000001") && contains(zip, "<steam-user>"),
          "the account name and SteamID are removed");
    const std::string body(api.requests[1].body.begin(), api.requests[1].body.end());
    check(contains(body, "\"attachment\":\"att_9\"") && contains(body, "\"versions\":[[") && !contains(body, "leoaccount"),
          "the report names the logs and carries the versions");
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
    api.answer_with({answer(200, R"({"device_code":"dev_code_0123456789","user_code":"WDJB-MJHT","verification_uri_complete":"https://account.playden.club/device?user_code=WDJB-MJHT","expires_in":900,"interval":5})")});
    check(model.action("focus:settings-account-link") && model.action("cross"), "Playden account opens linking");
    pump(model, service);
    {
        const auto& dialog = model.state().navigation.modals.back();
        check(dialog.title == "Link a Playden account" && contains(dialog.body, "WDJB-MJHT") &&
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
    activate(model, "share-as-account");
    check(model.ExportPreferences().share_as == playden::ShareAs::kAccount, "choosing the linked account makes it the default");
    model.action("circle");
    check(find_row(model, "share-as") && find_row(model, "share-as")->value == "Rina · Playden account",
          "the Share sheet shows the account");
    while (!model.state().navigation.modals.empty()) model.action("circle");
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
    service.Shutdown();
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
    std::filesystem::remove_all(base, ignored);
    std::puts("homebrew_sharing_test passed");
    return 0;
}
