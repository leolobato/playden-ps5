#include "runtime/session_storage.h"

extern "C" {
#include "pw_title_session.h"
}

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <sys/stat.h>
#include <unordered_map>
#include <unistd.h>

using namespace playden;
using namespace playden::runtime;

#define CHECK(condition)                                                        \
  do {                                                                          \
    if (!(condition)) {                                                         \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__,    \
                   #condition);                                                 \
      std::exit(1);                                                             \
    }                                                                           \
  } while (false)

namespace {
namespace fs = std::filesystem;

constexpr const char *kSession = "123e4567-e89b-42d3-a456-426614174000";
constexpr const char *kV1Session = "123e4567-e89b-12d3-a456-426614174000";

struct TemporaryDirectory {
  TemporaryDirectory() {
    char pattern[] = "/tmp/playden-session-storage-XXXXXX";
    CHECK(::mkdtemp(pattern));
    path = fs::canonical(pattern);
    CHECK(::chmod(path.c_str(), 0700) == 0);
  }
  ~TemporaryDirectory() { fs::remove_all(path); }
  fs::path path;
};

void PrivateDirectories(const fs::path &path) {
  fs::create_directories(path);
  const auto temporary_root = fs::canonical("/tmp");
  for (auto current = path; !current.empty() && current != temporary_root;
       current = current.parent_path()) {
    CHECK(::chmod(current.c_str(), 0700) == 0);
  }
}

void Write(const fs::path &path, const std::string &bytes, mode_t mode = 0600) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  CHECK(output.good());
  output << bytes;
  output.close();
  CHECK(output.good());
  CHECK(::chmod(path.c_str(), mode) == 0);
}

RuntimeInstall Record() {
  RuntimeInstall record;
  record.install_id = "install-hl2";
  record.title_id = "FAKE78001";
  record.runtime_number = 220;
  record.data_root = "/data/playden/games/install-hl2";
  record.runtime_bundle_id = "prospero-win-e16ec8f";
  record.recipe_id = "hl2";
  record.recipe_version = "1";
  record.settings_revision = std::numeric_limits<std::uint64_t>::max();
  record.install_path = "/games/220";
  record.state = PreparationState::kReady;
  return record;
}

fs::path MappedGame(const TemporaryDirectory &temporary) {
  return temporary.path / "playden/games/install-hl2";
}

std::string OwnerMarker(const RuntimeInstall &record) {
  return "schema=1\nkind=template\ninstall_id=" + record.install_id +
         "\ntitle_id=" + record.title_id +
         "\nkit_id=playden-prospero-kit-v1\nfiles_sha256=" +
         std::string(64, 'a') + "\nruntime_bundle_id=" + record.runtime_bundle_id +
         "\nruntime_revision=e16ec8f\nruntime_path=runtime\nvariant_id=hl2\n"
         "template_path=template\nrecipe_id=hl2\nrecipe_version=1\n";
}

void AdmitOwnedGame(const TemporaryDirectory &temporary,
                    const RuntimeInstall &record) {
  const auto game = MappedGame(temporary);
  PrivateDirectories(game / "library/prefixes/game");
  Write(game / "library/prefixes/game/.playden-template-owner",
        OwnerMarker(record));
}

std::string SessionJson(const RuntimeInstall &record, const std::string &id,
                        const std::string &phase = "exited") {
  return "{\"schema_version\":1,\"session_id\":\"" + id +
         "\",\"install_id\":\"" + record.install_id +
         "\",\"title_id\":\"" + record.title_id +
         "\",\"origin\":\"playden\",\"settings_revision\":" +
         std::to_string(record.settings_revision) +
         ",\"runtime_bundle_id\":\"" + record.runtime_bundle_id +
         "\",\"phase\":\"" + phase + "\",\"exit_code\":0}\n";
}

