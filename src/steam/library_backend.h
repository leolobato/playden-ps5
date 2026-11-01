#pragma once

#include "library/model.h"
#include "steam/session_store.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <vector>

namespace playden::steam {

enum class LibraryError {
  kNone,
  kCanceled,
  kNetwork,
  kProtocol,
  kCredentialsRevoked,
  kNoSession,
  kPersistence,
};

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
  // Called on the library worker, during Fetch, with the owned games once they are known. The
  // library shows them while licenses are still being read. Empty unless a refresh set one.
  void SetFetchProgress(std::function<void(LibraryResult)> progress) { progress_ = std::move(progress); }
  virtual LibraryResult Fetch(const StoredSession& session,
                              const std::atomic<bool>& cancel) = 0;
  // Licenses for the games already listed: family-shared and never-played free games are merged in.
  // The default leaves the list as it is and reads nothing.
  virtual LibraryResult ReadLicenses(const StoredSession& /*session*/, std::vector<playden::GameSummary> games,
                                     const std::atomic<bool>& /*cancel*/) {
    return {LibraryError::kNone, std::move(games), false, false};
  }
  // Games named after Fetch returned (looked up in the background), for that load's account; each is
  // handed over once. None by default.
  virtual std::vector<playden::GameSummary> TakeLateGames(std::uint64_t /*steam_id*/) { return {}; }
  // Called from the backend's own thread when TakeLateGames has games. It must only queue work.
  virtual void SetLateGamesCallback(std::function<void()> /*callback*/) {}

 protected:
  void ReportFetchProgress(const LibraryResult& result) {
    if (progress_) progress_(result);
  }

 private:
  std::function<void(LibraryResult)> progress_;
};

}  // namespace playden::steam
