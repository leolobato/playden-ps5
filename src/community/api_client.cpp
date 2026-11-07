// api_client.cpp - The Playden API's sharing and voting calls.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "community/api_client.h"

#include <strings.h>

#include <algorithm>
#include <cstdio>
#include <initializer_list>

#include "community/install_secret.h"
#include "community/printable_text.h"
#include "community/recipe_release.h"
#include "community/vote_state.h"
#include "diagnostics/redaction.h"
#include "steam/backend/json.h"

namespace playden::community {
namespace {

constexpr std::size_t kMaxResponseBytes = 64 << 10;
constexpr std::size_t kMaxMessageBytes = 200;

bool KeyCharacters(std::string_view text, std::size_t min, std::size_t max) {
  if (text.size() < min || text.size() > max) return false;
  for (char c : text)
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
      return false;
  return true;
}

// Printable ASCII without spaces at either end, 1-max bytes: kit, firmware and version strings.
bool ShortAscii(std::string_view text, std::size_t max) {
  if (text.empty() || text.size() > max || text.front() == ' ' || text.back() == ' ') return false;
  for (char c : text)
    if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7e) return false;
  return true;
}

// The shared rule (community/printable_text.h), between min and max characters.
bool PrintableText(std::string_view text, std::size_t min, std::size_t max) { return IsSubmissionText(text, min, max); }

std::string Escape(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 2);
  for (char c : text) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buffer[8];
          std::snprintf(buffer, sizeof buffer, "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
          out += buffer;
        } else {
          out.push_back(c);
        }
    }
  }
  return out;
}

std::string Quoted(std::string_view text) { return "\"" + Escape(text) + "\""; }

std::string Header(const steam::HttpResponse& response, const char* name) {
  for (const auto& [key, value] : response.headers)
    if (strcasecmp(key.c_str(), name) == 0) return value;
  return {};
}

ApiError LocalError(const char* code, std::string message) {
  ApiError error;
  error.code = code;
  error.message = std::move(message);
  return error;
}

// Printable ASCII, at most kMaxMessageBytes, with any credential taken out.
// Redacted before it is cut, so no part of a secret across the cut survives.
std::string CleanMessage(std::string_view text, const std::string& secret) {
  std::string printable;
  for (char c : text.substr(0, 4096))
    printable.push_back(static_cast<unsigned char>(c) >= 0x20 && static_cast<unsigned char>(c) <= 0x7e ? c : ' ');
  std::string out = diagnostics::RedactSecrets(printable, secret);
  if (out.size() > kMaxMessageBytes) out.resize(kMaxMessageBytes);
  return out;
}

std::int64_t RetryAfter(const steam::HttpResponse& response) {
  const std::string value = Header(response, "retry-after");
  if (value.empty() || value.size() > 9) return 0;  // an HTTP date, or nonsense
  std::int64_t seconds = 0;
  for (char c : value) {
    if (c < '0' || c > '9') return 0;
    seconds = seconds * 10 + (c - '0');
  }
  return seconds > 86400 ? 86400 : seconds;
}

// The error a non-success answer carries ({"error","message"}), or a plain one.
ApiError ErrorFrom(const steam::HttpResponse& response, const std::string& secret) {
  ApiError error;
  error.http_status = response.status_code;
  const std::string body(response.body.begin(), response.body.end());
  if (const auto doc = steam::json::Parse(body); doc && doc->is_object()) {
    if (const auto* code = doc->Find("error"); code && code->is_string() && KeyCharacters(code->text(), 1, 64))
      error.code = code->text();
    if (const auto* message = doc->Find("message"); message && message->is_string())
      error.message = CleanMessage(message->text(), secret);
  }
  if (error.code.empty()) error.code = "http_" + std::to_string(response.status_code);
  if (error.message.empty()) error.message = "status " + std::to_string(response.status_code);
  if (response.status_code == 429 || response.status_code == 503) error.retry_after = RetryAfter(response);
  return error;
}

struct Exchange {
  bool sent = false;  // a response with a status came back (otherwise error is set)
  steam::HttpResponse response;
  ApiError error;
};

