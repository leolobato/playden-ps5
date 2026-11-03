#include "runtime/game_install.h"

#include "platform/storage.h"
#include "library/store.h"

#include <cstdio>
#include <set>
#include <cerrno>
#include <dlfcn.h>
#include <sys/stat.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <unistd.h>
#include <unordered_map>
#include <vector>

using namespace playden::runtime;

#define CHECK(condition)                                                                  \
  do {                                                                                    \
    if (!(condition)) {                                                                   \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
      std::exit(1);                                                                       \
    }                                                                                     \
  } while (false)

namespace {
enum class SyncFault { kNone, kFile, kDirectory };
SyncFault sync_fault = SyncFault::kNone;
}
extern "C" int fsync(int fd) {
  struct stat info{};
  if (fstat(fd, &info) == 0 &&
      ((sync_fault == SyncFault::kFile && S_ISREG(info.st_mode)) ||
       (sync_fault == SyncFault::kDirectory && S_ISDIR(info.st_mode)))) {
    errno = EIO;
    return -1;
  }
  static const auto actual = reinterpret_cast<int (*)(int)>(dlsym(RTLD_NEXT, "fsync"));
  if (!actual) { errno = ENOSYS; return -1; }
  return actual(fd);
}

namespace {

struct TemporaryDirectory {
  TemporaryDirectory() {
    auto pattern = (std::filesystem::temp_directory_path() /
                    "playden-runtime-install-XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    const char* made = ::mkdtemp(writable.data());
    CHECK(made != nullptr);
    path = std::filesystem::canonical(made).string();
  }
  ~TemporaryDirectory() { std::filesystem::remove_all(path); }
  std::string path;
};

ReserveRequest Request(std::string install_id = "install-hl2") {
  ReserveRequest request;
  request.install_id = std::move(install_id);
  request.runtime_number = 220;
  request.default_profile_id = "game";
  request.runtime_bundle_id = "prospero-win-e16ec8f";
  request.recipe_id = "steam-220-hl2-v1";
  request.recipe_version = "1";
  request.settings_revision = std::numeric_limits<std::uint64_t>::max();
  request.launch_entry_id = "0";
  request.launch_path = "hl2.exe";
  request.launch_arguments = "-game hl2 -steam";
  request.launch_working_directory = "";
  request.install_path = "/mnt/ext1/playden/games/220";
  request.volume_id = "m2";
  request.kit_title_variants = {"FAKE78000", "FAKE78001", "FAKE78002"};
  return request;
}

NativeTitleAvailability Free(const std::string&) {
  return {TitleAvailability::kFree, TitleAvailability::kFree};
}

void ReservationSurvivesRestartAndReinstallReusesIdentity() {
  TemporaryDirectory directory;
  playden::Storage storage(directory.path);
  CHECK(storage.EnsureRootDirectory().ok());

  RuntimeInstallRepository first(storage, "runtime-installs.json", Free);
  CHECK(first.Load().ok());
  auto reserved = first.Reserve(Request());
  CHECK(reserved.ok());
  CHECK(reserved.value().title_id == "FAKE78000");
  CHECK(reserved.value().data_root == "/data/playden/games/install-hl2");
  CHECK(reserved.value().exit_policy == "exit-title");

  RuntimeInstallRepository restarted(
      storage, "runtime-installs.json", [](const std::string&) {
        return NativeTitleAvailability{TitleAvailability::kOccupied,
                                       TitleAvailability::kOccupied};
      });
  CHECK(restarted.Load().ok());
  const auto found = restarted.Find("install-hl2");
  CHECK(found && found->title_id == "FAKE78000");
  CHECK(found->settings_revision == std::numeric_limits<std::uint64_t>::max());
  CHECK(found->launch_entry_id == "0" && found->launch_arguments == "-game hl2 -steam");
  auto reinstall = Request();
  reinstall.kit_title_variants = {"FAKE78002"};
  const auto reused = restarted.Reserve(reinstall);
  CHECK(reused.ok() && reused.value().title_id == "FAKE78000");
  CHECK(restarted.Snapshot().size() == 1);
}

void NativeInventoryAndSourceCollisionsAreNeverAllocated() {
  TemporaryDirectory directory;
  playden::Storage storage(directory.path);
  std::unordered_map<std::string, NativeTitleAvailability> availability{
      {"FAKE78000", {TitleAvailability::kOccupied, TitleAvailability::kFree}},
      {"FAKE78001", {TitleAvailability::kFree, TitleAvailability::kUnknown}},
      {"FAKE78002", {TitleAvailability::kUnknown, TitleAvailability::kFree}},
      {"FAKE78003", {TitleAvailability::kFree, TitleAvailability::kFree}},
  };
  RuntimeInstallRepository repository(
      storage, "runtime-installs.json", [&](const std::string& title) {
        return availability.at(title);
      });
  CHECK(repository.Load().ok());
  auto collision_request = Request("collision-test");
  collision_request.kit_title_variants.push_back("FAKE78003");
  const auto reserved = repository.Reserve(collision_request);
  CHECK(reserved.ok() && reserved.value().title_id == "FAKE78003");

  auto exhausted = Request("exhausted-test");
  exhausted.kit_title_variants = {"PPSA99995", "FAKE77031", "FAKE78000", "FAKE78001",
                                  "FAKE78002", "FAKE78003"};
  const auto rejected = repository.Reserve(exhausted);
  CHECK(!rejected.ok() && rejected.error() == RepositoryError::kTitleVariantsExhausted);
}

void LegacyDownloadAdoptsWithoutCopyAndRemainsDownloaded() {
  TemporaryDirectory directory;
  playden::Storage storage(directory.path);
  RuntimeInstallRepository repository(storage, "runtime-installs.json", Free);
  CHECK(repository.Load().ok());

  LegacyInstall request;
  request.install_id = "legacy-hl2";
  request.runtime_number = 220;
  request.install_path = "/data/homebrew/FAKE77031/games/220";
  request.volume_id = "internal";
  request.launch_path = "hl2.exe";
  request.launch_entry_id = "0";
  request.launch_arguments = "-game hl2 -steam";
  request.default_profile_id = "game";
  request.runtime_bundle_id = "prospero-win-e16ec8f";
  request.recipe_id = "steam-220-hl2-v1";
  request.recipe_version = "1";
  request.kit_title_variants = {"FAKE78003"};
  const auto adopted = repository.Adopt(request);
  CHECK(adopted.ok());
  CHECK(adopted.value().state == PreparationState::kDownloaded);
  CHECK(adopted.value().install_path == "/data/homebrew/FAKE77031/games/220");
  CHECK(!std::filesystem::exists(std::filesystem::path(directory.path) / "data" / "playden" /
                                 "games" / "legacy-hl2"));
}

void StateArtifactsFailureAndJournalRoundTrip() {
  TemporaryDirectory directory;
  playden::Storage storage(directory.path);
  RuntimeInstallRepository repository(storage, "runtime-installs.json", Free);
  CHECK(repository.Load().ok());
  auto reserved = repository.Reserve(Request("stateful"));
  CHECK(reserved.ok());
  auto record = reserved.value();
  record.state = PreparationState::kRemoving;
  record.artifacts = {
      {"runtime", "e16ec8f", "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"},
      {"helper", "e9f2cfc", "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210"}};
  record.last_failure = RuntimeFailure{"registration-busy", "native service busy"};
  record.journal = PreparationJournal{"remove-1", "unregister", "source-retired"};
  CHECK(repository.Update(record).ok());
  auto invalid_hash = record;
  invalid_hash.artifacts[0].sha256 = "short";
  CHECK(repository.Update(invalid_hash).error() == RepositoryError::kInvalidRecord);
  auto invalid_version = record;
  invalid_version.recipe_version = "../unversioned";
  CHECK(repository.Update(invalid_version).error() == RepositoryError::kInvalidRecord);
  auto reassigned = record;
  reassigned.title_id = "FAKE78999";
  CHECK(repository.Update(reassigned).error() == RepositoryError::kConflict);

  RuntimeInstallRepository restarted(storage, "runtime-installs.json", Free);
  CHECK(restarted.Load().ok());
  const auto loaded = restarted.Find("stateful");
  CHECK(loaded && loaded->state == PreparationState::kRemoving);
  CHECK(loaded->artifacts.size() == 2 && loaded->artifacts[1].identity == "e9f2cfc");
  CHECK(loaded->last_failure && loaded->last_failure->code == "registration-busy");
  CHECK(loaded->journal && loaded->journal->step == "source-retired");

  auto contender = Request("contender");
  contender.kit_title_variants = {loaded->title_id};
  CHECK(restarted.Reserve(contender).error() == RepositoryError::kTitleVariantsExhausted);
  bool source_free = false;
  RuntimeInstallRepository removal_repository(
      storage, "runtime-installs.json", [&](const std::string&) {
        return NativeTitleAvailability{TitleAvailability::kFree,
                                       source_free ? TitleAvailability::kFree
                                                   : TitleAvailability::kOccupied};
      });
  CHECK(removal_repository.Load().ok());
  CHECK(removal_repository.EraseAfterUnregister("stateful").error() ==
        RepositoryError::kRemovalIncomplete);
  CHECK(removal_repository.Find("stateful"));
  source_free = true;
  CHECK(removal_repository.EraseAfterUnregister("stateful").ok());
  CHECK(!removal_repository.Find("stateful"));
  CHECK(removal_repository.Reserve(contender).ok());
}

// With one shell for every game the candidates are the whole FAKE78001-FAKE78999 range
// (runtime::KitTitleCandidates): more than ten games each get the lowest free title, titles
// taken on the console are skipped, games kept on old fixed titles keep them, a removed game's
// title is used again, and a title reserved before a stop stays with its game after a restart.
void AnyTitleRangeAllocatesBeyondTenAndReusesRemovedTitles() {
  TemporaryDirectory directory;
  playden::Storage storage(directory.path);
  std::vector<std::string> range;
  for (int number = 1; number <= 999; ++number) {
    char title[16];
    std::snprintf(title, sizeof(title), "FAKE78%03d", number);
    range.push_back(title);
  }
  // FAKE78003 is someone else's folder; FAKE78004 is still listed by the console.
  const auto availability = [](const std::string& title) {
    if (title == "FAKE78003") return NativeTitleAvailability{TitleAvailability::kFree, TitleAvailability::kOccupied};
    if (title == "FAKE78004") return NativeTitleAvailability{TitleAvailability::kOccupied, TitleAvailability::kFree};
    return NativeTitleAvailability{TitleAvailability::kFree, TitleAvailability::kFree};
  };
  RuntimeInstallRepository repository(storage, "runtime-installs.json", availability);
  CHECK(repository.Load().ok());
  // A game prepared with an older ten-title kit keeps its fixed title.
  auto fixed = Request("old-fixed");
  fixed.kit_title_variants = {"FAKE78001"};
  CHECK(repository.Reserve(fixed).ok());

  std::vector<std::string> titles;
  for (int game = 0; game < 12; ++game) {
    auto request = Request("game-" + std::to_string(game));
    request.kit_title_variants = range;
    const auto reserved = repository.Reserve(request);
    CHECK(reserved.ok());
    titles.push_back(reserved.value().title_id);
  }
  CHECK(titles.front() == "FAKE78002" && titles[1] == "FAKE78005" && titles.back() == "FAKE78015");
  CHECK(std::set<std::string>(titles.begin(), titles.end()).size() == 12);
  CHECK(repository.Snapshot().size() == 13);

  // Removing a game frees its title for the next one, and only once it is gone.
  auto removed = *repository.Find("game-3");
  CHECK(removed.title_id == "FAKE78007");
  removed.state = PreparationState::kRemoving;
  CHECK(repository.Update(removed).ok());
  auto next = Request("game-new");
  next.kit_title_variants = range;
  auto reserved = repository.Reserve(next);
  CHECK(reserved.ok() && reserved.value().title_id == "FAKE78016");  // still reserved while removing
  CHECK(repository.EraseAfterUnregister("game-3").ok());
  auto again = Request("game-again");
  again.kit_title_variants = range;
  reserved = repository.Reserve(again);
  CHECK(reserved.ok() && reserved.value().title_id == "FAKE78007");

  // After a restart every reservation still holds, registered or not.
  RuntimeInstallRepository restarted(storage, "runtime-installs.json", availability);
  CHECK(restarted.Load().ok());
  CHECK(restarted.Find("game-again")->title_id == "FAKE78007");
  auto after = Request("after-restart");
  after.kit_title_variants = range;
  reserved = restarted.Reserve(after);
  CHECK(reserved.ok() && reserved.value().title_id == "FAKE78017");
}

void MalformedAndUnsupportedFilesDoNotBecomeEmptyRepositories() {
  TemporaryDirectory directory;
  playden::Storage storage(directory.path);
  CHECK(storage.EnsureRootDirectory().ok());
  const auto write = [&](const std::string& text) {
    return storage.WriteAtomic("runtime-installs.json",
                               playden::Bytes(text.begin(), text.end()));
  };

  CHECK(write("{broken").ok());
  RuntimeInstallRepository malformed(storage, "runtime-installs.json", Free);
  CHECK(malformed.Load().error() == RepositoryError::kMalformed);
  CHECK(malformed.Reserve(Request()).error() == RepositoryError::kNotLoaded);

  CHECK(write(R"({"schema_version":2,"reservations":[],"installs":[]})").ok());
  RuntimeInstallRepository unsupported(storage, "runtime-installs.json", Free);
  CHECK(unsupported.Load().error() == RepositoryError::kUnsupportedSchema);
  CHECK(unsupported.Reserve(Request()).error() == RepositoryError::kNotLoaded);

  CHECK(write(R"({"schema_version":1,"reservations":[],"installs":[{"schema_version":1,"install_id":"x","title_id":"FAKE78000","app_id":"220"}]})").ok());
  RuntimeInstallRepository wrong_type(storage, "runtime-installs.json", Free);
  CHECK(wrong_type.Load().error() == RepositoryError::kMalformed);

  std::string oversized(300 * 1024, 'x');
  CHECK(write(oversized).ok());
  RuntimeInstallRepository bounded(storage, "runtime-installs.json", Free);
  CHECK(bounded.Load().error() == RepositoryError::kBounds);
}

void LongLaunchArgumentsRoundTripWithinTheRepositoryBound() {
  TemporaryDirectory directory;
  playden::Storage storage(directory.path);
  RuntimeInstallRepository repository(storage, "runtime-installs.json", Free);
  CHECK(repository.Load().ok());
  auto request = Request("long-arguments");
  request.launch_arguments = std::string(4096, 'a');
  CHECK(repository.Reserve(request).ok());
  RuntimeInstallRepository restarted(storage, "runtime-installs.json", Free);
  CHECK(restarted.Load().ok());
  CHECK(restarted.Find(request.install_id)->launch_arguments == request.launch_arguments);
  request.install_id = "overlong-arguments";
  request.launch_arguments.push_back('a');
  CHECK(!restarted.Reserve(request).ok());
  CHECK(!restarted.Find(request.install_id));
}

void RecordLimitDoesNotPublishAnUnreadableRepository() {
  TemporaryDirectory directory;
  playden::Storage storage(directory.path);
  RuntimeInstallRepository repository(storage, "runtime-installs.json", Free);
  CHECK(repository.Load().ok());
  for (unsigned index = 0; index < 128; ++index) {
    auto request = Request("install-" + std::to_string(index));
    request.kit_title_variants = {"FAKE" + std::to_string(78000 + index)};
    CHECK(repository.Reserve(request).ok());
  }
  auto extra = Request("install-128");
  extra.kit_title_variants = {"FAKE78128"};
  CHECK(repository.Reserve(extra).error() == RepositoryError::kBounds);
  CHECK(repository.Reserve(Request("install-0")).ok());
  RuntimeInstallRepository restarted(storage, "runtime-installs.json", Free);
  CHECK(restarted.Load().ok());
  CHECK(restarted.Snapshot().size() == 128);
}

void DescriptorIdentifiersMatchTheNativeGrammar() {
  TemporaryDirectory directory;
  playden::Storage storage(directory.path);
  RuntimeInstallRepository repository(storage, "runtime-installs.json", Free);
  CHECK(repository.Load().ok());
  const std::vector<std::string> invalid = {
      "bad+id", "bad.id", "Bad", "_bad", "-bad", std::string(65, 'a')};
  for (const auto& value : invalid) {
    for (unsigned field = 0; field < 5; ++field) {
      auto request = Request();
      if (field == 0) request.install_id = value;
      if (field == 1) request.default_profile_id = value;
      if (field == 2) request.runtime_bundle_id = value;
      if (field == 3) request.recipe_id = value;
      if (field == 4) request.recipe_version = value;
      CHECK(repository.Reserve(request).error() == RepositoryError::kInvalidRecord);
      CHECK(repository.Snapshot().empty());
    }
  }
  auto valid = Request(std::string(64, 'a'));
  valid.default_profile_id = "0-game_profile";
  CHECK(repository.Reserve(valid).ok());
}

void LaunchPathsUseTheSharedSteamPathContract() {
  TemporaryDirectory directory;
  playden::Storage storage(directory.path);
  RuntimeInstallRepository repository(storage, "runtime-installs.json", Free);
  CHECK(repository.Load().ok());
  auto request = Request();
  request.launch_path = ".\\bin//./Game.exe";
  request.launch_working_directory = ".\\bin//.";
  auto reserved = repository.Reserve(request);
  CHECK(reserved.ok());
  CHECK(reserved.value().launch_path == "bin/Game.exe");
  CHECK(reserved.value().launch_working_directory == "bin");
  auto changed = reserved.value();
  changed.launch_working_directory = "../../outside";
  CHECK(repository.Update(changed).error() == RepositoryError::kInvalidRecord);
  for (const auto& path : {"../../outside", "C:\\outside", "/outside", ".gn-download/x"}) {
    request.install_id = "invalid-path";
    request.launch_working_directory = path;
    CHECK(repository.Reserve(request).error() == RepositoryError::kInvalidRecord);
  }
  // Starting in the install root is kept across a restart; it only holds without a working folder.
  request.install_id = "install-root";
  request.launch_working_directory = ".";
  request.launch_in_install_root = true;
  CHECK(repository.Reserve(request).ok());
  request.install_id = "install-root-folder";
  request.launch_working_directory = "bin";
  auto with_folder = repository.Reserve(request);
  CHECK(with_folder.ok() && !with_folder.value().launch_in_install_root);
  RuntimeInstallRepository restarted(storage, "runtime-installs.json", Free);
  CHECK(restarted.Load().ok());
  CHECK(restarted.Find("install-hl2")->launch_path == "bin/Game.exe");
  CHECK(!restarted.Find("install-hl2")->launch_in_install_root);
  CHECK(restarted.Find("install-root")->launch_in_install_root &&
        restarted.Find("install-root")->launch_working_directory.empty());
}

void JournalSyncFailureRequiresReloadBeforeFurtherEffects() {
  for (const auto fault : {SyncFault::kFile, SyncFault::kDirectory}) {
    TemporaryDirectory directory;
    playden::Storage storage(directory.path);
    RuntimeInstallRepository repository(storage, "runtime-installs.json", Free);
    CHECK(repository.Load().ok());
    sync_fault = fault;
    CHECK(repository.Reserve(Request()).error() == RepositoryError::kIo);
    CHECK(!repository.Find("install-hl2"));
    CHECK(repository.Save().error() == RepositoryError::kNotLoaded);
    sync_fault = SyncFault::kNone;
    CHECK(repository.Load().ok());
    // Rename may already have published the new record before directory sync.
    CHECK(bool(repository.Find("install-hl2")) == (fault == SyncFault::kDirectory));
    CHECK(repository.Reserve(Request()).ok());
    struct stat info{};
    CHECK(lstat((directory.path + "/runtime-installs.json").c_str(), &info) == 0);
    CHECK(S_ISREG(info.st_mode) && info.st_uid == geteuid() && (info.st_mode & 07777) == 0600);
  }
}

void JournalUpdateSyncFailureReconcilesPublishedState() {
  for (const auto fault : {SyncFault::kFile, SyncFault::kDirectory}) {
    TemporaryDirectory directory;
    playden::Storage storage(directory.path);
    RuntimeInstallRepository repository(storage, "runtime-installs.json", Free);
    CHECK(repository.Load().ok());
    auto reserved = repository.Reserve(Request());
    CHECK(reserved.ok());
    auto changed = reserved.value();
    ++changed.settings_revision;
    sync_fault = fault;
    CHECK(repository.Update(changed).error() == RepositoryError::kIo);
    CHECK(repository.Load().error() == RepositoryError::kIo);
    CHECK(repository.Save().error() == RepositoryError::kNotLoaded);
    sync_fault = SyncFault::kNone;
    CHECK(repository.Load().ok());
    CHECK(repository.Find(changed.install_id)->settings_revision ==
          (fault == SyncFault::kDirectory ? changed.settings_revision : reserved.value().settings_revision));
  }
}

void RegistryPublicationRejectsSymlinkDestination() {
  TemporaryDirectory directory;
  playden::Storage storage(directory.path);
  RuntimeInstallRepository repository(storage, "runtime-installs.json", Free);
  CHECK(repository.Load().ok());
  std::ofstream(directory.path + "/outside") << "sentinel";
  CHECK(symlink("outside", (directory.path + "/runtime-installs.json").c_str()) == 0);
  CHECK(repository.Reserve(Request()).error() == RepositoryError::kIo);
  CHECK(repository.Save().error() == RepositoryError::kNotLoaded);
  std::ifstream sentinel(directory.path + "/outside");
  std::string text; sentinel >> text;
  CHECK(text == "sentinel");
  CHECK(symlink(directory.path.c_str(), (directory.path + "/alias").c_str()) == 0);
  playden::Storage alias(directory.path + "/alias");
  RuntimeInstallRepository linked_root(alias, "runtime-installs.json", Free);
  CHECK(linked_root.Load().error() == RepositoryError::kIo);
}

void RegistryRejectsSymlinksAndDoesNotTouchOldTemporaryFiles() {
  TemporaryDirectory directory;
  playden::Storage storage(directory.path);
  RuntimeInstallRepository repository(storage, "runtime-installs.json", Free);
  CHECK(repository.Load().ok());
  std::ofstream(directory.path + "/outside") << "sentinel";
  CHECK(symlink("outside", (directory.path + "/runtime-installs.json.tmp").c_str()) == 0);
  CHECK(repository.Reserve(Request()).ok());
  std::ifstream sentinel(directory.path + "/outside");
  std::string text; sentinel >> text;
  CHECK(text == "sentinel");
  CHECK(unlink((directory.path + "/runtime-installs.json").c_str()) == 0);
  CHECK(symlink("outside", (directory.path + "/runtime-installs.json").c_str()) == 0);
  CHECK(repository.Load().error() == RepositoryError::kIo);
}

}  // namespace

// The 0.6 games move: records under the old root are rewritten together, keep the old path as
// their mark (written only when set), and an invalid one leaves every record as it was.
void MovedRecordsRewriteTogetherAndKeepTheirMark() {
  CHECK(RebaseInstallPath("/data/homebrew/FAKE77031/games/220", "/data/homebrew/FAKE77031/games", "/data/playden-games") ==
        std::optional<std::string>("/data/playden-games/220"));
  CHECK(RebaseInstallPath("/data/homebrew/FAKE77031/games", "/data/homebrew/FAKE77031/games/", "/data/playden-games") ==
        std::optional<std::string>("/data/playden-games"));
  CHECK(!RebaseInstallPath("/data/homebrew/FAKE77031/games-old/1", "/data/homebrew/FAKE77031/games", "/data/playden-games"));
  CHECK(!RebaseInstallPath("/mnt/usb1/games/1", "/data/homebrew/FAKE77031/games", "/data/playden-games"));

  TemporaryDirectory directory;
  playden::Storage storage(directory.path);
  CHECK(storage.EnsureRootDirectory().ok());
  RuntimeInstallRepository repository(storage, "installs.json", Free);
  CHECK(repository.Load().ok());
  auto first = Request("install-a"); first.install_path = "/data/homebrew/FAKE77031/games/220";
  auto second = Request("install-b"); second.runtime_number = 400; second.install_path = "/data/homebrew/FAKE77031/games/400";
  CHECK(repository.Reserve(first).ok() && repository.Reserve(second).ok());
  const auto unmarked = storage.ReadDurable("installs.json", 1 << 20);
  CHECK(unmarked.ok() && std::string(unmarked.value().begin(), unmarked.value().end()).find("moved_from") == std::string::npos);

  auto a = *repository.Find("install-a"), b = *repository.Find("install-b");
  a.moved_from = a.install_path; a.install_path = "/data/playden-games/220";
  b.moved_from = b.install_path; b.install_path = "/data/playden-games/400";
  auto invalid = b; invalid.moved_from = invalid.install_path;  // the mark must name another place
  CHECK(!repository.UpdateAll({a, invalid}).ok());
  CHECK(repository.Find("install-a")->install_path == "/data/homebrew/FAKE77031/games/220");
  auto renamed = b; renamed.data_root = "/data/playden/games/other";
  CHECK(!repository.UpdateAll({a, renamed}).ok());
  CHECK(repository.UpdateAll({a, b}).ok());

  RuntimeInstallRepository restarted(storage, "installs.json", Free);
  CHECK(restarted.Load().ok());
  CHECK(restarted.Find("install-a")->install_path == "/data/playden-games/220" &&
        restarted.Find("install-a")->moved_from == "/data/homebrew/FAKE77031/games/220");
  CHECK(restarted.Find("install-b")->moved_from == "/data/homebrew/FAKE77031/games/400");
  auto cleared = *restarted.Find("install-a"); cleared.moved_from.clear();
  CHECK(restarted.Update(cleared).ok());
  RuntimeInstallRepository again(storage, "installs.json", Free);
  CHECK(again.Load().ok() && again.Find("install-a")->moved_from.empty() && !again.Find("install-b")->moved_from.empty());

  // A program's run (Run launcher or setup) is kept the same way, only while set, and only as a path in the game folder.
  auto setup = *again.Find("install-a"); setup.setup_program = "Binaries/BmLauncher.exe";
  auto outside = setup; outside.setup_program = "../BmLauncher.exe";
  CHECK(!again.Update(outside).ok());
  CHECK(again.Update(setup).ok());
  RuntimeInstallRepository reloaded(storage, "installs.json", Free);
  CHECK(reloaded.Load().ok() && reloaded.Find("install-a")->setup_program == "Binaries/BmLauncher.exe" &&
        reloaded.Find("install-b")->setup_program.empty());
  setup.setup_program.clear();
  CHECK(reloaded.Update(setup).ok());
  const auto unset = storage.ReadDurable("installs.json", 1 << 20);
  CHECK(unset.ok() && std::string(unset.value().begin(), unset.value().end()).find("setup_program") == std::string::npos);
}

std::string ReadText(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

void WriteText(const std::string& path, const std::string& text) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << text;
}

// What the code before runtime numbers wrote for these two Steam installs (captured from it).
constexpr const char* kSteamRegistryBefore =
    R"({"schema_version":1,"reservations":[{"install_id":"steam-220","title_id":"FAKE78000"},{"install_id":"steam-440","title_id":"FAKE78001"}],"installs":[{"schema_version":1,"install_id":"steam-220","title_id":"FAKE78000","app_id":220,"data_root":"/data/playden/games/steam-220","default_profile_id":"game","runtime_bundle_id":"prospero-win-e16ec8f","recipe_id":"steam-220-hl2-v1","recipe_version":"1","settings_revision":7,"exit_policy":"exit-title","launch_entry_id":"0","launch_path":"hl2.exe","launch_arguments":"-game hl2","launch_working_directory":"","install_path":"/mnt/ext1/playden/games/220","volume_id":"m2","state":"downloaded","artifacts":[],"last_failure":null,"journal":null},{"schema_version":1,"install_id":"steam-440","title_id":"FAKE78001","app_id":440,"data_root":"/data/playden/games/steam-440","default_profile_id":"game","runtime_bundle_id":"prospero-win-e16ec8f","recipe_id":"steam-440-auto","recipe_version":"1","settings_revision":7,"exit_policy":"exit-title","launch_entry_id":"0","launch_path":"bin/tf.exe","launch_arguments":"-game hl2","launch_working_directory":"","launch_in_install_root":true,"install_path":"/data/playden-games/440","volume_id":"internal","state":"failed","artifacts":[{"artifact_id":"wine","identity":"wine-1","sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}],"last_failure":{"code":"copy-failed","message":"No \"space\""},"journal":{"operation_id":"op-1","operation":"prepare","step":"copy"},"moved_from":"/mnt/ext1/playden/games/440"}]})";

