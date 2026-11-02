#include "runtime/writable_folders.h"

#include <cstdio>
#include <cerrno>
#if PLAYDEN_PLATFORM_PS5
#endif
#include <cstdlib>
#include <filesystem>
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
bool Empty(const fs::path& path) { return fs::is_empty(path); }
}  // namespace

int main() {
  char name[] = "/tmp/playden-writable-XXXXXX";
  CHECK(mkdtemp(name));
  const fs::path root = fs::canonical(name);
  const auto open = root / "open", locked = root / "data/playden-games", target = root / "target", link = root / "link";
  fs::create_directories(open);
  fs::create_directories(locked);
  fs::create_directories(target);
  fs::create_directory_symlink(target, link);
  std::vector<std::string> lines;
  const auto log = [&lines](const std::string& line) { lines.push_back(line); };
  unsigned calls = 0;
  const WritableFoldersAsRoot runs = [&calls](const std::function<bool()>& work) { ++calls; return work(); };
  const WritableFoldersAsRoot refuses = [&calls](const std::function<bool()>&) { ++calls; return false; };

  // Writable, missing and linked folders need nothing; the probe leaves no file behind.
  CHECK(EnsureWritableFolders({open, root / "missing", link, {}}, runs, log, root).empty());
  CHECK(calls == 0 && lines.empty() && Empty(open));

  if (geteuid() != 0) {
    // A linked folder is never followed (nor fixed), even when its target can't be written.
    CHECK(chmod(target.c_str(), 0555) == 0);
    CHECK(EnsureWritableFolders({link}, runs, log, root).empty());
    CHECK(calls == 0 && Mode(target) == 0555);
    CHECK(chmod(target.c_str(), 0755) == 0);

    // O_NOFOLLOW on the final folder must also reject linked ancestors.
    fs::create_directories(root / "mnt/usb1");
    fs::create_directory_symlink(target, root / "mnt/usb1/playden");
    fs::create_directories(target / "games");
    CHECK(chmod((target / "games").c_str(), 0555) == 0);
    CHECK(!EnsureWritableFolders({root / "mnt/usb1/playden/games"}, runs, log, root).empty());
    CHECK(Mode(target / "games") == 0555);
    CHECK(chmod((target / "games").c_str(), 0755) == 0);
    lines.clear();
    calls = 0;

    // A link swapped in between the probe and the fix is refused, its target left alone.
    const auto swapped = root / "mnt/usb0/playden/games";
    fs::create_directories(swapped);
    CHECK(chmod(swapped.c_str(), 0555) == 0 && chmod(target.c_str(), 0555) == 0);
    const WritableFoldersAsRoot swaps = [&](const std::function<bool()>& work) {
      ++calls;
      CHECK(chmod(swapped.c_str(), 0755) == 0);
      fs::rename(swapped, root / "swapped-away");
      CHECK(chmod((root / "swapped-away").c_str(), 0555) == 0);
      fs::create_directory_symlink(target, swapped);
      return work();
    };
    CHECK(!EnsureWritableFolders({swapped}, swaps, log, root).empty());
    CHECK(calls == 1 && Mode(target) == 0555 && Mode(root / "swapped-away") == 0555);
    CHECK(lines.size() == 1 && lines[0].find("chmod=") != std::string::npos);
    CHECK(chmod(target.c_str(), 0755) == 0 && chmod((root / "swapped-away").c_str(), 0755) == 0);
    lines.clear();
    calls = 0;

    // No way to raise root: the folder is named.
    CHECK(chmod(locked.c_str(), 0555) == 0);
    auto message = EnsureWritableFolders({open, locked}, {}, log, root);
    CHECK(message.find("can't write to " + locked.string() + ".") != std::string::npos);
    CHECK(Mode(locked) == 0555 && lines.size() == 1 && lines[0].find("fix=unavailable") != std::string::npos);

    // Root refused.
    lines.clear();
    message = EnsureWritableFolders({locked}, refuses, log, root);
    CHECK(calls == 1 && message.find(locked.string()) != std::string::npos && Mode(locked) == 0555);
    CHECK(lines.size() == 1 && lines[0].find("fix=root-refused") != std::string::npos);

    // The fix ran but the folder still can't be written.
    lines.clear();
    calls = 0;
    const WritableFoldersAsRoot undone = [&](const std::function<bool()>& work) {
      ++calls;
      const bool ok = work();
      chmod(locked.c_str(), 0555);
      return ok;
    };
    message = EnsureWritableFolders({locked}, undone, log, root);
    CHECK(calls == 1 && message.find(locked.string()) != std::string::npos);
    CHECK(lines.size() == 1 && lines[0].find("after-fix=") != std::string::npos);

    // Fixed: 0777, logged, then writable without another raise.
    lines.clear();
    calls = 0;
    CHECK(EnsureWritableFolders({open, locked}, runs, log, root).empty());
    CHECK(calls == 1 && Mode(locked) == 0777 && Empty(locked));
    CHECK(lines.size() == 1 && lines[0].find("writable folder fixed") != std::string::npos &&
          lines[0].find("mode=0555 -> 0777") != std::string::npos);
    CHECK(EnsureWritableFolders({locked}, runs, log, root).empty() && calls == 1);
  }

  // A fresh /data and an FTP-created 0755 runtime root both become sticky.
  CHECK(PrepareRuntimeRoot(root / "data/playden", runs, log, root).empty());
  CHECK(Mode(root / "data/playden") == 01777);
  CHECK(chmod((root / "data/playden").c_str(), 0755) == 0);
  CHECK(PrepareRuntimeRoot(root / "data/playden", runs, log, root).empty());
  CHECK(Mode(root / "data/playden") == 01777 && Mode(locked) == 0777);
  // A writable 0777 root is fine as it is: the PS5's /data drops the sticky bit.
  CHECK(chmod((root / "data/playden").c_str(), 0777) == 0);
  lines.clear();
  CHECK(EnsureWritableFolders({root / "data/playden"}, runs, log, root).empty());
  CHECK(Mode(root / "data/playden") == 0777 && lines.empty());
  CHECK(chmod(target.c_str(), 0555) == 0);
  const auto before_calls = calls;
  CHECK(EnsureWritableFolders({target}, runs, log, root).empty());
  CHECK(Mode(target) == 0777 && calls == before_calls);
  CHECK(chmod(target.c_str(), 0755) == 0);
  // The drop folder: made 0777 (not sticky) after the grant, an FTP-made 0755 one opened, a link refused.
  {
    const auto drop = root / "data/playden-import";
    const auto before = calls;
    CHECK(!PrepareDropFolder(root / "data/other", runs, log, root).empty() && calls == before);
    CHECK(PrepareDropFolder(drop, runs, log, root).empty());
    CHECK(fs::is_directory(drop) && Mode(drop) == 0777 && calls == before + 1);
    CHECK(chmod(drop.c_str(), 0755) == 0);
    CHECK(PrepareDropFolder(drop, runs, log, root).empty() && Mode(drop) == 0777);
    CHECK(PrepareDropFolder(drop, runs, log, root).empty() && Mode(drop) == 0777);
    CHECK(!PrepareDropFolder(drop, nullptr, log, root).empty());
    CHECK(!PrepareDropFolder(drop, refuses, log, root).empty());
    // An approved root: EnsureWritableFolders repairs it (and an import folder on a drive) to 0777.
    if (geteuid() != 0) {
      const auto drive_import = root / "mnt/usb0/playden/import";
      fs::create_directories(drive_import);
      CHECK(chmod(drop.c_str(), 0555) == 0 && chmod(drive_import.c_str(), 0555) == 0);
      CHECK(EnsureWritableFolders({drop, drive_import}, runs, log, root).empty());
      CHECK(Mode(drop) == 0777 && Mode(drive_import) == 0777);
    }
    CHECK(rmdir(drop.c_str()) == 0);
    CHECK(symlink(target.c_str(), drop.c_str()) == 0);
    CHECK(chmod(target.c_str(), 0755) == 0);
    CHECK(!PrepareDropFolder(drop, runs, log, root).empty() && Mode(target) == 0755 && fs::is_symlink(drop));
    CHECK(unlink(drop.c_str()) == 0);
  }
  // The app folder's stubs: missing ones made 0777, an existing one (it can hold old user data) kept, a link refused.
  {
    const auto app = root / "data/homebrew/FAKE77031";
    fs::create_directories(app);
    const auto before = calls;
    CHECK(!PrepareAppFolderStubs(root / "data/FAKE77031", runs, log, root).empty() && calls == before);
    CHECK(!PrepareAppFolderStubs(root / "data/homebrew", runs, log, root).empty() && calls == before);
    lines.clear();
    CHECK(PrepareAppFolderStubs(app, runs, log, root).empty() && calls == before + 1);
    CHECK(Mode(app / "playden-data") == 0777 && Mode(app / "logs") == 0777 && lines.size() == 2);
    CHECK(Empty(app / "playden-data") && Empty(app / "logs"));
    CHECK(fs::remove(app / "logs") && chmod((app / "playden-data").c_str(), 0755) == 0);
    CHECK(PrepareAppFolderStubs(app, runs, log, root).empty());
    CHECK(Mode(app / "playden-data") == 0755 && Mode(app / "logs") == 0777);
    CHECK(!PrepareAppFolderStubs(app, nullptr, log, root).empty());
    CHECK(!PrepareAppFolderStubs(app, refuses, log, root).empty());
    CHECK(fs::remove(app / "logs") && symlink(target.c_str(), (app / "logs").c_str()) == 0);
    const auto refused = PrepareAppFolderStubs(app, runs, log, root);
    CHECK(refused.find("/logs") != std::string::npos && Mode(target) == 0755 && fs::is_symlink(app / "logs"));
    CHECK(unlink((app / "logs").c_str()) == 0);
    // Missing app folder: nothing is made.
    CHECK(!PrepareAppFolderStubs(root / "data/homebrew/PPSA97031", runs, log, root).empty());
    CHECK(!fs::exists(root / "data/homebrew/PPSA97031"));
  }
  // The app's own folders go to 0777; missing ones, links and refused paths are left.
  {
    const auto app = root / "data/homebrew/FAKE77031";
    fs::create_directories(app / "assets");
    fs::create_directories(app / "sce_sys");
    CHECK(chmod((app / "assets").c_str(), 0755) == 0 && chmod((app / "sce_sys").c_str(), 0777) == 0);
    const auto link_target = root / "app-folder-link-target";
    fs::create_directories(link_target);
    CHECK(chmod(link_target.c_str(), 0755) == 0 && symlink(link_target.c_str(), (app / "trust").c_str()) == 0);
    const auto before = calls;
    lines.clear();
    CHECK(OpenAppFolders(app, runs, log, root).empty());
    CHECK(Mode(app / "assets") == 0777 && Mode(app / "sce_sys") == 0777 && Mode(link_target) == 0755);
    CHECK(!fs::exists(app / "runtime") && calls == before);
    CHECK(lines.size() == 1 && lines[0].find("/assets") != std::string::npos);
    CHECK(OpenAppFolders(app, nullptr, log, root).empty());
    CHECK(!OpenAppFolders(root / "data/FAKE77031", runs, log, root).empty());
    CHECK(!OpenAppFolders(root / "data/homebrew", runs, log, root).empty());
    CHECK(unlink((app / "trust").c_str()) == 0);
  }
  // /data swapped for a link to an outside folder: neither repair follows it as root.
  {
    const auto outside = root / "outside-data";
    CHECK(rename((root / "data").c_str(), outside.c_str()) == 0);
    CHECK(symlink(outside.c_str(), (root / "data").c_str()) == 0);
    CHECK(chmod((outside / "playden").c_str(), 0755) == 0 && chmod(locked.c_str(), 0555) == 0);
    CHECK(!PrepareRuntimeRoot(root / "data/playden", runs, log, root).empty());
    CHECK(Mode(outside / "playden") == 0755);
    CHECK(lines.back().find("open /data failed") != std::string::npos);
    CHECK(unlink((root / "data").c_str()) == 0 && rename(outside.c_str(), (root / "data").c_str()) == 0);
    CHECK(chmod(locked.c_str(), 0777) == 0);
  }
  std::error_code ignored;
  fs::remove_all(root, ignored);
  std::puts("writable folders tests passed");
  return 0;
}