// One request to <api base><path>, never redirected. Authorization only with a secret, and only
// when the URL is on the API's own origin.
Exchange Perform(const ApiContext& context, steam::HttpMethod method, const std::string& path, std::string body,
                 const std::string* secret, const std::string& idempotency_key, const std::atomic<bool>& cancel,
                 const char* content_type = "application/json", std::uint32_t timeout_ms = 0,
                 const std::string& account_token = {}) {
  Exchange exchange;
  const std::string url = context.endpoints.api_base_url + path;
  const std::string origin = UrlOrigin(url);
  // The secret goes to api.playden.club only; a dev build also to its configured API. Never to the
  // GitHub hosts EndpointUrlAllowed also lets downloads reach.
  const bool api_origin =
      origin == UrlOrigin(kPlaydenApiBaseUrl) || (DevEndpointsCompiledIn() && EndpointUrlAllowed(context.endpoints, url));
  if (origin.empty() || origin != UrlOrigin(context.endpoints.api_base_url) || !api_origin) {
    exchange.error = LocalError("not_allowed", "the API address is not allowed");
    return exchange;
  }
  const bool plain = url.size() >= 7 && strncasecmp(url.c_str(), "http://", 7) == 0;
  if (plain && !EndpointPlainHttpAllowed(context.endpoints, url)) {
    exchange.error = LocalError("not_allowed", "plain http is not allowed");
    return exchange;
  }
  if (!context.fetch) {
    exchange.error = LocalError("network", "no network");
    return exchange;
  }
  if (cancel) {
    exchange.error = LocalError("cancelled", "cancelled");
    return exchange;
  }
  steam::HttpRequest request;
  request.method = method;
  request.url = url;
  request.headers = {{"User-Agent", context.user_agent}, {"Accept", "application/json"}};
  if (method == steam::HttpMethod::kPost) request.headers.emplace_back("Content-Type", content_type);
  if (secret) request.headers.emplace_back("Authorization", "Bearer " + *secret);
  if (!idempotency_key.empty()) request.headers.emplace_back("Idempotency-Key", idempotency_key);
  if (!account_token.empty()) request.headers.emplace_back("X-Playden-Account", account_token);
  request.body.assign(body.begin(), body.end());
  request.timeout_ms = timeout_ms != 0 ? timeout_ms : context.timeout_ms;
  request.connect_timeout_ms = context.connect_timeout_ms;
  request.max_response_bytes = kMaxResponseBytes;
  request.allow_plain_http = plain;
  exchange.response = context.fetch(request, cancel);
  if (cancel || exchange.response.error == steam::HttpError::kCanceled) {
    exchange.error = LocalError("cancelled", "cancelled");
    return exchange;
  }
  if (!exchange.response.ok()) {
    // Never connected: the server doesn't answer and nothing was sent. The worker doesn't try again at once (the
    // player can), so a missing server fails in seconds.
    exchange.error = LocalError(exchange.response.timing.connected ? "network" : "unreachable",
                                steam::HttpErrorName(exchange.response.error));
    return exchange;
  }
  const long status = exchange.response.status_code;
  if (status >= 300 && status < 400) {
    // Never followed: a redirect could carry the secret elsewhere.
    exchange.error = LocalError("redirect", "unexpected redirect");
    exchange.error.http_status = status;
    return exchange;
  }
  exchange.sent = true;
  return exchange;
}

std::optional<steam::json::Value> JsonBody(const steam::HttpResponse& response) {
  const std::string body(response.body.begin(), response.body.end());
  auto doc = steam::json::Parse(body);
  if (!doc || !doc->is_object()) return std::nullopt;
  return doc;
}

// A string field (or nothing); *null set when it is JSON null.
std::optional<std::string> StringField(const steam::json::Value& object, const char* key, bool* null = nullptr) {
  if (null) *null = false;
  const auto* value = object.Find(key);
  if (!value) return std::nullopt;
  if (value->is_null()) {
    if (null) *null = true;
    return std::nullopt;
  }
  if (!value->is_string()) return std::nullopt;
  return value->text();
}

bool PrUrl(std::string_view url) {
  return ShortAscii(url, 2048) && url.find(' ') == std::string_view::npos && !UrlOrigin(std::string(url)).empty();
}

bool RecipeIdText(std::string_view id) {
  return id.size() <= runtime::kCommunityRecipeIdMaxBytes && runtime::IsCommunityRecipeId(id);
}

ApiError BadResponse(long status) {
  ApiError error = LocalError("bad_response", "the API's answer could not be read");
  error.http_status = status;
  return error;
}

bool HoldsSecret(const steam::json::Value& value, std::string_view secret) {
  if (value.is_string()) return value.text().find(secret) != std::string::npos;
  if (value.is_array())
    for (const auto& item : value.array())
      if (HoldsSecret(item, secret)) return true;
  if (value.is_object())
    for (const auto& [key, item] : value.object())
      if (key.find(secret) != std::string::npos || HoldsSecret(item, secret)) return true;
  return false;
}

// Whether an answer echoes the sharing ID anywhere (raw, or in any JSON string once unescaped). Such
// an answer is refused whole as bad_response: none of its fields is kept, logged or shown.
bool EchoesSecret(const steam::HttpResponse& response, std::string_view secret) {
  if (secret.empty()) return false;
  const std::string body(response.body.begin(), response.body.end());
  if (body.find(secret) != std::string::npos) return true;
  for (const auto& [key, value] : response.headers)
    if (key.find(secret) != std::string::npos || value.find(secret) != std::string::npos) return true;
  const auto doc = steam::json::Parse(body);
  return doc && HoldsSecret(*doc, secret);
}

const char* ChoiceName(ShareChoice choice) {
  switch (choice) {
    case ShareChoice::kUpdate: return "update";
    case ShareChoice::kNew: return "new";
    case ShareChoice::kNone: break;
  }
  return "";
}

