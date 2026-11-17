#include "library/organization.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include "steam_id.h"

namespace {

int failures = 0;
void Check(bool value, const char* message) {
  if (!value) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
  }
}

class TemporaryDirectory {
 public:
  TemporaryDirectory() {
    char value[] = "/tmp/playden-organization-XXXXXX";
    if (const char* path = mkdtemp(value)) path_ = path;
  }
  ~TemporaryDirectory() {
    if (!path_.empty()) { std::error_code error; std::filesystem::remove_all(path_, error); }
  }
  const std::string& path() const { return path_; }

 private:
  std::string path_;
};

constexpr std::uint64_t kFirstAccount = 76561198000000001ULL;
constexpr std::uint64_t kSecondAccount = 76561198000000002ULL;

constexpr std::uint64_t kDevice = playden::kDeviceOrganization;
const std::string kV2Path = "organization.v2";

std::string Path(std::uint64_t account) {
  return "organization-" + std::to_string(account) + ".v1";
}

void RewriteChecksum(playden::Bytes& bytes) {
  std::uint32_t checksum = 2166136261u;
  for (std::size_t index = 0; index + 4 < bytes.size(); ++index) {
    checksum ^= bytes[index]; checksum *= 16777619u;
  }
  for (unsigned shift = 0; shift < 32; shift += 8)
    bytes[bytes.size() - 4 + shift / 8] = static_cast<std::uint8_t>(checksum >> shift);
}

bool ReplaceBytes(playden::Bytes& bytes, const std::string& from, const std::string& to) {
  if (from.size() != to.size()) return false;
  const auto found = std::search(bytes.begin(), bytes.end(), from.begin(), from.end());
  if (found == bytes.end()) return false;
  std::copy(to.begin(), to.end(), found);
  RewriteChecksum(bytes);
  return true;
}

playden::OrganizationSnapshot Example() {
  playden::OrganizationSnapshot result;
  result.favorites = {SteamId(10), SteamId(20)};
  result.hidden = {SteamId(30)};
  result.collections = {
      {"collection-1", "旅のゲーム", true, {SteamId(10), SteamId(30)}},
      {"collection-2", "Co-op café", false, {SteamId(20)}},
  };
  result.ratings = {
      {SteamId(10), playden::CompatibilityRating::kWorks},
      {SteamId(20), playden::CompatibilityRating::kPlayable},
      {SteamId(30), playden::CompatibilityRating::kBroken},
  };
  result.notes = {{SteamId(10), "再訪したい"}, {SteamId(20), "Français — bientôt"}};
  result.query = "星 café";
  result.scope = "collection-1";
  result.sort = "recent-played";
  result.filters = {"compat-playable"};
  result.selected_game_id = SteamId(20);
  return result;
}

void V1RoundTripAndIsolation() {
  TemporaryDirectory directory;
  playden::Storage storage(directory.path());
  playden::OrganizationStore store(storage);
  auto expected = Example();
  expected.schema_version = 1;
  Check(store.SaveV1(kFirstAccount, expected).ok(), "organization snapshot saves");
  const auto restored = store.LoadV1(kFirstAccount);
  Check(restored.ok() && restored.value() && *restored.value() == expected,
        "Unicode organization snapshot round trips without loss");
  const auto other = store.LoadV1(kSecondAccount);
  Check(other.ok() && !other.value(), "another account sees a distinct missing snapshot");
  const auto first_bytes = storage.Read(Path(kFirstAccount));
  Check(first_bytes.ok() && storage.WriteAtomicPrivate(Path(kSecondAccount), first_bytes.value()).ok(),
        "copied cross-account fixture writes");
  const auto copied = store.LoadV1(kSecondAccount);
  Check(!copied.ok() && copied.error() == playden::StorageError::kInvalidData,
        "embedded account identity rejects a snapshot copied across accounts");
  Check(!store.SaveV1(0, expected).ok() && !store.LoadV1(0).ok() && !store.Clear(0).ok(),
        "zero account identity is rejected");

  struct stat info {};
  Check(stat((directory.path() + "/" + Path(kFirstAccount)).c_str(), &info) == 0,
        "organization file exists");
  Check((info.st_mode & 0777) == 0666, "organization file is open to FTP (#868)");
}

