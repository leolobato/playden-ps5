#include "app/persistence_controller.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>
#include <unistd.h>
#include "steam_id.h"

using namespace playden;
using namespace playden::app;
using namespace playden::homebrew;
using namespace std::chrono_literals;

namespace {

class TemporaryDirectory {
 public:
  TemporaryDirectory() {
    char value[] = "/tmp/playden-persistence-controller-XXXXXX";
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
  mutable std::atomic<int> loads{0}, saves{0}, clears{0};
  mutable std::atomic<bool> block_load{false}, load_entered{false}, release_load{false};
  StorageError load_error = StorageError::kNone;
  StorageError save_error = StorageError::kNone;
  StorageError clear_error = StorageError::kNone;

  Result<std::optional<OrganizationSnapshot>> Load(std::uint64_t account) const override {
    ++loads;
    load_entered = true;
    while (block_load && !release_load) std::this_thread::sleep_for(1ms);
    std::lock_guard<std::mutex> lock(mutex);
    if (load_error != StorageError::kNone)
      return Result<std::optional<OrganizationSnapshot>>::Failure(load_error, "load");
    const auto found = snapshots.find(account);
    return Result<std::optional<OrganizationSnapshot>>::Success(
        found == snapshots.end() ? std::optional<OrganizationSnapshot>{}
                                 : std::optional<OrganizationSnapshot>{found->second});
  }
  Result<void> Save(std::uint64_t account,
                    const OrganizationSnapshot& snapshot) const override {
    ++saves;
    if (save_error != StorageError::kNone)
      return Result<void>::Failure(save_error, "save");
    std::lock_guard<std::mutex> lock(mutex);
    snapshots[account] = snapshot;
    return Result<void>::Success();
  }
  Result<void> Clear(std::uint64_t account) const override {
    ++clears;
    if (clear_error != StorageError::kNone)
      return Result<void>::Failure(clear_error, "clear");
    std::lock_guard<std::mutex> lock(mutex);
    snapshots.erase(account);
    return Result<void>::Success();
  }
};

template <class Predicate>
void Wait(PersistenceController& controller, Predicate predicate) {
  const auto deadline = std::chrono::steady_clock::now() + 3s;
  while (std::chrono::steady_clock::now() < deadline) {
    controller.Tick();
    if (predicate()) return;
    std::this_thread::sleep_for(1ms);
  }
  assert(false && "persistence controller deadline exceeded");
}

void Connect(Simulation& model, PersistenceController& controller,
             std::uint64_t account = 123) {
  model.UpdateAccount({AuthState::SignedIn, std::to_string(account), "Synthetic", {}, {}, false});
  model.UpdateLibrary({{{SteamId(42), "Synthetic game", {}, {}, {}, {}, {}}},
                       false, {}, false, false, {}});
  controller.Activate(account);
}

// Settings: Up and Down stay in a section; another section is reached through the rail.
static void enter_settings_section(playden::homebrew::Simulation& model, int index) {
  using playden::homebrew::Route;
  if (model.state().navigation.route != Route::Settings || !model.state().navigation.modals.empty()) return;
  const auto& rows = model.view().rows;
  const std::string wanted = rows[static_cast<std::size_t>(index)].section;
  const auto here = [&] { return rows[static_cast<std::size_t>(model.view().focused_row)].section; };
  if (here() == wanted) return;
  std::vector<std::string> order;
  for (const auto& row : rows) if (order.empty() || order.back() != row.section) order.push_back(row.section);
  const auto position = [&](const std::string& section) { return std::find(order.begin(), order.end(), section) - order.begin(); };
  model.action("left");
  for (int step = 0; step < 16 && here() != wanted; ++step) model.action(position(here()) < position(wanted) ? "down" : "up");
  model.action("right");
}
void Choose(Simulation& model, const std::string& action) {
  const auto& rows = model.view().rows;
  const auto found = std::find_if(rows.begin(), rows.end(),
                                  [&action](const ViewRow& row) {
                                    return row.action == action;
                                  });
  assert(found != rows.end());
  const int target = static_cast<int>(found - rows.begin());
  enter_settings_section(model, target);
  for (int step = 0; step < 80 && model.view().focused_row != target; ++step)
    assert(model.action(model.view().focused_row < target ? "down" : "up"));
  assert(model.view().focused_row == target);
  assert(model.action("cross"));
}

void OpenSettings(Simulation& model) {
  for (int step = 0; step < 4 && model.state().navigation.route != Route::Settings; ++step)
    assert(model.action("r1"));
  assert(model.state().navigation.route == Route::Settings);
}

void PendingLoadReplaysLocalEditAndSavesMergedSnapshot() {
  TemporaryDirectory directory;
  Storage storage(directory.path());
  PreferencesStore preferences(storage);
  Repository repository;
  OrganizationSnapshot stored;
  stored.favorites.insert(SteamId(7));
  repository.snapshots[kDeviceOrganization] = stored;
  repository.block_load = true;
  PersistenceService service(preferences, repository);
  Simulation model({}, {}, ModelMode::Live);
  PersistenceController controller(model, service);
  Connect(model, controller);
  while (!repository.load_entered) std::this_thread::sleep_for(1ms);

  assert(model.action("square"));
  assert(model.state().library.favorites.contains(SteamId(42)));
  repository.release_load = true;
  Wait(controller, [&] { return repository.saves.load() > 0; });

  assert(model.state().library.favorites == std::set<GameId>({SteamId(7), SteamId(42)}));
  std::lock_guard<std::mutex> lock(repository.mutex);
  assert(repository.snapshots.at(kDeviceOrganization).favorites == std::set<GameId>({SteamId(7), SteamId(42)}));
}

// Sign-out's organization step deletes nothing: organization belongs to the device.
void SignOutClearKeepsOrganization() {
  TemporaryDirectory directory;
  Storage storage(directory.path());
  PreferencesStore preferences(storage);
  Repository repository;
  OrganizationSnapshot stored;
  stored.favorites.insert(SteamId(42));
  repository.snapshots[kDeviceOrganization] = stored;
  PersistenceService service(preferences, repository);
  Simulation model({}, {}, ModelMode::Live);
  PersistenceController controller(model, service);
  Connect(model, controller);
  Wait(controller, [&] { return model.state().library.favorites.contains(SteamId(42)); });

  controller.Clear();
  const auto result = controller.TakeClearResult();
  assert(result && *result == StorageError::kNone);
  assert(!controller.TakeClearResult());
  controller.Reset();
  model.UpdateAccount({AuthState::SignedOut, {}, {}, {}, {}, true});
  controller.Tick();
  service.WaitIdle();
  assert(repository.clears.load() == 0);
  assert(model.state().library.favorites.contains(SteamId(42)));
  std::lock_guard<std::mutex> lock(repository.mutex);
  assert(repository.snapshots.at(kDeviceOrganization).favorites.contains(SteamId(42)));
}

// Switching Steam accounts keeps the organization, loads nothing again, and keeps saving it.
void AccountSwitchKeepsOrganization() {
  TemporaryDirectory directory;
  Storage storage(directory.path());
  PreferencesStore preferences(storage);
  Repository repository;
  OrganizationSnapshot stored;
  stored.favorites.insert(SteamId(42));
  stored.notes[SteamId(42)] = "kept";
  repository.snapshots[kDeviceOrganization] = stored;
  PersistenceService service(preferences, repository);
  Simulation model({}, {}, ModelMode::Live);
  PersistenceController controller(model, service);
  Connect(model, controller, 123);
  Wait(controller, [&] { return model.state().library.favorites.contains(SteamId(42)); });

  controller.Reset();
  model.UpdateAccount({AuthState::SignedIn, "456", "Other", {}, {}, true});
  model.UpdateLibrary({{{SteamId(42), "Synthetic game", {}, {}, {}, {}, {}},
                        {SteamId(43), "Second game", {}, {}, {}, {}, {}}},
                       false, {}, false, false, {}});
  controller.Activate(456);
  controller.Tick();
  assert(repository.loads.load() == 1);
  assert(model.state().library.favorites.contains(SteamId(42)));
  assert(model.state().library.notes.at(SteamId(42)) == "kept");

  assert(model.action("select:43"));
  assert(model.action("square"));
  Wait(controller, [&] { return repository.saves.load() > 0; });
  std::lock_guard<std::mutex> lock(repository.mutex);
  const auto& saved = repository.snapshots.at(kDeviceOrganization);
  assert(saved.favorites == std::set<GameId>({SteamId(42), SteamId(43)}));
  assert(saved.notes.at(SteamId(42)) == "kept");
}

// With no account at all, an edit is saved.
void SavesWithoutAnAccount() {
  TemporaryDirectory directory;
  Storage storage(directory.path());
  PreferencesStore preferences(storage);
  Repository repository;
  PersistenceService service(preferences, repository);
  Simulation model({}, {}, ModelMode::Live);
  PersistenceController controller(model, service);
  service.WaitIdle();
  controller.Tick();
  const GameId local = GameId::Local("0b8f6c1e-4d2a-4f3b-9a1c-2e5d7f8a9b0c");
  model.UpdateLibrary({{{local, "Imported game", {}, {}, {}, {}, {}}}, false, {}, false, false, {}, Store::kLocal});
  assert(model.action("select:" + ToKey(local)));
  assert(model.action("square"));
  Wait(controller, [&] { return repository.saves.load() > 0; });
  std::lock_guard<std::mutex> lock(repository.mutex);
  assert(repository.snapshots.at(kDeviceOrganization).favorites.contains(local));
}

void ShutdownCapturesTheLastEdit() {
  TemporaryDirectory directory;
  Storage storage(directory.path());
  PreferencesStore preferences(storage);
  Repository repository;
  PersistenceService service(preferences, repository);
  Simulation model({}, {}, ModelMode::Live);
  PersistenceController controller(model, service);
  Connect(model, controller);
  service.WaitIdle();
  controller.Tick();
  assert(model.action("select:42"));
  assert(model.action("square"));
  controller.FlushAndShutdown();

  std::lock_guard<std::mutex> lock(repository.mutex);
  assert(repository.snapshots.at(kDeviceOrganization).favorites.contains(SteamId(42)));
}

void ShutdownWaitsForLoadBeforeMergingFinalEdit() {
  TemporaryDirectory directory;
  Storage storage(directory.path());
  PreferencesStore preferences(storage);
  Repository repository;
  OrganizationSnapshot stored;
  stored.notes[SteamId(42)] = "stored note";
  repository.snapshots[kDeviceOrganization] = stored;
  repository.block_load = true;
  PersistenceService service(preferences, repository);
  Simulation model({}, {}, ModelMode::Live);
  PersistenceController controller(model, service);
  Connect(model, controller);
  while (!repository.load_entered) std::this_thread::sleep_for(1ms);
  assert(model.action("square"));

  std::thread release([&] {
    std::this_thread::sleep_for(20ms);
    repository.release_load = true;
  });
  controller.FlushAndShutdown();
  release.join();

  std::lock_guard<std::mutex> lock(repository.mutex);
  const auto& saved = repository.snapshots.at(kDeviceOrganization);
  assert(saved.notes.at(SteamId(42)) == "stored note");
  assert(saved.favorites.contains(SteamId(42)));
}

void InvalidMergedLoadStaysUnsavedUntilEditAndRetryResolveIt() {
  TemporaryDirectory directory;
  Storage storage(directory.path());
  PreferencesStore preferences(storage);
  Repository repository;
  OrganizationSnapshot stored;
  for (std::uint32_t id = 100; id < 10100; ++id) stored.favorites.insert(SteamId(id));
  assert(stored.favorites.size() == 10000);
  repository.snapshots[kDeviceOrganization] = stored;
  repository.block_load = true;
  PersistenceService service(preferences, repository);
  Simulation model({}, {}, ModelMode::Live);
  PersistenceController controller(model, service);
  Connect(model, controller);
  while (!repository.load_entered) std::this_thread::sleep_for(1ms);
  assert(model.action("square"));
  repository.release_load = true;

  Wait(controller, [&] { return !model.state().persistence_available; });
  assert(repository.saves.load() == 0);
  assert(model.state().library.favorites == std::set<GameId>{SteamId(42)});

  assert(model.action("square"));
  controller.Retry();
  Wait(controller, [&] {
    return model.state().persistence_available && repository.saves.load() > 0;
  });
  assert(model.state().library.favorites.size() == 10000);
  assert(!model.state().library.favorites.contains(SteamId(42)));
}

void InitialPreferencesFailureRetriesWithoutOverwritingUnknownData() {
  TemporaryDirectory directory;
  const std::string blocked = directory.path() + "/blocked";
  std::FILE* file = std::fopen(blocked.c_str(), "wb");
  assert(file);
  std::fclose(file);
  Storage storage(blocked);
  PreferencesStore preferences(storage);
  Repository repository;
  PersistenceService service(preferences, repository);
  Simulation model({}, {}, ModelMode::Live);
  PersistenceController controller(model, service);
  Wait(controller, [&] { return !model.state().persistence_available; });
  assert(access((blocked + "/preferences.conf").c_str(), F_OK) != 0);

  model.UpdateAccount({AuthState::SignedIn, "123", "Synthetic", {}, {}, false});
  OpenSettings(model);
  Choose(model, "toggle-motion");
  assert(model.state().reduced_motion);

  assert(std::remove(blocked.c_str()) == 0);
  assert(mkdir(blocked.c_str(), 0700) == 0);
  controller.Retry();
  Wait(controller, [&] { return model.state().persistence_available; });
  assert(model.state().reduced_motion);
  controller.FlushAndShutdown();
  assert(preferences.Load().preferences.reduce_motion);
  assert(std::remove((blocked + "/preferences.conf").c_str()) == 0);
  std::remove((blocked + "/preferences.conf.tmp").c_str());
  assert(rmdir(blocked.c_str()) == 0);
}

}  // namespace

// "Saved UI data is unavailable" names no file or reason, so the log does: one line per new failure, with the
// store's message, and one when the same kind of save works again.
void LogsSaveFailuresOnceAndRecovery() {
  TemporaryDirectory directory;
  Storage storage(directory.path());
  PreferencesStore preferences(storage);
  Repository repository;
  repository.save_error = StorageError::kWriteFailed;
  PersistenceService service(preferences, repository);
  Simulation model({}, {}, ModelMode::Live);
  PersistenceController controller(model, service);
  std::vector<std::string> lines;
  controller.SetLog([&lines](const std::string& line) { lines.push_back(line); });
  service.WaitIdle();
  controller.Tick();
  const GameId local = GameId::Local("0b8f6c1e-4d2a-4f3b-9a1c-2e5d7f8a9b0c");
  model.UpdateLibrary({{{local, "Imported game", {}, {}, {}, {}, {}}}, false, {}, false, false, {}, Store::kLocal});
  assert(model.action("select:" + ToKey(local)));
  assert(model.action("square"));
  Wait(controller, [&] { return !lines.empty(); });
  assert(lines.size() == 1 && lines[0] == "organization save failed: write failed: save");
  // The same failure again is not logged twice.
  assert(model.action("square"));
  const int saves = repository.saves.load();
  Wait(controller, [&] { return repository.saves.load() > saves; });
  service.WaitIdle();
  controller.Tick();
  assert(lines.size() == 1);
  repository.save_error = StorageError::kNone;
  assert(model.action("square"));
  Wait(controller, [&] { return lines.size() == 2; });
  assert(lines[1] == "organization save works again");
}

// Organization and preferences files a crash left empty or zero-filled (#907) don't keep "Saved UI data is
// unavailable" up for good: they are set aside, the log says so once, and edits are saved again.
void TornFilesAreSetAsideAndSavingResumes() {
  TemporaryDirectory directory;
  const auto write = [&](const char* name, const std::string& bytes) {
    std::FILE* file = std::fopen((directory.path() + "/" + name).c_str(), "wb");
    assert(file);
    assert(std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size());
    std::fclose(file);
  };
  write("organization.v2", std::string(512, '\0'));
  write("preferences.conf", "");
  Storage storage(directory.path());
  PreferencesStore preferences(storage);
  OrganizationStore organizations(storage);
  PersistenceService service(preferences, organizations);
  Simulation model({}, {}, ModelMode::Live);
  PersistenceController controller(model, service);
  std::vector<std::string> lines;
  controller.SetLog([&lines](const std::string& line) { lines.push_back(line); });
  service.WaitIdle();
  controller.Tick();
  assert(model.state().persistence_available);
  assert(std::none_of(model.state().notices.begin(), model.state().notices.end(),
                      [](const Notice& notice) { return notice.id == "persistence"; }));
  assert(std::count(lines.begin(), lines.end(),
                    "organization load: organization.v2 was damaged (512 bytes), kept as organization.v2.damaged; "
                    "started again") == 1);
  assert(std::count(lines.begin(), lines.end(),
                    "preferences load: preferences.conf was damaged (0 bytes), kept as preferences.conf.damaged; "
                    "defaults restored") == 1);
  const GameId local = GameId::Local("0b8f6c1e-4d2a-4f3b-9a1c-2e5d7f8a9b0c");
  model.UpdateLibrary({{{local, "Imported game", {}, {}, {}, {}, {}}}, false, {}, false, false, {}, Store::kLocal});
  assert(model.action("select:" + ToKey(local)));
  assert(model.action("square"));
  OpenSettings(model);
  Choose(model, "toggle-motion");
  controller.FlushAndShutdown();
  const auto saved = organizations.Load(kDeviceOrganization);
  assert(saved.ok() && saved.value() && saved.value()->favorites.count(local) == 1);
  assert(preferences.Load().preferences.reduce_motion);
  for (const char* name : {"organization.v2", "organization.v2.damaged", "preferences.conf.damaged"})
    std::remove((directory.path() + "/" + name).c_str());
}

namespace {
class UpdateRepository final : public UpdateCheckRepository {
 public:
  mutable std::mutex mutex;
  mutable std::map<std::uint64_t, SavedUpdateCheck> saved;
  mutable std::atomic<int> saves{0};
  std::atomic<bool> fail_saves{false};
  Result<std::optional<SavedUpdateCheck>> Load(std::uint64_t) const override {
    return Result<std::optional<SavedUpdateCheck>>::Success(std::nullopt);
  }
  Result<void> Save(std::uint64_t account, const SavedUpdateCheck& check) const override {
    ++saves;
    if (fail_saves) return Result<void>::Failure(StorageError::kWriteFailed, "save");
    std::lock_guard<std::mutex> lock(mutex);
    saved[account] = check;
    return Result<void>::Success();
  }
};
}  // namespace

// A failed save of the update check is submitted again when the player asks to retry.
void UpdateCheckSaveRetries() {
  TemporaryDirectory directory;
  Storage storage(directory.path());
  PreferencesStore preferences(storage);
  Repository repository;
  UpdateRepository updates;
  updates.fail_saves = true;
  PersistenceService service(preferences, repository, &updates);
  Simulation model({}, {}, ModelMode::Live);
  PersistenceController controller(model, service);
  model.SetWallClock(1760000000);
  Connect(model, controller);
  Wait(controller, [&] { return model.state().update_check.loaded; });
  assert(model.action("update-check") && model.TakeUpdateCheckRequest());
  model.FinishUpdateCheck(false);
  Wait(controller, [&] { return updates.saves.load() == 1; });
  service.WaitIdle();
  updates.fail_saves = false;
  for (int i = 0; i < 20; ++i) { controller.Tick(); std::this_thread::sleep_for(1ms); }
  assert(updates.saves.load() == 1 && model.UpdateCheckDirty());
  controller.Retry();
  Wait(controller, [&] { return updates.saves.load() == 2; });
  service.WaitIdle();
  controller.Tick();
  std::lock_guard<std::mutex> lock(updates.mutex);
  assert(updates.saved[123].checked_at == model.state().update_check.checked_at && !model.UpdateCheckDirty());
}

int main() {
  PendingLoadReplaysLocalEditAndSavesMergedSnapshot();
  SignOutClearKeepsOrganization();
  AccountSwitchKeepsOrganization();
  SavesWithoutAnAccount();
  ShutdownCapturesTheLastEdit();
  ShutdownWaitsForLoadBeforeMergingFinalEdit();
  InvalidMergedLoadStaysUnsavedUntilEditAndRetryResolveIt();
  InitialPreferencesFailureRetriesWithoutOverwritingUnknownData();
  LogsSaveFailuresOnceAndRecovery();
  TornFilesAreSetAsideAndSavingResumes();
  UpdateCheckSaveRetries();
}
