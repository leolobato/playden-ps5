#include "steam/backend/depot_installer.h"

#include "steam/backend/depot_chunk.h"
#include "steam/backend/gzip.h"
#include "steam/install_volumes.h"

#include <mbedtls/sha1.h>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <limits>
#include <condition_variable>
#include <mutex>
#include <pthread.h>
#include <charconv>
#include <map>
#include <memory>
#include <set>

namespace playden::steam {
namespace {
// An imported game's copy journal (stores/local/copy_plan.h kCopyJournalDir).
constexpr const char* kCopyJournalDir = ".playden-copy";

using Clock = std::chrono::steady_clock;
struct ScopeExit {
  std::function<void()> fn;
  ~ScopeExit() { fn(); }
};
// Accumulate full clock precision, converting only when a snapshot is emitted.
template <class Fn>
auto Timed(Clock::duration& elapsed, Fn fn) -> decltype(fn()) {
  const auto start = Clock::now();
  ScopeExit end{[&] { elapsed += Clock::now() - start; }};
  return fn();
}
std::uint64_t Milliseconds(Clock::duration elapsed) {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
}

// Fastest: one chunk request, as a worker saw it.
struct ServerSample {
  std::size_t server = 0;
  std::uint64_t bytes = 0, transfer_ms = 0;  // manifest compressed size; total minus time to first byte
  bool timed = false, new_connection = false, failed = false;
};

bool Scored(const ServerSample& sample) {
  return sample.timed && !sample.failed && !sample.new_connection && sample.bytes >= DepotInstaller::kRaceChunkBytes;
}

double SampleRate(const ServerSample& sample) {  // MB/s
  return static_cast<double>(sample.bytes) / static_cast<double>(std::max<std::uint64_t>(sample.transfer_ms, 1)) / 1000.0;
}

double Median(std::vector<double> values) {
  if (values.empty()) return 0;
  std::sort(values.begin(), values.end());
  const auto middle = values.size() / 2;
  return values.size() % 2 ? values[middle] : (values[middle - 1] + values[middle]) / 2;
}

std::string Hex(const std::array<std::uint8_t, 20>& bytes) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string text;
  for (const auto byte : bytes) {
    text.push_back(kDigits[byte >> 4]);
    text.push_back(kDigits[byte & 15]);
  }
  return text;
}

// A string value for the install record, JSON-escaped; `fallback` when empty.
std::string RecordText(const std::string& value, const char* fallback) {
  std::string text;
  for (const char c : value.empty() ? std::string(fallback) : value) {
    if (c == '"' || c == '\\') {
      text += '\\';
      text += c;
    } else if (static_cast<unsigned char>(c) < 0x20) {
      char escaped[8];
      std::snprintf(escaped, sizeof(escaped), "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
      text += escaped;
    } else {
      text += c;
    }
  }
  return text;
}

std::string Lower(std::string text) {
  for (auto& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return text;
}

bool MakeDirs(const std::string& path, std::uint64_t* created = nullptr) {
  if (path.empty()) return false;
  std::string current;
  std::size_t start = 0;
  if (path[0] == '/') { current = "/"; start = 1; }
  while (start <= path.size()) {
    const auto slash = path.find('/', start);
    const auto part = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
    if (!part.empty()) {
      if (current.size() > 1 || (current.size() == 1 && current[0] != '/')) current += '/';
      current += part;
      if (mkdir(current.c_str(), 0777) == 0) {
        if (created) ++*created;
      } else {
        struct stat info {};
        if (stat(current.c_str(), &info) != 0 || !S_ISDIR(info.st_mode)) return false;
      }
    }
    if (slash == std::string::npos) break;
    start = slash + 1;
  }
  return true;
}

std::string Parent(const std::string& path) {
  const auto slash = path.rfind('/');
  return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

bool WriteAt(const std::string& path, std::uint64_t file_size, std::uint64_t offset,
             const std::vector<std::uint8_t>& data) {
  const int fd = open(path.c_str(), O_WRONLY | O_CREAT, 0666);
  if (fd < 0) return false;
  bool ok = true;
  struct stat info {};
  if (fstat(fd, &info) != 0) ok = false;
  if (ok && static_cast<std::uint64_t>(info.st_size) != file_size &&
      ftruncate(fd, static_cast<off_t>(file_size)) != 0)
    ok = false;
  std::size_t written = 0;
  while (ok && written < data.size()) {
    const auto result = pwrite(fd, data.data() + written, data.size() - written,
                               static_cast<off_t>(offset + written));
    if (result <= 0) ok = false;
    else written += static_cast<std::size_t>(result);
  }
  if (close(fd) != 0) ok = false;
  return ok;
}

bool EnsureFileSize(const std::string& path, std::uint64_t size) {
  const int fd = open(path.c_str(), O_WRONLY | O_CREAT, 0666);
  if (fd < 0) return false;
  const bool ok = ftruncate(fd, static_cast<off_t>(size)) == 0;
  return close(fd) == 0 && ok;
}

std::uint64_t DefaultFreeBytes(const std::string& path) {
  const auto space = QueryVolumeSpace(path);
  if (space) return space->free_bytes;
  return VolumeSpaceSupported() ? 0 : std::numeric_limits<std::uint64_t>::max();
}

// Deletes a tree without following links. `error` keeps the first failure's errno.
// A folder's entries other than "." and "..", read before any is deleted: deleting while reading
// can make readdir skip entries. False when the folder cannot be opened.
bool ListEntries(const std::string& path, std::vector<std::string>& names) {
  DIR* dir = opendir(path.c_str());
  if (!dir) return false;
  while (dirent* entry = readdir(dir)) {
    std::string name = entry->d_name;
    if (name != "." && name != "..") names.push_back(std::move(name));
  }
  closedir(dir);
  return true;
}

bool RemoveTree(const std::string& path, int* error = nullptr) {
  const auto failed = [error] {
    if (error && *error == 0) *error = errno;
    return false;
  };
  struct stat info {};
  if (lstat(path.c_str(), &info) != 0) return errno == ENOENT || failed();
  if (!S_ISDIR(info.st_mode)) return unlink(path.c_str()) == 0 || failed();
  std::vector<std::string> names;
  if (!ListEntries(path, names)) return failed();
  bool ok = true;
  for (const auto& name : names) ok = RemoveTree(path + "/" + name, error) && ok;
  return (rmdir(path.c_str()) == 0 || failed()) && ok;
}

// Bytes the filesystem has allocated to a file, at most its size; 0 when absent.
std::uint64_t AllocatedBytes(const std::string& path, std::uint64_t size) {
  struct stat info {};
  if (stat(path.c_str(), &info) != 0 || info.st_blocks <= 0) return 0;
  return std::min<std::uint64_t>(size, static_cast<std::uint64_t>(info.st_blocks) * 512);
}

bool Exists(const std::string& path) {
  struct stat info {};
  return stat(path.c_str(), &info) == 0;
}

bool SameFile(const ManifestFile& left, const ManifestFile& right) {
  if (left.size != right.size || left.flags != right.flags || left.link_target != right.link_target ||
      left.sha_content != right.sha_content || left.chunks.size() != right.chunks.size())
    return false;
  for (std::size_t index = 0; index < left.chunks.size(); ++index)
    if (left.chunks[index].sha != right.chunks[index].sha) return false;
  return true;
}

InstallError FromCm(CmError error) {
  switch (error) {
    case CmError::kNone: return InstallError::kNone;
    case CmError::kCanceled: return InstallError::kCanceled;
    case CmError::kNoSession: return InstallError::kNoSession;
    case CmError::kRejected: return InstallError::kRejected;
    case CmError::kNetwork:
    case CmError::kTimeout: return InstallError::kNetwork;
    case CmError::kProtocol: return InstallError::kProtocol;
  }
  return InstallError::kProtocol;
}

using Placement = ManifestPlacement;

std::string JournalKey(std::uint32_t depot, const std::string& path, std::uint64_t offset) {
  return std::to_string(depot) + "\t" + std::to_string(offset) + "\t" + path;
}

// Updates. Names in the work folder (DepotInstaller::kUpdateDir).
constexpr const char* kUpdateStaging = "staging";
constexpr const char* kUpdateStaged = "staged";
constexpr const char* kUpdateOld = "old";
constexpr const char* kUpdateFiles = "files";
constexpr const char* kUpdateTree = "tree";
constexpr const char* kUpdateRecord = "record";
constexpr const char* kUpdatePaths = "paths";
constexpr const char* kSteamFacts = ".playden-steam";  // runtime/steamworks.h kSteamworksFactsFile

std::string UpdateDir(const std::string& root) { return root + "/" + DepotInstaller::kUpdateDir; }

bool WriteText(const std::string& path, const std::string& text) {
  const auto temporary = path + ".tmp";
  FILE* file = std::fopen(temporary.c_str(), "w");
  if (!file) return false;
  bool ok = text.empty() || std::fwrite(text.data(), 1, text.size(), file) == text.size();
  ok = std::fclose(file) == 0 && ok;
  ok = ok && std::rename(temporary.c_str(), path.c_str()) == 0;
  if (!ok) std::remove(temporary.c_str());
  return ok;
}

// A whole file, read through a heap buffer; false when it can't be read (missing included).
bool ReadText(const std::string& path, std::string* text) {
  FILE* file = std::fopen(path.c_str(), "rb");
  if (!file) return false;
  text->clear();
  std::vector<char> buffer(64 * 1024);
  for (std::size_t read = 0; (read = std::fread(buffer.data(), 1, buffer.size(), file)) > 0;) text->append(buffer.data(), read);
  const bool ok = std::ferror(file) == 0;
  std::fclose(file);
  return ok;
}

std::vector<std::string> Lines(const std::string& text) {
  std::vector<std::string> lines;
  std::size_t start = 0;
  while (start < text.size()) {
    auto end = text.find('\n', start);
    if (end == std::string::npos) end = text.size();
    auto line = text.substr(start, end - start);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (!line.empty()) lines.push_back(std::move(line));
    start = end + 1;
  }
  return lines;
}

// A game path an update may touch: relative, without "." or ".." parts, and not one of Playden's own
// files at the game's root.
bool GamePath(const std::string& path) {
  if (path.empty() || path[0] == '/' || path.compare(0, 8, ".playden") == 0) return false;
  std::size_t start = 0;
  while (start <= path.size()) {
    const auto slash = path.find('/', start);
    const auto part = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
    if (part.empty() || part == "." || part == "..") return false;
    if (slash == std::string::npos) break;
    start = slash + 1;
  }
  return true;
}

// A file list (DepotInstaller::kFileList format): 'f' or 'd', a tab, a path.
struct ListedPath {
  char kind = 'f';
  std::string path;
};
std::vector<ListedPath> ReadFileList(const std::string& path) {
  std::vector<ListedPath> entries;
  std::string text;
  if (!ReadText(path, &text)) return entries;
  for (auto& line : Lines(text)) {
    if (line.size() < 3 || line[1] != '\t' || (line[0] != 'f' && line[0] != 'd')) continue;
    auto entry_path = line.substr(2);
    if (GamePath(entry_path)) entries.push_back({line[0], std::move(entry_path)});
  }
  return entries;
}

// A finished install's tree (DepotInstaller::kFileTree), by lowercase path; nothing when it is missing
// or isn't a "v1" tree.
struct TreeFile {
  std::string sha;
  std::uint64_t size = 0;
  std::string path;
};
std::optional<std::map<std::string, TreeFile>> ReadFileTree(const std::string& path) {
  std::string text;
  if (!ReadText(path, &text)) return std::nullopt;
  const auto lines = Lines(text);
  if (lines.empty() || lines[0] != "v1") return std::nullopt;
  std::map<std::string, TreeFile> tree;
  for (std::size_t index = 1; index < lines.size(); ++index) {
    const auto& line = lines[index];
    const auto first = line.find('\t');
    const auto second = first == std::string::npos ? std::string::npos : line.find('\t', first + 1);
    if (second == std::string::npos) continue;
    TreeFile file;
    file.sha = line.substr(0, first);
    const auto size = std::from_chars(line.data() + first + 1, line.data() + second, file.size);
    if (size.ec != std::errc{} || size.ptr != line.data() + second) continue;
    file.path = line.substr(second + 1);
    tree[Lower(file.path)] = std::move(file);
  }
  return tree;
}

// Whether the file at `path` is a regular file of `size` bytes whose SHA-1 is `sha` (read through a
// heap buffer; worker stacks are small). An empty file needs no hash.
bool SameContent(const std::string& path, std::uint64_t size, const std::array<std::uint8_t, 20>& sha,
                 const std::atomic<bool>& cancel) {
  struct stat info {};
  if (lstat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode) || static_cast<std::uint64_t>(info.st_size) != size)
    return false;
  if (size == 0) return true;
  FILE* file = std::fopen(path.c_str(), "rb");
  if (!file) return false;
  std::vector<std::uint8_t> buffer(1024 * 1024);
  mbedtls_sha1_context context;
  mbedtls_sha1_init(&context);
  bool ok = mbedtls_sha1_starts(&context) == 0;
  std::uint64_t total = 0;
  for (std::size_t read = 0; ok && !cancel.load() && (read = std::fread(buffer.data(), 1, buffer.size(), file)) > 0;) {
    ok = mbedtls_sha1_update(&context, buffer.data(), read) == 0;
    total += read;
  }
  ok = ok && !cancel.load() && std::ferror(file) == 0 && total == size;
  std::array<std::uint8_t, 20> digest{};
  ok = ok && mbedtls_sha1_finish(&context, digest.data()) == 0;
  mbedtls_sha1_free(&context);
  std::fclose(file);
  return ok && digest == sha;
}

bool IsDirectory(const std::string& path) {
  struct stat info {};
  return lstat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

bool Present(const std::string& path) {
  struct stat info {};
  return lstat(path.c_str(), &info) == 0;
}

// Whether every folder above the game path <root>/<path> is a real folder (lstat: a link isn't). An update
// never deletes or moves anything through a link, which could lead out of the game's folder.
bool RealParents(const std::string& root, const std::string& path) {
  for (auto parent = Parent(path); !parent.empty(); parent = Parent(parent))
    if (!IsDirectory(root + "/" + parent)) return false;
  return true;
}

struct UpdatePlan {
  std::vector<ListedPath> old_list, new_list;
  std::set<std::string> old_files;  // lowercase paths of the old version's files
  std::set<std::string> new_files;  // lowercase paths of the new version's files
  std::set<std::string> new_dirs;
  std::map<std::string, std::string> new_spelling;  // lowercase -> path as the new version names it
};

UpdatePlan ReadUpdatePlan(const std::string& work) {
  UpdatePlan plan;
  plan.old_list = ReadFileList(work + "/" + kUpdateOld);
  plan.new_list = ReadFileList(work + "/" + kUpdateFiles);
  for (const auto& entry : plan.old_list)
    if (entry.kind == 'f') plan.old_files.insert(Lower(entry.path));
  for (const auto& entry : plan.new_list) {
    const auto key = Lower(entry.path);
    (entry.kind == 'd' ? plan.new_dirs : plan.new_files).insert(key);
    plan.new_spelling[key] = entry.path;
  }
  return plan;
}

// Makes the folder <root>/<path> and its parents, from the top. Where one of them is something else, only a
// file of the old version that the new one doesn't have is deleted to make way; anything else (a file the
// game wrote, a link) stays and the folder can't be made: `blocked` then names it. With `apply` false
// nothing changes, it only says whether the folder could be made.
bool MakeGameDirs(const std::string& root, const std::string& path, const UpdatePlan& plan, std::string* blocked,
                  bool apply = true) {
  std::size_t start = 0;
  while (start <= path.size()) {
    const auto slash = path.find('/', start);
    const auto prefix = path.substr(0, slash);
    const auto full = root + "/" + prefix;
    struct stat info {};
    const bool present = lstat(full.c_str(), &info) == 0;
    if (present && !S_ISDIR(info.st_mode)) {
      const auto key = Lower(prefix);
      if (S_ISLNK(info.st_mode) || plan.new_files.count(key) || !plan.old_files.count(key)) {
        if (blocked) *blocked = prefix;
        return false;
      }
      if (!apply) return true;  // the old version's file goes; nothing is under it
      if (unlink(full.c_str()) != 0) {
        if (blocked) *blocked = prefix;
        return false;
      }
    } else if (!present && !apply) {
      return true;
    }
    if (apply && mkdir(full.c_str(), 0777) != 0 && !IsDirectory(full)) {
      if (blocked) *blocked = prefix;
      return false;
    }
    if (slash == std::string::npos) break;
    start = slash + 1;
  }
  return true;
}

// Clears the way for a file at <root>/<path> where the old version had a folder: the old version's files
// under it that the new one doesn't have go, then the folders left empty. Something else in there
// (written by the game) keeps the folder, and the update fails there. Nothing under a link is touched.
bool ClearFolderForFile(const std::string& root, const std::string& path, const UpdatePlan& plan) {
  const auto key = Lower(path) + "/";
  std::vector<std::string> folders;
  for (const auto& entry : plan.old_list) {
    if (Lower(entry.path).compare(0, key.size(), key) != 0 || !RealParents(root, entry.path)) continue;
    if (entry.kind == 'd') {
      folders.push_back(entry.path);
    } else if (!plan.new_files.count(Lower(entry.path))) {
      std::remove((root + "/" + entry.path).c_str());
      for (auto parent = Parent(entry.path); parent.size() > path.size(); parent = Parent(parent)) folders.push_back(parent);
    }
  }
  std::sort(folders.begin(), folders.end(),
            [](const std::string& left, const std::string& right) { return left.size() > right.size(); });
  for (const auto& folder : folders)
    if (RealParents(root, folder)) rmdir((root + "/" + folder).c_str());
  return rmdir((root + "/" + path).c_str()) == 0;
}

}  // namespace

DownloadTuning ReadDownloadTuning(const std::string& path) {
  DownloadTuning tuning;
  FILE* input = std::fopen(path.c_str(), "r");
  if (!input) return tuning;
  ScopeExit close{[&] { std::fclose(input); }};
  char line[272];
  bool seen_slots = false, seen_ahead = false, seen_server = false;
  while (std::fgets(line, sizeof(line), input)) {
    std::string text(line);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    if (text.empty()) continue;
    const auto equals = text.find('=');
    if (equals == std::string::npos) return {};
    const auto key = text.substr(0, equals);
    if (key == "server" && !seen_server && text.substr(equals + 1) == kFastestDownloadServer) {
      seen_server = true;
      tuning.fastest = true;
      continue;
    }
    if (key == "server" && !seen_server && ValidCdnHost(text.substr(equals + 1))) {
      seen_server = true;
      tuning.preferred_server = text.substr(equals + 1);
      continue;
    }
    int value = 0;
    const auto parsed = std::from_chars(text.data() + equals + 1, text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) return {};
    if (key == "slots" && !seen_slots) {
      seen_slots = true;
      tuning.slots = std::clamp(value, 1, 8);
    } else if (key == "ahead" && !seen_ahead && value >= 1 && value <= 65536) {
      seen_ahead = true;
      tuning.ahead_files = value;
    } else return {};
  }
  if (std::ferror(input)) return {};
  return tuning;
}

const char* InstallErrorName(InstallError error) {
  switch (error) {
    case InstallError::kNone: return "none";
    case InstallError::kCanceled: return "canceled";
    case InstallError::kNoSession: return "no_session";
    case InstallError::kRejected: return "rejected";
    case InstallError::kNetwork: return "network";
    case InstallError::kCorrupt: return "corrupt";
    case InstallError::kNoSpace: return "no_space";
    case InstallError::kStorage: return "storage";
    case InstallError::kProtocol: return "protocol";
    case InstallError::kGameRunning: return "game_running";
  }
  return "protocol";
}

DepotMerge MergeDepotManifests(const std::vector<DepotManifest>& manifests,
                               const std::function<void(const char* result, const std::string& detail)>& log) {
  DepotMerge merged;
  const auto entry = [&manifests](std::size_t depot, const ManifestFile& file) {
    std::string text = "depot=" + std::to_string(manifests[depot].depot_id) + " ";
    if (file.is_directory()) text += "folder";
    else if (file.is_symlink()) text += "link";
    else text += "file size=" + std::to_string(file.size);
    return text + " path=" + file.path;
  };
  auto& placements = merged.placements;
  for (std::size_t depot = 0; depot < manifests.size(); ++depot) {
    for (const auto& file : manifests[depot].files) {
      const auto key = Lower(file.path);
      const auto found = placements.find(key);
      if (found != placements.end()) {
        if (SameFile(*found->second.file, file)) continue;
        if (found->second.file->is_directory() != file.is_directory()) {
          const auto& folder = file.is_directory() ? file : *found->second.file;
          const auto& other = file.is_directory() ? *found->second.file : file;
          const auto detail = entry(found->second.depot, *found->second.file) + " | " + entry(depot, file);
          if (other.is_symlink()) {
            if (log) log("link-over-folder", detail);
            merged.conflict = file.path;
            return merged;
          }
          if (log) log("file-over-folder", detail);
          merged.files_over_folders.emplace_back(key, folder.path);
          if (file.is_directory()) continue;
        }
      }
      placements[key] = {depot, &file};
    }
  }
  for (const auto& [key, folder] : merged.files_over_folders) {
    const auto& winner = placements.at(key);
    const auto inside = placements.lower_bound(key + "/");
    if (inside != placements.end() && inside->first.compare(0, key.size() + 1, key + "/") == 0) {
      if (log) log("folder-not-empty", entry(winner.depot, *winner.file) + " | " + entry(inside->second.depot, *inside->second.file));
      merged.conflict = winner.file->path;
      return merged;
    }
  }
  for (const auto& [key, placement] : placements) {
    if (placement.file->is_directory() || placement.file->is_symlink()) continue;
    merged.install_bytes += placement.file->size;
    for (const auto& chunk : placement.file->chunks) merged.download_bytes += chunk.compressed_size;
  }
  return merged;
}

DepotInstaller::DepotInstaller(CmClient& cm, CdnClient& cdn) {
  hooks_.access = [&cm](const StoredSession& session, std::uint32_t app_id,
                        const std::vector<DepotRequest>& depots, const std::atomic<bool>& cancel) {
    return cm.FetchDepotAccess(session, app_id, depots, cancel);
  };
  // Which check a manifest that arrived failed, for the install log; empty after a network failure.
  auto manifest_problem = std::make_shared<std::string>();
  hooks_.servers = [&cdn, manifest_problem](std::uint32_t cell, const std::atomic<bool>& cancel,
                                            std::vector<ContentServer>* out) {
    manifest_problem->clear();  // a manifest problem from an earlier run must not hide why this list failed
    return cdn.Servers(cell, cancel, out);
  };
  hooks_.manifest = [&cdn, manifest_problem](const ContentServer& server, const DepotGrant& grant,
                                             const std::atomic<bool>& cancel, DepotManifest* out) {
    manifest_problem->clear();
    std::vector<std::uint8_t> zip;
    const auto fetched = cdn.Manifest(server, grant.depot_id, grant.manifest_gid, grant.manifest_request_code,
                                      cancel, &zip);
    if (fetched == CdnError::kCanceled) return InstallError::kCanceled;
    if (fetched != CdnError::kNone) return InstallError::kNetwork;
    const auto corrupt = [&](std::string what) {
      *manifest_problem = "depot=" + std::to_string(grant.depot_id) + " bytes=" + std::to_string(zip.size()) + " " + what;
      return InstallError::kCorrupt;
    };
    std::vector<std::uint8_t> raw;
    if (!UnzipFirstEntry(zip, &InflateRaw, &raw)) return corrupt("unzip");
    if (!ParseDepotManifest(raw, out)) return corrupt("parse raw=" + std::to_string(raw.size()));
    if (out->filenames_encrypted && !DecryptManifestFilenames(out, grant.key, &SymmetricDecrypt))
      return corrupt("filenames");
    if (out->depot_id != grant.depot_id || out->gid != grant.manifest_gid)
      return corrupt("ids got_depot=" + std::to_string(out->depot_id) + " got_gid=" + std::to_string(out->gid));
    std::string reason;
    if (!ValidateDepotManifest(*out, &reason)) return corrupt("invalid " + reason);
    return InstallError::kNone;
  };
  const auto ca_bundle = cdn.ca_bundle();
  hooks_.make_fetcher = [ca_bundle] {
    auto client = std::make_shared<CdnClient>(ca_bundle);
    InstallerHooks::ChunkFetcher fetcher;
    fetcher.fetch = [client](const ContentServer& server, std::uint32_t depot, const std::string& sha,
                              std::size_t cap, const std::atomic<bool>& cancel, std::vector<std::uint8_t>* out) {
      return client->Chunk(server, depot, sha, cap, cancel, out);
    };
    fetcher.last_failure = [client] { return client->last_failure(); };
    fetcher.stats = [client] { return client->stats(); };
    return fetcher;
  };
  hooks_.cdn_stats = [&cdn] { return cdn.stats(); };
  hooks_.free_bytes = &DefaultFreeBytes;
  hooks_.last_failure = [&cdn, manifest_problem] {
    return manifest_problem->empty() ? cdn.last_failure() : *manifest_problem;
  };
  hooks_.pause = [](int milliseconds, const std::atomic<bool>& cancel) {
    for (int waited = 0; waited < milliseconds && !cancel.load(); waited += 100) usleep(100 * 1000);
  };
}

DepotInstaller::DepotInstaller(InstallerHooks hooks) : hooks_(std::move(hooks)) {
  if (!hooks_.free_bytes) hooks_.free_bytes = &DefaultFreeBytes;
}

InstallMeasure DepotInstaller::Measure(const StoredSession& session, std::uint32_t app_id,
                                      const std::vector<DepotInfo>& depots, const std::string& branch,
                                      const std::atomic<bool>& cancel) {
  InstallMeasure measure;
  const auto fail = [&measure](InstallError error, std::string detail) {
    measure.error = error;
    measure.detail = std::move(detail);
    return measure;
  };
  if (app_id == 0 || depots.empty()) return fail(InstallError::kProtocol, "nothing to measure");
  std::vector<DepotRequest> requests;
  for (const auto& depot : depots) {
    if (!depot.manifest_gid || *depot.manifest_gid == 0) return fail(InstallError::kProtocol, "depot without manifest");
    requests.push_back({depot.id, *depot.manifest_gid, depot.depot_from_app.has_value(), depot.source_app, branch});
  }
  const auto access = hooks_.access(session, app_id, requests, cancel);
  if (!access.ok()) return fail(FromCm(access.error), access.detail.empty() ? "depot access failed" : access.detail);
  if (access.grants.empty()) return fail(InstallError::kRejected, "no depot granted");
  measure.partial = !access.skipped.empty();
  std::vector<ContentServer> servers;
  const auto listed = hooks_.servers(session.cell_id.value_or(0), cancel, &servers);
  if (listed == CdnError::kCanceled) return fail(InstallError::kCanceled, "canceled");
  if (listed != CdnError::kNone || servers.empty()) return fail(InstallError::kNetwork, "no content servers");
  // As Run: another server after a failure or a manifest that doesn't parse, with a pause between.
  std::vector<DepotManifest> manifests(access.grants.size());
  for (std::size_t index = 0; index < access.grants.size(); ++index) {
    InstallError fetched = InstallError::kNetwork;
    bool network_failed = false;
    const std::size_t attempts = std::min<std::size_t>(std::max<std::size_t>(servers.size() * 2, 4), 6);
    for (std::size_t attempt = 0; attempt < attempts; ++attempt) {
      fetched = hooks_.manifest(servers[attempt % servers.size()], access.grants[index], cancel, &manifests[index]);
      if (fetched != InstallError::kNetwork && fetched != InstallError::kCorrupt) break;
      network_failed = network_failed || fetched == InstallError::kNetwork;
      if (hooks_.pause && attempt + 1 < attempts) hooks_.pause(1000 << std::min<std::size_t>(attempt, 3), cancel);
      if (cancel.load()) return fail(InstallError::kCanceled, "canceled");
    }
    // As Run: broken only after a server failed to send it at all is a network problem, which a later try gets past.
    if (fetched == InstallError::kCorrupt && network_failed) fetched = InstallError::kNetwork;
    if (fetched != InstallError::kNone) return fail(fetched, "manifest unavailable");
  }
  const auto merged = MergeDepotManifests(manifests);
  if (!merged.conflict.empty()) return fail(InstallError::kCorrupt, "conflicting depots");
  measure.error = InstallError::kNone;
  measure.install_bytes = merged.install_bytes;
  measure.download_bytes = merged.download_bytes;
  return measure;
}

InstallOutcome DepotInstaller::Run(const StoredSession& session, const InstallTarget& target,
                                   const std::atomic<bool>& cancel,
                                   const std::function<void(const InstallProgress&)>& on_progress) {
  InstallOutcome outcome;
  InstallStats stats;
  const int slots = std::clamp(tuning_.slots, 1, 8);
  const int ahead_files = std::clamp(tuning_.ahead_files, 1, 65536);
  stats.slots = slots; stats.ahead_files = ahead_files;
  CdnStats worker_cdn;
  bool workers_running = false;
  CdnStats coordinator_cdn;
  const auto initial_cdn = hooks_.cdn_stats ? hooks_.cdn_stats() : CdnStats{};
  Clock::duration fetch{}, process{}, create{}, write{}, journal_time{}, backoff{};
  auto phase_start = Clock::now();
  Clock::time_point download_start{}, last_stats{};
  bool loop_finished = false;
  auto emit = [&](const char* phase) {
    if (!hooks_.stats) return;
    stats.fetch_ms = Milliseconds(fetch); stats.process_ms = Milliseconds(process);
    stats.create_ms = Milliseconds(create); stats.write_ms = Milliseconds(write);
    stats.journal_ms = Milliseconds(journal_time); stats.backoff_ms = Milliseconds(backoff);
    if (download_start != Clock::time_point{} && !loop_finished)
      stats.download_ms = Milliseconds(Clock::now() - download_start);
    {
      const auto current = workers_running ? coordinator_cdn : (hooks_.cdn_stats ? hooks_.cdn_stats() : CdnStats{});
#define CDN_DELTA(field) stats.field = current.field - initial_cdn.field
      CDN_DELTA(requests); CDN_DELTA(failures); CDN_DELTA(resolve_ms); CDN_DELTA(connect_ms);
      CDN_DELTA(tls_ms); CDN_DELTA(first_byte_ms); CDN_DELTA(total_ms); CDN_DELTA(new_connections); CDN_DELTA(bytes);
#undef CDN_DELTA
#define CDN_ADD(field) stats.field += worker_cdn.field
      CDN_ADD(requests); CDN_ADD(failures); CDN_ADD(resolve_ms); CDN_ADD(connect_ms);
      CDN_ADD(tls_ms); CDN_ADD(first_byte_ms); CDN_ADD(total_ms); CDN_ADD(new_connections); CDN_ADD(bytes);
#undef CDN_ADD
    }
    hooks_.stats(phase, stats);
  };
  ScopeExit end_stats{[&] { emit("end"); emit("servers"); }};
  auto fail = [&outcome](InstallError error, const char* detail) {
    outcome.error = error;
    outcome.detail = detail;
    return outcome;
  };
  if (target.app_id == 0 || target.install_root.empty() || target.install_root[0] != '/' ||
      target.depots.empty() || target.install_root.find("/..") != std::string::npos)
    return fail(InstallError::kProtocol, "invalid install target");
  if (cancel.load()) return fail(InstallError::kCanceled, "canceled");
  auto trace = [this](const char* step) { if (hooks_.trace) hooks_.trace(step); };
  const std::string update_dir = UpdateDir(target.install_root);
  if (target.update) {
    if (!Exists(target.install_root + "/" + kInstallRecord)) return fail(InstallError::kProtocol, "update without install");
    // Everything is staged already: only putting it in place is left.
    if (const auto pending = installs::ReadUpdateState(target.install_root);
        pending && pending->phase != installs::UpdatePhase::kStaging) {
      if (target.may_apply && !target.may_apply()) return fail(InstallError::kGameRunning, "the game runs");
      InstallProgress applying;
      applying.phase = InstallPhase::kApplying;
      if (on_progress) on_progress(applying);
      return FinishUpdate(target.install_root, cancel, hooks_.trace);
    }
  }
  trace("access");

  std::vector<DepotRequest> requests;
  for (const auto& depot : target.depots) {
    if (!depot.manifest_gid || *depot.manifest_gid == 0) return fail(InstallError::kProtocol, "depot without manifest");
    // Keys and request codes are asked for under the app that lists the depot: a DLC app for its own depots.
    // The manifest request code names the branch the manifest is from.
    requests.push_back({depot.id, *depot.manifest_gid, depot.depot_from_app.has_value(), depot.source_app, target.branch});
  }
  const auto access = hooks_.access(session, target.app_id, requests, cancel);
  if (!access.ok()) {
    outcome.error = FromCm(access.error);
    outcome.detail = access.detail.empty() ? "depot access failed" : access.detail;
    return outcome;
  }
  if (access.grants.empty() || access.grants.size() + access.skipped.size() != requests.size())
    return fail(InstallError::kProtocol, "incomplete depot access");
  outcome.skipped_depots = access.skipped;
  // Without any of the game's own content (its borrowed depots refused) an install would finish without the game,
  // and an update would delete it.
  if (std::none_of(access.grants.begin(), access.grants.end(), [&target](const DepotGrant& grant) {
        const auto depot = std::find_if(target.depots.begin(), target.depots.end(),
                                        [&grant](const DepotInfo& info) { return info.id == grant.depot_id; });
        return depot != target.depots.end() && (depot->owner_app == 0 || depot->owner_app == target.app_id);
      }))
    return fail(InstallError::kRejected, "no content of the game granted");

  stats.access_ms = Milliseconds(Clock::now() - phase_start);
  phase_start = Clock::now();
  std::vector<ContentServer> servers;
  const auto listed = hooks_.servers(session.cell_id.value_or(0), cancel, &servers);
  if (listed == CdnError::kCanceled) return fail(InstallError::kCanceled, "canceled");
  if (listed != CdnError::kNone || servers.empty()) {
    outcome.error = InstallError::kNetwork;
    outcome.detail = "no content servers";
    if (hooks_.last_failure) outcome.detail += " " + hooks_.last_failure();
    return outcome;
  }

  const bool fastest = tuning_.fastest;
  const auto order = PreferServer(&servers, fastest ? std::string() : tuning_.preferred_server);
  if (hooks_.server_list)
    hooks_.server_list(servers, fastest ? "fastest" : tuning_.preferred_server.empty() ? "auto"
                                : order.preferred_found ? "chosen" : "missing");
  std::vector<InstallServerStats> coordinator_servers;
  for (std::size_t i = 0; i < std::min<std::size_t>(servers.size(), 8); ++i)
    coordinator_servers.push_back({CdnServerLabel(servers[i].host), i});
  if (servers.size() > 8) coordinator_servers.push_back({"other", 8});
  std::vector<Clock::duration> coordinator_fetch(coordinator_servers.size());
  stats.server_stats = coordinator_servers;

  stats.servers_ms = Milliseconds(Clock::now() - phase_start);
  phase_start = Clock::now();
  trace("manifests");
  // Manifests, in mounting order.
  std::vector<DepotManifest> manifests(access.grants.size());
  for (std::size_t index = 0; index < access.grants.size(); ++index) {
    InstallError fetched = InstallError::kNetwork;
    bool network_failed = false;
    // A server that fails or sends a manifest that does not parse (a busy CDN can answer 502, or with a
    // cut-off body) is followed by another, after 1, 2, 4 and 8 s, as for chunks.
    const std::size_t attempts = std::min<std::size_t>(std::max<std::size_t>(servers.size() * 2, 4), 6);
    for (std::size_t attempt = 0; attempt < attempts; ++attempt) {
      const auto server_index = attempt % servers.size();
      const auto bucket = std::min<std::size_t>(server_index, 8);
      const auto before = hooks_.cdn_stats ? hooks_.cdn_stats() : CdnStats{};
      fetched = Timed(coordinator_fetch[bucket], [&] {
        return hooks_.manifest(servers[server_index], access.grants[index], cancel, &manifests[index]);
      });
      auto& server = coordinator_servers[bucket];
      ++server.requests;
      if (fetched != InstallError::kNone) ++server.failures;
      if (hooks_.cdn_stats) server.bytes += hooks_.cdn_stats().bytes - before.bytes;
      server.fetch_ms = Milliseconds(coordinator_fetch[bucket]);
      stats.server_stats = coordinator_servers;
      if (fetched != InstallError::kNetwork && fetched != InstallError::kCorrupt) break;
      network_failed = network_failed || fetched == InstallError::kNetwork;
      if (hooks_.pause && attempt + 1 < attempts) hooks_.pause(1000 << std::min<std::size_t>(attempt, 3), cancel);
      if (cancel.load()) return fail(InstallError::kCanceled, "canceled");
    }
    // A manifest that only ever came back broken is corrupt; one that some server could not send at all is a
    // network problem, which Resume or a later try gets past.
    if (fetched == InstallError::kCorrupt && network_failed) fetched = InstallError::kNetwork;
    if (fetched != InstallError::kNone) {
      outcome.error = fetched;
      outcome.detail = "manifest unavailable";
      if (hooks_.last_failure) outcome.detail += " " + hooks_.last_failure();
      return outcome;
    }
  }

  stats.manifests_ms = Milliseconds(Clock::now() - phase_start);
  phase_start = Clock::now();
  trace("merge");
  // As MergeDepotManifests lays the files out; every clash of a file and a folder is logged with both entries.
  std::size_t merge_lines = 0;
  const auto log_merge = [&](const char* result, const std::string& detail) {
    if (hooks_.merge && ++merge_lines <= kMergeLogLimit) hooks_.merge(result, detail);
  };
  auto merged = MergeDepotManifests(manifests, log_merge);
  if (!merged.conflict.empty()) return fail(InstallError::kCorrupt, ("conflicting depots " + merged.conflict).c_str());
  const auto& placements = merged.placements;
  for (const auto& [key, folder] : merged.files_over_folders) {
    const auto& winner = placements.at(key);
    // An earlier install may have left the folder on disk; an empty one makes way for the file. An update
    // leaves the game alone until it puts its files in place.
    if (target.update) continue;
    for (const auto& path : {folder, winner.file->path}) {
      const std::string full = target.install_root + "/" + path;
      struct stat info{};
      if (lstat(full.c_str(), &info) != 0 || !S_ISDIR(info.st_mode)) continue;
      if (rmdir(full.c_str()) != 0) {
        log_merge("disk-folder-kept", "path=" + path + " errno=" + std::to_string(errno));
        return fail(InstallError::kStorage, ("cannot replace folder " + path).c_str());
      }
      log_merge("disk-folder-removed", "path=" + path);
    }
  }
  if (merge_lines > kMergeLogLimit && hooks_.merge)
    hooks_.merge("more", "lines=" + std::to_string(merge_lines) + " logged=" + std::to_string(kMergeLogLimit));

  InstallProgress progress;
  // An update downloads only the files whose content changed (by lowercase path). Unchanged: the
  // previous tree has the same path with the same SHA-1 and size, and the file on disk has that size;
  // without a tree, the file on disk has the manifest's size and SHA-1 (hashing it: "Checking files").
  // A resumed update for the same manifests keeps the list it made.
  std::set<std::string> staged_keys;
  if (target.update) {
    const auto pending = installs::ReadUpdateState(target.install_root);
    bool same_target = pending && pending->depots.size() == access.grants.size();
    for (std::size_t index = 0; same_target && index < access.grants.size(); ++index)
      same_target = pending->depots[index].id == access.grants[index].depot_id &&
                    pending->depots[index].gid == access.grants[index].manifest_gid;
    std::string staged_text;
    if (same_target && ReadText(update_dir + "/" + kUpdateStaged, &staged_text)) {
      for (const auto& path : Lines(staged_text)) staged_keys.insert(Lower(path));
      trace("update-resume");
    } else {
      // Another target than an interrupted run's: what it staged (a file where this version has a folder,
      // say) and its journal go. Its "paths" stay: an uninstall still finds what it may have put in place.
      const auto staging = update_dir + "/" + kUpdateStaging;
      if (ReadText(update_dir + "/" + kUpdateStaged, &staged_text)) {
        std::vector<std::string> folders;
        for (const auto& path : Lines(staged_text)) {
          if (!GamePath(path)) continue;
          std::remove((staging + "/" + path).c_str());
          for (auto parent = Parent(path); !parent.empty(); parent = Parent(parent)) folders.push_back(parent);
        }
        std::sort(folders.begin(), folders.end(),
                  [](const std::string& left, const std::string& right) { return left.size() > right.size(); });
        for (const auto& folder : folders) rmdir((staging + "/" + folder).c_str());
      }
      int error = 0;
      if (!RemoveTree(staging, &error)) return fail(InstallError::kStorage, "cannot clear update staging");
      for (const char* name : {kUpdateStaged, "journal"})
        if (std::remove((update_dir + "/" + name).c_str()) != 0 && errno != ENOENT)
          return fail(InstallError::kStorage, "cannot clear update journal");
      const auto previous = ReadFileTree(target.install_root + "/" + kFileTree);
      if (!previous) {
        trace("update-check");
        progress.phase = InstallPhase::kCheckingFiles;
        if (on_progress) on_progress(progress);
      }
      for (const auto& [key, placement] : placements) {
        const auto& file = *placement.file;
        if (file.is_directory() || file.is_symlink()) continue;
        const auto path = target.install_root + "/" + file.path;
        bool same = false;
        if (previous) {
          const auto found = previous->find(key);
          struct stat info {};
          same = found != previous->end() && found->second.path == file.path && found->second.size == file.size &&
                 found->second.sha == Hex(file.sha_content) && lstat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode) &&
                 static_cast<std::uint64_t>(info.st_size) == file.size;
        } else {
          same = SameContent(path, file.size, file.sha_content, cancel);
        }
        if (cancel.load()) return fail(InstallError::kCanceled, "canceled");
        if (!same) staged_keys.insert(key);
      }
      progress.phase = InstallPhase::kDownloading;
    }
  }
  const auto downloads = [&](const std::string& key, const Placement& placement) {
    if (!target.update) return true;
    return !placement.file->is_directory() && !placement.file->is_symlink() && staged_keys.count(key) > 0;
  };
  for (const auto& [key, placement] : placements) {
    if (placement.file->is_directory() || placement.file->is_symlink()) continue;
    progress.install_total += placement.file->size;
    ++progress.files_total;
    if (!downloads(key, placement)) continue;
    for (const auto& chunk : placement.file->chunks) progress.download_total += chunk.compressed_size;
  }
  // What the finished install holds: its tree (each file the merge kept; a name with a line break can't be a
  // line, so updates treat such a file as new) and its record. Version 2 of the record adds the branch, the
  // content language and each depot's owner (the game or a DLC) and source (the app whose appinfo lists it).
  // Another store's game adds "store" and "game".
  const auto tree_text = [&] {
    std::string text = "v1\n";
    for (const auto& [key, placement] : placements) {
      const auto& file = *placement.file;
      if (file.is_directory() || file.is_symlink() || file.path.find_first_of("\r\n") != std::string::npos) continue;
      text += Hex(file.sha_content) + "\t" + std::to_string(file.size) + "\t" + file.path + "\n";
    }
    return text;
  };
  const auto list_text = [&] {
    std::string text;
    for (const auto& [key, placement] : placements)
      if (!placement.file->is_symlink())
        text += std::string(placement.file->is_directory() ? "d" : "f") + "\t" + placement.file->path + "\n";
    return text;
  };
  const auto record_text = [&] {
    std::string identity;
    if (target.game.valid() && target.game.store != Store::kSteam)
      identity = ",\"store\":\"" + std::string(Traits(target.game.store).key) + "\",\"game\":\"" + target.game.value + "\"";
    std::string text = "{\"version\":2,\"app_id\":" + std::to_string(target.app_id) + identity +
                       ",\"install_bytes\":" + std::to_string(progress.install_total) +
                       ",\"files\":" + std::to_string(progress.files_total) + ",\"branch\":\"" +
                       RecordText(target.branch, "public") + "\",\"language\":\"" + RecordText(target.language, "english") +
                       "\",\"depots\":[";
    for (std::size_t index = 0; index < access.grants.size(); ++index) {
      // Grants skip refused depots, so the selection is looked up by id.
      const auto& grant = access.grants[index];
      const auto depot = std::find_if(target.depots.begin(), target.depots.end(),
                                      [&grant](const DepotInfo& info) { return info.id == grant.depot_id; });
      const auto owner = depot != target.depots.end() && depot->owner_app ? depot->owner_app : target.app_id;
      const auto source = depot != target.depots.end() && depot->source_app ? depot->source_app : target.app_id;
      text += std::string(index ? "," : "") + "{\"id\":" + std::to_string(grant.depot_id) + ",\"manifest\":\"" +
              std::to_string(grant.manifest_gid) + "\",\"owner\":" + std::to_string(owner) +
              ",\"source\":" + std::to_string(source) + "}";
    }
    return text + "]}\n";
  };
  installs::UpdateState update_state;
  update_state.branch = target.branch.empty() ? "public" : target.branch;
  update_state.language = target.language.empty() ? "english" : target.language;
  for (const auto& grant : access.grants) update_state.depots.push_back({grant.depot_id, grant.manifest_gid, 0, 0});

  trace("journal");
  // Journal of chunks already written in a previous run.
  const std::string journal_dir = target.update ? update_dir : target.install_root + "/" + kJournalDir;
  const std::string journal_path = journal_dir + "/journal";
  // Where downloaded files go: the game's folder, or an update's staging folder.
  const std::string write_root = target.update ? update_dir + "/" + kUpdateStaging : target.install_root;
  if (!MakeDirs(journal_dir) || !MakeDirs(write_root)) return fail(InstallError::kStorage, "cannot create install folder");
  if (!target.update) {
    // Every path this install may create, so removal never has to list directories (the console's
    // readdir is unreliable).
    if (!WriteText(target.install_root + "/" + kFileList, list_text())) return fail(InstallError::kStorage, "cannot write file list");
  } else {
    // The update's plan, before anything is staged: every path either version has (for an uninstall in
    // between; never fewer than an earlier run listed), the game's file list, the changed files and what
    // the game gets at the end.
    std::string old_list;
    if (!ReadText(target.install_root + "/" + kFileList, &old_list) && Exists(target.install_root + "/" + kFileList))
      return fail(InstallError::kStorage, "cannot read file list");
    const auto new_list = list_text();
    std::string earlier;
    ReadText(update_dir + "/" + kUpdatePaths, &earlier);
    std::set<std::string> union_lines;
    const std::string* texts[] = {&earlier, &old_list, &new_list};
    for (const auto* text : texts)
      for (auto& line : Lines(*text)) union_lines.insert(std::move(line));
    std::string paths_text;
    for (const auto& line : union_lines) paths_text += line + "\n";
    std::string staged_text;
    for (const auto& key : staged_keys) {
      const auto found = placements.find(key);
      if (found == placements.end()) continue;
      if (found->second.file->path.find_first_of("\r\n") != std::string::npos)
        return fail(InstallError::kCorrupt, "unlistable file name");
      staged_text += found->second.file->path + "\n";
    }
    if (!WriteText(update_dir + "/" + kUpdatePaths, paths_text) || !WriteText(update_dir + "/" + kUpdateOld, old_list) ||
        !WriteText(update_dir + "/" + kUpdateFiles, new_list) || !WriteText(update_dir + "/" + kUpdateTree, tree_text()) ||
        !WriteText(update_dir + "/" + kUpdateRecord, record_text()) ||
        !WriteText(update_dir + "/" + kUpdateStaged, staged_text) ||
        !WriteText(update_dir + "/" + installs::kUpdateStateName, installs::FormatUpdateState(update_state)))
      return fail(InstallError::kStorage, "cannot write update plan");
    trace("update-plan");
  }
  std::string journal_header = "#v2 ";
  for (std::size_t index = 0; index < access.grants.size(); ++index) {
    if (index) journal_header += ',';
    journal_header += std::to_string(access.grants[index].depot_id) + ':' +
                      std::to_string(access.grants[index].manifest_gid);
  }
  std::set<std::string> done;
  bool has_header = false, nonempty_journal = false, reset_journal = false, first_line = true;
  bool unterminated_journal = false;
  if (FILE* input = std::fopen(journal_path.c_str(), "r")) {
    const auto read_line = [&](std::string entry) {
      while (!entry.empty() && (entry.back() == '\n' || entry.back() == '\r')) entry.pop_back();
      const bool at_start = first_line;
      first_line = false;
      if (entry.empty()) return;
      nonempty_journal = true;
      if (entry[0] == '#') {
        // Version headers must occur exactly once, on the first line. Other comments are ignored.
        const bool version_prefix = entry.compare(0, 2, "#v") == 0 || entry.compare(0, 2, "#V") == 0;
        const bool version_line = version_prefix && (entry.size() == 2 ||
            (entry.size() > 2 && std::isdigit(static_cast<unsigned char>(entry[2]))));
        if (version_line) {
          if (!at_start || has_header || entry != journal_header) reset_journal = true;
          has_header = true;
        }
        return;
      }
      done.insert(std::move(entry));
    };
    char buffer[8192];
    std::string line;
    bool oversized = false;
    const auto max_line = std::max<std::size_t>(journal_header.size() + 2, sizeof(buffer));
    while (std::fgets(buffer, sizeof(buffer), input)) {
      const std::string part(buffer);
      if (line.size() + part.size() > max_line) oversized = true;
      if (!oversized) line += part;
      if (!part.empty() && part.back() == '\n') {
        if (oversized) { reset_journal = true; nonempty_journal = true; first_line = false; }
        else read_line(std::move(line));
        line.clear(); oversized = false;
      }
    }
    if (oversized) { reset_journal = true; nonempty_journal = true; }
    else if (!line.empty()) { read_line(std::move(line)); unterminated_journal = true; }
    const bool read_ok = std::ferror(input) == 0;
    const bool closed = std::fclose(input) == 0;
    if (!read_ok || !closed) return fail(InstallError::kStorage, "cannot read journal");
  } else if (Exists(journal_path)) return fail(InstallError::kStorage, "cannot read journal");
  if (reset_journal || !nonempty_journal) {
    done.clear();
    const auto temporary = journal_path + ".tmp";
    FILE* header = std::fopen(temporary.c_str(), "w");
    if (!header) return fail(InstallError::kStorage, "cannot write journal");
    const bool written = std::fprintf(header, "%s\n", journal_header.c_str()) >= 0;
    const bool closed = std::fclose(header) == 0;
    if (!written || !closed || std::rename(temporary.c_str(), journal_path.c_str()) != 0) {
      std::remove(temporary.c_str());
      return fail(InstallError::kStorage, "cannot write journal");
    }
    unterminated_journal = false;
    if (reset_journal) trace("journal-reset");
  } else if (!has_header) trace("journal-legacy");

  // Space still needed: per file, the smaller of its unwritten chunks and its unallocated bytes.
  // A resumed install on a preallocating filesystem already holds its files at full size.
  std::uint64_t remaining_bytes = 0;
  std::map<std::string, std::uint64_t> allocated;
  for (const auto& [key, placement] : placements) {
    if (placement.file->is_directory() || placement.file->is_symlink() || !downloads(key, placement)) continue;
    const auto depot_id = manifests[placement.depot].depot_id;
    const auto path = write_root + "/" + placement.file->path;
    const auto held = AllocatedBytes(path, placement.file->size);
    allocated[key] = held;
    progress.held_bytes += held;
    bool pending = placement.file->chunks.empty() && !Exists(path);
    std::uint64_t unwritten = pending ? placement.file->size : 0;
    for (const auto& chunk : placement.file->chunks) {
      if (done.count(JournalKey(depot_id, placement.file->path, chunk.offset))) {
        progress.download_done += chunk.compressed_size;
        ++stats.chunks_skipped;
      } else {
        unwritten += chunk.original_size;
        ++stats.chunks_pending;
        stats.bytes_pending += chunk.compressed_size;
        pending = true;
      }
    }
    const auto needed = std::min(unwritten, placement.file->size - held);
    if (remaining_bytes > std::numeric_limits<std::uint64_t>::max() - needed)
      return fail(InstallError::kNoSpace, "install size exceeds available space");
    remaining_bytes += needed;
    if (pending) ++stats.files_pending;
  }
  trace("space");
  const auto available_bytes = hooks_.free_bytes(target.install_root);
  if (available_bytes < kInstallSpaceMargin || remaining_bytes > available_bytes - kInstallSpaceMargin)
    return fail(InstallError::kNoSpace, "not enough free space");
  if (on_progress) on_progress(progress);

  stats.prepare_ms = Milliseconds(Clock::now() - phase_start);
  stats.files_total = progress.files_total;
  stats.download_total = progress.download_total;
  emit("shape");
  trace("write");
  FILE* journal = std::fopen(journal_path.c_str(), "a");
  if (!journal) return fail(InstallError::kStorage, "cannot write journal");
  // A valid final line need not have a newline; separate it from newly appended chunk keys.
  if (unterminated_journal && (std::fprintf(journal, "\n") < 0 || std::fflush(journal) != 0)) {
    std::fclose(journal);
    return fail(InstallError::kStorage, "cannot write journal");
  }
  download_start = last_stats = Clock::now();
  auto report_progress = [&] {
    const auto now = Clock::now();
    if (now - last_stats >= std::chrono::seconds(60)) { emit("progress"); last_stats = now; }
  };
  struct FileWork {
    Placement placement;
    bool prepared = false;
    std::size_t pending = 0;
    std::uint64_t allocated = 0;  // counted in progress.held_bytes
  };
  struct ChunkWork { std::size_t file; const ManifestChunk* chunk; std::string journal_key; };
  std::vector<FileWork> files;
  std::vector<ChunkWork> chunks;
  for (const auto& [key, placement] : placements) {
    if (placement.file->is_symlink() || !downloads(key, placement)) continue;
    const auto index = files.size();
    const auto held = allocated.find(key);
    files.push_back({placement, false, 0, held == allocated.end() ? 0 : held->second});
    for (const auto& chunk : placement.file->chunks) {
      auto entry = JournalKey(access.grants[placement.depot].depot_id, placement.file->path, chunk.offset);
      if (done.count(entry)) continue;
      chunks.push_back({index, &chunk, std::move(entry)});
      ++files.back().pending;
    }
  }
  // Fastest: before the workers start, a short race on this download's largest chunks picks `primary`,
  // or the winner remembered from an earlier download does. Every worker starts each chunk at `primary`
  // and fails over from there. During the download the median rate of large requests to `primary` is
  // judged every kCheckWindowSeconds; two slow windows in a row start a shorter race (a re-race).
  const std::size_t server_count = servers.size();
  const bool checking = fastest && server_count > 1;
  const auto now = [this] { return hooks_.now ? hooks_.now() : Clock::now(); };
  const auto server_tag = [&servers](std::size_t index) {
    return CdnServerLabel(servers[index].host) + "#" + std::to_string(index);
  };
  enum class Probe { kNone, kRace, kRerace };
  struct ProbeState {
    Probe kind = Probe::kNone;
    std::uint64_t generation = 0;
    std::size_t race_end = 0;  // a race uses chunks [0, race_end); a re-race the next large chunks
    std::size_t in_flight = 0;
    bool started = false;
    Clock::time_point start{};
    std::vector<ServerSample> samples;
  };
  struct ProbeReport {
    const char* step = "race";
    const char* result = "";
    std::string detail;
    std::optional<std::string> remember;  // for remember_fastest; empty forgets
  };
  // Shared with the workers under `mutex` (declared below).
  std::size_t primary = 0;
  bool primary_chosen = order.preferred_found;  // a worker that failed over returns to primary after a while
  ProbeState probe;
  std::vector<ServerSample> window;  // scored requests since window_start
  Clock::time_point window_start{};
  // MB/s a check compares against: the first judged window after the start or a race, with every worker on
  // `primary`. Not the race's score: a race spreads the workers over the servers, so each request there gets
  // more of the line than once they all share one server.
  double baseline = 0;
  int sags = 0, reraces = 0;
  bool limit_logged = false;
  std::vector<ProbeReport> reports;  // logged by the coordinator, outside the lock
  if (fastest) {
    std::size_t large = 0;
    for (const auto& work : chunks) if (work.chunk->compressed_size >= kRaceChunkBytes) ++large;
    const char* skip = server_count == 1 ? "one-server"
        : stats.bytes_pending < kRaceMinPendingBytes ? "skipped-small"
        : large < 2 * server_count ? "skipped-no-large-chunks" : nullptr;
    std::optional<std::size_t> cached;
    if (hooks_.cached_fastest) {
      if (const auto host = hooks_.cached_fastest(); host && !host->empty()) {
        for (std::size_t i = 0; i < server_count && !cached; ++i)
          if (Lower(servers[i].host) == Lower(*host)) cached = i;
        if (!cached && hooks_.remember_fastest) hooks_.remember_fastest("");
      }
    }
    if (cached) { primary = *cached; primary_chosen = true; }
    if (skip || cached) {
      if (hooks_.race) hooks_.race("race", skip ? skip : "cached", "winner=" + (cached ? server_tag(*cached) : "none"));
    } else {
      // The largest chunks move to the front in their own order; the others keep theirs. Two chunks per
      // server visit with the first one unscored give chunks / 2 samples, so 8 per server.
      const auto race_chunks = std::min<std::size_t>(server_count * 8, 64);
      std::vector<std::size_t> candidates;
      for (std::size_t i = 0; i < chunks.size(); ++i)
        if (chunks[i].chunk->compressed_size >= kRaceChunkBytes) candidates.push_back(i);
      std::stable_sort(candidates.begin(), candidates.end(), [&](std::size_t left, std::size_t right) {
        return chunks[left].chunk->compressed_size > chunks[right].chunk->compressed_size;
      });
      candidates.resize(std::min(candidates.size(), race_chunks));
      std::vector<bool> raced(chunks.size());
      for (const auto i : candidates) raced[i] = true;
      std::vector<ChunkWork> ordered;
      ordered.reserve(chunks.size());
      for (std::size_t i = 0; i < chunks.size(); ++i) if (raced[i]) ordered.push_back(std::move(chunks[i]));
      for (std::size_t i = 0; i < chunks.size(); ++i) if (!raced[i]) ordered.push_back(std::move(chunks[i]));
      chunks.swap(ordered);
      probe.kind = Probe::kRace;
      probe.race_end = candidates.size();
    }
  }
  window_start = now();
  struct Worker {
    InstallerHooks::ChunkFetcher fetcher;
    pthread_t thread{};
    bool started = false, busy = false, ready = false;
    std::size_t work = 0;
    std::size_t served = 0;  // the server the ready chunk came from
    std::vector<std::uint8_t> plain;
    std::uint64_t compressed_bytes = 0, retries = 0, corrupt_retries = 0;
    Clock::duration fetch{}, process{}, backoff{};
    CdnStats cdn;
    std::vector<InstallServerStats> server_stats;
    std::function<void()> run;
  };
  std::vector<std::unique_ptr<Worker>> workers;
  std::mutex mutex;
  std::condition_variable changed;
  std::atomic<bool> stop{false};
  std::size_t next_chunk = 0, occupied = 0, waiting = 0;
  std::uint32_t peak_active = 0, peak_waiting = 0;
  InstallError first_error = InstallError::kNone;
  std::string first_detail;
  const auto fail_locked = [&](InstallError error, std::string detail) {
    if (first_error == InstallError::kNone) {
      first_error = error;
      first_detail = std::move(detail);
    }
    stop.store(true);
    changed.notify_all();
  };
  const auto publish_failure = [&](InstallError error, std::string detail) {
    std::lock_guard<std::mutex> lock(mutex);
    fail_locked(error, std::move(detail));
  };
  // Ends the race or re-race once every server has its samples (or keeps failing), its chunks are done
  // or its time is up. The winner has the best median rate; a server with more than half of its
  // requests failed gets none.
  const auto end_probe_locked = [&](Clock::time_point at) {
    if (probe.kind == Probe::kNone) return;
    const bool race = probe.kind == Probe::kRace;
    struct Score { std::size_t requests = 0, failures = 0; std::vector<double> rates; };
    std::vector<Score> scores(server_count);
    for (const auto& sample : probe.samples) {
      auto& score = scores[sample.server];
      ++score.requests;
      if (sample.failed) ++score.failures;
      else if (Scored(sample)) score.rates.push_back(SampleRate(sample));
    }
    const std::size_t need = race ? kRaceSamples : kReraceSamples;
    const bool settled = std::all_of(scores.begin(), scores.end(), [&](const Score& score) {
      return score.rates.size() >= need || score.failures >= need;
    });
    const bool drained = probe.in_flight == 0 && (next_chunk == chunks.size() || (race && next_chunk >= probe.race_end));
    const bool capped = probe.started &&
        at - probe.start >= std::chrono::seconds(race ? kRaceCapSeconds : kReraceCapSeconds);
    if (!settled && !drained && !capped) return;
    std::optional<std::size_t> winner;
    double best = 0;
    std::size_t scored = 0;
    std::string rates;
    for (std::size_t i = 0; i < server_count; ++i) {
      const auto& score = scores[i];
      scored += score.rates.size();
      const bool failing = score.failures * 2 > score.requests;
      const auto rate = failing ? 0.0 : Median(score.rates);
      if (!failing && !score.rates.empty() && (!winner || rate > best)) { winner = i; best = rate; }
      if (i >= 8 || score.requests == 0 || (!failing && score.rates.empty())) continue;
      char text[24] = "failed";
      if (!failing) std::snprintf(text, sizeof(text), "%.1f", rate);
      rates += (rates.empty() ? "" : ",") + server_tag(i) + ":" + text;
    }
    ProbeReport report;
    report.step = race ? "race" : "rerace";
    report.result = winner ? "won" : "timeout";
    report.detail = "winner=" + (winner ? server_tag(*winner) : "none") + " rates=" + (rates.empty() ? "none" : rates) +
                    " samples=" + std::to_string(scored) +
                    " race_ms=" + std::to_string(probe.started ? Milliseconds(at - probe.start) : 0);
    if (winner) {
      primary = *winner;
      primary_chosen = true;
      report.remember = servers[*winner].host;
    } else if (!race) {
      report.remember = std::string();  // the remembered winner no longer holds
    }
    reports.push_back(std::move(report));
    probe.kind = Probe::kNone;
    probe.started = false;
    probe.in_flight = 0;
    probe.samples.clear();
    ++probe.generation;
    window.clear();
    window_start = at;
    sags = 0;
    baseline = 0;
  };
  const auto check_locked = [&](Clock::time_point at) {
    if (!checking || probe.kind != Probe::kNone || at - window_start < std::chrono::seconds(kCheckWindowSeconds)) return;
    std::vector<double> rates;
    for (const auto& sample : window) if (sample.server == primary) rates.push_back(SampleRate(sample));
    window.clear();
    window_start = at;
    if (rates.size() < kCheckMinSamples) return;  // tiny files, or a writer that holds the workers
    const auto rate = Median(rates);
    if (baseline <= 0) { baseline = rate; return; }
    if (rate >= baseline / 2) { sags = 0; return; }
    if (++sags < 2) return;
    sags = 0;
    if (reraces >= kReraceLimit) {
      if (!limit_logged) reports.push_back({"rerace", "limit", "winner=" + server_tag(primary), std::nullopt});
      limit_logged = true;
      return;
    }
    ++reraces;
    probe.kind = Probe::kRerace;
    probe.started = true;
    probe.start = at;
  };
  // Workers publish their own counters under this lock. Never query their clients here.
  const auto snapshot = [&] {
    std::lock_guard<std::mutex> lock(mutex);
    fetch = {}; process = {}; backoff = {}; worker_cdn = {};
    stats.peak_active = peak_active; stats.peak_waiting = peak_waiting;
    stats.chunk_retries = stats.corrupt_retries = 0;
    stats.server_stats = coordinator_servers;
    for (const auto& worker : workers) {
      fetch += worker->fetch; process += worker->process; backoff += worker->backoff;
      stats.chunk_retries += worker->retries; stats.corrupt_retries += worker->corrupt_retries;
      for (std::size_t i = 0; i < worker->server_stats.size(); ++i) {
        auto& server = stats.server_stats[i];
        const auto& published = worker->server_stats[i];
        server.requests += published.requests; server.failures += published.failures;
        server.bytes += published.bytes; server.fetch_ms += published.fetch_ms;
      }
#define SUM_CDN(field) worker_cdn.field += worker->cdn.field
      SUM_CDN(requests); SUM_CDN(failures); SUM_CDN(resolve_ms); SUM_CDN(connect_ms); SUM_CDN(tls_ms);
      SUM_CDN(first_byte_ms); SUM_CDN(total_ms); SUM_CDN(new_connections); SUM_CDN(bytes);
#undef SUM_CDN
    }
  };
  const auto join_workers = [&] {
    stop.store(true);
    changed.notify_all();
    for (auto& worker : workers) {
      if (worker->started) { pthread_join(worker->thread, nullptr); worker->started = false; }
    }
    workers_running = false;
    snapshot();
  };
  ScopeExit joined{join_workers};
  ScopeExit close_journal{[&] { if (journal) std::fclose(journal); }};
  coordinator_cdn = hooks_.cdn_stats ? hooks_.cdn_stats() : CdnStats{};
  workers_running = true;
  if (!chunks.empty()) trace("first-chunk");
  pthread_attr_t attr;
  const bool attr_ready = pthread_attr_init(&attr) == 0;
  const bool stack_ready = attr_ready && pthread_attr_setstacksize(&attr, 256 * 1024) == 0;
  if (!stack_ready && !chunks.empty()) publish_failure(InstallError::kNetwork, "cannot start workers");
  for (int index = 0; stack_ready && index < slots && !chunks.empty() && !stop.load(); ++index) {
    auto worker = std::make_unique<Worker>();
#if defined(__cpp_exceptions)
    try {
#endif
      if (hooks_.make_fetcher) worker->fetcher = hooks_.make_fetcher();
      else { worker->fetcher.fetch = hooks_.chunk; worker->fetcher.last_failure = hooks_.last_failure; }
#if defined(__cpp_exceptions)
    } catch (...) { publish_failure(InstallError::kNetwork, "cannot create fetcher"); break; }
#endif
    if (!worker->fetcher.fetch) { publish_failure(InstallError::kNetwork, "cannot create fetcher"); break; }
    auto* self = worker.get();
    self->run = [&, self, slot = static_cast<std::size_t>(index)] {
      // Destroy the HTTP client on the same thread that used it, after publishing its counters.
      ScopeExit release_fetcher{[self] { self->fetcher = {}; }};
      std::size_t server_cursor = 0, last_base = 0;
      std::uint64_t probe_generation = 0;
      std::size_t probe_taken = 0;
      std::vector<ServerSample> attempts;
      std::uint64_t chunks_since_failover = 0;
      unsigned failed_returns = 0;
      bool returning = false;
      // Both the counters and full-precision timers live on the heap, off the native worker stack.
      std::vector<InstallServerStats> server_stats = coordinator_servers;
      for (auto& server : server_stats) server.requests = server.failures = server.bytes = server.fetch_ms = 0;
      std::vector<Clock::duration> server_fetch(server_stats.size());
      Clock::duration fetch_time{}, process_time{}, backoff_time{};
      std::uint64_t retries = 0, corrupt_retries = 0;
#if defined(__cpp_exceptions)
      try {
#endif
        while (!stop.load()) {
          std::size_t base = 0;
          bool probe_chunk = false, returns = false;
          std::uint64_t chunk_generation = 0;
          {
            std::unique_lock<std::mutex> lock(mutex);
            changed.wait(lock, [&] { return stop.load() || !self->busy; });
            if (stop.load() || next_chunk == chunks.size()) break;
            self->work = next_chunk++;
            self->busy = true;
            ++occupied;
            peak_active = std::max(peak_active, static_cast<std::uint32_t>(occupied));
            base = primary;
            returns = primary_chosen;
            // A race goes round the servers, two chunks in a row on each: the first may open the connection.
            if (probe.kind == Probe::kRace ? self->work < probe.race_end
                : probe.kind == Probe::kRerace && chunks[self->work].chunk->compressed_size >= kRaceChunkBytes) {
              if (probe_generation != probe.generation) { probe_generation = probe.generation; probe_taken = 0; }
              base = (slot + probe_taken++ / 2) % server_count;
              probe_chunk = true;
              chunk_generation = probe.generation;
              ++probe.in_flight;
              if (!probe.started) { probe.started = true; probe.start = now(); }
            }
          }
          if (!probe_chunk && base != last_base) {
            // A new primary: start there, as a new download would.
            last_base = base;
            server_cursor = 0;
            chunks_since_failover = 0;
            failed_returns = 0;
            returning = false;
          }
          // A race chunk fails over from its own server and leaves the worker's failover state alone.
          std::size_t probe_cursor = 0;
          auto& cursor = probe_chunk ? probe_cursor : server_cursor;
          if (!probe_chunk && returns && server_cursor % server_count != 0 && failed_returns < 4 &&
              chunks_since_failover >= (32u << failed_returns)) {
            server_cursor = 0;
            returning = true;
          }
          const auto failed_over = [&] {
            if (!probe_chunk && returning && server_cursor % server_count == 0) { ++failed_returns; returning = false; }
            ++cursor;
            if (!probe_chunk) chunks_since_failover = 0;
          };
          const auto& work = chunks[self->work];
          const auto& placement = files[work.file].placement;
          const auto& grant = access.grants[placement.depot];
          const auto& chunk = *work.chunk;
          InstallError result = InstallError::kNetwork;
          std::string detail;
          std::vector<std::uint8_t> plain;
          std::uint64_t compressed_bytes = 0;
          const auto attempts_allowed = static_cast<std::size_t>(kChunkAttempts) * std::min<std::size_t>(server_count, 2);
          attempts.clear();
          for (std::size_t attempt = 0; attempt < attempts_allowed && !stop.load(); ++attempt) {
            if (attempt > 0) ++retries;
            std::vector<std::uint8_t> encrypted;
            const auto server_index = (base + cursor) % server_count;
            const auto bucket = std::min<std::size_t>(server_index, 8);
            const auto before = self->fetcher.stats ? self->fetcher.stats() : CdnStats{};
            const auto fetched = Timed(fetch_time, [&] {
              return Timed(server_fetch[bucket], [&] {
                return self->fetcher.fetch(servers[server_index], grant.depot_id, Hex(chunk.sha),
                                           static_cast<std::size_t>(chunk.compressed_size) + 4096, stop, &encrypted);
              });
            });
            const auto after = self->fetcher.stats ? self->fetcher.stats() : CdnStats{};
            auto& server = server_stats[bucket];
            ++server.requests;
            if (fetched != CdnError::kNone) ++server.failures;
            server.bytes += self->fetcher.stats ? after.bytes - before.bytes : encrypted.size();
            server.fetch_ms = Milliseconds(server_fetch[bucket]);
            if (fetched == CdnError::kCanceled || stop.load()) { result = InstallError::kCanceled; break; }
            // Transfer time only: connecting and waiting for the first byte are not the server's bandwidth.
            ServerSample sample;
            sample.server = server_index;
            sample.bytes = chunk.compressed_size;
            sample.timed = static_cast<bool>(self->fetcher.stats);
            sample.new_connection = after.new_connections > before.new_connections;
            const auto total_ms = after.total_ms - before.total_ms;
            sample.transfer_ms = total_ms - std::min(total_ms, after.first_byte_ms - before.first_byte_ms);
            sample.failed = fetched != CdnError::kNone;
            attempts.push_back(sample);
            if (fetched != CdnError::kNone) {
              failed_over();
              result = InstallError::kNetwork;
              if (hooks_.pause && attempt + 1 < attempts_allowed)
                Timed(backoff_time, [&] { hooks_.pause(1000 << std::min<std::size_t>(attempt, 3), stop); });
              continue;
            }
            const auto processed = Timed(process_time, [&] {
              return ProcessDepotChunk(encrypted, grant.key, chunk.original_size, chunk.crc, chunk.sha, &plain);
            });
            if (processed == ChunkError::kNone) {
              if (!probe_chunk && server_cursor % server_count == 0) {
                returning = false;
              } else if (!probe_chunk) ++chunks_since_failover;
              compressed_bytes = encrypted.size(); result = InstallError::kNone; break;
            }
            ++server.failures;  // received bytes that failed verification are unusable too
            attempts.back().failed = true;
            failed_over();
            if (attempt + 1 < attempts_allowed) ++corrupt_retries;
            result = InstallError::kCorrupt;
          }
          if (stop.load()) result = InstallError::kCanceled;
          if (result != InstallError::kNone) {
            detail = result == InstallError::kCanceled ? "canceled" : "chunk unavailable";
            if (result == InstallError::kNetwork && self->fetcher.last_failure) {
              const auto failure = self->fetcher.last_failure();
              if (!failure.empty()) detail += " " + failure;
            }
          }
          const auto cdn = self->fetcher.stats ? self->fetcher.stats() : CdnStats{};
          {
            std::lock_guard<std::mutex> lock(mutex);
            self->fetch = fetch_time; self->process = process_time; self->backoff = backoff_time;
            self->retries = retries; self->corrupt_retries = corrupt_retries; self->cdn = cdn;
            self->server_stats = server_stats;
            if (probe_chunk && probe.kind != Probe::kNone && chunk_generation == probe.generation) {
              --probe.in_flight;
              probe.samples.insert(probe.samples.end(), attempts.begin(), attempts.end());
            } else if (checking && probe.kind == Probe::kNone) {
              for (const auto& sample : attempts) if (Scored(sample)) window.push_back(sample);
            }
            if (result != InstallError::kNone) { fail_locked(result, std::move(detail)); break; }
            end_probe_locked(now());
            self->plain = std::move(plain); self->compressed_bytes = compressed_bytes;
            self->served = attempts.empty() ? base : attempts.back().server;
            self->ready = true;
            ++waiting;
            peak_waiting = std::max(peak_waiting, static_cast<std::uint32_t>(waiting));
            changed.notify_all();
          }
        }
#if defined(__cpp_exceptions)
      } catch (...) { publish_failure(InstallError::kNetwork, "chunk worker failed"); }
#endif
    };
    workers.push_back(std::move(worker));
    if (pthread_create(&self->thread, &attr, [](void* context) -> void* {
          static_cast<Worker*>(context)->run(); return nullptr;
        }, self) != 0) {
      publish_failure(InstallError::kNetwork, "cannot start workers"); break;
    }
    self->started = true;
  }
  if (attr_ready) pthread_attr_destroy(&attr);
  const auto prepare_file = [&](std::size_t index) {
    auto& work = files[index];
    if (work.prepared) return true;
    const auto& file = *work.placement.file;
    const auto path = write_root + "/" + file.path;
    if (file.is_directory()) {
      if (!Timed(create, [&] { return MakeDirs(path, &stats.folders_created); })) return false;
    } else {
      if (!Timed(create, [&] { return MakeDirs(Parent(path), &stats.folders_created); })) return false;
      const bool existed = Exists(path);
      if (file.chunks.empty() || !existed) {
        if (!Timed(create, [&] { return EnsureFileSize(path, file.size); })) return false;
        if (!existed) ++stats.files_created;
        const auto now_held = AllocatedBytes(path, file.size);
        if (now_held > work.allocated) { progress.held_bytes += now_held - work.allocated; work.allocated = now_held; }
      }
    }
    work.prepared = true;
    return true;
  };
  // Ends a race at its deadline, judges the check window, logs what ended and names the server for
  // the Queue: the server the last written chunk came from, `primary` before one, none while the race runs.
  // A provider without a display name is not shown by its host.
  std::optional<std::size_t> named, last_served;
  bool racing = false;
  const auto show = [&](std::optional<std::size_t> index) {
    if (index == named) return;
    named = index;
    progress.server_name.clear();
    if (!index) return;
    progress.server_name = CdnServerDisplayName(servers[*index].host);
    if (Lower(progress.server_name) == Lower(servers[*index].host)) progress.server_name = "Network cache";
  };
  const auto tick = [&] {
    std::vector<ProbeReport> ended;
    std::size_t current = 0;
    {
      std::lock_guard<std::mutex> lock(mutex);
      const auto at = now();
      end_probe_locked(at);
      check_locked(at);
      ended.swap(reports);
      racing = probe.kind == Probe::kRace;
      current = primary;
    }
    for (const auto& report : ended) {
      if (hooks_.race) hooks_.race(report.step, report.result, report.detail);
      if (report.remember && hooks_.remember_fastest) hooks_.remember_fastest(*report.remember);
    }
    show(racing ? std::nullopt : last_served ? last_served : std::optional<std::size_t>(current));
  };
  std::size_t prepare_next = 0, front_file = 0, written_chunks = 0;
  while (true) {
    if (cancel.load()) publish_failure(InstallError::kCanceled, "canceled");
    Worker* ready = nullptr;
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (first_error != InstallError::kNone) break;
      for (auto& worker : workers) if (worker->ready) {
        ready = worker.get(); ready->ready = false; --waiting; break;
      }
    }
    snapshot();
    tick();
    report_progress();
    if (ready) {
      const auto& work = chunks[ready->work];
      const auto& file = *files[work.file].placement.file;
      last_served = ready->served;
      if (!racing) show(last_served);
      if (!prepare_file(work.file)) { publish_failure(InstallError::kStorage, "cannot create file"); break; }
      if (stats.chunks_done == 0) trace("first-write");
      const auto path = write_root + "/" + file.path;
      if (!Timed(write, [&] { return WriteAt(path, file.size, work.chunk->offset, ready->plain); })) {
        publish_failure(InstallError::kStorage, "cannot write file"); break;
      }
      const bool recorded = Timed(journal_time, [&] {
        return std::fprintf(journal, "%s\n", work.journal_key.c_str()) >= 0 && std::fflush(journal) == 0;
      });
      if (!recorded) { publish_failure(InstallError::kStorage, "cannot write journal"); break; }
      ++stats.chunks_done; ++written_chunks; --files[work.file].pending;
      stats.compressed_bytes += ready->compressed_bytes;
      stats.original_bytes += work.chunk->original_size;
      progress.download_done += work.chunk->compressed_size;
      // Writing into a sparse file allocates it; a preallocated file is already counted.
      auto& written = files[work.file];
      const auto grow = std::min<std::uint64_t>(work.chunk->original_size, file.size - written.allocated);
      written.allocated += grow;
      progress.held_bytes += grow;
      if (on_progress) on_progress(progress);
      // Release the occupied slot only after its output has been written and freed.
      std::vector<std::uint8_t>().swap(ready->plain);
      {
        std::lock_guard<std::mutex> lock(mutex);
        ready->busy = false; --occupied; changed.notify_all();
      }
      continue;
    }
    while (front_file < files.size() && files[front_file].prepared && files[front_file].pending == 0) ++front_file;
    while (prepare_next < files.size() && files[prepare_next].prepared) ++prepare_next;
    if (prepare_next < files.size() && prepare_next - std::min(prepare_next, front_file) < static_cast<std::size_t>(ahead_files)) {
      if (!prepare_file(prepare_next++)) { publish_failure(InstallError::kStorage, "cannot create file"); break; }
      continue;
    }
    if (written_chunks == chunks.size() && prepare_next == files.size()) break;
    std::unique_lock<std::mutex> lock(mutex);
    changed.wait_for(lock, std::chrono::milliseconds(50), [&] { return stop.load() || waiting != 0; });
  }
  join_workers();
  tick();  // a race that ended with the last chunk
  if (first_error != InstallError::kNone) {
    outcome.error = first_error; outcome.detail = first_detail; outcome.progress = progress;
    return outcome;
  }
  std::fclose(journal);
  journal = nullptr;
  stats.download_ms = Milliseconds(Clock::now() - download_start);
  loop_finished = true;

  if (target.update) {
    // Everything is staged. Before the game's files change, every folder the new version needs can be made:
    // once replacing starts the update can only finish, and a file the game wrote (or a link) where a folder
    // goes would stop it there.
    const auto plan = ReadUpdatePlan(update_dir);
    std::string blocked;
    for (const auto& key : staged_keys) {
      const auto found = placements.find(key);
      if (found == placements.end() || Parent(found->second.file->path).empty()) continue;
      if (!MakeGameDirs(target.install_root, Parent(found->second.file->path), plan, &blocked, false))
        return fail(InstallError::kStorage, ("cannot create folder for " + found->second.file->path + ": " + blocked +
                                             " is in the way").c_str());
    }
    for (const auto& entry : plan.new_list)
      if (entry.kind == 'd' && !MakeGameDirs(target.install_root, entry.path, plan, &blocked, false))
        return fail(InstallError::kStorage, ("cannot create folder " + entry.path + ": " + blocked + " is in the way").c_str());
    // From here the game's files change: the update finishes before the game runs, and doesn't start while it runs.
    if (target.may_apply && !target.may_apply()) return fail(InstallError::kGameRunning, "the game runs");
    if (target.before_apply && !target.before_apply(update_dir, outcome.skipped_depots))
      return fail(InstallError::kStorage, "cannot prepare update");
    progress.phase = InstallPhase::kApplying;
    if (on_progress) on_progress(progress);
    update_state.phase = installs::UpdatePhase::kReplacing;
    if (!WriteText(update_dir + "/" + installs::kUpdateStateName, installs::FormatUpdateState(update_state)))
      return fail(InstallError::kStorage, "cannot write update state");
    const auto finished = FinishUpdate(target.install_root, cancel, hooks_.trace);
    outcome.error = finished.error;
    outcome.detail = finished.detail;
    outcome.progress = progress;
    return outcome;
  }
  // The merged tree, then the install record; then drop the journal: the record marks a complete,
  // verified install.
  if (!WriteText(target.install_root + "/" + kFileTree, tree_text())) return fail(InstallError::kStorage, "cannot write file tree");
  if (!WriteText(target.install_root + "/" + kInstallRecord, record_text()))
    return fail(InstallError::kStorage, "cannot write install record");
  std::remove(journal_path.c_str());
  rmdir(journal_dir.c_str());
  outcome.error = InstallError::kNone;
  outcome.progress = progress;
  return outcome;
}

InstallOutcome DepotInstaller::FinishUpdate(const std::string& install_root, const std::atomic<bool>& cancel,
                                           const std::function<void(const char*)>& trace) {
  InstallOutcome outcome;
  const auto fail = [&outcome](InstallError error, std::string detail) {
    outcome.error = error;
    outcome.detail = std::move(detail);
    return outcome;
  };
  const auto step = [&trace](const char* name) { if (trace) trace(name); };
  auto state = installs::ReadUpdateState(install_root);
  if (!state || state->phase == installs::UpdatePhase::kStaging) return fail(InstallError::kProtocol, "no update to finish");
  const auto work = UpdateDir(install_root);
  const auto plan = ReadUpdatePlan(work);
  // The old file list goes once the deletion is done; after that the new tree, file list and record may
  // already be in place.
  const bool deleted = !Present(work + "/" + kUpdateOld);
  if (!deleted && plan.new_list.empty() && !Present(work + "/" + kUpdateFiles))
    return fail(InstallError::kStorage, "update file list missing");
  const auto set_phase = [&](installs::UpdatePhase phase) {
    state->phase = phase;
    return WriteText(work + "/" + installs::kUpdateStateName, installs::FormatUpdateState(*state));
  };
  if (state->phase == installs::UpdatePhase::kReplacing && !deleted) {
    step("update-replace");
    // Each staged file over the game's. One already moved (no staged copy left) is done.
    std::string staged_text;
    if (!ReadText(work + "/" + kUpdateStaged, &staged_text)) return fail(InstallError::kStorage, "staged list missing");
    for (const auto& path : Lines(staged_text)) {
      if (cancel.load()) return fail(InstallError::kCanceled, "canceled");
      if (!GamePath(path)) continue;
      const auto source = work + "/" + kUpdateStaging + "/" + path;
      if (!Present(source)) continue;
      const auto target = install_root + "/" + path;
      std::string blocked;
      if (!Parent(path).empty() && !MakeGameDirs(install_root, Parent(path), plan, &blocked))
        return fail(InstallError::kStorage, "cannot create folder for " + path + ": " + blocked + " is in the way");
      if (IsDirectory(target) && !ClearFolderForFile(install_root, path, plan))
        return fail(InstallError::kStorage, "cannot replace folder " + path);
      if (std::rename(source.c_str(), target.c_str()) != 0) return fail(InstallError::kStorage, "cannot replace " + path);
    }
    // Folders the new version lists, empty ones included.
    for (const auto& entry : plan.new_list)
      if (std::string blocked; entry.kind == 'd' && !MakeGameDirs(install_root, entry.path, plan, &blocked))
        return fail(InstallError::kStorage, "cannot create folder " + entry.path + ": " + blocked + " is in the way");
    if (!set_phase(installs::UpdatePhase::kDeleting)) return fail(InstallError::kStorage, "cannot write update state");
  }
  step("update-delete");
  // The old version's files the new one doesn't have; one the new version names in another case goes
  // too, unless it is the same file (a filesystem that ignores case).
  std::vector<std::string> folders;
  for (const auto& entry : deleted ? std::vector<ListedPath>{} : plan.old_list) {
    if (cancel.load()) return fail(InstallError::kCanceled, "canceled");
    const auto key = Lower(entry.path);
    if (entry.kind == 'd') {
      if (!plan.new_dirs.count(key)) folders.push_back(entry.path);
      continue;
    }
    const auto old_path = install_root + "/" + entry.path;
    const auto spelling = plan.new_spelling.find(key);
    if (plan.new_files.count(key)) {
      if (spelling == plan.new_spelling.end() || spelling->second == entry.path) continue;
      struct stat old_info {}, new_info {};
      const auto new_path = install_root + "/" + spelling->second;
      if (lstat(old_path.c_str(), &old_info) != 0 || lstat(new_path.c_str(), &new_info) != 0 ||
          (old_info.st_dev == new_info.st_dev && old_info.st_ino == new_info.st_ino))
        continue;
    } else if (plan.new_dirs.count(key)) {
      continue;  // made a folder while replacing
    }
    if (IsDirectory(old_path)) continue;
    // A folder above it became a link: what it leads to is not the game's.
    if (!RealParents(install_root, entry.path)) {
      step("update-skip-link");
      continue;
    }
    // Gone already, or a parent became a file (ENOTDIR): nothing to delete.
    if (std::remove(old_path.c_str()) != 0 && errno != ENOENT && errno != ENOTDIR)
      return fail(InstallError::kStorage, "cannot delete " + entry.path);
    for (auto parent = Parent(entry.path); !parent.empty(); parent = Parent(parent)) folders.push_back(parent);
  }
  // Folders that left empty, deepest first; a folder the new version has, or holding anything, stays.
  std::sort(folders.begin(), folders.end());
  folders.erase(std::unique(folders.begin(), folders.end()), folders.end());
  std::sort(folders.begin(), folders.end(),
            [](const std::string& left, const std::string& right) { return left.size() > right.size(); });
  for (const auto& folder : folders)
    if (!plan.new_dirs.count(Lower(folder)) && GamePath(folder) && RealParents(install_root, folder))
      rmdir((install_root + "/" + folder).c_str());
  if (std::remove((work + "/" + kUpdateOld).c_str()) != 0 && errno != ENOENT)
    return fail(InstallError::kStorage, "cannot finish deleting");
  step("update-record");
  // The game's launch was set up from the old files: it is made again before the game next starts.
  if (!WriteText(install_root + "/" + installs::kFilesChangedName, "v1\n"))
    return fail(InstallError::kStorage, "cannot mark the game's files changed");
  // The new version's tree, file list and Steamworks facts, then its record, then the work folder.
  const std::pair<const char*, std::string> moves[] = {
      {kUpdateTree, kFileTree}, {kUpdateFiles, kFileList}, {kSteamFacts, kSteamFacts}, {kUpdateRecord, kInstallRecord}};
  for (const auto& [from, to] : moves) {
    const auto source = work + "/" + from;
    if (!Present(source)) continue;
    if (std::rename(source.c_str(), (install_root + "/" + to).c_str()) != 0)
      return fail(InstallError::kStorage, std::string("cannot write ") + to);
  }
  // The work folder: staged files by their list (the console's readdir is unreliable), the plan, and the
  // state last. Without the state the game is updated; a folder the sweep leaves goes at uninstall.
  std::string staged_text;
  if (ReadText(work + "/" + kUpdateStaged, &staged_text)) {
    for (const auto& path : Lines(staged_text))
      if (GamePath(path)) std::remove((work + "/" + kUpdateStaging + "/" + path).c_str());
  }
  for (const char* name : {kUpdateStaged, kUpdatePaths, "journal"}) std::remove((work + "/" + name).c_str());
  if (std::remove((work + "/" + installs::kUpdateStateName).c_str()) != 0 && errno != ENOENT)
    return fail(InstallError::kStorage, "cannot remove update state");
  int error = 0;
  if (!RemoveTree(work, &error)) step("update-sweep-incomplete");
  const auto record = installs::ReadInstallRecord(install_root + "/" + kInstallRecord);
  outcome.progress.install_total = record ? record->install_bytes : 0;
  outcome.error = InstallError::kNone;
  return outcome;
}

bool DiscardManagedUpdate(const std::string& install_root, std::string* failure) {
  const auto fail = [failure](std::string reason) {
    if (failure) *failure = std::move(reason);
    return false;
  };
  if (install_root.size() < 2 || install_root[0] != '/' || install_root.find("/..") != std::string::npos)
    return fail("The game's folder path is not valid.");
  const auto state = installs::ReadUpdateState(install_root);
  if (state && state->phase != installs::UpdatePhase::kStaging)
    return fail("The update is putting its files in place and has to finish.");
  const auto work = UpdateDir(install_root);
  // The staged files by their list first: the console's readdir is unreliable.
  std::string staged_text;
  if (ReadText(work + "/" + kUpdateStaged, &staged_text)) {
    std::vector<std::string> folders;
    for (const auto& path : Lines(staged_text)) {
      if (!GamePath(path)) continue;
      std::remove((work + "/" + kUpdateStaging + "/" + path).c_str());
      for (auto parent = Parent(path); !parent.empty(); parent = Parent(parent)) folders.push_back(parent);
    }
    std::sort(folders.begin(), folders.end(),
              [](const std::string& left, const std::string& right) { return left.size() > right.size(); });
    for (const auto& folder : folders) rmdir((work + "/" + kUpdateStaging + "/" + folder).c_str());
  }
  int error = 0;
  if (!RemoveTree(work, &error)) return fail("Some of the update's files could not be deleted (" + std::string(std::strerror(error)) + ").");
  return true;
}

bool RemoveManagedInstall(const std::string& install_root, std::string* failure) {
  const auto fail = [failure](std::string reason) {
    if (failure) *failure = std::move(reason);
    return false;
  };
  if (install_root.size() < 2 || install_root[0] != '/' || install_root.find("/..") != std::string::npos)
    return fail("The game's folder path is not valid.");
  if (!Exists(install_root)) return true;
  // The copy of an imported game keeps its plan in .playden-copy (stores/local/copy_plan.h).
  const std::string copy_dir = install_root + "/" + kCopyJournalDir;
  if (!Exists(install_root + "/" + DepotInstaller::kInstallRecord) &&
      !Exists(install_root + "/" + DepotInstaller::kJournalDir) && !Exists(copy_dir))
    return fail("The game's folder holds no Playden install record, so it was left as it is.");
  // Remove exactly the paths the install recorded, deepest first, then Playden's own files.
  // An update in progress adds what either version has (its "paths"), and its work folder goes first.
  std::vector<std::string> files;
  std::vector<std::string> directories;
  for (const auto& list_path : {install_root + "/" + DepotInstaller::kFileList,
                                install_root + "/" + DepotInstaller::kUpdateDir + "/" + kUpdatePaths}) {
    FILE* list = std::fopen(list_path.c_str(), "r");
    if (!list) continue;
    char line[8192];
    while (std::fgets(line, sizeof(line), list)) {
      std::string entry(line);
      while (!entry.empty() && (entry.back() == '\n' || entry.back() == '\r')) entry.pop_back();
      if (entry.size() < 3 || entry[1] != '\t') continue;
      const auto path = entry.substr(2);
      if (path.empty() || path[0] == '/' || path.find("..") != std::string::npos) continue;
      (entry[0] == 'd' ? directories : files).push_back(install_root + "/" + path);
    }
    std::fclose(list);
  }
  if (Exists(UpdateDir(install_root))) DiscardManagedUpdate(install_root);
  RemoveTree(UpdateDir(install_root));
  for (const auto& file : files) std::remove(file.c_str());
  for (const char* name : {DepotInstaller::kFileTree, ".playden-tree.tmp"}) std::remove((install_root + "/" + name).c_str());
  std::sort(directories.begin(), directories.end(),
            [](const std::string& left, const std::string& right) { return left.size() > right.size(); });
  for (const auto& directory : directories) rmdir(directory.c_str());
  // Parent folders of recorded files that the manifest did not list explicitly.
  std::vector<std::string> parents;
  for (const auto& file : files)
    for (auto parent = Parent(file); parent.size() > install_root.size(); parent = Parent(parent)) parents.push_back(parent);
  std::sort(parents.begin(), parents.end(),
            [](const std::string& left, const std::string& right) { return left.size() > right.size(); });
  for (const auto& parent : parents) rmdir(parent.c_str());
  // Files the list did not cover (Playden's journals, files the game or other tools wrote) go in a walk.
  // The install record goes last: a folder that could not be emptied stays Playden's, so
  // uninstalling it again tries again.
  int error = 0;
  bool emptied = true;
  std::vector<std::string> names;
  if (ListEntries(install_root, names)) {
    for (const auto& name : names)
      if (name != DepotInstaller::kInstallRecord) emptied = RemoveTree(install_root + "/" + name, &error) && emptied;
  } else {
    emptied = false;
    error = errno;
  }
  if (emptied) {
    if (std::remove((install_root + "/" + DepotInstaller::kInstallRecord).c_str()) != 0 && errno != ENOENT) {
      emptied = false;
      error = errno;
    } else if (rmdir(install_root.c_str()) != 0) {
      emptied = false;
      error = errno;
    }
  }
  if (emptied) return true;
  return fail("Some of the game's files could not be deleted (" + std::string(std::strerror(error)) + ").");
}

}  // namespace playden::steam