ReserveRequest SteamRequest(std::uint32_t app, std::string install_path) {
  ReserveRequest request;
  request.install_id = "steam-" + std::to_string(app);
  request.runtime_number = app;  // no game: Steam, as on disk
  request.default_profile_id = "game";
  request.runtime_bundle_id = "prospero-win-e16ec8f";
  request.recipe_id = "steam-220-hl2-v1";
  request.recipe_version = "1";
  request.settings_revision = 7;
  request.launch_entry_id = "0";
  request.launch_path = "hl2.exe";
  request.launch_arguments = "-game hl2";
  request.install_path = std::move(install_path);
  request.volume_id = "m2";
  request.kit_title_variants = {"FAKE78000", "FAKE78001", "FAKE78002", "FAKE78003"};
  return request;
}

const auto kLocalA = playden::GameId::Local("0f8fad5b-d9cb-469f-a165-70867728950e");
const auto kLocalB = playden::GameId::Local("7c9e6679-7425-40de-944b-e07fc1f90ae7");

ReserveRequest LocalRequest(const playden::GameId& game, std::uint32_t number) {
  auto request = SteamRequest(220, "/data/playden-games/" + playden::ToInstallId(game));
  request.install_id = playden::ToInstallId(game);
  request.runtime_number = number;
  request.game = game;
  request.recipe_id = playden::GeneratedRecipeId(game);
  return request;
}