std::string RecipeProblem(const SubmissionRequest& request) {
  const std::string& text = request.recipe_json;
  if (text.empty()) return "the recipe is empty";
  if (text.size() > kMaxSubmissionRecipeBytes) return "the recipe is larger than 16 KB";
  runtime::CommunityRecipe community;
  std::string reason;
  if (!runtime::ParseCommunityRecipe(text, &community, &reason)) return "the recipe does not parse: " + reason;
  if (community.game != request.game) return "the recipe is for another game";
  if (!community.recipe.locked.empty()) return "the recipe locks rows (community recipes lock nothing)";
  const auto doc = steam::json::Parse(text);
  if (!doc || !doc->is_object()) return "the recipe is not a JSON object";
  // The API doesn't take INI values yet (a recipe such as Batman: Arkham Asylum's sets them).
  if (doc->Find("ini")) return "the recipe service doesn't take recipes that change the game's INI files yet";
  for (const auto& [key, value] : doc->object())
    if (!AcceptedRecipeKeys().count(key)) return "the recipe has a field the API refuses: " + key;
  const auto* meta = doc->Find("community");
  if (!meta || !meta->is_object()) return "the recipe has no community object";
  for (const auto& [key, value] : meta->object())
    if (!AcceptedCommunityKeys().count(key)) return "the recipe has a community field the API refuses: " + key;
  for (const char* key : {"tested_kit", "tested_firmware", "tested_date", "status"}) {
    const auto* value = meta->Find(key);
    if (!value || !value->is_string() || value->text().empty())
      return std::string("the recipe needs community.") + key;
  }
  return {};
}

}  // namespace

const std::set<std::string>& AcceptedRecipeKeys() {
  static const std::set<std::string> keys = {
      "recipe_id", "recipe_version", "game",     "made_on",    "name",           "launch_entries", "arguments",
      "resolution_arguments", "dll_overrides", "architecture", "runtime", "prefix", "graphics",  "detected",
      "defaults",  "locked",         "requirements", "registry", "executable",   "community"};
  return keys;
}

const std::set<std::string>& AcceptedCommunityKeys() {
  static const std::set<std::string> keys = {"format_version", "title",          "author",      "contributors",
                                             "tested_kit",     "min_kit",        "tested_firmware", "tested_date",
                                             "status",         "notes",          "base"};
  return keys;
}

std::string RecipeJsonForSubmission(runtime::CommunityRecipe community, std::string_view title) {
  community.title = std::string(title);
  community.recipe.locked.clear();
  return runtime::SerializeCommunityRecipe(community);
}

bool IsSubmissionId(std::string_view id) { return KeyCharacters(id, 1, 128); }
bool IsIdempotencyKey(std::string_view key) { return KeyCharacters(key, 8, 128); }

std::string SubmissionRequestProblem(const SubmissionRequest& request) {
  if (!request.game.valid()) return "no game: a store and its key";
  if (!IsGameName(request.game_name)) return "the game's name must be 1-120 printable characters";
  if (auto problem = RecipeProblem(request); !problem.empty()) return problem;
  if (!request.title.empty() && !PrintableText(request.title, 1, kSubmissionTitleMaxCharacters))
    return "the title must be at most 60 printable characters";
  if (!request.name.empty() && !PrintableText(request.name, 1, kSubmissionNameMaxCharacters))
    return "the name must be at most 40 printable characters";
  if (request.based_on) {
    if (!RecipeIdText(request.based_on->recipe_id) || !IsRevision(request.based_on->revision))
      return "based_on needs a community recipe id and a 64-hex revision";
  } else if (request.choice != ShareChoice::kNone) {
    return "a choice needs based_on";
  }
  if (!request.submission_id.empty() && !IsSubmissionId(request.submission_id)) return "bad submission id";
  if (request.summary.size() > kSubmissionSummaryMaxLines) return "the summary has more than 4 lines";
  for (const auto& line : request.summary)
    if (!PrintableText(line, 1, kSubmissionSummaryLineMaxCharacters)) return "a summary line must be 1-300 printable characters";
  if (!ShortAscii(request.kit, 64) || !ShortAscii(request.firmware, 64) || !ShortAscii(request.playden_version, 64))
    return "kit, firmware and Playden version are required";
  if (!IsIdempotencyKey(request.idempotency_key)) return "the Idempotency-Key must be 8-128 of A-Z a-z 0-9 _ -";
  return {};
}

std::string VoteRequestProblem(const VoteRequest& request) {
  if (!RecipeIdText(request.recipe_id)) return "not a community recipe id";
  if (!IsRevision(request.revision)) return "the revision must be 64 lowercase hex";
  if (!IsValidVote(request.vote, request.reason)) return "the vote must be up, or down with an optional known reason";
  if (!request.reason_text.empty() &&
      (request.vote != "down" || request.reason != "other" ||
       !PrintableText(request.reason_text, 1, kReportReasonTextMaxCharacters)))
    return "what Other was must be at most 120 printable characters, with a thumbs down for Other";
  if (!ShortAscii(request.kit, 64) || !ShortAscii(request.firmware, 64) || !ShortAscii(request.playden_version, 64))
    return "kit, firmware and Playden version are required";
  return {};
}

