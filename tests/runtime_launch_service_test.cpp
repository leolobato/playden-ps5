#include "runtime/launch_service.h"

#undef fsync

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

using namespace playden::runtime;

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__,    \
                   #condition);                                                \
      std::exit(1);                                                            \
    }                                                                          \
  } while (false)

namespace {

constexpr const char *kSession = "d9428888-122b-4b1e-8d5d-fd3b5f4b9b2c";
bool fail_file_sync = false;
bool fail_directory_sync = false;

mode_t Mode(const std::filesystem::path &path) {
  struct stat status {};
  CHECK(::lstat(path.c_str(), &status) == 0);
  return status.st_mode & 0777;
}

std::string ReadFile(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  CHECK(input.good());
  return {std::istreambuf_iterator<char>(input), {}};
}

LaunchSession JournalSession() {
  LaunchSession session;
  session.session_id = kSession;
  session.install_id = "install-hl2";
  session.title_id = "FAKE78001";
  session.runtime_bundle_id = "prospero-win-e16ec8f";
  session.settings_revision = std::numeric_limits<std::uint64_t>::max();
  session.account_token = "account-private-token";
  session.requested_ms = std::numeric_limits<std::int64_t>::min();
  session.handoff_ms = std::numeric_limits<std::int64_t>::max();
  return session;
}

RuntimeInstall ReadyInstall() {
  RuntimeInstall record;
  record.install_id = "install-hl2";
  record.title_id = "FAKE78001";
  record.runtime_number = 220;
  record.data_root = "/data/playden/games/install-hl2";
  record.default_profile_id = "game";
  record.runtime_bundle_id = "prospero-win-e16ec8f";
  record.recipe_id = "steam-220-hl2-v1";
  record.recipe_version = "1";
  record.settings_revision = 17;
  record.launch_entry_id = "0";
  record.launch_path = "hl2.exe";
  record.install_path = "/data/homebrew/FAKE77031/games/220";
  record.volume_id = "internal";
  record.state = PreparationState::kReady;
  return record;
}

struct FakeRecords final : InstallRecordStore {
  std::unordered_map<std::string, RuntimeInstall> records;
  std::optional<RuntimeInstall> Find(const std::string &id) const override {
    const auto found = records.find(id);
    return found == records.end() ? std::nullopt
                                  : std::optional<RuntimeInstall>(found->second);
  }
  std::vector<RuntimeInstall> Snapshot() const override { return {}; }
  RecordUpdateResult Update(const RuntimeInstall &) override {
    return {false, "read-only", "launch tests never update install records"};
  }
};

struct FakeJournal final : LaunchSessionJournal {
  std::optional<LaunchSession> pending;
  std::unordered_map<std::string, LaunchSession> history;
  std::string fail_operation;
  int fail_pending_call = 0;
  int pending_calls = 0;
  std::vector<std::string> calls;

  LaunchJournalLoad LoadPending() override {
    calls.push_back("load");
    if (fail_operation == "load")
      return LaunchJournalLoad::Failure("load-failed", "pending read failed");
    return LaunchJournalLoad::Success(pending);
  }
  LaunchJournalResult CreateHistory(const LaunchSession &session) override {
    calls.push_back("history:" + session.session_id);
    if (fail_operation == "history")
      return LaunchJournalResult::Failure("history-failed", "history write failed");
    if (!history.emplace(session.session_id, session).second)
      return LaunchJournalResult::Failure("history-exists", "history exists");
    return LaunchJournalResult::Success();
  }
  LaunchJournalResult PersistPending(const LaunchSession &session) override {
    ++pending_calls;
    calls.push_back("pending:" + std::to_string(static_cast<int>(session.phase)));
    if (fail_operation == "pending" ||
        (fail_pending_call != 0 && pending_calls == fail_pending_call))
      return LaunchJournalResult::Failure("pending-failed", "pending write failed");
    pending = session;
    return LaunchJournalResult::Success();
  }
  LaunchJournalResult ClearPending() override {
    calls.push_back("clear");
    pending.reset();
    return LaunchJournalResult::Success();
  }
};

LaunchPreflight ReadyPreflight(const RuntimeInstall &record) {
  LaunchPreflight result;
  result.capability_available = true;
  result.volume_mounted = true;
  result.bottle_present = true;
  result.runtime_present = true;
  result.helper_valid = true;
  result.profile_present = true;
  result.title_owned = true;
  result.inventory_confirmed = true;
  result.activity = NativeActivity::kStopped;
  result.settings_revision = record.settings_revision;
  return result;
}

struct Fixture {
  FakeRecords records;
  FakeJournal journal;
  LaunchPreflight preflight = ReadyPreflight(ReadyInstall());
  CloudLaunchCheck cloud{CloudLaunchState::kReady, "account-a", false, {}, {}};
  LaunchEvidence evidence;
  std::string current_account = "account-a";
  std::string uuid = kSession;
  std::int64_t now_ms = 1000;
  bool application_thread = true;
  bool flush_ok = true;
  int backend_calls = 0;
  int flush_calls = 0;
  std::vector<std::string> order;
  bool backend_saw_pending = false;
  bool backend_saw_flush = false;
  playden::ps5::TitleResult native_result{false, 0x80940010u, "launch"};