void ActualWriterFixtureIsAdmittedThroughCanonicalDataMapping() {
  TemporaryDirectory temporary;
  const auto record = Record();
  AdmitOwnedGame(temporary, record);

  PwTitleLaunch launch{};
  launch.schema_version = 1;
  launch.app_id = record.runtime_number;
  launch.settings_revision = record.settings_revision;
  const auto copy = [](auto &output, const std::string &input) {
    CHECK(input.size() < sizeof(output));
    std::strcpy(output, input.c_str());
  };
  copy(launch.install_id, record.install_id);
  copy(launch.title_id, record.title_id);
  copy(launch.runtime_bundle_id, record.runtime_bundle_id);
  copy(launch.data_root, MappedGame(temporary).string());
  const auto produce = [&](const char *argument, int exit_code) {
    PwTitleSession produced{};
    char error[256]{};
    const int started = pw_title_session_start(&produced, &launch, argument,
                                                error, sizeof(error));
    if (started != 0)
      std::fprintf(stderr, "producer start failed: %s\n", error);
    CHECK(started == 0);
    CHECK(pw_title_session_running(&produced) == 0);
    CHECK(pw_title_session_exited(&produced, 1, exit_code) == 0);
  };
  produce("session=123e4567-e89b-42d3-a456-426614174000", -7);
  produce("session=123e4567-e89b-12d3-a456-426614174000", 0);

  produce("session=123e4567-e89b-72d3-c456-426614174000", 0);

  FilesystemReconcileSessionSource source(
      HostDataRootForTests{temporary.path});
  const auto result = source.Enumerate(record, 8);
  CHECK(result.ok && result.sessions.size() == 3);
  const auto &session = result.sessions[1];
  CHECK(session.schema_version == 1 && session.session_id == kSession);
  CHECK(session.install_id == record.install_id &&
        session.title_id == record.title_id);
  CHECK(session.runtime_bundle_id == record.runtime_bundle_id);
  CHECK(session.settings_revision == std::numeric_limits<std::uint64_t>::max());
  CHECK(session.origin == SessionOrigin::kPlayden);
  CHECK(session.phase == WrapperSessionPhase::kExited);
  CHECK(session.exit_code && *session.exit_code == -7);
  CHECK(result.sessions.front().session_id == kV1Session);
}

void SourceAcceptsCanonicalV1UuidAndRejectsIdentityOrNumericDrift() {
  TemporaryDirectory temporary;
  const auto record = Record();
  AdmitOwnedGame(temporary, record);
  const auto sessions = MappedGame(temporary) / "logs/sessions";
  PrivateDirectories(sessions);
  Write(sessions / (std::string(kV1Session) + ".json"),
        SessionJson(record, kV1Session));
  FilesystemReconcileSessionSource source(HostDataRootForTests{temporary.path});
  auto result = source.Enumerate(record, 8);
  CHECK(result.ok && result.sessions.size() == 1 &&
        result.sessions.front().session_id == kV1Session);

  Write(sessions / (std::string(kV1Session) + ".json"),
        SessionJson(record, kSession));
  result = source.Enumerate(record, 8);
  CHECK(!result.ok && result.sessions.empty());

  Write(sessions / (std::string(kV1Session) + ".json"),
        SessionJson(record, kV1Session));
  auto bytes = SessionJson(record, kV1Session);
  const auto exact = std::to_string(std::numeric_limits<std::uint64_t>::max());
  bytes.replace(bytes.find(exact), exact.size(), "18446744073709551616");
  Write(sessions / (std::string(kV1Session) + ".json"), bytes);
  result = source.Enumerate(record, 8);
  CHECK(!result.ok && result.sessions.empty());
}

// A title shell with publication-open-modes makes its session folders 0777 (#868); an older one 0700. Both are read.
void SourceReadsOpenSessionFolders() {
  for (const mode_t mode : {mode_t{0755}, mode_t{0777}}) {
    TemporaryDirectory temporary;
    const auto record = Record();
    AdmitOwnedGame(temporary, record);
    const auto sessions = MappedGame(temporary) / "logs/sessions";
    PrivateDirectories(sessions);
    Write(sessions / (std::string(kSession) + ".json"), SessionJson(record, kSession));
    CHECK(::chmod(sessions.parent_path().c_str(), mode) == 0 && ::chmod(sessions.c_str(), mode) == 0);
    FilesystemReconcileSessionSource source(HostDataRootForTests{temporary.path});
    const auto result = source.Enumerate(record, 8);
    CHECK(result.ok && result.sessions.size() == 1);
  }
}

