// SPDX-License-Identifier: GPL-3.0-or-later
// Uninstall Playden (#868): RemovePlaydenFiles removes every root Playden writes to, keeps the saves when asked and
// the player's own files, follows no link, touches nothing outside its roots, and finishes after a stop. The README's
// "Where Playden keeps data" table lists exactly platform/data_roots.h's roots.
#include "platform/data_roots.h"
#include "runtime/playden_removal.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x); std::exit(1); } } while (0)

namespace fs = std::filesystem;
using namespace playden::runtime;
namespace roots = playden::platform::data_roots;

namespace {
void Write(const fs::path& path, const std::string& text = "x", mode_t mode = 0666) {
  fs::create_directories(path.parent_path());
  std::ofstream(path) << text;
  CHECK(::chmod(path.c_str(), mode) == 0);
}

struct Tree {
  fs::path root;
  PlaydenRemovalPaths paths;
  Tree() {
    char name[] = "/tmp/playden-removal-XXXXXX";
    CHECK(::mkdtemp(name));
    root = fs::canonical(name);
    paths = ConsoleRemovalPaths(root);
    const auto data = paths.data_root;
    // What Playden 0.11 and this version leave: private and open folders, root's folders (the kit).
    Write(data / "runtime-state/installs.json", "{}", 0600);
    CHECK(::chmod((data / "runtime-state").c_str(), 0700) == 0);
    Write(data / "user/session.v1", "token", 0600);
    Write(data / "user/game-settings-private/game-settings.json");
    Write(data / "art/cover.png");
    Write(data / "logs/playden.log");
    Write(data / "kits/playden-kit/manifest.json", "{}", 0644);
    Write(data / "title-rule/allow-list", "FAKE78001\n", 0644);
    Write(data / "games/steam-220/library/prefixes/game/user.reg", "", 0600);
    Write(data / "games/steam-10.kept/library/prefixes/game/drive_c/users/steamuser/save.dat", "save");
    Write(data / "games/steam-10.saves/1760000000/users/steamuser/save.dat", "backup");
    Write(paths.games_files_root / "220/hl2.exe");
    Write(paths.games_files_root / ".playden-saves/220/x");
    Write(root / "mnt/usb0/playden/games/400/game.exe");
    Write(root / "mnt/usb0/playden/import/My Game/game.exe");  // the player's
    fs::create_directories(root / "mnt/ext1/playden/import");     // empty: goes
    Write(root / "mnt/ext1/playden/games/500/game.exe");
    Write(paths.app_root / "eboot.bin", "app", 0755);
    Write(paths.app_root / "playden-data/MOVED.txt");
    Write(paths.app_root / "logs/MOVED.txt");
    // Outside every root: never touched, not even through a link planted in a root.
    Write(root / "data/homebrew/OTHER0001/eboot.bin");
    Write(root / "data/other-app/settings.json");
    fs::create_directory_symlink(root / "data/other-app", data / "user/linked-folder");
    fs::create_symlink(root / "data/homebrew/OTHER0001/eboot.bin", paths.games_files_root / "220/linked.exe");
  }
  ~Tree() { std::error_code ignored; fs::remove_all(root, ignored); }
  void Untouched() const {
    CHECK(fs::exists(root / "data/homebrew/OTHER0001/eboot.bin"));
    CHECK(fs::exists(root / "data/other-app/settings.json"));
    CHECK(fs::exists(paths.app_root / "eboot.bin"));
  }
};

void RemovesEverythingButSaves(bool keep_saves) {
  Tree tree;
  std::vector<std::string> lines;
  unsigned raises = 0;
  const RemovalAsRoot as_root = [&raises](const std::function<bool()>& work) { ++raises; return work(); };
  CHECK(SaveRemovalJournal(tree.paths.data_root, {RemovalStep::kFiles, keep_saves}));
  CHECK(RemovalStarted(tree.paths.data_root));
  const auto result = RemovePlaydenFiles(tree.paths, keep_saves, as_root, [&lines](const std::string& l) { lines.push_back(l); });
  for (const auto& failure : result.failed) std::fprintf(stderr, "failed: %s\n", failure.c_str());
  CHECK(result.complete && result.failed.empty());
  tree.Untouched();
  CHECK(!fs::exists(tree.paths.games_files_root));
  CHECK(!fs::exists(tree.root / "mnt/usb0/playden/games") && !fs::exists(tree.root / "mnt/ext1/playden"));
  CHECK(fs::exists(tree.root / "mnt/usb0/playden/import/My Game/game.exe"));  // the player's
  CHECK(!fs::exists(tree.paths.import_root));  // there was none
  CHECK(!fs::exists(tree.paths.app_root / "playden-data") && !fs::exists(tree.paths.app_root / "logs"));
  CHECK(!RemovalStarted(tree.paths.data_root));
  const auto kept = tree.paths.data_root / "games/steam-10.kept/library/prefixes/game/drive_c/users/steamuser/save.dat";
  if (keep_saves) {
    CHECK(fs::exists(kept));
    std::set<std::string> left;
    for (const auto& entry : fs::recursive_directory_iterator(tree.paths.data_root))
      if (entry.is_regular_file()) left.insert(entry.path().lexically_relative(tree.paths.data_root).string());
    CHECK(left == std::set<std::string>{"games/steam-10.kept/library/prefixes/game/drive_c/users/steamuser/save.dat"});
  } else {
    CHECK(!fs::exists(tree.paths.data_root));
  }
}

// Stopped part way (the console went off): the next run finishes it, and a run with nothing left is fine.
void FinishesAfterAStop() {
  Tree tree;
  CHECK(SaveRemovalJournal(tree.paths.data_root, {RemovalStep::kFiles, false}));
  std::error_code ignored;
  fs::remove_all(tree.paths.games_files_root, ignored);
  fs::remove_all(tree.paths.data_root / "kits", ignored);
  const auto journal = LoadRemovalJournal(tree.paths.data_root);
  CHECK(journal && journal->step == RemovalStep::kFiles && !journal->keep_saves);
  CHECK(RemovePlaydenFiles(tree.paths, journal->keep_saves, {}, {}).complete);
  CHECK(!fs::exists(tree.paths.data_root));
  CHECK(RemovePlaydenFiles(tree.paths, false, {}, {}).complete);
  tree.Untouched();
}

// What refuses as the app user is tried again as root; what still refuses is named, and the journal stays so the
// next start tries again.
void ReportsWhatStays() {
  if (::geteuid() == 0) return;  // root has no refusal to show here
  Tree tree;
  CHECK(SaveRemovalJournal(tree.paths.data_root, {RemovalStep::kFiles, false}));
  CHECK(::chmod((tree.paths.data_root / "art").c_str(), 0500) == 0);
  const auto result = RemovePlaydenFiles(tree.paths, false, [](const std::function<bool()>&) { return false; }, {});
  CHECK(!result.complete && !result.failed.empty());
  CHECK(result.failed[0].find("art") != std::string::npos);
  CHECK(RemovalStarted(tree.paths.data_root));
  CHECK(::chmod((tree.paths.data_root / "art").c_str(), 0777) == 0);
  CHECK(RemovePlaydenFiles(tree.paths, false, {}, {}).complete && !fs::exists(tree.paths.data_root));
}

void JournalRoundTrip() {
  char name[] = "/tmp/playden-removal-journal-XXXXXX";
  CHECK(::mkdtemp(name));
  const fs::path root = fs::canonical(name);
  CHECK(!LoadRemovalJournal(root) && !RemovalStarted(root));
  CHECK(SaveRemovalJournal(root, {RemovalStep::kTitles, false}));
  auto loaded = LoadRemovalJournal(root);
  CHECK(loaded && loaded->step == RemovalStep::kTitles && !loaded->keep_saves);
  struct stat info{};
  CHECK(::stat((root / roots::kUninstallJournal).c_str(), &info) == 0 && (info.st_mode & 0777) == 0666);
  CHECK(SaveRemovalJournal(root, {RemovalStep::kFiles, true}));
  loaded = LoadRemovalJournal(root);
  CHECK(loaded && loaded->step == RemovalStep::kFiles && loaded->keep_saves);
  // Damaged, or not a file: taken as a removal at its titles that keeps saves, which deletes no bottle.
  fs::remove(root / roots::kUninstallJournal);
  Write(root / roots::kUninstallJournal, "garbage");
  loaded = LoadRemovalJournal(root);
  CHECK(loaded && loaded->step == RemovalStep::kTitles && loaded->keep_saves);
  Write(root / roots::kUninstallJournal, "{\"schema\":1,\"step\":\"files\"");
  loaded = LoadRemovalJournal(root);
  CHECK(loaded && loaded->step == RemovalStep::kTitles && loaded->keep_saves);
  fs::remove(root / roots::kUninstallJournal);
  fs::create_directory(root / roots::kUninstallJournal);
  loaded = LoadRemovalJournal(root);
  CHECK(loaded && loaded->step == RemovalStep::kTitles && loaded->keep_saves && RemovalStarted(root));
  fs::remove_all(root);
}

// The README's table names every root in data_roots, and nothing else.
void ReadmeListsEveryRoot(const char* readme_path) {
  std::ifstream in(readme_path);
  CHECK(in.good());
  const std::string text{std::istreambuf_iterator<char>(in), {}};
  const auto start = text.find("## Where Playden keeps data");
  CHECK(start != std::string::npos);
  const auto end = text.find("\n## ", start + 1);
  const auto section = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
  std::set<std::string> listed;
  for (std::size_t at = 0; (at = section.find("\n| `", at)) != std::string::npos;) {
    at += 4;
    const auto close = section.find('`', at);
    CHECK(close != std::string::npos);
    listed.insert(section.substr(at, close - at));
  }
  std::set<std::string> wanted;
  for (const auto& root : roots::DataRoots()) wanted.insert(root.path.string());
  for (const auto& path : wanted) if (!listed.count(path)) std::fprintf(stderr, "README misses %s\n", path.c_str());
  for (const auto& path : listed) if (!wanted.count(path)) std::fprintf(stderr, "README lists %s, not in data_roots\n", path.c_str());
  CHECK(listed == wanted);
}

// A link in place of a folder Playden lists (games/, a drive's playden/, /data/playden itself) is never listed or
// entered: what it points at stays whole.
void LinkedFoldersAreNotEntered() {
  for (const bool keep_saves : {true, false}) {
    Tree tree;
    const auto elsewhere = tree.root / "data/elsewhere";
    Write(elsewhere / "steam-1/save.dat", "not Playden's");
    Write(elsewhere / "steam-2.kept/save.dat", "not Playden's either");
    std::error_code ignored;
    fs::remove_all(tree.paths.data_root / "games", ignored);
    fs::create_directory_symlink(elsewhere, tree.paths.data_root / "games");
    fs::remove_all(tree.root / "mnt/ext1/playden", ignored);
    fs::create_directory_symlink(elsewhere, tree.root / "mnt/ext1/playden");
    RemovePlaydenFiles(tree.paths, keep_saves, {}, {});
    CHECK(fs::exists(elsewhere / "steam-1/save.dat") && fs::exists(elsewhere / "steam-2.kept/save.dat"));
    tree.Untouched();
  }
  Tree tree;
  const auto elsewhere = tree.root / "data/elsewhere";
  Write(elsewhere / "file", "x");
  std::error_code ignored;
  fs::remove_all(tree.paths.data_root, ignored);
  fs::create_directory_symlink(elsewhere, tree.paths.data_root);
  const auto result = RemovePlaydenFiles(tree.paths, false, {}, {});
  CHECK(!result.complete && fs::exists(elsewhere / "file"));
}
}  // namespace

int main(int argc, char** argv) {
  JournalRoundTrip();
  RemovesEverythingButSaves(true);
  RemovesEverythingButSaves(false);
  FinishesAfterAStop();
  ReportsWhatStays();
  LinkedFoldersAreNotEntered();
  if (argc > 1) ReadmeListsEveryRoot(argv[1]);
  std::puts("playden removal: ok");
  return 0;
}
