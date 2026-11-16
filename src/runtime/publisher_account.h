#pragma once

#include "runtime/publisher_manifest.h"
#include "runtime/publisher_pack.h"

#include <cstdint>
#include <string>
#include <string_view>

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
};

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
