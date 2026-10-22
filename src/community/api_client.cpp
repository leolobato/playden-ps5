// api_client.cpp - The Playden API's sharing and voting calls.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "community/api_client.h"

#include <strings.h>

#include <cstdio>

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
                 const std::string* secret, const std::string& idempotency_key, const std::atomic<bool>& cancel) {
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
  if (method == steam::HttpMethod::kPost) request.headers.emplace_back("Content-Type", "application/json");
  if (secret) request.headers.emplace_back("Authorization", "Bearer " + *secret);
  if (!idempotency_key.empty()) request.headers.emplace_back("Idempotency-Key", idempotency_key);
  request.body.assign(body.begin(), body.end());
  request.timeout_ms = context.timeout_ms;
  request.max_response_bytes = kMaxResponseBytes;
  request.allow_plain_http = plain;
  exchange.response = context.fetch(request, cancel);
  if (cancel || exchange.response.error == steam::HttpError::kCanceled) {
    exchange.error = LocalError("cancelled", "cancelled");
    return exchange;
  }
  if (!exchange.response.ok()) {
    exchange.error = LocalError("network", steam::HttpErrorName(exchange.response.error));
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
  if (community.recipe.app_id != request.app_id) return "the recipe is for another app ID";
  if (!community.recipe.locked.empty()) return "the recipe locks rows (community recipes lock nothing)";
  const auto doc = steam::json::Parse(text);
  if (!doc || !doc->is_object()) return "the recipe is not a JSON object";
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
      "recipe_id", "recipe_version", "app_id",   "name",       "launch_entries", "arguments",   "resolution_arguments",
      "dll_overrides", "architecture", "runtime", "prefix",    "graphics",       "detected",    "defaults",
      "locked",    "requirements",   "registry", "steam_appid", "executable",    "community"};
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
  if (request.app_id == 0) return "no app ID";
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
  if (!ShortAscii(request.kit, 64) || !ShortAscii(request.firmware, 64) || !ShortAscii(request.playden_version, 64))
    return "kit, firmware and Playden version are required";
  if (!IsIdempotencyKey(request.idempotency_key)) return "the Idempotency-Key must be 8-128 of A-Z a-z 0-9 _ -";
  return {};
}

std::string VoteRequestProblem(const VoteRequest& request) {
  if (request.app_id == 0) return "no app ID";
  if (!RecipeIdText(request.recipe_id)) return "not a community recipe id";
  std::uint32_t id_app = 0;
  std::string slug;
  if (!runtime::SplitCommunityRecipeId(request.recipe_id, &id_app, &slug) || id_app != request.app_id)
    return "the recipe id is not for this app ID";
  if (!IsRevision(request.revision)) return "the revision must be 64 lowercase hex";
  if (!IsValidVote(request.vote, request.reason)) return "the vote must be up, or down with an optional known reason";
  if (!ShortAscii(request.kit, 64) || !ShortAscii(request.firmware, 64) || !ShortAscii(request.playden_version, 64))
    return "kit, firmware and Playden version are required";
  return {};
}

std::string SubmissionBody(const SubmissionRequest& request) {
  if (!SubmissionRequestProblem(request).empty()) return {};
  std::string recipe = request.recipe_json;
  while (!recipe.empty() && (recipe.back() == '\n' || recipe.back() == ' ' || recipe.back() == '\r')) recipe.pop_back();
  std::string body = "{\"app_id\":" + std::to_string(request.app_id) + ",\"recipe\":" + recipe;
  if (!request.title.empty()) body += ",\"title\":" + Quoted(request.title);
  if (!request.name.empty()) body += ",\"name\":" + Quoted(request.name);
  if (request.based_on)
    body += ",\"based_on\":{\"recipe_id\":" + Quoted(request.based_on->recipe_id) +
            ",\"revision\":" + Quoted(request.based_on->revision) + "}";
  if (request.choice != ShareChoice::kNone) body += std::string(",\"choice\":\"") + ChoiceName(request.choice) + "\"";
  if (!request.submission_id.empty()) body += ",\"submission_id\":" + Quoted(request.submission_id);
  body += ",\"kit\":" + Quoted(request.kit) + ",\"firmware\":" + Quoted(request.firmware) +
          ",\"playden_version\":" + Quoted(request.playden_version) + "}";
  return body;
}

std::string VoteBody(const VoteRequest& request) {
  if (!VoteRequestProblem(request).empty()) return {};
  std::string body = "{\"app_id\":" + std::to_string(request.app_id) + ",\"recipe_id\":" + Quoted(request.recipe_id) +
                     ",\"revision\":" + Quoted(request.revision) + ",\"vote\":" + Quoted(request.vote);
  if (!request.reason.empty()) body += ",\"reason\":" + Quoted(request.reason);
  body += ",\"kit\":" + Quoted(request.kit) + ",\"firmware\":" + Quoted(request.firmware) +
          ",\"playden_version\":" + Quoted(request.playden_version) + "}";
  return body;
}

SubmissionResult SubmitRecipe(const ApiContext& context, const std::string& secret, const SubmissionRequest& request,
                              const std::atomic<bool>& cancel) {
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
                          request.idempotency_key, cancel);
  if (!exchange.sent) {
    result.error = exchange.error;
    return result;
  }
  const auto& response = exchange.response;
  const long status = response.status_code;
  if (EchoesSecret(response, secret)) {
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
    if (!id || !IsSubmissionId(*id) || (*state != "provisioning" && *state != "in-review") ||
        (!pr_null && (!pr_url || !PrUrl(*pr_url)))) {
      result.error = BadResponse(status);
      return result;
    }
    result.outcome = SubmissionOutcome::kCreated;
    result.id = *id;
    result.recipe_id = *recipe_id;
    result.state = *state;
    result.pr_url = pr_url.value_or("");
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
  const auto pr_url = StringField(*doc, "pr_url");
  if (!pr_url || !PrUrl(*pr_url)) {
    result.error = BadResponse(status);
    result.recipe_id.clear();
    return result;
  }
  result.outcome = SubmissionOutcome::kDuplicateInReview;
  result.pr_url = *pr_url;
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
  return result;
}

}  // namespace playden::community