void SourceTreatsUnsafeOrAmbiguousEvidenceAsFailure() {
  const auto run = [](const auto &damage) {
    TemporaryDirectory temporary;
    const auto record = Record();
    AdmitOwnedGame(temporary, record);
    const auto sessions = MappedGame(temporary) / "logs/sessions";
    PrivateDirectories(sessions);
    Write(sessions / (std::string(kSession) + ".json"),
          SessionJson(record, kSession));
    damage(temporary, record, sessions);
    FilesystemReconcileSessionSource source(HostDataRootForTests{temporary.path});
    const auto result = source.Enumerate(record, 8);
    CHECK(!result.ok && result.sessions.empty());
  };

  run([](const auto &temporary, const auto &, const auto &) {
    fs::remove(MappedGame(temporary) /
               "library/prefixes/game/.playden-template-owner");
  });
  run([](const auto &temporary, const auto &, const auto &) {
    CHECK(::chmod((MappedGame(temporary) /
                   "library/prefixes/game/.playden-template-owner")
                      .c_str(),
                  0644) == 0);
  });
  run([](const auto &temporary, const auto &record, const auto &) {
    auto marker = OwnerMarker(record);
    marker.replace(marker.find("install-hl2"), std::strlen("install-hl2"),
                   "other-install");
    Write(MappedGame(temporary) /
              "library/prefixes/game/.playden-template-owner",
          marker);
  });
  run([](const auto &temporary, const auto &record, const auto &) {
    auto marker = OwnerMarker(record);
    marker += "install_id=install-hl2\n";
    Write(MappedGame(temporary) /
              "library/prefixes/game/.playden-template-owner",
          marker);
  });
  run([](const auto &temporary, const auto &record, const auto &) {
    auto marker = OwnerMarker(record);
    marker.replace(marker.find(std::string(64, 'a')), 64, std::string(64, 'A'));
    Write(MappedGame(temporary) /
              "library/prefixes/game/.playden-template-owner",
          marker);
  });
  run([](const auto &temporary, const auto &record, const auto &) {
    auto marker = OwnerMarker(record);
    marker.replace(marker.find("runtime_path=runtime"),
                   std::strlen("runtime_path=runtime"),
                   "runtime_path=../runtime");
    Write(MappedGame(temporary) /
              "library/prefixes/game/.playden-template-owner",
          marker);
  });
  run([](const auto &temporary, const auto &record, const auto &) {
    auto marker = OwnerMarker(record);
    marker.replace(marker.find("recipe_version=1"),
                   std::strlen("recipe_version=1"), "recipe_version=2");
    Write(MappedGame(temporary) /
              "library/prefixes/game/.playden-template-owner",
          marker);
  });
  run([](const auto &, const auto &, const auto &sessions) {
    const auto target = sessions.parent_path() / "other-sessions";
    fs::rename(sessions, target);
    CHECK(::symlink(target.c_str(), sessions.c_str()) == 0);
  });
  run([](const auto &, const auto &, const auto &sessions) {
    CHECK(::chmod((sessions / (std::string(kSession) + ".json")).c_str(),
                  0644) == 0);
  });
  run([](const auto &, const auto &, const auto &sessions) {
    Write(sessions / "orphan.json.tmp", "incomplete");
  });
  run([](const auto &, const auto &, const auto &sessions) {
    Write(sessions / (std::string(kSession) + ".json"),
          std::string(FilesystemReconcileSessionSource::kMaxSessionBytes + 1,
                      'x'));
  });
}

