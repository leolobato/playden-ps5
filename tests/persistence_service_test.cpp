#include "app/persistence_service.h"
#include "library/update_check.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>
#include "steam_id.h"

using namespace playden;
using namespace playden::app;
using namespace std::chrono_literals;

namespace {

class TemporaryDirectory {
 public:
  TemporaryDirectory() {
    char value[] = "/tmp/playden-persistence-service-XXXXXX";
    const char* created = mkdtemp(value);
    assert(created);
    path_ = created;
  }
  ~TemporaryDirectory() {
    std::remove((path_ + "/preferences.conf.tmp").c_str());
    std::remove((path_ + "/preferences.conf").c_str());
    rmdir(path_.c_str());
  }
  const std::string& path() const { return path_; }

 private:
  std::string path_;
};

class Repository final : public OrganizationRepository {
 public:
  mutable std::mutex mutex;
  mutable std::map<std::uint64_t, OrganizationSnapshot> snapshots;
  mutable std::vector<char> order;
  mutable std::atomic<int> load_calls{0}, save_calls{0}, clear_calls{0};
  mutable std::atomic<bool> block_load{false}, load_entered{false}, load_release{false};
  mutable std::atomic<bool> block_save{false}, save_entered{false}, save_release{false};
  mutable std::atomic<bool> block_clear{false}, clear_entered{false}, clear_release{false};
  StorageError load_error = StorageError::kNone;
  StorageError save_error = StorageError::kNone;
  StorageError clear_error = StorageError::kNone;