void RejectsInvalidValuesWithoutReplacingGoodData() {
  TemporaryDirectory directory;
  playden::Storage storage(directory.path());
  playden::OrganizationStore store(storage);
  const auto expected = Example();
  Check(store.Save(kDevice, expected).ok(), "baseline organization saves");
  auto rejects = [&](playden::OrganizationSnapshot invalid, const char* message) {
    Check(!store.Save(kDevice, invalid).ok(), message);
    const auto retained = store.Load(kDevice);
    Check(retained.ok() && retained.value() && *retained.value() == expected,
          "rejected update preserves previous organization file");
  };

  auto invalid = expected; invalid.schema_version = 3; rejects(invalid, "future schema rejected");
  invalid = expected; invalid.favorites.insert(SteamId(0)); rejects(invalid, "zero favorite rejected");
  invalid = expected; invalid.collections.push_back(invalid.collections.front()); rejects(invalid, "duplicate collection ID rejected");
  invalid = expected; invalid.collections.front().id = "../escape"; rejects(invalid, "unsafe collection ID rejected");
  invalid = expected; invalid.collections.front().id = "installed"; rejects(invalid, "simulation rail ID cannot be a collection ID");
  invalid = expected; invalid.collections.front().id = "imported"; rejects(invalid, "Imported rail ID cannot be a collection ID");
  invalid = expected; invalid.collections.front().id = "steam"; rejects(invalid, "Steam rail ID cannot be a collection ID");
  invalid = expected; invalid.collections.front().id = "new-collection"; rejects(invalid, "collection action ID cannot be a collection ID");
  invalid = expected; invalid.collections.front().name.assign(129, 'n'); rejects(invalid, "oversized collection name rejected");
  invalid = expected; invalid.collections.front().name = "   \t"; rejects(invalid, "whitespace-only collection name rejected");
  invalid = expected; invalid.collections.front().name = " Leading"; rejects(invalid, "leading collection whitespace rejected instead of normalized");
  invalid = expected; invalid.collections.front().name = "Trailing "; rejects(invalid, "trailing collection whitespace rejected instead of normalized");
  invalid = expected; invalid.collections[1].name = invalid.collections[0].name; rejects(invalid, "duplicate collection name rejected");
  invalid = expected; invalid.collections.front().members.insert(SteamId(0)); rejects(invalid, "zero collection member rejected");
  invalid = expected; invalid.ratings[SteamId(10)] = static_cast<playden::CompatibilityRating>(4); rejects(invalid, "unknown compatibility rejected");
  invalid = expected; invalid.notes[SteamId(10)].assign(4097, 'n'); rejects(invalid, "oversized note rejected");
  invalid = expected; invalid.query.assign(257, 'q'); rejects(invalid, "oversized query rejected");
  invalid = expected; invalid.sort = "invented"; rejects(invalid, "unknown sort rejected");
  invalid = expected; invalid.filters.insert("fixture-only"); rejects(invalid, "unknown filter rejected");
  {
    // The Library's Installed rail is a live scope, so choosing it is saved and restored.
    auto installed = expected; installed.scope = "installed";
    Check(store.Save(kDevice, installed).ok(), "installed scope accepted");
    const auto restored = store.Load(kDevice);
    Check(restored.ok() && restored.value() && restored.value()->scope == "installed", "installed scope restored");
    Check(store.Save(kDevice, expected).ok(), "restore the expected organization file after installed scope");
  }
  {
    // So is the Imported rail: choosing it was undone as "too large to save".
    auto imported = expected; imported.scope = "imported";
    Check(store.Save(kDevice, imported).ok(), "imported scope accepted");
    const auto restored = store.Load(kDevice);
    Check(restored.ok() && restored.value() && restored.value()->scope == "imported", "imported scope restored");
    Check(store.Save(kDevice, expected).ok(), "restore the expected organization file after imported scope");
  }
  {
    // And the Steam rail of the store section.
    auto steam = expected; steam.scope = "steam";
    Check(store.Save(kDevice, steam).ok(), "steam scope accepted");
    const auto restored = store.Load(kDevice);
    Check(restored.ok() && restored.value() && *restored.value() == steam, "steam scope restored");
    Check(store.Save(kDevice, expected).ok(), "restore the expected organization file after steam scope");
  }
  for (const char* scope : {"steam-collection:uc-1a2B3c4D5e6F", "gog-tag:1234567", "steam-collection:a.b_c-d",
                            "steam-collection:uc-AbC*+dEf==", "steam-collection:srm-Xy/z"}) {
    // A store's own collection (#536): saved and restored, though the store gives the collections only later.
    auto chosen = expected; chosen.scope = scope;
    Check(store.Save(kDevice, chosen).ok(), (std::string("store collection scope accepted: ") + scope).c_str());
    const auto restored = store.Load(kDevice);
    Check(restored.ok() && restored.value() && *restored.value() == chosen,
          (std::string("store collection scope restored: ") + scope).c_str());
    Check(store.Save(kDevice, expected).ok(), "restore the expected organization file after a store collection scope");
  }
  invalid = expected; invalid.scope = "steam-collection:"; rejects(invalid, "a store collection scope without an ID rejected");
  invalid = expected; invalid.scope = "steam-collection:a b"; rejects(invalid, "a store collection ID with a space rejected");
  invalid = expected; invalid.scope = "gog-tag:a:b"; rejects(invalid, "a store collection ID with ':' rejected");
  invalid = expected; invalid.scope = "steam-collection:caf\xC3\xA9"; rejects(invalid, "a non-ASCII store collection ID rejected");
  invalid = expected; invalid.scope = "epic-collection:1"; rejects(invalid, "another store's collection rejected");
  invalid = expected; invalid.scope = "steam-collection:" + std::string(65, 'a');
  rejects(invalid, "a store collection ID over 64 bytes rejected");
  {
    auto longest = expected; longest.scope = "steam-collection:" + std::string(64, 'a');
    Check(store.Save(kDevice, longest).ok(), "a 64-byte store collection ID accepted");
    Check(store.Save(kDevice, expected).ok(), "restore the expected organization file after the longest ID");
  }
  invalid = expected; invalid.scope = "new-collection"; rejects(invalid, "collection action cannot be restored as scope");
  invalid = expected; invalid.sort = "install-size"; rejects(invalid, "simulation-only metadata sort rejected");
  {
    // Acquisition dates come from the Steam CM, so the recently-added sort is a live fact.
    auto dated = expected; dated.sort = "recent-added";
    Check(store.Save(kDevice, dated).ok(), "recently-added sort accepted");
    const auto restored = store.Load(kDevice);
    Check(restored.ok() && restored.value() && restored.value()->sort == "recent-added",
          "recently-added sort restored");
    Check(store.Save(kDevice, expected).ok(), "restore the expected organization file");
  }
  invalid = expected; invalid.filters = {"controller-full"}; rejects(invalid, "simulation-only controller filter rejected");
  invalid = expected; invalid.scope = "missing-collection"; rejects(invalid, "unknown scope rejected");
  invalid = expected; invalid.notes[SteamId(40)] = std::string("bad-") + static_cast<char>(0xc0);
  rejects(invalid, "invalid UTF-8 note rejected");

  invalid = expected;
  invalid.favorites.clear();
  for (std::uint32_t id = 1; id <= 10001; ++id) invalid.favorites.insert(SteamId(id));
  rejects(invalid, "more than ten thousand organization game IDs rejected");

  auto changed = expected;
  changed.query = "replacement";
  const std::string temporary_path = directory.path() + "/" + kV2Path + ".tmp";
  Check(mkdir(temporary_path.c_str(), 0700) == 0, "write-failure blocker creates");
  Check(!store.Save(kDevice, changed).ok(), "atomic private write failure propagates");
  const auto retained = store.Load(kDevice);
  Check(retained.ok() && retained.value() && *retained.value() == expected,
        "atomic write failure preserves the previous organization file");
  rmdir(temporary_path.c_str());
}

