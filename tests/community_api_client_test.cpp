// community_api_client_test.cpp - The Playden API's sharing and voting calls against a fake fetch:
// exact request bodies, headers, every answer's shape and error mapping, local refusals, and that the
// install secret never reaches a body, a URL or a result. Built twice, without dev endpoints (plain
// http refused, nothing fetched) and with them (plain http to the configured API only).
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "community/api_client.h"
#include "community/install_secret.h"
#include "steam/backend/json.h"

#include <strings.h>

#include <cstdio>
#include <string>
#include <vector>

using namespace playden;
using namespace playden::community;

namespace {

int failures = 0;
#define CHECK(cond)                                                                 \
  do {                                                                              \
    if (!(cond)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                                   \
    }                                                                               \
  } while (0)

const std::string kSecret = "0123456789abcdef0123456789abcdef";
const std::string kRevision(64, 'a');
const std::string kRevision2(64, 'b');
std::string fixtures;

std::string Slurp(const std::string& path) {
  std::string text;
  if (std::FILE* file = std::fopen(path.c_str(), "rb")) {
    char buffer[1024];
    std::size_t count = 0;
    while ((count = std::fread(buffer, 1, sizeof buffer, file)) > 0) text.append(buffer, count);
    std::fclose(file);
  }
  return text;
}

struct FakeApi {
  std::vector<steam::HttpRequest> requests;
  std::vector<steam::HttpResponse> answers;  // in order; the last repeats
  update::Fetch Fetch() {
    return [this](const steam::HttpRequest& request, const std::atomic<bool>&) {
      requests.push_back(request);
      if (answers.empty()) return steam::HttpResponse{};
      const auto answer = answers.size() > 1 ? answers.front() : answers.back();
      if (answers.size() > 1) answers.erase(answers.begin());
      return answer;
    };
  }
};

steam::HttpResponse Answer(long status, const std::string& body,
                           std::vector<std::pair<std::string, std::string>> headers = {}) {
  steam::HttpResponse response;
  response.status_code = status;
  response.body.assign(body.begin(), body.end());
  response.headers = std::move(headers);
  return response;
}

std::string HeaderOf(const steam::HttpRequest& request, const char* name) {
  for (const auto& [key, value] : request.headers)
    if (strcasecmp(key.c_str(), name) == 0) return value;
  return {};
}

bool HasHeader(const steam::HttpRequest& request, const char* name) {
  for (const auto& [key, value] : request.headers)
    if (strcasecmp(key.c_str(), name) == 0) return true;
  return false;
}

std::string Body(const steam::HttpRequest& request) { return std::string(request.body.begin(), request.body.end()); }

// The secret anywhere in a request but its Authorization header.
bool SecretOutsideAuthorization(const steam::HttpRequest& request) {
  if (request.url.find(kSecret) != std::string::npos || Body(request).find(kSecret) != std::string::npos) return true;
  for (const auto& [key, value] : request.headers)
    if (strcasecmp(key.c_str(), "authorization") != 0 && value.find(kSecret) != std::string::npos) return true;
  return false;
}

runtime::CommunityRecipe ExportRecipe() {
  runtime::CommunityRecipe community;
  std::string reason;
  const bool parsed = runtime::ParseCommunityRecipe(
      Slurp(fixtures + "/hostile/export-no-slug-no-title.json"), &community, &reason);
  CHECK(parsed);
  return community;
}

SubmissionRequest MinimalRequest() {
  SubmissionRequest request;
  request.game = GameRef::Steam(274520);
  request.game_name = "Darkwood";
  request.recipe_json = RecipeJsonForSubmission(ExportRecipe(), "120 Hz, pointer on left stick");
  request.title = "120 Hz, pointer on left stick";
  request.kit = "0.12";
  request.firmware = "13.60";
  request.playden_version = "0.6.0";
  request.idempotency_key = "key-0001";
  return request;
}

std::string TrimmedRecipe(const std::string& text) {
  std::string out = text;
  while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
  return out;
}

ApiContext Context(FakeApi* api) {
  ApiContext context;
  context.fetch = api->Fetch();
  return context;
}

// Every key Playden's export can write is one the API accepts: a recipe with every optional field
// set, serialized, checked key by key at the top level and in community.
void ExportKeysAccepted() {
  runtime::CommunityRecipe community;
  std::string reason;
  CHECK(runtime::ParseCommunityRecipe(
      Slurp(fixtures + "/recipes/steam/274520/120hz-left-stick-pointer.json"), &community, &reason));
  community.recipe.detected = "unity";
  community.recipe.requirements["audio"] = "ps5";
  runtime::RegistryValue value;
  value.key = "Software\\Wine\\AppDefaults\\Darkwood.exe\\X11 Driver";
  value.name = "Decorated";
  value.data = "\"N\"";
  community.recipe.registry.push_back(value);
  community.recipe.locked.insert("refresh");
  community.recipe.launch_entries.front().in_install_root = true;
  community.min_kit = "0.12";
  community.contributors = {"someone"};
  community.base_repository = "https://github.com/x/prospero-win-profiles";
  community.base_path = "profiles/darkwood.ini";
  community.base_revision = std::string(40, 'c');
  community.made_on = "imported";  // shared from an imported copy with the Steam app ID
  const std::string text = runtime::SerializeCommunityRecipe(community);
  const auto doc = steam::json::Parse(text);
  CHECK(doc && doc->is_object());
  if (!doc) return;
  std::size_t top = 0;
  for (const auto& [key, item] : doc->object()) {
    ++top;
    if (!AcceptedRecipeKeys().count(key)) std::fprintf(stderr, "export writes a key the API refuses: %s\n", key.c_str());
    CHECK(AcceptedRecipeKeys().count(key));
  }
  CHECK(top >= 19);  // every optional emitter fired
  const auto* meta = doc->Find("community");
  CHECK(meta && meta->is_object());
  if (meta)
    for (const auto& [key, item] : meta->object()) {
      if (!AcceptedCommunityKeys().count(key))
        std::fprintf(stderr, "export writes a community key the API refuses: %s\n", key.c_str());
      CHECK(AcceptedCommunityKeys().count(key));
    }
  CHECK(meta && meta->Find("base") && meta->Find("min_kit"));
  // The game object replaced app_id and steam_appid (the API's RECIPE_KEYS).
  CHECK(!AcceptedRecipeKeys().count("store") && AcceptedRecipeKeys().count("game") && AcceptedRecipeKeys().count("made_on") &&
        !AcceptedRecipeKeys().count("app_id") && !AcceptedRecipeKeys().count("steam_appid"));

  // Made ready to share: titled, no locks, and accepted locally.
  const std::string shared = RecipeJsonForSubmission(community, "A title");
  runtime::CommunityRecipe back;
  CHECK(runtime::ParseCommunityRecipe(shared, &back, &reason));
  CHECK(back.title == "A title" && back.recipe.locked.empty());
  SubmissionRequest request = MinimalRequest();
  request.recipe_json = shared;
  request.title = "A title";
  CHECK(SubmissionRequestProblem(request).empty());
  // The same with its locks kept is refused before sending.
  request.recipe_json = text;
  CHECK(SubmissionRequestProblem(request).find("locks") != std::string::npos);
}

void SubmissionBodies() {
  auto request = MinimalRequest();
  const std::string recipe = TrimmedRecipe(request.recipe_json);
  CHECK(SubmissionBody(request) ==
        "{\"game\":{\"store\":\"steam\",\"key\":\"274520\",\"name\":\"Darkwood\"},\"recipe\":" + recipe +
            ",\"title\":\"120 Hz, pointer on left stick\",\"kit\":\"0.12\",\"firmware\":\"13.60\","
            "\"playden_version\":\"0.6.0\"}");
  request.name = "Todor \"TP\"";
  request.based_on = RecipeRef{"community-steam-274520-120hz-left-stick-pointer", kRevision};
  request.choice = ShareChoice::kUpdate;
  request.submission_id = "sub_123";
  CHECK(SubmissionBody(request) ==
        "{\"game\":{\"store\":\"steam\",\"key\":\"274520\",\"name\":\"Darkwood\"},\"recipe\":" + recipe +
            ",\"title\":\"120 Hz, pointer on left stick\",\"name\":\"Todor \\\"TP\\\"\",\"based_on\":{\"recipe_id\":"
            "\"community-steam-274520-120hz-left-stick-pointer\",\"revision\":\"" +
            kRevision +
            "\"},\"choice\":\"update\",\"submission_id\":\"sub_123\",\"kit\":\"0.12\",\"firmware\":\"13.60\","
            "\"playden_version\":\"0.6.0\"}");
  request.choice = ShareChoice::kNew;
  CHECK(SubmissionBody(request).find("\"choice\":\"new\"") != std::string::npos);
  // The body is JSON, with the recipe as an object.
  const auto doc = steam::json::Parse(SubmissionBody(request));
  CHECK(doc && doc->Find("recipe") && doc->Find("recipe")->is_object());
  CHECK(SubmissionBody(request).find(kSecret) == std::string::npos);
  CHECK(SubmissionBody(request).find("app_id") == std::string::npos);

  // An imported game without a Steam app ID shares under the name key Playden made from its name.
  auto community = ExportRecipe();
  community.game = GameRef{"imported", NameKey("Night Sky: Director's Cut")};
  community.game_name = "Night Sky: Director's Cut";
  community.recipe.id = runtime::CommunityRecipeIdFor(community.game, "");
  auto imported = MinimalRequest();
  imported.game = community.game;
  imported.game_name = community.game_name;
  imported.recipe_json = RecipeJsonForSubmission(community, "Arrow keys");
  CHECK(SubmissionRequestProblem(imported).empty());
  const auto body = SubmissionBody(imported);
  CHECK(body.rfind("{\"game\":{\"store\":\"imported\",\"key\":\"night-sky-director-s-cut\",\"name\":"
                   "\"Night Sky: Director's Cut\"},\"recipe\":{", 0) == 0);
  CHECK(body.find("\"recipe_id\": \"community-imported-night-sky-director-s-cut\"") != std::string::npos);
  // The recipe's own game must be the share's.
  imported.game = GameRef::Steam(274520);
  CHECK(SubmissionRequestProblem(imported).find("another game") != std::string::npos && SubmissionBody(imported).empty());
}

void LocalRefusals() {
  FakeApi api;
  const auto context = Context(&api);
  std::atomic<bool> cancel{false};
  const auto refused = [&](SubmissionRequest request, const std::string& secret = kSecret) {
    const std::size_t before = api.requests.size();
    const auto result = SubmitRecipe(context, secret, request, cancel);
    const bool refused_locally = result.outcome == SubmissionOutcome::kError && result.error.code == "invalid_request" &&
                                 result.error.http_status == 0 && !result.error.message.empty();
    // Refused: nothing sent. Accepted: sent once.
    CHECK(api.requests.size() == before + (refused_locally ? 0 : 1));
    return refused_locally;
  };
  CHECK(refused(MinimalRequest(), ""));
  CHECK(refused(MinimalRequest(), "0123456789ABCDEF0123456789ABCDEF"));
  CHECK(refused(MinimalRequest(), kSecret + "0"));
  auto request = MinimalRequest();
  request.choice = ShareChoice::kUpdate;  // no based_on
  CHECK(refused(request));
  request = MinimalRequest();
  request.title.clear();  // optional: left out of the body
  CHECK(SubmissionBody(MinimalRequest()).find("},\"title\":") != std::string::npos);  // a title is sent when set
  CHECK(!refused(request) && SubmissionBody(request).find("},\"title\":") == std::string::npos);
  request.title = std::string(61, 'x');
  CHECK(refused(request));
  request.title = std::string(60, 'x');
  CHECK(!refused(request));
  request.title = "tab\there";
  CHECK(refused(request));
  request.title = "\xc3\xa9t\xc3\xa9";  // "été": printable
  CHECK(!refused(request));
  request.title = "bad \xff utf8";
  CHECK(refused(request));
  // Zero-width, bidi, ideographic spaces and private use: the API's checker refuses them.
  for (const char* bad : {"A\u200Bb", "\u3000Title\u3000", "Title\u202E", "\u2066x\u2069", "\uE000", "\u00ADx"}) {
    request.title = bad;
    CHECK(refused(request));
    CHECK(!IsSubmissionText(bad, 1, kSubmissionTitleMaxCharacters));
  }
  request.title = "Darkwood 4K";
  request = MinimalRequest();
  request.name = std::string(41, 'n');
  CHECK(refused(request));
  request.name = std::string(40, 'n');
  CHECK(!refused(request));
  request.name = "\u3000Leo\u3000";
  CHECK(refused(request));
  request = MinimalRequest();
  request.firmware.clear();
  CHECK(refused(request));
  request = MinimalRequest();
  request.idempotency_key = "short";
  CHECK(refused(request));
  request.idempotency_key = "has space ok";
  CHECK(refused(request));
  // The share's game must be the recipe's, a store and its key, with a printable name.
  request = MinimalRequest();
  request.game = GameRef::Steam(10);
  CHECK(refused(request));
  request = MinimalRequest();
  request.game = GameRef{};
  CHECK(refused(request));
  request = MinimalRequest();
  request.game = GameRef{"imported", "darkwood"};
  CHECK(refused(request));
  for (const char* name : {"", "Dark\nwood", "\u3000", "Dark\u2028wood"}) {
    request = MinimalRequest();
    request.game_name = name;
    CHECK(refused(request));
  }
  request = MinimalRequest();
  request.game_name = "\uff24\uff41\uff52\uff4b\u3000wood";  // a full-width name and space are fine
  CHECK(SubmissionRequestProblem(request).empty());
  request = MinimalRequest();
  request.based_on = RecipeRef{"community-steam-274520-x", "abc"};
  CHECK(refused(request));
  request = MinimalRequest();
  request.submission_id = "../../x";
  CHECK(refused(request));
  // A key the API refuses.
  request = MinimalRequest();
  request.recipe_json.insert(1, "\"store\": \"steam\",");
  CHECK(refused(request));
  request = MinimalRequest();
  const auto community_at = request.recipe_json.find("\"community\": {");
  request.recipe_json.insert(community_at + 14, "\"game\": \"x\",");
  CHECK(refused(request));
  // Larger than 16 KB.
  request = MinimalRequest();
  auto big = ExportRecipe();
  big.notes = std::string(17000, 'n');
  request.recipe_json = RecipeJsonForSubmission(big, "t");
  CHECK(refused(request));
  // Not a recipe at all.
  request.recipe_json = "{}";
  CHECK(refused(request));
}

void SubmitAnswers() {
  FakeApi api;
  const auto context = Context(&api);
  std::atomic<bool> cancel{false};
  auto request = MinimalRequest();

  api.answers = {Answer(201, R"({"id":"s1","recipe_id":"community-steam-274520","state":"provisioning","pr_url":null})")};
  auto result = SubmitRecipe(context, kSecret, request, cancel);
  CHECK(result.outcome == SubmissionOutcome::kCreated);
  CHECK(result.id == "s1" && result.recipe_id == "community-steam-274520" && result.state == "provisioning" &&
        result.pr_url.empty());
  CHECK(api.requests.size() == 1);
  const auto& sent = api.requests.back();
  CHECK(sent.method == steam::HttpMethod::kPost);
  CHECK(sent.url == "https://api.playden.club/v1/submissions");
  CHECK(HeaderOf(sent, "Authorization") == "Bearer " + kSecret);
  CHECK(HeaderOf(sent, "Idempotency-Key") == "key-0001");
  CHECK(HeaderOf(sent, "Content-Type") == "application/json");
  CHECK(Body(sent) == SubmissionBody(request));
  CHECK(!sent.allow_plain_http);
  CHECK(!SecretOutsideAuthorization(sent));

  api.answers = {Answer(201, R"({"id":"s2","recipe_id":"community-steam-274520-darkwood-120","state":"in-review",)"
                             R"("pr_url":"https://github.com/leolobato/playden-ps5-recipes/pull/7"})")};
  result = SubmitRecipe(context, kSecret, request, cancel);
  CHECK(result.outcome == SubmissionOutcome::kCreated && result.state == "in-review" &&
        result.pr_url == "https://github.com/leolobato/playden-ps5-recipes/pull/7");

  api.answers = {Answer(200, R"({"state":"duplicate","recipe_id":"community-steam-274520-120hz-left-stick-pointer","revision":")" +
                                 kRevision + "\"}")};
  result = SubmitRecipe(context, kSecret, request, cancel);
  CHECK(result.outcome == SubmissionOutcome::kDuplicatePublished && result.revision == kRevision &&
        result.recipe_id == "community-steam-274520-120hz-left-stick-pointer");

  api.answers = {Answer(200, R"({"state":"duplicate","recipe_id":"community-steam-274520-x","pr_url":"https://github.com/p/r/pull/9"})")};
  result = SubmitRecipe(context, kSecret, request, cancel);
  CHECK(result.outcome == SubmissionOutcome::kDuplicateInReview && result.revision.empty() &&
        result.pr_url == "https://github.com/p/r/pull/9");

  // Answers that don't fit the contract.
  for (const auto& bad : std::vector<steam::HttpResponse>{
           Answer(201, R"({"recipe_id":"community-steam-274520","state":"provisioning","pr_url":null})"),
           Answer(201, R"({"id":"s1","recipe_id":"community-steam-274520","state":"published","pr_url":null})"),
           Answer(201, R"({"id":"s/1","recipe_id":"community-steam-274520","state":"provisioning","pr_url":null})"),
           Answer(201, R"({"id":"s1","recipe_id":"steam-274520-auto","state":"provisioning","pr_url":null})"),
           Answer(201, R"({"id":"s1","recipe_id":"community-steam-274520","state":"in-review","pr_url":"javascript:x"})"),
           Answer(200, R"({"state":"duplicate","recipe_id":"community-steam-274520-x"})"),
           Answer(200, R"({"state":"duplicate","recipe_id":"community-steam-274520-x","revision":"short"})"),
           Answer(200, R"({"state":"created","recipe_id":"community-steam-274520-x"})"),
           Answer(200, "not json"),
       }) {
    api.answers = {bad};
    result = SubmitRecipe(context, kSecret, request, cancel);
    CHECK(result.outcome == SubmissionOutcome::kError && result.error.code == "bad_response");
    CHECK(result.id.empty() && result.recipe_id.empty());
  }

  // Errors.
  api.answers = {Answer(400, R"({"error":"choice_required","message":"Choose update or new."})")};
  result = SubmitRecipe(context, kSecret, request, cancel);
  CHECK(result.outcome == SubmissionOutcome::kError && result.error.http_status == 400 &&
        result.error.code == "choice_required" && result.error.message == "Choose update or new." &&
        result.error.retry_after == 0);
  api.answers = {Answer(429, R"({"error":"rate_limited","message":"Slow down."})", {{"retry-after", "30"}})};
  result = SubmitRecipe(context, kSecret, request, cancel);
  CHECK(result.error.http_status == 429 && result.error.code == "rate_limited" && result.error.retry_after == 30);
  api.answers = {Answer(429, R"({"error":"rate_limited","message":"x"})", {{"Retry-After", "Wed, 21 Oct 2026 07:28:00 GMT"}})};
  result = SubmitRecipe(context, kSecret, request, cancel);
  CHECK(result.error.code == "rate_limited" && result.error.retry_after == 0);
  api.answers = {Answer(503, R"({"error":"try_again","message":"Try again."})", {{"Retry-After", "5"}})};
  result = SubmitRecipe(context, kSecret, request, cancel);
  CHECK(result.error.http_status == 503 && result.error.code == "try_again" && result.error.retry_after == 5);
  api.answers = {Answer(413, R"({"error":"too_large","message":"Recipe over 16 KB."})")};
  result = SubmitRecipe(context, kSecret, request, cancel);
  CHECK(result.error.code == "too_large");
  api.answers = {Answer(409, R"({"error":"not_yours","message":"x"})")};
  CHECK(SubmitRecipe(context, kSecret, request, cancel).error.code == "not_yours");
  api.answers = {Answer(500, "<html>oops</html>")};
  result = SubmitRecipe(context, kSecret, request, cancel);
  CHECK(result.error.http_status == 500 && result.error.code == "http_500" && result.error.message == "status 500");
  // A message that echoes the secret: the whole answer is refused, nothing of it kept.
  api.answers = {Answer(401, "{\"error\":\"unauthorized\",\"message\":\"bad Bearer " + kSecret + " or " + kSecret +
                                 "\\n" + std::string(400, 'x') + "\"}")};
  result = SubmitRecipe(context, kSecret, request, cancel);
  CHECK(result.error.code == "bad_response" && result.error.http_status == 401);
  CHECK(result.error.message.find(kSecret) == std::string::npos);
  // Another credential in a message, with control characters, too long: cleaned.
  api.answers = {Answer(401, "{\"error\":\"unauthorized\",\"message\":\"bad Bearer abcdefgh12345678 or" +
                                 std::string("\\n") + std::string(400, 'x') + "\"}")};
  result = SubmitRecipe(context, kSecret, request, cancel);
  CHECK(result.error.code == "unauthorized");
  CHECK(result.error.message.find("abcdefgh12345678") == std::string::npos);
  CHECK(result.error.message.find("<redacted>") != std::string::npos);
  CHECK(result.error.message.find('\n') == std::string::npos);
  CHECK(result.error.message.size() <= 220);
  // The secret across the cut: no piece of it survives.
  for (std::size_t filler = 150; filler <= 200; ++filler) {
    api.answers = {Answer(401, "{\"error\":\"unauthorized\",\"message\":\"" + std::string(filler, 'x') + kSecret + "\"}")};
    result = SubmitRecipe(context, kSecret, request, cancel);
    for (std::size_t at = 0; at + 8 <= kSecret.size(); ++at)
      CHECK(result.error.message.find(kSecret.substr(at, 8)) == std::string::npos);
  }

  // Transport failures, a redirect (never followed), cancel.
  steam::HttpResponse failed;
  failed.error = steam::HttpError::kTimeout;
  api.answers = {failed};
  result = SubmitRecipe(context, kSecret, request, cancel);
  CHECK(result.error.code == "unreachable" && result.error.http_status == 0);  // nothing connected
  failed.timing.connected = true;
  api.answers = {failed};
  result = SubmitRecipe(context, kSecret, request, cancel);
  CHECK(result.error.code == "network" && result.error.http_status == 0);  // connected, the answer was lost
  CHECK(!api.requests.empty() && api.requests.back().connect_timeout_ms == kApiConnectTimeoutMs);
  api.requests.clear();
  api.answers = {Answer(307, "", {{"Location", "https://evil.example/v1/submissions"}})};
  result = SubmitRecipe(context, kSecret, request, cancel);
  CHECK(result.error.code == "redirect" && result.error.http_status == 307);
  CHECK(api.requests.size() == 1);
  std::atomic<bool> cancelled{true};
  api.requests.clear();
  result = SubmitRecipe(context, kSecret, request, cancelled);
  CHECK(result.error.code == "cancelled" && api.requests.empty());
  // No fetch at all.
  ApiContext offline;
  CHECK(SubmitRecipe(offline, kSecret, request, cancel).error.code == "network");
}

// A retry of one share attempt sends the same key; a new attempt gets a new one.
void IdempotencyKeys() {
  FakeApi api;
  const auto context = Context(&api);
  std::atomic<bool> cancel{false};
  const auto key = NewIdempotencyKey();
  CHECK(key && IsIdempotencyKey(*key) && key->size() == 32);
  const auto other = NewIdempotencyKey();
  CHECK(other && *other != *key);
  auto request = MinimalRequest();
  request.idempotency_key = *key;
  steam::HttpResponse failed;
  failed.error = steam::HttpError::kConnectionFailed;
  failed.timing.connected = true;
  api.answers = {failed, Answer(201, R"({"id":"s1","recipe_id":"community-steam-274520","state":"provisioning","pr_url":null})")};
  CHECK(SubmitRecipe(context, kSecret, request, cancel).error.code == "network");
  CHECK(SubmitRecipe(context, kSecret, request, cancel).outcome == SubmissionOutcome::kCreated);
  CHECK(api.requests.size() == 2);
  CHECK(HeaderOf(api.requests[0], "Idempotency-Key") == *key);
  CHECK(HeaderOf(api.requests[1], "Idempotency-Key") == *key);
  CHECK(Body(api.requests[0]) == Body(api.requests[1]));
  CHECK(IsIdempotencyKey("abcdefgh") && IsIdempotencyKey("A_b-0123") && !IsIdempotencyKey("abcdefg") &&
        !IsIdempotencyKey(std::string(129, 'a')) && !IsIdempotencyKey("abcd/efgh"));
}

void Submissions() {
  FakeApi api;
  const auto context = Context(&api);
  std::atomic<bool> cancel{false};
  api.answers = {Answer(200, R"({"id":"s1","state":"published","recipe_id":"community-steam-274520-a","pr_url":"https://github.com/p/r/pull/1"})")};
  auto status = GetSubmission(context, "s1", cancel);
  CHECK(status.ok && status.id == "s1" && status.state == "published" && status.recipe_id == "community-steam-274520-a" &&
        status.pr_url == "https://github.com/p/r/pull/1");
  CHECK(api.requests.size() == 1);
  CHECK(api.requests.back().method == steam::HttpMethod::kGet);
  CHECK(api.requests.back().url == "https://api.playden.club/v1/submissions/s1");
  CHECK(!HasHeader(api.requests.back(), "Authorization"));
  CHECK(api.requests.back().body.empty());
  for (const char* state : {"provisioning", "in-review", "closed", "duplicate"}) {
    api.answers = {Answer(200, std::string(R"({"id":"s1","state":")") + state + R"(","recipe_id":"community-steam-274520-a","pr_url":null})")};
    status = GetSubmission(context, "s1", cancel);
    CHECK(status.ok && status.state == state && status.pr_url.empty());
  }
  api.answers = {Answer(200, R"({"id":"s1","state":"merged","recipe_id":"community-steam-274520-a","pr_url":null})")};
  CHECK(GetSubmission(context, "s1", cancel).error.code == "bad_response");
  api.answers = {Answer(200, R"({"id":"s2","state":"closed","recipe_id":"community-steam-274520-a","pr_url":null})")};
  CHECK(GetSubmission(context, "s1", cancel).error.code == "bad_response");
  api.answers = {Answer(404, R"({"error":"unknown_submission","message":"No such submission."})")};
  status = GetSubmission(context, "s1", cancel);
  CHECK(!status.ok && status.error.http_status == 404 && status.error.code == "unknown_submission");
  api.requests.clear();
  for (const char* bad : {"", "../v1/votes", "s1?x=1", "a b"}) CHECK(GetSubmission(context, bad, cancel).error.code == "invalid_request");
  CHECK(api.requests.empty());
}

// An answer holding the sharing ID anywhere is refused whole: no field of it reaches a result.
void SecretEchoes() {
  FakeApi api;
  const auto context = Context(&api);
  std::atomic<bool> cancel{false};
  const auto request = MinimalRequest();
  const auto clean = [](const SubmissionResult& result) {
    return result.outcome == SubmissionOutcome::kError && result.error.code == "bad_response" && result.id.empty() &&
           result.recipe_id.empty() && result.state.empty() && result.pr_url.empty() && result.revision.empty() &&
           result.error.message.find(kSecret) == std::string::npos;
  };
  const std::string pr = "https://github.com/p/r/pull/1";
  for (const std::string& body : {
           R"({"id":")" + kSecret + R"(","recipe_id":"community-steam-274520-a","state":"in-review","pr_url":null})",
           R"({"id":"s1","recipe_id":"community-steam-274520-)" + kSecret + R"(","state":"in-review","pr_url":null})",
           R"({"id":"s1","recipe_id":"community-steam-274520-a","state":"in-review","pr_url":")" + pr + "?x=" + kSecret + R"("})",
           // Escaped: found once unescaped.
           std::string(R"({"id":"s1","recipe_id":"community-steam-274520-a","state":"in-review","pr_url":null,"x":"\u0030123456789abcdef0123456789abcdef"})"),
       }) {
    api.answers = {Answer(201, body)};
    CHECK(clean(SubmitRecipe(context, kSecret, request, cancel)));
  }
  api.answers = {Answer(200, R"({"state":"duplicate","recipe_id":"community-steam-274520-a","revision":")" + kSecret + kSecret + R"("})")};
  CHECK(clean(SubmitRecipe(context, kSecret, request, cancel)));
  // An error code that is the secret (it passes the code's character check).
  api.answers = {Answer(400, R"({"error":")" + kSecret + R"(","message":"x"})")};
  auto result = SubmitRecipe(context, kSecret, request, cancel);
  CHECK(clean(result) && result.error.http_status == 400);
  // The status call (no Authorization) refuses it too when it knows the secret.
  api.answers = {Answer(200, R"({"id":"s1","state":"published","recipe_id":"community-steam-274520-a","pr_url":")" + pr + "/" + kSecret + R"("})")};
  auto status = GetSubmission(context, "s1", cancel, kSecret);
  CHECK(!status.ok && status.error.code == "bad_response" && status.pr_url.empty() && status.recipe_id.empty());
  api.answers = {Answer(404, R"({"error":")" + kSecret + R"(","message":"x"})")};
  status = GetSubmission(context, "s1", cancel, kSecret);
  CHECK(!status.ok && status.error.code == "bad_response");
  // Votes.
  VoteRequest vote;
  vote.recipe_id = "community-steam-274520-120hz-left-stick-pointer";
  vote.revision = kRevision;
  vote.vote = "up";
  vote.kit = "0.12";
  vote.firmware = "13.60";
  vote.playden_version = "0.6.0";
  api.answers = {Answer(429, R"({"error":"rate_limited","message":"slow down )" + kSecret + R"("})")};
  const auto voted = SendVote(context, kSecret, vote, cancel);
  CHECK(!voted.ok && voted.error.code == "bad_response" && voted.error.message.find(kSecret) == std::string::npos);
}

VoteRequest Vote() {
  VoteRequest vote;
  vote.recipe_id = "community-steam-274520-120hz-left-stick-pointer";
  vote.revision = kRevision;
  vote.vote = "up";
  vote.kit = "0.12";
  vote.firmware = "13.60";
  vote.playden_version = "0.6.0";
  return vote;
}

void Votes() {
  auto vote = Vote();
  CHECK(VoteBody(vote) ==
        "{\"recipe_id\":\"community-steam-274520-120hz-left-stick-pointer\",\"revision\":\"" + kRevision +
            "\",\"vote\":\"up\",\"kit\":\"0.12\",\"firmware\":\"13.60\",\"playden_version\":\"0.6.0\"}");
  vote.vote = "down";
  vote.reason = "no-start";
  CHECK(VoteBody(vote) ==
        "{\"recipe_id\":\"community-steam-274520-120hz-left-stick-pointer\",\"revision\":\"" + kRevision +
            "\",\"vote\":\"down\",\"reason\":\"no-start\",\"kit\":\"0.12\",\"firmware\":\"13.60\","
            "\"playden_version\":\"0.6.0\"}");
  vote.reason.clear();
  CHECK(VoteRequestProblem(vote).empty());
  // What Other was: only with a thumbs down for Other, 1-120 printable characters.
  vote.reason = "other";
  vote.reason_text = "Music stutters";
  CHECK(VoteRequestProblem(vote).empty());
  CHECK(VoteBody(vote).find("\"reason\":\"other\",\"reason_text\":\"Music stutters\"") != std::string::npos);
  vote.reason = "slow";
  CHECK(!VoteRequestProblem(vote).empty());
  vote.reason = "other";
  vote.reason_text = std::string(121, 'a');
  CHECK(!VoteRequestProblem(vote).empty());
  vote.reason_text = "two\nlines";
  CHECK(!VoteRequestProblem(vote).empty());
  vote.reason.clear();
  vote.reason_text.clear();

  FakeApi api;
  const auto context = Context(&api);
  std::atomic<bool> cancel{false};
  api.answers = {Answer(204, "", {{"Date", "Sat, 07 Nov 2026 10:00:00 GMT"}})};
  auto result = SendVote(context, kSecret, Vote(), cancel);
  CHECK(result.ok && result.accepted_at == 1794045600);  // the API's clock, as summary.json's generated_at
  CHECK(api.requests.size() == 1 && api.requests.back().url == "https://api.playden.club/v1/votes");
  CHECK(api.requests.back().method == steam::HttpMethod::kPost);
  CHECK(HeaderOf(api.requests.back(), "Authorization") == "Bearer " + kSecret);
  CHECK(!HasHeader(api.requests.back(), "Idempotency-Key"));
  CHECK(Body(api.requests.back()) == VoteBody(Vote()));
  CHECK(!SecretOutsideAuthorization(api.requests.back()));
  api.answers = {Answer(400, R"({"error":"unknown_revision","message":"Never published."})")};
  result = SendVote(context, kSecret, Vote(), cancel);
  CHECK(!result.ok && result.error.code == "unknown_revision");
  api.answers = {Answer(429, R"({"error":"rate_limited","message":"x"})", {{"Retry-After", "60"}})};
  result = SendVote(context, kSecret, Vote(), cancel);
  CHECK(!result.ok && result.error.retry_after == 60);

  api.requests.clear();
  const auto refused = [&](const VoteRequest& request, const std::string& secret = kSecret) {
    return SendVote(context, secret, request, cancel).error.code == "invalid_request";
  };
  CHECK(refused(Vote(), "nope"));
  vote = Vote();
  vote.reason = "slow";  // up with a reason
  CHECK(refused(vote));
  vote.vote = "down";
  vote.reason = "boring";
  CHECK(refused(vote));
  vote = Vote();
  vote.vote = "meh";
  CHECK(refused(vote));
  vote = Vote();
  vote.revision = kRevision.substr(0, 32);
  CHECK(refused(vote));
  // Only the id's shape: the vote names no game (the API knows each published revision's).
  for (const char* id : {"community-steam-274520-", "community-local-1-a", "community-274520-a", "community-steam-274520-a\n",
                         "steam-274520-auto"}) {
    vote = Vote();
    vote.recipe_id = id;
    CHECK(refused(vote));
  }
  vote = Vote();
  vote.recipe_id = "community-imported-tmnt-mutant-melee-arrow-keys";
  CHECK(VoteRequestProblem(vote).empty() && VoteBody(vote).find("app_id") == std::string::npos);
  vote = Vote();
  vote.kit.clear();
  CHECK(refused(vote));
  CHECK(api.requests.empty());
  for (const char* reason : {"no-start", "crash", "slow", "controls", "other"}) {
    vote = Vote();
    vote.vote = "down";
    vote.reason = reason;
    CHECK(VoteRequestProblem(vote).empty());
  }
}

ReportRequest Report() {
  ReportRequest report;
  report.store = "steam";
  report.store_id = "274520";
  report.name = "Darkwood";
  report.result = "partial";
  report.reason = "slow";
  report.details = "Drops to 20 fps in the forest.";
  report.installed_on = "usb";
  report.controls = {"dualsense", "mouse-preset"};
  report.settings = "Resolution 3840×2160 · 120 Hz on";
  report.based_on = RecipeRef{"community-steam-274520-120hz-left-stick-pointer", kRevision};
  report.versions = {{"Playden version", "0.10.0"}, {"firmware", "13.60"}};
  report.author = "TodorPenchev";
  report.attachment = "att_1";
  report.kit = "0.19";
  report.firmware = "13.60";
  report.playden_version = "0.10.0";
  report.idempotency_key = "report-key-1";
  return report;
}

// Test reports and their logs (plan 2026-11-06 Phase B): the contract in playden-api's README.
void Reports() {
  CHECK(ReportBody(Report()) ==
        "{\"game\":{\"store\":\"steam\",\"store_id\":\"274520\",\"name\":\"Darkwood\"},\"result\":\"partial\",\"reason\":\"slow\","
        "\"details\":\"Drops to 20 fps in the forest.\",\"installed_on\":\"usb\",\"controls\":[\"dualsense\",\"mouse-preset\"],"
        "\"settings\":\"Resolution 3840×2160 · 120 Hz on\",\"based_on\":{\"recipe_id\":\"community-steam-274520-120hz-left-stick-pointer\","
        "\"revision\":\"" + kRevision + "\"},\"versions\":[[\"Playden version\",\"0.10.0\"],[\"firmware\",\"13.60\"]],"
        "\"name\":\"TodorPenchev\",\"attachment\":\"att_1\",\"kit\":\"0.19\",\"firmware\":\"13.60\",\"playden_version\":\"0.10.0\"}");
  auto imported = Report();
  imported.store = "imported";
  imported.store_id.clear();
  imported.based_on.reset();
  CHECK(ReportBody(imported).find("\"store_id\":null") != std::string::npos);
  const auto problem = [](auto change) {
    auto report = Report();
    change(report);
    return !ReportRequestProblem(report).empty();
  };
  CHECK(ReportRequestProblem(Report()).empty());
  CHECK(problem([](ReportRequest& r) { r.store = "itch"; }));
  CHECK(problem([](ReportRequest& r) { r.store = "imported"; }));  // an imported game has no store ID
  CHECK(problem([](ReportRequest& r) { r.store_id = "27 x"; }));
  CHECK(problem([](ReportRequest& r) { r.result = "fine"; }));
  CHECK(problem([](ReportRequest& r) { r.result = "works"; }));  // a reason only for a problem
  CHECK(problem([](ReportRequest& r) { r.details = std::string(501, 'a'); }));
  CHECK(problem([](ReportRequest& r) { r.details = "two\nlines"; }));
  CHECK(problem([](ReportRequest& r) { r.reason_text = "fps drops"; }));  // only with the reason other
  CHECK(problem([](ReportRequest& r) { r.reason = "other"; r.reason_text = std::string(121, 'a'); }));
  {
    auto other = Report();
    other.reason = "other";
    other.reason_text = "Music stutters";
    CHECK(ReportRequestProblem(other).empty());
    CHECK(ReportBody(other).find("\"reason\":\"other\",\"reason_text\":\"Music stutters\"") != std::string::npos);
  }
  CHECK(problem([](ReportRequest& r) { r.installed_on = "cloud"; }));
  CHECK(problem([](ReportRequest& r) { r.controls = {"wheel"}; }));
  CHECK(problem([](ReportRequest& r) { r.recipe_json = "[]"; }));
  // Any store's game may send its recipe.
  CHECK(!problem([](ReportRequest& r) { r.store = "imported"; r.store_id.clear(); r.recipe_json = "{}"; }));
  CHECK(problem([](ReportRequest& r) { r.versions.assign(25, {"a", "b"}); }));
  CHECK(problem([](ReportRequest& r) { r.attachment = "a b"; }));
  CHECK(problem([](ReportRequest& r) { r.kit.clear(); }));
  CHECK(problem([](ReportRequest& r) { r.idempotency_key = "short"; }));

  FakeApi api;
  const auto context = Context(&api);
  std::atomic<bool> cancel{false};
  api.answers = {Answer(201, R"({"id":"r_7","state":"open","issue_url":"https://github.com/leolobato/playden-ps5/issues/801","updated":false})")};
  auto result = SendReport(context, kSecret, Report(), cancel);
  CHECK(result.ok && result.id == "r_7" && result.state == "open" && !result.updated &&
        result.issue_url == "https://github.com/leolobato/playden-ps5/issues/801");
  CHECK(api.requests.size() == 1 && api.requests.back().url == "https://api.playden.club/v1/reports");
  CHECK(HeaderOf(api.requests.back(), "Idempotency-Key") == "report-key-1" && HeaderOf(api.requests.back(), "Content-Type") == "application/json");
  CHECK(Body(api.requests.back()) == ReportBody(Report()) && !SecretOutsideAuthorization(api.requests.back()));
  api.answers = {Answer(200, R"({"id":"r_7","state":"open","issue_url":"https://github.com/leolobato/playden-ps5/issues/801","updated":true})")};
  CHECK(SendReport(context, kSecret, Report(), cancel).updated);
  api.answers = {Answer(201, R"({"id":"r_8","state":"provisioning","issue_url":null})")};
  result = SendReport(context, kSecret, Report(), cancel);
  CHECK(result.ok && result.state == "provisioning" && result.issue_url.empty());
  api.answers = {Answer(201, R"({"id":"r_8","state":"open","issue_url":null})")};  // open needs its issue
  CHECK(SendReport(context, kSecret, Report(), cancel).error.code == "bad_response");
  api.answers = {Answer(201, R"({"id":"r_8","state":"open","issue_url":"https://x/)" + kSecret + R"("})")};
  CHECK(SendReport(context, kSecret, Report(), cancel).error.code == "bad_response");
  api.answers = {Answer(429, R"({"error":"rate_limited","message":"x"})", {{"Retry-After", "30"}})};
  result = SendReport(context, kSecret, Report(), cancel);
  CHECK(!result.ok && result.error.code == "rate_limited" && result.error.retry_after == 30);

  // The logs: a zip of at most 8 MB, its own content type and key.
  api.requests.clear();
  std::string zip = std::string("PK\x03\x04", 4) + "rest";
  api.answers = {Answer(201, R"({"id":"att_1","url":"https://attachments.playden.club/att_1/playden-logs.zip"})")};
  const auto uploaded = UploadAttachment(context, kSecret, zip, "report-key-1-logs", cancel);
  CHECK(uploaded.ok && uploaded.id == "att_1" && uploaded.url == "https://attachments.playden.club/att_1/playden-logs.zip");
  CHECK(api.requests.size() == 1 && api.requests.back().url == "https://api.playden.club/v1/attachments" &&
        HeaderOf(api.requests.back(), "Content-Type") == "application/zip" && Body(api.requests.back()) == zip &&
        HeaderOf(api.requests.back(), "Idempotency-Key") == "report-key-1-logs");
  CHECK(UploadAttachment(context, kSecret, "not a zip", "report-key-1-logs", cancel).error.code == "invalid_request");
  CHECK(UploadAttachment(context, kSecret, std::string("PK\x03\x04", 4) + std::string(kMaxAttachmentBytes, 'x'),
                         "report-key-1-logs", cancel).error.code == "invalid_request");
  CHECK(UploadAttachment(context, "nope", zip, "report-key-1-logs", cancel).error.code == "invalid_request");
  CHECK(api.requests.size() == 1);
}

// Linking an account: the device flow's two calls, the account, unlinking, and the header on shares and reports.
void AccountLink() {
  FakeApi api;
  const auto context = Context(&api);
  std::atomic<bool> cancel{false};
  api.answers = {Answer(200, R"({"device_code":"dev_code_0123456789","user_code":"WDJB-MJHT","verification_uri":"https://account.playden.club/device","verification_uri_complete":"https://account.playden.club/device?user_code=WDJB-MJHT","expires_in":900,"interval":5})")};
  const auto start = StartAccountLink(context, cancel);
  CHECK(start.ok && start.user_code == "WDJB-MJHT" && start.interval == 5 && start.expires_in == 900 &&
        start.verification_uri_complete == "https://account.playden.club/device?user_code=WDJB-MJHT");
  CHECK(api.requests.back().url == "https://api.playden.club/api/auth/device/code" && Body(api.requests.back()) == R"({"client_id":"playden-ps5"})" &&
        !HasHeader(api.requests.back(), "Authorization"));
  api.answers = {Answer(400, R"({"error":"authorization_pending","error_description":"wait"})")};
  CHECK(PollAccountLink(context, start.device_code, cancel).state == AccountLinkState::kPending);
  CHECK(Body(api.requests.back()).find("urn:ietf:params:oauth:grant-type:device_code") != std::string::npos);
  api.answers = {Answer(400, R"({"error":"slow_down"})")};
  CHECK(PollAccountLink(context, start.device_code, cancel).state == AccountLinkState::kSlowDown);
  api.answers = {Answer(400, R"({"error":"expired_token"})")};
  CHECK(PollAccountLink(context, start.device_code, cancel).state == AccountLinkState::kExpired);
  api.answers = {Answer(400, R"({"error":"access_denied"})")};
  CHECK(PollAccountLink(context, start.device_code, cancel).state == AccountLinkState::kDenied);
  const std::string token = "tok_rina_0123456789abcdef";
  api.answers = {Answer(200, R"({"access_token":")" + token + R"(","token_type":"Bearer","expires_in":7776000})")};
  const auto linked = PollAccountLink(context, start.device_code, cancel);
  CHECK(linked.state == AccountLinkState::kLinked && linked.token == token);
  api.answers = {Answer(200, R"({"name":"Rina","trusted":true})")};
  const auto account = GetLinkedAccount(context, token, cancel);
  CHECK(account.ok && account.name == "Rina" && account.trusted && HeaderOf(api.requests.back(), "X-Playden-Account") == token);
  api.answers = {Answer(401, R"({"error":"account_unlinked","message":"ended"})")};
  CHECK(GetLinkedAccount(context, token, cancel).unlinked);
  api.answers = {Answer(200, R"({"name":")" + token + R"(","trusted":false})")};
  CHECK(!GetLinkedAccount(context, token, cancel).ok);  // an answer echoing the token is refused
  api.answers = {Answer(204, "")};
  CHECK(UnlinkAccount(context, token, cancel, nullptr) && api.requests.back().url == "https://api.playden.club/v1/account/unlink");
  // Shares and reports carry the account beside the install secret.
  api.answers = {Answer(201, R"({"id":"r_7","state":"open","issue_url":"https://github.com/leolobato/playden-ps5/issues/801"})")};
  CHECK(SendReport(context, kSecret, Report(), cancel, token).ok);
  CHECK(HeaderOf(api.requests.back(), "X-Playden-Account") == token && HeaderOf(api.requests.back(), "Authorization") == "Bearer " + kSecret);
  api.answers = {Answer(401, R"({"error":"account_unlinked","message":"ended"})")};
  CHECK(SendReport(context, kSecret, Report(), cancel, token).error.code == "account_unlinked");
  CHECK(IsAccountToken(token) && !IsAccountToken("short") && !IsAccountToken("has space in it 0123"));
}

// Plain http: only a dev build, only to the configured API.
void PlainHttp() {
  FakeApi api;
  ApiContext context = Context(&api);
  context.endpoints.api_base_url = "http://10.0.1.20:8787";
  context.endpoints.allow_plain_http = true;
  std::atomic<bool> cancel{false};
  api.answers = {Answer(204, "")};
  const auto vote = SendVote(context, kSecret, Vote(), cancel);
  api.answers = {Answer(201, R"({"id":"s1","recipe_id":"community-steam-274520","state":"provisioning","pr_url":null})")};
  const auto share = SubmitRecipe(context, kSecret, MinimalRequest(), cancel);
  api.answers = {Answer(200, R"({"id":"s1","state":"closed","recipe_id":"community-steam-274520-a","pr_url":"http://10.0.1.9:3000/p/r/pulls/1"})")};
  const auto status = GetSubmission(context, "s1", cancel);
  if (DevEndpointsCompiledIn()) {
    CHECK(vote.ok && share.outcome == SubmissionOutcome::kCreated && status.ok);
    CHECK(api.requests.size() == 3);
    for (const auto& request : api.requests) {
      CHECK(request.allow_plain_http);
      CHECK(request.url.rfind("http://10.0.1.20:8787/v1/", 0) == 0);
    }
  } else {
    CHECK(!vote.ok && vote.error.code == "not_allowed");
    CHECK(share.outcome == SubmissionOutcome::kError && share.error.code == "not_allowed");
    CHECK(!status.ok && status.error.code == "not_allowed");
    CHECK(api.requests.empty());
  }
  // An API on another host over https: only the dev build's configured origin.
  api.requests.clear();
  context.endpoints.api_base_url = "https://example.org";
  context.endpoints.allow_plain_http = false;
  api.answers = {Answer(204, "")};
  const auto elsewhere = SendVote(context, kSecret, Vote(), cancel);
  CHECK(elsewhere.ok == DevEndpointsCompiledIn());
  if (!DevEndpointsCompiledIn()) CHECK(api.requests.empty());
  // A GitHub host is allowed for downloads, never as the API (the secret would go there).
  api.requests.clear();
  context.endpoints.api_base_url = "https://api.github.com";
  CHECK(DevEndpointsCompiledIn() || SendVote(context, kSecret, Vote(), cancel).error.code == "not_allowed");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <fixtures/community-recipes>\n", argv[0]);
    return 2;
  }
  fixtures = argv[1];
  ExportKeysAccepted();
  SubmissionBodies();
  LocalRefusals();
  SubmitAnswers();
  IdempotencyKeys();
  Submissions();
  SecretEchoes();
  Votes();
  Reports();
  AccountLink();
  PlainHttp();
  if (failures) std::fprintf(stderr, "%d check(s) failed\n", failures);
  return failures ? 1 : 0;
}