std::string SubmissionBody(const SubmissionRequest& request) {
  if (!SubmissionRequestProblem(request).empty()) return {};
  std::string recipe = request.recipe_json;
  while (!recipe.empty() && (recipe.back() == '\n' || recipe.back() == ' ' || recipe.back() == '\r')) recipe.pop_back();
  std::string body = "{\"game\":{\"store\":" + Quoted(request.game.store) + ",\"key\":" + Quoted(request.game.key) +
                     ",\"name\":" + Quoted(request.game_name) + "},\"recipe\":" + recipe;
  if (!request.title.empty()) body += ",\"title\":" + Quoted(request.title);
  if (!request.name.empty()) body += ",\"name\":" + Quoted(request.name);
  if (request.based_on)
    body += ",\"based_on\":{\"recipe_id\":" + Quoted(request.based_on->recipe_id) +
            ",\"revision\":" + Quoted(request.based_on->revision) + "}";
  if (request.choice != ShareChoice::kNone) body += std::string(",\"choice\":\"") + ChoiceName(request.choice) + "\"";
  if (!request.submission_id.empty()) body += ",\"submission_id\":" + Quoted(request.submission_id);
  if (!request.summary.empty()) {
    body += ",\"summary\":[";
    for (std::size_t i = 0; i < request.summary.size(); ++i) body += (i ? "," : "") + Quoted(request.summary[i]);
    body += "]";
  }
  body += ",\"kit\":" + Quoted(request.kit) + ",\"firmware\":" + Quoted(request.firmware) +
          ",\"playden_version\":" + Quoted(request.playden_version) + "}";
  return body;
}

std::string VoteBody(const VoteRequest& request) {
  if (!VoteRequestProblem(request).empty()) return {};
  std::string body = "{\"recipe_id\":" + Quoted(request.recipe_id) +
                     ",\"revision\":" + Quoted(request.revision) + ",\"vote\":" + Quoted(request.vote);
  if (!request.reason.empty()) body += ",\"reason\":" + Quoted(request.reason);
  if (!request.reason_text.empty()) body += ",\"reason_text\":" + Quoted(request.reason_text);
  body += ",\"kit\":" + Quoted(request.kit) + ",\"firmware\":" + Quoted(request.firmware) +
          ",\"playden_version\":" + Quoted(request.playden_version) + "}";
  return body;
}

SubmissionResult SubmitRecipe(const ApiContext& context, const std::string& secret, const SubmissionRequest& request,
                              const std::atomic<bool>& cancel, const std::string& account_token) {
  SubmissionResult result;
  if (!IsInstallSecret(secret)) {
    result.error = LocalError("invalid_request", "no valid sharing ID");
    return result;
  }
  if (auto problem = SubmissionRequestProblem(request); !problem.empty()) {
    result.error = LocalError("invalid_request", problem);
    return result;
  }
  std::string body = SubmissionBody(request);
  if (body.size() > kMaxSubmissionBodyBytes) {
    result.error = LocalError("invalid_request", "the request is larger than 24 KB");
    return result;
  }
  auto exchange = Perform(context, steam::HttpMethod::kPost, "/v1/submissions", std::move(body), &secret,
                          request.idempotency_key, cancel, "application/json", 0, account_token);
  if (!exchange.sent) {
    result.error = exchange.error;
    return result;
  }
  const auto& response = exchange.response;
  const long status = response.status_code;
  if (EchoesSecret(response, secret) || EchoesSecret(response, account_token)) {
    result.error = BadResponse(status);
    return result;
  }
  if (status != 200 && status != 201) {
    result.error = ErrorFrom(response, secret);
    return result;
  }
  const auto doc = JsonBody(response);
  if (!doc) {
    result.error = BadResponse(status);
    return result;
  }
  const auto state = StringField(*doc, "state");
  const auto recipe_id = StringField(*doc, "recipe_id");
  if (!state || !recipe_id || !RecipeIdText(*recipe_id)) {
    result.error = BadResponse(status);
    return result;
  }
  if (status == 201) {
    bool pr_null = false;
    const auto id = StringField(*doc, "id");
    const auto pr_url = StringField(*doc, "pr_url", &pr_null);
    // A replay of the same Idempotency-Key answers with the share's state now: its pull request may be merged or
    // closed since the answer that was lost.
    if (!id || !IsSubmissionId(*id) ||
        (*state != "provisioning" && *state != "in-review" && *state != "published" && *state != "closed") ||
        (!pr_null && (!pr_url || !PrUrl(*pr_url)))) {
      result.error = BadResponse(status);
      return result;
    }
    result.outcome = SubmissionOutcome::kCreated;
    result.id = *id;
    result.recipe_id = *recipe_id;
    result.state = *state;
    result.pr_url = pr_url.value_or("");
    if (const auto* unchanged = doc->Find("unchanged"); unchanged && unchanged->is_bool()) result.unchanged = unchanged->boolean();
    return result;
  }
  // 200: a duplicate, of a published revision or of another console's open pull request.
  if (*state != "duplicate") {
    result.error = BadResponse(status);
    return result;
  }
  result.recipe_id = *recipe_id;
  result.state = *state;
  if (const auto revision = StringField(*doc, "revision")) {
    if (!IsRevision(*revision)) {
      result.error = BadResponse(status);
      result.recipe_id.clear();
      return result;
    }
    result.outcome = SubmissionOutcome::kDuplicatePublished;
    result.revision = *revision;
    return result;
  }
  // Another console's share whose pull request isn't open yet has none: null.
  bool pr_null = false;
  const auto pr_url = StringField(*doc, "pr_url", &pr_null);
  if (!pr_null && (!pr_url || !PrUrl(*pr_url))) {
    result.error = BadResponse(status);
    result.recipe_id.clear();
    return result;
  }
  result.outcome = SubmissionOutcome::kDuplicateInReview;
  result.pr_url = pr_url.value_or("");
  return result;
}