void V1RejectsDamagedAndOversizedFiles() {
  TemporaryDirectory directory;
  playden::Storage storage(directory.path());
  playden::OrganizationStore store(storage);
  Check(store.SaveV1(kFirstAccount, Example()).ok(), "corruption fixture saves");
  const auto stored = storage.Read(Path(kFirstAccount));
  Check(stored.ok(), "corruption fixture reads");
  if (!stored.ok()) return;

  auto corrupt = stored.value();
  corrupt[corrupt.size() / 2] ^= 0x5a;
  Check(storage.WriteAtomicPrivate(Path(kFirstAccount), corrupt).ok(), "corrupt file writes");
  auto loaded = store.LoadV1(kFirstAccount);
  Check(!loaded.ok() && loaded.error() == playden::StorageError::kInvalidData,
        "checksum corruption is distinguished from missing data");

  auto truncated = stored.value();
  truncated.resize(9);
  Check(storage.WriteAtomicPrivate(Path(kFirstAccount), truncated).ok(), "truncated file writes");
  loaded = store.LoadV1(kFirstAccount);
  Check(!loaded.ok() && loaded.error() == playden::StorageError::kInvalidData,
        "truncated organization is rejected");

  auto future = stored.value();
  future[8] = 2;
  RewriteChecksum(future);
  Check(storage.WriteAtomicPrivate(Path(kFirstAccount), future).ok(), "future-version fixture writes");
  loaded = store.LoadV1(kFirstAccount);
  Check(!loaded.ok() && loaded.error() == playden::StorageError::kInvalidData,
        "valid-checksum future version is rejected");

  const playden::Bytes oversized(1024 * 1024 + 1, 0x41);
  Check(storage.WriteAtomicPrivate(Path(kFirstAccount), oversized).ok(), "oversized file fixture writes");
  loaded = store.LoadV1(kFirstAccount);
  Check(!loaded.ok(), "oversized organization is rejected by exact read bound");

  const std::string blocker = directory.path() + "/not-a-directory";
  { std::ofstream output(blocker); output << "block"; }
  playden::Storage failing_storage(blocker);
  playden::OrganizationStore failing(failing_storage);
  loaded = failing.LoadV1(kFirstAccount);
  Check(!loaded.ok() && loaded.error() != playden::StorageError::kNotFound,
        "storage read failure remains distinct from missing organization");
  const auto clear_failed = failing.Clear(kFirstAccount);
  Check(!clear_failed.ok(), "organization clear propagates storage removal failure");
}

void V1RejectsSemanticCollectionNamesOnLoad() {
  TemporaryDirectory directory;
  playden::Storage storage(directory.path());
  playden::OrganizationStore store(storage);
  auto snapshot = Example();
  snapshot.collections[0].name = "Alpha";
  snapshot.collections[1].name = "Bravo";
  Check(store.SaveV1(kFirstAccount, snapshot).ok(), "semantic collection-name fixture saves");
  const auto stored = storage.Read(Path(kFirstAccount));
  Check(stored.ok(), "semantic collection-name fixture reads");
  if (!stored.ok()) return;

  auto whitespace = stored.value();
  Check(ReplaceBytes(whitespace, "Alpha", "     ") &&
            storage.WriteAtomicPrivate(Path(kFirstAccount), whitespace).ok(),
        "valid-checksum whitespace-name fixture writes");
  auto loaded = store.LoadV1(kFirstAccount);
  Check(!loaded.ok() && loaded.error() == playden::StorageError::kInvalidData,
        "load rejects whitespace-only collection name");

  auto duplicate = stored.value();
  Check(ReplaceBytes(duplicate, "Bravo", "Alpha") &&
            storage.WriteAtomicPrivate(Path(kFirstAccount), duplicate).ok(),
        "valid-checksum duplicate-name fixture writes");
  loaded = store.LoadV1(kFirstAccount);
  Check(!loaded.ok() && loaded.error() == playden::StorageError::kInvalidData,
        "load rejects duplicate collection names");
}

void V1PreservesOrphanedPersonalState() {
  TemporaryDirectory directory;
  playden::Storage storage(directory.path());
  playden::OrganizationStore store(storage);
  auto snapshot = Example();
  snapshot.selected_game_id = SteamId(999999);
  snapshot.notes[SteamId(888888)] = "Keep this when ownership metadata is omitted";
  snapshot.favorites.insert(SteamId(777777));
  Check(store.SaveV1(kFirstAccount, snapshot).ok(), "orphaned personal state saves");
  const auto loaded = store.LoadV1(kFirstAccount);
  Check(loaded.ok() && loaded.value() && loaded.value()->selected_game_id == SteamId(999999) &&
            loaded.value()->notes.at(SteamId(888888)) == "Keep this when ownership metadata is omitted" &&
            loaded.value()->favorites.count(SteamId(777777)),
        "organization store does not require current ownership or drop omitted game IDs");
}

void ClearRemovesFinalAndTemporaryFiles() {
  TemporaryDirectory directory;
  playden::Storage storage(directory.path());
  playden::OrganizationStore store(storage);
  Check(store.Save(kDevice, Example()).ok(), "clear fixture saves");
  Check(storage.WriteAtomicPrivate(kV2Path + ".tmp", {'s', 't', 'a', 'l', 'e'}).ok(),
        "stale temporary fixture writes");
  Check(store.Clear(kDevice).ok(), "organization clear succeeds");
  const auto missing = store.Load(kDevice);
  Check(missing.ok() && !missing.value(), "cleared organization becomes missing");
  const auto temporary = storage.Read(kV2Path + ".tmp");
  Check(!temporary.ok() && temporary.error() == playden::StorageError::kNotFound,
        "clear removes stale temporary file");
  Check(store.Clear(kDevice).ok(), "organization clear is idempotent");
}

void SelectionDoesNotConsumeAnnotationCapacity() {
  TemporaryDirectory directory;playden::Storage storage(directory.path());playden::OrganizationStore store(storage);
  playden::OrganizationSnapshot value;
  for(std::uint32_t id=1;id<=10000;++id)value.favorites.insert(SteamId(id));
  value.selected_game_id= SteamId(20000);
  Check(playden::OrganizationSnapshotIsValid(value),"fixed selection does not consume the annotation ID budget");
  Check(store.Save(kDevice,value).ok(),"full annotations preserve independent selection");
  const auto loaded=store.Load(kDevice);
  Check(loaded.ok() && loaded.value() && loaded.value()->selected_game_id==SteamId(20000),"selection round trips with full annotation capacity");
}

void SharedValidationMatchesEncodedBoundary() {
  TemporaryDirectory directory;playden::Storage storage(directory.path());playden::OrganizationStore store(storage);
  playden::OrganizationSnapshot value;
  for(std::uint32_t id=1;id<=255;++id)value.notes[SteamId(id)]=std::string(4096,'x');
  value.notes[SteamId(256)]=std::string(1327,'x');
  Check(playden::OrganizationSnapshotIsValid(value),"exact one MiB snapshot validates");
  Check(store.Save(kDevice,value).ok(),"exact one MiB snapshot saves");
  const auto bytes=storage.Read(kV2Path);
  Check(bytes.ok() && bytes.value().size()==1024*1024,"shared validator includes every encoded prefix and checksum");
  value.notes[SteamId(256)].push_back('x');
  Check(!playden::OrganizationSnapshotIsValid(value),"one extra encoded byte is rejected before presentation apply");
  Check(!store.Save(kDevice,value).ok(),"oversize snapshot cannot replace previous file");
  const auto restored=store.Load(kDevice);
  Check(restored.ok() && restored.value() && restored.value()->notes.at(SteamId(256)).size()==1327,"failed replacement preserves prior snapshot");
}


