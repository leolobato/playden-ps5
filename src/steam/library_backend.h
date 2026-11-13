#pragma once

#include "library/model.h"
#include "steam/session_store.h"

#include <atomic>
#include <cstdint>
#include <functional>
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
};

}  // namespace playden::steam
