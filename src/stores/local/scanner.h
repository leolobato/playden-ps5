#pragma once
// Finds the programs in a Windows game folder the player copied to a drive (port of the macOS
// app's LocalWindowsGame). Every walk goes through platform/directory_walk.h: a PS5 title can't
// open relative to a descriptor, and a walk the platform can't do is refused, never redone with
// plain path operations.
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace playden::stores::local {

// Programs are looked for this many components deep: "a/b/c/game.exe" is the deepest.
inline constexpr int kMaxProgramDepth = 4;
// Entries listed (any kind, hidden ones included) and programs found before a scan stops.
inline constexpr std::size_t kMaxScanEntries = 20000;
inline constexpr std::size_t kMaxScanPrograms = 512;
// Folders an import root lists before its scan stops.
inline constexpr std::size_t kMaxImportCandidates = 512;

enum class ScanStatus {
  kOk,
  kMissing,      // the folder (or the drive) isn't there
  kNotFolder,
  kUnsupported,  // the platform refused a descriptor walk (ENOSYS/EINVAL): nothing was read
  kFailed,       // a folder could not be listed, or a link sits in the folder's own path
};

struct Program {
  std::string path;  // inside the game folder, '/' separators
  std::uint64_t bytes = 0;
  // Installers, uninstallers, redistributables, crash handlers and tools: shown as
  // "Installer or tool" and ranked last.
  bool helper = false;
  bool printable = true;  // IsPlayableInPlace (in-place eligibility also needs this)
};

struct FolderScan {
  ScanStatus status = ScanStatus::kFailed;
  int error = 0;  // errno of the failure
  std::string title;
  std::vector<Program> programs;  // ranked: the likely game first
  // Regular files the walk saw (hidden files and links left out). Display estimates only: they
  // cover the first kMaxProgramDepth levels and stop at the limits; the copy plan has the real totals.
  std::uint64_t total_bytes = 0;
  std::uint64_t file_count = 0;
  bool deeper_folders = false;  // folders below kMaxProgramDepth were not walked
  // The folder's console path passes IsPlayableInPlace: only then can it play in place (the
  // descriptor rule and Wine's path); otherwise it can only be copied or moved into Playden.
  bool printable_path = false;
  bool limit_reached = false;  // kMaxScanEntries or kMaxScanPrograms: the lists are partial
  std::size_t unreadable = 0;  // entries that could not be opened and were skipped
};

// Scans an absolute console path such as /mnt/usb0/playden/import/Delores.
FolderScan ScanGameFolder(const std::string& folder);

// The folder's name, or its parent's when the name is generic ("game", "bin", "x64", ...), unless
// the parent is an import root (playden/import, playden-import) or the file system root.
std::string TitleForFolder(const std::string& folder);
bool IsLikelyHelper(const std::string& relative_path);
// Non-helpers first; then names matching the title (ASCII letters only, lower case, either
// contains the other); then shallower; then larger; then by path (byte order).
void RankPrograms(std::vector<Program>& programs, const std::string& title);
// The program that starts the game without asking: the only one that isn't an installer or tool.
// Two or more candidates, or none, are the player's choice. Any list of items with a `helper` flag
// (Program, or the UI's copy of it).
template <typename Programs>
std::optional<std::size_t> ClearGameProgram(const Programs& programs) {
  std::optional<std::size_t> found;
  for (std::size_t index = 0; index < programs.size(); ++index) {
    if (programs[index].helper) continue;
    if (found) return std::nullopt;
    found = index;
  }
  return found;
}
bool IsPrintableAscii(const std::string& text);
// A console path a game can play from in place: printable ASCII without ':' or '\\', which Wine's path for it can't
// hold (prospero-win maps the console root to Z:, so a colon or backslash would split the folder name).
bool IsPlayableInPlace(const std::string& text);

// A path inside a folder: relative, '/' separators, no empty, "." or ".." parts, no backslash or NUL.
bool IsSafeRelativePath(const std::string& path);

enum class ProgramCheck { kOk, kInvalidPath, kMissing, kNotRegularFile, kUnsupported, kFailed };
// Before saving or launching: the path is safe, and walking it from the folder without following
// links reaches a regular file.
ProgramCheck CheckProgram(const std::string& folder, const std::string& program);

// Where games to import are looked for. `folder` is relative to `mount`; entries in imports.json
// name a game by `volume` and its folder relative to the same mount (the drop folder:
// volume "internal", folder "playden-import/<name>"). Volume IDs are the install volumes'.
struct ImportRoot {
  const char* volume;
  const char* mount;
  const char* folder;
};
const std::vector<ImportRoot>& ImportRoots();

struct CandidateScan {
  ScanStatus status = ScanStatus::kFailed;  // a missing root is kOk with no folders
  int error = 0;
  std::vector<std::string> folders;  // names of the direct child folders, sorted
  bool limit_reached = false;
};
// Direct child folders of an import root; hidden entries, links and files are left out.
CandidateScan ScanImportRoot(const std::string& root);

}  // namespace playden::stores::local