const playden::GameId kLocalGame = playden::GameId::Local("0b8f6c1e-4d2a-4f3b-9a1c-2e5d7f8a9b0c");

// Sets a file's modification time to `seconds` since the epoch.
bool Touch(const std::string& path, long seconds) {
  const timeval times[2] = {{seconds, 0}, {seconds, 0}};
  return utimes(path.c_str(), times) == 0;
}

bool Exists(const TemporaryDirectory& directory, const std::string& name) {
  struct stat info {};
  return stat((directory.path() + "/" + name).c_str(), &info) == 0;
}

void V2RoundTripsEveryStore() {
  TemporaryDirectory directory;
  playden::Storage storage(directory.path());
  playden::OrganizationStore store(storage);
  auto expected = Example();
  expected.favorites.insert(kLocalGame);
  expected.hidden.insert(kLocalGame);
  expected.collections[1].members.insert(kLocalGame);
  expected.ratings[kLocalGame] = playden::CompatibilityRating::kPlayable;
  expected.notes[kLocalGame] = "Imported";
  expected.selected_game_id = kLocalGame;
  expected.filters.insert("hide-demos");
  expected.filters.insert("hide-family");
  Check(playden::OrganizationSnapshotIsValid(expected), "a Local game is valid organization");
  Check(store.Save(kDevice, expected).ok(), "device organization saves");
  const auto restored = store.Load(kDevice);
  Check(restored.ok() && restored.value() && *restored.value() == expected,
        "a Local game round trips in favorites, hidden, collections, ratings, notes, selection and filters");
  auto v1_demos = Example();
  v1_demos.schema_version = 1;
  v1_demos.filters = {"hide-demos"};
  Check(!store.SaveV1(kFirstAccount, v1_demos).ok(), "version 1 cannot hold hidden demos");
  auto three_filters = expected;
  three_filters.filters.insert("controller-full");
  Check(!store.Save(kDevice, three_filters).ok(), "version 2 holds only Playable, hidden demos and family games, and a store");
  auto every_filter = expected;
  every_filter.filters.insert("store-imported");
  Check(store.Save(kDevice, every_filter).ok(), "the Store filter saves with the three others");
  const auto with_store = store.Load(kDevice);
  Check(with_store.ok() && with_store.value() && *with_store.value() == every_filter, "and round trips");
  for (const std::string filter : {"store-steam", "store-gog"}) {
    auto one = Example();
    one.filters = {filter};
    Check(store.Save(kDevice, one).ok(), ("the Store filter saves " + filter).c_str());
    const auto loaded = store.Load(kDevice);
    Check(loaded.ok() && loaded.value() && *loaded.value() == one, ("and reads " + filter).c_str());
  }
  auto two_stores = expected;
  two_stores.filters = {"store-steam", "store-gog"};
  Check(!store.Save(kDevice, two_stores).ok(), "the Store filter picks one store");
  auto unknown_store = expected;
  unknown_store.filters = {"store-epic"};
  Check(!store.Save(kDevice, unknown_store).ok(), "the writer refuses a store it doesn't know");
  auto v1_store = Example();
  v1_store.schema_version = 1;
  v1_store.filters = {"store-steam"};
  Check(!store.SaveV1(kFirstAccount, v1_store).ok(), "version 1 cannot hold the Store filter");
  auto v1_family = Example();
  v1_family.schema_version = 1;
  v1_family.filters = {"hide-family"};
  Check(!store.SaveV1(kFirstAccount, v1_family).ok(), "version 1 cannot hold hidden family games");
  playden::OrganizationSnapshot unselected;
  Check(store.Save(kDevice, unselected).ok(), "no selection saves");
  const auto empty = store.Load(kDevice);
  Check(empty.ok() && empty.value() && !empty.value()->selected_game_id.valid() && *empty.value() == unselected,
        "no selection round trips");
  Check(!Exists(directory, Path(kFirstAccount)), "nothing is written to a version 1 file");
  Check(!store.Save(kFirstAccount, expected).ok() && !store.Load(kFirstAccount).ok() &&
            !store.Clear(kFirstAccount).ok() && !store.Load(0).ok(),
        "only the device scope is accepted");
  auto invalid = expected;
  invalid.favorites.insert(playden::GameId{playden::Store::kLocal, "not-a-uuid"});
  Check(!store.Save(kDevice, invalid).ok(), "an invalid game is rejected");
  struct stat info {};
  Check(stat((directory.path() + "/" + kV2Path).c_str(), &info) == 0 && (info.st_mode & 0777) == 0666,
        "organization.v2 is open to FTP (#868)");
}

// Corrupt organization.v2 fails the load like a corrupt .v1 did; it never falls back to the merge.
// A torn save (#907): the file is set aside, the organization starts again empty (no .v1 merge, then or
// later), and saving works.
void V2TornIsSetAsideWithoutMerging() {
  TemporaryDirectory directory;
  playden::Storage storage(directory.path());
  playden::OrganizationStore store(storage);
  Check(store.SaveV1(kFirstAccount, Example()).ok(), "torn: legacy fixture saves");
  playden::OrganizationSnapshot device;
  device.favorites.insert(kLocalGame);
  Check(store.Save(kDevice, device).ok(), "torn: device fixture saves");
  const auto stored = storage.Read(kV2Path);
  Check(stored.ok(), "torn: device fixture reads");
  if (!stored.ok()) return;
  auto truncated = stored.value();
  truncated.resize(9);
  const playden::Bytes torn[] = {playden::Bytes{}, playden::Bytes(512, 0), truncated};
  for (const auto& bytes : torn) {
    Check(storage.WriteAtomicPrivate(kV2Path, bytes).ok(), "torn: fixture writes");
    auto loaded = store.Load(kDevice);
    Check(loaded.ok() && !loaded.value(), "torn: an empty, zero-filled or cut-short file loads as no organization");
    const auto note = store.TakeLoadNote();
    Check(note.find("damaged") != std::string::npos && note.find("organization.v2.damaged") != std::string::npos,
          "torn: the load says it set the file aside");
    Check(store.TakeLoadNote().empty(), "torn: the note is taken once");
    Check(!storage.Read(kV2Path).ok(), "torn: organization.v2 is gone");
    const auto kept = storage.Read("organization.v2.damaged");
    Check(kept.ok() && kept.value() == bytes, "torn: its bytes are kept as organization.v2.damaged");
    loaded = store.Load(kDevice);
    Check(loaded.ok() && !loaded.value() && store.TakeLoadNote().empty(),
          "torn: the next load doesn't merge the .v1 files either");
    Check(store.Save(kDevice, device).ok(), "torn: saving works again");
    loaded = store.Load(kDevice);
    Check(loaded.ok() && loaded.value() && loaded.value()->favorites == device.favorites, "torn: the save loads back");
  }
  Check(store.Clear(kDevice).ok() && !storage.Read("organization.v2.damaged").ok(),
        "torn: Clear also removes the damaged file, so the next load merges again");
  const auto merged = store.Load(kDevice);
  Check(merged.ok() && merged.value() && merged.value()->favorites == Example().favorites, "torn: merge after Clear");

  // The damaged copy can't be written (here a folder is in its way): the file stays, and the load fails as before,
  // so the .v1 files can't come back at a later start.
  const std::string blocker = directory.path() + "/organization.v2.damaged";
  Check(mkdir(blocker.c_str(), 0700) == 0, "torn: blocker folder");
  const playden::Bytes zeros(64, 0);
  Check(storage.WriteAtomicPrivate(kV2Path, zeros).ok(), "torn: fixture writes again");
  const auto refused = store.Load(kDevice);
  Check(!refused.ok() && refused.error() == playden::StorageError::kInvalidData,
        "torn: a file that can't be set aside fails the load");
  const auto still = storage.Read(kV2Path);
  Check(still.ok() && still.value() == zeros, "torn: and stays as it is");
  rmdir(blocker.c_str());
}

