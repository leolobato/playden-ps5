#pragma once
// api_client.h - The Playden API's sharing and voting calls (playden-api README, v1), synchronous:
// call them on a worker thread. Each takes the endpoints, one HTTP exchange (update::Fetch) and a
// cancel flag, and sends nothing when the request breaks the contract (a local error instead).
//   POST /v1/submissions  share a recipe (Authorization: Bearer <install secret>, Idempotency-Key)
//   GET  /v1/submissions/<id>  a submission's state (no Authorization)
//   POST /v1/votes        a thumbs up or down on a published recipe revision (Authorization)
//   POST /v1/attachments  a report's redacted logs zip (Authorization, Idempotency-Key)
//   POST /v1/reports      a game test report, opened as an issue (Authorization, Idempotency-Key)
// The install secret goes only in the Authorization header, only to the API's own origin, and never
// into a body, a URL, a result or an error message. An answer that holds the secret anywhere (a
// hostile or broken API echoing it) is refused whole as bad_response. Redirects are never followed.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <atomic>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "community/endpoints.h"
#include "community/printable_text.h"
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
  // The game the recipe is for (SharingGameRef: an imported game without a Steam app ID shares under its name key)
  // and its name as the store or folder shows it (IsGameName); sent as "game": {"store", "key", "name"}.
  GameRef game;
  std::string game_name;
  // The recipe as SerializeCommunityRecipe writes it (see RecipeJsonForSubmission): it must parse
  // with ParseCommunityRecipe for game, use only the accepted keys, lock nothing and fit 16 KB.
  std::string recipe_json;
  std::string title;  // optional: 0-60 characters IsSubmissionText accepts; empty is left out
  std::string name;   // 0-40 such characters; empty: left out (the recipe has no author: "from the community")
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

// Game test reports (plan 2026-11-06 Phase B). The API opens an issue in the issue form's layout.
inline constexpr std::size_t kMaxAttachmentBytes = 8 << 20;  // the API's attachment limit
inline constexpr std::size_t kMaxReportBodyBytes = 48 << 10;  // the API's report body limit
inline constexpr std::size_t kReportDetailsMaxCharacters = 500;
inline constexpr std::size_t kReportReasonTextMaxCharacters = 120;
inline constexpr std::size_t kReportGameNameMaxCharacters = 120;
inline constexpr std::size_t kReportSettingsMaxCharacters = 2000;
inline constexpr std::size_t kReportMaxVersionRows = 24;

struct ReportRequest {
  std::string store;         // steam, epic, gog or imported
  std::string store_id;      // the store's game ID ("274520"), 1-64 of A-Z a-z 0-9 . _ -; empty for an imported game
  std::string name;          // the game's name, 1-120 printable characters
  std::string result;        // works, partial, broken or no-start
  std::string reason;        // optional: no-start, crash, slow, controls, other (not with works)
  std::string reason_text;   // optional, with reason "other" only: what it was, 1-120 printable characters on one line
  std::string details;       // optional: what happened, 0-500 printable characters on one line
  std::string installed_on;  // console, m2, usb, imported or other
  std::vector<std::string> controls;  // dualsense, mouse-preset, arrow-keys-preset, usb-mouse-keyboard
  std::string settings;      // the changed settings as Playden lists them, 0-2000 characters; empty: default
  std::string recipe_json;   // optional: the recipe export, at most 16 KB
  std::optional<RecipeRef> based_on;  // the community recipe in use
  std::vector<std::pair<std::string, std::string>> versions;  // summary.md's facts, at most 24
  std::string author;        // optional: the player's name, 1-40 printable characters
  std::string attachment;    // optional: an id UploadAttachment returned
  std::string kit, firmware, playden_version;  // required
  std::string idempotency_key;  // one per report attempt (NewIdempotencyKey)
};

struct ReportResult {
  bool ok = false;
  std::string id;         // the report's id (1-128 of A-Z a-z 0-9 _ -)
  std::string state;      // provisioning (no issue yet) or open
  std::string issue_url;  // empty while provisioning
  bool updated = false;   // added to this console's earlier report for the game and runtime kit version
  ApiError error;
};

struct AttachmentResult {
  bool ok = false;
  std::string id;
  std::string url;  // where the issue links it
  ApiError error;
};

std::string ReportRequestProblem(const ReportRequest& request);
std::string ReportBody(const ReportRequest& request);
// zip: a report zip (LogsSource::WriteReport), at most kMaxAttachmentBytes.
AttachmentResult UploadAttachment(const ApiContext& context, const std::string& secret, const std::string& zip,
                                  const std::string& idempotency_key, const std::atomic<bool>& cancel);
ReportResult SendReport(const ApiContext& context, const std::string& secret, const ReportRequest& request,
                        const std::atomic<bool>& cancel, const std::string& account_token = {});

// Linking an account (the OAuth device flow on the API host): Playden asks for a code, shows it with
// a QR code of verification_uri_complete, and polls until the player approved it on the account page.
inline constexpr const char* kAccountClientId = "playden-ps5";
struct AccountLinkStart {
  bool ok = false;
  std::string device_code;  // kept by the worker only
  std::string user_code;    // shown on screen, e.g. "WDJB-MJHT"
  std::string verification_uri_complete;
  std::int64_t interval = 5;     // seconds between polls
  std::int64_t expires_in = 900;
  ApiError error;
};
AccountLinkStart StartAccountLink(const ApiContext& context, const std::atomic<bool>& cancel);
enum class AccountLinkState { kPending, kSlowDown, kLinked, kExpired, kDenied, kError };
struct AccountLinkPoll {
  AccountLinkState state = AccountLinkState::kError;
  std::string token;  // kLinked: the session token, kept by the worker only
  ApiError error;
};
AccountLinkPoll PollAccountLink(const ApiContext& context, const std::string& device_code, const std::atomic<bool>& cancel);
struct LinkedAccount {
  bool ok = false;
  bool unlinked = false;  // 401 account_unlinked: the link ended (the token is forgotten)
  std::string name;
  bool trusted = false;
  ApiError error;
};
LinkedAccount GetLinkedAccount(const ApiContext& context, const std::string& token, const std::atomic<bool>& cancel);
bool UnlinkAccount(const ApiContext& context, const std::string& token, const std::atomic<bool>& cancel, ApiError* error);

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

// account_token (optional): the linked account, sent as X-Playden-Account (plan 2026-11-06 Phase D).
SubmissionResult SubmitRecipe(const ApiContext& context, const std::string& secret, const SubmissionRequest& request,
                              const std::atomic<bool>& cancel, const std::string& account_token = {});
// known_secret is never sent: an answer holding it is refused (bad_response).
SubmissionStatus GetSubmission(const ApiContext& context, const std::string& id, const std::atomic<bool>& cancel,
                               std::string_view known_secret = {});
VoteResult SendVote(const ApiContext& context, const std::string& secret, const VoteRequest& request,
                    const std::atomic<bool>& cancel);

}  // namespace playden::community