void SourceBoundsEnumerationAndRejectsNonCanonicalRecordRoots() {
  TemporaryDirectory temporary;
  const auto record = Record();
  AdmitOwnedGame(temporary, record);
  const auto sessions = MappedGame(temporary) / "logs/sessions";
  PrivateDirectories(sessions);
  Write(sessions / (std::string(kSession) + ".json"),
        SessionJson(record, kSession));
  Write(sessions / (std::string(kV1Session) + ".json"),
        SessionJson(record, kV1Session));
  FilesystemReconcileSessionSource source(HostDataRootForTests{temporary.path});
  CHECK(!source.Enumerate(record, 1).ok);

  auto arbitrary = record;
  arbitrary.data_root = MappedGame(temporary).string();
  CHECK(!source.Enumerate(arbitrary, 8).ok);
  arbitrary = record;
  arbitrary.install_id = "../install-hl2";
  CHECK(!source.Enumerate(arbitrary, 8).ok);
}

void HistoricalRuntimeIdentityRemainsVisibleBesideCurrentEvidence() {
  TemporaryDirectory temporary;
  const auto record = Record();
  AdmitOwnedGame(temporary, record);
  const auto sessions = MappedGame(temporary) / "logs/sessions";
  PrivateDirectories(sessions);
  Write(sessions / (std::string(kSession) + ".json"),
        SessionJson(record, kSession));
  auto historical = record;
  historical.settings_revision = 4;
  historical.runtime_bundle_id = "prospero-win-previous";
  Write(sessions / (std::string(kV1Session) + ".json"),
        SessionJson(historical, kV1Session));

  FilesystemReconcileSessionSource source(HostDataRootForTests{temporary.path});
  const auto result = source.Enumerate(record, 8);
  CHECK(result.ok && result.sessions.size() == 2);
  CHECK(result.sessions[0].session_id == kV1Session);
  CHECK(result.sessions[0].settings_revision == 4);
  CHECK(result.sessions[0].runtime_bundle_id == "prospero-win-previous");
  CHECK(result.sessions[1].session_id == kSession);
  CHECK(result.sessions[1].settings_revision == record.settings_revision);

  struct Journal final : ReconcileJournal {
    std::unordered_map<std::string, ReconcileEntry> entries;
    ReconcileJournalLoad Load(const std::string &id) override {
      const auto found = entries.find(id);
      return ReconcileJournalLoad::Success(
          found == entries.end() ? std::optional<ReconcileEntry>{}
                                 : std::optional<ReconcileEntry>{found->second});
    }
    ReconcileJournalResult Save(const ReconcileEntry &entry) override {
      entries[entry.session.session_id] = entry;
      return ReconcileJournalResult::Success();
    }
  } journal;
  struct Cloud final : ReconcileCloud {
    int starts = 0;
    CloudOperationResult StartCheck(const SessionCloudRequest &request) override {
      ++starts;
      return CloudOperationResult::Succeeded("operation-current",
                                             request.session.session_id,
                                             request.account_token);
    }
    CloudOperationResult Poll(const std::string &, const std::string &,
                              const std::string &) override {
      CHECK(false);
      return {};
    }
    void Cancel(const std::string &, const std::string &,
                const std::string &) override {
      CHECK(false);
    }
  } cloud;
  SessionReconcileDependencies dependencies;
  dependencies.activity = [](const std::string &) {
    return NativeActivity::kStopped;
  };
  dependencies.current_account_token = [] { return "account-current"; };
  dependencies.owned = [](const RuntimeInstall &) { return true; };
  SessionReconciler reconciler(source, journal, cloud, dependencies);
  const auto reconciled = reconciler.Reconcile({record});
  CHECK(reconciled.rejected_sessions == 1 && reconciled.sessions.size() == 2);
  CHECK(reconciled.sessions[0].session_id == kV1Session &&
        reconciled.sessions[0].state == SessionReconcileState::kRejected);
  CHECK(reconciled.sessions[1].session_id == kSession &&
        reconciled.sessions[1].state == SessionReconcileState::kCompleted);
  CHECK(cloud.starts == 1);
}