  Result<std::optional<OrganizationSnapshot>> Load(std::uint64_t account) const override {
    ++load_calls;
    load_entered = true;
    while (block_load && !load_release) std::this_thread::sleep_for(1ms);
    std::lock_guard<std::mutex> lock(mutex);
    order.push_back('L');
    if (load_error != StorageError::kNone)
      return Result<std::optional<OrganizationSnapshot>>::Failure(load_error, "load");
    const auto found = snapshots.find(account);
    return Result<std::optional<OrganizationSnapshot>>::Success(
        found == snapshots.end() ? std::optional<OrganizationSnapshot>{}
                                 : std::optional<OrganizationSnapshot>{found->second});
  }
  Result<void> Save(std::uint64_t account,
                    const OrganizationSnapshot& snapshot) const override {
    ++save_calls;
    save_entered = true;
    while (block_save && !save_release) std::this_thread::sleep_for(1ms);
    std::lock_guard<std::mutex> lock(mutex);
    order.push_back('S');
    if (save_error != StorageError::kNone)
      return Result<void>::Failure(save_error, "save");
    snapshots[account] = snapshot;
    return Result<void>::Success();
  }
  Result<void> Clear(std::uint64_t account) const override {
    ++clear_calls;
    clear_entered = true;
    while (block_clear && !clear_release) std::this_thread::sleep_for(1ms);
    std::lock_guard<std::mutex> lock(mutex);
    order.push_back('C');
    if (clear_error != StorageError::kNone)
      return Result<void>::Failure(clear_error, "clear");
    snapshots.erase(account);
    return Result<void>::Success();
  }
};

OrganizationSnapshot Snapshot(GameId favorite) {
  OrganizationSnapshot snapshot;
  snapshot.favorites.insert(favorite);
  snapshot.selected_game_id = favorite;
  return snapshot;
}

void Wait(const std::atomic<bool>& entered) {
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (!entered && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(1ms);
  assert(entered);
}

void WaitCount(const std::atomic<int>& value, int expected) {
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (value.load() < expected && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(1ms);
  assert(value.load() >= expected);
}

PersistenceEvent Await(PersistenceService& service, PersistenceEventKind kind,
                       PersistenceRequestId request = 0) {
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (std::chrono::steady_clock::now() < deadline) {
    for (auto& event : service.Pump(1)) {
      if (event.kind == kind && (!request || event.request_id == request)) return event;
    }
    std::this_thread::sleep_for(1ms);
  }
  assert(false && "persistence worker deadline exceeded");
  return {};
}

void PreferencesLoadSaveCoalesceAndShutdown() {
  TemporaryDirectory directory;
  Storage storage(directory.path());
  PreferencesStore preferences(storage);
  Repository organizations;
  organizations.block_load = true;
  PersistenceService service(preferences, organizations);
  service.LoadOrganization(11, 1);
  Wait(organizations.load_entered);
  Preferences first;
  const auto replaced = service.SavePreferences(first, 1);
  first.reduce_motion = true;
  const auto latest = service.SavePreferences(first, 2);
  assert(replaced && latest && replaced != latest);
  organizations.load_release = true;
  const auto saved = Await(service, PersistenceEventKind::kPreferencesSaved, latest);
  assert(saved.error == StorageError::kNone && saved.revision == 2);
  assert(preferences.Load().preferences.reduce_motion);
  assert(service.SavePreferences(Preferences{}, 1) == 0);

  Preferences final;
  const auto final_request = service.SavePreferences(final, 3);
  assert(final_request);
  service.Shutdown();
  assert(!preferences.Load().preferences.reduce_motion);
  const auto flushed = Await(service, PersistenceEventKind::kPreferencesSaved, final_request);
  assert(flushed.error == StorageError::kNone && service.LoadPreferences() == 0);
}

void PreferencesStatusesAndFailures() {
  {
    TemporaryDirectory directory;
    Storage storage(directory.path());
    PreferencesStore preferences(storage);
    Repository organizations;
    PersistenceService service(preferences, organizations);
    const auto request = service.LoadPreferences();
    const auto loaded = Await(service, PersistenceEventKind::kPreferencesLoaded, request);
    assert(loaded.preferences && !loaded.preferences->reduce_motion);
    assert(loaded.preferences_status == PreferencesLoadStatus::kDefaultMissing);
    assert(loaded.error == StorageError::kNone);
  }
  {
    TemporaryDirectory directory;
    const std::string blocker = directory.path() + "/not-a-directory";
    std::FILE* file = std::fopen(blocker.c_str(), "wb");
    assert(file);
    std::fclose(file);
    Storage storage(blocker);
    PreferencesStore preferences(storage);
    Repository organizations;
    PersistenceService service(preferences, organizations);
    auto loaded = Await(service, PersistenceEventKind::kPreferencesLoaded,
                        service.LoadPreferences());
    assert(loaded.preferences_status == PreferencesLoadStatus::kDefaultReadError);
    assert(loaded.error == StorageError::kReadFailed);
    Preferences value;
    const auto saved = Await(service, PersistenceEventKind::kPreferencesSaved,
                             service.SavePreferences(value, 4));
    assert(saved.error != StorageError::kNone);
    std::remove(blocker.c_str());
  }
}

void OrganizationCoalescingAndStaleLoad() {
  TemporaryDirectory directory;
  Storage storage(directory.path());
  PreferencesStore preferences(storage);
  Repository organizations;
  organizations.block_load = true;
  PersistenceService service(preferences, organizations);
  const auto stale = service.LoadOrganization(11, 1);
  assert(stale);
  Wait(organizations.load_entered);
  const auto old_save = service.SaveOrganization(11, 1, 1, Snapshot(SteamId(1)));
  const auto new_save = service.SaveOrganization(11, 1, 2, Snapshot(SteamId(2)));
  assert(old_save && new_save && old_save != new_save);
  service.InvalidateOrganization(2);
  organizations.load_release = true;
  std::this_thread::sleep_for(20ms);
  assert(service.Pump(32).empty());
  assert(organizations.save_calls == 1);
  {
    std::lock_guard<std::mutex> lock(organizations.mutex);
    assert(organizations.snapshots.at(11).favorites == std::set<GameId>{SteamId(2)});
  }

  organizations.load_entered = false;
  const auto current = service.LoadOrganization(22, 2);
  const auto loaded = Await(service, PersistenceEventKind::kOrganizationLoaded, current);
  assert(loaded.account_id == 22 && loaded.account_generation == 2);
  assert(!loaded.organization && loaded.error == StorageError::kNone);
}

void InvalidationPreservesAnAcceptedAccountSave() {
  TemporaryDirectory directory;
  Storage storage(directory.path());
  PreferencesStore preferences(storage);
  Repository organizations;
  organizations.block_load = true;
  PersistenceService service(preferences, organizations);
  assert(service.LoadOrganization(11, 1));
  Wait(organizations.load_entered);
  assert(service.SaveOrganization(11, 1, 1, Snapshot(SteamId(11))));

  service.InvalidateOrganization(2);
  assert(service.LoadOrganization(22, 2));
  organizations.load_release = true;
  service.WaitIdle();

  std::lock_guard<std::mutex> lock(organizations.mutex);
  assert(organizations.snapshots.at(11).favorites == std::set<GameId>{SteamId(11)});
}

void OrganizationSaveCoalesces() {
  TemporaryDirectory directory;
  Storage storage(directory.path());
  PreferencesStore preferences(storage);
  Repository organizations;
  organizations.block_load = true;
  PersistenceService service(preferences, organizations);
  service.LoadOrganization(33, 1);
  Wait(organizations.load_entered);
  const auto replaced = service.SaveOrganization(33, 1, 8, Snapshot(SteamId(8)));
  const auto latest = service.SaveOrganization(33, 1, 9, Snapshot(SteamId(9)));
  assert(service.SaveOrganization(33, 1, 8, Snapshot(SteamId(10))) == 0);
  organizations.load_release = true;
  const auto saved = Await(service, PersistenceEventKind::kOrganizationSaved, latest);
  assert(replaced && latest && saved.revision == 9 && organizations.save_calls == 1);
  std::lock_guard<std::mutex> lock(organizations.mutex);
  assert(organizations.snapshots[33].favorites == std::set<GameId>{SteamId(9)});
  assert(organizations.snapshots[33].selected_game_id == SteamId(9));
}

void TerminalCapacityRejectsWithoutChangingScope() {
  TemporaryDirectory directory;
  Storage storage(directory.path());
  PreferencesStore preferences(storage);
  Repository organizations;
  PersistenceService service(preferences, organizations);
  for (int index = 1; index <= 64; ++index) {
    PersistenceRequestId request = 0;
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!request && std::chrono::steady_clock::now() < deadline) {
      request = service.ClearOrganization(77, static_cast<std::uint64_t>(index));
      if (!request) std::this_thread::sleep_for(1ms);
    }
    assert(request);
    WaitCount(organizations.clear_calls, index);
  }
  std::this_thread::sleep_for(10ms);
  assert(service.LoadOrganization(88, 100) == 0);
  std::size_t terminals = 0;
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (terminals < 64 && std::chrono::steady_clock::now() < deadline) {
    for (const auto& event : service.Pump(64)) {
      assert(event.kind == PersistenceEventKind::kOrganizationCleared);
      ++terminals;
    }
    if (terminals < 64) std::this_thread::sleep_for(1ms);
  }
  assert(terminals == 64);
  const auto current = service.LoadOrganization(77, 64);
  assert(current);
  const auto loaded = Await(service, PersistenceEventKind::kOrganizationLoaded, current);
  assert(loaded.account_id == 77 && loaded.account_generation == 64);
}

void SaveThenClearBarrierAndFailure() {
  TemporaryDirectory directory;
  Storage storage(directory.path());
  PreferencesStore preferences(storage);
  Repository organizations;
  organizations.block_save = true;
  PersistenceService service(preferences, organizations);
  const auto save = service.SaveOrganization(44, 1, 1, Snapshot(SteamId(4)));
  Wait(organizations.save_entered);
  const auto clear = service.ClearOrganization(44, 2);
  assert(save && clear);
  assert(service.SaveOrganization(44, 2, 2, Snapshot(SteamId(5))) == 0);
  assert(service.LoadOrganization(44, 2) == 0);
  service.InvalidateOrganization(3);
  organizations.save_release = true;
  const auto cleared = Await(service, PersistenceEventKind::kOrganizationCleared, clear);
  assert(cleared.error == StorageError::kNone && cleared.account_generation == 2);
  {
    std::lock_guard<std::mutex> lock(organizations.mutex);
    assert(!organizations.snapshots.count(44));
    assert(organizations.order.size() >= 2);
    assert(organizations.order[organizations.order.size() - 2] == 'S');
    assert(organizations.order.back() == 'C');
  }

  organizations.clear_error = StorageError::kRemoveFailed;
  const auto failed_clear = service.ClearOrganization(44, 4);
  const auto failed = Await(service, PersistenceEventKind::kOrganizationCleared,
                            failed_clear);
  assert(failed.error == StorageError::kRemoveFailed);
}

void ShutdownDrainsOrganizationSaveAndClear() {
  TemporaryDirectory directory;
  Storage storage(directory.path());
  PreferencesStore preferences(storage);
  Repository organizations;
  organizations.block_save = true;
  PersistenceService service(preferences, organizations);
  service.SaveOrganization(55, 1, 7, Snapshot(SteamId(7)));
  Wait(organizations.save_entered);
  const auto clear = service.ClearOrganization(55, 2);
  std::thread shutdown([&] { service.Shutdown(); });
  std::this_thread::sleep_for(10ms);
  organizations.save_release = true;
  shutdown.join();
  const auto event = Await(service, PersistenceEventKind::kOrganizationCleared, clear);
  assert(event.error == StorageError::kNone);
  std::lock_guard<std::mutex> lock(organizations.mutex);
  assert(!organizations.snapshots.count(55));
}

void OrganizationReadWriteFailures() {
  TemporaryDirectory directory;
  Storage storage(directory.path());
  PreferencesStore preferences(storage);
  Repository organizations;
  PersistenceService service(preferences, organizations);
  organizations.load_error = StorageError::kInvalidData;
  auto loaded = Await(service, PersistenceEventKind::kOrganizationLoaded,
                      service.LoadOrganization(66, 1));
  assert(loaded.error == StorageError::kInvalidData && !loaded.organization);
  organizations.load_error = StorageError::kNone;
  organizations.save_error = StorageError::kWriteFailed;
  auto saved = Await(service, PersistenceEventKind::kOrganizationSaved,
                     service.SaveOrganization(66, 1, 2, Snapshot(SteamId(6))));
  assert(saved.error == StorageError::kWriteFailed && saved.revision == 2);
}

void WaitIdleDrainsAnAcceptedLoadWithoutStoppingWorker() {
  TemporaryDirectory directory;
  Storage storage(directory.path());
  PreferencesStore preferences(storage);
  Repository organizations;
  organizations.block_load = true;
  PersistenceService service(preferences, organizations);
  const auto load = service.LoadOrganization(91, 1);
  assert(load);
  Wait(organizations.load_entered);
  const auto save = service.SaveOrganization(91, 1, 1, Snapshot(SteamId(91)));
  assert(save);

  std::atomic<bool> idle{false};
  std::thread waiter([&] {
    service.WaitIdle();
    idle = true;
  });
  std::this_thread::sleep_for(10ms);
  assert(!idle);
  organizations.load_release = true;
  waiter.join();
  assert(idle);

  const auto loaded = Await(service, PersistenceEventKind::kOrganizationLoaded, load);
  assert(loaded.error == StorageError::kNone);
  assert(Await(service, PersistenceEventKind::kOrganizationSaved, save).error ==
         StorageError::kNone);
}

}  // namespace

// Each Steam account's update check is read and saved on the worker; without a store nothing is asked.
void UpdateChecksLoadAndSavePerAccount() {
  TemporaryDirectory directory;
  Storage storage(directory.path());
  PreferencesStore preferences(storage);
  Repository organizations;
  {
    PersistenceService without(preferences, organizations);
    assert(!without.LoadUpdateCheck(1) && !without.SaveUpdateCheck(1, 1, {}));
  }
  UpdateCheckStore updates(storage);
  PersistenceService service(preferences, organizations, &updates);
  auto loaded = Await(service, PersistenceEventKind::kUpdateCheckLoaded, service.LoadUpdateCheck(7));
  assert(loaded.error == StorageError::kNone && !loaded.update_check && loaded.account_id == 7);
  SavedUpdateCheck check;
  check.checked_at = 1'800'000'000;
  check.games[SteamId(440)] = {true, false, 12, 30, 99, true};
  const auto saved = Await(service, PersistenceEventKind::kUpdateCheckSaved, service.SaveUpdateCheck(7, 3, check));
  assert(saved.error == StorageError::kNone && saved.revision == 3 && saved.account_id == 7);
  loaded = Await(service, PersistenceEventKind::kUpdateCheckLoaded, service.LoadUpdateCheck(7));
  assert(loaded.update_check && *loaded.update_check == check);
  loaded = Await(service, PersistenceEventKind::kUpdateCheckLoaded, service.LoadUpdateCheck(8));
  assert(loaded.error == StorageError::kNone && !loaded.update_check);
  service.Shutdown();
  std::remove((directory.path() + "/updates-7.v2").c_str());
}

namespace {
// An update check repository that can hold the worker in a save.
class BlockingUpdates final : public UpdateCheckRepository {
 public:
  mutable std::mutex mutex;
  mutable std::map<std::uint64_t, SavedUpdateCheck> saved;
  mutable std::atomic<int> saves{0}, loads{0};
  mutable std::atomic<bool> block_save{false}, save_entered{false}, save_release{false};
  Result<std::optional<SavedUpdateCheck>> Load(std::uint64_t account) const override {
    ++loads;
    std::lock_guard<std::mutex> lock(mutex);
    const auto found = saved.find(account);
    return Result<std::optional<SavedUpdateCheck>>::Success(
        found == saved.end() ? std::optional<SavedUpdateCheck>{} : std::optional<SavedUpdateCheck>{found->second});
  }
  Result<void> Save(std::uint64_t account, const SavedUpdateCheck& check) const override {
    ++saves;
    save_entered = true;
    while (block_save && !save_release) std::this_thread::sleep_for(1ms);
    std::lock_guard<std::mutex> lock(mutex);
    saved[account] = check;
    return Result<void>::Success();
  }
};

SavedUpdateCheck CheckAt(std::int64_t when) {
  SavedUpdateCheck check;
  check.checked_at = when;
  return check;
}
}  // namespace

// Steam's and GOG's update checks go to their own repositories, even for the same account number: a GOG save never
// replaces a Steam save still waiting, and each event names its store.
void UpdateChecksPerStore() {
  TemporaryDirectory directory;
  Storage storage(directory.path());
  PreferencesStore preferences(storage);
  Repository organizations;
  BlockingUpdates steam, gog;
  {
    PersistenceService service(preferences, organizations, &steam);
    assert(!service.LoadUpdateCheck(Store::kGog, 7) && !service.SaveUpdateCheck(Store::kGog, 7, 1, {}));
    service.SetUpdateChecks(Store::kGog, &gog);
    service.SetUpdateChecks(Store::kGog, nullptr);
    assert(!service.LoadUpdateCheck(Store::kGog, 7) && "a removed repository takes no requests");
  }
  PersistenceService service(preferences, organizations, &steam);
  service.SetUpdateChecks(Store::kGog, &gog);
  steam.block_save = true;
  const auto first = service.SaveUpdateCheck(7, 1, CheckAt(1));
  Wait(steam.save_entered);
  const auto waiting = service.SaveUpdateCheck(Store::kSteam, 7, 2, CheckAt(2));
  const auto of_gog = service.SaveUpdateCheck(Store::kGog, 7, 5, CheckAt(5));
  const auto newer_gog = service.SaveUpdateCheck(Store::kGog, 7, 6, CheckAt(6));
  assert(first && waiting && of_gog && newer_gog);
  steam.save_release = true;
  std::map<PersistenceRequestId, PersistenceEvent> events;
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (events.size() < 3 && std::chrono::steady_clock::now() < deadline) {
    for (auto& event : service.Pump(8)) events[event.request_id] = std::move(event);
    std::this_thread::sleep_for(1ms);
  }
  assert(events.size() == 3 && events.count(first) && events.count(waiting) && events.count(newer_gog));
  assert(!events.count(of_gog) && "a GOG save replaces the GOG save still waiting");
  assert(events[first].store == Store::kSteam && events[waiting].store == Store::kSteam &&
         events[newer_gog].store == Store::kGog && events[newer_gog].account_id == 7 && events[newer_gog].revision == 6);
  assert(steam.saves.load() == 2 && gog.saves.load() == 1);
  {
    std::lock_guard<std::mutex> lock(steam.mutex);
    assert(steam.saved[7].checked_at == 2 && "the waiting Steam save was not replaced by GOG's");
  }
  {
    std::lock_guard<std::mutex> lock(gog.mutex);
    assert(gog.saved[7].checked_at == 6);
  }
  // Loads read each store's own repository.
  const auto loaded = Await(service, PersistenceEventKind::kUpdateCheckLoaded, service.LoadUpdateCheck(Store::kGog, 7));
  assert(loaded.store == Store::kGog && loaded.update_check && loaded.update_check->checked_at == 6 && gog.loads.load() == 1);
  const auto steam_loaded = Await(service, PersistenceEventKind::kUpdateCheckLoaded, service.LoadUpdateCheck(7));
  assert(steam_loaded.store == Store::kSteam && steam_loaded.update_check->checked_at == 2 && steam.loads.load() == 1);
  service.Shutdown();
}

int main() {
  PreferencesLoadSaveCoalesceAndShutdown();
  PreferencesStatusesAndFailures();
  OrganizationCoalescingAndStaleLoad();
  InvalidationPreservesAnAcceptedAccountSave();
  OrganizationSaveCoalesces();
  SaveThenClearBarrierAndFailure();
  ShutdownDrainsOrganizationSaveAndClear();
  OrganizationReadWriteFailures();
  TerminalCapacityRejectsWithoutChangingScope();
  WaitIdleDrainsAnAcceptedLoadWithoutStoppingWorker();
  UpdateChecksLoadAndSavePerAccount();
  UpdateChecksPerStore();
}
