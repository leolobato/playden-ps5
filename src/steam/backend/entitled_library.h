#pragma once

#include "steam/backend/cm_client.h"
#include "steam/backend/family_groups.h"
#include "steam/library_backend.h"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace playden::steam {

// The owned library from GetOwnedGames, enriched with CM acquisition dates. As in the macOS
// reference, both run in parallel and a CM failure is never fatal: games then simply lack a date.
// A CM logon rejection never reports revoked credentials.
// GetOwnedGames leaves out free-to-play games added to the library but never played. Licensed
// packages none of whose apps are listed are looked up in PICS, and their games and demos (marked)
// are added. Each app is looked up once per run, so after the first load a refresh only asks about
// new licenses. Apps only a Steam family member's licenses grant are marked family_shared.
class EntitledLibraryBackend final : public LibraryBackend {
 public:
  EntitledLibraryBackend(LibraryBackend& owned, AcquisitionSource& acquisitions)
      : owned_(owned), acquisitions_(acquisitions) {}
  // Stops a running family probe and waits for it.
  ~EntitledLibraryBackend() override;

  LibraryResult Fetch(const StoredSession& session, const std::atomic<bool>& cancel) override;
  // Receives each successful license read, for size estimates.
  void SetLicensedDepots(LicensedDepots* licensed) { licensed_ = licensed; }

  // Writes "family" lines: which family-shared apps the Library got or left out, and what Steam's family
  // service lists, asked over HTTPS (with this CA bundle) on a thread of its own until it answers once this
  // run. A load starts it when none runs and never waits for it. Purely diagnostic: its failures are log
  // lines. Off until set; set it before the first Fetch, as it is not synchronized with a running one.
  void SetFamilyDiagnostics(std::string ca_bundle, FamilyLog log);
  // For tests: the family service requests go to `perform`.
  void SetFamilyDiagnostics(FamilyPerform perform, FamilyLog log);

  // Category of the most recent CM attempt, for diagnostics without identifiers.
  CmError last_acquisition_error() const { return last_error_.load(); }

 private:
  // True when this load started the probe; it then owes the probe its licenses.
  bool StartFamilyProbe(const StoredSession& session);
  void StopFamilyProbe();
  // Hands the license sets of the load that started the probe (or that it had none) to the probe.
  void PublishFamilyLicenses(const AcquisitionResult* licenses);
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
  // Shared apps PICS left out -> why, and the shared apps whose outcome is already logged this run.
  std::unordered_map<std::uint32_t, std::string> family_left_out_;
  std::set<std::uint32_t> family_logged_;
  // Shared apps whose failed PICS lookup is already logged; later failures only count as pending.
  std::set<std::uint32_t> family_failed_logged_;

  FamilyLog family_log_;
  FamilyPerform family_perform_;
  std::string family_ca_bundle_;
  // The probe works on copies; these outlive it because the destructor joins it. Starting and stopping it
  // take family_start_mutex_.
  std::mutex family_start_mutex_;
  std::thread family_thread_;
  std::atomic<bool> family_running_{false};
  std::atomic<bool> family_answered_{false};
  std::atomic<bool> family_stop_{false};
  // The license sets of the load that started the probe, for the comparison, once it published them.
  std::mutex family_mutex_;
  std::condition_variable family_cv_;
  bool family_licenses_published_ = false;
  bool family_licenses_ok_ = false;
  std::set<std::uint32_t> family_shared_apps_;
  std::set<std::uint32_t> family_owned_apps_;
};

}  // namespace playden::steam
