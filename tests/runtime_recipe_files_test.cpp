// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime/recipe_files.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(condition)                                                                  \
  do {                                                                                    \
    if (!(condition)) {                                                                   \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
      std::exit(1);                                                                       \
    }                                                                                     \
  } while (false)

namespace fs = std::filesystem;
using namespace playden::runtime;

namespace {
void Write(const fs::path& path, const std::string& bytes) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << bytes;
}
std::string Read(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
mode_t Mode(const fs::path& path) {
  struct stat info {};
  CHECK(::lstat(path.c_str(), &info) == 0);
  return info.st_mode & 07777;
}
}  // namespace

int main() {
  char pattern[] = "/private/tmp/playden-recipe-files-XXXXXX";
  const fs::path base = ::mkdtemp(pattern);
  const auto source = base / "source";
  const std::string relative = "zombie/DebugFileWatcher.class";
  Write(source / relative, "ours");

  // Missing folders are made, open to FTP like the rest of the game's files, whatever the umask.
  {
    const auto game = base / "new";
    fs::create_directories(game);
    const auto mask = ::umask(022);
    const auto result = ApplyRecipeFiles(game.string(), source.string(), {relative});
    ::umask(mask);
    CHECK(result.ok && result.changed == std::vector<std::string>{relative} && result.skipped.empty());
    CHECK(Read(game / relative) == "ours");
    CHECK(Mode(game / "zombie") == 0777 && Mode(game / relative) == 0666);
    CHECK(!fs::exists(game / "zombie/.playden-DebugFileWatcher.class.tmp"));
    // A second run has nothing to do and nothing to report.
    const auto again = ApplyRecipeFiles(game.string(), source.string(), {relative});
    CHECK(again.ok && again.changed.empty() && again.skipped.empty());
  }
  // A file already there that isn't Playden's stays: a game version that ships its own must keep it.
  {
    const auto game = base / "own";
    Write(game / relative, "the game's");
    const auto result = ApplyRecipeFiles(game.string(), source.string(), {relative});
    CHECK(result.ok && result.changed.empty() && result.skipped.size() == 1);
    CHECK(Read(game / relative) == "the game's");
  }
  // Links are not followed, as the file or as a folder on the way to it.
  {
    const auto game = base / "file-link";
    const auto outside = base / "outside-file";
    Write(outside, "outside");
    fs::create_directories(game / "zombie");
    fs::create_symlink(outside, game / relative);
    const auto result = ApplyRecipeFiles(game.string(), source.string(), {relative});
    CHECK(result.ok && result.changed.empty() && result.skipped.size() == 1 && Read(outside) == "outside");
  }
  {
    const auto game = base / "folder-link";
    const auto outside = base / "outside-folder";
    fs::create_directories(game);
    fs::create_directories(outside);
    fs::create_directory_symlink(outside, game / "zombie");
    const auto result = ApplyRecipeFiles(game.string(), source.string(), {relative});
    CHECK(result.ok && result.changed.empty() && result.skipped.size() == 1);
    CHECK(!fs::exists(outside / "DebugFileWatcher.class"));
  }
  // A file in the way of the folder stays too.
  {
    const auto game = base / "file-in-the-way";
    Write(game / "zombie", "a file");
    const auto result = ApplyRecipeFiles(game.string(), source.string(), {relative});
    CHECK(result.ok && result.changed.empty() && result.skipped.size() == 1 && Read(game / "zombie") == "a file");
  }
  // A package without the source starts the game as it would have, with no empty folder left behind.
  {
    const auto game = base / "no-source";
    fs::create_directories(game);
    const auto result = ApplyRecipeFiles(game.string(), (base / "nothing").string(), {relative});
    CHECK(result.ok && result.changed.empty() && result.skipped.size() == 1 && !fs::exists(game / "zombie"));
  }
  // A folder that can't be written to fails with a sentence for the player. Root writes anyway.
  if (::geteuid() != 0) {
    const auto game = base / "read-only";
    fs::create_directories(game);
    CHECK(::chmod(game.c_str(), 0555) == 0);
    const auto result = ApplyRecipeFiles(game.string(), source.string(), {relative});
    CHECK(::chmod(game.c_str(), 0755) == 0);
    CHECK(!result.ok && result.message.find(relative) != std::string::npos);
  }
  // A file an earlier recipe placed is removed while it still has the bytes Playden shipped, and only then.
  {
    const std::string ours = "ae0bec9e9d33c56354bd612c8a6010840324fc5b9786ce0a18b696b304b09397";  // SHA-256 of "ours"
    const std::vector<std::pair<std::string, std::string>> retired{{relative, ours}};
    const auto game = base / "retired";
    Write(game / relative, "ours");
    const auto result = RetireRecipeFiles(game.string(), retired);
    CHECK(result.ok && result.changed == std::vector<std::string>{relative} && result.skipped.empty());
    CHECK(!fs::exists(game / relative) && fs::is_directory(game / "zombie"));
    // Gone already: nothing to do and nothing to report, with or without its folder.
    const auto again = RetireRecipeFiles(game.string(), retired);
    CHECK(again.ok && again.changed.empty() && again.skipped.empty());
    fs::remove_all(game / "zombie");
    const auto no_folder = RetireRecipeFiles(game.string(), retired);
    CHECK(no_folder.ok && no_folder.changed.empty() && no_folder.skipped.empty());
  }
  {
    // The game's own file, or one the player changed, stays.
    const auto game = base / "retired-own";
    Write(game / relative, "the game's");
    const auto result = RetireRecipeFiles(game.string(), {{relative, std::string(64, '0')}});
    CHECK(result.ok && result.changed.empty() && result.skipped.size() == 1 && Read(game / relative) == "the game's");
  }
  {
    // A link to a file with Playden's bytes is not followed, and neither link nor file goes.
    const auto game = base / "retired-link";
    const auto outside = base / "retired-outside";
    Write(outside, "ours");
    fs::create_directories(game / "zombie");
    fs::create_symlink(outside, game / relative);
    const auto result = RetireRecipeFiles(
        game.string(), {{relative, "ae0bec9e9d33c56354bd612c8a6010840324fc5b9786ce0a18b696b304b09397"}});
    CHECK(result.ok && result.changed.empty() && result.skipped.size() == 1);
    CHECK(fs::is_symlink(game / relative) && Read(outside) == "ours");
  }
  fs::remove_all(base);
  return 0;
}