void SteamRegistryBytesAreUnchanged() {
  TemporaryDirectory directory;
  playden::Storage storage(directory.path);
  CHECK(storage.EnsureRootDirectory().ok());
  RuntimeInstallRepository repository(storage, "installs.json", Free);
  CHECK(repository.Load().ok());
  CHECK(repository.Reserve(SteamRequest(220, "/mnt/ext1/playden/games/220")).ok());
  auto tf2 = SteamRequest(440, "/data/playden-games/440");
  tf2.recipe_id = "steam-440-auto"; tf2.launch_path = "bin/tf.exe"; tf2.launch_in_install_root = true;
  tf2.volume_id = "internal";
  auto reserved = repository.Reserve(tf2);
  CHECK(reserved.ok() && reserved.value().game == playden::GameId::Steam(440));
  auto record = reserved.value();
  record.state = PreparationState::kFailed;
  record.last_failure = RuntimeFailure{"copy-failed", "No \"space\""};
  record.journal = PreparationJournal{"op-1", "prepare", "copy"};
  record.moved_from = "/mnt/ext1/playden/games/440";
  record.artifacts.push_back({"wine", "wine-1", std::string(64, 'a')});
  CHECK(repository.Update(record).ok());
  // A Steam game's number is its app ID and allocates nothing.
  const auto steam = repository.RuntimeNumberFor(playden::GameId::Steam(570));
  CHECK(steam.ok() && steam.value() == 570);
  const auto path = directory.path + "/installs.json";
  CHECK(ReadText(path) == kSteamRegistryBefore);

  // The old file reads as Steam and saves byte for byte.
  RuntimeInstallRepository restarted(storage, "installs.json", Free);
  CHECK(restarted.Load().ok());
  CHECK(restarted.Find("steam-440")->game == playden::GameId::Steam(440));
  CHECK(restarted.Find("steam-440")->runtime_number == 440);
  CHECK(restarted.Save().ok());
  CHECK(ReadText(path) == kSteamRegistryBefore);
}

