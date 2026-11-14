#pragma once

#include "app/persistence_service.h"
#include "homebrew/simulation.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>

namespace playden::app {

// Main-thread bridge between the renderer-neutral model and persistence IO.
// PersistenceService and Simulation outlive this object.
class PersistenceController {
 public:
  PersistenceController(homebrew::Simulation&, PersistenceService&);

  // Organization is device-wide (kDeviceOrganization): it loads at
  // construction and is saved whether or not a store account is signed in.
  // A store account arriving loads its own update check from that store's repository. Without a store, Steam.
  void Activate(Store store, std::uint64_t account_id);
  void Activate(std::uint64_t account_id) { Activate(Store::kSteam, account_id); }

  // A store account left: saves pending edits; preferences, organization and the other stores' update checks
  // stay, this store's update check leaves the model. Without a store, Steam.
  void Reset(Store store);
  void Reset() { Reset(Store::kSteam); }
  void Tick();
  void Retry();

  // Sign-out step: organization outlives accounts, so this only saves pending
  // edits and reports success. The result remains available until consumed.
  void Clear();
  std::optional<StorageError> TakeClearResult();

  // Resolves required startup loads, captures the final model revisions, and
  // drains accepted saves before stopping the worker.
  void FlushAndShutdown();

  // Receives one line when preferences or organization fail to load or save, and one when they work again
  // ("Saved UI data is unavailable" says nothing else). A repeat of the same failure is not logged again.
  void SetLog(std::function<void(const std::string&)> log) { log_ = std::move(log); }

 private:
  void Pump(std::size_t maximum);
  void ObserveAndSave();
  void BeginPreferencesLoad();
  void BeginOrganizationLoad();
  void Apply(PersistenceEvent);
  enum Channel {
    kPreferencesLoad, kPreferencesSave, kOrganizationLoad, kOrganizationSave, kUpdateCheckLoad, kUpdateCheckSave, kChannels
  };
  // failure: empty when the channel worked.
  void Report(Channel channel, const std::string& failure);

  homebrew::Simulation& model_;
  PersistenceService& service_;
  // Always 0: device-wide organization is never invalidated by an account change.
  std::uint64_t organization_generation_ = 0;
  PersistenceRequestId preferences_load_request_ = 0;
  PersistenceRequestId organization_load_request_ = 0;
  homebrew::PersistenceLoadToken preferences_load_token_;
  homebrew::PersistenceLoadToken organization_load_token_;
  std::uint64_t submitted_preferences_revision_ = 0;
  std::uint64_t submitted_organization_revision_ = 0;
  // Per store: the account whose update check the model holds, its load, and the revision last submitted.
  struct UpdateSlot {
    std::uint64_t account = 0;
    PersistenceRequestId load_request = 0;
    std::uint64_t submitted_revision = 0;
  };
  std::map<Store, UpdateSlot> update_slots_;
  std::optional<StorageError> clear_result_;
  bool preferences_load_failed_ = false;
  bool organization_load_failed_ = false;
  bool shutting_down_ = false;
  std::function<void(const std::string&)> log_;
  std::string reported_[kChannels];  // the last failure logged per channel; empty once it worked again
};

}  // namespace playden::app
