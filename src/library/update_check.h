// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "library/model.h"
#include "platform/storage.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>

namespace playden {

// What a check found for one installed game: a newer version, DLC the install doesn't hold, the selection
// it updates to (0 unknown) and, once measured from the manifests (`sized`), what the update downloads and
// stages beside the game.
struct SavedGameUpdate {
  bool newer = false;
  bool new_dlc = false;
  std::uint64_t download_bytes = 0;
  std::uint64_t staged_bytes = 0;
  std::uint64_t target = 0;
  bool sized = false;
};
bool operator==(const SavedGameUpdate& left, const SavedGameUpdate& right);

// The last check of a store account's installed games for updates: when it last finished for all of them,
// and the games it found with an update (none without one), so Settings and the update list work after a
// restart without going online.
struct SavedUpdateCheck {
  std::int64_t checked_at = 0;  // unix seconds; 0: never finished
  std::map<GameId, SavedGameUpdate> games;
};
bool operator==(const SavedUpdateCheck& left, const SavedUpdateCheck& right);

class UpdateCheckRepository {
 public:
  virtual ~UpdateCheckRepository() = default;
  // Nothing saved for the account: an empty optional.
  virtual Result<std::optional<SavedUpdateCheck>> Load(std::uint64_t account_id) const = 0;
  virtual Result<void> Save(std::uint64_t account_id, const SavedUpdateCheck& check) const = 0;
};

// `<prefix><account>.v2` in the storage root, owner-only: "version=2", "checked_at=<seconds>", then one
// "<game key> <flags> <download bytes> <staged bytes> <target>" line per game (flags: 1 newer, 2 new DLC,
// 4 sized). Without it, `<prefix><account>.v1` ("version=1", lines "<game key> <flags> <bytes>" with Steam's
// whole-depot estimate) is read for its flags only. A damaged file fails the load with kInvalidData.
// Steam's prefix is "updates-" (its account number); GOG's is "updates-gog-" (its account key, a hash of the
// user ID). A load keeps the games of every store the file names: the reader takes its own store's.
class UpdateCheckStore final : public UpdateCheckRepository {
 public:
  explicit UpdateCheckStore(Storage& storage, std::string prefix = "updates-")
      : storage_(storage), prefix_(std::move(prefix)) {}
  Result<std::optional<SavedUpdateCheck>> Load(std::uint64_t account_id) const override;
  Result<void> Save(std::uint64_t account_id, const SavedUpdateCheck& check) const override;

 private:
  std::string Path(std::uint64_t account, int version = 2) const;

  Storage& storage_;
  std::string prefix_;
};

}  // namespace playden