void LocalGamesGetNumbersFromTheReservedRange() {
  TemporaryDirectory directory;
  playden::Storage storage(directory.path);
  CHECK(storage.EnsureRootDirectory().ok());
  RuntimeInstallRepository repository(storage, "installs.json", Free);
  CHECK(!repository.RuntimeNumberFor(kLocalA).ok());  // not loaded
  CHECK(repository.Load().ok());
  CHECK(repository.Reserve(SteamRequest(220, "/mnt/ext1/playden/games/220")).ok());
  const auto path = directory.path + "/installs.json";
  const auto steam_only = ReadText(path);

  const auto first = repository.RuntimeNumberFor(kLocalA);
  CHECK(first.ok() && first.value() == 0xF0000000u);
  // Saved before it is handed out: only the allocator's key is new.
  const auto allocated = ReadText(path);
  CHECK(allocated == steam_only.substr(0, steam_only.size() - 1) + ",\"next_runtime_number\":4026531841}");
  // The same game keeps its number, also before it has a record; another game gets the next one.
  CHECK(repository.RuntimeNumberFor(kLocalA).value() == 0xF0000000u);
  CHECK(ReadText(path) == allocated);
  const auto second = repository.RuntimeNumberFor(kLocalB);
  CHECK(second.ok() && second.value() == 0xF0000001u);
  CHECK(!repository.RuntimeNumberFor(playden::GameId{}).ok());
  CHECK(!repository.RuntimeNumberFor(playden::GameId{playden::Store::kLocal, "not-a-uuid"}).ok());

  // A record keeps store and game; a number held by another game is refused.
  // Given to kLocalA (no record yet): kLocalB can't take it.
  CHECK(repository.Reserve(LocalRequest(kLocalB, 0xF0000000u)).error() == RepositoryError::kConflict);
  auto reserved = repository.Reserve(LocalRequest(kLocalA, 0xF0000000u));
  CHECK(reserved.ok() && reserved.value().game == kLocalA && reserved.value().runtime_number == 0xF0000000u);
  CHECK(reserved.value().install_id == "local-0f8fad5b-d9cb-469f-a165-70867728950e");
  auto stolen = LocalRequest(kLocalB, 0xF0000000u);
  CHECK(repository.Reserve(stolen).error() == RepositoryError::kConflict);
  auto steam_number = LocalRequest(kLocalB, 220);
  CHECK(repository.Reserve(steam_number).error() == RepositoryError::kConflict);
  auto out_of_range = LocalRequest(kLocalB, 4000);
  CHECK(repository.Reserve(out_of_range).error() == RepositoryError::kInvalidRecord);
  const auto text = ReadText(path);
  CHECK(text.find("\"app_id\":4026531840,\"store\":\"local\",\"game\":\"0f8fad5b-d9cb-469f-a165-70867728950e\",") !=
        std::string::npos);
  CHECK(text.find("\"next_runtime_number\":4026531842}") != std::string::npos);
  // The Steam record's bytes are as before.
  const auto steam_record_end = steam_only.find("]}");
  const auto steam_record = steam_only.substr(steam_only.find("\"installs\":[") + 12,
                                              steam_record_end - steam_only.find("\"installs\":[") - 12);
  CHECK(text.find(steam_record) != std::string::npos);
  // Identity is immutable.
  auto changed = reserved.value(); changed.game = kLocalB;
  CHECK(repository.Update(changed).error() == RepositoryError::kConflict);
  auto unnamed = reserved.value(); unnamed.game = {};  // as read back from a journal: keeps its game
  unnamed.settings_revision = 9;
  CHECK(repository.Update(unnamed).ok() && repository.Find(reserved.value().install_id)->game == kLocalA);

  RuntimeInstallRepository restarted(storage, "installs.json", Free);
  CHECK(restarted.Load().ok());
  const auto found = restarted.Find(reserved.value().install_id);
  CHECK(found && found->game == kLocalA && found->runtime_number == 0xF0000000u);
  CHECK(restarted.RuntimeNumberFor(kLocalA).value() == 0xF0000000u);  // from its record
  // kLocalB's number was handed out but never recorded: it is not handed out again.
  const auto third = restarted.RuntimeNumberFor(kLocalB);
  CHECK(third.ok() && third.value() == 0xF0000002u);
  CHECK(restarted.Save().ok());
  CHECK(ReadText(path).find("\"next_runtime_number\":4026531843}") != std::string::npos);

  // Uninstalled in this session, the game gets its number back when installed again.
  auto removing = *restarted.Find(reserved.value().install_id);
  removing.state = PreparationState::kRemoving;
  CHECK(restarted.Update(removing).ok());
  CHECK(restarted.EraseAfterUnregister(removing.install_id).ok());
  CHECK(!restarted.Find(removing.install_id));
  CHECK(restarted.RuntimeNumberFor(kLocalA).value() == 0xF0000000u);
  CHECK(restarted.Reserve(LocalRequest(kLocalA, 0xF0000000u)).ok());
}