SubmissionStatus GetSubmission(const ApiContext& context, const std::string& id, const std::atomic<bool>& cancel,
                               std::string_view known_secret) {
  SubmissionStatus status;
  if (!IsSubmissionId(id)) {
    status.error = LocalError("invalid_request", "bad submission id");
    return status;
  }
  auto exchange = Perform(context, steam::HttpMethod::kGet, "/v1/submissions/" + id, {}, nullptr, {}, cancel);
  if (!exchange.sent) {
    status.error = exchange.error;
    return status;
  }
  const auto& response = exchange.response;
  if (EchoesSecret(response, known_secret)) {
    status.error = BadResponse(response.status_code);
    return status;
  }
  if (response.status_code != 200) {
    status.error = ErrorFrom(response, std::string(known_secret));
    return status;
  }
  const auto doc = JsonBody(response);
  bool recipe_null = false, pr_null = false;
  const auto got_id = doc ? StringField(*doc, "id") : std::nullopt;
  const auto state = doc ? StringField(*doc, "state") : std::nullopt;
  const auto recipe_id = doc ? StringField(*doc, "recipe_id", &recipe_null) : std::nullopt;
  const auto pr_url = doc ? StringField(*doc, "pr_url", &pr_null) : std::nullopt;
  const bool state_ok = state && (*state == "provisioning" || *state == "in-review" || *state == "published" ||
                                  *state == "closed" || *state == "duplicate");
  if (!doc || !got_id || *got_id != id || !state_ok || (!recipe_null && (!recipe_id || !RecipeIdText(*recipe_id))) ||
      (!pr_null && (!pr_url || !PrUrl(*pr_url)))) {
    status.error = BadResponse(response.status_code);
    return status;
  }
  status.ok = true;
  status.id = *got_id;
  status.state = *state;
  status.recipe_id = recipe_id.value_or("");
  status.pr_url = pr_url.value_or("");
  return status;
}

VoteResult SendVote(const ApiContext& context, const std::string& secret, const VoteRequest& request,
                    const std::atomic<bool>& cancel) {
  VoteResult result;
  if (!IsInstallSecret(secret)) {
    result.error = LocalError("invalid_request", "no valid sharing ID");
    return result;
  }
  if (auto problem = VoteRequestProblem(request); !problem.empty()) {
    result.error = LocalError("invalid_request", problem);
    return result;
  }
  auto exchange = Perform(context, steam::HttpMethod::kPost, "/v1/votes", VoteBody(request), &secret, {}, cancel);
  if (!exchange.sent) {
    result.error = exchange.error;
    return result;
  }
  if (EchoesSecret(exchange.response, secret)) {
    result.error = BadResponse(exchange.response.status_code);
    return result;
  }
  if (exchange.response.status_code != 204 && exchange.response.status_code != 200) {
    result.error = ErrorFrom(exchange.response, secret);
    return result;
  }
  result.ok = true;
  result.accepted_at = ParseUtcTime(Header(exchange.response, "date")).value_or(0);
  return result;
}

namespace {

bool OneOf(std::string_view value, std::initializer_list<std::string_view> names) {
  for (const auto name : names)
    if (value == name) return true;
  return false;
}

// A store's game ID: 1-64 of A-Z a-z 0-9 . _ - (Steam's and GOG's are digits; Epic's are not).
bool StoreIdText(std::string_view id) {
  if (id.empty() || id.size() > 64) return false;
  for (const char c : id)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '_' || c == '-'))
      return false;
  return true;
}

// UTF-8 characters (code points) in text: every byte that doesn't continue one.
std::size_t Characters(std::string_view text) {
  return static_cast<std::size_t>(
      std::count_if(text.begin(), text.end(), [](char c) { return (static_cast<unsigned char>(c) & 0xc0) != 0x80; }));
}

// Printable text that may hold line breaks (the settings list): each line printable on its own, at most max characters
// in all, line breaks included (the API counts them so).
bool MultilineText(std::string_view text, std::size_t max) {
  if (text.size() > max * 4 || Characters(text) > max) return false;
  std::size_t start = 0;
  while (start <= text.size()) {
    const auto end = std::min(text.find('\n', start), text.size());
    const auto line = text.substr(start, end - start);
    if (!line.empty() && !PrintableText(line, 1, max)) return false;
    if (end == text.size()) break;
    start = end + 1;
  }
  return true;
}

}  // namespace

