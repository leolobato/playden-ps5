// SPDX-License-Identifier: GPL-3.0-or-later
// Moves and renames in the Files tab: FileStore::Move.
#include "web/file_store.h"

#include <cstdio>
#include <fcntl.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace playden::web;
namespace fs = std::filesystem;
using playden::platform::SetOwnershipProbeForTesting;

#define CHECK(condition)                                                                  \
  do {                                                                                    \
    if (!(condition)) {                                                                   \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
      std::exit(1);                                                                       \
    }                                                                                     \
  } while (false)

namespace {

void Put(const fs::path& path, const std::string& text) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << text;
}

std::string Text(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), {});
}

std::vector<std::string> Names(const fs::path& folder) {
  std::vector<std::string> names;
  for (const auto& entry : fs::directory_iterator(folder)) names.push_back(entry.path().filename().string());
  std::sort(names.begin(), names.end());
  return names;
}

bool Has(const fs::path& folder, const std::string& name) {
  for (const auto& found : Names(folder))
    if (found == name) return true;
  return false;
}

// Nothing of a move's own is left: no temporary, in the folder or below.
bool NoTemporaries(const fs::path& folder) {
  for (const auto& entry : fs::recursive_directory_iterator(folder)) {
    const auto name = entry.path().filename().string();
    if (name.rfind(kUploadPrefix, 0) == 0 || name.rfind(kRestorePrefix, 0) == 0) return false;
  }
  return true;
}

mode_t Mode(const fs::path& path) {
  struct stat info{};
  CHECK(::lstat(path.c_str(), &info) == 0);
  return info.st_mode & 07777;
}

FileRoot Root(const fs::path& path, mode_t folder_mode = 0700, mode_t file_mode = 0600) {
  FileRoot root;
  root.path = path.string();
  root.folder_mode = folder_mode;
  root.file_mode = file_mode;
  return root;
}

const FileEntry* Entry(const FileListing& listing, const std::string& name) {
  for (const auto& entry : listing.entries)
    if (entry.name == name) return &entry;
  return nullptr;
}

}  // namespace

