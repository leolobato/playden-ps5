// Ports of the macOS LocalWindowsGameTests, plus the console's walk rules. Built twice: once with
// the host's descriptor walk, once with the PS5's walk by path (PLAYDEN_PLATFORM_PS5).
#include "stores/local/scanner.h"
#include "platform/directory_walk.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <set>
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
    auto pattern = (fs::temp_directory_path() / "playden-local-scanner-XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    CHECK(::mkdtemp(writable.data()) != nullptr);
    path = fs::canonical(writable.data());
  }
  ~TemporaryDirectory() {
    std::error_code ignored;
    fs::remove_all(path, ignored);
  }
  fs::path path;
};

void File(const fs::path& path, std::uint64_t bytes = 4) {
  fs::create_directories(path.parent_path());
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  CHECK(fd >= 0);
  CHECK(::ftruncate(fd, static_cast<off_t>(bytes)) == 0);
  ::close(fd);
}

std::vector<std::string> Paths(const FolderScan& scan) {
  std::vector<std::string> paths;
  for (const auto& program : scan.programs) paths.push_back(program.path);
  return paths;
}

bool Has(const FolderScan& scan, const std::string& path) {
  const auto paths = Paths(scan);
  return std::find(paths.begin(), paths.end(), path) != paths.end();
}

void ExecutablesListTheGameFirstAndHelpersLast() {
  TemporaryDirectory root;
  File(root.path / "Blur/game/unins000.exe", 900);
  File(root.path / "Blur/game/Blur.exe", 100);
  File(root.path / "Blur/game/_Redist/dxsetup.exe", 2000);
  File(root.path / "Blur/game/_Redist/vcredist_x86.exe", 2000);
  File(root.path / "Blur/game/tools/Editor.exe", 5000);
  File(root.path / "Blur/game/UnityCrashHandler64.exe", 3000);
  const auto scan = ScanGameFolder((root.path / "Blur/game").string());
  CHECK(scan.status == ScanStatus::kOk);
  CHECK(scan.title == "Blur");
  const auto paths = Paths(scan);
  CHECK(paths.size() == 6);
  CHECK(paths[0] == "Blur.exe");  // named like the game
  CHECK(paths[1] == "tools/Editor.exe");
  CHECK((std::set<std::string>(paths.begin() + 2, paths.end()) ==
         std::set<std::string>{"unins000.exe", "_Redist/dxsetup.exe", "_Redist/vcredist_x86.exe", "UnityCrashHandler64.exe"}));
  CHECK(std::all_of(scan.programs.begin() + 2, scan.programs.end(), [](const Program& p) { return p.helper; }));
  CHECK(!scan.programs[0].helper && !scan.programs[1].helper);
  CHECK(scan.file_count == 6 && scan.total_bytes == 900 + 100 + 2000 + 2000 + 5000 + 3000);
  CHECK(!scan.limit_reached && !scan.deeper_folders && scan.printable_path);
}

void GenericFolderNamesUseTheParentAsTitle() {
  CHECK(TitleForFolder("/Games/Blur/game") == "Blur");
  CHECK(TitleForFolder("/Games/Hades") == "Hades");
  CHECK(TitleForFolder("/Games/Hades/") == "Hades");
  CHECK(TitleForFolder("/mnt/usb0/Some Game/Binaries") == "Some Game");
  CHECK(TitleForFolder("/mnt/usb0/Hades/X64") == "Hades");
  // A generic name right in an import root keeps its own name: the root isn't the game.
  CHECK(TitleForFolder("/mnt/usb0/playden/import/Game") == "Game");
  CHECK(TitleForFolder("/data/playden-import/bin") == "bin");
  CHECK(TitleForFolder("/game") == "game");
  CHECK(TitleForFolder("/mnt/usb0/playden/import/Hollow Knight") == "Hollow Knight");
}