std::string ReportSettingsText(std::string_view settings) {
  std::size_t characters = 0;
  for (std::size_t i = 0; i < settings.size(); ++i) {
    if ((static_cast<unsigned char>(settings[i]) & 0xc0) == 0x80) continue;  // continues the character before
    if (++characters > kReportSettingsMaxCharacters) return std::string(settings.substr(0, i));
  }
  return std::string(settings);
}

std::string ReportRequestProblem(const ReportRequest& request) {
  if (!OneOf(request.store, {"steam", "epic", "gog", "imported"})) return "the store must be steam, epic, gog or imported";
  if (request.store == "imported" ? !request.store_id.empty() : !StoreIdText(request.store_id))
    return "a store game needs its ID, an imported game none";
  if (!PrintableText(request.name, 1, kReportGameNameMaxCharacters)) return "the game's name must be 1-120 printable characters";
  if (!OneOf(request.result, {"works", "partial", "broken", "no-start"})) return "the result must be works, partial, broken or no-start";
  if (!request.reason.empty() &&
      (request.result == "works" || !OneOf(request.reason, {"no-start", "crash", "slow", "controls", "other"})))
    return "a reason is one of the five, and only for a problem";
  if (!request.reason_text.empty() &&
      (request.reason != "other" || !PrintableText(request.reason_text, 1, kReportReasonTextMaxCharacters)))
    return "what Other was must be at most 120 printable characters, with the reason other";
  if (!request.details.empty() && !PrintableText(request.details, 1, kReportDetailsMaxCharacters))
    return "what happened must be at most 500 printable characters";
  if (!OneOf(request.installed_on, {"console", "m2", "usb", "imported", "other"})) return "where the game is installed is missing";
  for (const auto& control : request.controls)
    if (!OneOf(control, {"dualsense", "mouse-preset", "arrow-keys-preset", "usb-mouse-keyboard"})) return "unknown controls";
  if (!MultilineText(request.settings, kReportSettingsMaxCharacters)) return "the settings list is too long or not printable";
  if (!request.recipe_json.empty()) {
    if (request.recipe_json.size() > kMaxSubmissionRecipeBytes) return "the recipe is larger than 16 KB";
    const auto doc = steam::json::Parse(request.recipe_json);
    if (!doc || !doc->is_object()) return "the recipe is not a JSON object";
  }
  if (request.based_on && (!RecipeIdText(request.based_on->recipe_id) || !IsRevision(request.based_on->revision)))
    return "based_on needs a community recipe id and a 64-hex revision";
  if (request.versions.size() > kReportMaxVersionRows) return "too many version rows";
  for (const auto& [field, value] : request.versions)
    if (!PrintableText(field, 1, 40) || (!value.empty() && !PrintableText(value, 1, 200))) return "a version row is not printable";
  if (!request.author.empty() && !PrintableText(request.author, 1, kSubmissionNameMaxCharacters))
    return "the name must be at most 40 printable characters";
  if (!request.attachment.empty() && !IsSubmissionId(request.attachment)) return "bad attachment id";
  if (!ShortAscii(request.kit, 64) || !ShortAscii(request.firmware, 64) || !ShortAscii(request.playden_version, 64))
    return "kit, firmware and Playden version are required";
  if (!IsIdempotencyKey(request.idempotency_key)) return "the Idempotency-Key must be 8-128 of A-Z a-z 0-9 _ -";
  return {};
}

std::string ReportBody(const ReportRequest& request) {
  if (!ReportRequestProblem(request).empty()) return {};
  std::string body = "{\"game\":{\"store\":" + Quoted(request.store) +
                     ",\"store_id\":" + (request.store_id.empty() ? std::string("null") : Quoted(request.store_id)) +
                     ",\"name\":" + Quoted(request.name) + "},\"result\":" + Quoted(request.result);
  if (!request.reason.empty()) body += ",\"reason\":" + Quoted(request.reason);
  if (!request.reason_text.empty()) body += ",\"reason_text\":" + Quoted(request.reason_text);
  if (!request.details.empty()) body += ",\"details\":" + Quoted(request.details);
  body += ",\"installed_on\":" + Quoted(request.installed_on) + ",\"controls\":[";
  for (std::size_t i = 0; i < request.controls.size(); ++i) body += (i ? "," : "") + Quoted(request.controls[i]);
  body += "],\"settings\":" + Quoted(request.settings);
  if (!request.recipe_json.empty()) {
    std::string recipe = request.recipe_json;
    while (!recipe.empty() && (recipe.back() == '\n' || recipe.back() == ' ' || recipe.back() == '\r')) recipe.pop_back();
    body += ",\"recipe\":" + recipe;
  }
  if (request.based_on)
    body += ",\"based_on\":{\"recipe_id\":" + Quoted(request.based_on->recipe_id) +
            ",\"revision\":" + Quoted(request.based_on->revision) + "}";
  body += ",\"versions\":[";
  for (std::size_t i = 0; i < request.versions.size(); ++i)
    body += std::string(i ? "," : "") + "[" + Quoted(request.versions[i].first) + "," + Quoted(request.versions[i].second) + "]";
  body += "]";
  if (!request.author.empty()) body += ",\"name\":" + Quoted(request.author);
  if (!request.attachment.empty()) body += ",\"attachment\":" + Quoted(request.attachment);
  body += ",\"kit\":" + Quoted(request.kit) + ",\"firmware\":" + Quoted(request.firmware) +
          ",\"playden_version\":" + Quoted(request.playden_version) + "}";
  return body;
}

