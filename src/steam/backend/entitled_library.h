#pragma once

#include "steam/backend/cm_client.h"
#include "steam/library_backend.h"

#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace playden::steam {

// The owned library from GetOwnedGames, enriched with CM acquisition dates. As in the macOS
// reference, both run in parallel and a CM failure is never fatal: games then simply lack a date.
// A CM logon rejection never reports revoked credentials.
// GetOwnedGames leaves out free-to-play games added to the library but never played. Licensed
// packages none of whose apps are listed are looked up in PICS, and their games and demos (marked)
// are added. Each app is looked up once per run, so after the first load a refresh only asks about
// new licenses.
class EntitledLibraryBackend final : public LibraryBackend {
 public:
  EntitledLibraryBackend(LibraryBackend& owned, AcquisitionSource& acquisitions)
      : owned_(owned), acquisitions_(acquisitions) {}

  LibraryResult Fetch(const StoredSession& session, const std::atomic<bool>& cancel) override;
  // Receives each successful license read, for size estimates.
  void SetLicensedDepots(LicensedDepots* licensed) { licensed_ = licensed; }

  // Category of the most recent CM attempt, for diagnostics without identifiers.
  CmError last_acquisition_error() const { return last_error_.load(); }

 private:
  void AddLicensedGames(const StoredSession& session, const AcquisitionResult& acquisitions,
                        const std::atomic<bool>& cancel, LibraryResult* result);

  LibraryBackend& owned_;
  AcquisitionSource& acquisitions_;
  LicensedDepots* licensed_ = nullptr;
  std::atomic<CmError> last_error_{CmError::kNone};
  // App ID -> its name when PICS calls it a game or a demo, empty when it is something else or
  // undescribed. Demos are also in demos_.
  std::mutex looked_up_mutex_;
  std::unordered_map<std::uint32_t, std::string> looked_up_;
  std::unordered_map<std::uint32_t, std::string> icons_;
  std::unordered_set<std::uint32_t> demos_;
};

}  // namespace playden::steam