void ProgramChecks() {
  TemporaryDirectory root;
  File(root.path / "Empty/readme.txt");
  File(root.path / "Hades/x64/Hades.exe");
  const auto empty = (root.path / "Empty").string(), hades = (root.path / "Hades").string();
  CHECK(CheckProgram(empty, "Game.exe") == ProgramCheck::kMissing);
  CHECK(CheckProgram(empty, "../readme.txt") == ProgramCheck::kInvalidPath);
  CHECK(CheckProgram(hades, "x64/Hades.exe") == ProgramCheck::kOk);
  CHECK(CheckProgram(hades + "/", "x64/Hades.exe") == ProgramCheck::kOk);
  CHECK(CheckProgram(hades, "x64") == ProgramCheck::kNotRegularFile);
  CHECK(CheckProgram(hades, "x64/Hades.exe/more") == ProgramCheck::kNotRegularFile);
  CHECK(CheckProgram((root.path / "Gone").string(), "Game.exe") == ProgramCheck::kMissing);
  for (const char* bad : {"", "/x64/Hades.exe", "x64//Hades.exe", "./x64/Hades.exe", "x64/./Hades.exe", "x64/",
                          "x64\\Hades.exe", "x64/../x64/Hades.exe", ".."})
    CHECK(CheckProgram(hades, bad) == ProgramCheck::kInvalidPath);
  CHECK(IsSafeRelativePath("a b/c.exe") && !IsSafeRelativePath(std::string("a\0b", 3)));
}

void LinksAreNeverFollowed() {
  TemporaryDirectory root;
  File(root.path / "outside/Secret.exe", 10'000);
  File(root.path / "outside/dir/Other.exe", 10'000);
  File(root.path / "Game/Game.exe", 10);
  fs::create_symlink(root.path / "outside/Secret.exe", root.path / "Game/Linked.exe");
  fs::create_directory_symlink(root.path / "outside/dir", root.path / "Game/linked-dir");
  fs::create_symlink("../outside/Secret.exe", root.path / "Game/Relative.exe");
  const auto game = (root.path / "Game").string();
  const auto scan = ScanGameFolder(game);
  CHECK(scan.status == ScanStatus::kOk);
  CHECK(Paths(scan) == std::vector<std::string>{"Game.exe"});
  CHECK(scan.file_count == 1 && scan.total_bytes == 10);
  // The program check doesn't follow them either, wherever they point.
  CHECK(CheckProgram(game, "Linked.exe") == ProgramCheck::kNotRegularFile);
  CHECK(CheckProgram(game, "Relative.exe") == ProgramCheck::kNotRegularFile);
  CHECK(CheckProgram(game, "linked-dir/Other.exe") == ProgramCheck::kNotRegularFile);
  // A link in the folder's own path: refused, not followed.
  fs::create_directory_symlink(root.path / "Game", root.path / "Alias");
  const auto aliased = ScanGameFolder((root.path / "Alias").string());
  CHECK(aliased.status != ScanStatus::kOk && aliased.programs.empty());
  CHECK(CheckProgram((root.path / "Alias").string(), "Game.exe") != ProgramCheck::kOk);
}

void HiddenEntriesAreSkippedAndExtensionsIgnoreCase() {
  TemporaryDirectory root;
  File(root.path / "Game/.hidden/Game.exe");
  File(root.path / "Game/.Sneaky.exe");
  File(root.path / "Game/GAME.EXE");
  File(root.path / "Game/Launcher.Exe");
  File(root.path / "Game/notes.exe.txt");
  File(root.path / "Game/.exe");
  fs::create_directories(root.path / "Game/folder.exe");
  const auto scan = ScanGameFolder((root.path / "Game").string());
  CHECK(scan.status == ScanStatus::kOk);
  CHECK(Paths(scan) == (std::vector<std::string>{"GAME.EXE", "Launcher.Exe"}));
  CHECK(scan.file_count == 3);  // the hidden ones aren't counted
}

void DepthIsLimited() {
  TemporaryDirectory root;
  File(root.path / "Game/a/b/c/Deep.exe", 1);
  File(root.path / "Game/a/b/c/d/Deeper.exe", 1'000'000);
  File(root.path / "Game/a/b/c/d/e/Deepest.exe", 1);
  const auto scan = ScanGameFolder((root.path / "Game").string());
  CHECK(scan.status == ScanStatus::kOk);
  CHECK(Paths(scan) == std::vector<std::string>{"a/b/c/Deep.exe"});
  CHECK(scan.deeper_folders && !scan.limit_reached);
  CHECK(scan.file_count == 1);
}

void LimitsStopTheScanAndSaySo() {
  {
    TemporaryDirectory root;
    for (std::size_t i = 0; i < kMaxScanPrograms + 1; ++i) File(root.path / "Game" / ("p" + std::to_string(i) + ".exe"), 1);
    const auto scan = ScanGameFolder((root.path / "Game").string());
    CHECK(scan.status == ScanStatus::kOk && scan.limit_reached);
    CHECK(scan.programs.size() == kMaxScanPrograms);
  }
  {
    TemporaryDirectory root;
    for (std::size_t i = 0; i < kMaxScanEntries + 1; ++i) {
      const auto path = root.path / "Game" / ("f" + std::to_string(i) + ".dat");
      if (i == 0) fs::create_directories(path.parent_path());
      const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT, 0644);
      CHECK(fd >= 0);
      ::close(fd);
    }
    const auto scan = ScanGameFolder((root.path / "Game").string());
    CHECK(scan.status == ScanStatus::kOk && scan.limit_reached);
    CHECK(scan.file_count == kMaxScanEntries);
  }
  {  // A folder's own programs are found before the limit is spent in its subfolders, whatever the listing order.
    TemporaryDirectory root;
    for (const char* folder : {"a", "m", "z"})
      for (std::size_t i = 0; i < kMaxScanEntries / 2; ++i) {
        const auto path = root.path / "Game" / folder / ("f" + std::to_string(i) + ".dat");
        if (i == 0) fs::create_directories(path.parent_path());
        const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT, 0644);
        CHECK(fd >= 0);
        ::close(fd);
      }
    File(root.path / "Game/Wanted.exe", 1);
    const auto scan = ScanGameFolder((root.path / "Game").string());
    CHECK(scan.status == ScanStatus::kOk && scan.limit_reached);
    CHECK(scan.programs.size() == 1 && scan.programs.front().path == "Wanted.exe");
  }
}

