#include "stores/local/import_store.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace playden::stores::local;
namespace fs = std::filesystem;

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x); std::exit(1); } } while (0)

namespace {

struct TemporaryDirectory {
  TemporaryDirectory() {
    auto pattern = (fs::temp_directory_path() / "playden-imports-XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    CHECK(::mkdtemp(writable.data()) != nullptr);
    path = fs::canonical(writable.data()).string();
  }
  ~TemporaryDirectory() {
    std::error_code ignored;
    fs::remove_all(path, ignored);
  }
  std::string path;
};

// Fixed IDs, then none.
ImportStore::IdSource Ids(std::vector<std::string> ids) {
  auto shared = std::make_shared<std::vector<std::string>>(std::move(ids));
  return [shared]() -> std::optional<std::string> {
    if (shared->empty()) return std::nullopt;
    auto id = shared->front();
    shared->erase(shared->begin());
    return id;
  };
}
ImportStore::Clock Clock() { return [] { return std::uint64_t{1760000000}; }; }

constexpr const char* kA = "1b4e28ba-2fa1-41d2-883f-0016d3cca427";
constexpr const char* kB = "6fa459ea-ee8a-4ca4-894e-db77e160355e";
constexpr const char* kC = "16fd2706-8baf-433b-82eb-8c7fada847da";

std::string Text(const playden::Storage& storage) {
  const auto read = storage.ReadDurable("imports.json");
  CHECK(read.ok());
  return std::string(read.value().begin(), read.value().end());
}

void Write(const playden::Storage& storage, const std::string& text) {
  CHECK(storage.WriteDurable("imports.json", playden::Bytes(text.begin(), text.end())).ok());
}

void RandomIdsAreLowerCaseVersionFour() {
  const auto first = RandomImportId(), second = RandomImportId();
  CHECK(first && second && *first != *second);
  CHECK(IsImportId(*first) && (*first)[14] == '4');
  CHECK(std::string("89ab").find((*first)[19]) != std::string::npos);
  CHECK(!IsImportId("1B4E28BA-2FA1-41D2-883F-0016D3CCA427") && !IsImportId("1b4e28ba2fa141d2883f0016d3cca427"));
}

void AddFindAndRoundTrip() {
  TemporaryDirectory directory;
  playden::Storage storage(directory.path);
  CHECK(storage.EnsureRootDirectory().ok());
  ImportStore store(storage, "imports.json", Ids({kA, kB}), Clock());
  CHECK(store.Add("usb0", "playden/import/Delores", "Delores", "Delores.exe").error() == ImportError::kNotLoaded);
  CHECK(store.Load().ok() && store.Games().empty());  // a missing file loads empty
  const auto added = store.Add("usb0", "playden/import/Delores", "Delores", "Delores.exe");
  CHECK(added.ok() && added.value().id == kA && added.value().state == ImportState::kInPlace);
  CHECK(added.value().added_at == 1760000000);
  const auto drop = store.Add("internal", "playden-import/Night Sky", "Night \"Sky\"\n", "NightSky.exe");
  CHECK(drop.ok() && drop.value().id == kB);
  CHECK(store.Find(kA)->folder == "playden/import/Delores");
  CHECK(store.FindByFolder("internal", "playden-import/Night Sky")->id == kB);
  CHECK(!store.FindByFolder("usb1", "playden/import/Delores"));  // slots, not drives
  CHECK(!store.Find(kC));
  const auto file = Text(storage);
  CHECK(file.rfind("{\"version\":1,\"games\":[{\"id\":\"1b4e28ba-2fa1-41d2-883f-0016d3cca427\",\"title\":\"Delores\","
                   "\"volume\":\"usb0\",\"folder\":\"playden/import/Delores\",\"program\":\"Delores.exe\","
                   "\"state\":\"in_place\",\"added_at\":1760000000}", 0) == 0);

  ImportStore reloaded(storage, "imports.json", Ids({}), Clock());
  CHECK(reloaded.Load().ok());
  const auto games = reloaded.Games();
  CHECK(games.size() == 2 && games[0].id == kA && games[1].id == kB && games[1].title == "Night \"Sky\"\n");
  CHECK(reloaded.Save().ok() && Text(storage) == file);  // byte-stable

  // No random source: nothing is added.
  CHECK(reloaded.Add("usb0", "playden/import/Other", "Other", "Other.exe").error() == ImportError::kNoRandom);
  CHECK(reloaded.Games().size() == 2);
}

void DuplicatesAndInvalidFieldsAreRefused() {
  TemporaryDirectory directory;
  playden::Storage storage(directory.path);
  CHECK(storage.EnsureRootDirectory().ok());
  ImportStore store(storage, "imports.json", Ids({kA, kA, kB, kC}), Clock());
  CHECK(store.Load().ok());
  CHECK(store.Add("usb0", "playden/import/Delores", "Delores", "Delores.exe").ok());
  CHECK(store.Add("usb0", "playden/import/Delores", "Again", "Other.exe").error() == ImportError::kDuplicate);
  // A generated ID already in use is never handed out twice.
  const auto other = store.Add("usb1", "playden/import/Delores", "Delores", "Delores.exe");
  CHECK(other.ok() && other.value().id == kB);
  for (const auto& bad : std::vector<std::vector<std::string>>{
           {"", "f", "t", "p.exe"}, {"USB0", "f", "t", "p.exe"}, {"usb0", "/abs", "t", "p.exe"},
           {"usb0", "a/../b", "t", "p.exe"}, {"usb0", "f", "", "p.exe"}, {"usb0", "f", "t", "../p.exe"},
           {"usb0", "f", "t", "a\\p.exe"}, {"usb0", "f/", "t", "p.exe"}})
    CHECK(store.Add(bad[0], bad[1], bad[2], bad[3]).error() == ImportError::kInvalid);
  CHECK(store.Games().size() == 2);
}

void InPlaceRemoveAndRestoreFromRemoved() {
  TemporaryDirectory directory;
  playden::Storage storage(directory.path);
  CHECK(storage.EnsureRootDirectory().ok());
  ImportStore store(storage, "imports.json", Ids({kA, kB}), Clock());
  CHECK(store.Load().ok());
  CHECK(store.Add("usb0", "playden/import/Celeste", "Celeste", "Celeste.exe").ok());
  CHECK(store.Remove(kA).ok());
  CHECK(store.Games().empty() && store.Removed().size() == 1);
  const auto removed = store.FindRemoved("usb0", "playden/import/Celeste");
  CHECK(removed && removed->id == kA && removed->program == "Celeste.exe");
  CHECK(store.Remove(kA).error() == ImportError::kNotFound);

  ImportStore reloaded(storage, "imports.json", Ids({kB}), Clock());
  CHECK(reloaded.Load().ok() && reloaded.Removed().size() == 1);
  // Adding the removed folder again brings its ID (and through it, bottle and settings) back.
  const auto readded = reloaded.Add("usb0", "playden/import/Celeste", "Celeste", "bin/Celeste.exe");
  CHECK(readded.ok() && readded.value().id == kA && readded.value().program == "bin/Celeste.exe");
  CHECK(reloaded.Removed().empty());
  // Removed twice keeps one entry for the folder.
  CHECK(reloaded.Remove(kA).ok() && reloaded.Add("usb0", "playden/import/Celeste", "Celeste", "Celeste.exe").ok());
  CHECK(reloaded.Remove(kA).ok() && reloaded.Removed().size() == 1);
  // Another USB slot is the same folder (drives change places): its ID, bottle and settings come back.
  const auto other = reloaded.Add("usb1", "playden/import/Celeste", "Celeste", "Celeste.exe");
  CHECK(other.ok() && other.value().id == kA);
  // Another kind of drive is another folder: a new ID.
  CHECK(reloaded.Remove(kA).ok());
  const auto m2 = reloaded.Add("m2", "playden/import/Celeste", "Celeste", "Celeste.exe");
  CHECK(m2.ok() && m2.value().id == kB);
}

void CopyTransitionsFollowTheStateTable() {
  TemporaryDirectory directory;
  playden::Storage storage(directory.path);
  CHECK(storage.EnsureRootDirectory().ok());
  ImportStore store(storage, "imports.json", Ids({kA, kB}), Clock());
  CHECK(store.Load().ok());
  CHECK(store.Add("usb0", "playden/import/Rally", "Old School Rally", "Old School Rally.exe").ok());

  // Moves the table doesn't have.
  CHECK(store.StartCopy(kA).error() == ImportError::kWrongState);
  CHECK(store.FinishCopy(kA).error() == ImportError::kWrongState);
  CHECK(store.CancelCopy(kA).error() == ImportError::kWrongState);
  CHECK(store.Uninstalled(kA).error() == ImportError::kWrongState);
  CHECK(store.QueueCopy(kC).error() == ImportError::kNotFound);

  // in_place -> copy_queued -> cancel -> in_place.
  CHECK(store.QueueCopy(kA).ok() && store.Find(kA)->state == ImportState::kCopyQueued);
  CHECK(store.QueueCopy(kA).error() == ImportError::kWrongState);
  CHECK(store.Remove(kA).error() == ImportError::kWrongState);  // cancel first
  const auto cancelled = store.CancelCopy(kA);
  CHECK(cancelled.ok() && cancelled.value().state == ImportState::kInPlace);
  CHECK(cancelled.value().failure.empty());

  // copying -> failed: like cancel, with the failure kept (and saved).
  CHECK(store.QueueCopy(kA).ok() && store.StartCopy(kA).ok() && store.Find(kA)->state == ImportState::kCopying);
  CHECK(store.Remove(kA).error() == ImportError::kWrongState);
  CHECK(store.CancelCopy(kA, "The drive was disconnected.").ok());
  {
    ImportStore reloaded(storage, "imports.json", Ids({}), Clock());
    CHECK(reloaded.Load().ok());
    CHECK(reloaded.Find(kA)->state == ImportState::kInPlace && reloaded.Find(kA)->failure == "The drive was disconnected.");
  }
  // A new copy clears the old failure; copying -> copied.
  CHECK(store.QueueCopy(kA).ok() && store.Find(kA)->failure.empty());
  CHECK(store.StartCopy(kA).ok() && store.FinishCopy(kA).ok() && store.Find(kA)->state == ImportState::kCopied);
  CHECK(store.QueueCopy(kA).error() == ImportError::kWrongState);
  CHECK(store.CancelCopy(kA).error() == ImportError::kWrongState);
  // A copied game must be uninstalled before it can be removed.
  CHECK(store.Remove(kA).error() == ImportError::kWrongState);
  CHECK(store.Uninstalled(kA).ok());
  CHECK(store.Games().empty() && store.FindRemoved("usb0", "playden/import/Rally")->id == kA);
  const auto back = store.Add("usb0", "playden/import/Rally", "Old School Rally", "Old School Rally.exe");
  CHECK(back.ok() && back.value().id == kA && back.value().state == ImportState::kInPlace);

  // A drop-folder game plays in place too: a failed copy (or move) leaves it in place, with the failure.
  CHECK(store.Add("internal", "playden-import/Delores", "Delores", "Delores.exe").ok());
  CHECK(store.QueueCopy(kB).ok() && store.StartCopy(kB).ok());
  const auto dropped = store.CancelCopy(kB, "No space left.");
  CHECK(dropped.ok() && dropped.value().id == kB && dropped.value().state == ImportState::kInPlace);
  CHECK(store.Find(kB)->state == ImportState::kInPlace && store.Find(kB)->failure == "No space left.");
  {
    ImportStore reloaded(storage, "imports.json", Ids({}), Clock());
    CHECK(reloaded.Load().ok() && reloaded.Find(kB)->failure == "No space left.");
  }
  CHECK(store.QueueCopy(kB).ok() && store.CancelCopy(kB).ok() && store.Find(kB)->failure.empty());

  CHECK(store.SetProgram(kA, "bin/Rally.exe").ok() && store.Find(kA)->program == "bin/Rally.exe");
  CHECK(store.SetProgram(kA, "../x.exe").error() == ImportError::kInvalid);
}

void DamagedFilesAreNotReplaced() {
  TemporaryDirectory directory;
  playden::Storage storage(directory.path);
  CHECK(storage.EnsureRootDirectory().ok());
  const auto expect = [&](const std::string& text, ImportError error) {
    Write(storage, text);
    ImportStore store(storage, "imports.json", Ids({kC}), Clock());
    CHECK(store.Load().error() == error);
    CHECK(!store.loaded() && store.Games().empty());
    CHECK(store.Add("usb0", "x", "X", "x.exe").error() == ImportError::kNotLoaded);
    CHECK(store.Save().error() == ImportError::kNotLoaded);
    CHECK(Text(storage) == text);  // left as it was
  };
  const std::string game = std::string("{\"id\":\"") + kA +
                           "\",\"title\":\"T\",\"volume\":\"usb0\",\"folder\":\"f\",\"program\":\"p.exe\","
                           "\"state\":\"in_place\",\"added_at\":1}";
  expect("{broken", ImportError::kMalformed);
  expect("[]", ImportError::kMalformed);
  expect(R"({"version":2,"games":[],"removed":[]})", ImportError::kUnsupportedVersion);
  expect(R"({"version":"1","games":[],"removed":[]})", ImportError::kMalformed);
  expect(R"({"version":1,"games":[]})", ImportError::kMalformed);
  expect("{\"version\":1,\"games\":[" + game + "," + game + "],\"removed\":[]}", ImportError::kMalformed);
  expect("{\"version\":1,\"games\":[" + game + "],\"removed\":[{\"id\":\"" + kA +
             "\",\"volume\":\"usb0\",\"folder\":\"g\",\"program\":\"p.exe\"}]}",
         ImportError::kMalformed);
  for (const auto& [from, to] : std::vector<std::pair<std::string, std::string>>{
           {"in_place", "moved"}, {"\"f\"", "\"../f\""}, {"\"p.exe\"", "\"/p.exe\""}, {"usb0", "USB0"},
           {kA, "1B4E28BA-2FA1-41D2-883F-0016D3CCA427"}, {"\"added_at\":1", "\"added_at\":-1"},
           {"\"title\":\"T\"", "\"title\":\"\""}}) {
    auto damaged = game;
    damaged.replace(damaged.find(from), from.size(), to);
    expect("{\"version\":1,\"games\":[" + damaged + "],\"removed\":[]}", ImportError::kMalformed);
  }
  std::string oversized(300 * 1024, ' ');
  CHECK(storage.WriteDurable("imports.json", playden::Bytes(oversized.begin(), oversized.end())).ok());
  ImportStore big(storage, "imports.json", Ids({}), Clock());
  CHECK(!big.Load().ok() && !big.loaded());

  // A good file with a failure loads.
  auto failed = game;
  failed.replace(failed.find("\"added_at\""), 0, "\"failure\":\"Out of space\",");
  Write(storage, "{\"version\":1,\"games\":[" + failed + "],\"removed\":[]}");
  ImportStore good(storage, "imports.json", Ids({}), Clock());
  CHECK(good.Load().ok() && good.Find(kA)->failure == "Out of space");
}

void FileIsPrivate() {
  TemporaryDirectory directory;
  playden::Storage storage(directory.path);
  CHECK(storage.EnsureRootDirectory().ok());
  ImportStore store(storage, "imports.json", Ids({kA}), Clock());
  CHECK(store.Load().ok() && store.Add("usb0", "playden/import/A", "A", "A.exe").ok());
  struct stat info{};
  CHECK(::stat((directory.path + "/imports.json").c_str(), &info) == 0 && (info.st_mode & 0777) == 0600);
}

}  // namespace

// USB drives can change places: a game can follow its folder, and a removed game keeps its ID on either mount.
void UsbDrivesChangePlaces() {
  TemporaryDirectory directory;
  playden::Storage storage(directory.path);
  CHECK(storage.EnsureRootDirectory().ok());
  ImportStore store(storage, "imports.json", Ids({kA, kB, kC}), Clock());
  CHECK(store.Load().ok());
  CHECK(store.Add("usb0", "playden/import/Celeste", "Celeste", "Celeste.exe").ok());
  CHECK(store.Add("usb1", "playden/import/Rally", "Rally", "Rally.exe").ok());
  CHECK(store.SetVolume(kA, "usb1").ok());
  CHECK(store.Find(kA)->volume == "usb1" && store.FindByFolder("usb1", "playden/import/Celeste")->id == kA);
  // That folder on that drive is another game's.
  CHECK(store.Add("usb0", "playden/import/Rally", "Rally 2", "Rally.exe").ok());
  CHECK(store.SetVolume(kB, "usb0").error() == ImportError::kDuplicate && store.Find(kB)->volume == "usb1");
  CHECK(store.SetVolume(kA, "not a volume!").error() == ImportError::kInvalid);
  CHECK(store.SetVolume("nobody", "usb0").error() == ImportError::kNotFound);
  // It is saved.
  ImportStore reloaded(storage, "imports.json", Ids({kC}), Clock());
  CHECK(reloaded.Load().ok() && reloaded.Find(kA)->volume == "usb1");
  // Removed on one mount, added on another: the same game.
  CHECK(reloaded.Remove(kA).ok());
  CHECK(reloaded.FindRemoved("usb2", "playden/import/Celeste")->id == kA);
  const auto readded = reloaded.Add("usb2", "playden/import/Celeste", "Celeste", "Celeste.exe");
  CHECK(readded.ok() && readded.value().id == kA && readded.value().volume == "usb2" && reloaded.Removed().empty());
  // Console storage has no mounts to change.
  CHECK(!reloaded.FindRemoved("m2", "playden/import/Celeste"));
}

int main() {
  UsbDrivesChangePlaces();
  RandomIdsAreLowerCaseVersionFour();
  AddFindAndRoundTrip();
  DuplicatesAndInvalidFieldsAreRefused();
  InPlaceRemoveAndRestoreFromRemoved();
  CopyTransitionsFollowTheStateTable();
  DamagedFilesAreNotReplaced();
  FileIsPrivate();
  std::puts("local_import_store_test passed");
  return 0;
}
