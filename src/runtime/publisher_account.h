#pragma once

#include "runtime/publisher_manifest.h"
#include "runtime/publisher_pack.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace playden::runtime {

// A publisher account on the console: the credential blob the player's sign-in delivered
// (opaque JSON, read only through the stage manifest's account spec), kept privately in the
// Steam state root as publisher-account-<slug>.v1 (0600, replaced atomically). Playden never
// checks it over the network: a launch that finds it expired leaves a status file, and the
// account becomes Expired (display name kept, blob dropped) until the player signs in again.
enum class PublisherAccountState { kSignedOut, kSignedIn, kExpired };

struct PublisherAccount {
  PublisherAccountState state = PublisherAccountState::kSignedOut;
  std::string display;          // the account's display field ("" when the blob has none)
  std::uint64_t obtained_at = 0;  // unix seconds of the sign-in
  std::uint64_t renewed_at = 0;   // unix seconds of the last refresh (RefreshPublisherAccount), else obtained_at
};

// One refresh request and its answer, for the transport the caller provides (HTTPS in Playden, a
// fake in tests). The request's Cookie header holds secrets: the transport never logs it.
struct PublisherRefreshRequest {
  std::string url;
  std::vector<std::pair<std::string, std::string>> headers;
};
struct PublisherRefreshAnswer {
  bool reached = false;  // false: no HTTP answer (network, TLS, timeout)
  long status = 0;
  std::vector<std::pair<std::string, std::string>> headers;  // all of them, Set-Cookie included, in order
  std::string body;
};
using PublisherRefreshTransport = std::function<PublisherRefreshAnswer(const PublisherRefreshRequest&)>;

enum class PublisherRefreshResult {
  kRefreshed,        // the sign-in now holds the answer's fields and cookies
  kExpired,          // the server says the sign-in is gone: the account is Expired
  kFailed,           // nothing changed (no answer, an answer Playden can't use, or a sign-in in the meantime)
  kNotRefreshable,   // the sign-in lacks the refresh cookies (pasted, or from an older page)
  kSignedOut,        // no signed-in account
};
struct PublisherRefreshOutcome {
  PublisherRefreshResult result = PublisherRefreshResult::kFailed;
  long status = 0;     // the HTTP status, when there was an answer
  std::string reason;  // for the log; never holds a secret
};

const char* PublisherRefreshResultName(PublisherRefreshResult result);

// "steamhost", "ea", "rockstar" or "ubisoft": the name used in file names and recipes.
const char* PublisherSlug(Publisher publisher);
// publisher-account-<slug>.v1
std::string PublisherAccountFileName(Publisher publisher);

// The sign-in sink. Accepts a JSON object of at most spec.max_bytes whose first present stage
// field is a string matching the spec's pattern, and stores it whole, replacing any account.
// display receives the first present display field. Copies of the blob are wiped; the caller
// wipes its own. On refusal, reason is a sentence for the player.
bool AcceptPublisherCredential(const std::string& state_root, Publisher publisher, std::string_view blob,
                               const PublisherAccountSpec& spec, std::uint64_t now, std::string* display,
                               std::string* reason);

// The stored account; a missing or damaged file is kSignedOut.
PublisherAccount LoadPublisherAccount(const std::string& state_root, Publisher publisher);

// The stage value of a signed-in account, for the credential file. The caller wipes it
// (steam::ZeroizeSecret) once written.
bool PublisherStageValue(const std::string& state_root, Publisher publisher, const PublisherAccountSpec& spec,
                         std::string* value, std::string* reason);

// Drops the blob and keeps the display name and sign-in time. Nothing to do when signed out.
bool MarkPublisherAccountExpired(const std::string& state_root, Publisher publisher, std::string* reason);

// Renews a signed-in account with the pack's refresh recipe: one request carrying the recipe's
// cookies (read from the sign-in), then the answer's recipe fields and renewed cookies replace the
// old ones, and renewed_at becomes now. The display name and sign-in time stay. An answer whose
// status is one of refresh.expired_statuses marks the account expired. The account is re-read
// before writing: a sign-in, sign-out or expiry in the meantime wins and the result is kFailed.
// Every copy of a secret is wiped.
PublisherRefreshOutcome RefreshPublisherAccount(const std::string& state_root, Publisher publisher,
                                                const PublisherAccountSpec& account, const PublisherRefreshSpec& refresh,
                                                const PublisherRefreshTransport& transport, std::uint64_t now);

// The publisher titles whose "not tested yet" notice the player went past (PublisherTitleSpec::untested), by
// Steam app id: publisher-untested-<slug>.v1, one decimal id per line. It isn't part of the account: signing
// out keeps it. A missing or damaged file holds none, so the notice shows again.
std::string PublisherUntestedFileName(Publisher publisher);
bool PublisherUntestedNoticeSeen(const std::string& state_root, Publisher publisher, std::uint32_t app);
bool NotePublisherUntestedNoticeSeen(const std::string& state_root, Publisher publisher, std::uint32_t app,
                                     std::string* reason);

// Deletes the account. Signed out already is success.
bool SignOutPublisherAccount(const std::string& state_root, Publisher publisher, std::string* reason);

// After a session of a staged game: reads <title_folder>/<status.file> (links not followed).
// When its trimmed content equals status.expired the account is marked expired and *expired
// is set; when it equals status.missing (the game started without a sign-in) *missing is set
// once the file is gone, and the account stays as it is. The file is deleted once read. A missing
// file changes nothing.
bool ApplyPublisherSessionStatus(const std::string& state_root, Publisher publisher, const std::string& title_folder,
                                 const PublisherStatusSpec& status, bool* expired, std::string* reason,
                                 bool* missing = nullptr);

}  // namespace playden::runtime