void NonAsciiPathsAreFlagged() {
  TemporaryDirectory root;
  File(root.path / "Caf\xc3\xa9 Racer/Caf\xc3\xa9 Racer.exe");
  File(root.path / "Caf\xc3\xa9 Racer/Racer.exe");
  File(root.path / "Plain/\xe3\x82\xb2\xe3\x83\xbc\xe3\x83\xa0.exe");
  const auto accented = ScanGameFolder((root.path / "Caf\xc3\xa9 Racer").string());
  CHECK(accented.status == ScanStatus::kOk && !accented.printable_path);
  CHECK(accented.programs.size() == 2);
  const auto plain = ScanGameFolder((root.path / "Plain").string());
  CHECK(plain.status == ScanStatus::kOk && plain.printable_path);
  CHECK(plain.programs.size() == 1 && !plain.programs[0].printable);
  CHECK(IsPrintableAscii("/mnt/usb0/playden/import/Old School Rally") && !IsPrintableAscii("a\tb"));
  // A colon or backslash can't be in Wine's path for the folder (prospero-win's Z: is the console root).
  CHECK(IsPlayableInPlace("/mnt/usb0/playden/import/Old School Rally"));
  CHECK(!IsPlayableInPlace("/mnt/usb0/playden/import/Warhammer 40k: Space Marine"));
  CHECK(!IsPlayableInPlace("/mnt/usb0/playden/import/a\\b") && !IsPlayableInPlace("/mnt/usb0/Caf\xc3\xa9"));
}

