#include "library/organization.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
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
  Check((info.st_mode & 0777) == 0600, "organization file is owner-only");
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
  Check(!store.Save(kDevice, three_filters).ok(), "version 2 holds at most Playable and hidden demos");
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
  Check(stat((directory.path() + "/" + kV2Path).c_str(), &info) == 0 && (info.st_mode & 0777) == 0600,
        "organization.v2 is owner-only");
}

// Corrupt organization.v2 fails the load like a corrupt .v1 did; it never falls back to the merge.
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
  Check(!loaded.ok() && loaded.error() == playden::StorageError::kInvalidData,
        "checksum corruption is reported, not replaced by the merge");
  const auto still = storage.Read(kV2Path);
  Check(still.ok() && still.value() == corrupt, "a damaged organization.v2 is left as it is");

  auto truncated = stored.value();
  truncated.resize(9);
  Check(storage.WriteAtomicPrivate(kV2Path, truncated).ok(), "truncated file writes");
  loaded = store.Load(kDevice);
  Check(!loaded.ok() && loaded.error() == playden::StorageError::kInvalidData, "truncated organization.v2 is rejected");

  auto future = stored.value();
  future[8] = 3;
  RewriteChecksum(future);
  Check(storage.WriteAtomicPrivate(kV2Path, future).ok(), "future-version fixture writes");
  loaded = store.Load(kDevice);
  Check(!loaded.ok() && loaded.error() == playden::StorageError::kInvalidData, "future version is rejected");

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

}  // namespace

int main() {
  SelectionDoesNotConsumeAnnotationCapacity();
  SharedValidationMatchesEncodedBoundary();
  V1RoundTripAndIsolation();
  V2RoundTripsEveryStore();
  V2DamagedFailsWithoutMerging();
  MergesEveryAccount();
  MergeWithoutLegacyFilesIsMissing();
  MergePastTheLimitsKeepsTheNewestFile();
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