void V2DamagedFailsWithoutMerging() {
  TemporaryDirectory directory;
  playden::Storage storage(directory.path());
  playden::OrganizationStore store(storage);
  auto legacy = Example();
  Check(store.SaveV1(kFirstAccount, legacy).ok(), "legacy fixture saves");
  playden::OrganizationSnapshot device;
  device.favorites.insert(kLocalGame);
  Check(store.Save(kDevice, device).ok(), "device fixture saves");
  const auto stored = storage.Read(kV2Path);
  Check(stored.ok(), "device fixture reads");
  if (!stored.ok()) return;

  auto corrupt = stored.value();
  corrupt[corrupt.size() / 2] ^= 0x5a;
  Check(storage.WriteAtomicPrivate(kV2Path, corrupt).ok(), "corrupt file writes");
  auto loaded = store.Load(kDevice);
  Check(loaded.ok() && !loaded.value(), "checksum corruption is set aside, not replaced by the merge");
  const auto kept = storage.Read("organization.v2.damaged");
  Check(!storage.Read(kV2Path).ok() && kept.ok() && kept.value() == corrupt,
        "a damaged organization.v2 is kept as organization.v2.damaged");
  (void)store.TakeLoadNote();

  // A frame that checks out but holds what this build can't read stays as it is: a newer Playden may have
  // written it.
  auto future = stored.value();
  future[8] = 3;
  RewriteChecksum(future);
  Check(storage.WriteAtomicPrivate(kV2Path, future).ok(), "future-version fixture writes");
  loaded = store.Load(kDevice);
  Check(!loaded.ok() && loaded.error() == playden::StorageError::kInvalidData, "future version is rejected");
  const auto future_still = storage.Read(kV2Path);
  Check(future_still.ok() && future_still.value() == future, "a future version is left as it is");

  auto bad_key = stored.value();
  Check(ReplaceBytes(bad_key, "local-", "steam-") && storage.WriteAtomicPrivate(kV2Path, bad_key).ok(),
        "unknown key fixture writes");
  loaded = store.Load(kDevice);
  Check(!loaded.ok() && loaded.error() == playden::StorageError::kInvalidData, "an unparseable game key is rejected");

  const auto v1 = store.LoadV1(kFirstAccount);
  Check(v1.ok() && v1.value() && v1.value()->favorites == legacy.favorites, "the .v1 file is untouched");
}

void MergesEveryAccount() {
  TemporaryDirectory directory;
  playden::Storage storage(directory.path());
  playden::OrganizationStore store(storage);
  // Older account: its own collection IDs collide with the newer one's.
  playden::OrganizationSnapshot older;
  older.favorites = {SteamId(10), SteamId(50)};
  older.hidden = {SteamId(60)};
  older.collections = {
      {"collection-1", "Co-op café", true, {SteamId(50), SteamId(10)}},
      {"collection-2", "Racing", false, {SteamId(70)}},
  };
  older.ratings = {{SteamId(10), playden::CompatibilityRating::kBroken},
                   {SteamId(50), playden::CompatibilityRating::kWorks}};
  older.notes = {{SteamId(10), "older note"}, {SteamId(50), "only older"}};
  older.query = "older";
  older.selected_game_id = SteamId(50);
  // Newer account.
  playden::OrganizationSnapshot newer;
  newer.favorites = {SteamId(20)};
  newer.hidden = {SteamId(30)};
  newer.collections = {
      {"collection-1", "旅のゲーム", false, {SteamId(20)}},
      {"collection-2", "Co-op café", false, {SteamId(20)}},
  };
  newer.ratings = {{SteamId(10), playden::CompatibilityRating::kPlayable}};
  newer.notes = {{SteamId(10), "newer note"}};
  newer.sort = "playtime";
  newer.scope = "collection-2";
  newer.selected_game_id = SteamId(20);
  Check(store.SaveV1(kFirstAccount, older).ok() && store.SaveV1(kSecondAccount, newer).ok(),
        "legacy fixtures save");
  // A damaged account file and unrelated names are skipped.
  constexpr std::uint64_t kThirdAccount = 76561198000000003ULL;
  Check(storage.WriteAtomicPrivate(Path(kThirdAccount), {'b', 'a', 'd'}).ok(), "damaged legacy fixture writes");
  Check(storage.WriteAtomicPrivate("organization-0.v1", {'x'}).ok() &&
            storage.WriteAtomicPrivate("organization-12a.v1", {'x'}).ok(),
        "misnamed fixtures write");
  // The newer file is the second account's, although its account number sorts later and the
  // other account was written last.
  Check(Touch(directory.path() + "/" + Path(kFirstAccount), 1700000000) &&
            Touch(directory.path() + "/" + Path(kSecondAccount), 1800000000) &&
            Touch(directory.path() + "/" + Path(kThirdAccount), 1900000000),
        "modification times set");
  const auto first_before = storage.Read(Path(kFirstAccount));
  const auto second_before = storage.Read(Path(kSecondAccount));

  const auto loaded = store.Load(kDevice);
  Check(loaded.ok() && loaded.value(), "first load merges the account files");
  if (!loaded.ok() || !loaded.value()) return;
  const auto& merged = *loaded.value();
  Check(merged.schema_version == 2, "merged organization is version 2");
  Check(merged.favorites == std::set<playden::GameId>({SteamId(10), SteamId(20), SteamId(50)}), "favorites united");
  Check(merged.hidden == std::set<playden::GameId>({SteamId(30), SteamId(60)}), "hidden united");
  Check(merged.collections.size() == 3, "collections united by name");
  if (merged.collections.size() == 3) {
    Check(merged.collections[0].id == "collection-1" && merged.collections[0].name == "旅のゲーム" &&
              merged.collections[1].id == "collection-2" && merged.collections[1].name == "Co-op café",
          "the newest file's collections come first, in its order, with its IDs");
    Check(merged.collections[1].members == std::set<playden::GameId>({SteamId(10), SteamId(20), SteamId(50)}) &&
              merged.collections[1].pinned,
          "a collection of the same name unites members and keeps a pin");
    Check(merged.collections[2].name == "Racing" && merged.collections[2].id == "collection-3" &&
              merged.collections[2].members == std::set<playden::GameId>({SteamId(70)}),
          "an older file's collection is appended under a free ID");
  }
  Check(merged.ratings.at(SteamId(10)) == playden::CompatibilityRating::kPlayable &&
            merged.ratings.at(SteamId(50)) == playden::CompatibilityRating::kWorks,
        "ratings come from the newest file that has one");
  Check(merged.notes.at(SteamId(10)) == "newer note" && merged.notes.at(SteamId(50)) == "only older",
        "notes come from the newest file that has one");
  Check(merged.selected_game_id == SteamId(20) && merged.sort == "playtime" && merged.scope == "collection-2" &&
            merged.query.empty(),
        "selection and view come from the newest file");

  Check(Exists(directory, kV2Path), "the merge is written to organization.v2");
  const auto first_after = storage.Read(Path(kFirstAccount));
  const auto second_after = storage.Read(Path(kSecondAccount));
  Check(first_before.ok() && first_after.ok() && first_before.value() == first_after.value() &&
            second_before.ok() && second_after.ok() && second_before.value() == second_after.value() &&
            Exists(directory, Path(kThirdAccount)),
        ".v1 files are neither changed nor removed");

  // Later loads read organization.v2 only: a .v1 change no longer matters.
  auto changed = newer;
  changed.favorites.insert(SteamId(99));
  Check(store.SaveV1(kSecondAccount, changed).ok(), "legacy fixture changes");
  const auto again = store.Load(kDevice);
  Check(again.ok() && again.value() && *again.value() == merged, "the merge happens once");
}