AttachmentResult UploadAttachment(const ApiContext& context, const std::string& secret, const std::string& zip,
                                  const std::string& idempotency_key, const std::atomic<bool>& cancel) {
  AttachmentResult result;
  if (!IsInstallSecret(secret)) {
    result.error = LocalError("invalid_request", "no valid sharing ID");
    return result;
  }
  if (zip.size() < 4 || zip.compare(0, 4, "PK\x03\x04") != 0 || zip.size() > kMaxAttachmentBytes) {
    result.error = LocalError("invalid_request", "the logs are not a zip of at most 8 MB");
    return result;
  }
  if (!IsIdempotencyKey(idempotency_key)) {
    result.error = LocalError("invalid_request", "the Idempotency-Key must be 8-128 of A-Z a-z 0-9 _ -");
    return result;
  }
  // Uploads can be slow on a console's network: two minutes, not the usual 30 s.
  auto exchange = Perform(context, steam::HttpMethod::kPost, "/v1/attachments", zip, &secret, idempotency_key, cancel,
                          "application/zip", 120000);
  if (!exchange.sent) {
    result.error = exchange.error;
    return result;
  }
  const auto& response = exchange.response;
  if (EchoesSecret(response, secret)) {
    result.error = BadResponse(response.status_code);
    return result;
  }
  if (response.status_code != 200 && response.status_code != 201) {
    result.error = ErrorFrom(response, secret);
    return result;
  }
  const auto doc = JsonBody(response);
  const auto id = doc ? StringField(*doc, "id") : std::nullopt;
  const auto url = doc ? StringField(*doc, "url") : std::nullopt;
  if (!id || !IsSubmissionId(*id) || !url || !PrUrl(*url)) {
    result.error = BadResponse(response.status_code);
    return result;
  }
  result.ok = true;
  result.id = *id;
  result.url = *url;
  return result;
}

ReportResult SendReport(const ApiContext& context, const std::string& secret, const ReportRequest& request,
                        const std::atomic<bool>& cancel, const std::string& account_token) {
  ReportResult result;
  if (!IsInstallSecret(secret)) {
    result.error = LocalError("invalid_request", "no valid sharing ID");
    return result;
  }
  if (auto problem = ReportRequestProblem(request); !problem.empty()) {
    result.error = LocalError("invalid_request", problem);
    return result;
  }
  std::string body = ReportBody(request);
  if (body.size() > kMaxReportBodyBytes) {
    result.error = LocalError("invalid_request", "the report is larger than 48 KB");
    return result;
  }
  auto exchange = Perform(context, steam::HttpMethod::kPost, "/v1/reports", std::move(body), &secret,
                          request.idempotency_key, cancel, "application/json", 0, account_token);
  if (!exchange.sent) {
    result.error = exchange.error;
    return result;
  }
  const auto& response = exchange.response;
  const long status = response.status_code;
  if (EchoesSecret(response, secret) || EchoesSecret(response, account_token)) {
    result.error = BadResponse(status);
    return result;
  }
  if (status != 200 && status != 201) {
    result.error = ErrorFrom(response, secret);
    return result;
  }
  const auto doc = JsonBody(response);
  bool issue_null = false;
  const auto id = doc ? StringField(*doc, "id") : std::nullopt;
  const auto state = doc ? StringField(*doc, "state") : std::nullopt;
  const auto issue_url = doc ? StringField(*doc, "issue_url", &issue_null) : std::nullopt;
  const auto* updated = doc ? doc->Find("updated") : nullptr;
  if (!id || !IsSubmissionId(*id) || !state || (*state != "provisioning" && *state != "open") ||
      (!issue_null && (!issue_url || !PrUrl(*issue_url))) || (*state == "open" && issue_null) ||
      (updated && !updated->is_bool())) {
    result.error = BadResponse(status);
    return result;
  }
  result.ok = true;
  result.id = *id;
  result.state = *state;
  result.issue_url = issue_url.value_or("");
  result.updated = updated && updated->boolean();
  return result;
}