  Fixture() { records.records.emplace("install-hl2", ReadyInstall()); }

  LaunchDependencies Dependencies() {
    LaunchDependencies dependencies;
    dependencies.preflight = [&](const RuntimeInstall &) { return preflight; };
    dependencies.cloud = [&](const RuntimeInstall &, bool) { return cloud; };
    dependencies.evidence = [&](const RuntimeInstall &, const std::string &) {
      return evidence;
    };
    dependencies.backend = [&](const RuntimeInstall &record,
                               const std::string &session) {
      ++backend_calls;
      backend_saw_pending = journal.pending &&
                            journal.pending->phase == LaunchSessionPhase::kHandoffBegun;
      backend_saw_flush = flush_calls == 1;
      order.push_back("backend:" + record.title_id + ":session=" + session);
      return native_result;
    };
    dependencies.flush = [&] {
      ++flush_calls;
      order.push_back("flush");
      return flush_ok ? LaunchActionResult::Success()
                      : LaunchActionResult::Failure("flush-failed", "flush failed");
    };
    dependencies.uuid = [&] { return uuid; };
    dependencies.current_account_token = [&] { return current_account; };
    dependencies.now_ms = [&] { return now_ms; };
    dependencies.is_application_thread = [&] { return application_thread; };
    dependencies.evidence_wait_ms = 5000;
    return dependencies;
  }

  LaunchService Service() {
    return LaunchService(records, journal, Dependencies());
  }

