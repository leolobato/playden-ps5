#include "stores/local/scanner.h"

#include "platform/directory_walk.h"

#include <algorithm>
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace playden::stores::local {
namespace {

constexpr int kOpenAny = O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC;
constexpr int kOpenFolder = O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;

char Lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

std::string Lowercase(std::string text) {
  for (auto& c : text) c = Lower(c);
  return text;
}

// Lower-case ASCII letters only; other bytes (digits, spaces, UTF-8) are dropped.
std::string Letters(const std::string& text) {
  std::string letters;
  for (const char c : text) {
    const char lower = Lower(c);
    if (lower >= 'a' && lower <= 'z') letters.push_back(lower);
  }
  return letters;
}

std::vector<std::string> Split(const std::string& path) {
  std::vector<std::string> parts;
  std::size_t start = 0;
  for (;;) {
    const auto slash = path.find('/', start);
    parts.push_back(path.substr(start, slash == std::string::npos ? std::string::npos : slash - start));
    if (slash == std::string::npos) break;
    start = slash + 1;
  }
  return parts;
}

std::string Trimmed(std::string folder) {
  while (folder.size() > 1 && folder.back() == '/') folder.pop_back();
  return folder;
}

bool EndsWithExe(const std::string& name) {
  return name.size() > 4 && Lowercase(name.substr(name.size() - 4)) == ".exe";
}

bool Unsupported(int error) { return error == ENOSYS || error == EINVAL; }

bool IsImportRootFolder(const std::vector<std::string>& parts, std::size_t parent) {
  if (parts[parent] == "playden-import") return true;
  return parts[parent] == "import" && parent > 0 && parts[parent - 1] == "playden";
}

// One open folder of the walk: its descriptor, its listing and where it is in the game folder.
struct Frame {
  int fd = -1;
  DIR* listing = nullptr;
  std::string relative;  // empty for the game folder itself
  int level = 0;         // its children are level + 1 components deep
  // Its subfolders (name, listed inode), walked once its own entries are done: one listing open per level.
  std::vector<std::pair<std::string, std::uint64_t>> folders;
  std::size_t next_folder = 0;
};

void Close(Frame& frame) {
  if (frame.listing) ::closedir(frame.listing);
  if (frame.fd >= 0) ::close(frame.fd);
  frame.listing = nullptr;
  frame.fd = -1;
}

// Takes `fd`. False (errno set) when the folder can't be listed.
bool OpenFrame(int fd, std::string relative, int level, Frame* frame) {
  const int listing_fd = ::dup(fd);
  DIR* listing = listing_fd >= 0 ? ::fdopendir(listing_fd) : nullptr;
  if (!listing) {
    const int error = errno;
    if (listing_fd >= 0) ::close(listing_fd);
    ::close(fd);
    errno = error;
    return false;
  }
  *frame = Frame{fd, listing, std::move(relative), level, {}, 0};
  return true;
}

ScanStatus StatusForOpen(int error) {
  if (Unsupported(error)) return ScanStatus::kUnsupported;
  if (error == ENOENT) return ScanStatus::kMissing;
  if (error == ENOTDIR) return ScanStatus::kNotFolder;
  return ScanStatus::kFailed;
}

}  // namespace

bool IsPrintableAscii(const std::string& text) {
  return std::all_of(text.begin(), text.end(), [](unsigned char c) { return c >= 0x20 && c < 0x7f; });
}

bool IsPlayableInPlace(const std::string& text) {
  return IsPrintableAscii(text) && text.find_first_of(":\\") == std::string::npos;
}

bool IsSafeRelativePath(const std::string& path) {
  if (path.empty() || path.size() > 4096 || path.front() == '/') return false;
  if (path.find('\\') != std::string::npos || path.find('\0') != std::string::npos) return false;
  for (const auto& part : Split(path))
    if (part.empty() || part == "." || part == "..") return false;
  return true;
}

bool IsLikelyHelper(const std::string& relative_path) {
  static const char* const kHelperNames[] = {
      "unins", "setup", "install", "vcredist", "vc_redist", "dxsetup", "dxwebsetup", "directx", "dotnet", "ndp",
      "physx", "oalinst", "crash", "unitycrashhandler", "bugreport", "errorreport", "reporter", "redist", "ue4prereq",
      "easyanticheat", "battleye", "uninstall", "update", "patch", "config", "settings", "dxsdk"};
  static const char* const kHelperFolders[] = {
      "_redist", "redist", "_commonredist", "commonredist", "redistributables", "__installer", "directx",
      "support", "dotnet", "vcredist", "_installer", "installers", "easyanticheat", "battleye"};
  auto parts = Split(Lowercase(relative_path));
  parts.erase(std::remove(parts.begin(), parts.end(), std::string()), parts.end());
  if (parts.empty()) return false;
  const auto& name = parts.back();
  for (const auto* prefix : kHelperNames)
    if (name.compare(0, std::char_traits<char>::length(prefix), prefix) == 0) return true;
  for (std::size_t i = 0; i + 1 < parts.size(); ++i)
    for (const auto* folder : kHelperFolders)
      if (parts[i] == folder) return true;
  return false;
}

std::string TitleForFolder(const std::string& folder) {
  static const char* const kGeneric[] = {"game", "games", "bin", "bin32", "bin64", "x64", "x86",
                                         "win32", "win64", "binaries", "app", "files", "data"};
  auto parts = Split(Trimmed(folder));
  parts.erase(std::remove(parts.begin(), parts.end(), std::string()), parts.end());
  if (parts.empty()) return {};
  const auto& name = parts.back();
  const auto lower = Lowercase(name);
  const bool generic = std::any_of(std::begin(kGeneric), std::end(kGeneric),
                                   [&](const char* word) { return lower == word; });
  if (!generic || parts.size() < 2 || IsImportRootFolder(parts, parts.size() - 2)) return name;
  return parts[parts.size() - 2];
}

void RankPrograms(std::vector<Program>& programs, const std::string& title) {
  const auto wanted = Letters(title);
  const auto matches = [&](const Program& program) {
    auto name = program.path.substr(program.path.find_last_of('/') + 1);
    const auto dot = name.find_last_of('.');
    if (dot != std::string::npos && dot > 0) name.erase(dot);
    const auto letters = Letters(name);
    return !wanted.empty() && !letters.empty() &&
           (letters.find(wanted) != std::string::npos || wanted.find(letters) != std::string::npos);
  };
  const auto depth = [](const Program& program) {
    return std::count(program.path.begin(), program.path.end(), '/');
  };
  std::stable_sort(programs.begin(), programs.end(), [&](const Program& a, const Program& b) {
    if (a.helper != b.helper) return !a.helper;
    const bool match_a = matches(a), match_b = matches(b);
    if (match_a != match_b) return match_a;
    const auto depth_a = depth(a), depth_b = depth(b);
    if (depth_a != depth_b) return depth_a < depth_b;
    if (a.bytes != b.bytes) return a.bytes > b.bytes;
    return a.path < b.path;
  });
}

FolderScan ScanGameFolder(const std::string& folder_path, const ScanLimits& limits) {
  FolderScan scan;
  const auto folder = Trimmed(folder_path);
  scan.title = TitleForFolder(folder);
  scan.printable_path = IsPlayableInPlace(folder);
  const int root = platform::OpenDirectoryWalk(folder);
  if (root < 0) {
    scan.error = errno;
    scan.status = StatusForOpen(scan.error);
    return scan;
  }
  // The walk keeps one frame per level (at most limits.max_depth): bounded descriptors (scanner.h).
  std::vector<Frame> stack;
  stack.reserve(static_cast<std::size_t>(std::max(limits.max_depth, 1)));
  Frame first;
  if (!OpenFrame(root, {}, 0, &first)) {
    scan.error = errno;
    scan.status = StatusForOpen(scan.error);
    return scan;
  }
  stack.push_back(std::move(first));
  const std::size_t max_programs = std::max<std::size_t>(limits.max_programs, 1);
  // Past twice the cap the list is ranked and the best kept, so the game is kept wherever the walk meets it.
  const auto trim_programs = [&] {
    RankPrograms(scan.programs, scan.title);
    if (scan.programs.size() > max_programs) scan.programs.resize(max_programs);
  };
  ScanStatus status = ScanStatus::kOk;
  while (!stack.empty() && status == ScanStatus::kOk) {
    Frame& frame = stack.back();
    errno = 0;
    const auto* entry = ::readdir(frame.listing);
    if (!entry) {
      if (errno != 0) { scan.error = errno; status = ScanStatus::kFailed; break; }
      if (frame.next_folder < frame.folders.size()) {
        const auto& [name, listed] = frame.folders[frame.next_folder++];
        const int child = platform::OpenWalkChildUnprivileged(frame.fd, name.c_str(), kOpenFolder, listed);
        if (child < 0) { ++scan.unreadable; continue; }  // gone, or swapped for a link or a file
        Frame next;
        const auto relative = frame.relative.empty() ? name : frame.relative + "/" + name;
        if (!OpenFrame(child, relative, frame.level + 1, &next)) { ++scan.unreadable; continue; }
        stack.push_back(std::move(next));  // `frame` is not used after this
        continue;
      }
      Close(frame);
      stack.pop_back();
      continue;
    }
    const std::string name = entry->d_name;
    if (name == "." || name == "..") continue;
    if (++scan.entries > limits.max_entries) { --scan.entries; scan.limit_reached = true; break; }
    if (name.front() == '.') continue;
    const auto type = limits.ignore_listed_types ? DT_UNKNOWN : entry->d_type;
    if (type == DT_LNK) continue;
    const bool program_name = EndsWithExe(name);
    // Only folders and programs are opened: a listed file of another kind costs the readdir alone.
    if (type != DT_DIR && type != DT_UNKNOWN && !program_name) continue;
    const auto listed = static_cast<std::uint64_t>(entry->d_fileno);
    // A folder below the deepest level: its entries would be deeper than programs are looked for.
    const bool too_deep = frame.level + 1 >= limits.max_depth;
    if (type == DT_DIR) {
      if (too_deep) scan.deeper_folders = true;
      else frame.folders.emplace_back(name, listed);
      continue;
    }
    if (!program_name) {
      // No type in the listing (exFAT may give none): opening it as a folder tells, as ScanImportRoot does. A file
      // fails with ENOTDIR before anything is read; a link fails with ELOOP/EMLINK and is never followed.
      const int child = platform::OpenWalkChildUnprivileged(frame.fd, name.c_str(), kOpenFolder, listed);
      if (child < 0) {
        const int error = errno;
        if (Unsupported(error)) { scan.error = error; status = ScanStatus::kUnsupported; break; }
        if (error != ENOTDIR && error != ELOOP && error != EMLINK) ++scan.unreadable;
        continue;
      }
      ::close(child);
      if (too_deep) scan.deeper_folders = true;
      else frame.folders.emplace_back(name, listed);
      continue;
    }
    const int child = platform::OpenWalkChildUnprivileged(frame.fd, name.c_str(), kOpenAny, listed);
    if (child < 0) {
      const int error = errno;
      if (Unsupported(error)) { scan.error = error; status = ScanStatus::kUnsupported; break; }
      // ELOOP/EMLINK: a link whose listing said DT_UNKNOWN (exFAT does).
      if (error != ELOOP && error != EMLINK) ++scan.unreadable;
      continue;
    }
    struct stat info{};
    const bool stated = ::fstat(child, &info) == 0;
    ::close(child);
    if (!stated) { ++scan.unreadable; continue; }
    if (S_ISDIR(info.st_mode)) {  // a folder named like a program
      if (too_deep) scan.deeper_folders = true;
      else frame.folders.emplace_back(name, listed);
      continue;
    }
    if (!S_ISREG(info.st_mode)) continue;
    const auto relative = frame.relative.empty() ? name : frame.relative + "/" + name;
    const auto bytes = info.st_size > 0 ? static_cast<std::uint64_t>(info.st_size) : 0;
    scan.programs.push_back(Program{relative, bytes, IsLikelyHelper(relative), IsPlayableInPlace(relative)});
    if (scan.programs.size() >= 2 * max_programs) trim_programs();
  }
  for (auto& frame : stack) Close(frame);
  scan.status = status;
  if (status != ScanStatus::kOk) {
    scan.programs.clear();
    return scan;
  }
  trim_programs();
  return scan;
}

ScanGap GapOf(const FolderScan& scan) {
  if (scan.status != ScanStatus::kOk) return ScanGap::kFailed;
  if (scan.limit_reached) return ScanGap::kTooManyEntries;
  if (scan.deeper_folders) return ScanGap::kTooDeep;
  if (scan.unreadable) return ScanGap::kUnreadable;
  return ScanGap::kNone;
}

ProgramCheck CheckProgram(const std::string& folder, const std::string& program) {
  if (!IsSafeRelativePath(program)) return ProgramCheck::kInvalidPath;
  int fd = platform::OpenDirectoryWalk(Trimmed(folder));
  if (fd < 0) {
    if (Unsupported(errno)) return ProgramCheck::kUnsupported;
    return errno == ENOENT || errno == ENOTDIR ? ProgramCheck::kMissing : ProgramCheck::kFailed;
  }
  const auto parts = Split(program);
  for (std::size_t i = 0; i < parts.size(); ++i) {
    const bool last = i + 1 == parts.size();
    const int child = last ? platform::OpenWalkChildUnprivileged(fd, parts[i].c_str(), kOpenAny)
                           : platform::OpenWalkChild(fd, parts[i].c_str(), kOpenFolder);
    const int error = errno;
    ::close(fd);
    if (child < 0) {
      if (Unsupported(error)) return ProgramCheck::kUnsupported;
      if (error == ENOENT) return ProgramCheck::kMissing;
      // A link (ELOOP/EMLINK), or a file where a folder was expected.
      if (error == ELOOP || error == EMLINK || error == ENOTDIR) return ProgramCheck::kNotRegularFile;
      return ProgramCheck::kFailed;
    }
    fd = child;
  }
  struct stat info{};
  const bool regular = ::fstat(fd, &info) == 0 && S_ISREG(info.st_mode);
  ::close(fd);
  return regular ? ProgramCheck::kOk : ProgramCheck::kNotRegularFile;
}

const std::vector<ImportRoot>& ImportRoots() {
  static const std::vector<ImportRoot> roots = {
      {"internal", "/data", "playden-import"},
      {"m2", "/mnt/ext1", "playden/import"},
      {"extended", "/mnt/ext0", "playden/import"},
      {"usb0", "/mnt/usb0", "playden/import"},
      {"usb1", "/mnt/usb1", "playden/import"},
      {"usb2", "/mnt/usb2", "playden/import"},
      {"usb3", "/mnt/usb3", "playden/import"},
      {"usb4", "/mnt/usb4", "playden/import"},
      {"usb5", "/mnt/usb5", "playden/import"},
      {"usb6", "/mnt/usb6", "playden/import"},
      {"usb7", "/mnt/usb7", "playden/import"},
  };
  return roots;
}

CandidateScan ScanImportRoot(const std::string& root_path) {
  CandidateScan scan;
  const int root = platform::OpenDirectoryWalk(Trimmed(root_path));
  if (root < 0) {
    scan.error = errno;
    scan.status = StatusForOpen(scan.error);
    // No drive, or no import folder on it: nothing to offer.
    if (scan.status == ScanStatus::kMissing) { scan.status = ScanStatus::kOk; scan.error = 0; }
    return scan;
  }
  Frame frame;
  if (!OpenFrame(root, {}, 0, &frame)) {
    scan.error = errno;
    scan.status = StatusForOpen(scan.error);
    return scan;
  }
  std::size_t entries = 0;
  scan.status = ScanStatus::kOk;
  for (;;) {
    errno = 0;
    const auto* entry = ::readdir(frame.listing);
    if (!entry) {
      if (errno != 0) { scan.error = errno; scan.status = ScanStatus::kFailed; }
      break;
    }
    const std::string name = entry->d_name;
    if (name == "." || name == "..") continue;
    if (++entries > kMaxImportRootEntries) { scan.limit_reached = true; break; }
    if (name.front() == '.' || entry->d_type == DT_LNK) continue;
    if (entry->d_type != DT_DIR && entry->d_type != DT_UNKNOWN) continue;
    const int child = platform::OpenWalkChild(frame.fd, name.c_str(), kOpenFolder,
                                              static_cast<std::uint64_t>(entry->d_fileno));
    if (child < 0) {
      if (Unsupported(errno)) { scan.error = errno; scan.status = ScanStatus::kUnsupported; break; }
      continue;  // a link, a file, or gone
    }
    ::close(child);
    if (scan.folders.size() >= kMaxImportCandidates) { scan.limit_reached = true; break; }
    scan.folders.push_back(name);
  }
  Close(frame);
  if (scan.status != ScanStatus::kOk) scan.folders.clear();
  std::sort(scan.folders.begin(), scan.folders.end());
  return scan;
}

}  // namespace playden::stores::local