void MergeWithoutLegacyFilesIsMissing() {
  TemporaryDirectory directory;
  playden::Storage storage(directory.path());
  playden::OrganizationStore store(storage);
  Check(storage.WriteAtomicPrivate(Path(kFirstAccount), {'b', 'a', 'd'}).ok(), "damaged legacy fixture writes");
  const auto loaded = store.Load(kDevice);
  Check(loaded.ok() && !loaded.value(), "no readable .v1 file is a missing organization");
  Check(!Exists(directory, kV2Path), "nothing is written when there is nothing to merge");
}

void MergePastTheLimitsKeepsTheNewestFile() {
  TemporaryDirectory directory;
  playden::Storage storage(directory.path());
  playden::OrganizationStore store(storage);
  playden::OrganizationSnapshot older, newer;
  for (std::uint32_t id = 1; id <= 6000; ++id) older.favorites.insert(SteamId(id));
  for (std::uint32_t id = 6001; id <= 12000; ++id) newer.favorites.insert(SteamId(id));
  Check(store.SaveV1(kFirstAccount, older).ok() && store.SaveV1(kSecondAccount, newer).ok(), "large fixtures save");
  Check(Touch(directory.path() + "/" + Path(kFirstAccount), 1700000000) &&
            Touch(directory.path() + "/" + Path(kSecondAccount), 1800000000),
        "modification times set");
  const auto loaded = store.Load(kDevice);
  Check(loaded.ok() && loaded.value() && loaded.value()->favorites == newer.favorites,
        "a union past ten thousand games keeps the newest file whole");
}

// ---- Files a newer Playden writes: keys and scopes this build may not know. ----

// A version 2 file written by hand, for keys and scopes the writer refuses. Lists keep their order and duplicates.
struct RawCollection {
  std::string id;
  std::string name;
  bool pinned = false;
  std::vector<std::string> members;
};

struct RawV2 {
  std::string selected;
  std::vector<std::string> favorites;
  std::vector<std::string> hidden;
  std::vector<RawCollection> collections;
  std::vector<std::pair<std::string, std::uint8_t>> ratings;
  std::vector<std::pair<std::string, std::string>> notes;
  std::string query;
  std::string scope = "all";
  std::string sort = "name";
  std::vector<std::string> filters;
};

class RawWriter {
 public:
  void U8(std::uint8_t value) { bytes_.push_back(value); }
  void U16(std::uint16_t value) { Integer(value, 2); }
  void U32(std::uint32_t value) { Integer(value, 4); }
  void String(const std::string& value) {
    U32(static_cast<std::uint32_t>(value.size()));
    bytes_.insert(bytes_.end(), value.begin(), value.end());
  }
  void Keys(const std::vector<std::string>& keys) {
    U32(static_cast<std::uint32_t>(keys.size()));
    for (const auto& key : keys) String(key);
  }
  playden::Bytes Take() {
    U32(0);  // checksum placeholder
    RewriteChecksum(bytes_);
    return std::move(bytes_);
  }

 private:
  void Integer(std::uint64_t value, unsigned size) {
    for (unsigned index = 0; index < size; ++index, value >>= 8) bytes_.push_back(static_cast<std::uint8_t>(value));
  }
  playden::Bytes bytes_;
};

playden::Bytes EncodeV2(const RawV2& file) {
  RawWriter writer;
  for (const char byte : std::string("PLDORG")) writer.U8(static_cast<std::uint8_t>(byte));
  writer.U8(0);
  writer.U8(0);
  writer.U16(2);
  writer.String(file.selected);
  writer.Keys(file.favorites);
  writer.Keys(file.hidden);
  writer.U32(static_cast<std::uint32_t>(file.collections.size()));
  for (const auto& collection : file.collections) {
    writer.String(collection.id);
    writer.String(collection.name);
    writer.U8(collection.pinned ? 1 : 0);
    writer.Keys(collection.members);
  }
  writer.U32(static_cast<std::uint32_t>(file.ratings.size()));
  for (const auto& [key, rating] : file.ratings) {
    writer.String(key);
    writer.U8(rating);
  }
  writer.U32(static_cast<std::uint32_t>(file.notes.size()));
  for (const auto& [key, note] : file.notes) {
    writer.String(key);
    writer.String(note);
  }
  writer.String(file.query);
  writer.String(file.scope);
  writer.String(file.sort);
  writer.Keys(file.filters);
  return writer.Take();
}

