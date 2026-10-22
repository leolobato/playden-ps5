#pragma once
// api_client.h - The Playden API's sharing and voting calls (playden-api README, v1), synchronous:
// call them on a worker thread. Each takes the endpoints, one HTTP exchange (update::Fetch) and a
// cancel flag, and sends nothing when the request breaks the contract (a local error instead).
//   POST /v1/submissions  share a recipe (Authorization: Bearer <install secret>, Idempotency-Key)
//   GET  /v1/submissions/<id>  a submission's state (no Authorization)
//   POST /v1/votes        a thumbs up or down on a published recipe revision (Authorization)
// The install secret goes only in the Authorization header, only to the API's own origin, and never
// into a body, a URL, a result or an error message. Redirects are never followed.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <atomic>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>

#include "community/endpoints.h"
#include "community/share_decision.h"
#include "runtime/game_settings.h"
#include "update/asset_download.h"

namespace playden::community {

inline constexpr std::size_t kMaxSubmissionRecipeBytes = 16 << 10;  // the API's recipe limit
inline constexpr std::size_t kMaxSubmissionBodyBytes = 24 << 10;    // the API's body limit
inline constexpr std::size_t kSubmissionTitleMaxCharacters = 60;
inline constexpr std::size_t kSubmissionNameMaxCharacters = 40;

// The recipe keys the API accepts; it refuses any other (store and game among them).
const std::set<std::string>& AcceptedRecipeKeys();
const std::set<std::string>& AcceptedCommunityKeys();

struct ApiContext {
  Endpoints endpoints = ReleaseEndpoints();
  update::Fetch fetch;
  std::string user_agent = "Playden";
  std::uint32_t timeout_ms = 30000;
};

// An answer that is not a success. http_status 0: nothing came back, or the request was refused
// before sending. code is the API's error code (bad_json, rule, choice_required, rate_limited, ...)
// or one of Playden's own: invalid_request (refused locally, message says why), not_allowed (the
// endpoint's URL), network, cancelled, redirect, bad_response. message is short printable ASCII,
// never holding the secret.
struct ApiError {
  long http_status = 0;
  std::string code;
  std::string message;
  std::int64_t retry_after = 0;  // seconds, from a 429 or 503 (try_again) Retry-After; 0 when absent
};

struct SubmissionRequest {
  std::uint32_t app_id = 0;
  // The recipe as SerializeCommunityRecipe writes it (see RecipeJsonForSubmission): it must parse
  // with ParseCommunityRecipe for app_id, use only the accepted keys, lock nothing and fit 16 KB.
  std::string recipe_json;
  std::string title;  // 1-60 printable characters
  std::string name;   // 0-40 printable characters; empty: left out (the API credits "Anonymous")
  std::optional<RecipeRef> based_on;           // left out when not set
  ShareChoice choice = ShareChoice::kNone;     // sent only when the player was asked
  std::string submission_id;                   // the game's saved submission; left out when empty
  std::string kit, firmware, playden_version;  // required
  // One per share attempt (NewIdempotencyKey), the same for every retry of it.
  std::string idempotency_key;
};

enum class SubmissionOutcome {
  kCreated,             // 201: id, recipe_id, state (provisioning or in-review), pr_url (empty while provisioning)
  kDuplicatePublished,  // 200: recipe_id and revision; Playden may offer a thumbs up
  kDuplicateInReview,   // 200: recipe_id and pr_url of another console's open PR; no vote to offer
  kError,
};

struct SubmissionResult {
  SubmissionOutcome outcome = SubmissionOutcome::kError;
  std::string id;
  std::string recipe_id;
  std::string state;
  std::string pr_url;
  std::string revision;
  ApiError error;
};

struct SubmissionStatus {
  bool ok = false;
  std::string id;
  std::string state;  // provisioning, in-review, published, closed or duplicate
  std::string recipe_id;
  std::string pr_url;  // empty when null
  ApiError error;      // 404 unknown_submission when the API doesn't know the id
};

struct VoteRequest {
  std::uint32_t app_id = 0;
  std::string recipe_id;
  std::string revision;  // 64 hex, a published revision
  std::string vote;      // up or down
  std::string reason;    // down only, optional: no-start, crash, slow, controls, other
  std::string kit, firmware, playden_version;  // required
};

struct VoteResult {
  bool ok = false;
  ApiError error;
};

// The export of a game's effective settings made ready to share: its title set and its locks
// dropped (the API refuses locked rows; a recipe's defaults still apply, the player can change them).
std::string RecipeJsonForSubmission(runtime::CommunityRecipe community, std::string_view title);

// Every reason request breaks the contract, empty when it may be sent (the secret is not checked).
std::string SubmissionRequestProblem(const SubmissionRequest& request);
std::string VoteRequestProblem(const VoteRequest& request);

// The exact request bodies (compact JSON, fields in a fixed order, the recipe as given). Empty when
// the request has a problem.
std::string SubmissionBody(const SubmissionRequest& request);
std::string VoteBody(const VoteRequest& request);

// A submission id as the API makes them and Playden saves and puts in a URL: 1-128 of A-Z a-z 0-9 _ -.
bool IsSubmissionId(std::string_view id);
// An Idempotency-Key: 8-128 of A-Z a-z 0-9 _ -.
bool IsIdempotencyKey(std::string_view key);

SubmissionResult SubmitRecipe(const ApiContext& context, const std::string& secret, const SubmissionRequest& request,
                              const std::atomic<bool>& cancel);
SubmissionStatus GetSubmission(const ApiContext& context, const std::string& id, const std::atomic<bool>& cancel);
VoteResult SendVote(const ApiContext& context, const std::string& secret, const VoteRequest& request,
                    const std::atomic<bool>& cancel);

}  // namespace playden::community
