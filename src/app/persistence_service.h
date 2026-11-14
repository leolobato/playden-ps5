#pragma once

#include "app/preferences.h"
#include "library/organization.h"
#include "library/update_check.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace playden::app {

using PersistenceRequestId = std::uint64_t;

enum class PersistenceEventKind {
  kPreferencesLoaded,
  kPreferencesSaved,
  kOrganizationLoaded,
  kOrganizationSaved,
  kOrganizationCleared,
  kUpdateCheckLoaded,
  kUpdateCheckSaved,
};

struct PersistenceEvent {
  PersistenceEventKind kind = PersistenceEventKind::kPreferencesLoaded;
  PersistenceRequestId request_id = 0;
  std::uint64_t account_id = 0;
  Store store = Store::kSteam;  // update checks: the store whose account `account_id` is
  std::uint64_t account_generation = 0;
  std::uint64_t revision = 0;
  StorageError error = StorageError::kNone;
  // The store's reason when the load or save failed. After a load that worked: what the store recovered from
  // first (a damaged file set aside), else empty.
  std::string message;
  PreferencesLoadStatus preferences_status = PreferencesLoadStatus::kLoaded;
  std::optional<Preferences> preferences;
  std::optional<OrganizationSnapshot> organization;
  std::optional<SavedUpdateCheck> update_check;  // loaded; empty when the account has none saved
};

// A single worker owns all preferences, organization and update check IO.
// Dependencies outlive the service. Accepted saves and organization clears are durable
// shutdown work; loads may be discarded when stale or during shutdown.
class PersistenceService {
 public:
  // `updates` is Steam's update check repository. A store without one has its update checks neither loaded nor
  // saved (their requests return 0).
  PersistenceService(PreferencesStore&, OrganizationRepository&, UpdateCheckRepository* updates = nullptr);
  // Another store's update check repository (GOG's), set before its first request; nullptr removes it.
  void SetUpdateChecks(Store store, UpdateCheckRepository* updates);
  ~PersistenceService();
  PersistenceService(const PersistenceService&) = delete;
  PersistenceService& operator=(const PersistenceService&) = delete;

  PersistenceRequestId LoadPreferences();
  PersistenceRequestId SavePreferences(Preferences preferences,
                                       std::uint64_t revision);
  PersistenceRequestId LoadOrganization(std::uint64_t account_id,
                                        std::uint64_t account_generation);
  PersistenceRequestId SaveOrganization(std::uint64_t account_id,
                                        std::uint64_t account_generation,
                                        std::uint64_t revision,
                                        OrganizationSnapshot snapshot);
  PersistenceRequestId ClearOrganization(std::uint64_t account_id,
                                         std::uint64_t account_generation);
  void InvalidateOrganization(std::uint64_t account_generation);
  // A store account's last update check. A save replaces one of the same store and account still waiting.
  // Without a store, Steam's.
  PersistenceRequestId LoadUpdateCheck(Store store, std::uint64_t account_id);
  PersistenceRequestId SaveUpdateCheck(Store store, std::uint64_t account_id, std::uint64_t revision,
                                       SavedUpdateCheck check);
  PersistenceRequestId LoadUpdateCheck(std::uint64_t account_id) { return LoadUpdateCheck(Store::kSteam, account_id); }
  PersistenceRequestId SaveUpdateCheck(std::uint64_t account_id, std::uint64_t revision, SavedUpdateCheck check) {
    return SaveUpdateCheck(Store::kSteam, account_id, revision, std::move(check));
  }
  // Waits for all currently accepted work to finish without stopping the
  // worker. Used by shutdown coordination when a load must be merged before
  // the final save is submitted.
  void WaitIdle();
  void Shutdown();
  std::vector<PersistenceEvent> Pump(std::size_t max_events);

 private:
  enum class Command {
    kLoadPreferences,
    kSavePreferences,
    kLoadOrganization,
    kSaveOrganization,
    kClearOrganization,
    kLoadUpdateCheck,
    kSaveUpdateCheck,
  };
  struct Job {
    Command command = Command::kLoadPreferences;
    PersistenceRequestId request = 0;
    std::uint64_t account = 0;
    Store store = Store::kSteam;
    UpdateCheckRepository* updates = nullptr;  // the store's, resolved when the job is accepted
    std::uint64_t generation = 0;
    std::uint64_t revision = 0;
    Preferences preferences;
    OrganizationSnapshot organization;
    SavedUpdateCheck update_check;
  };

  PersistenceRequestId Submit(Job);
  static bool IsOrganization(Command);
  static bool IsLoad(Command);
  void Run();
  void Execute(Job);
  void PushEvent(PersistenceEvent, bool clear_terminal = false);
  bool ClearRequest(PersistenceRequestId) const;

  PreferencesStore& preferences_;
  OrganizationRepository& organizations_;
  std::map<Store, UpdateCheckRepository*> updates_;  // guarded by mutex_
  mutable std::mutex mutex_;
  std::condition_variable wake_;
  std::condition_variable idle_;
  std::deque<Job> pending_;
  std::optional<Job> active_;
  std::deque<PersistenceEvent> events_;
  std::deque<PersistenceRequestId> clear_requests_;
  PersistenceRequestId next_request_ = 0;
  std::uint64_t preferences_revision_ = 0;
  std::uint64_t organization_generation_ = 0;
  std::uint64_t organization_account_ = 0;
  std::uint64_t organization_revision_ = 0;
  bool clear_barrier_ = false;
  bool stopping_ = false;
  bool joined_ = false;
  std::thread worker_;
};

}  // namespace playden::app