void FailuresAreReported() {
  TemporaryDirectory root;
  File(root.path / "file.txt");
  CHECK(ScanGameFolder((root.path / "missing").string()).status == ScanStatus::kMissing);
  CHECK(ScanGameFolder((root.path / "file.txt").string()).status == ScanStatus::kNotFolder);
  CHECK(ScanGameFolder("relative/path").status == ScanStatus::kUnsupported);
}

// File lists of the three games the import spike played (sizes made up, shapes as shipped).
void SpikeGamesRankTheirProgramFirst() {
  TemporaryDirectory root;
  const auto import = root.path / "mnt/usb0/playden/import";
  File(import / "Delores/Delores.exe", 6'000'000);
  File(import / "Delores/SDL2.dll", 1'500'000);
  File(import / "Delores/Delores.ggpack1", 40'000'000);
  File(import / "Delores/crashpad_handler.exe", 9'000'000);
  File(import / "Night Sky/NightSky.exe", 2'000'000);
  File(import / "Night Sky/steam_api.dll", 200'000);
  File(import / "Night Sky/steam_appid.txt", 6);
  File(import / "Night Sky/setup/dxsetup.exe", 500'000);
  File(import / "Night Sky/setup/vcredist_x86.exe", 5'000'000);
  File(import / "Night Sky/setup/oalinst.exe", 800'000);
  File(import / "Old School Rally/Old School Rally.exe", 650'000);
  File(import / "Old School Rally/UnityCrashHandler64.exe", 1'100'000);
  File(import / "Old School Rally/UnityPlayer.dll", 30'000'000);
  File(import / "Old School Rally/Old School Rally_Data/Plugins/x86_64/steam_api64.dll", 300'000);
  File(import / "Old School Rally/MonoBleedingEdge/EmbedRuntime/mono-2.0-bdwgc.dll", 5'000'000);
  const auto candidates = ScanImportRoot(import.string());
  CHECK(candidates.status == ScanStatus::kOk);
  CHECK(candidates.folders == (std::vector<std::string>{"Delores", "Night Sky", "Old School Rally"}));
  const auto delores = ScanGameFolder((import / "Delores").string());
  CHECK(delores.title == "Delores" && Paths(delores).front() == "Delores.exe" && delores.programs.back().helper);
  CHECK(delores.file_count == 4);
  const auto night = ScanGameFolder((import / "Night Sky").string());
  CHECK(night.title == "Night Sky" && Paths(night).front() == "NightSky.exe");
  CHECK(std::all_of(night.programs.begin() + 1, night.programs.end(), [](const Program& p) { return p.helper; }));
  const auto rally = ScanGameFolder((import / "Old School Rally").string());
  CHECK(rally.status == ScanStatus::kOk && rally.printable_path);
  CHECK(Paths(rally) == (std::vector<std::string>{"Old School Rally.exe", "UnityCrashHandler64.exe"}));
  CHECK(!rally.programs[0].helper && rally.programs[1].helper);
  CHECK(rally.file_count == 5);
  CHECK(CheckProgram((import / "Old School Rally").string(), "Old School Rally.exe") == ProgramCheck::kOk);
}

void ImportRootListsChildFolders() {
  TemporaryDirectory root;
  CHECK(ScanImportRoot((root.path / "absent/playden/import").string()).status == ScanStatus::kOk);
  CHECK(ScanImportRoot((root.path / "absent/playden/import").string()).folders.empty());
  const auto import = root.path / "import";
  fs::create_directories(import / "B Game");
  fs::create_directories(import / "A Game/sub");
  fs::create_directories(import / ".Trashes");
  File(import / "loose.exe");
  fs::create_directory_symlink(root.path, import / "Link");
  const auto scan = ScanImportRoot(import.string());
  CHECK(scan.status == ScanStatus::kOk && !scan.limit_reached);
  CHECK(scan.folders == (std::vector<std::string>{"A Game", "B Game"}));
  CHECK(ScanImportRoot("relative").status == ScanStatus::kUnsupported);
  CHECK(ScanImportRoot((import / "loose.exe").string()).status == ScanStatus::kNotFolder);
  CHECK(ImportRoots().size() == 11 && std::string(ImportRoots()[0].volume) == "internal");
  // A drive with an EFI partition takes a slot for it, so a game partition can land past usb1.
  CHECK(std::string(ImportRoots().back().volume) == "usb7" && std::string(ImportRoots().back().mount) == "/mnt/usb7");
}

void HelperRules() {
  CHECK(IsLikelyHelper("Setup.exe") && IsLikelyHelper("UNINS000.EXE") && IsLikelyHelper("bin/UnityCrashHandler64.exe"));
  CHECK(IsLikelyHelper("_CommonRedist/DirectX/Jun2010/foo.exe") && IsLikelyHelper("EasyAntiCheat/Game.exe"));
  CHECK(!IsLikelyHelper("Game.exe") && !IsLikelyHelper("bin/Game.exe") && !IsLikelyHelper("redistx/Game.exe"));
  CHECK(!IsLikelyHelper("Setup/Game.exe"));  // "setup" is a name prefix, not a helper folder
}

void ClearProgramIsTheOnlyNonHelper() {
  using Programs = std::vector<Program>;
  CHECK(ClearGameProgram(Programs{{"Game.exe", 9, false}}) == std::size_t{0});
  // One program besides helpers, wherever it is listed.
  CHECK(ClearGameProgram(Programs{{"setup.exe", 1, true}, {"Game.exe", 9, false}, {"UnityCrashHandler64.exe", 1, true}}) ==
        std::size_t{1});
  CHECK(!ClearGameProgram(Programs{{"Game.exe", 9, false}, {"GameLauncher.exe", 2, false}}));  // a choice
  CHECK(!ClearGameProgram(Programs{{"setup.exe", 1, true}}));
  CHECK(!ClearGameProgram(Programs{}));
}

}  // namespace

#if defined(PLAYDEN_PLATFORM_PS5)
// exFAT lists an empty file under a different inode than stat reports; force that mismatch.
void ListingMismatchOnlyFailsForUnprivilegedRegularFiles() {
  TemporaryDirectory root;
  File(root.path / "game" / "empty.lock", 0);
  fs::create_directories(root.path / "game" / "sub");
  const int parent = playden::platform::OpenDirectoryWalk(root.path / "game");
  CHECK(parent >= 0);
  const std::uint64_t wrong = 1;
  const int flags = O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC;
  int fd = playden::platform::OpenWalkChild(parent, "empty.lock", flags, wrong);
  CHECK(fd < 0 && errno == ESTALE);
  fd = playden::platform::OpenWalkChildUnprivileged(parent, "empty.lock", flags, wrong);
  CHECK(fd >= 0);
  ::close(fd);
  fd = playden::platform::OpenWalkChildUnprivileged(parent, "sub", flags, wrong);
  CHECK(fd < 0 && errno == ESTALE);
  fd = playden::platform::OpenWalkChildUnprivileged(parent, "sub", flags | O_DIRECTORY, wrong);
  CHECK(fd < 0 && errno == ESTALE);
  ::close(parent);
}
#endif

int main() {
#if defined(PLAYDEN_PLATFORM_PS5)
  ListingMismatchOnlyFailsForUnprivilegedRegularFiles();
#endif
  ExecutablesListTheGameFirstAndHelpersLast();
  GenericFolderNamesUseTheParentAsTitle();
  ProgramChecks();
  LinksAreNeverFollowed();
  HiddenEntriesAreSkippedAndExtensionsIgnoreCase();
  DepthIsLimited();
  LimitsStopTheScanAndSaySo();
  NonAsciiPathsAreFlagged();
  FailuresAreReported();
  SpikeGamesRankTheirProgramFirst();
  ImportRootListsChildFolders();
  HelperRules();
  ClearProgramIsTheOnlyNonHelper();
  std::puts("local_scanner_test passed");
  return 0;
}