  void RequestAndHandoff(LaunchService &service) {
    const auto requested = service.Request("install-hl2");
    CHECK(requested.accepted && requested.session_id == kSession);
    CHECK(service.Snapshot().state == LaunchState::kReadyToHandoff);
    CHECK(service.Handoff().accepted);
  }
};

void MissingAndUnreadyInstallsNeverInvokeBackend() {
  Fixture f;
  auto service = f.Service();
  CHECK(!service.Request("missing").accepted);
  f.records.records["install-hl2"].state = PreparationState::kPreparing;
  CHECK(!service.Request("install-hl2").accepted);
  CHECK(f.backend_calls == 0 && !f.journal.pending && f.journal.history.empty());
}

void EveryFreshPreflightGateBlocksBeforePersistenceOrBackend() {
  using Mutator = std::function<void(LaunchPreflight &)>;
  const std::vector<Mutator> invalid = {
      [](auto &p) { p.capability_available = false; },
      [](auto &p) { p.volume_mounted = false; },
      [](auto &p) { p.bottle_present = false; },
      [](auto &p) { p.runtime_present = false; },
      [](auto &p) { p.helper_valid = false; },
      [](auto &p) { p.profile_present = false; },
      [](auto &p) { p.title_owned = false; },
      [](auto &p) { p.inventory_confirmed = false; },
      [](auto &p) { p.activity = NativeActivity::kActive; },
      [](auto &p) { p.activity = NativeActivity::kUnknown; },
      [](auto &p) { ++p.settings_revision; },
  };
  for (const auto &mutate : invalid) {
    Fixture f;
    mutate(f.preflight);
    auto service = f.Service();
    const auto result = service.Request("install-hl2");
    CHECK(!result.accepted);
    CHECK(f.backend_calls == 0 && !f.journal.pending && f.journal.history.empty());
  }
}

void InvalidUuidAndPersistenceFailuresNeverInvokeBackend() {
  Fixture malformed;
  malformed.uuid = "not-a-uuid";
  auto malformed_service = malformed.Service();
  CHECK(!malformed_service.Request("install-hl2").accepted);
  CHECK(malformed.backend_calls == 0 && malformed.journal.history.empty());

  Fixture history;
  history.journal.fail_operation = "history";
  auto history_service = history.Service();
  CHECK(!history_service.Request("install-hl2").accepted);
  CHECK(history.backend_calls == 0 && !history.journal.pending);

  Fixture pending;
  pending.journal.fail_operation = "pending";
  auto pending_service = pending.Service();
  CHECK(!pending_service.Request("install-hl2").accepted);
  CHECK(pending.backend_calls == 0 && !pending.journal.pending);
  CHECK(pending.journal.history.size() == 1);
}

void DuplicateUuidNeverOverwritesHistory() {
  Fixture f;
  f.journal.history.emplace(kSession, LaunchSession{});
  auto service = f.Service();
  const auto result = service.Request("install-hl2");
  CHECK(!result.accepted && result.error == LaunchError::kPersistence);
  CHECK(f.journal.history.size() == 1 && !f.journal.pending && f.backend_calls == 0);
}

void PendingSessionBlocksDuplicatesAcrossRestart() {
  Fixture f;
  auto first = f.Service();
  CHECK(first.Request("install-hl2").accepted);
  CHECK(f.journal.pending && f.journal.history.size() == 1);

  auto restarted = f.Service();
  CHECK(!restarted.Request("install-hl2").accepted);
  CHECK(restarted.Snapshot().session_id == kSession);
  CHECK(restarted.Snapshot().state == LaunchState::kFailed);
  CHECK(!restarted.Handoff().accepted);
  CHECK(f.journal.history.size() == 1 && f.backend_calls == 0);
}

void HandoffRechecksPreflightThreadAccountAndFlush() {
  Fixture wrong_thread;
  auto service = wrong_thread.Service();
  CHECK(service.Request("install-hl2").accepted);
  wrong_thread.application_thread = false;
  CHECK(!service.Handoff().accepted && wrong_thread.backend_calls == 0);

  Fixture changed_settings;
  auto changed_service = changed_settings.Service();
  CHECK(changed_service.Request("install-hl2").accepted);
  ++changed_settings.preflight.settings_revision;
  CHECK(!changed_service.Handoff().accepted && changed_settings.backend_calls == 0);

  Fixture missing_volume;
  auto volume_service = missing_volume.Service();
  CHECK(volume_service.Request("install-hl2").accepted);
  missing_volume.preflight.volume_mounted = false;
  CHECK(!volume_service.Handoff().accepted && missing_volume.backend_calls == 0);

  Fixture account;
  auto account_service = account.Service();
  CHECK(account_service.Request("install-hl2").accepted);
  account.current_account = "account-b";
  CHECK(!account_service.Handoff().accepted && account.backend_calls == 0);

  Fixture flush;
  auto flush_service = flush.Service();
  CHECK(flush_service.Request("install-hl2").accepted);
  flush.flush_ok = false;
  CHECK(!flush_service.Handoff().accepted && flush.backend_calls == 0);
  CHECK(flush.flush_calls == 1);
}

void HandoffPersistsBeforeFlushAndBackend() {
  Fixture f;
  auto service = f.Service();
  CHECK(service.Request("install-hl2").accepted);
  f.journal.calls.clear();
  CHECK(service.Handoff().accepted);
  CHECK(f.journal.pending &&
        f.journal.pending->phase == LaunchSessionPhase::kHandoffBegun);
  CHECK(f.flush_calls == 1 && f.backend_calls == 1);
  CHECK(f.backend_saw_pending && f.backend_saw_flush);
  const auto pending_call = f.journal.calls.front();
  CHECK(pending_call == "pending:" +
                            std::to_string(static_cast<int>(LaunchSessionPhase::kHandoffBegun)));
  CHECK(f.order.size() == 2 && f.order[0] == "flush" &&
        f.order[1] == std::string("backend:FAKE78001:session=") + kSession);
}

void NativeReturnNeverSynthesizesRunning() {
  Fixture f;
  auto service = f.Service();
  f.native_result = {true, 0, "launch"};
  f.RequestAndHandoff(service);
  CHECK(service.Snapshot().state == LaunchState::kLaunching);
  f.now_ms += 6000;
  CHECK(service.Snapshot().state == LaunchState::kFailed);
  CHECK(f.journal.pending);
}

void ErrorWithStartRunsAndErrorWithoutStartFailsAfterBoundedWait() {
  Fixture started;
  auto started_service = started.Service();
  started.RequestAndHandoff(started_service);
  started.evidence = LaunchEvidence::Starting(ReadyInstall(), kSession);
  CHECK(started_service.Snapshot().state == LaunchState::kLaunching);
  started.evidence = LaunchEvidence::Running(ReadyInstall(), kSession);
  CHECK(started_service.Snapshot().state == LaunchState::kRunning);

  Fixture absent;
  auto absent_service = absent.Service();
  absent.RequestAndHandoff(absent_service);
  absent.now_ms += 6000;
  CHECK(absent_service.Snapshot().state == LaunchState::kFailed);
  CHECK(absent.journal.pending->native_code == 0x80940010u);

  Fixture other_error;
  other_error.native_result = {false, 71, "launch"};
  auto other_service = other_error.Service();
  other_error.RequestAndHandoff(other_service);
  other_error.now_ms += 6000;
  CHECK(other_service.Snapshot().state == LaunchState::kFailed);
}

void UnknownOrActiveActivityPreservesUncertainPending() {
  Fixture f;
  auto service = f.Service();
  f.RequestAndHandoff(service);
  f.now_ms += 6000;
  f.preflight.activity = NativeActivity::kUnknown;
  CHECK(service.Snapshot().state == LaunchState::kLaunching);
  f.preflight.activity = NativeActivity::kActive;
  CHECK(service.Snapshot().state == LaunchState::kLaunching);
  CHECK(f.journal.pending && !service.Request("install-hl2").accepted);
}

void RestartAfterHandoffNeverCallsBackendAgain() {
  Fixture f;
  auto service = f.Service();
  f.RequestAndHandoff(service);
  CHECK(f.backend_calls == 1);
  auto restarted = f.Service();
  CHECK(restarted.Snapshot().state == LaunchState::kLaunching);
  CHECK(!restarted.Handoff().accepted && f.backend_calls == 1);
}

void LostNativeResultWriteLeavesRecoverableHandoffPending() {
  Fixture f;
  f.journal.fail_pending_call = 3;
  auto service = f.Service();
  CHECK(service.Request("install-hl2").accepted);
  CHECK(!service.Handoff().accepted && f.backend_calls == 1);
  CHECK(f.journal.pending &&
        f.journal.pending->phase == LaunchSessionPhase::kHandoffBegun &&
        !f.journal.pending->has_native_result);
  auto restarted = f.Service();
  CHECK(restarted.Snapshot().state == LaunchState::kLaunching);
  CHECK(!restarted.Handoff().accepted && f.backend_calls == 1);
}

void FailedHandoffPublicationCannotRetryBackend() {
  Fixture f;
  f.journal.fail_pending_call = 2;
  auto service = f.Service();
  CHECK(service.Request("install-hl2").accepted);
  CHECK(!service.Handoff().accepted && f.backend_calls == 0);
  CHECK(!service.Handoff().accepted && f.backend_calls == 0);
  CHECK(f.journal.pending_calls == 2);
}

void WrapperIdentityAndRevisionAreValidatedBeforeStateMapping() {
  Fixture wrong;
  auto wrong_service = wrong.Service();
  wrong.RequestAndHandoff(wrong_service);
  wrong.evidence = LaunchEvidence::Running(ReadyInstall(), kSession);
  wrong.evidence.title_id = "FAKE78002";
  auto wrong_title = wrong_service.Snapshot();
  CHECK(wrong_title.state == LaunchState::kFailed &&
        wrong_title.error == LaunchError::kInvalidEvidence);

  Fixture revision;
  auto revision_service = revision.Service();
  revision.RequestAndHandoff(revision_service);
  revision.evidence = LaunchEvidence::Running(ReadyInstall(), kSession);
  ++revision.evidence.settings_revision;
  CHECK(revision_service.Snapshot().state == LaunchState::kFailed);

  Fixture malformed;
  auto malformed_service = malformed.Service();
  malformed.RequestAndHandoff(malformed_service);
  malformed.evidence = LaunchEvidence::Running(ReadyInstall(), kSession);
  malformed.evidence.valid = false;
  CHECK(malformed_service.Snapshot().state == LaunchState::kFailed);
}

void ExitRequiresValidatedTerminalEvidenceAndPreservesUnknownCode() {
  Fixture f;
  auto service = f.Service();
  f.RequestAndHandoff(service);
  f.evidence = LaunchEvidence::Exited(ReadyInstall(), kSession, std::nullopt);
  const auto exited = service.Snapshot();
  CHECK(exited.state == LaunchState::kExited && !exited.exit_code);
}

void RecordLeftRunningEndsAfterTitleStops() {
  Fixture f;
  auto service = f.Service();
  f.RequestAndHandoff(service);
  f.evidence = LaunchEvidence::Running(ReadyInstall(), kSession);
  f.preflight.activity = NativeActivity::kActive;
  CHECK(service.Snapshot().state == LaunchState::kRunning);
  f.preflight.activity = NativeActivity::kStopped;
  for (int i = 1; i < LaunchService::kStoppedPollsToEnd; ++i)
    CHECK(service.Snapshot().state == LaunchState::kRunning);
  // An active observation restarts the count.
  f.preflight.activity = NativeActivity::kActive;
  CHECK(service.Snapshot().state == LaunchState::kRunning);
  f.preflight.activity = NativeActivity::kStopped;
  for (int i = 1; i < LaunchService::kStoppedPollsToEnd; ++i)
    CHECK(service.Snapshot().state == LaunchState::kRunning);
  const auto ended = service.Snapshot();
  CHECK(ended.state == LaunchState::kExited && ended.code == "title-ended-without-record" && !ended.exit_code);
  CHECK(service.Finish().accepted && !f.journal.pending);
}

void NewSessionStartsItsOwnStoppedCount() {
  constexpr const char *kNextSession = "5b0f4c8e-3a7d-4f21-9c6e-2d8a1b7e4f90";
  Fixture f;
  auto service = f.Service();
  f.RequestAndHandoff(service);
  f.evidence = LaunchEvidence::Running(ReadyInstall(), kSession);
  f.preflight.activity = NativeActivity::kStopped;
  for (int i = 1; i < LaunchService::kStoppedPollsToEnd; ++i)
    CHECK(service.Snapshot().state == LaunchState::kRunning);
  f.evidence = LaunchEvidence::Exited(ReadyInstall(), kSession, 0);
  CHECK(service.Snapshot().state == LaunchState::kExited);
  CHECK(service.Finish().accepted);

  // Stopped observations of the earlier session must not end the next one.
  f.uuid = kNextSession;
  CHECK(service.Request("install-hl2").accepted);
  CHECK(service.Handoff().accepted);
  f.evidence = LaunchEvidence::Running(ReadyInstall(), kNextSession);
  CHECK(service.Snapshot().state == LaunchState::kRunning);
  for (int i = 2; i < LaunchService::kStoppedPollsToEnd; ++i)
    CHECK(service.Snapshot().state == LaunchState::kRunning);
  const auto ended = service.Snapshot();
  CHECK(ended.state == LaunchState::kExited && ended.code == "title-ended-without-record");
}

void FinishEndsOnlyDecidedSessions() {
  Fixture f;
  auto service = f.Service();
  CHECK(!service.Finish().accepted);
  f.RequestAndHandoff(service);
  f.evidence = LaunchEvidence::Running(ReadyInstall(), kSession);
  const auto running = service.Finish();
  CHECK(!running.accepted && running.code == "launch-undecided" && f.journal.pending);
  f.evidence = LaunchEvidence::Exited(ReadyInstall(), kSession, 0);
  CHECK(service.Finish().accepted && !f.journal.pending);
  CHECK(service.Snapshot().state == LaunchState::kExited);
  CHECK(f.journal.history.count(kSession) == 1);
  // The next launch starts with a new session and calls the backend again.
  f.uuid = "0f0e2d6c-3a52-4d7e-9d0b-6c51b1f1d2a3";
  f.evidence = {};
  CHECK(service.Request("install-hl2").accepted && service.Handoff().accepted);
  CHECK(f.backend_calls == 2);

  Fixture failed;
  auto failed_service = failed.Service();
  failed.RequestAndHandoff(failed_service);
  failed.now_ms += 6000;
  CHECK(failed_service.Snapshot().state == LaunchState::kFailed);
  CHECK(failed_service.Finish().accepted && !failed.journal.pending);

  // A session prepared before a restart never reached the backend.
  Fixture prepared;
  auto first = prepared.Service();
  CHECK(first.Request("install-hl2").accepted);
  auto restarted = prepared.Service();
  CHECK(restarted.Snapshot().state == LaunchState::kFailed);
  CHECK(restarted.Finish().accepted && !prepared.journal.pending);
  CHECK(prepared.backend_calls == 0);

  // A recovered handoff stays pending while the title may still be starting.
  Fixture recovered;
  auto before = recovered.Service();
  recovered.RequestAndHandoff(before);
  auto after = recovered.Service();
  CHECK(!after.Finish().accepted && recovered.journal.pending);
}

void CloudWaitCancelOfflineOptInAndAccountCoherence() {
  Fixture waiting;
  waiting.cloud = {CloudLaunchState::kWaiting, "account-a", false,
                   "cloud-waiting", "Cloud check is still running"};
  auto waiting_service = waiting.Service();
  CHECK(waiting_service.Request("install-hl2").accepted);
  CHECK(waiting_service.Snapshot().state == LaunchState::kWaitingForCloud);
  CHECK(waiting_service.Cancel().accepted);
  CHECK(waiting.backend_calls == 0 && !waiting.journal.pending);

  Fixture conflict;
  conflict.cloud = {CloudLaunchState::kConflict, "account-a", false,
                    "cloud-conflict", "Cloud conflict needs a choice"};
  auto conflict_service = conflict.Service();
  CHECK(conflict_service.Request("install-hl2").accepted);
  CHECK(!conflict_service.PlayWithoutSync().accepted);
  CHECK(conflict.journal.history.empty() && conflict.backend_calls == 0);

  Fixture offline;
  bool permit = false;
  auto dependencies = offline.Dependencies();
  dependencies.cloud = [&](const RuntimeInstall &, bool play_without_sync) {
    if (!play_without_sync)
      return CloudLaunchCheck{CloudLaunchState::kOffline, "account-a", true,
                              "cloud-offline", "Cloud is offline"};
    permit = true;
    return CloudLaunchCheck{CloudLaunchState::kReady, "account-a", true, {}, {}};
  };
  LaunchService offline_service(offline.records, offline.journal,
                                std::move(dependencies));
  CHECK(offline_service.Request("install-hl2").accepted);
  CHECK(offline_service.Snapshot().state == LaunchState::kWaitingForCloud);
  CHECK(offline_service.PlayWithoutSync().accepted && permit);
  CHECK(offline_service.Snapshot().state == LaunchState::kReadyToHandoff);

  Fixture changed;
  changed.cloud = {CloudLaunchState::kWaiting, "account-a", false,
                   "cloud-waiting", "waiting"};
  auto changed_service = changed.Service();
  CHECK(changed_service.Request("install-hl2").accepted);
  changed.current_account = "account-b";
  changed.cloud = {CloudLaunchState::kReady, "account-a", false, {}, {}};
  CHECK(!changed_service.Request("install-hl2").accepted);
  CHECK(changed.journal.history.empty() && changed.backend_calls == 0);
}

void ClockCannotCreateRunningEvidence() {
  Fixture f;
  auto service = f.Service();
  f.RequestAndHandoff(service);
  f.now_ms += 1000000;
  CHECK(service.Snapshot().state == LaunchState::kFailed);
  CHECK(service.Snapshot().state != LaunchState::kRunning);
}

void MissingFutureCloudHookAllowsTaskSixLaunch() {
  Fixture f;
  auto dependencies = f.Dependencies();
  dependencies.cloud = {};
  LaunchService service(f.records, f.journal, std::move(dependencies));
  const auto requested = service.Request("install-hl2");
  CHECK(requested.accepted && service.Snapshot().state == LaunchState::kReadyToHandoff);
}

void ConcreteJournalRequiresPrivateRootAndSupportedState() {
  namespace fs = std::filesystem;
  const auto base = fs::canonical(fs::temp_directory_path()) /
                    ("playden-launch-service-policy-" + std::to_string(getpid()));
  fs::remove_all(base);
  fs::create_directory(base);
  fs::permissions(base, fs::perms::owner_all | fs::perms::group_read |
                            fs::perms::group_exec | fs::perms::others_read |
                            fs::perms::others_exec);
  playden::Storage public_storage(base.string());
  StorageLaunchSessionJournal public_journal(public_storage);
  CHECK(!public_journal.PersistPending(JournalSession()).ok);
  CHECK(!fs::exists(base / "launch-pending.v1"));

  fs::remove_all(base);
  fs::create_directory(base);
  fs::permissions(base, fs::perms::owner_all);
  playden::Storage storage(base.string());
  StorageLaunchSessionJournal journal(storage);
  auto unsupported = JournalSession();
  unsupported.schema_version = 2;
  CHECK(!journal.PersistPending(unsupported).ok);
  unsupported = JournalSession();
  unsupported.phase = static_cast<LaunchSessionPhase>(99);
  CHECK(!journal.PersistPending(unsupported).ok);
  CHECK(!fs::exists(base / "launch-pending.v1"));
  fs::remove_all(base);
}

void ConcreteJournalRoundTripsAndHistoryIsImmutable() {
  namespace fs = std::filesystem;
  const auto root = fs::canonical(fs::temp_directory_path()) /
                    ("playden-launch-service-roundtrip-" + std::to_string(getpid()));
  fs::remove_all(root);
  fs::create_directory(root);
  fs::permissions(root, fs::perms::owner_all);
  playden::Storage storage(root.string());
  StorageLaunchSessionJournal journal(storage);
  auto session = JournalSession();
  CHECK(journal.CreateHistory(session).ok);
  const auto history_path = root / (std::string("launch-history-") + kSession + ".v1");
  const auto history = ReadFile(history_path);
  session.account_token = "replacement-must-not-win";
  CHECK(!journal.CreateHistory(session).ok);
  CHECK(ReadFile(history_path) == history);
  CHECK(journal.PersistPending(session).ok);
  StorageLaunchSessionJournal restarted(storage);
  const auto loaded = restarted.LoadPending();
  CHECK(loaded.ok && loaded.session &&
        loaded.session->settings_revision == std::numeric_limits<std::uint64_t>::max() &&
        loaded.session->requested_ms == std::numeric_limits<std::int64_t>::min() &&
        loaded.session->handoff_ms == std::numeric_limits<std::int64_t>::max() &&
        loaded.session->account_token == "replacement-must-not-win");
  CHECK(Mode(root / "launch-pending.v1") == 0600);
  CHECK(Mode(history_path) == 0600);
  CHECK(restarted.ClearPending().ok);
  CHECK(!fs::exists(root / "launch-pending.v1"));
  CHECK(restarted.LoadPending().ok && !restarted.LoadPending().session);
  fs::remove_all(root);
}

void ConcreteJournalRefusesSymlinks() {
  namespace fs = std::filesystem;
  const auto root = fs::canonical(fs::temp_directory_path()) /
                    ("playden-launch-service-symlink-" + std::to_string(getpid()));
  fs::remove_all(root);
  fs::create_directory(root);
  fs::permissions(root, fs::perms::owner_all);
  const auto target = root.parent_path() / (root.filename().string() + "-target");
  { std::ofstream(target) << "preserve"; }
  CHECK(::symlink(target.c_str(), (root / "launch-pending.v1").c_str()) == 0);
  playden::Storage storage(root.string());
  StorageLaunchSessionJournal journal(storage);
  CHECK(!journal.LoadPending().ok);
  CHECK(!journal.PersistPending(JournalSession()).ok);
  CHECK(fs::is_symlink(root / "launch-pending.v1"));
  CHECK(ReadFile(target) == "preserve");
  fs::remove_all(root);
  fs::remove(target);
}

#if defined(PLAYDEN_TEST_DURABLE_FAULTS)
void ConcreteJournalReportsSyncFailuresForReconciliation() {
  namespace fs = std::filesystem;
  const auto root = fs::canonical(fs::temp_directory_path()) /
                    ("playden-launch-service-sync-" + std::to_string(getpid()));
  fs::remove_all(root);
  fs::create_directory(root);
  fs::permissions(root, fs::perms::owner_all);
  playden::Storage storage(root.string());
  StorageLaunchSessionJournal journal(storage);

  fail_file_sync = true;
  CHECK(!journal.PersistPending(JournalSession()).ok);
  CHECK(!fail_file_sync);
  CHECK(!fs::exists(root / "launch-pending.v1"));

  fail_directory_sync = true;
  CHECK(!journal.PersistPending(JournalSession()).ok);
  CHECK(!fail_directory_sync);
  const auto recovered = journal.LoadPending();
  CHECK(recovered.ok && recovered.session && recovered.session->session_id == kSession);

  fail_directory_sync = true;
  CHECK(!journal.ClearPending().ok);
  CHECK(!fail_directory_sync);
  CHECK(!fs::exists(root / "launch-pending.v1"));
  const auto absent = journal.LoadPending();
  CHECK(absent.ok && !absent.session);
  fs::remove_all(root);
}
#endif

} // namespace