const std::string kLocalKey = "local-0b8f6c1e-4d2a-4f3b-9a1c-2e5d7f8a9b0c";
const std::string kEpicKey = "epic-0f8fad5bd9cb469fa16570867728950e";
const std::string kItchKey = "itch-abc";
const playden::GameId kGogFavorite = playden::GameId::Gog(1207658924);
const playden::GameId kGogHidden = playden::GameId::Gog(1453375253);
const playden::GameId kGogOther = playden::GameId::Gog(1207664663);

// Example() in the hand encoder, keys in the writer's order. Used to check the encoder against the writer.
RawV2 ExampleRaw() {
  RawV2 file;
  file.selected = "20";
  file.favorites = {"10", "20"};
  file.hidden = {"30"};
  file.collections = {{"collection-1", "旅のゲーム", true, {"10", "30"}}, {"collection-2", "Co-op café", false, {"20"}}};
  file.ratings = {{"10", 1}, {"20", 2}, {"30", 3}};
  file.notes = {{"10", "再訪したい"}, {"20", "Français — bientôt"}};
  file.query = "星 café";
  file.scope = "collection-1";
  file.sort = "recent-played";
  file.filters = {"compat-playable"};
  return file;
}

playden::Result<std::optional<playden::OrganizationSnapshot>> LoadFile(playden::Storage& storage,
                                                                       const playden::Bytes& bytes) {
  playden::OrganizationStore store(storage);
  if (!storage.WriteAtomicPrivate(kV2Path, bytes).ok())
    return playden::Result<std::optional<playden::OrganizationSnapshot>>::Failure(playden::StorageError::kWriteFailed,
                                                                                  "fixture write failed");
  return store.Load(kDevice);
}

bool FailsAsDamage(const playden::Result<std::optional<playden::OrganizationSnapshot>>& loaded) {
  return !loaded.ok() && loaded.error() == playden::StorageError::kInvalidData;
}

void RawEncoderMatchesTheWriter() {
  TemporaryDirectory directory;
  playden::Storage storage(directory.path());
  playden::OrganizationStore store(storage);
  Check(store.Save(kDevice, Example()).ok(), "encoder reference saves");
  const auto written = storage.Read(kV2Path);
  Check(written.ok() && written.value() == EncodeV2(ExampleRaw()), "the test encoder writes what Save writes");
}

// GOG games everywhere, and the GOG rail as the scope: the writer saves it and the reader keeps all of it.
void V2KeepsGogKeysAndTheGogScope() {
  TemporaryDirectory directory;
  playden::Storage storage(directory.path());
  playden::OrganizationStore store(storage);
  auto expected = Example();
  expected.favorites.insert(kGogFavorite);
  expected.favorites.insert(kLocalGame);
  expected.hidden.insert(kGogHidden);
  expected.collections[1].members.insert(kGogOther);
  expected.ratings[kGogFavorite] = playden::CompatibilityRating::kWorks;
  expected.ratings[kGogHidden] = playden::CompatibilityRating::kBroken;
  expected.notes[kGogOther] = "GOG note";
  expected.selected_game_id = kGogFavorite;
  expected.filters.insert("hide-demos");
  expected.scope = "all";
  Check(store.Save(kDevice, expected).ok(), "GOG keys save");

  auto gog_scope = expected;
  gog_scope.scope = "gog";
  Check(store.Save(kDevice, gog_scope).ok(), "the writer saves the scope gog");

  const auto stored = storage.Read(kV2Path);
  Check(stored.ok(), "GOG fixture reads");
  if (!stored.ok()) return;
  const auto loaded = store.Load(kDevice);
  Check(loaded.ok() && loaded.value() && *loaded.value() == gog_scope,
        "GOG keys are kept in favorites, hidden, collections, ratings, notes and selection, and so is the scope gog");
  const auto after = storage.Read(kV2Path);
  Check(after.ok() && after.value() == stored.value(), "loading does not rewrite the file");
}

void V2SkipsUnknownStoreKeys() {
  TemporaryDirectory directory;
  playden::Storage storage(directory.path());
  auto file = ExampleRaw();
  file.selected = kEpicKey;
  file.favorites = {"10", kItchKey, "20", kEpicKey, kItchKey, kLocalKey};
  file.hidden = {kEpicKey, "30"};
  file.collections[0].members = {"10", kEpicKey, "30", kItchKey};
  file.collections.push_back({"collection-3", "Only newer stores", true, {kEpicKey, kEpicKey}});
  file.ratings = {{"10", 1}, {kEpicKey, 3}, {"20", 2}, {kItchKey, 0}, {kItchKey, 2}, {"30", 3}};
  file.notes = {{kItchKey, "itch note"}, {"10", "再訪したい"}, {kItchKey, "again"}, {"20", "Français — bientôt"}};
  const auto bytes = EncodeV2(file);

  auto expected = Example();
  expected.favorites.insert(kLocalGame);
  expected.collections.push_back({"collection-3", "Only newer stores", true, {}});
  expected.selected_game_id = {};
  const auto loaded = LoadFile(storage, bytes);
  Check(loaded.ok() && loaded.value() && *loaded.value() == expected,
        "only unknown-store entries are gone; collections keep name, pin and other members; no selection");
  const auto after = storage.Read(kV2Path);
  Check(after.ok() && after.value() == bytes, "loading a file with unknown keys does not rewrite it");

  auto duplicate = ExampleRaw();
  duplicate.favorites = {"10", "10"};
  Check(FailsAsDamage(LoadFile(storage, EncodeV2(duplicate))), "a duplicate known key is still refused");
  duplicate = ExampleRaw();
  duplicate.ratings = {{"10", 1}, {"10", 2}};
  Check(FailsAsDamage(LoadFile(storage, EncodeV2(duplicate))), "a duplicate known rating is still refused");
}