int main() {
  const auto base = fs::canonical(fs::temp_directory_path()) / ("playden-web-move-" + std::to_string(::getpid()));
  fs::remove_all(base);
  fs::create_directories(base);
  ::umask(022);
  std::string fail_at;
  std::int64_t now = 1'000'000;
  FileStore::Options options;
  options.fault = [&](const std::string& step) { return step != fail_at; };
  options.now = [&] { return now; };
  FileStore store(options);
  MoveSummary summary;

  // Rename in place: the previous version goes with its file.
  {
    const auto folder = base / "rename";
    const auto root = Root(folder);
    Put(folder / "save.dat", "new");
    Put(folder / ".playden-bak.save.dat", "old");
    Put(folder / "other.dat", "other");
    CHECK(store.Move(root, "save.dat", root, "", "slot1.dat", MoveClash::kAsk, &summary).ok());
    CHECK(summary.name == "slot1.dat" && summary.files == 1);
    CHECK((Names(folder) == std::vector<std::string>{".playden-bak.slot1.dat", "other.dat", "slot1.dat"}));
    CHECK(Text(folder / "slot1.dat") == "new" && Text(folder / ".playden-bak.slot1.dat") == "old");
    FileListing listing;
    CHECK(store.List(root, "", &listing).ok() && Entry(listing, "slot1.dat")->previous && !Entry(listing, "save.dat"));

    // A name that's there asks first, and nothing changes.
    const auto error = store.Move(root, "slot1.dat", root, "", "OTHER.dat", MoveClash::kAsk, &summary);
    CHECK(error.status == 409 && summary.clash && !summary.clash_folder && summary.name == "other.dat");
    CHECK(Text(folder / "slot1.dat") == "new" && Text(folder / "other.dat") == "other");

    // The same name: nothing to do.
    CHECK(store.Move(root, "slot1.dat", root, "", "slot1.dat", MoveClash::kAsk, &summary).ok());
    CHECK(Text(folder / "slot1.dat") == "new");

    // A new case for the same name, which a drive that ignores case takes for the old one: through a temporary.
    CHECK(store.Move(root, "slot1.dat", root, "", "Slot1.dat", MoveClash::kAsk, &summary).ok());
    CHECK((Names(folder) == std::vector<std::string>{".playden-bak.Slot1.dat", "Slot1.dat", "other.dat"}));
    fs::create_directories(folder / "saves");
    Put(folder / "saves/a.sav", "a");
    CHECK(store.Move(root, "saves", root, "", "Saves", MoveClash::kAsk, &summary).ok() && summary.folders == 1);
    CHECK(Has(folder, "Saves") && Text(folder / "Saves/a.sav") == "a");

    // Stopped between its two steps, the sweep finishes it once the temporary is old.
    fail_at = "rename-back";
    CHECK(!store.Move(root, "other.dat", root, "", "Other.dat", MoveClash::kAsk, &summary).ok());
    fail_at.clear();
    CHECK(!Has(folder, "other.dat") && !Has(folder, "Other.dat") && !NoTemporaries(folder));
    now += 7200;
    FileListing swept;
    CHECK(store.List(root, "", &swept).ok());
    CHECK(Has(folder, "Other.dat") && Text(folder / "Other.dat") == "other" && NoTemporaries(folder));
    // A folder's too, and it is never deleted.
    fail_at = "rename-back";
    CHECK(!store.Move(root, "Saves", root, "", "SAVES", MoveClash::kAsk, &summary).ok());
    fail_at.clear();
    now += 7200;
    CHECK(store.List(root, "", &swept).ok());
    CHECK(Has(folder, "SAVES") && Text(folder / "SAVES/a.sav") == "a" && NoTemporaries(folder));

    // Bad names and paths.
    CHECK(store.Move(root, "Slot1.dat", root, "", "a/b", MoveClash::kAsk, &summary).status == 400);
    CHECK(store.Move(root, "Slot1.dat", root, "", ".playden-x", MoveClash::kAsk, &summary).status == 400);
    CHECK(store.Move(root, "missing.dat", root, "", "x.dat", MoveClash::kAsk, &summary).status == 404);
    CHECK(store.Move(root, "../rename/Slot1.dat", root, "", "x.dat", MoveClash::kAsk, &summary).status == 404);
    CHECK(store.Move(root, "Slot1.dat", root, "..", "x.dat", MoveClash::kAsk, &summary).status == 404);
  }

  // Moves in one root: a folder goes whole; Replace keeps the file there as the previous version; Keep both numbers.
  {
    const auto folder = base / "one-root";
    const auto root = Root(folder);
    Put(folder / "Saves/slot.sav", "moved");
    Put(folder / "Saves/.playden-bak.slot.sav", "moved-old");
    Put(folder / "Saves/deep/x.sav", "x");
    Put(folder / "Saves/playden_online.txt", "token");
    Put(folder / "Backup/slot.sav", "there");
    Put(folder / "Backup/.playden-bak.slot.sav", "there-old");
    Put(folder / "Backup/keep.sav", "keep");
    Put(folder / "Elsewhere/readme.txt", "hi");

    // Into itself.
    CHECK(store.Move(root, "Saves", root, "Saves/deep", "Saves", MoveClash::kAsk, &summary).status == 400);
    CHECK(store.Move(root, "Saves", root, "saves", "x", MoveClash::kAsk, &summary).status == 400);

    // A file onto a file: Replace.
    CHECK(store.Move(root, "Saves/slot.sav", root, "Backup", "slot.sav", MoveClash::kAsk, &summary).status == 409 && summary.clash);
    CHECK(store.Move(root, "Saves/slot.sav", root, "Backup", "slot.sav", MoveClash::kReplace, &summary).ok());
    CHECK(Text(folder / "Backup/slot.sav") == "moved" && Text(folder / "Backup/.playden-bak.slot.sav") == "there");
    CHECK(!Has(folder / "Saves", "slot.sav") && !Has(folder / "Saves", ".playden-bak.slot.sav"));

    // Keep both: a numbered name, before the extension; a folder's after its whole name.
    Put(folder / "Saves/keep.sav", "second");
    CHECK(store.Move(root, "Saves/keep.sav", root, "Backup", "keep.sav", MoveClash::kKeepBoth, &summary).ok());
    CHECK(summary.name == "keep (2).sav" && Text(folder / "Backup/keep (2).sav") == "second" && Text(folder / "Backup/keep.sav") == "keep");
    fs::create_directories(folder / "Backup/Saves.v1");
    CHECK(store.Move(root, "Elsewhere", root, "Backup", "Saves.v1", MoveClash::kKeepBoth, &summary).ok());
    CHECK(summary.name == "Saves.v1 (2)" && Text(folder / "Backup/Saves.v1 (2)/readme.txt") == "hi");

    // A file and a folder of the same name: neither replaces the other.
    Put(folder / "Backup/deep", "a file");
    CHECK(store.Move(root, "Saves/deep", root, "Backup", "deep", MoveClash::kReplace, &summary).status == 409);
    fs::remove(folder / "Backup/deep");

    // A folder goes whole, with what Playden keeps in it.
    CHECK(store.Move(root, "Saves", root, "Backup", "Saves", MoveClash::kAsk, &summary).ok() && summary.folders == 1);
    CHECK(!fs::exists(folder / "Saves") && Text(folder / "Backup/Saves/deep/x.sav") == "x" && Text(folder / "Backup/Saves/playden_online.txt") == "token");

    // Replace on a folder merges: files there with other names stay, the same names are replaced.
    Put(folder / "Merge/deep/x.sav", "newer");
    Put(folder / "Merge/deep/y.sav", "y");
    Put(folder / "Merge/new/z.sav", "z");
    CHECK(store.Move(root, "Merge", root, "Backup", "saves", MoveClash::kAsk, &summary).status == 409 && summary.clash_folder && summary.name == "Saves");
    CHECK(store.Move(root, "Merge", root, "Backup", "saves", MoveClash::kReplace, &summary).ok());
    CHECK(summary.name == "Saves" && summary.files == 2 && summary.left == 0);
    CHECK(Text(folder / "Backup/Saves/deep/x.sav") == "newer" && Text(folder / "Backup/Saves/deep/.playden-bak.x.sav") == "x");
    CHECK(Text(folder / "Backup/Saves/deep/y.sav") == "y" && Text(folder / "Backup/Saves/new/z.sav") == "z");
    CHECK(!fs::exists(folder / "Merge"));

    // A merge into the folder that holds the moved one.
    Put(folder / "Outer/Outer/a.sav", "a");
    CHECK(store.Move(root, "Outer/Outer", root, "", "Outer", MoveClash::kReplace, &summary).status == 409);
    CHECK(Text(folder / "Outer/Outer/a.sav") == "a");
  }

  // Read-only and private paths: never moved, renamed or chosen.
  {
    const auto folder = base / "guarded";
    auto root = Root(folder);
    root.private_paths = {"AppData/Local/Steam"};
    Put(folder / "AppData/Local/Steam/local.vdf", "secret");
    Put(folder / "AppData/Local/Game/save.sav", "save");
    Put(folder / "steam_settings/configs.user.ini", "name");
    Put(folder / "Docs/a.txt", "a");
    CHECK(store.Move(root, "AppData", root, "Docs", "AppData", MoveClash::kAsk, &summary).status == 403);
    CHECK(store.Move(root, "AppData/Local", root, "", "Local", MoveClash::kAsk, &summary).status == 403);
    CHECK(store.Move(root, "AppData/Local/Steam", root, "", "Steam", MoveClash::kAsk, &summary).status == 404);
    CHECK(store.Move(root, "Docs/a.txt", root, "AppData/Local/Steam", "a.txt", MoveClash::kAsk, &summary).status == 404);
    CHECK(store.Move(root, "Docs", root, "AppData/Local", "Steam", MoveClash::kAsk, &summary).status == 403);
    CHECK(store.Move(root, "Docs", root, "", "AppData", MoveClash::kReplace, &summary).status == 403);
    CHECK(store.Move(root, "steam_settings", root, "", "settings", MoveClash::kAsk, &summary).status == 403);
    CHECK(store.Move(root, "steam_settings/configs.user.ini", root, "", "c.ini", MoveClash::kAsk, &summary).status == 403);
    CHECK(store.Move(root, "Docs/a.txt", root, "steam_settings", "a.txt", MoveClash::kAsk, &summary).status == 403);
    CHECK(store.Move(root, "Docs", root, "", "STEAM_SETTINGS", MoveClash::kAsk, &summary).status == 403);
    CHECK(Text(folder / "AppData/Local/Steam/local.vdf") == "secret" && Text(folder / "Docs/a.txt") == "a");
    // A folder below can move.
    CHECK(store.Move(root, "AppData/Local/Game", root, "Docs", "Game", MoveClash::kAsk, &summary).ok());
    CHECK(Text(folder / "Docs/Game/save.sav") == "save");
  }

  // Across roots: each file copied whole with the destination's modes, then the source goes. What Playden keeps stays.
  SetOwnershipProbeForTesting([](const fs::path&) -> std::optional<bool> { return true; });
  {
    const auto bottle = base / "across/user";
    const auto game = base / "across/game";
    const auto from = Root(bottle);
    auto to = Root(game, 0777, 0666);
    to.game_folder = true;
    fs::create_directories(game);
    Put(bottle / "Saved/slot.sav", "slot");
    Put(bottle / "Saved/.playden-bak.slot.sav", "slot-old");
    Put(bottle / "Saved/sub/b.sav", "b");
    Put(bottle / "Saved/playden_online.txt", "token");
    Put(bottle / "Saved/steam_settings/x.ini", "x");
    Put(bottle / "Saved/.playden-bak.orphan.sav", "orphan");
    fs::create_symlink(base / "outside", bottle / "Saved/link");
    ::chmod((bottle / "Saved/slot.sav").c_str(), 0600);
    CHECK(store.Move(from, "Saved", to, "", "Saved", MoveClash::kAsk, &summary).ok());
    CHECK(summary.files == 2 && summary.left == 4);
    CHECK(Text(game / "Saved/slot.sav") == "slot" && Text(game / "Saved/.playden-bak.slot.sav") == "slot-old" && Text(game / "Saved/sub/b.sav") == "b");
    CHECK(Mode(game / "Saved/slot.sav") == 0666 && Mode(game / "Saved") == 0777 && Mode(game / "Saved/sub") == 0777);
    CHECK(!fs::exists(game / "Saved/playden_online.txt") && !fs::exists(game / "Saved/steam_settings") && !fs::exists(game / "Saved/.playden-bak.orphan.sav"));
    // What stays, with the folder that holds it.
    CHECK((Names(bottle / "Saved") == std::vector<std::string>{".playden-bak.orphan.sav", "link", "playden_online.txt", "steam_settings"}));
    CHECK(NoTemporaries(game) && NoTemporaries(bottle));

    // A single file, and Replace across roots: the file there is kept as the previous version.
    Put(bottle / "one.sav", "one");
    Put(game / "one.sav", "game-one");
    CHECK(store.Move(from, "one.sav", to, "", "one.sav", MoveClash::kAsk, &summary).status == 409);
    CHECK(store.Move(from, "one.sav", to, "", "one.sav", MoveClash::kReplace, &summary).ok() && summary.files == 1);
    CHECK(Text(game / "one.sav") == "one" && Text(game / ".playden-bak.one.sav") == "game-one" && !fs::exists(bottle / "one.sav"));

    // Back again, into a root that isn't made yet: it is made.
    const auto later = Root(base / "across/later");
    auto made_from = later;
    made_from.create_from = (base / "across").string();
    CHECK(store.Move(to, "Saved/sub", made_from, "", "sub", MoveClash::kAsk, &summary).ok());
    CHECK(Text(base / "across/later/sub/b.sav") == "b" && !fs::exists(game / "Saved/sub") && Mode(base / "across/later/sub/b.sav") == 0600);

    // A stop at any step leaves each file whole in one place or both, and nothing half-written in view.
    for (const char* step : {"copy-sync", "publish", "sync-publish", "copy-previous", "move-delete-previous", "move-delete-source"}) {
      const auto src = base / ("stop-" + std::string(step)) / "src";
      const auto dst = base / ("stop-" + std::string(step)) / "dst";
      fs::create_directories(dst);
      Put(src / "F/a.sav", "aaaa");
      Put(src / "F/.playden-bak.a.sav", "old");
      fail_at = step;
      const auto error = store.Move(Root(src), "F", Root(dst), "", "F", MoveClash::kAsk, &summary);
      fail_at.clear();
      CHECK(!error.ok());
      const bool source_whole = Text(src / "F/a.sav") == "aaaa";
      const bool target_whole = fs::exists(dst / "F/a.sav") && Text(dst / "F/a.sav") == "aaaa";
      CHECK(source_whole || target_whole);
      if (fs::exists(dst / "F/a.sav")) CHECK(target_whole);
      FileListing listing;
      CHECK(store.List(Root(dst), "F", &listing).ok());
      for (const auto& entry : listing.entries) CHECK(entry.name == "a.sav");
      CHECK(NoTemporaries(dst) || std::string(step) == "sync-publish");
      // A second run finishes what is left.
      if (source_whole && target_whole) continue;
      CHECK(store.Move(Root(src), "F", Root(dst), "", "F", MoveClash::kReplace, &summary).ok());
      CHECK(!fs::exists(src / "F") && Text(dst / "F/a.sav") == "aaaa");
      // The previous version went first, so a second run keeps it.
      CHECK(Text(dst / "F/.playden-bak.a.sav") == "old");
    }

    // Progress: totals first, then bytes per chunk and files as each is done. A false answer stops the move within
    // a chunk: the file being copied stays whole where it was, with no temporary left, and a second run finishes it.
    {
      const auto src = base / "progress/src";
      const auto dst = base / "progress/dst";
      fs::create_directories(dst);
      const std::string big(2 * 1024 * 1024 + 5, 'b');
      Put(src / "P/big.bin", big);
      Put(src / "P/.playden-bak.big.bin", "old");
      Put(src / "P/z.sav", "z");
      std::vector<MoveProgress> seen;
      const MoveObserver watch = [&](const MoveProgress& progress) {
        seen.push_back(progress);
        return true;
      };
      CHECK(store.Move(Root(src), "P", Root(dst), "", "P", MoveClash::kAsk, &summary, watch).ok() && summary.files == 2);
      CHECK(!seen.empty() && seen.front().total_files == 2 && seen.front().total_bytes == big.size() + 3 + 1 && seen.front().bytes == 0);
      CHECK(seen.back().bytes == big.size() + 3 + 1);
      bool chunked = false;
      for (const auto& progress : seen) chunked = chunked || (progress.name == "big.bin" && progress.bytes > 3 && progress.bytes < big.size() + 3);
      CHECK(chunked);
      // A single file counts its previous version in the total too.
      Put(src / "one.bin", big);
      Put(src / ".playden-bak.one.bin", "old");
      seen.clear();
      CHECK(store.Move(Root(src), "one.bin", Root(dst), "", "one.bin", MoveClash::kAsk, &summary, watch).ok());
      CHECK(seen.front().total_files == 1 && seen.front().total_bytes == big.size() + 3 && seen.back().files == 1);
      // Stopped after the first chunk of the big file.
      Put(src / "S/big.bin", big);
      const MoveObserver stop = [&](const MoveProgress& progress) { return progress.bytes < 1024 * 1024; };
      const auto stopped = store.Move(Root(src), "S", Root(dst), "", "S", MoveClash::kAsk, &summary, stop);
      CHECK(!stopped.ok() && Text(src / "S/big.bin") == big && !fs::exists(dst / "S/big.bin") && NoTemporaries(dst));
      CHECK(store.Move(Root(src), "S", Root(dst), "", "S", MoveClash::kReplace, &summary).ok() && Text(dst / "S/big.bin") == big);
      // A stop after the last file, with only a folder left: the move doesn't claim it finished.
      Put(src / "T/a.sav", "a");
      fs::create_directories(src / "T/empty");
      const MoveObserver after_file = [](const MoveProgress& progress) { return progress.files == 0; };
      CHECK(!store.Move(Root(src), "T", Root(dst), "", "T", MoveClash::kAsk, &summary, after_file).ok());
      CHECK(Text(dst / "T/a.sav") == "a" && fs::exists(src / "T/empty"));
      // One root: a rename, nothing to watch.
      seen.clear();
      CHECK(store.Move(Root(dst), "S", Root(dst), "", "S2", MoveClash::kAsk, &summary, watch).ok() && seen.empty());
    }

    // A source that changed while it was copied stays where it was.
    {
      const auto src = base / "changed/src";
      const auto dst = base / "changed/dst";
      fs::create_directories(dst);
      Put(src / "a.sav", "aaaa");
      options.fault = [&](const std::string& step) {
        if (step == "move-delete-source") return true;
        if (step == "publish") Put(src / "a.sav", "bbbbbb");
        return true;
      };
      FileStore changing(options);
      CHECK(changing.Move(Root(src), "a.sav", Root(dst), "", "a.sav", MoveClash::kAsk, &summary).status == 409);
      CHECK(Text(src / "a.sav") == "bbbbbb" && Text(dst / "a.sav") == "aaaa");
      // The same size, in the same second: only the time's nanoseconds differ.
      Put(src / "b.sav", "aaaa");
      options.fault = [&](const std::string& step) {
        if (step != "publish") return true;
        struct stat before{};
        CHECK(::stat((src / "b.sav").c_str(), &before) == 0);
        Put(src / "b.sav", "cccc");
        struct timespec times[2] = {before.st_atimespec, before.st_mtimespec};
        times[1].tv_nsec = before.st_mtimespec.tv_nsec == 0 ? 1 : before.st_mtimespec.tv_nsec - 1;
        CHECK(::utimensat(AT_FDCWD, (src / "b.sav").c_str(), times, 0) == 0);
        return true;
      };
      FileStore same_second(options);
      CHECK(same_second.Move(Root(src), "b.sav", Root(dst), "", "b.sav", MoveClash::kAsk, &summary).status == 409);
      CHECK(Text(src / "b.sav") == "cccc" && Text(dst / "b.sav") == "aaaa");
    }

    // A previous version that can't be read stops the move before anything is copied, and both stay.
    {
      const auto src = base / "unreadable/src";
      const auto dst = base / "unreadable/dst";
      fs::create_directories(dst);
      Put(src / "a.sav", "aaaa");
      Put(src / ".playden-bak.a.sav", "old");
      ::chmod((src / ".playden-bak.a.sav").c_str(), 0);
      CHECK(!store.Move(Root(src), "a.sav", Root(dst), "", "a.sav", MoveClash::kAsk, &summary).ok());
      ::chmod((src / ".playden-bak.a.sav").c_str(), 0600);
      CHECK(Text(src / "a.sav") == "aaaa" && Text(src / ".playden-bak.a.sav") == "old" && Names(dst).empty());
    }
  }
  SetOwnershipProbeForTesting(nullptr);

  // The space check comes before any byte.
  {
    const auto src = base / "space/src";
    const auto dst = base / "space/dst";
    fs::create_directories(dst);
    Put(src / "a.sav", "aaaa");
    FileStore::Options full = options;
    full.fault = nullptr;
    full.free_space = [](const std::string&) -> std::optional<std::uint64_t> { return 1024; };
    FileStore tight(full);
    CHECK(tight.Move(Root(src), "a.sav", Root(dst), "", "a.sav", MoveClash::kAsk, &summary).status == 507);
    CHECK(Text(src / "a.sav") == "aaaa" && Names(dst).empty());
  }

  fs::remove_all(base);
  std::puts("web_file_move: ok");
  return 0;
}
