// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime/self_heal.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace playden::runtime;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x); std::exit(1); } } while (0)

namespace fs = std::filesystem;
namespace {
unsigned Mode(const fs::path& path) {
  struct stat info{};
  CHECK(lstat(path.c_str(), &info) == 0);
  return info.st_mode & 07777;
}
void Folder(const fs::path& path, mode_t mode) {
  fs::create_directories(path);
  CHECK(chmod(path.c_str(), mode) == 0);
}
void File(const fs::path& path, mode_t mode, const std::string& text = "x") {
  std::ofstream(path) << text;
  CHECK(chmod(path.c_str(), mode) == 0);
}
}  // namespace

int main() {
  char name[] = "/tmp/playden-self-heal-XXXXXX";
  CHECK(mkdtemp(name));
  const fs::path root = fs::canonical(name);
  std::vector<std::string> lines;
  const auto log = [&lines](const std::string& line) { lines.push_back(line); };

  // Nothing there yet (a first start): nothing to do, nothing failed.
  auto result = TightenPrivateModes(root / "data/playden", log);
  CHECK(result.tightened == 0 && result.failed == 0 && lines.empty());

  // What a recursive chmod 777 over FTP leaves behind.
  const auto data = root / "data/playden";
  const auto state = data / "runtime-state";
  Folder(data, 0777);
  Folder(state, 0777);
  File(state / "layout-0.6.json", 0777);
  File(state / "installs.json", 0777);
  File(state / "legacy.json", 0644);
  File(state / "read-only", 0400);
  Folder(state / "requests", 0777);
  File(state / "requests/settings-request-1.v1", 0777);
  Folder(state / "trash", 0777);
  Folder(state / "trash/FAKE00001-op", 0755);
  File(state / "trash/FAKE00001-op/eboot.bin", 0755);
  Folder(state / "unknown", 0777);
  File(state / "unknown/file", 0777);
  Folder(data / "user", 0777);
  File(data / "user/session.v1", 0777);
  File(data / "user/library-1.cache", 0666);  // an ordinary cache, written 0666
  Folder(data / "user/game-settings-private", 0777);
  File(data / "user/game-settings-private/game-settings.json", 0777);
  Folder(data / "art", 0777);
  File(data / "art/cover.png", 0777);
  Folder(data / "logs", 0777);
  File(data / "logs/playden.log", 0777);
  // A link in a private folder is never followed; a file with another link is left alone.
  File(root / "outside", 0777);
  fs::create_symlink(root / "outside", state / "link");
  File(root / "shared", 0777);
  fs::create_hard_link(root / "shared", state / "hard");

  result = TightenPrivateModes(data, log);
  CHECK(Mode(state) == 0700);
  CHECK(Mode(state / "layout-0.6.json") == 0600 && Mode(state / "installs.json") == 0600);
  CHECK(Mode(state / "legacy.json") == 0600);
  CHECK(Mode(state / "read-only") == 0400);  // never given bits it lacked
  CHECK(Mode(state / "requests") == 0700 && Mode(state / "requests/settings-request-1.v1") == 0600);
  CHECK(Mode(state / "trash") == 0700);
  CHECK(Mode(state / "trash/FAKE00001-op") == 0755 && Mode(state / "trash/FAKE00001-op/eboot.bin") == 0755);
  CHECK(Mode(state / "unknown") == 0777 && Mode(state / "unknown/file") == 0777);
  CHECK(Mode(data / "user") == 0777);  // shared
  CHECK(Mode(data / "user/session.v1") == 0600 && Mode(data / "user/library-1.cache") == 0666);
  CHECK(Mode(data / "user/game-settings-private") == 0700);
  CHECK(Mode(data / "user/game-settings-private/game-settings.json") == 0600);
  CHECK(Mode(data / "art") == 0700 && Mode(data / "art/cover.png") == 0777);
  CHECK(Mode(data / "logs") == 0777 && Mode(data / "logs/playden.log") == 0777);
  CHECK(Mode(root / "outside") == 0777 && Mode(root / "shared") == 0777);
  CHECK(result.tightened == 11);
  CHECK(result.foreign == 1 && result.failed == 0);
  CHECK(lines.size() == 12);

  // A second start finds nothing to change and says nothing.
  lines.clear();
  result = TightenPrivateModes(data, log);
  CHECK(result.tightened == 0 && result.failed == 0 && result.foreign == 1 && lines.size() == 1);

  // A bottle after a recursive 777: the title's private folders and files go back, drive_c stays.
  const auto bottle = data / "games/steam-274520";
  CHECK(!BottleTooOpen(bottle));  // missing
  for (const auto* folder : {"", "library", "library/prefixes", "library/prefixes/game/drive_c/users/prospero",
                             "library/profiles", "library/input", ".playden-control", "logs/sessions",
                             "title-metadata/sce_sys"})
    Folder(bottle / folder, 0777);
  for (const auto* folder : {"", "library", "library/prefixes", "library/prefixes/game", "library/prefixes/game/drive_c",
                             "library/prefixes/game/drive_c/users", "logs", "title-metadata"})
    CHECK(chmod((bottle / folder).c_str(), 0777) == 0);
  for (const auto* file : {".playden-control/publication-state.v1", ".playden-control/settings-operation.v1",
                           "library/profiles/profiles.lst", "library/input/pad.ini", "library/prefixes/game/user.reg",
                           "logs/sessions/one.json", "title-metadata/param.json", "title-metadata/sce_sys/icon0.png",
                           "library/prefixes/game/drive_c/users/prospero/save.dat"})
    File(bottle / file, 0777);
  CHECK(BottleTooOpen(bottle));
  if (geteuid() != 0) {
    // A part that can't be repaired keeps the bottle itself open, so the next Play comes back.
    CHECK(chmod((bottle / "logs").c_str(), 0000) == 0);
    result = TightenBottleModes(bottle, log);
    CHECK(result.failed == 2 && Mode(bottle) == 0777 && BottleTooOpen(bottle));  // logs and logs/sessions
    CHECK(Mode(bottle / ".playden-control/publication-state.v1") == 0600);
    CHECK(chmod((bottle / "logs").c_str(), 0777) == 0);
  }
  lines.clear();
  result = TightenBottleModes(bottle, log);
  for (const auto* folder : {"", "library", "library/prefixes", "library/prefixes/game", "library/profiles",
                             "library/input", ".playden-control", "logs", "logs/sessions", "title-metadata",
                             "title-metadata/sce_sys"})
    CHECK(Mode(bottle / folder) == 0700);
  for (const auto* file : {".playden-control/publication-state.v1", ".playden-control/settings-operation.v1",
                           "library/profiles/profiles.lst", "library/input/pad.ini", "library/prefixes/game/user.reg",
                           "logs/sessions/one.json", "title-metadata/param.json", "title-metadata/sce_sys/icon0.png"})
    CHECK(Mode(bottle / file) == 0600);
  CHECK(Mode(bottle / "library/prefixes/game/drive_c") == 0777);
  CHECK(Mode(bottle / "library/prefixes/game/drive_c/users/prospero/save.dat") == 0777);
  CHECK(result.failed == 0 && !BottleTooOpen(bottle));
  // A bottle a kit keeps traversable (0711) is left as it is.
  CHECK(chmod(bottle.c_str(), 0711) == 0 && chmod((bottle / "library").c_str(), 0711) == 0);
  CHECK(!BottleTooOpen(bottle));
  result = TightenBottleModes(bottle, log);
  CHECK(result.tightened == 0 && Mode(bottle) == 0711 && Mode(bottle / "library") == 0711);

  // The kit: extra bits go, missing bits are not added, links are not followed.
  const auto kit = data / "kits/playden-kit";
  Folder(kit / "bin", 0777);
  Folder(kit / "lib", 0777);
  File(kit / "bin/a.dll", 0777);
  File(kit / "bin/run", 0777);
  File(kit / "lib/short", 0400);
  File(kit / "lib/right", 0644);
  fs::create_symlink(root / "outside", kit / "lib/linked");
  File(kit / "manifest.json", 0644,
       R"({"files":[{"path":"bin/a.dll","size":1,"sha256":"","mode":"0644"},)"
       R"({"path":"bin/run","size":1,"sha256":"","mode":"0755"},)"
       R"({"path":"lib/linked","size":1,"sha256":"","mode":"0644"},)"
       R"({"path":"lib/right","size":1,"sha256":"","mode":"0644"},)"
       R"({"path":"lib/short","size":1,"sha256":"","mode":"0644"},)"
       R"({"path":"../outside","size":1,"sha256":"","mode":"0600"}]})");
  unsigned calls = 0;
  const WritableFoldersAsRoot runs = [&calls](const std::function<bool()>& work) { ++calls; return work(); };
  const WritableFoldersAsRoot refuses = [&calls](const std::function<bool()>&) { ++calls; return false; };

  lines.clear();
  result = TightenKitModes(kit, refuses, log);
  CHECK(calls == 1 && result.tightened == 0 && result.failed == 2 && Mode(kit / "bin/a.dll") == 0777);
  result = TightenKitModes(kit, {}, log);
  CHECK(result.failed == 2 && Mode(kit / "bin/a.dll") == 0777);

  lines.clear();
  result = TightenKitModes(kit, runs, log);
  CHECK(calls == 2 && result.tightened == 2 && result.failed == 0 && lines.size() == 1);  // one summary line
  CHECK(Mode(kit / "bin/a.dll") == 0644 && Mode(kit / "bin/run") == 0755);
  CHECK(Mode(kit / "lib/short") == 0400 && Mode(kit / "lib/right") == 0644);
  CHECK(Mode(root / "outside") == 0777);

  // A kit with its modes is not entered as root.
  result = TightenKitModes(kit, runs, log);
  CHECK(calls == 2 && result.tightened == 0 && result.failed == 0);
  // No kit, or a broken manifest: nothing to do.
  CHECK(TightenKitModes(data / "kits/missing", runs, log).tightened == 0);
  File(kit / "manifest.json", 0644, "{not json");
  CHECK(TightenKitModes(kit, runs, log).failed == 0 && calls == 2);

  fs::remove_all(root);
  std::puts("runtime_self_heal ok");
  return 0;
}