void V2StillRefusesDamage() {
  TemporaryDirectory directory;
  playden::Storage storage(directory.path());
  for (const std::string key : {"gog-0", "gog-01", "0440", "local-0F8FAD5B-D9CB-469F-A165-70867728950E", "",
                                "steam-440", "Epic-abc", "epic-", "-abc", "epic-ABC"}) {
    auto file = ExampleRaw();
    file.favorites.push_back(key);
    Check(FailsAsDamage(LoadFile(storage, EncodeV2(file))), ("a malformed key is damage: " + key).c_str());
  }
  auto file = ExampleRaw();
  file.selected = "gog-0";
  Check(FailsAsDamage(LoadFile(storage, EncodeV2(file))), "a malformed selected key is damage");
  file = ExampleRaw();
  file.ratings.push_back({kEpicKey, 4});
  Check(FailsAsDamage(LoadFile(storage, EncodeV2(file))), "an unknown rating on an unknown-store key is damage");
  file = ExampleRaw();
  file.notes.push_back({kEpicKey, std::string("bad-") + static_cast<char>(0xc0)});
  Check(FailsAsDamage(LoadFile(storage, EncodeV2(file))), "an invalid UTF-8 note on an unknown-store key is damage");
  file = ExampleRaw();
  file.collections[1].members.push_back("0440");
  Check(FailsAsDamage(LoadFile(storage, EncodeV2(file))), "a malformed collection member is damage");

  file = ExampleRaw();
  file.favorites.push_back(kEpicKey);
  auto corrupt = EncodeV2(file);
  corrupt[corrupt.size() - 1] ^= 0x5a;
  const auto torn = LoadFile(storage, corrupt);
  Check(torn.ok() && !torn.value(), "a bad checksum is set aside as a torn save");
  file = ExampleRaw();
  file.scope = "";
  Check(FailsAsDamage(LoadFile(storage, EncodeV2(file))), "an empty scope is damage");
  file.scope = std::string("bad-") + static_cast<char>(0xc0);
  Check(FailsAsDamage(LoadFile(storage, EncodeV2(file))), "an invalid UTF-8 scope is damage");
}

void V2UnknownScopeReadsAsAll() {
  TemporaryDirectory directory;
  playden::Storage storage(directory.path());
  auto file = ExampleRaw();
  file.scope = "collection-2";
  auto loaded = LoadFile(storage, EncodeV2(file));
  Check(loaded.ok() && loaded.value() && loaded.value()->scope == "collection-2",
        "a scope naming a collection is kept");
  for (const std::string scope :
       {"collection-9", "gog", "steam", "epic", "favorites", "hidden", "installed", "imported", "new-collection",
        "steam-collection:uc-1", "gog-tag:42", "steam-collection:", "steam-collection:a b", "epic-collection:1"}) {
    file.scope = scope;
    loaded = LoadFile(storage, EncodeV2(file));
    const bool known = scope == "favorites" || scope == "hidden" || scope == "installed" || scope == "imported" ||
                       scope == "gog" || scope == "steam" || scope == "steam-collection:uc-1" || scope == "gog-tag:42";
    auto expected = Example();
    expected.scope = known ? scope : "all";
    Check(loaded.ok() && loaded.value() && *loaded.value() == expected,
          ("scope " + scope + (known ? " is kept" : " reads as all, nothing else changes")).c_str());
  }
}

// Filters a newer Playden writes: a filter this build doesn't know, or the Store filter naming a store it doesn't know, is
// skipped and the rest of the file kept; so is a Store filter picking several stores. Damage is still refused.
void V2SkipsUnknownFilters() {
  TemporaryDirectory directory;
  playden::Storage storage(directory.path());
  auto file = ExampleRaw();
  file.filters = {"newer-filter", "compat-playable", "store-epic", "hide-demos", "hide-family", "store-gog", "x"};
  auto expected = Example();
  expected.filters = {"compat-playable", "hide-demos", "hide-family", "store-gog"};
  const auto bytes = EncodeV2(file);
  auto loaded = LoadFile(storage, bytes);
  Check(loaded.ok() && loaded.value() && *loaded.value() == expected,
        "unknown filters and stores are skipped, more than three filters are read, the rest is kept");
  const auto after = storage.Read(kV2Path);
  Check(after.ok() && after.value() == bytes, "loading a file with unknown filters does not rewrite it");

  file.filters = {"store-epic"};
  expected.filters = {};
  loaded = LoadFile(storage, EncodeV2(file));
  Check(loaded.ok() && loaded.value() && *loaded.value() == expected, "an unknown store reads as Any");

  file.filters = {"compat-playable", "store-steam", "store-imported"};
  expected.filters = {"compat-playable"};
  loaded = LoadFile(storage, EncodeV2(file));
  Check(loaded.ok() && loaded.value() && *loaded.value() == expected, "several stores read as Any");

  file.filters.assign(64, "");
  for (std::size_t index = 0; index < file.filters.size(); ++index) file.filters[index] = "newer-" + std::to_string(index);
  expected.filters = {};
  loaded = LoadFile(storage, EncodeV2(file));
  Check(loaded.ok() && loaded.value() && *loaded.value() == expected, "64 filters are read");
  file.filters.push_back("newer-64");
  Check(FailsAsDamage(LoadFile(storage, EncodeV2(file))), "65 filters are damage");

  for (const std::vector<std::string>& filters :
       std::vector<std::vector<std::string>>{{""}, {"store-epic", "store-epic"}, {"hide-demos", "hide-demos"},
                                              {std::string("bad-") + static_cast<char>(0xc0)}, {std::string(33, 'f')}}) {
    file.filters = filters;
    Check(FailsAsDamage(LoadFile(storage, EncodeV2(file))), "an empty, repeated, invalid or oversized filter is damage");
  }
}

}  // namespace

int main() {
  SelectionDoesNotConsumeAnnotationCapacity();
  SharedValidationMatchesEncodedBoundary();
  V1RoundTripAndIsolation();
  V2RoundTripsEveryStore();
  V2TornIsSetAsideWithoutMerging();
  V2DamagedFailsWithoutMerging();
  MergesEveryAccount();
  MergeWithoutLegacyFilesIsMissing();
  MergePastTheLimitsKeepsTheNewestFile();
  RawEncoderMatchesTheWriter();
  V2KeepsGogKeysAndTheGogScope();
  V2SkipsUnknownStoreKeys();
  V2StillRefusesDamage();
  V2UnknownScopeReadsAsAll();
  V2SkipsUnknownFilters();
  RejectsInvalidValuesWithoutReplacingGoodData();
  V1RejectsDamagedAndOversizedFiles();
  V1RejectsSemanticCollectionNamesOnLoad();
  V1PreservesOrphanedPersonalState();
  ClearRemovesFinalAndTemporaryFiles();
  if (failures) {
    std::fprintf(stderr, "%d organization assertion(s) failed\n", failures);
    return EXIT_FAILURE;
  }
  std::puts("library_organization_test: all assertions passed");
  return EXIT_SUCCESS;
}