void RuntimeNumbersRefuseOverflowAndDuplicates() {
  TemporaryDirectory directory;
  playden::Storage storage(directory.path);
  CHECK(storage.EnsureRootDirectory().ok());
  const auto path = directory.path + "/installs.json";
  const std::string empty = R"({"schema_version":1,"reservations":[],"installs":[])";
  {
    WriteText(path, empty + R"(,"next_runtime_number":4294967294})");
    RuntimeInstallRepository repository(storage, "installs.json", Free);
    CHECK(repository.Load().ok());
    const auto last = repository.RuntimeNumberFor(kLocalA);
    CHECK(last.ok() && last.value() == 0xFFFFFFFEu);
    const auto none = repository.RuntimeNumberFor(kLocalB);
    CHECK(!none.ok() && none.error() == RepositoryError::kBounds);
    CHECK(ReadText(path) == empty + R"(,"next_runtime_number":4294967295})");
    RuntimeInstallRepository restarted(storage, "installs.json", Free);
    CHECK(restarted.Load().ok() && restarted.RuntimeNumberFor(kLocalB).error() == RepositoryError::kBounds);
  }
  for (const char* bad : {R"(,"next_runtime_number":4294967296})", R"(,"next_runtime_number":5})",
                          R"(,"next_runtime_number":"x"})"}) {
    WriteText(path, empty + bad);
    RuntimeInstallRepository repository(storage, "installs.json", Free);
    CHECK(repository.Load().error() == RepositoryError::kMalformed);
  }
  // A file that has the key keeps it, even with only Steam games.
  WriteText(path, empty + R"(,"next_runtime_number":4026531840})");
  {
    RuntimeInstallRepository repository(storage, "installs.json", Free);
    CHECK(repository.Load().ok() && repository.Save().ok());
    CHECK(ReadText(path) == empty + R"(,"next_runtime_number":4026531840})");
  }

  // Two games with one number: the registry is refused.
  WriteText(path, empty + "}");
  {
    RuntimeInstallRepository repository(storage, "installs.json", Free);
    CHECK(repository.Load().ok());
    CHECK(repository.RuntimeNumberFor(kLocalA).value() == 0xF0000000u);
    CHECK(repository.RuntimeNumberFor(kLocalB).value() == 0xF0000001u);
    CHECK(repository.Reserve(LocalRequest(kLocalA, 0xF0000000u)).ok());
    CHECK(repository.Reserve(LocalRequest(kLocalB, 0xF0000001u)).ok());
  }
  const auto valid = ReadText(path);
  {
    RuntimeInstallRepository repository(storage, "installs.json", Free);
    CHECK(repository.Load().ok());
  }
  auto duplicate = valid;
  const std::string second = "\"app_id\":4026531841,";
  CHECK(duplicate.find(second) != std::string::npos);
  duplicate.replace(duplicate.find(second), second.size(), "\"app_id\":4026531840,");
  WriteText(path, duplicate);
  {
    RuntimeInstallRepository repository(storage, "installs.json", Free);
    CHECK(repository.Load().error() == RepositoryError::kConflict);
  }
  // A record past the allocator's next moves it on; Steam numbers and unknown stores are checked.
  auto behind = valid;
  behind.replace(behind.find("\"next_runtime_number\":4026531842"), 32, "\"next_runtime_number\":4026531840");
  WriteText(path, behind);
  {
    RuntimeInstallRepository repository(storage, "installs.json", Free);
    CHECK(repository.Load().ok());
    CHECK(repository.RuntimeNumberFor(playden::GameId::Local("9a1d1f2e-0000-4000-8000-000000000001")).value() ==
          0xF0000002u);
  }
  for (const auto& [from, to] : std::vector<std::pair<std::string, std::string>>{
           {"\"store\":\"local\"", "\"store\":\"steam\""},
           {"\"store\":\"local\"", "\"store\":\"gog\""},
           {"\"game\":\"0f8fad5b", "\"game\":\"0F8FAD5B"},
           {"\"app_id\":4026531840,", "\"app_id\":4000,"},
           {",\"store\":\"local\"", ""}}) {
    auto edited = valid;
    CHECK(edited.find(from) != std::string::npos);
    edited.replace(edited.find(from), from.size(), to);
    WriteText(path, edited);
    RuntimeInstallRepository repository(storage, "installs.json", Free);
    CHECK(!repository.Load().ok());
  }
}

