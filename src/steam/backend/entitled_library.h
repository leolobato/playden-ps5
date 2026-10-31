#pragma once

#include "steam/backend/app_names.h"
#include "steam/backend/cm_client.h"
#include "steam/backend/family_groups.h"
#include "steam/library_backend.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace playden::steam {

// The owned library from GetOwnedGames, enriched with CM acquisition dates. As in the macOS
// reference, both run in parallel and a CM failure is never fatal: games then simply lack a date.
// A CM logon rejection never reports revoked credentials.
// GetOwnedGames leaves out free-to-play games added to the library but never played. The apps of licensed
// packages none of whose apps are listed are added when they are games or demos (marked). Apps only a Steam
// family member's licenses grant are marked family_shared.
// A load never waits for app lookups. It names apps from Steam's family service (the family's games), from
// saved names, and leaves the rest to a background lookup (PICS, then Steam's HTTP host by hash), which hands
// each game it names to the Library as TakeLateGames. Names are saved, so each app is looked up once.
class EntitledLibraryBackend final : public LibraryBackend {
 public:
  EntitledLibraryBackend(LibraryBackend& owned, AcquisitionSource& acquisitions)
      : owned_(owned), acquisitions_(acquisitions) {}
  // Stops a running family probe and waits for it.
  ~EntitledLibraryBackend() override;

  LibraryResult Fetch(const StoredSession& session, const std::atomic<bool>& cancel) override;
  std::vector<GameSummary> TakeLateGames(std::uint64_t steam_id) override;
  void SetLateGamesCallback(std::function<void()> callback) override;
  // Where app names are saved (app-names.cache); without it they last for the run. Set before the first Fetch.
  void SetAppNameStorage(const Storage* storage);
  // For tests: waits until no background lookup runs. False on timeout.
  bool WaitForLookups(std::chrono::milliseconds timeout);
  // For tests: how long a load waits for the family service's games (4 s by default).
  void SetFamilyWait(std::chrono::milliseconds wait) { family_wait_ = wait; }
  // Receives each successful license read, for size estimates.
  void SetLicensedDepots(LicensedDepots* licensed) { licensed_ = licensed; }
  // The licensed DLC apps already looked up that name `app` as their game, sorted, whoever's licenses they
  // came from (names are public data). Steam's listofdlc stops at 64 entries; this finds the rest.
  std::vector<std::uint32_t> DlcOf(std::uint32_t app);

  // Asks Steam's family service over HTTPS (with this CA bundle; the background lookup's HTTP uses it too)
  // and writes "family" lines: which family-shared apps the Library got or left out, and what the service
  // lists. The service is asked on a thread of its own by each load (none while one runs); a load waits for
  // it a few seconds at most, and its failures are log lines. Off until set; set it before the first Fetch,
  // as it is not synchronized with a running one.
  void SetFamilyDiagnostics(std::string ca_bundle, FamilyLog log);
  // For tests: the family service and the by-hash app info requests go to `perform`.
  void SetFamilyDiagnostics(FamilyPerform perform, FamilyLog log);

  // Category of the most recent CM attempt, for diagnostics without identifiers.
  CmError last_acquisition_error() const { return last_error_.load(); }

 private:
  // True when this load started the probe; it then owes the probe its licenses.
  bool StartFamilyProbe(const StoredSession& session);
  void StopFamilyProbe();
  // Hands the license sets of the load that started the probe (or that it had none) to the probe.
  void PublishFamilyLicenses(const AcquisitionResult* licenses);
  // The family service's apps for this account: this load's answer (waited for until `started` + the
  // family wait), or the last one. None without an answer for the account.
  // `fresh` says the answer is this load's own; only then does it tell which shared apps are no games.
  std::optional<std::map<std::uint32_t, SharedLibraryApp>> FamilyApps(std::uint64_t steam_id, bool probing,
                                                                      std::chrono::steady_clock::time_point started,
                                                                      bool* fresh);
  void AddLicensedGames(const StoredSession& session, const AcquisitionResult& acquisitions,
                        const std::map<std::uint32_t, SharedLibraryApp>* family, bool family_fresh,
                        const std::atomic<bool>& cancel, LibraryResult* result);
  void LoadNamesLocked();
  void SaveNames();
  void StartLookups(const StoredSession& session, std::vector<std::uint32_t> apps);
  void StopLookups();
  void RunLookups(const StoredSession& session, const std::vector<std::uint32_t>& apps);
  bool Record(std::uint32_t app, const std::string& text);
  void RecordOther(std::uint32_t app, const std::string& reason);
  void PublishLateGames();

  LibraryBackend& owned_;
  AcquisitionSource& acquisitions_;
  LicensedDepots* licensed_ = nullptr;
  std::atomic<CmError> last_error_{CmError::kNone};
  // What each looked-up app is, loaded from names_storage_ on the first load. The load and the background
  // lookup take names_mutex_ for it and for the late games.
  std::mutex names_mutex_;
  const Storage* names_storage_ = nullptr;
  AppNames names_;
  bool names_loaded_ = false;
  bool names_dirty_ = false;
  std::unordered_set<std::uint32_t> demos_;
  // The last load's licensed apps, for games the background lookup names after it returned.
  struct LateContext {
    std::uint64_t steam_id = 0;
    std::set<std::uint32_t> shared;
    std::map<std::uint32_t, std::uint32_t> acquired_at;
    std::map<std::uint32_t, std::uint32_t> family_acquired;
    std::set<std::uint32_t> wanted;
  };
  LateContext late_context_;
  std::vector<GameSummary> late_games_;
  std::mutex late_callback_mutex_;
  std::function<void()> late_callback_;
  std::mutex lookup_start_mutex_;
  std::thread lookup_thread_;
  std::atomic<bool> lookup_stop_{false};
  std::atomic<bool> lookup_running_{false};
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
  // The family compared with licenses in the log this run.
  std::atomic<bool> family_answered_{false};
  // The account the family service said has no family: not asked again this run.
  std::atomic<std::uint64_t> family_no_group_{0};
  std::chrono::milliseconds family_wait_{4000};
  std::atomic<bool> family_stop_{false};
  // The license sets of the load that started the probe, for the comparison, once it published them.
  std::mutex family_mutex_;
  std::condition_variable family_cv_;
  bool family_licenses_published_ = false;
  bool family_licenses_ok_ = false;
  std::set<std::uint32_t> family_shared_apps_;
  std::set<std::uint32_t> family_owned_apps_;
  // The family service's last answer (family_mutex_), and whether this load's probe has answered.
  std::uint64_t family_web_account_ = 0;
  std::map<std::uint32_t, SharedLibraryApp> family_web_apps_;
  bool family_probe_done_ = true;
  bool family_web_fresh_ = false;  // family_web_apps_ came from the probe started last
};

}  // namespace playden::steam