ReconcileEntry Entry() {
  const auto record = Record();
  ReconcileEntry entry;
  entry.session.schema_version = 1;
  entry.session.session_id = kSession;
  entry.session.origin = SessionOrigin::kHome;
  entry.session.install_id = record.install_id;
  entry.session.title_id = record.title_id;
  entry.session.runtime_bundle_id = record.runtime_bundle_id;
  entry.session.settings_revision = record.settings_revision;
  entry.session.phase = WrapperSessionPhase::kFailed;
  entry.session.exit_code = -7;
  entry.account_token = "account-7";
  entry.phase = ReconcilePhase::kCloudRunning;
  entry.operation_id = "operation-9";
  entry.code = "retry";
  entry.message = "retry after restart";
  return entry;
}

void DurableJournalRoundTripsAcrossRestartAndOverwritesOneSession() {
  TemporaryDirectory temporary;
  Storage storage(temporary.path.string());
  DurableReconcileJournal first(storage);
  const auto missing = first.Load(kSession);
  CHECK(missing.ok && !missing.entry);
  auto entry = Entry();
  CHECK(first.Save(entry).ok);
  struct stat info{};
  const auto path = temporary.path / (std::string(kSession) + ".json");
  CHECK(::lstat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode) &&
        (info.st_mode & 07777) == 0600 && info.st_uid == ::geteuid());

  DurableReconcileJournal restarted(storage);
  auto loaded = restarted.Load(kSession);
  CHECK(loaded.ok && loaded.entry);
  CHECK(loaded.entry->session.settings_revision ==
        std::numeric_limits<std::uint64_t>::max());
  CHECK(loaded.entry->schema_version == 1 &&
        loaded.entry->session.schema_version == 1);
  CHECK(loaded.entry->session.session_id == kSession);
  CHECK(loaded.entry->session.install_id == "install-hl2");
  CHECK(loaded.entry->session.title_id == "FAKE78001");
  CHECK(loaded.entry->session.runtime_bundle_id == "prospero-win-e16ec8f");
  CHECK(loaded.entry->session.origin == SessionOrigin::kHome);
  CHECK(loaded.entry->session.phase == WrapperSessionPhase::kFailed);
  CHECK(loaded.entry->session.exit_code == -7);
  CHECK(loaded.entry->account_token == "account-7" &&
        loaded.entry->operation_id == "operation-9");
  CHECK(loaded.entry->phase == ReconcilePhase::kCloudRunning);
  CHECK(loaded.entry->code == "retry" &&
        loaded.entry->message == "retry after restart");

  entry.phase = ReconcilePhase::kCompleted;
  entry.code.clear();
  entry.message.clear();
  CHECK(restarted.Save(entry).ok);
  loaded = first.Load(kSession);
  CHECK(loaded.ok && loaded.entry &&
        loaded.entry->phase == ReconcilePhase::kCompleted);
  std::size_t regular = 0;
  for (const auto &item : fs::directory_iterator(temporary.path))
    if (item.is_regular_file()) ++regular;
  CHECK(regular == 1);
}

void DurableJournalRequiresOperationForRecordedCloudCompletion() {
  TemporaryDirectory temporary;
  Storage storage(temporary.path.string());
  DurableReconcileJournal journal(storage);
  for (const auto phase : {ReconcilePhase::kCloudRunning, ReconcilePhase::kCompletionPending, ReconcilePhase::kCompleted}) {
    auto entry = Entry();
    entry.phase = phase;
    CHECK(journal.Save(entry).ok);
    const auto path = temporary.path / (std::string(kSession) + ".json");
    std::ifstream input(path, std::ios::binary);
    std::string bytes{std::istreambuf_iterator<char>(input), {}};
    CHECK(input.good() || input.eof());
    const auto operation = bytes.find("operation-9");
    CHECK(operation != std::string::npos);
    bytes.erase(operation, std::strlen("operation-9"));
    Write(path, bytes);
    CHECK(!journal.Load(kSession).ok);
    entry.operation_id.clear();
    CHECK(!journal.Save(entry).ok);
  }
}