int main() {
  SteamRegistryBytesAreUnchanged();
  LocalGamesGetNumbersFromTheReservedRange();
  RuntimeNumbersRefuseOverflowAndDuplicates();
  JournalSyncFailureRequiresReloadBeforeFurtherEffects();
  JournalUpdateSyncFailureReconcilesPublishedState();
  RegistryRejectsSymlinksAndDoesNotTouchOldTemporaryFiles();
  RegistryPublicationRejectsSymlinkDestination();
  ReservationSurvivesRestartAndReinstallReusesIdentity();
  NativeInventoryAndSourceCollisionsAreNeverAllocated();
  AnyTitleRangeAllocatesBeyondTenAndReusesRemovedTitles();
  LegacyDownloadAdoptsWithoutCopyAndRemainsDownloaded();
  StateArtifactsFailureAndJournalRoundTrip();
  MalformedAndUnsupportedFilesDoNotBecomeEmptyRepositories();
  LongLaunchArgumentsRoundTripWithinTheRepositoryBound();
  DescriptorIdentifiersMatchTheNativeGrammar();
  RecordLimitDoesNotPublishAnUnreadableRepository();
  LaunchPathsUseTheSharedSteamPathContract();
  MovedRecordsRewriteTogetherAndKeepTheirMark();
  std::puts("runtime_game_install_test: all assertions passed");
  return EXIT_SUCCESS;
}
