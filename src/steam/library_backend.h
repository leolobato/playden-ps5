#pragma once

#include "library/model.h"
#include "steam/session_store.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace playden::steam {

using LibraryError = playden::LibraryError;

struct LibraryResult {
  LibraryError error = LibraryError::kProtocol;
  std::vector<playden::GameSummary> games;
  bool metadata_partial = false;
  // The account's licenses were read with the games (Steam's CM answered): the library says what it owns.
  bool licenses_read = false;
};

class LibraryBackend {
 public:
  virtual ~LibraryBackend() = default;
  virtual LibraryResult Fetch(const StoredSession& session,
                              const std::atomic<bool>& cancel) = 0;
  // Games named after Fetch returned (looked up in the background), for that load's account; each is
  // handed over once. None by default.
  virtual std::vector<playden::GameSummary> TakeLateGames(std::uint64_t /*steam_id*/) { return {}; }
  // Called from the backend's own thread when TakeLateGames has games. It must only queue work.
  virtual void SetLateGamesCallback(std::function<void()> /*callback*/) {}
  // The account's collections in the store, after a successful Fetch with the same session. None when the store
  // couldn't give them (or the backend doesn't read them); a library load never fails for them.
  virtual std::optional<std::vector<playden::LibraryCollection>> FetchCollections(const StoredSession& /*session*/,
                                                                                const std::atomic<bool>& /*cancel*/) {
    return std::nullopt;
  }
};

}  // namespace playden::steam
