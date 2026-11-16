#include "runtime/settings_publication.h"
#include "runtime/settings_publication_owner.h"
#include "platform/managed_file_lock.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <unistd.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <csignal>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%d: %s\n", __LINE__, #x); std::exit(1); } } while (0)
#include "settings_publication_fixture.h"
namespace fs = std::filesystem;
PackageActionResult Ok() { return {PackageActionState::kComplete, {}, {}}; }
PackageActionResult Fail() { return {PackageActionState::kFailed, "injected", "injected failure"}; }
std::string Read(const fs::path& path) { std::ifstream file(path, std::ios::binary); return {std::istreambuf_iterator<char>(file), {}}; }
struct Test {
  fs::path root, control; SettingsPublicationOperation operation; PublicationPaths paths; PublicationDependencies deps;
  unsigned blocks = 0, preflights = 0, applies = 0, verifies = 0, reconciles = 0, activities = 0;
  bool ready = false, fail_apply = false, fail_verify = false, fail_reconcile = false, stopped = true;
  Test() {
    char pattern[] = "/tmp/playden-publication-XXXXXX"; CHECK(mkdtemp(pattern)); root = fs::canonical(pattern);
    std::string reason; auto frozen = FreezeSettingsPublicationOperation(Fixture(), &reason); CHECK(frozen); operation = *frozen;
    const auto data = root / "data/playden/games/install-220"; control = data / ".playden-control";
    fs::create_directories(control); CHECK(chmod(control.c_str(), 0700) == 0);
    auto directory = playden::platform::ManagedDirectory::Open(control, playden::platform::DirectoryPolicy::kPrivate); CHECK(directory);
    int fd = directory->CreateExclusiveFile("publication.lock", 0600); CHECK(fd >= 0 && fsync(fd) == 0 && close(fd) == 0 && directory->Sync());
    CHECK(chmod(data.c_str(), 0700) == 0);
    for (const auto& suffix : {"library", "library/prefixes", "library/prefixes/game"}) {
      fs::create_directories(data / suffix); CHECK(chmod((data / suffix).c_str(), 0700) == 0);
    }
    std::ofstream(data / "library/prefixes/game/user.reg") << "WINE REGISTRY Version 2\n";
    CHECK(chmod((data / "library/prefixes/game/user.reg").c_str(), 0644) == 0);
    paths = {data / "library/profiles/playden-steam-220.profile", data / "library/profiles/profiles.lst",
             data / "library/input", root / "data/homebrew/FAKE78001/game-launch.json", data / "library/profiles/.playden-publication"};
    // Title/game staging precedes settings publication; the production writer
    // must never create or restage this directory.
    fs::create_directories(paths.descriptor.parent_path() / "games/220");
    deps.host_namespace_prefix = root;
    deps.activity = [&](const RuntimeInstall&) { ++activities; return stopped ? NativeActivity::kStopped : NativeActivity::kUnknown; };
    deps.block_owner = [&](const auto&) { CheckLease(); ++blocks; ready = false; return Ok(); };
    deps.preflight = [&](const auto& operation, const auto& p, int borrowed) {
      CHECK(borrowed >= 0); CheckLease(); ++preflights;
      return PreflightSettingsPublication(operation, p, borrowed, root);
    };
    deps.apply_files = [&](const auto& o, const auto&, int borrowed) {
      CHECK(borrowed >= 0); CheckLease(); CHECK(!ready && Read(control / "publication-state.v1") == o.pending_receipt_bytes);
      ++applies;
      auto result = ApplyPendingPublicationFiles(o, paths, borrowed, root);
      if (result.state != PackageActionState::kComplete) return result;
      return fail_apply ? Fail() : Ok();
    };
    deps.verify_files = [&](const auto& o, const auto&, int borrowed, bool committed) {
      CHECK(borrowed >= 0); CheckLease(); CHECK(!ready);
      CHECK(Read(control / "publication-state.v1") == (committed ? o.committed_receipt_bytes : o.pending_receipt_bytes));
      ++verifies; return fail_verify ? Fail() : VerifySettingsPublicationFiles(o, paths, borrowed, committed, root);
    };
    deps.reconcile_committed = [&](const auto& o) {
      CheckLease(); CHECK(!ready && Read(control / "publication-state.v1") == o.committed_receipt_bytes);
      ++reconciles; if (fail_reconcile) return Fail(); ready = true; return Ok();
    };
  }
  ~Test() { fs::remove_all(root); }
  void CheckLease() {
    auto directory = playden::platform::ManagedDirectory::Open(control, playden::platform::DirectoryPolicy::kPrivate); CHECK(directory);
    CHECK(!playden::platform::ManagedFileLock::Acquire(*directory, "publication.lock", playden::platform::LockMode::kReader));
  }
  std::string Registry() { return Read(root / "data/playden/games/install-220/library/prefixes/game/user.reg"); }
  void ConfigureRegistry() {
    const auto file = root / "data/playden/games/install-220/library/prefixes/game/user.reg";
    std::ofstream(file) << "WINE REGISTRY Version 2\n\n[Software\\\\Unrelated] 42\n\"Keep\"=\"value\"\n";
    auto source = Fixture();
    source.plan.registry = {{"Software\\Wine\\AppDefaults\\probe.exe", "Version", "\"win7\""}};
    source.registry_unix_time = 1760000000;
    auto backup = CreatePublicationRegistryBackup(control, source.transaction_id, Registry(), 0644);
    CHECK(backup.ok()); source.registry_backup = backup.value(); Refresh(source);
    std::string reason; auto frozen = FreezeSettingsPublicationOperation(source, &reason); CHECK(frozen); operation = *frozen;
  }
  fs::path DxvkPath() { return paths.descriptor.parent_path() / "games/220/dxvk.conf"; }
  void ConfigureDxvk(const std::string& before, const std::string& desired, unsigned mode = 0644) {
    auto source = Fixture(); source.plan.dxvk_config = desired;
    fs::create_directories(DxvkPath().parent_path());
    if (!before.empty()) {
      // Effect-only fixtures explicitly authorize their captured old DXVK file.
      // Production preflight requires prior committed ownership instead.
      deps.preflight = [&](const auto&, const auto&, int borrowed) {
        CHECK(borrowed >= 0); CheckLease(); ++preflights; return Ok();
      };
      std::ofstream(DxvkPath()) << before; CHECK(chmod(DxvkPath().c_str(), mode) == 0);
      source.dxvk_before = {true, mode, before.size(), *HashArtifactBytes(before), before};
    }
    Refresh(source); std::string reason;
    auto frozen = FreezeSettingsPublicationOperation(source, &reason); CHECK(frozen); operation = *frozen;
  }
  PackageActionResult Run() { return PublishSettingsOperation(operation, paths, deps); }
};
struct OwnerPackage final : PackageInstaller {
  PackageStatus status{true, true, true, true, true, true, true, false, 0};
  unsigned mutations = 0;
  PackageStatus Inspect(const RuntimeInstall&) override { return status; }
  PackageActionResult StageTitle(const RuntimeInstall&) override { ++mutations; return Fail(); }
  PackageActionResult CopyTemplate(const RuntimeInstall&) override { ++mutations; return Fail(); }
  PackageActionResult ValidateArtifacts(const RuntimeInstall&) override { ++mutations; return Fail(); }
  PackageActionResult PublishProfile(const RuntimeInstall&) override { ++mutations; return Fail(); }
};
struct OwnerNative final : NativeCoordinator {
  NativeActivity activity = NativeActivity::kStopped;
  NativeStatus status{true, false, true, true, false, false};
  unsigned registrations = 0; bool queue = false, fail = false; InstallRecordStore* records = nullptr;
  NativeActivity Activity(const std::string&) override { return activity; }
  NativeStatus Query(const RuntimeInstall&) override { return status; }
  RegistrationResult Register(const RuntimeInstall& record) override {
    auto stored = records->Find(record.install_id); CHECK(stored && stored->journal && stored->journal->step == "register");
    CHECK(record.state == stored->state && record.settings_revision == stored->settings_revision && record.journal);
    CHECK(record.journal->operation_id == stored->journal->operation_id &&
          record.journal->operation == stored->journal->operation && record.journal->step == stored->journal->step);
    CHECK(stored->state == PreparationState::kRegistering); ++registrations;
    if (fail) return {RegistrationResultState::kFailed, "submit-failed", "injected native submission failure"};
    if (queue) status.scan_pending = true; else status.installed = true;
    return {queue ? RegistrationResultState::kScanQueued : RegistrationResultState::kAccepted, {}, {}};
  }
};
struct RejectReadyStore final : InstallRecordStore {
  InstallRecordStore& store; bool reject = false; std::string reject_control_step;
  explicit RejectReadyStore(InstallRecordStore& s) : store(s) {}
  std::optional<RuntimeInstall> Find(const std::string& id) const override { return store.Find(id); }
  std::vector<RuntimeInstall> Snapshot() const override { return store.Snapshot(); }
  RecordUpdateResult Update(const RuntimeInstall& record) override {
    if (record.journal && record.journal->operation == "settings-control" && record.journal->step == reject_control_step)
      return {false, "control-write", "injected control intent persistence refusal"};
    if (reject && record.state == PreparationState::kReady) return {false, "ready-write", "injected Ready persistence refusal"};
    return store.Update(record);
  }
};
void SeedOwner(RuntimeInstallRepository& repository, const RuntimeInstall& r, bool fixture_control_initialized = true) {
  CHECK(repository.Load().ok()); ReserveRequest request;
  request.install_id = r.install_id; request.runtime_number = r.runtime_number; request.default_profile_id = r.default_profile_id;
  request.runtime_bundle_id = r.runtime_bundle_id; request.recipe_id = r.recipe_id; request.recipe_version = r.recipe_version;
  request.settings_revision = r.settings_revision; request.launch_entry_id = r.launch_entry_id; request.launch_path = r.launch_path;
  request.launch_arguments = r.launch_arguments; request.launch_working_directory = r.launch_working_directory;
  request.install_path = r.install_path; request.volume_id = r.volume_id; request.kit_title_variants = {r.title_id};
  CHECK(repository.Reserve(request).ok());
  auto owned = r;
  // Most publication fixtures created their control/lock before seeding OWNER.
  // Record that test-only authorized setup; initializer tests opt out and prove
  // the actual production creation and durable intent themselves.
  if (fixture_control_initialized) {
    owned.state = PreparationState::kPreparing;
    owned.journal = PreparationJournal{Fixture().transaction_id, "settings-control", "control-ready"};
  }
  CHECK(repository.Update(owned).ok());
}
int main() {
  {
    Test t; auto missing = t.deps; missing.verify_files = {};
    CHECK(PublishSettingsOperation(t.operation, t.paths, missing).state == PackageActionState::kFailed && t.blocks == 0);
    t.stopped = false; CHECK(t.Run().state == PackageActionState::kFailed && t.blocks == 0);
    t.stopped = true; auto wrong = t.paths; wrong.profile = t.root / "foreign";
    CHECK(PublishSettingsOperation(t.operation, wrong, t.deps).state == PackageActionState::kFailed && t.blocks == 0);
    // One "game stopped" check per apply, under the lease and before any effect; a
    // title started from Home would need the lease, so later answers are not asked for.
    t.deps.activity = [&](const auto&) { return ++t.activities == 1 ? NativeActivity::kStopped : NativeActivity::kActive; };
    t.activities = 0; CHECK(t.Run().state == PackageActionState::kComplete && t.activities == 1 && t.ready);
  }
  {
    Test t; auto directory = playden::platform::ManagedDirectory::Open(t.control, playden::platform::DirectoryPolicy::kPrivate); CHECK(directory);
    auto reader = playden::platform::ManagedFileLock::Acquire(*directory, "publication.lock", playden::platform::LockMode::kReader); CHECK(reader);
    CHECK(t.Run().code == "publication-busy" && t.blocks == 0 && t.applies == 0);
    CHECK(!fs::exists(t.control / "settings-operation.v1") && !fs::exists(t.control / "publication-state.v1"));
    reader.reset(); CHECK(t.Run().state == PackageActionState::kComplete && t.ready);
  }
  // Reconstruct/reload after every coordinator durability boundary. Files are
  // actual native verification runs on fixture files; production mutator
  // durability/interruption tests remain a later gate.
  for (unsigned failure = 0; failure < 7; ++failure) {
    Test t;
    t.deps.after_step = [failure](PublicationStep step) { return static_cast<unsigned>(step) != failure; };
    CHECK(t.Run().state == PackageActionState::kFailed && !t.ready);
    const auto before = t.applies;
    const bool committed = Read(t.control / "publication-state.v1") == t.operation.committed_receipt_bytes;
    t.deps.after_step = {};
    CHECK(t.Run().state == PackageActionState::kComplete && t.ready);
    CHECK(Read(t.control / "publication-state.v1") == t.operation.committed_receipt_bytes);
    CHECK(LoadSettingsPublicationOperation(t.control).state == PublicationRecoveryState::kValid);
    if (committed) CHECK(t.applies == before); // Ready/commit retry never reapplies settings.
  }
  // Abrupt process exit bypasses RAII and leaves only the durable filesystem
  // evidence. A different process must acquire the released kernel lease and
  // reconstruct the operation before completing it.
  for (unsigned failure = 0; failure < 7; ++failure) {
    Test t;
    const auto child = fork(); CHECK(child >= 0);
    if (child == 0) {
      t.deps.after_step = [failure](PublicationStep step) {
        if (static_cast<unsigned>(step) == failure) _exit(77);
        return true;
      };
      t.Run(); _exit(78);
    }
    int status = 0; CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 77);
    const bool committed = Read(t.control / "publication-state.v1") == t.operation.committed_receipt_bytes;
    CHECK(t.Run().state == PackageActionState::kComplete && t.ready);
    CHECK(t.applies == (committed ? 0u : 1u));
    CHECK(Read(t.control / "publication-state.v1") == t.operation.committed_receipt_bytes);
  }
  {
    Test t; t.deps.block_owner = [](const auto&) { return Fail(); };
    CHECK(t.Run().state == PackageActionState::kFailed && t.applies == 0 && !fs::exists(t.control / "settings-operation.v1"));
  }
  {
    Test t;
    fs::create_directories(t.paths.profile.parent_path()); CHECK(chmod(t.paths.profile.parent_path().c_str(), 0700) == 0);
    // A profile an earlier, interrupted first setup left behind is no reason to stop.
    std::ofstream(t.paths.profile) << "stale profile\n"; CHECK(chmod(t.paths.profile.c_str(), 0644) == 0);
    CHECK(t.Run().state == PackageActionState::kComplete && t.applies == 1 && t.ready);
    CHECK(Read(t.paths.profile) == t.operation.plan.profile_bytes);
  }
  {
    Test t; t.deps.after_step = [&](PublicationStep step) {
      if (step == PublicationStep::kFilesApplied) std::ofstream(t.paths.input_directory / "gamepad.input") << "changed";
      return true;
    };
    CHECK(t.Run().code == "publication-verification" && !t.ready && t.reconciles == 0);
    CHECK(Read(t.control / "publication-state.v1") == t.operation.pending_receipt_bytes);
  }
  {
    Test t; t.deps.after_step = [&](PublicationStep step) {
      if (step == PublicationStep::kOwnerBlocked) {
        fs::rename(t.control / "publication.lock", t.control / "retired-lock");
        std::ofstream(t.control / "publication.lock"); CHECK(chmod((t.control / "publication.lock").c_str(), 0600) == 0);
      }
      return true;
    };
    CHECK(t.Run().state == PackageActionState::kFailed && t.applies == 0 && !fs::exists(t.control / "settings-operation.v1"));
  }
  {
    Test t; t.fail_apply = true; CHECK(t.Run().state == PackageActionState::kFailed && !t.ready);
    CHECK(Read(t.control / "publication-state.v1") == t.operation.pending_receipt_bytes && t.reconciles == 0);
    t.fail_apply = false; CHECK(t.Run().state == PackageActionState::kComplete && t.ready);
  }
  {
    Test t; t.fail_verify = true; CHECK(t.Run().state == PackageActionState::kFailed && !t.ready);
    CHECK(Read(t.control / "publication-state.v1") == t.operation.pending_receipt_bytes && t.reconciles == 0);
  }
  {
    Test t; t.fail_reconcile = true; CHECK(t.Run().state == PackageActionState::kFailed && !t.ready);
    CHECK(Read(t.control / "publication-state.v1") == t.operation.committed_receipt_bytes);
    const auto applies = t.applies; t.fail_reconcile = false; t.fail_verify = true;
    CHECK(t.Run().state == PackageActionState::kFailed && !t.ready && t.applies == applies);
    t.fail_verify = false;
    CHECK(t.Run().state == PackageActionState::kComplete && t.ready && t.applies == applies);
  }
  {
    Test t; t.deps.after_step = [](PublicationStep step) { return step != PublicationStep::kJournalDurable; };
    CHECK(t.Run().state == PackageActionState::kFailed && !t.ready);
    auto source = Fixture(); source.install.launch_arguments = "changed"; std::string reason;
    auto changed = FreezeSettingsPublicationOperation(source, &reason); CHECK(changed); t.operation = *changed; t.deps.after_step = {};
    CHECK(t.Run().code == "publication-drift" && t.applies == 0);
  }
  {
    Test t; // Foreign receipts, including exact desired commits without a journal, grant no retry ownership.
    playden::Storage storage(t.control.string()); auto bytes = t.operation.committed_receipt_bytes;
    CHECK(storage.WriteDurable("publication-state.v1", playden::Bytes(bytes.begin(), bytes.end())).ok());
    CHECK(t.Run().code == "publication-collision" && t.applies == 0 && !t.ready);
    CHECK(Read(t.control / "publication-state.v1") == bytes);
  }
  {
    Test t; std::ofstream(t.control / "settings-operation.v1") << "corrupt";
    CHECK(chmod((t.control / "settings-operation.v1").c_str(), 0600) == 0);
    CHECK(t.Run().code == "publication-journal" && t.applies == 0 && !t.ready);
  }
  {
    Test t; t.ConfigureRegistry();
    const auto original = t.Registry(); std::string intended = original, reason;
    CHECK(ApplyRegistryValues(&intended, t.operation.plan.registry, t.operation.registry_unix_time, &reason));
    CHECK(t.Run().state == PackageActionState::kComplete && t.ready && t.Registry() == intended);
    auto backup = LoadPublicationRegistryBackup(t.control, t.operation.transaction_id, *t.operation.registry_backup);
    CHECK(backup.state == PublicationRecoveryState::kValid && backup.bytes == original);
    auto directory = playden::platform::ManagedDirectory::Open(t.control, playden::platform::DirectoryPolicy::kPrivate); CHECK(directory);
    auto lease = playden::platform::ManagedFileLock::Acquire(*directory, "publication.lock", playden::platform::LockMode::kPublisher); CHECK(lease);
    CHECK(ApplyPendingPublicationRegistry(t.operation, lease->fd(), t.root).code == "publication-registry-ownership");
    CHECK(t.Registry() == intended); // committed state never grants mutation permission
  }
  {
    Test t; t.ConfigureRegistry();
    CHECK(ApplyPendingPublicationRegistry(t.operation, -1, t.root).code == "publication-lock");
    const auto file = t.root / "data/playden/games/install-220/library/prefixes/game/user.reg";
    std::ofstream(file) << "WINE REGISTRY Version 2\n; changed after snapshot\n";
    const auto foreign = t.Registry();
    CHECK(t.Run().code == "publication-preflight-snapshot" && !t.ready && t.Registry() == foreign);
    CHECK(!fs::exists(t.control / "publication-state.v1") && !fs::exists(t.control / "settings-operation.v1"));
  }
  {
    Test t; t.ConfigureRegistry();
    const auto child = fork(); CHECK(child >= 0);
    if (child == 0) {
      t.deps.apply_files = [&](const auto& operation, const auto&, int borrowed) {
        CHECK(ApplyPendingPublicationRegistry(operation, borrowed, t.root).state == PackageActionState::kComplete);
        _exit(77); return Fail();
      };
      t.Run(); _exit(78);
    }
    int status = 0; CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 77);
    CHECK(Read(t.control / "publication-state.v1") == t.operation.pending_receipt_bytes);
    CHECK(!fs::exists(t.paths.profile)); // registry was durably published before later files
    const auto applied = t.Registry(); CHECK(applied.find("1760000000") != std::string::npos);
    CHECK(t.Run().state == PackageActionState::kComplete && t.ready && t.Registry() == applied);
  }
  {
    Test t; t.ConfigureRegistry(); const auto original = t.Registry();
    const auto child = fork(); CHECK(child >= 0);
    if (child == 0) {
      t.deps.after_step = [](PublicationStep step) {
        if (step == PublicationStep::kPendingDurable) {
          CHECK(std::signal(SIGXFSZ, SIG_IGN) != SIG_ERR);
          struct rlimit limit{32, 32}; CHECK(setrlimit(RLIMIT_FSIZE, &limit) == 0);
        }
        return true;
      };
      const auto result = t.Run();
      _exit(result.code == "publication-registry-write" ? 77 : 78);
    }
    int status = 0; CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 77);
    CHECK(t.Registry() == original && Read(t.control / "publication-state.v1") == t.operation.pending_receipt_bytes);
    CHECK(!fs::exists(t.paths.profile));
    CHECK(t.Run().state == PackageActionState::kComplete && t.ready);
    auto backup = LoadPublicationRegistryBackup(t.control, t.operation.transaction_id, *t.operation.registry_backup);
    CHECK(backup.state == PublicationRecoveryState::kValid && backup.bytes == original);
  }
  {
    EffectiveSettings settings;
    for (const auto* id : {"frame_limit", "vsync", "frame_latency", "overlay_detail", "reported_vram"}) settings.values[id].available = false;
    settings.values["frame_limit"].available = true; settings.values["frame_limit"].value = "60";
    const auto rendered = RenderDxvkConfig(settings); CHECK(!rendered.empty());
    Test t; t.ConfigureDxvk("", rendered);
    CHECK(t.Run().state == PackageActionState::kComplete && Read(t.DxvkPath()) == rendered);
    auto foreign = Fixture(); foreign.plan.dxvk_config = "# Generated by Playden from this game's settings. foreign header\n";
    Refresh(foreign); std::string reason; CHECK(!FreezeSettingsPublicationOperation(foreign, &reason));
  }
  const std::string dxvk_old = std::string("# Generated by Playden from this game's settings.") + "\ndxgi.maxFrameRate = 30\n";
  const std::string dxvk_new = std::string("# Generated by Playden from this game's settings.") + "\ndxgi.maxFrameRate = 60\n";
  for (const unsigned mode : {0600u, 0644u}) {
    Test t; t.ConfigureDxvk(dxvk_old, dxvk_new, mode);
    CHECK(t.Run().state == PackageActionState::kComplete && t.ready && Read(t.DxvkPath()) == dxvk_new);
    struct stat info{}; CHECK(stat(t.DxvkPath().c_str(), &info) == 0 && (info.st_mode & 07777) == mode);
  }
  {
    Test t; t.ConfigureDxvk("", dxvk_new);
    CHECK(t.Run().state == PackageActionState::kComplete && t.ready && Read(t.DxvkPath()) == dxvk_new);
  }
  {
    Test t; t.ConfigureDxvk(dxvk_old, "");
    CHECK(t.Run().state == PackageActionState::kComplete && t.ready && !fs::exists(t.DxvkPath()));
  }
  {
    Test t; t.ConfigureDxvk(dxvk_old, dxvk_new);
    std::ofstream(t.DxvkPath()) << "foreign";
    CHECK(t.Run().code == "publication-dxvk-collision" && !t.ready && Read(t.DxvkPath()) == "foreign");
    CHECK(Read(t.control / "publication-state.v1") == t.operation.pending_receipt_bytes);
  }
  for (const bool remove : {false, true}) {
    Test t; t.ConfigureDxvk(dxvk_old, remove ? "" : dxvk_new);
    const auto child = fork(); CHECK(child >= 0);
    if (child == 0) {
      t.deps.apply_files = [&](const auto& operation, const auto&, int borrowed) {
        CHECK(ApplyPendingPublicationDxvk(operation, borrowed, t.root).state == PackageActionState::kComplete);
        _exit(77); return Fail();
      };
      t.Run(); _exit(78);
    }
    int status = 0; CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 77);
    CHECK(Read(t.control / "publication-state.v1") == t.operation.pending_receipt_bytes);
    CHECK(!fs::exists(t.paths.profile));
    CHECK(t.Run().state == PackageActionState::kComplete && t.ready);
    CHECK(remove ? !fs::exists(t.DxvkPath()) : Read(t.DxvkPath()) == dxvk_new);
  }
  {
    Test t; t.ConfigureDxvk(dxvk_old, dxvk_new);
    const auto child = fork(); CHECK(child >= 0);
    if (child == 0) {
      t.deps.apply_files = [&](const auto& operation, const auto&, int borrowed) {
        CHECK(std::signal(SIGXFSZ, SIG_IGN) != SIG_ERR);
        struct rlimit limit{32, 32}; CHECK(setrlimit(RLIMIT_FSIZE, &limit) == 0);
        const auto result = ApplyPendingPublicationDxvk(operation, borrowed, t.root);
        _exit(result.code == "publication-dxvk-write" ? 77 : 78); return Fail();
      };
      t.Run(); _exit(78);
    }
    int status = 0; CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 77);
    CHECK(Read(t.DxvkPath()) == dxvk_old && Read(t.control / "publication-state.v1") == t.operation.pending_receipt_bytes);
    CHECK(!fs::exists(t.paths.profile));
    CHECK(t.Run().state == PackageActionState::kComplete && t.ready && Read(t.DxvkPath()) == dxvk_new);
  }
  for (const unsigned bad : {0u, 1u, 2u}) {
    Test t; t.ConfigureDxvk(dxvk_old, dxvk_new);
    if (bad == 0) CHECK(link(t.DxvkPath().c_str(), (t.root / "other-link").c_str()) == 0);
    if (bad == 1) {
      fs::rename(t.DxvkPath(), t.root / "other-target");
      CHECK(symlink((t.root / "other-target").c_str(), t.DxvkPath().c_str()) == 0);
    }
    if (bad == 2) CHECK(chmod(t.DxvkPath().c_str(), 0444) == 0);
    if (bad == 1) {  // never written through a symbolic link
      CHECK(t.Run().code == "publication-dxvk-read" && !t.ready && Read(t.DxvkPath()) == dxvk_old);
      continue;
    }
    // A second link or another mode is not a reason to stop: the original bytes are
    // replaced by a fresh single-link file in the captured mode.
    CHECK(t.Run().state == PackageActionState::kComplete && t.ready && Read(t.DxvkPath()) == dxvk_new);
    struct stat info{}; CHECK(lstat(t.DxvkPath().c_str(), &info) == 0 && info.st_nlink == 1 && (info.st_mode & 07777) == 0644);
  }
  for (unsigned boundary = 0; boundary < 7; ++boundary) {
    Test t;
    const std::vector<fs::path> effects = {t.paths.profile.parent_path(), t.paths.input_directory, t.paths.profile,
      t.paths.input_directory / "gamepad.input", t.paths.catalog, t.paths.descriptor, t.paths.metadata};
    const auto save = t.DxvkPath().parent_path() / "save.dat";
    std::ofstream(save) << "save survives";
    const auto child = fork(); CHECK(child >= 0);
    if (child == 0) {
      t.deps.apply_files = [&](const auto& operation, const auto& paths, int borrowed) {
        return ApplyPendingPublicationFiles(operation, paths, borrowed, t.root, [&](const fs::path& effect) {
          if (effect == effects[boundary]) _exit(77);
          return true;
        });
      };
      t.Run(); _exit(78);
    }
    int status = 0; CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 77);
    CHECK(Read(t.control / "publication-state.v1") == t.operation.pending_receipt_bytes);
    CHECK(t.Run().state == PackageActionState::kComplete && t.ready && Read(save) == "save survives");
  }
  {
    Test t;
    const auto child = fork(); CHECK(child >= 0);
    if (child == 0) {
      t.deps.apply_files = [&](const auto& operation, const auto& paths, int borrowed) {
        return ApplyPendingPublicationFiles(operation, paths, borrowed, t.root, [&](const fs::path& effect) {
          if (effect == paths.input_directory) {
            CHECK(std::signal(SIGXFSZ, SIG_IGN) != SIG_ERR);
            struct rlimit limit{32, 32}; CHECK(setrlimit(RLIMIT_FSIZE, &limit) == 0);
          }
          return true;
        });
      };
      const auto result = t.Run(); _exit(result.code == "publication-file-write" ? 77 : 78);
    }
    int status = 0; CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 77);
    CHECK(!fs::exists(t.paths.profile) && Read(t.control / "publication-state.v1") == t.operation.pending_receipt_bytes);
    CHECK(t.Run().state == PackageActionState::kComplete && t.ready);
  }
  {
    Test t; t.deps.after_step = [&](PublicationStep step) {
      if (step == PublicationStep::kPendingDurable) {
        fs::create_directories(t.paths.catalog.parent_path()); CHECK(chmod(t.paths.catalog.parent_path().c_str(), 0700) == 0);
        std::ofstream(t.paths.catalog) << "foreign\n"; CHECK(chmod(t.paths.catalog.c_str(), 0600) == 0);
      }
      return true;
    };
    // No previous committed set owns the catalog: the apply writes over what it finds.
    CHECK(t.Run().state == PackageActionState::kComplete && t.ready && Read(t.paths.catalog) == t.operation.catalog_bytes);
  }
  for (const bool missing_prior : {false, true}) {
    Test t; CHECK(t.Run().state == PackageActionState::kComplete);
    auto source = Fixture(); source.transaction_id = "00000000-0000-7000-8000-000000000003";
    source.previous_revision = 7; source.install.settings_revision = source.plan.settings_revision = 8;
    source.previous_receipt_bytes = t.operation.committed_receipt_bytes; source.previous_metadata_bytes = t.operation.metadata_bytes;
    source.plan.input_presets[0].second = "[input]\nmode = keyboard\ncross = enter\n";
    Refresh(source); std::string reason; auto frozen = FreezeSettingsPublicationOperation(source, &reason); CHECK(frozen);
    auto directory = playden::platform::ManagedDirectory::Open(t.control, playden::platform::DirectoryPolicy::kPrivate); CHECK(directory);
    auto lease = playden::platform::ManagedFileLock::Acquire(*directory, "publication.lock", playden::platform::LockMode::kPublisher); CHECK(lease);
    CHECK(VerifySettingsPublicationFiles(t.operation, t.paths, lease->fd(), true, t.root).state == PackageActionState::kComplete);
    // Test-only next-operation fixture setup; production journal retirement and
    // OWNER admission are separate, still unfinished boundaries.
    playden::Storage storage(t.control.string()); CHECK(storage.RemoveDurable("settings-operation.v1").ok());
    if (missing_prior) {
      fs::remove(t.paths.profile);
      CHECK(PreflightSettingsPublication(*frozen, t.paths, lease->fd(), t.root).state == PackageActionState::kFailed);
    } else CHECK(PreflightSettingsPublication(*frozen, t.paths, lease->fd(), t.root).state == PackageActionState::kComplete);
    CHECK(CreateSettingsPublicationOperation(t.control, *frozen).ok());
    CHECK(storage.WriteDurable("publication-state.v1", playden::Bytes(frozen->pending_receipt_bytes.begin(), frozen->pending_receipt_bytes.end())).ok());
    if (missing_prior) fs::remove(t.paths.profile);
    const auto result = ApplyPendingPublicationFiles(*frozen, t.paths, lease->fd(), t.root);
    if (missing_prior) CHECK(result.code == "publication-file-collision");
    else {
      CHECK(result.state == PackageActionState::kComplete);
      CHECK(Read(t.paths.input_directory / "gamepad.input") == source.plan.input_presets[0].second);
      CHECK(VerifySettingsPublicationFiles(*frozen, t.paths, lease->fd(), false, t.root).state == PackageActionState::kComplete);
    }
  }
  {
    Test t;
    fs::create_directories(t.paths.input_directory); CHECK(chmod(t.paths.input_directory.c_str(), 0700) == 0);
    std::ofstream(t.paths.input_directory / "gamepad.input") << "stale";
    CHECK(t.Run().state == PackageActionState::kComplete && t.ready);
    CHECK(Read(t.paths.input_directory / "gamepad.input") == t.operation.plan.input_presets[0].second);
  }
  {
    Test t; const auto rendered = std::string("# Generated by Playden from this game's settings.\ndxgi.maxFrameRate = 60\n");
    auto source = Fixture(); source.plan.dxvk_config = rendered;
    source.dxvk_before = {true, 0644, rendered.size(), *HashArtifactBytes(rendered), rendered}; Refresh(source);
    std::string reason; auto frozen = FreezeSettingsPublicationOperation(source, &reason); CHECK(frozen); t.operation = *frozen;
    std::ofstream(t.DxvkPath()) << rendered; CHECK(chmod(t.DxvkPath().c_str(), 0644) == 0);
    CHECK(t.Run().state == PackageActionState::kComplete && t.ready && Read(t.DxvkPath()) == rendered);
  }
  {
    Test t; t.ConfigureRegistry();
    t.deps.after_step = [&](PublicationStep step) {
      if (step == PublicationStep::kPendingDurable) {
        std::ofstream(t.root / "data/playden/games/install-220/library/prefixes/game/user.reg")
            << "WINE REGISTRY Version 2\n; changed during pending\n";
      }
      return true;
    };
    CHECK(t.Run().code == "publication-registry-collision" && !t.ready);
    CHECK(Read(t.control / "publication-state.v1") == t.operation.pending_receipt_bytes);
    CHECK(t.Registry().find("changed during pending") != std::string::npos);
  }
  {
    Test t; CHECK(t.Run().state == PackageActionState::kComplete);
    auto source = Fixture(); source.transaction_id = "00000000-0000-7000-8000-000000000003";
    source.previous_revision = 7; source.install.settings_revision = source.plan.settings_revision = 8;
    source.previous_receipt_bytes = t.operation.committed_receipt_bytes; source.previous_metadata_bytes = t.operation.metadata_bytes;
    source.plan.input_presets.push_back({"extra.input", "[input]\nmode = keyboard\ncross = space\n"});
    Refresh(source); std::string reason; auto frozen = FreezeSettingsPublicationOperation(source, &reason); CHECK(frozen);
    auto directory = playden::platform::ManagedDirectory::Open(t.control, playden::platform::DirectoryPolicy::kPrivate); CHECK(directory);
    auto lease = playden::platform::ManagedFileLock::Acquire(*directory, "publication.lock", playden::platform::LockMode::kPublisher); CHECK(lease);
    CHECK(PreflightSettingsPublication(*frozen, t.paths, lease->fd(), t.root).state == PackageActionState::kComplete);
    // A preset file left by an earlier publication (presets switched A -> B -> A) is no
    // collision, whatever its bytes or mode: the apply writes over it.
    const auto extra = t.paths.input_directory / "extra.input";
    std::ofstream(extra) << "[input]\nmode = keyboard\ncross = enter\n"; CHECK(chmod(extra.c_str(), 0644) == 0);
    CHECK(PreflightSettingsPublication(*frozen, t.paths, lease->fd(), t.root).state == PackageActionState::kComplete);
    CHECK(Read(extra) == "[input]\nmode = keyboard\ncross = enter\n");
    CHECK(Read(t.control / "publication-state.v1") == t.operation.committed_receipt_bytes);
  }
  for (const bool queued : {false, true}) {
    Test t; const auto owner_root = t.root / "owner"; fs::create_directory(owner_root); CHECK(chmod(owner_root.c_str(), 0700) == 0);
    playden::Storage storage(owner_root.string());
    auto availability = [](const std::string&) { return NativeTitleAvailability{TitleAvailability::kFree, TitleAvailability::kFree}; };
    RuntimeInstallRepository repository(storage, "installs.json", availability); SeedOwner(repository, t.operation.install);
    RepositoryInstallRecordStore base(repository); RejectReadyStore records(base); OwnerPackage package; OwnerNative native;
    native.records = &records; native.queue = queued; records.reject = !queued;
    SettingsPublicationOwner owner(records, package, native);
    auto prepared = owner.Prepare(t.operation.install, t.operation.plan, t.operation.kit_id, t.operation.transaction_id, 1760000000, t.root);
    if (!prepared.ok()) std::fprintf(stderr, "Prepare: %s\n", prepared.message().c_str());
    CHECK(prepared.ok()); t.operation = prepared.value(); t.deps = owner.Dependencies(t.root);
    CHECK(!fs::exists(t.control / "settings-operation.v1") && !fs::exists(t.paths.profile));
    unsigned applies = 0; auto apply = t.deps.apply_files;
    t.deps.apply_files = [&](const auto& o, const auto& paths, int fd) { ++applies; return apply(o, paths, fd); };
    CHECK(t.Run().state == PackageActionState::kFailed && native.registrations == 1 && applies == 1);
    CHECK(Read(t.control / "publication-state.v1") == t.operation.committed_receipt_bytes);
    RuntimeInstallRepository reload(storage, "installs.json", availability); CHECK(reload.Load().ok());
    auto blocked = reload.Find(t.operation.install.install_id); CHECK(blocked && blocked->state == PreparationState::kRegistering);
    CHECK(blocked->journal && blocked->journal->operation_id == t.operation.transaction_id && blocked->journal->step == "await-native");
    auto premature = Fixture(); premature.transaction_id = "00000000-0000-7000-8000-000000000003";
    premature.previous_revision = 7; premature.install.settings_revision = premature.plan.settings_revision = 8;
    premature.previous_receipt_bytes = t.operation.committed_receipt_bytes; premature.previous_metadata_bytes = t.operation.metadata_bytes;
    Refresh(premature); std::string premature_reason; auto premature_frozen = FreezeSettingsPublicationOperation(premature, &premature_reason); CHECK(premature_frozen);
    CHECK(PublishSettingsOperation(*premature_frozen, t.paths, t.deps).code == "publication-retirement-owner");
    CHECK(applies == 1 && native.registrations == 1);
    CHECK(LoadSettingsPublicationOperation(t.control).operation->transaction_id == t.operation.transaction_id);
    RepositoryInstallRecordStore resumed_base(reload); RejectReadyStore resumed_records(resumed_base);
    native.records = &resumed_records;
    SettingsPublicationOwner resumed_owner(resumed_records, package, native);
    auto recovered_intent = resumed_owner.Prepare(t.operation.install, t.operation.plan, t.operation.kit_id,
        t.operation.transaction_id, 1900000000, t.root);
    CHECK(recovered_intent.ok() && recovered_intent.value().registry_unix_time == 1760000000);
    CHECK(recovered_intent.value().plan_sha256 == t.operation.plan_sha256);
    auto changed_plan = t.operation.plan; changed_plan.profile_bytes += "# changed\n";
    CHECK(!resumed_owner.Prepare(t.operation.install, changed_plan, t.operation.kit_id, t.operation.transaction_id, 1900000000, t.root).ok());
    t.deps = resumed_owner.Dependencies(t.root);
    t.deps.apply_files = [&](const auto& o, const auto& paths, int fd) { ++applies; return apply(o, paths, fd); };
    if (queued) {
      CHECK(t.Run().code == "publication-registration-pending" && native.registrations == 1 && applies == 1);
      native.status.installed = true; native.status.scan_pending = false;
    }
    CHECK(t.Run().state == PackageActionState::kComplete && native.registrations == 1 && applies == 1 && package.mutations == 0);
    CHECK(reload.Load().ok()); auto ready = reload.Find(t.operation.install.install_id);
    CHECK(ready && ready->state == PreparationState::kReady && !ready->journal && !ready->last_failure);
    CHECK(ready->settings_revision == t.operation.plan.settings_revision);
    if (!queued) {
      auto source = Fixture(); source.transaction_id = "00000000-0000-7000-8000-000000000003";
      source.previous_revision = t.operation.plan.settings_revision;
      source.previous_receipt_bytes = t.operation.committed_receipt_bytes; source.previous_metadata_bytes = t.operation.metadata_bytes;
      source.install.settings_revision = source.plan.settings_revision = 8;
      auto next = resumed_owner.Prepare(source.install, source.plan, source.kit_id, source.transaction_id, 1760000100, t.root);
      if (!next.ok()) std::fprintf(stderr, "Prepare next: %s\n", next.message().c_str()); CHECK(next.ok());
      auto frozen = std::optional<SettingsPublicationOperation>(next.value());
      CHECK(frozen->previous_revision == 7 && frozen->previous_receipt_bytes == t.operation.committed_receipt_bytes);
      CHECK(frozen->install.state == PreparationState::kReady && !frozen->install.journal);
      const auto archive = t.control / ("settings-operation-" + t.operation.transaction_id + ".v1");
      // An update follows a registered Ready title: it never asks the console's lists,
      // so they may even be unavailable.
      t.operation = *frozen; native.status.available = false;
      t.deps.after_step = [](PublicationStep step) { return step != PublicationStep::kPreviousArchived; };
      CHECK(t.Run().state == PackageActionState::kFailed && fs::exists(archive) && !fs::exists(t.control / "settings-operation.v1"));
      CHECK(resumed_records.Find(t.operation.install.install_id)->state == PreparationState::kReady);
      auto after_archive = resumed_owner.Prepare(t.operation.install, t.operation.plan, t.operation.kit_id,
          t.operation.transaction_id, 1900000000, t.root);
      CHECK(after_archive.ok() && after_archive.value().plan_sha256 == t.operation.plan_sha256);
      CHECK(after_archive.value().registry_unix_time == 1760000100);
      t.deps.after_step = {};
      CHECK(t.Run().state == PackageActionState::kComplete && native.registrations == 1 && applies == 2);
      native.status.available = true;
      CHECK(reload.Load().ok()); CHECK(reload.Find(t.operation.install.install_id)->settings_revision == 8);
    }
  }
  for (unsigned fault = 0; fault < 5; ++fault) {
    Test t; const auto owner_root = t.root / "owner"; fs::create_directory(owner_root); CHECK(chmod(owner_root.c_str(), 0700) == 0);
    playden::Storage storage(owner_root.string());
    auto availability = [](const std::string&) { return NativeTitleAvailability{TitleAvailability::kFree, TitleAvailability::kFree}; };
    RuntimeInstallRepository repository(storage, "installs.json", availability); SeedOwner(repository, t.operation.install);
    RepositoryInstallRecordStore records(repository); OwnerPackage package; OwnerNative native; native.records = &records;
    // A different game folder is a different install; a changed runtime artifact or
    // launch field is not (a kit respin changes artifacts while a journal waits).
    if (fault == 0) { auto changed = t.operation.install; changed.install_path = "/data/homebrew/FAKE78001/games/221"; CHECK(repository.Update(changed).ok()); }
    if (fault == 1) package.status.manifest_matches = false;  // the title marker names another install
    if (fault == 2) native.activity = NativeActivity::kUnknown;
    if (fault == 3) native.fail = true;
    if (fault == 4) native.status.scan_pending = true;
    SettingsPublicationOwner owner(records, package, native); t.deps = owner.Dependencies(t.root);
    CHECK(t.Run().state == PackageActionState::kFailed && package.mutations == 0);
    CHECK(repository.Find(t.operation.install.install_id)->state != PreparationState::kReady);
    if (fault < 3) {
      CHECK(native.registrations == 0 && !fs::exists(t.control / "settings-operation.v1") && !fs::exists(t.control / "publication-state.v1"));
    } else if (fault == 4) {
      auto blocked = repository.Find(t.operation.install.install_id);
      CHECK(blocked->state == PreparationState::kRegistering && blocked->journal && blocked->journal->step == "await-native");
      native.status.scan_pending = false;
      CHECK(t.Run().code == "publication-registration-pending" && native.registrations == 0);
      native.status.installed = true;
      CHECK(t.Run().state == PackageActionState::kComplete && native.registrations == 0);
    } else {
      CHECK(native.registrations == 1);
      auto blocked = repository.Find(t.operation.install.install_id); CHECK(blocked->journal && blocked->journal->step == "register");
      CHECK(t.Run().code == "publication-registration-pending" && native.registrations == 1);
      native.status.installed = true; native.fail = false;
      CHECK(t.Run().state == PackageActionState::kComplete && native.registrations == 1);
    }
  }
  // A kit respin records new runtime artifact hashes while a settings journal waits:
  // the frozen operation still belongs to this install and completes (#5).
  {
    Test t; const auto owner_root = t.root / "owner"; fs::create_directory(owner_root); CHECK(chmod(owner_root.c_str(), 0700) == 0);
    playden::Storage storage(owner_root.string());
    auto availability = [](const std::string&) { return NativeTitleAvailability{TitleAvailability::kFree, TitleAvailability::kFree}; };
    RuntimeInstallRepository repository(storage, "installs.json", availability); SeedOwner(repository, t.operation.install);
    RepositoryInstallRecordStore records(repository); OwnerPackage package; OwnerNative native; native.records = &records;
    SettingsPublicationOwner owner(records, package, native); t.deps = owner.Dependencies(t.root);
    t.deps.after_step = [](PublicationStep step) { return step != PublicationStep::kPendingDurable; };
    CHECK(t.Run().state == PackageActionState::kFailed);
    auto respun = *repository.Find(t.operation.install.install_id);
    respun.artifacts = {{"wine", "e16ec8f", std::string(64, 'b')}}; CHECK(repository.Update(respun).ok());
    t.deps.after_step = {};
    CHECK(t.Run().state == PackageActionState::kComplete && native.registrations == 1);
    auto ready = repository.Find(t.operation.install.install_id);
    CHECK(ready->state == PreparationState::kReady && !ready->journal && ready->artifacts[0].sha256 == std::string(64, 'b'));
  }
  {
    Test t; const auto owner_root = t.root / "owner"; fs::create_directory(owner_root); CHECK(chmod(owner_root.c_str(), 0700) == 0);
    playden::Storage storage(owner_root.string());
    auto availability = [](const std::string&) { return NativeTitleAvailability{TitleAvailability::kFree, TitleAvailability::kFree}; };
    RuntimeInstallRepository repository(storage, "installs.json", availability); SeedOwner(repository, t.operation.install);
    RepositoryInstallRecordStore records(repository); OwnerPackage package; OwnerNative native; native.records = &records;
    SettingsPublicationOwner owner(records, package, native);
    auto plan = t.operation.plan;
    plan.registry = {{"Software\\Wine\\AppDefaults\\probe.exe", "Version", "\"win10\""},
                     {"Control Panel\\Desktop", "LogPixels", "dword:000000a8"}};
    plan.dxvk_config = "# Generated by Playden from this game's settings. Changes here are replaced on the next launch.\ndxgi.maxFrameRate = 30\n";
    const auto original = t.Registry();
    const auto producer = fork(); CHECK(producer >= 0);
    if (producer == 0) {
      auto captured = owner.Prepare(t.operation.install, plan, t.operation.kit_id, t.operation.transaction_id, 1760000000, t.root);
      _exit(captured.ok() ? 77 : 78);
    }
    int status = 0; CHECK(waitpid(producer, &status, 0) == producer && WIFEXITED(status) && WEXITSTATUS(status) == 77);
    RuntimeInstallRepository restart(storage, "installs.json", availability); CHECK(restart.Load().ok());
    RepositoryInstallRecordStore restarted_records(restart); native.records = &restarted_records;
    SettingsPublicationOwner restarted_owner(restarted_records, package, native);
    auto prepared = restarted_owner.Prepare(t.operation.install, plan, t.operation.kit_id, t.operation.transaction_id, 1900000000, t.root);
    if (!prepared.ok()) std::fprintf(stderr, "Prepare registry: %s\n", prepared.message().c_str()); CHECK(prepared.ok());
    CHECK(prepared.value().registry_unix_time == 1760000000);
    CHECK(prepared.value().registry_backup && t.Registry() == original && !fs::exists(t.DxvkPath()));
    CHECK(!fs::exists(t.control / "settings-operation.v1") && !fs::exists(t.control / "publication-state.v1"));
    // A complete durable prepared intent authorizes this exact backup's recovery.
    auto intent = LoadPreparedSettingsPublicationOperation(t.control, t.operation.transaction_id);
    CHECK(intent.state == PublicationRecoveryState::kValid && intent.operation);
    auto again = restarted_owner.Prepare(t.operation.install, plan, t.operation.kit_id, t.operation.transaction_id, 1900000000, t.root);
    CHECK(again.ok() && again.value().plan_sha256 == prepared.value().plan_sha256 && again.value().registry_unix_time == 1760000000);
    t.operation = prepared.value(); t.deps = restarted_owner.Dependencies(t.root);
    CHECK(t.Run().state == PackageActionState::kComplete && native.registrations == 1);
    CHECK(t.Registry().find("\"Version\"=\"win10\"") != std::string::npos);
    CHECK(t.Registry().find("\"LogPixels\"=dword:000000a8") != std::string::npos);
    CHECK(Read(t.DxvkPath()) == plan.dxvk_config);
    auto backup = LoadPublicationRegistryBackup(t.control, t.operation.transaction_id, *t.operation.registry_backup);
    CHECK(backup.state == PublicationRecoveryState::kValid && backup.bytes == original);
  }
  // Service recovery discovers the frozen operation through persisted OWNER
  // UUID after producer exit, then resumes queued native registration without
  // regenerating a plan or overwriting the newer registration arm.
  {
    Test t; fs::remove_all(t.control);
    const auto owner_root = t.root / "owner"; fs::create_directory(owner_root); CHECK(chmod(owner_root.c_str(), 0700) == 0);
    playden::Storage storage(owner_root.string());
    auto availability = [](const std::string&) { return NativeTitleAvailability{TitleAvailability::kFree, TitleAvailability::kFree}; };
    RuntimeInstallRepository repository(storage, "installs.json", availability); SeedOwner(repository, t.operation.install, false);
    RepositoryInstallRecordStore records(repository); OwnerPackage package; OwnerNative native; native.records = &records; native.queue = true;
    SettingsPublicationOwner owner(records, package, native);
    CHECK(owner.Prepare(t.operation.install, t.operation.plan, t.operation.kit_id, t.operation.transaction_id, 1760000000, t.root).ok());
    RuntimeInstallRepository restarted(storage, "installs.json", availability); CHECK(restarted.Load().ok());
    RepositoryInstallRecordStore recovered_records(restarted); native.records = &recovered_records;
    SettingsPublicationOwner resumed_owner(recovered_records, package, native);
    RuntimeInstallService service(recovered_records, package, native, {}, {},
        [&](const RuntimeInstall& record) { return resumed_owner.Resume(record, t.root); });
    CHECK(!service.Step(t.operation.install.install_id).ok());
    auto observed = recovered_records.Find(t.operation.install.install_id);
    CHECK(observed && observed->state == PreparationState::kRegistering && observed->journal);
    CHECK(observed->journal->operation == "settings-publication" && observed->journal->step == "await-native" && !observed->last_failure);
    CHECK(native.registrations == 1 && package.mutations == 0);
    const auto profile = Read(t.paths.profile);
    CHECK(Read(t.control / "publication-state.v1") == LoadSettingsPublicationOperation(t.control).operation->committed_receipt_bytes);
    CHECK(!service.Step(t.operation.install.install_id).ok() && native.registrations == 1 && Read(t.paths.profile) == profile);
    native.status.installed = true; native.status.scan_pending = false;
    CHECK(service.Step(t.operation.install.install_id).ok() && native.registrations == 1);
    observed = recovered_records.Find(t.operation.install.install_id);
    CHECK(observed->state == PreparationState::kReady && !observed->journal && !observed->last_failure);
    // Updates keep the previous OWNER revision while pending, then recovery
    // advances it to the frozen desired revision under the same real protocol.
    const auto previous_revision = t.operation.plan.settings_revision;
    auto desired = t.operation.install; desired.settings_revision = previous_revision + 1;
    auto next_plan = t.operation.plan; next_plan.settings_revision = previous_revision + 1;
    auto next = resumed_owner.Prepare(desired, next_plan, t.operation.kit_id,
        "22222222-2222-4222-8222-222222222222", 1760000010, t.root);
    CHECK(next.ok()); t.operation = next.value(); t.deps = resumed_owner.Dependencies(t.root);
    t.deps.after_step = [](PublicationStep step) { return step != PublicationStep::kPendingDurable; };
    CHECK(t.Run().state == PackageActionState::kFailed);
    observed = recovered_records.Find(t.operation.install.install_id);
    CHECK(observed->settings_revision == previous_revision && observed->journal->operation_id == t.operation.transaction_id);
    CHECK(service.Step(t.operation.install.install_id).ok() && native.registrations == 1);
    CHECK(recovered_records.Find(t.operation.install.install_id)->settings_revision == previous_revision + 1);
  }
  // A managed UUID discovers only its exact frozen journal. Corrupt/missing
  // evidence is refused and retained, without legacy profile effects/register.
  for (unsigned damage = 0; damage < 3; ++damage) {
    Test t; fs::remove_all(t.control);
    const auto owner_root = t.root / "owner"; fs::create_directory(owner_root); CHECK(chmod(owner_root.c_str(), 0700) == 0);
    playden::Storage storage(owner_root.string());
    auto availability = [](const std::string&) { return NativeTitleAvailability{TitleAvailability::kFree, TitleAvailability::kFree}; };
    RuntimeInstallRepository repository(storage, "installs.json", availability); SeedOwner(repository, t.operation.install, false);
    RepositoryInstallRecordStore records(repository); OwnerPackage package; OwnerNative native;
    SettingsPublicationOwner owner(records, package, native);
    CHECK(owner.InitializeControl(t.operation.install, t.operation.transaction_id, t.root).state == PackageActionState::kComplete);
    if (damage > 0) CHECK(owner.Prepare(t.operation.install, t.operation.plan, t.operation.kit_id, t.operation.transaction_id, 1760000000, t.root).ok());
    if (damage == 1) std::ofstream(t.control / ("settings-intent-" + t.operation.transaction_id + ".v1"), std::ios::binary | std::ios::trunc) << "corrupt";
    if (damage == 2) {
      auto stored = *records.Find(t.operation.install.install_id);
      stored.journal->operation_id = "22222222-2222-4222-8222-222222222222"; CHECK(records.Update(stored).ok);
    }
    const auto before = Read(owner_root / "installs.json");
    RuntimeInstallService service(records, package, native, {}, {}, [&](const RuntimeInstall& record) { return owner.Resume(record, t.root); });
    CHECK(!service.Step(t.operation.install.install_id).ok());
    CHECK(Read(owner_root / "installs.json") == before && package.mutations == 0 && native.registrations == 0);
    CHECK(!fs::exists(t.paths.profile) && !fs::exists(t.control / "publication-state.v1"));
    if (damage == 1) CHECK(Read(t.control / ("settings-intent-" + t.operation.transaction_id + ".v1")) == "corrupt");
  }
  // Initial control setup recovers from real process exit at each durability
  // boundary, with a fresh repository and no manually recreated lock/directory.
  for (unsigned boundary = 0; boundary < 4; ++boundary) {
    Test t; fs::remove_all(t.control);
    const auto owner_root = t.root / "owner"; fs::create_directory(owner_root); CHECK(chmod(owner_root.c_str(), 0700) == 0);
    playden::Storage storage(owner_root.string());
    auto availability = [](const std::string&) { return NativeTitleAvailability{TitleAvailability::kFree, TitleAvailability::kFree}; };
    RuntimeInstallRepository repository(storage, "installs.json", availability); SeedOwner(repository, t.operation.install, false);
    RepositoryInstallRecordStore records(repository); OwnerPackage package; OwnerNative native; native.records = &records;
    SettingsPublicationOwner owner(records, package, native);
    const auto child = fork(); CHECK(child >= 0);
    if (child == 0) {
      owner.InitializeControl(t.operation.install, t.operation.transaction_id, t.root, [boundary](PublicationControlStep step) {
        if (static_cast<unsigned>(step) == boundary) _exit(77);
        return true;
      });
      _exit(78);
    }
    int status = 0; CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 77);
    RuntimeInstallRepository restarted(storage, "installs.json", availability); CHECK(restarted.Load().ok());
    RepositoryInstallRecordStore resumed_records(restarted); native.records = &resumed_records;
    SettingsPublicationOwner resumed(resumed_records, package, native);
    auto durable = resumed_records.Find(t.operation.install.install_id); CHECK(durable && durable->journal);
    CHECK(durable->state == PreparationState::kPreparing && durable->journal->operation == "settings-control");
    CHECK(durable->journal->operation_id == t.operation.transaction_id);
    CHECK(resumed.InitializeControl(t.operation.install, "22222222-2222-4222-8222-222222222222", t.root).state == PackageActionState::kFailed);
    CHECK(resumed.InitializeControl(t.operation.install, t.operation.transaction_id, t.root).state == PackageActionState::kComplete);
    struct stat original{}, after{}; CHECK(lstat((t.control / "publication.lock").c_str(), &original) == 0);
    CHECK(original.st_uid == geteuid() && (original.st_mode & 07777) == 0600 && original.st_size == 0 && original.st_nlink == 1);
    CHECK(resumed.InitializeControl(t.operation.install, t.operation.transaction_id, t.root).state == PackageActionState::kComplete);
    CHECK(lstat((t.control / "publication.lock").c_str(), &after) == 0 && original.st_ino == after.st_ino);
    auto captured = resumed.Prepare(t.operation.install, t.operation.plan, t.operation.kit_id, t.operation.transaction_id, 1760000000, t.root);
    CHECK(captured.ok()); t.operation = captured.value(); t.deps = resumed.Dependencies(t.root);
    CHECK(t.Run().state == PackageActionState::kComplete && native.registrations == 1 && package.mutations == 0);
  }
  // An existing private control folder (an interrupted first setup) is adopted; a
  // symbolic link, a non-private data root or an invalid id still refuse, changing no OWNER.
  for (unsigned collision = 0; collision < 4; ++collision) {
    Test t; fs::remove_all(t.control);
    const auto owner_root = t.root / "owner"; fs::create_directory(owner_root); CHECK(chmod(owner_root.c_str(), 0700) == 0);
    playden::Storage storage(owner_root.string());
    auto availability = [](const std::string&) { return NativeTitleAvailability{TitleAvailability::kFree, TitleAvailability::kFree}; };
    RuntimeInstallRepository repository(storage, "installs.json", availability); SeedOwner(repository, t.operation.install, false);
    RepositoryInstallRecordStore records(repository); OwnerPackage package; OwnerNative native;
    SettingsPublicationOwner owner(records, package, native);
    if (collision == 0) { fs::create_directory(t.control); CHECK(chmod(t.control.c_str(), 0700) == 0); }
    if (collision == 1) CHECK(symlink(t.paths.descriptor.parent_path().c_str(), t.control.c_str()) == 0);
    if (collision == 2) CHECK(chmod(t.control.parent_path().c_str(), 0755) == 0);
    const auto before = Read(owner_root / "installs.json");
    const auto txn = collision == 3 ? "../invalid" : t.operation.transaction_id;
    if (collision == 0) {
      CHECK(owner.InitializeControl(t.operation.install, txn, t.root).state == PackageActionState::kComplete);
      CHECK(fs::exists(t.control / "publication.lock"));
      continue;
    }
    CHECK(owner.InitializeControl(t.operation.install, txn, t.root).state == PackageActionState::kFailed);
    CHECK(Read(owner_root / "installs.json") == before && native.registrations == 0 && package.mutations == 0);
    if (collision > 1) CHECK(!fs::exists(t.control));
  }
  // No filesystem setup precedes durable OWNER admission; failed completion
  // persistence leaves its original arm and the same lock for explicit retry.
  for (const auto* rejected_step : {"initialize-control", "control-ready"}) {
    Test t; fs::remove_all(t.control);
    const auto owner_root = t.root / "owner"; fs::create_directory(owner_root); CHECK(chmod(owner_root.c_str(), 0700) == 0);
    playden::Storage storage(owner_root.string());
    auto availability = [](const std::string&) { return NativeTitleAvailability{TitleAvailability::kFree, TitleAvailability::kFree}; };
    RuntimeInstallRepository repository(storage, "installs.json", availability); SeedOwner(repository, t.operation.install, false);
    RepositoryInstallRecordStore base(repository); RejectReadyStore records(base); records.reject_control_step = rejected_step;
    OwnerPackage package; OwnerNative native; SettingsPublicationOwner owner(records, package, native);
    CHECK(owner.InitializeControl(t.operation.install, t.operation.transaction_id, t.root).state == PackageActionState::kFailed);
    const bool armed = std::string(rejected_step) == "control-ready";
    CHECK(fs::exists(t.control / "publication.lock") == armed && fs::exists(t.control) == armed);
    auto observed = records.Find(t.operation.install.install_id); CHECK(observed && observed->journal);
    CHECK(observed->journal->operation == (armed ? "settings-control" : "prepare"));
    records.reject_control_step.clear();
    CHECK(owner.InitializeControl(t.operation.install, t.operation.transaction_id, t.root).state == PackageActionState::kComplete);
  }
  // Matching creation intent does not permit malformed/replaced lock adoption.
  for (unsigned damage = 0; damage < 7; ++damage) {
    Test t; fs::remove_all(t.control);
    const auto owner_root = t.root / "owner"; fs::create_directory(owner_root); CHECK(chmod(owner_root.c_str(), 0700) == 0);
    playden::Storage storage(owner_root.string());
    auto availability = [](const std::string&) { return NativeTitleAvailability{TitleAvailability::kFree, TitleAvailability::kFree}; };
    RuntimeInstallRepository repository(storage, "installs.json", availability); SeedOwner(repository, t.operation.install, false);
    RepositoryInstallRecordStore records(repository); OwnerPackage package; OwnerNative native;
    SettingsPublicationOwner owner(records, package, native);
    // Stop before lock creation, or after completion for missing-inode cases.
    const auto boundary = damage < 5 ? PublicationControlStep::kDirectorySynced : PublicationControlStep::kOwnerPrepared;
    CHECK(owner.InitializeControl(t.operation.install, t.operation.transaction_id, t.root,
        [boundary](PublicationControlStep step) { return step != boundary; }).state == PackageActionState::kFailed);
    const auto lock_path = t.control / "publication.lock";
    if (damage < 4) { std::ofstream(lock_path) << (damage == 1 ? "foreign" : ""); CHECK(chmod(lock_path.c_str(), 0600) == 0); }
    if (damage == 0) CHECK(chmod(lock_path.c_str(), 0644) == 0);
    if (damage == 2) CHECK(link(lock_path.c_str(), (t.root / "foreign-link").c_str()) == 0);
    if (damage == 3 && geteuid() == 0) CHECK(chown(lock_path.c_str(), 1, -1) == 0);
    if (damage == 3 && geteuid() != 0) CHECK(chmod(lock_path.c_str(), 0400) == 0);
    if (damage == 4) CHECK(symlink((t.root / "absent").c_str(), lock_path.c_str()) == 0);
    if (damage == 5) CHECK(unlink(lock_path.c_str()) == 0);
    if (damage == 6) fs::remove_all(t.control);
    const auto before = Read(owner_root / "installs.json");
    CHECK(owner.InitializeControl(t.operation.install, t.operation.transaction_id, t.root).state == PackageActionState::kFailed);
    CHECK(Read(owner_root / "installs.json") == before && package.mutations == 0 && native.registrations == 0);
    if (damage > 4) CHECK(!fs::exists(lock_path));
  }
  // Control setup does not ask whether the game runs (the apply asks once, under its
  // lease); a held reader lease, as a running title keeps, still refuses the publisher.
  {
    Test t; fs::remove_all(t.control);
    const auto owner_root = t.root / "owner"; fs::create_directory(owner_root); CHECK(chmod(owner_root.c_str(), 0700) == 0);
    playden::Storage storage(owner_root.string());
    auto availability = [](const std::string&) { return NativeTitleAvailability{TitleAvailability::kFree, TitleAvailability::kFree}; };
    RuntimeInstallRepository repository(storage, "installs.json", availability); SeedOwner(repository, t.operation.install, false);
    RepositoryInstallRecordStore records(repository); OwnerPackage package; OwnerNative native;
    SettingsPublicationOwner owner(records, package, native);
    native.activity = NativeActivity::kActive;
    CHECK(owner.InitializeControl(t.operation.install, t.operation.transaction_id, t.root).state == PackageActionState::kComplete);
    native.activity = NativeActivity::kStopped;
    auto control = playden::platform::ManagedDirectory::Open(t.control, playden::platform::DirectoryPolicy::kPrivate); CHECK(control);
    auto reader = playden::platform::ManagedFileLock::Acquire(*control, "publication.lock", playden::platform::LockMode::kReader); CHECK(reader);
    const auto before = Read(owner_root / "installs.json");
    CHECK(owner.InitializeControl(t.operation.install, t.operation.transaction_id, t.root).state == PackageActionState::kFailed);
    CHECK(!owner.Prepare(t.operation.install, t.operation.plan, t.operation.kit_id, t.operation.transaction_id, 1760000000, t.root).ok());
    CHECK(Read(owner_root / "installs.json") == before && !fs::exists(t.control / "publication-state.v1"));
  }
  // Prepare uses an existing control folder and lock left without a setup intent.
  {
    Test t;
    const auto owner_root = t.root / "owner"; fs::create_directory(owner_root); CHECK(chmod(owner_root.c_str(), 0700) == 0);
    playden::Storage storage(owner_root.string());
    auto availability = [](const std::string&) { return NativeTitleAvailability{TitleAvailability::kFree, TitleAvailability::kFree}; };
    RuntimeInstallRepository repository(storage, "installs.json", availability); SeedOwner(repository, t.operation.install, false);
    RepositoryInstallRecordStore records(repository); OwnerPackage package; OwnerNative native;
    SettingsPublicationOwner owner(records, package, native);
    native.records = &records;
    auto prepared = owner.Prepare(t.operation.install, t.operation.plan, t.operation.kit_id, t.operation.transaction_id, 1760000000, t.root);
    CHECK(prepared.ok() && fs::exists(t.control / ("settings-intent-" + t.operation.transaction_id + ".v1")));
    t.operation = prepared.value(); t.deps = owner.Dependencies(t.root);
    CHECK(t.Run().state == PackageActionState::kComplete && native.registrations == 1);
  }
  // Prepare itself initializes a missing control path in a fresh owned bottle.
  {
    Test t; fs::remove_all(t.control);
    const auto owner_root = t.root / "owner"; fs::create_directory(owner_root); CHECK(chmod(owner_root.c_str(), 0700) == 0);
    playden::Storage storage(owner_root.string());
    auto availability = [](const std::string&) { return NativeTitleAvailability{TitleAvailability::kFree, TitleAvailability::kFree}; };
    RuntimeInstallRepository repository(storage, "installs.json", availability); SeedOwner(repository, t.operation.install, false);
    RepositoryInstallRecordStore records(repository); OwnerPackage package; OwnerNative native; native.records = &records;
    SettingsPublicationOwner owner(records, package, native);
    auto prepared = owner.Prepare(t.operation.install, t.operation.plan, t.operation.kit_id, t.operation.transaction_id, 1760000000, t.root);
    CHECK(prepared.ok() && fs::exists(t.control / "publication.lock") && !fs::exists(t.control / "publication-state.v1"));
    t.operation = prepared.value(); t.deps = owner.Dependencies(t.root);
    CHECK(t.Run().state == PackageActionState::kComplete && native.registrations == 1);
  }
  // Requested settings survive cold restart before capture and after control
  // initialization, without reloading a changed recipe/settings repository.
  for (unsigned boundary = 0; boundary < 2; ++boundary) {
    Test t; fs::remove_all(t.control);
    const auto owner_root = t.root / "owner"; fs::create_directory(owner_root); CHECK(chmod(owner_root.c_str(), 0700) == 0);
    playden::Storage storage(owner_root.string());
    auto availability = [](const std::string&) { return NativeTitleAvailability{TitleAvailability::kFree, TitleAvailability::kFree}; };
    RuntimeInstallRepository repository(storage, "installs.json", availability);
    auto preparing = t.operation.install; preparing.state = PreparationState::kPreparing;
    SeedOwner(repository, preparing, false);
    RepositoryInstallRecordStore records(repository); OwnerPackage package; OwnerNative native; native.records = &records;
    SettingsPublicationOwner owner(records, package, native);
    SettingsPublicationRequest request;
    request.transaction_id = t.operation.transaction_id; request.kit_id = t.operation.kit_id;
    request.install = t.operation.install; request.plan = t.operation.plan; request.registry_unix_time = 1760000000;
    auto requested = owner.Request(request, owner_root);
    if (requested.state != PackageActionState::kComplete) std::fprintf(stderr, "Request: %s %s\n", requested.code.c_str(), requested.message.c_str());
    CHECK(requested.state == PackageActionState::kComplete);
    CHECK(owner.Request(request, owner_root).state == PackageActionState::kComplete);
    auto changed_request = request; changed_request.plan.profile_bytes += "\n";
    CHECK(owner.Request(changed_request, owner_root).state == PackageActionState::kFailed);
    CHECK(!fs::exists(t.control));
    auto armed = records.Find(request.install.install_id); CHECK(armed && armed->journal && armed->journal->operation == "settings-request");
    if (boundary) CHECK(owner.InitializeControl(*armed, request.transaction_id, t.root).state == PackageActionState::kComplete);
    CHECK(!fs::exists(t.control / ("settings-intent-" + request.transaction_id + ".v1")));
    RuntimeInstallRepository restart(storage, "installs.json", availability); CHECK(restart.Load().ok());
    RepositoryInstallRecordStore restarted_records(restart); native.records = &restarted_records;
    SettingsPublicationOwner resumed(restarted_records, package, native);
    RuntimeInstallService service(restarted_records, package, native, {}, {},
        [&](const RuntimeInstall& record) { return resumed.ResumeRequested(record, owner_root, t.root); });
    CHECK(service.Step(request.install.install_id).ok());
    auto ready = restarted_records.Find(request.install.install_id);
    CHECK(ready && ready->state == PreparationState::kReady && !ready->journal && native.registrations == 1);
    CHECK(Read(t.paths.profile) == request.plan.profile_bytes);
    auto frozen = LoadSettingsPublicationOperation(t.control);
    CHECK(frozen.operation && frozen.operation->registry_unix_time == 1760000000);
    CHECK(resumed.Request(request, owner_root).state == PackageActionState::kFailed);
    // Updating a Ready title keeps the committed revision until publication.
    package.status.profile_published = true;
    package.status.profile_settings_revision = ready->settings_revision;
    auto update = request; update.transaction_id = "22222222-2222-4222-8222-222222222222";
    update.install = *ready; ++update.install.settings_revision;
    update.plan.settings_revision = update.install.settings_revision;
    update.plan.dxvk_config = "# Generated by Playden from this game's settings.\ndxgi.maxFrameRate = 30\n";
    // A registry edit: its capture leaves a registry backup next to the intent.
    update.plan.registry = {{"Software\\Wine\\AppDefaults\\probe.exe", "Version", "\"win7\""}};
    CHECK(resumed.Request(update, owner_root).state == PackageActionState::kComplete);
    auto update_arm = restarted_records.Find(ready->install_id);
    CHECK(update_arm && update_arm->settings_revision == ready->settings_revision && update_arm->journal &&
          update_arm->journal->operation_id == update.transaction_id && update_arm->journal->step == "capture-update");
    CHECK(Read(t.paths.profile) == request.plan.profile_bytes && !fs::exists(t.DxvkPath()));
    // Registry backups and prepared intents in the control folder, of any transaction.
    const auto capture_files = [&] {
      std::size_t count = 0;
      std::error_code error;
      for (const auto& entry : fs::directory_iterator(t.control, error)) {
        const auto name = entry.path().filename().string();
        count += name.rfind("registry-backup-", 0) == 0 || name.rfind("settings-intent-", 0) == 0;
      }
      return count;
    };
    const auto baseline = capture_files();
    const auto first_backup = t.control / ("registry-backup-" + request.transaction_id + ".v1");
    const bool first_backup_present = fs::exists(first_backup);
    if (boundary) {
      // The committed operation no longer decodes (another build's codec): with a pending receipt
      // the capture fails closed and the journal stays; with the committed receipt, which binds the
      // published metadata, it is set aside under its own name and the capture goes on without it.
      const auto journal = t.control / "settings-operation.v1", receipt = t.control / "publication-state.v1";
      const auto bytes = Read(journal), committed = Read(receipt);
      std::ofstream(journal, std::ios::binary | std::ios::trunc) << bytes << "trailing";
      std::ofstream(receipt, std::ios::binary | std::ios::trunc) << frozen.operation->pending_receipt_bytes;
      auto refused = resumed.Prepare(update.install, update.plan, update.kit_id, update.transaction_id,
                                     update.registry_unix_time, t.root);
      CHECK(!refused.ok() && refused.message().find("(kept: ") != std::string::npos);
      CHECK(Read(journal) == bytes + "trailing" && !fs::exists(t.control / "settings-operation.unreadable-1760000000.v1"));
      std::ofstream(receipt, std::ios::binary | std::ios::trunc) << committed;
      auto prepared = resumed.Prepare(update.install, update.plan, update.kit_id, update.transaction_id,
                                     update.registry_unix_time, t.root);
      if (!prepared.ok()) std::fprintf(stderr, "Prepare: %s\n", prepared.message().c_str());
      CHECK(prepared.ok()); // Cold restart after prepared handoff, before publication.
      CHECK(!fs::exists(journal) && Read(t.control / "settings-operation.unreadable-1760000000.v1") == bytes + "trailing");
      CHECK(SetAsideSettingsPublicationOperation(t.control, 1760000000).message().find("No active journal") != std::string::npos);
    }
    // A Ready update's request that never reached its publication (the record still carries the
    // request arm) can be dropped: the record returns to its committed publication, the saved
    // request and its capture's backup and intent go; other transactions' files stay. Not while
    // the lock is held (the game runs), nor for a journal other than the record's.
    {
      const auto update_backup = t.control / ("registry-backup-" + update.transaction_id + ".v1");
      const auto update_intent = t.control / ("settings-intent-" + update.transaction_id + ".v1");
      CHECK(fs::exists(update_backup) == (boundary != 0) && fs::exists(update_intent) == (boundary != 0));
      CHECK(capture_files() == baseline + (boundary ? 2 : 0));
      // Without a capture, a link under the intent's name is not followed nor removed.
      const auto outside = t.root / "outside-intent";
      if (!boundary) {
        std::ofstream(outside) << "outside";
        fs::create_symlink(outside, update_intent);
      }
      const int held = open((t.control / "publication.lock").c_str(), O_RDONLY);
      CHECK(held >= 0 && flock(held, LOCK_SH) == 0);
      CHECK(resumed.DiscardRequested(*update_arm, owner_root, t.root).code == "publication-busy");
      CHECK(close(held) == 0);
      auto other = *update_arm; other.journal->operation_id = request.transaction_id;
      CHECK(resumed.DiscardRequested(other, owner_root, t.root).code == "publication-discard-owner");
      auto arm = restarted_records.Find(ready->install_id);
      CHECK(arm && arm->journal && arm->journal->step == "capture-update");
      const auto dropped = resumed.DiscardRequested(*update_arm, owner_root, t.root);
      if (dropped.state != PackageActionState::kComplete) std::fprintf(stderr, "Discard: %s %s\n", dropped.code.c_str(), dropped.message.c_str());
      CHECK(dropped.state == PackageActionState::kComplete && dropped.message.empty());
      auto back = restarted_records.Find(ready->install_id);
      CHECK(back && back->state == PreparationState::kReady && !back->journal && !back->last_failure &&
            back->settings_revision == ready->settings_revision);
      CHECK(!fs::exists(owner_root / ("settings-request-" + update.transaction_id + ".v1")));
      CHECK(!fs::exists(update_backup));
      if (boundary) {
        CHECK(!fs::exists(update_intent) && capture_files() == baseline);
      } else {
        CHECK(fs::is_symlink(update_intent) && Read(outside) == "outside");
        fs::remove(update_intent);
      }
      CHECK(fs::exists(first_backup) == first_backup_present);
      CHECK(Read(t.paths.profile) == request.plan.profile_bytes && !fs::exists(t.DxvkPath()));
      CHECK(resumed.DiscardRequested(*update_arm, owner_root, t.root).code == "publication-discard-owner");
      // The same update, requested again under a new transaction, goes through as before.
      update.transaction_id = "33333333-3333-4333-8333-333333333333";
      CHECK(resumed.Request(update, owner_root).state == PackageActionState::kComplete);
      update_arm = restarted_records.Find(ready->install_id);
      CHECK(update_arm && update_arm->journal && update_arm->journal->operation_id == update.transaction_id);
      if (boundary) {
        auto prepared = resumed.Prepare(update.install, update.plan, update.kit_id, update.transaction_id,
                                       update.registry_unix_time, t.root);
        CHECK(prepared.ok());
        // A retry leaves only its own capture's files, not the dropped one's as well.
        CHECK(capture_files() == baseline + 2);
      }
    }
    RuntimeInstallRepository update_restart(storage, "installs.json", availability); CHECK(update_restart.Load().ok());
    RepositoryInstallRecordStore update_records(update_restart); native.records = &update_records;
    SettingsPublicationOwner update_owner(update_records, package, native);
    RuntimeInstallService update_service(update_records, package, native, {}, {},
        [&](const RuntimeInstall& record) { return update_owner.ResumeRequested(record, owner_root, t.root); });
    CHECK(update_service.Step(ready->install_id).ok());
    auto updated = update_records.Find(ready->install_id);
    CHECK(updated && updated->state == PreparationState::kReady && !updated->journal &&
          updated->settings_revision == update.plan.settings_revision && native.registrations == 1);
    CHECK(Read(t.DxvkPath()) == update.plan.dxvk_config);
    // The committed operation loads: it is never set aside.
    CHECK(SetAsideSettingsPublicationOperation(t.control, 1760000001).message().find("never set aside") != std::string::npos);
    CHECK(fs::exists(t.control / "settings-operation.v1") && !fs::exists(t.control / "settings-operation.unreadable-1760000001.v1"));
  }
  {
    Test t; fs::remove_all(t.control);
    const auto owner_root = t.root / "owner"; fs::create_directory(owner_root); CHECK(chmod(owner_root.c_str(), 0700) == 0);
    playden::Storage storage(owner_root.string());
    auto availability = [](const std::string&) { return NativeTitleAvailability{TitleAvailability::kFree, TitleAvailability::kFree}; };
    RuntimeInstallRepository repository(storage, "installs.json", availability);
    auto preparing = t.operation.install; preparing.state = PreparationState::kPreparing;
    SeedOwner(repository, preparing, false);
    RepositoryInstallRecordStore records(repository); OwnerPackage package; OwnerNative native; native.records = &records;
    SettingsPublicationOwner owner(records, package, native);
    SettingsPublicationRequest request;
    request.transaction_id = t.operation.transaction_id; request.kit_id = t.operation.kit_id;
    request.install = preparing; request.plan = t.operation.plan; request.registry_unix_time = 1760000000;
    const auto before = Read(owner_root / "installs.json");
    CHECK(CreateSettingsPublicationRequest(owner_root, request).ok()); // Inert pre-arm orphan.
    CHECK(owner.Request(request, owner_root).code == "publication-request-orphan");
    CHECK(owner.ResumeRequested(preparing, owner_root, t.root).state == PackageActionState::kFailed);
    CHECK(Read(owner_root / "installs.json") == before && !fs::exists(t.control));
  }
  // Adopt existing Steam paths without moving downloads, local saves or DLLs.
  for (const auto* download_root : {"/data/homebrew/FAKE77031/games/220", "/mnt/usb0/playden/games/220"}) {
    Test t; fs::remove_all(t.control);
    auto record = t.operation.install; record.install_path = download_root;
    auto plan = t.operation.plan;
    std::string windows = download_root; std::replace(windows.begin(), windows.end(), '/', '\\');
    const std::string old_root = "Z:\\data\\homebrew\\FAKE78001\\games\\220";
    for (std::size_t pos = 0; (pos = plan.profile_bytes.find(old_root, pos)) != std::string::npos; pos += windows.size() + 2)
      plan.profile_bytes.replace(pos, old_root.size(), "Z:" + windows);
    plan.dxvk_config = "# Generated by Playden from this game's settings.\ndxgi.maxFrameRate = 30\n";
    const auto game = t.root / fs::path(download_root).relative_path(); fs::create_directories(game);
    std::ofstream(game / "probe.exe") << "existing-game";
    std::ofstream(game / "steam_api.dll") << "unchanged-original-dll";
    std::ofstream(game / "player-save.dat") << "existing-player-save";
    const auto owner_root = t.root / "owner"; fs::create_directory(owner_root); CHECK(chmod(owner_root.c_str(), 0700) == 0);
    playden::Storage storage(owner_root.string());
    auto availability = [](const std::string&) { return NativeTitleAvailability{TitleAvailability::kFree, TitleAvailability::kFree}; };
    RuntimeInstallRepository repository(storage, "installs.json", availability); SeedOwner(repository, record, false);
    RepositoryInstallRecordStore records(repository); OwnerPackage package; OwnerNative native; native.records = &records;
    SettingsPublicationOwner owner(records, package, native);
    auto prepared = owner.Prepare(record, plan, t.operation.kit_id, t.operation.transaction_id, 1760000000, t.root);
    CHECK(prepared.ok()); t.operation = prepared.value(); t.deps = owner.Dependencies(t.root);
    auto published = t.Run();
    if (published.state != PackageActionState::kComplete) std::fprintf(stderr, "Adopted publication: %s %s\n", published.code.c_str(), published.message.c_str());
    CHECK(published.state == PackageActionState::kComplete && native.registrations == 1);
    auto ready = records.Find(record.install_id); CHECK(ready && ready->state == PreparationState::kReady && ready->install_path == download_root);
    CHECK(Read(game / "dxvk.conf") == plan.dxvk_config && Read(game / "probe.exe") == "existing-game");
    CHECK(Read(game / "steam_api.dll") == "unchanged-original-dll" && Read(game / "player-save.dat") == "existing-player-save");
    CHECK(Read(t.paths.profile) == plan.profile_bytes && !fs::exists(t.paths.descriptor.parent_path() / "games/220/dxvk.conf"));
    CHECK(package.mutations == 0); // No download/title/bottle staging during publication.
  }
  // Presets switched A -> B -> A -> B': B's preset file stays behind while A is published,
  // and the next B overwrites it instead of stopping in recovery (one ownership check each).
  {
    Test t; fs::remove_all(t.control);
    const auto owner_root = t.root / "owner"; fs::create_directory(owner_root); CHECK(chmod(owner_root.c_str(), 0700) == 0);
    playden::Storage storage(owner_root.string());
    auto availability = [](const std::string&) { return NativeTitleAvailability{TitleAvailability::kFree, TitleAvailability::kFree}; };
    RuntimeInstallRepository repository(storage, "installs.json", availability); SeedOwner(repository, t.operation.install, false);
    RepositoryInstallRecordStore records(repository); OwnerNative native; native.records = &records;
    struct CountingPackage final : PackageInstaller {
      PackageStatus status{true, true, true, true, true, true, false, false, 0}; unsigned inspections = 0;
      PackageStatus Inspect(const RuntimeInstall&) override { ++inspections; return status; }
      PackageActionResult StageTitle(const RuntimeInstall&) override { return Fail(); }
      PackageActionResult CopyTemplate(const RuntimeInstall&) override { return Fail(); }
      PackageActionResult ValidateArtifacts(const RuntimeInstall&) override { return Fail(); }
      PackageActionResult PublishProfile(const RuntimeInstall&) override { return Fail(); }
    } package;
    SettingsPublicationOwner owner(records, package, native);
    const std::pair<std::string, std::string> gamepad = t.operation.plan.input_presets[0];
    const std::pair<std::string, std::string> extra_v1{"extra.input", "[input]\nmode = keyboard\ncross = space\n"};
    const std::pair<std::string, std::string> extra_v2{"extra.input", "[input]\nmode = keyboard\ncross = enter\n"};
    const std::vector<std::vector<std::pair<std::string, std::string>>> steps = {
        {gamepad}, {gamepad, extra_v1}, {gamepad}, {gamepad, extra_v2}};
    for (std::size_t i = 0; i < steps.size(); ++i) {
      auto install = *records.Find(t.operation.install.install_id);
      install.settings_revision = 7 + i;
      auto plan = t.operation.plan; plan.settings_revision = install.settings_revision; plan.input_presets = steps[i];
      const auto transaction = "00000000-0000-7000-8000-00000000010" + std::to_string(i);
      auto prepared = owner.Prepare(install, plan, t.operation.kit_id, transaction, 1760000000, t.root);
      if (!prepared.ok()) std::fprintf(stderr, "A-B-A prepare %zu: %s\n", i, prepared.message().c_str());
      CHECK(prepared.ok()); t.operation = prepared.value(); t.deps = owner.Dependencies(t.root);
      package.inspections = 0;
      const auto applied = t.Run();
      if (applied.state != PackageActionState::kComplete) std::fprintf(stderr, "A-B-A %zu: %s %s\n", i, applied.code.c_str(), applied.message.c_str());
      CHECK(applied.state == PackageActionState::kComplete && package.inspections == 1);
      CHECK(records.Find(install.install_id)->state == PreparationState::kReady);
    }
    CHECK(Read(t.paths.input_directory / "extra.input") == extra_v2.second);
    CHECK(records.Find(t.operation.install.install_id)->settings_revision == 10 && native.registrations == 1);
  }
  // A game that ships its own dxvk.conf: the first apply succeeds, the frozen before-
  // snapshot (journal, then its archive) keeps the game's bytes, and the file is
  // written over (or removed when no DXVK settings apply, as the title's start check
  // requires). A crash after the DXVK effect resumes; a change after capture still stops.
  const std::string shipped = "# This game's own DXVK settings\ndxgi.syncInterval = 1\n";
  for (unsigned variant = 0; variant < 5; ++variant) {
    Test t; fs::remove_all(t.control);
    const auto owner_root = t.root / "owner"; fs::create_directory(owner_root); CHECK(chmod(owner_root.c_str(), 0700) == 0);
    playden::Storage storage(owner_root.string());
    auto availability = [](const std::string&) { return NativeTitleAvailability{TitleAvailability::kFree, TitleAvailability::kFree}; };
    RuntimeInstallRepository repository(storage, "installs.json", availability); SeedOwner(repository, t.operation.install, false);
    RepositoryInstallRecordStore records(repository); OwnerPackage package; OwnerNative native; native.records = &records;
    SettingsPublicationOwner owner(records, package, native);
    std::ofstream(t.DxvkPath()) << shipped; CHECK(chmod(t.DxvkPath().c_str(), 0755) == 0);
    auto plan = t.operation.plan;
    const bool remove = variant == 1;
    if (!remove) plan.dxvk_config = "# Generated by Playden from this game's settings.\ndxgi.maxFrameRate = 30\n";
    auto prepared = owner.Prepare(t.operation.install, plan, t.operation.kit_id, t.operation.transaction_id, 1760000000, t.root);
    if (!prepared.ok()) std::fprintf(stderr, "shipped dxvk prepare: %s\n", prepared.message().c_str());
    CHECK(prepared.ok() && prepared.value().dxvk_before.present && prepared.value().dxvk_before.bytes == shipped);
    t.operation = prepared.value(); t.deps = owner.Dependencies(t.root);
    if (variant == 4) CHECK(chmod(t.DxvkPath().c_str(), 0600) == 0);  // same bytes, another mode: still the original
    if (variant == 2) {  // crash right after the DXVK effect, then resume in a new process
      const auto child = fork(); CHECK(child >= 0);
      if (child == 0) {
        t.deps.apply_files = [&](const auto& operation, const auto&, int borrowed) {
          CHECK(ApplyPendingPublicationRegistry(operation, borrowed, t.root).state == PackageActionState::kComplete);
          CHECK(ApplyPendingPublicationDxvk(operation, borrowed, t.root).state == PackageActionState::kComplete);
          _exit(77); return Fail();
        };
        t.Run(); _exit(78);
      }
      int status = 0; CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 77);
      CHECK(Read(t.DxvkPath()) == plan.dxvk_config);
      t.deps = owner.Dependencies(t.root);
    }
    if (variant == 3) {  // changed after capture: neither the snapshot nor the intended bytes
      t.deps.after_step = [&](PublicationStep step) {
        if (step == PublicationStep::kPendingDurable) std::ofstream(t.DxvkPath()) << "changed by hand\n";
        return true;
      };
      CHECK(t.Run().code == "publication-dxvk-collision" && Read(t.DxvkPath()) == "changed by hand\n");
      CHECK(LoadSettingsPublicationOperation(t.control).operation->dxvk_before.bytes == shipped);
      continue;
    }
    const auto result = t.Run();
    if (result.state != PackageActionState::kComplete) std::fprintf(stderr, "shipped dxvk: %s %s\n", result.code.c_str(), result.message.c_str());
    CHECK(result.state == PackageActionState::kComplete && native.registrations == 1);
    CHECK(remove ? !fs::exists(t.DxvkPath()) : Read(t.DxvkPath()) == plan.dxvk_config);
    struct stat info{};
    if (!remove) CHECK(stat(t.DxvkPath().c_str(), &info) == 0 && (info.st_mode & 07777) == 0644);
    CHECK(LoadSettingsPublicationOperation(t.control).operation->dxvk_before.bytes == shipped);
  }
  // A Ready update at the same revision only replaces an owned publication (schema 2) when the
  // kit admits unowned game folders: one already unowned (or a game that did not move, with a
  // kit that doesn't) is refused before anything is written.
  {
    Test t; const auto owner_root = t.root / "owner"; fs::create_directory(owner_root); CHECK(chmod(owner_root.c_str(), 0700) == 0);
    const auto requests = t.root / "requests"; fs::create_directory(requests); CHECK(chmod(requests.c_str(), 0700) == 0);
    playden::Storage storage(owner_root.string());
    auto availability = [](const std::string&) { return NativeTitleAvailability{TitleAvailability::kFree, TitleAvailability::kFree}; };
    RuntimeInstallRepository repository(storage, "installs.json", availability); SeedOwner(repository, t.operation.install, false);
    auto ready = *repository.Find(t.operation.install.install_id);
    ready.state = PreparationState::kReady; ready.journal.reset(); ready.last_failure.reset();
    CHECK(repository.Update(ready).ok());
    RepositoryInstallRecordStore records(repository); OwnerPackage package; OwnerNative native; native.records = &records;
    const auto profiles = t.paths.metadata.parent_path();
    fs::create_directories(profiles); CHECK(chmod(profiles.c_str(), 0700) == 0);
    const auto publish = [&](const std::string& bytes) {
      { std::ofstream(t.paths.metadata, std::ios::trunc) << bytes; }
      CHECK(chmod(t.paths.metadata.c_str(), 0600) == 0);
    };
    PwTitlePublicationMetadata unowned_metadata{};
    CHECK(pw_title_publication_metadata_parse(reinterpret_cast<const uint8_t*>(t.operation.metadata_bytes.data()),
                                              t.operation.metadata_bytes.size(), &unowned_metadata) == PW_OK);
    CHECK(unowned_metadata.game_root_unowned == 0);
    unowned_metadata.schema_version = 3; unowned_metadata.game_root_unowned = 1;
    const auto unowned_bytes = SerializePublicationMetadata(unowned_metadata); CHECK(unowned_bytes);
    SettingsPublicationRequest request;
    request.transaction_id = "00000000-0000-7000-8000-000000000009"; request.kit_id = t.operation.kit_id;
    request.install = ready; request.plan = t.operation.plan; request.registry_unix_time = 1760000000;
    CHECK(request.plan.settings_revision == ready.settings_revision);
    SettingsPublicationOwner capable(records, package, native, true, t.root);
    {
      // A profile the writer emitted but the profile parser rejects is refused before any OWNER check,
      // and the refusal names why it is invalid instead of blaming the install record. Guards against a
      // settings publish failing opaquely when the writer and the profile parser disagree.
      auto invalid = request; invalid.plan.profile_bytes += "[unsupported]\nkey = value\n";
      const auto refused = capable.Request(invalid, requests);
      CHECK(refused.state == PackageActionState::kFailed);
      CHECK(refused.code == "publication-request-invalid");
      CHECK(refused.message == "Requested profile is unsupported or has mismatched identities.");
      CHECK(!repository.Find(ready.install_id)->journal && fs::is_empty(requests));
    }
    publish(*unowned_bytes);
    CHECK(PublishedGameRootUnowned(ready, t.root) == std::optional<bool>(true));
    CHECK(capable.Request(request, requests).code == "publication-request-owner");
    publish(t.operation.metadata_bytes);
    CHECK(PublishedGameRootUnowned(ready, t.root) == std::optional<bool>(false));
    SettingsPublicationOwner strict(records, package, native, false, t.root);
    CHECK(strict.Request(request, requests).code == "publication-request-owner");
    CHECK(!repository.Find(ready.install_id)->journal && fs::is_empty(requests));
    // The owned one is published again as unowned: the request is armed.
    const auto armed = capable.Request(request, requests);
    if (armed.state != PackageActionState::kComplete) std::fprintf(stderr, "same-revision request: %s %s\n", armed.code.c_str(), armed.message.c_str());
    CHECK(armed.state == PackageActionState::kComplete);
    CHECK(repository.Find(ready.install_id)->journal->operation == "settings-request");
  }
  std::puts("settings publication coordinator tests passed");
}