AccountLinkStart StartAccountLink(const ApiContext& context, const std::atomic<bool>& cancel) {
  AccountLinkStart result;
  auto exchange = Perform(context, steam::HttpMethod::kPost, "/api/auth/device/code",
                          std::string("{\"client_id\":\"") + kAccountClientId + "\"}", nullptr, {}, cancel);
  if (!exchange.sent) {
    result.error = exchange.error;
    return result;
  }
  const auto& response = exchange.response;
  if (response.status_code != 200) {
    result.error = ErrorFrom(response, {});
    return result;
  }
  const auto doc = JsonBody(response);
  const auto device = doc ? StringField(*doc, "device_code") : std::nullopt;
  const auto user = doc ? StringField(*doc, "user_code") : std::nullopt;
  const auto uri = doc ? StringField(*doc, "verification_uri_complete") : std::nullopt;
  const auto number = [&](const char* key, std::int64_t fallback) {
    const auto* value = doc ? doc->Find(key) : nullptr;
    const auto parsed = value ? value->AsUint64() : std::nullopt;
    return parsed && *parsed > 0 && *parsed < 86400 ? static_cast<std::int64_t>(*parsed) : fallback;
  };
  if (!device || !IsAccountToken(*device) || !user || !ShortAscii(*user, 16) || user->empty() || !uri || !PrUrl(*uri)) {
    result.error = BadResponse(response.status_code);
    return result;
  }
  result.ok = true;
  result.device_code = *device;
  result.user_code = *user;
  result.verification_uri_complete = *uri;
  result.interval = number("interval", 5);
  result.expires_in = number("expires_in", 900);
  return result;
}

AccountLinkPoll PollAccountLink(const ApiContext& context, const std::string& device_code, const std::atomic<bool>& cancel) {
  AccountLinkPoll result;
  if (!IsAccountToken(device_code)) {
    result.error = LocalError("invalid_request", "no device code");
    return result;
  }
  const std::string body = std::string("{\"grant_type\":\"urn:ietf:params:oauth:grant-type:device_code\",\"device_code\":") +
                           Quoted(device_code) + ",\"client_id\":\"" + kAccountClientId + "\"}";
  auto exchange = Perform(context, steam::HttpMethod::kPost, "/api/auth/device/token", body, nullptr, {}, cancel);
  if (!exchange.sent) {
    result.error = exchange.error;
    return result;
  }
  const auto& response = exchange.response;
  const auto doc = JsonBody(response);
  if (response.status_code == 200) {
    const auto token = doc ? StringField(*doc, "access_token") : std::nullopt;
    if (!token || !IsAccountToken(*token)) {
      result.error = BadResponse(200);
      return result;
    }
    result.state = AccountLinkState::kLinked;
    result.token = *token;
    return result;
  }
  // RFC 8628 errors: keep polling, slower, or stop.
  const auto error = doc ? StringField(*doc, "error") : std::nullopt;
  if (error && *error == "authorization_pending") result.state = AccountLinkState::kPending;
  else if (error && *error == "slow_down") result.state = AccountLinkState::kSlowDown;
  else if (error && *error == "expired_token") result.state = AccountLinkState::kExpired;
  else if (error && *error == "access_denied") result.state = AccountLinkState::kDenied;
  else result.error = ErrorFrom(response, device_code);
  return result;
}

LinkedAccount GetLinkedAccount(const ApiContext& context, const std::string& token, const std::atomic<bool>& cancel) {
  LinkedAccount result;
  if (!IsAccountToken(token)) {
    result.error = LocalError("invalid_request", "no account token");
    return result;
  }
  auto exchange = Perform(context, steam::HttpMethod::kGet, "/v1/account", {}, nullptr, {}, cancel, "application/json", 0, token);
  if (!exchange.sent) {
    result.error = exchange.error;
    return result;
  }
  const auto& response = exchange.response;
  if (EchoesSecret(response, token)) {
    result.error = BadResponse(response.status_code);
    return result;
  }
  if (response.status_code == 401) {
    result.unlinked = true;
    result.error = ErrorFrom(response, token);
    return result;
  }
  const auto doc = response.status_code == 200 ? JsonBody(response) : std::nullopt;
  const auto name = doc ? StringField(*doc, "name") : std::nullopt;
  const auto* trusted = doc ? doc->Find("trusted") : nullptr;
  if (!name || (!name->empty() && !PrintableText(*name, 1, kSubmissionNameMaxCharacters)) || !trusted || !trusted->is_bool()) {
    result.error = response.status_code == 200 ? BadResponse(200) : ErrorFrom(response, token);
    return result;
  }
  result.ok = true;
  result.name = *name;
  result.trusted = trusted->boolean();
  return result;
}

bool UnlinkAccount(const ApiContext& context, const std::string& token, const std::atomic<bool>& cancel, ApiError* error) {
  if (!IsAccountToken(token)) return true;
  auto exchange = Perform(context, steam::HttpMethod::kPost, "/v1/account/unlink", "{}", nullptr, {}, cancel, "application/json", 0, token);
  if (!exchange.sent) {
    if (error) *error = exchange.error;
    return false;
  }
  // Already unlinked counts as done.
  if (exchange.response.status_code == 204 || exchange.response.status_code == 401) return true;
  if (error) *error = ErrorFrom(exchange.response, token);
  return false;
}

}  // namespace playden::community