extern "C" int playden_test_fsync(int fd) {
  struct stat status {};
  if (::fstat(fd, &status) == 0) {
    if (S_ISREG(status.st_mode) && fail_file_sync) {
      fail_file_sync = false;
      errno = EIO;
      return -1;
    }
    if (S_ISDIR(status.st_mode) && fail_directory_sync) {
      fail_directory_sync = false;
      errno = EIO;
      return -1;
    }
  }
  return ::fsync(fd);
}

void RequestedSettingsUpdateBlocksPlayAndHandoff() {
  Fixture f; auto service = f.Service();
  f.records.records["install-hl2"].journal = PreparationJournal{kSession, "settings-request", "capture-update"};
  CHECK(!service.Request("install-hl2").accepted);
  CHECK(f.backend_calls == 0);
  f.records.records["install-hl2"].journal.reset();
  CHECK(service.Request("install-hl2").accepted);
  f.records.records["install-hl2"].journal = PreparationJournal{kSession, "settings-request", "capture-update"};
  CHECK(!service.Handoff().accepted && f.backend_calls == 0);
}
int main() {
  RequestedSettingsUpdateBlocksPlayAndHandoff();
  MissingAndUnreadyInstallsNeverInvokeBackend();
  EveryFreshPreflightGateBlocksBeforePersistenceOrBackend();
  InvalidUuidAndPersistenceFailuresNeverInvokeBackend();
  DuplicateUuidNeverOverwritesHistory();
  PendingSessionBlocksDuplicatesAcrossRestart();
  HandoffRechecksPreflightThreadAccountAndFlush();
  HandoffPersistsBeforeFlushAndBackend();
  NativeReturnNeverSynthesizesRunning();
  ErrorWithStartRunsAndErrorWithoutStartFailsAfterBoundedWait();
  UnknownOrActiveActivityPreservesUncertainPending();
  RestartAfterHandoffNeverCallsBackendAgain();
  LostNativeResultWriteLeavesRecoverableHandoffPending();
  FailedHandoffPublicationCannotRetryBackend();
  WrapperIdentityAndRevisionAreValidatedBeforeStateMapping();
  ExitRequiresValidatedTerminalEvidenceAndPreservesUnknownCode();
  FinishEndsOnlyDecidedSessions();
  RecordLeftRunningEndsAfterTitleStops();
  NewSessionStartsItsOwnStoppedCount();
  CloudWaitCancelOfflineOptInAndAccountCoherence();
  ClockCannotCreateRunningEvidence();
  MissingFutureCloudHookAllowsTaskSixLaunch();
  ConcreteJournalRequiresPrivateRootAndSupportedState();
  ConcreteJournalRoundTripsAndHistoryIsImmutable();
  ConcreteJournalRefusesSymlinks();
#if defined(PLAYDEN_TEST_DURABLE_FAULTS)
  ConcreteJournalReportsSyncFailuresForReconciliation();
#endif
  std::puts("runtime_launch_service_test: all assertions passed");
  return EXIT_SUCCESS;
}