void DurableJournalAcceptsPinnedProducerIdGrammar() {
  TemporaryDirectory temporary;
  Storage storage(temporary.path.string());
  DurableReconcileJournal journal(storage);
  auto entry = Entry();
  entry.session.session_id = "123e4567-e89b-72d3-c456-426614174000";
  CHECK(journal.Save(entry).ok);
  const auto loaded = journal.Load(entry.session.session_id);
  CHECK(loaded.ok && loaded.entry &&
        loaded.entry->session.session_id == entry.session.session_id);
}

void DurableJournalRejectsWrongIdentitySchemaCorruptionAndPrivatePathProblems() {
  TemporaryDirectory temporary;
  Storage storage(temporary.path.string());
  DurableReconcileJournal journal(storage);
  CHECK(!journal.Load("../../bad").ok);
  CHECK(journal.Save(Entry()).ok);
  const auto path = temporary.path / (std::string(kSession) + ".json");

  std::ifstream input(path, std::ios::binary);
  const std::string bytes{std::istreambuf_iterator<char>(input), {}};
  CHECK(input.good() || input.eof());
  auto wrong = bytes;
  wrong.replace(wrong.find(kSession), std::strlen(kSession), kV1Session);
  Write(path, wrong);
  CHECK(!journal.Load(kSession).ok);
  auto schema = bytes;
  schema.replace(schema.find("\"schema_version\":1"),
                 std::strlen("\"schema_version\":1"),
                 "\"schema_version\":2");
  Write(path, schema);
  CHECK(!journal.Load(kSession).ok);
  auto phase = bytes;
  const auto phase_at = phase.find("\"phase\":\"cloud-running\"");
  CHECK(phase_at != std::string::npos);
  phase.replace(phase_at, std::strlen("\"phase\":\"cloud-running\""),
                "\"phase\":\"unknown\"");
  Write(path, phase);
  CHECK(!journal.Load(kSession).ok);
  Write(path, "{broken");
  CHECK(!journal.Load(kSession).ok);
  Write(path, std::string(DurableReconcileJournal::kMaxJournalBytes + 1, 'x'));
  CHECK(!journal.Load(kSession).ok);
  Write(path, bytes, 0640);
  CHECK(!journal.Load(kSession).ok);

  CHECK(::chmod(temporary.path.c_str(), 0755) == 0);
  CHECK(!journal.Load(kSession).ok);
  CHECK(!journal.Save(Entry()).ok);
  CHECK(::chmod(temporary.path.c_str(), 0700) == 0);
  Write(path, bytes);
  CHECK(journal.Load(kSession).ok);

  const auto alias = temporary.path.parent_path() /
                     (temporary.path.filename().string() + "-alias");
  CHECK(::symlink(temporary.path.c_str(), alias.c_str()) == 0);
  Storage aliased_storage(alias.string());
  DurableReconcileJournal aliased(aliased_storage);
  CHECK(!aliased.Load(kSession).ok);
  CHECK(!aliased.Save(Entry()).ok);
  fs::remove(alias);
}

} // namespace

int main() {
  ActualWriterFixtureIsAdmittedThroughCanonicalDataMapping();
  SourceAcceptsCanonicalV1UuidAndRejectsIdentityOrNumericDrift();
  SourceTreatsUnsafeOrAmbiguousEvidenceAsFailure();
  SourceReadsOpenSessionFolders();
  SourceBoundsEnumerationAndRejectsNonCanonicalRecordRoots();
  HistoricalRuntimeIdentityRemainsVisibleBesideCurrentEvidence();
  DurableJournalRoundTripsAcrossRestartAndOverwritesOneSession();
  DurableJournalAcceptsPinnedProducerIdGrammar();
  DurableJournalRequiresOperationForRecordedCloudCompletion();
  DurableJournalRejectsWrongIdentitySchemaCorruptionAndPrivatePathProblems();
  return 0;
}
