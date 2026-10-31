#include "steam/backend/cloud_sync.h"

#include "steam/backend/app_info.h"
#include "steam/backend/depot_manifest.h"
#include "steam/backend/gzip.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#if !PLAYDEN_PLATFORM_PS5
#include <sys/time.h>
#endif

#include <mbedtls/sha1.h>
#include <mbedtls/sha256.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>

namespace playden::steam::cloud {
namespace {

constexpr std::uint32_t kSessionDeadlineMs = 300'000;
constexpr const char* kBaselineFile = "cloud-baseline.txt";
constexpr const char* kIndexFile = "cloud-index.txt";
constexpr const char* kClientIdFile = "cloud-client-id.txt";
constexpr const char* kBackupFolder = "backup";
constexpr std::size_t kKeptBackups = 5;

std::string Hex(const std::uint8_t* data, std::size_t size) {
  static const char digits[] = "0123456789abcdef";
  std::string text;
  text.reserve(size * 2);
  for (std::size_t index = 0; index < size; ++index) {
    text.push_back(digits[data[index] >> 4]);
    text.push_back(digits[data[index] & 15]);
  }
  return text;
}

std::optional<std::vector<std::uint8_t>> Unhex(const std::string& text) {
  if (text.size() % 2) return std::nullopt;
  std::vector<std::uint8_t> bytes;
  for (std::size_t index = 0; index < text.size(); index += 2) {
    unsigned value = 0;
    if (std::sscanf(text.c_str() + index, "%2x", &value) != 1) return std::nullopt;
    bytes.push_back(static_cast<std::uint8_t>(value));
  }
  return bytes;
}

std::vector<std::uint8_t> Sha1(const std::vector<std::uint8_t>& data) {
  std::vector<std::uint8_t> digest(20);
  mbedtls_sha1(data.data(), data.size(), digest.data());
  return digest;
}

bool IsDirectory(const std::string& path) {
  struct stat info {};
  return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

bool MakeDirectories(const std::string& path) {
  for (std::size_t slash = path.find('/', 1); slash != std::string::npos; slash = path.find('/', slash + 1)) {
    const auto prefix = path.substr(0, slash);
    if (!IsDirectory(prefix) && mkdir(prefix.c_str(), 0777) != 0 && !IsDirectory(prefix)) return false;
  }
  return IsDirectory(path) || mkdir(path.c_str(), 0777) == 0 || IsDirectory(path);
}

std::string Parent(const std::string& path) {
  const auto slash = path.find_last_of('/');
  return slash == std::string::npos || slash == 0 ? std::string() : path.substr(0, slash);
}

// Worker threads on the console have small stacks: file buffers live on the heap.
constexpr std::size_t kReadChunk = 64 * 1024;

std::optional<std::vector<std::uint8_t>> ReadFile(const std::string& path, std::uint64_t limit) {
  struct stat info {};
  if (stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode) || static_cast<std::uint64_t>(info.st_size) > limit)
    return std::nullopt;
  FILE* file = std::fopen(path.c_str(), "rb");
  if (!file) return std::nullopt;
  std::vector<std::uint8_t> data;
  data.reserve(static_cast<std::size_t>(info.st_size));
  std::vector<std::uint8_t> buffer(kReadChunk);
  std::size_t read = 0;
  while ((read = std::fread(buffer.data(), 1, buffer.size(), file)) > 0) {
    if (data.size() + read > limit) {
      std::fclose(file);
      return std::nullopt;
    }
    data.insert(data.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(read));
  }
  const bool failed = std::ferror(file) != 0;
  std::fclose(file);
  if (failed) return std::nullopt;
  return data;
}

bool WriteFileAtomic(const std::string& path, const std::vector<std::uint8_t>& data) {
  if (!MakeDirectories(Parent(path))) return false;
  const auto temporary = path + ".playden-partial";
  FILE* file = std::fopen(temporary.c_str(), "wb");
  if (!file) return false;
  const bool written = data.empty() || std::fwrite(data.data(), 1, data.size(), file) == data.size();
  if (std::fclose(file) != 0 || !written) {
    std::remove(temporary.c_str());
    return false;
  }
  if (std::rename(temporary.c_str(), path.c_str()) != 0) {
    std::remove(temporary.c_str());
    return false;
  }
  return true;
}

void SetModified(const std::string& path, std::uint64_t unix_seconds) {
#if PLAYDEN_PLATFORM_PS5
  (void)path;
  (void)unix_seconds;
#else
  if (unix_seconds == 0) return;
  struct timeval times[2] = {{static_cast<time_t>(unix_seconds), 0}, {static_cast<time_t>(unix_seconds), 0}};
  utimes(path.c_str(), times);
#endif
}

std::string RootFolder(const GameSaveLocation& location, SaveRoot root) {
  return root == SaveRoot::kGame ? location.game_root : location.prefix_root;
}

std::string Absolute(const GameSaveLocation& location, const LocalSavePath& local) {
  return RootFolder(location, local.root) + "/" + local.path;
}

// Size and SHA-1 of a file, hashed in chunks so large saves are never held in memory whole.
std::optional<FileFact> Fingerprint(const std::string& path) {
  struct stat info {};
  if (stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) return std::nullopt;
  if (static_cast<std::uint64_t>(info.st_size) > kMaxRemoteFileBytes) return std::nullopt;
  FILE* file = std::fopen(path.c_str(), "rb");
  if (!file) return std::nullopt;
  mbedtls_sha1_context context;
  mbedtls_sha1_init(&context);
  mbedtls_sha1_starts(&context);
  std::vector<std::uint8_t> buffer(kReadChunk);
  std::uint64_t size = 0;
  std::size_t read = 0;
  while ((read = std::fread(buffer.data(), 1, buffer.size(), file)) > 0) {
    mbedtls_sha1_update(&context, buffer.data(), read);
    size += read;
  }
  const bool failed = std::ferror(file) != 0 || size > kMaxRemoteFileBytes;
  std::fclose(file);
  FileFact fact;
  fact.sha1.resize(20);
  mbedtls_sha1_finish(&context, fact.sha1.data());
  mbedtls_sha1_free(&context);
  if (failed) return std::nullopt;
  fact.present = true;
  fact.size = size;
  fact.time = static_cast<std::uint64_t>(info.st_mtime);
  return fact;
}

// Files below a rule's folder (relative to the save root). Playden's own staging names are skipped.
void Enumerate(const std::string& root, const std::string& relative, bool recursive, int depth,
               std::vector<std::string>* out) {
  if (depth > 16 || out->size() >= kMaxLocalSaveFiles) return;
  const auto folder = relative.empty() ? root : root + "/" + relative;
  DIR* directory = opendir(folder.c_str());
  if (!directory) return;
  while (dirent* entry = readdir(directory)) {
    const std::string name = entry->d_name;
    if (name == "." || name == ".." || name.rfind(".playden", 0) == 0 ||
        name.find(".playden-partial") != std::string::npos)
      continue;
    const auto child = relative.empty() ? name : relative + "/" + name;
    struct stat info {};
    if (stat((root + "/" + child).c_str(), &info) != 0) continue;
    if (S_ISDIR(info.st_mode)) {
      if (recursive) Enumerate(root, child, recursive, depth + 1, out);
    } else if (S_ISREG(info.st_mode)) {
      out->push_back(child);
      if (out->size() >= kMaxLocalSaveFiles) break;
    }
  }
  closedir(directory);
}

struct Baseline {
  bool loaded = false;
  std::string account_key;
  std::uint64_t change_number = 0;
  std::map<std::string, FileFact> files;  // by lowercased remote name
  bool uploaded = false;  // this console has changed this game's Cloud saves before
};

std::string Lower(std::string text) {
  for (auto& value : text) value = static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
  return text;
}

Baseline LoadBaseline(const std::string& state_root) {
  Baseline baseline;
  FILE* file = std::fopen((state_root + "/" + kBaselineFile).c_str(), "r");
  if (!file) return baseline;
  std::vector<char> buffer(8192);
  char* line = buffer.data();
  bool header = std::fgets(line, static_cast<int>(buffer.size()), file) && std::strncmp(line, "version=1", 9) == 0;
  while (header && std::fgets(line, static_cast<int>(buffer.size()), file)) {
    std::string text(line, std::strcspn(line, "\r\n"));
    if (text.rfind("account=", 0) == 0) baseline.account_key = text.substr(8);
    else if (text.rfind("change=", 0) == 0) baseline.change_number = std::strtoull(text.c_str() + 7, nullptr, 10);
    else if (text == "uploaded=1") baseline.uploaded = true;
    else {
      const auto first = text.find('\t');
      const auto second = first == std::string::npos ? std::string::npos : text.find('\t', first + 1);
      if (second == std::string::npos) continue;
      FileFact fact;
      fact.present = true;
      const auto sha = Unhex(text.substr(0, first));
      if (!sha || sha->size() != 20) continue;
      fact.sha1 = *sha;
      fact.size = std::strtoull(text.c_str() + first + 1, nullptr, 10);
      baseline.files[Lower(text.substr(second + 1))] = std::move(fact);
    }
  }
  std::fclose(file);
  baseline.loaded = header && !baseline.account_key.empty();
  return baseline;
}

bool SaveBaseline(const std::string& state_root, const std::string& account_key, std::uint64_t change_number,
                  const std::vector<std::pair<std::string, FileFact>>& files, bool uploaded) {
  std::string text = "version=1\naccount=" + account_key + "\nchange=" + std::to_string(change_number) + "\n" +
                     (uploaded ? "uploaded=1\n" : "");
  for (const auto& [name, fact] : files)
    text += Hex(fact.sha1.data(), fact.sha1.size()) + "\t" + std::to_string(fact.size) + "\t" + name + "\n";
  return WriteFileAtomic(state_root + "/" + kBaselineFile, std::vector<std::uint8_t>(text.begin(), text.end()));
}

std::uint64_t ClientId(const std::string& state_root) {
  const auto path = state_root + "/" + kClientIdFile;
  if (const auto data = ReadFile(path, 64)) {
    const std::string text(data->begin(), data->end());
    if (const auto value = std::strtoull(text.c_str(), nullptr, 10); value != 0) return value;
  }
  // Not std::random_device: on the console it goes through libc arc4random, and with exceptions off any
  // failure aborts Playden. The first Cloud upload, the first caller, crashed here or just after (#35).
  // Any non-zero value works; it only tells Steam which client this is.
  std::uint64_t value = 0;
  if (const int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC); fd >= 0) {
    if (read(fd, &value, sizeof(value)) != static_cast<ssize_t>(sizeof(value))) value = 0;
    close(fd);
  }
  if (value == 0)
    value = static_cast<std::uint64_t>(std::chrono::system_clock::now().time_since_epoch().count()) ^
            (static_cast<std::uint64_t>(getpid()) << 48);
  if (value == 0) value = 1;
  const auto text = std::to_string(value) + "\n";
  WriteFileAtomic(path, std::vector<std::uint8_t>(text.begin(), text.end()));
  return value;
}

bool SameContent(const FileFact& left, const FileFact& right) {
  return left.present && right.present && left.size == right.size && left.sha1 == right.sha1;
}

// Signed transfer URLs from Steam: a plain host name (optionally with a port) and an absolute path.
std::optional<std::string> TransferUrl(bool https, const std::string& host, const std::string& path) {
  if (host.empty() || host.size() > 253 || path.empty() || path.front() != '/') return std::nullopt;
  for (const char value : host)
    if (!(std::isalnum(static_cast<unsigned char>(value)) || value == '.' || value == '-' || value == ':')) return std::nullopt;
  for (const char value : path)
    if (static_cast<unsigned char>(value) <= 0x20 || value == '#') return std::nullopt;
  return std::string(https ? "https://" : "http://") + host + path;
}

void PruneBackups(const std::string& state_root) {
  std::vector<std::string> names;
  const auto folder = state_root + "/" + kBackupFolder;
  if (DIR* directory = opendir(folder.c_str())) {
    while (dirent* entry = readdir(directory)) {
      const std::string name = entry->d_name;
      if (name != "." && name != "..") names.push_back(name);
    }
    closedir(directory);
  }
  std::sort(names.begin(), names.end());
  while (names.size() > kKeptBackups) {
    // Backups are only ever Playden's copies; remove the oldest folder tree.
    std::vector<std::string> files;
    Enumerate(folder + "/" + names.front(), "", true, 0, &files);
    for (const auto& file : files) std::remove((folder + "/" + names.front() + "/" + file).c_str());
    std::vector<std::string> stack = {folder + "/" + names.front()};
    // Remove now-empty folders deepest first.
    std::vector<std::string> directories;
    while (!stack.empty()) {
      const auto current = stack.back();
      stack.pop_back();
      directories.push_back(current);
      if (DIR* directory = opendir(current.c_str())) {
        while (dirent* entry = readdir(directory)) {
          const std::string name = entry->d_name;
          if (name != "." && name != ".." && IsDirectory(current + "/" + name)) stack.push_back(current + "/" + name);
        }
        closedir(directory);
      }
    }
    for (auto it = directories.rbegin(); it != directories.rend(); ++it) rmdir(it->c_str());
    names.erase(names.begin());
  }
}

std::string Plural(std::size_t count, const char* one, const char* many) {
  return std::to_string(count) + " " + (count == 1 ? one : many);
}

// ---- The index: what the last full check saw, so the quick check needs neither Steam's app info
// nor hashing. One file per game next to the baseline:
//   version=1, account=<key>, change=<n>
//   rule<TAB><g|p><TAB><recursive 0|1><TAB><directory><TAB><pattern><TAB><cloud prefix>
//   file<TAB><g|p><TAB><size><TAB><mtime><TAB><path>

struct LocalStamp {
  std::uint64_t size = 0;
  std::int64_t time = 0;
  bool operator==(const LocalStamp& other) const { return size == other.size && time == other.time; }
};
using LocalFiles = std::map<std::string, std::pair<LocalSavePath, LocalStamp>>;  // by LocalSavePath::key()

struct SaveIndex {
  bool loaded = false;
  std::string account_key;
  std::uint64_t change_number = 0;
  // The folders the files were found in: a reinstall (game folder back, new bottle) changes them.
  std::string game_root, prefix_root;
  std::vector<SaveRule> rules;
  LocalFiles files;
};

bool SameFiles(const LocalFiles& left, const LocalFiles& right) {
  if (left.size() != right.size()) return false;
  for (const auto& [key, entry] : left) {
    const auto other = right.find(key);
    if (other == right.end() || !(other->second.second == entry.second)) return false;
  }
  return true;
}

// The save files a mapping covers on this device now: everything under each rule's folder plus the
// known paths (the console may not list some folders), with size and time. Roots that are not
// there (an uninstalled game's folder) are skipped.
LocalFiles ScanLocal(const GameSaveLocation& location, const SavePaths& paths, const std::vector<LocalSavePath>& known) {
  std::vector<LocalSavePath> candidates;
  for (const auto& rule : paths.rules()) {
    const auto root = RootFolder(location, rule.root);
    if (root.empty()) continue;
    std::vector<std::string> found;
    Enumerate(root, rule.directory, rule.recursive, 0, &found);
    for (const auto& path : found) candidates.push_back({rule.root, path});
  }
  candidates.insert(candidates.end(), known.begin(), known.end());
  LocalFiles files;
  for (const auto& candidate : candidates) {
    if (files.size() >= kMaxLocalSaveFiles) break;
    if (RootFolder(location, candidate.root).empty() || files.count(candidate.key()) || !paths.RemoteFor(candidate)) continue;
    struct stat info {};
    if (stat(Absolute(location, candidate).c_str(), &info) != 0 || !S_ISREG(info.st_mode)) continue;
    files[candidate.key()] = {candidate, {static_cast<std::uint64_t>(info.st_size), static_cast<std::int64_t>(info.st_mtime)}};
  }
  return files;
}

std::vector<std::string> SplitTabs(const std::string& text, std::size_t fields) {
  std::vector<std::string> out;
  std::size_t start = 0;
  while (out.size() + 1 < fields) {
    const auto tab = text.find('\t', start);
    if (tab == std::string::npos) return {};
    out.push_back(text.substr(start, tab - start));
    start = tab + 1;
  }
  out.push_back(text.substr(start));  // the last field keeps any tabs
  return out;
}

bool WriteIndex(const GameSaveLocation& location, const std::string& account_key, std::uint64_t change_number,
                const std::vector<SaveRule>& rules, const LocalFiles& files) {
  const auto root = [](SaveRoot value) { return value == SaveRoot::kGame ? "g" : "p"; };
  std::string text = "version=1\naccount=" + account_key + "\nchange=" + std::to_string(change_number) + "\ngame_root=" +
                     location.game_root + "\nprefix_root=" + location.prefix_root + "\n";
  for (const auto& rule : rules)
    text += std::string("rule\t") + root(rule.root) + "\t" + (rule.recursive ? "1" : "0") + "\t" + rule.directory + "\t" +
            rule.pattern + "\t" + rule.cloud_prefix + "\n";
  for (const auto& [key, entry] : files)
    text += std::string("file\t") + root(entry.first.root) + "\t" + std::to_string(entry.second.size) + "\t" +
            std::to_string(entry.second.time) + "\t" + entry.first.path + "\n";
  return WriteFileAtomic(location.state_root + "/" + kIndexFile, std::vector<std::uint8_t>(text.begin(), text.end()));
}

SaveIndex LoadIndex(const std::string& state_root) {
  SaveIndex index;
  const auto data = ReadFile(state_root + "/" + kIndexFile, 16 * 1024 * 1024);
  if (!data) return index;
  const std::string text(data->begin(), data->end());
  std::size_t start = 0;
  bool header = false;
  while (start < text.size()) {
    auto end = text.find('\n', start);
    if (end == std::string::npos) end = text.size();
    const auto line = text.substr(start, end - start);
    start = end + 1;
    if (!header) {
      if (line != "version=1") return index;
      header = true;
    } else if (line.rfind("account=", 0) == 0) {
      index.account_key = line.substr(8);
    } else if (line.rfind("change=", 0) == 0) {
      index.change_number = std::strtoull(line.c_str() + 7, nullptr, 10);
    } else if (line.rfind("game_root=", 0) == 0) {
      index.game_root = line.substr(10);
    } else if (line.rfind("prefix_root=", 0) == 0) {
      index.prefix_root = line.substr(12);
    } else if (line.rfind("rule\t", 0) == 0) {
      const auto fields = SplitTabs(line.substr(5), 5);
      if (fields.size() != 5 || (fields[0] != "g" && fields[0] != "p")) return index;
      index.rules.push_back({fields[0] == "g" ? SaveRoot::kGame : SaveRoot::kPrefix, fields[2], fields[3], fields[1] == "1", fields[4]});
    } else if (line.rfind("file\t", 0) == 0) {
      const auto fields = SplitTabs(line.substr(5), 4);
      if (fields.size() != 4 || (fields[0] != "g" && fields[0] != "p") || fields[3].empty()) return index;
      const LocalSavePath path{fields[0] == "g" ? SaveRoot::kGame : SaveRoot::kPrefix, fields[3]};
      index.files[path.key()] = {path, {std::strtoull(fields[1].c_str(), nullptr, 10), std::strtoll(fields[2].c_str(), nullptr, 10)}};
    }
  }
  index.loaded = header && !index.account_key.empty() && !index.rules.empty();
  return index;
}

std::vector<LocalSavePath> Paths(const LocalFiles& files) {
  std::vector<LocalSavePath> out;
  for (const auto& [key, entry] : files) out.push_back(entry.first);
  return out;
}

}  // namespace

std::string AccountKey(std::uint64_t steam_id) {
  const std::string input = "steam:" + std::to_string(steam_id);
  std::uint8_t digest[32] = {};
  mbedtls_sha256(reinterpret_cast<const unsigned char*>(input.data()), input.size(), digest, 0);
  return Hex(digest, sizeof(digest));
}

// A CM logon refused with AccessDenied. On 10 Oct Steam answered this both to a token it no longer
// accepted and, it seems, to logons it was limiting; Playden waits for its next run before trying the
// token again (cm_client.cpp).
constexpr const char* kSignInRefused =
    "Steam turned down the sign-in. Playden tries again next time it starts; if this keeps happening, use Settings, Account, Sign in again.";

CloudSync::CloudSync(CmClient& cm, HttpClient& http) {
  hooks_.session = [&cm](const StoredSession& session, std::uint32_t deadline_ms, const std::atomic<bool>& cancel,
                         const std::function<CmError(const CmClient::ServiceCall&, std::uint64_t)>& work) {
    return cm.WithServiceSession(session, deadline_ms, cancel, work);
  };
  hooks_.app_info = [&cm](const StoredSession& session, std::uint32_t app_id, const std::atomic<bool>& cancel,
                          CmError* error) -> std::optional<CloudSaveInfo> {
    const auto buffers = cm.FetchAppInfo(session, {app_id}, cancel);
    *error = buffers.error;
    if (buffers.error != CmError::kNone) return std::nullopt;
    const auto found = buffers.buffers.find(app_id);
    if (found == buffers.buffers.end()) return std::nullopt;
    if (const auto info = ParseAppInfo(app_id, found->second)) return info->cloud;
    return std::nullopt;
  };
  hooks_.http = [&http](const HttpRequest& request, const std::atomic<bool>& cancel) { return http.Perform(request, cancel); };
}

CloudSync::CloudSync(CloudHooks hooks) : hooks_(std::move(hooks)) {}

#define PLAYDEN_CLOUD_TRACE(step) \
  do { if (hooks_.trace) hooks_.trace(step); } while (false)

CloudPlan CloudSync::Check(const StoredSession& session, const GameSaveLocation& location, const std::atomic<bool>& cancel) {
  CloudPlan plan;
  const auto fail = [&](PlanStatus status, std::string message) {
    plan.status = status;
    plan.message = std::move(message);
    plan.decisions.clear();
    return plan;
  };
  PLAYDEN_CLOUD_TRACE("app-info");
  CmError info_error = CmError::kNone;
  const auto info = hooks_.app_info(session, location.app_id, cancel, &info_error);
  if (info_error == CmError::kRejected) return fail(PlanStatus::kFailed, kSignInRefused);
  if (info_error != CmError::kNone) return fail(PlanStatus::kFailed, "Steam is not reachable. Try again.");
  if (!info || !info->declared) return fail(PlanStatus::kUnsupported, "This game does not use Steam Cloud.");

  PLAYDEN_CLOUD_TRACE("list");
  std::optional<RemoteFileList> remote;
  std::uint64_t steam_id = 0;
  const auto listed = hooks_.session(session, kSessionDeadlineMs, cancel,
                                     [&](const CmClient::ServiceCall& call, std::uint64_t logged_on) {
    steam_id = logged_on;
    std::vector<std::uint8_t> reply;
    if (const auto error = call(kGetAppFileChangelist, EncodeChangelistRequest(location.app_id, 0), &reply, nullptr);
        error != CmError::kNone)
      return error;
    remote = DecodeChangelistResponse(reply);
    return remote ? CmError::kNone : CmError::kProtocol;
  });
  if (listed == CmError::kRejected) return fail(PlanStatus::kFailed, "Steam refused the Cloud save list.");
  if (listed != CmError::kNone || !remote) return fail(PlanStatus::kFailed, "Steam did not return the Cloud save list. Try again.");

  PLAYDEN_CLOUD_TRACE("mapping");
  const auto mapping = BuildSaveMapping(*info, location.app_id, steam_id);
  if (!mapping.usable()) return fail(PlanStatus::kUnsupported, mapping.unresolved.empty() ? "This game has no Cloud save locations."
                                                                                     : mapping.unresolved.front());
  const SavePaths paths(mapping);
  plan.change_number = remote->change_number;
  plan.account_key = AccountKey(steam_id);
  auto baseline = LoadBaseline(location.state_root);
  const bool trusted = baseline.loaded && baseline.account_key == plan.account_key;
  if (!trusted) baseline.files.clear();
  plan.requires_account_confirmation = !trusted;

  // Every slot either side knows about, keyed by local identity.
  std::map<std::string, Decision> slots;
  std::set<std::string> unmapped_remote;
  for (const auto& file : remote->files) {
    std::string error;
    const auto local = paths.LocalFor(file.name, &error);
    if (!local) {
      if (file.state == RemoteState::kPresent) unmapped_remote.insert(file.name);
      continue;
    }
    auto& slot = slots[local->key()];
    slot.remote_name = file.name;
    slot.local = *local;
    if (RootFolder(location, local->root).empty()) {
      // Its folder is not on this device (an uninstalled game's own folder): never written or deleted.
      slot.action = Action::kUnavailable;
      continue;
    }
    if (file.state == RemoteState::kPresent) {
      slot.remote_fact = {true, file.sha1, file.raw_size, file.time_stamp};
      if (file.reupload_requested) slot.action = Action::kUpload;  // provisional; settled below
    } else if (file.state == RemoteState::kForgotten) {
      slot.action = Action::kUnavailable;
    }
  }
  PLAYDEN_CLOUD_TRACE("scan");
  // Local files under every rule's folder, plus the known names (the console may not list folders).
  std::vector<LocalSavePath> candidates;
  for (const auto& rule : paths.rules()) {
    std::vector<std::string> found;
    const auto root = RootFolder(location, rule.root);
    if (root.empty()) continue;
    Enumerate(root, rule.directory, rule.recursive, 0, &found);
    for (const auto& path : found) candidates.push_back({rule.root, path});
  }
  for (const auto& [key, slot] : slots) candidates.push_back(slot.local);
  PLAYDEN_CLOUD_TRACE("fingerprint");
  std::set<std::string> seen;
  std::uint64_t scanned_bytes = 0;
  for (const auto& candidate : candidates) {
    if (!seen.insert(candidate.key()).second || RootFolder(location, candidate.root).empty()) continue;
    const auto name = paths.RemoteFor(candidate);
    if (!name) continue;
    const auto fact = Fingerprint(Absolute(location, candidate));
    if (!fact) continue;
    scanned_bytes += fact->size;
    if (seen.size() > kMaxLocalSaveFiles || scanned_bytes > kMaxLocalSaveBytes)
      return fail(PlanStatus::kFailed, "This game's local saves are too large to sync.");
    auto& slot = slots[candidate.key()];
    if (slot.remote_name.empty()) slot.remote_name = *name;
    slot.local = candidate;
    slot.local_fact = *fact;
  }

  PLAYDEN_CLOUD_TRACE("plan");
  for (auto& [key, slot] : slots) {
    const bool reupload = slot.action == Action::kUpload;
    if (slot.action == Action::kUnavailable) {
      plan.decisions.push_back(slot);
      continue;
    }
    const auto& local = slot.local_fact;
    const auto& cloud = slot.remote_fact;
    const auto base_it = baseline.files.find(Lower(slot.remote_name));
    const FileFact* base = base_it == baseline.files.end() ? nullptr : &base_it->second;
    if (!local.present && !cloud.present) continue;
    if (SameContent(local, cloud)) slot.action = reupload ? Action::kUpload : Action::kUnchanged;
    else if (cloud.present && !local.present)
      // Missing here but unchanged in the Cloud since the last sync: deleted on purpose? Ask.
      slot.action = base && SameContent(*base, cloud) ? Action::kConflict : Action::kDownload;
    else if (local.present && !cloud.present)
      slot.action = base && SameContent(*base, local) ? Action::kConflict : Action::kUpload;
    else if (base && SameContent(*base, local)) slot.action = Action::kDownload;
    else if (base && SameContent(*base, cloud)) slot.action = Action::kUpload;
    else slot.action = Action::kConflict;
    plan.decisions.push_back(slot);
  }
  // The first time this console would replace Cloud saves, it asks: right after Cloud saves were
  // downloaded, a game that did not find them writes fresh ones, which would otherwise go up over
  // the account's progress (#69). Every upload of that sync becomes part of the one choice.
  if (!baseline.uploaded) {
    bool replaces = false;
    for (const auto& decision : plan.decisions)
      if (decision.action == Action::kUpload && decision.remote_fact.present && !SameContent(decision.local_fact, decision.remote_fact))
        replaces = true;
    if (replaces) {
      plan.first_overwrite = true;
      for (auto& decision : plan.decisions)
        if (decision.action == Action::kUpload) decision.action = Action::kConflict;
    }
  }
  for (const auto& decision : plan.decisions) {
    if (decision.action == Action::kUnavailable) continue;
    switch (decision.action) {
      case Action::kDownload: ++plan.downloads; break;
      case Action::kUpload: ++plan.uploads; break;
      case Action::kConflict: ++plan.conflicts; break;
      default: break;
    }
    if (decision.local_fact.present) {
      ++plan.local_files;
      plan.local_bytes += decision.local_fact.size;
      plan.local_latest = std::max(plan.local_latest, decision.local_fact.time);
    }
    if (decision.remote_fact.present) {
      ++plan.remote_files;
      plan.remote_bytes += decision.remote_fact.size;
      plan.remote_latest = std::max(plan.remote_latest, decision.remote_fact.time);
    }
  }
  if (plan.conflicts > 0) {
    plan.status = PlanStatus::kConflict;
    plan.message = plan.first_overwrite
                       ? "Saves here would replace the ones in Steam Cloud, and this console hasn't uploaded any for this game before. Choose which to keep."
                       : "Saves here and in Steam Cloud both changed. Choose which to keep.";
  } else if (plan.uploads > 0 && plan.requires_account_confirmation) {
    plan.status = PlanStatus::kNeedsAccount;
    plan.message = "Confirm this Steam account before Playden changes its Cloud saves.";
  } else if (plan.downloads > 0 || plan.uploads > 0) {
    plan.status = PlanStatus::kReady;
    plan.message = plan.downloads > 0 && plan.uploads > 0
                       ? Plural(plan.downloads, "save to download", "saves to download") + ", " +
                             Plural(plan.uploads, "to upload", "to upload")
                   : plan.downloads > 0 ? Plural(plan.downloads, "save to download", "saves to download")
                                        : Plural(plan.uploads, "save to upload", "saves to upload");
  } else {
    plan.status = PlanStatus::kUpToDate;
    plan.message = plan.remote_files == 0 && plan.local_files == 0 ? "No Cloud saves yet" : "Saves are up to date";
  }
  if (!unmapped_remote.empty() && plan.status != PlanStatus::kConflict)
    plan.message += " · " + Plural(unmapped_remote.size(), "Cloud file is", "Cloud files are") + " outside known save folders";

  PLAYDEN_CLOUD_TRACE("index");
  std::vector<LocalSavePath> known;
  std::vector<std::pair<std::string, FileFact>> agreed;
  for (const auto& decision : plan.decisions) {
    if (decision.local_fact.present) known.push_back(decision.local);
    if (decision.action != Action::kUnavailable && SameContent(decision.local_fact, decision.remote_fact))
      agreed.emplace_back(decision.remote_name, decision.remote_fact);
  }
  WriteIndex(location, plan.account_key, plan.change_number, paths.rules(), ScanLocal(location, paths, known));
  // Already in step: that is a baseline too, so the first later change on one side syncs without
  // asking. An empty agreement only refreshes a baseline this account already has.
  if (plan.status == PlanStatus::kUpToDate && (trusted || !agreed.empty()))
    SaveBaseline(location.state_root, plan.account_key, plan.change_number, agreed, trusted && baseline.uploaded);
  return plan;
}

CloudOutcome CloudSync::Apply(const StoredSession& session, const GameSaveLocation& location, const CloudPlan& plan,
                              Resolution resolution, bool confirm_account, const std::atomic<bool>& cancel,
                              const ApplyProgress& progress) {
  CloudOutcome outcome;
  const auto fail = [&](std::string message, std::string detail = {}) {
    outcome.ok = false;
    outcome.message = std::move(message);
    outcome.detail = std::move(detail);
    return outcome;
  };
  if (plan.status == PlanStatus::kFailed || plan.status == PlanStatus::kUnsupported) return fail(plan.message);

  // Settle conflicts with the player's choice.
  std::vector<Decision> downloads, uploads, local_deletes, remote_deletes;
  for (const auto& decision : plan.decisions) {
    auto action = decision.action;
    if (action == Action::kConflict) {
      if (resolution == Resolution::kAutomatic) return fail("Choose which saves to keep first.");
      if (resolution == Resolution::kUseLocal)
        action = decision.local_fact.present ? Action::kUpload : Action::kDeleteRemote;
      else
        action = decision.remote_fact.present ? Action::kDownload : Action::kDeleteLocal;
    }
    switch (action) {
      case Action::kDownload: downloads.push_back(decision); break;
      case Action::kUpload: uploads.push_back(decision); break;
      case Action::kDeleteLocal: local_deletes.push_back(decision); break;
      case Action::kDeleteRemote: remote_deletes.push_back(decision); break;
      default: break;
    }
  }
  std::uint64_t total = 0, done = 0;
  for (const auto& decision : downloads) total += decision.remote_fact.size;
  for (const auto& decision : uploads) total += decision.local_fact.size;
  const auto count_bytes = [&](std::uint64_t bytes) {
    done += bytes;
    if (progress) progress(done, total);
  };
  if (progress) progress(0, total);
  const bool remote_changes = !uploads.empty() || !remote_deletes.empty();
  if (remote_changes && plan.requires_account_confirmation && !confirm_account)
    return fail("Confirm this Steam account before Playden changes its Cloud saves.");

  const auto backup_root = location.state_root + "/" + kBackupFolder + "/" +
                           std::to_string(hooks_.now ? hooks_.now() : std::chrono::duration_cast<std::chrono::seconds>(
                                              std::chrono::system_clock::now().time_since_epoch()).count());
  bool backed_up = false;
  const auto backup = [&](const Decision& decision) {
    const auto source = Absolute(location, decision.local);
    const auto data = ReadFile(source, kMaxRemoteFileBytes);
    if (!data) return true;  // nothing there to keep
    backed_up = true;
    return WriteFileAtomic(backup_root + "/" + (decision.local.root == SaveRoot::kGame ? "game/" : "prefix/") +
                               decision.local.path,
                           *data);
  };

  std::string failure;
  std::string detail;  // for the log: which file and what failed (never a URL path or header: they carry tokens)
  std::optional<RemoteFileList> final_list;
  const auto result = hooks_.session(session, kSessionDeadlineMs, cancel,
                                     [&](const CmClient::ServiceCall& call, std::uint64_t steam_id) {
    if (AccountKey(steam_id) != plan.account_key) {
      failure = "The signed-in Steam account changed. Check again.";
      return CmError::kRejected;
    }
    std::vector<std::uint8_t> reply;
    const auto list = [&]() -> std::optional<RemoteFileList> {
      reply.clear();
      if (call(kGetAppFileChangelist, EncodeChangelistRequest(location.app_id, 0), &reply, nullptr) != CmError::kNone)
        return std::nullopt;
      return DecodeChangelistResponse(reply);
    };
    const auto current = list();
    if (!current) {
      failure = "Steam did not return the Cloud save list. Try again.";
      return CmError::kProtocol;
    }
    if (current->change_number != plan.change_number) {
      failure = "Steam Cloud changed since the check. Check again.";
      return CmError::kRejected;
    }

    // One Cloud file's current content, checked against what the check saw.
    const auto fetch = [&](const Decision& decision) -> std::optional<std::vector<std::uint8_t>> {
      PLAYDEN_CLOUD_TRACE("download");
      reply.clear();
      const auto named = "file=" + decision.remote_name;
      int eresult = 0;
      if (call(kClientFileDownload, EncodeFileDownloadRequest(location.app_id, decision.remote_name), &reply, &eresult) !=
          CmError::kNone) {
        failure = "Steam refused a Cloud save download.";
        detail = named + " download-request eresult=" + std::to_string(eresult);
        return std::nullopt;
      }
      const auto ticket = DecodeFileDownloadResponse(reply);
      if (!ticket || ticket->encrypted || ticket->explicit_delete || ticket->sha1 != decision.remote_fact.sha1 ||
          ticket->raw_file_size != decision.remote_fact.size || ticket->file_size > kMaxHttpResponseBytes) {
        detail = !ticket ? named + " ticket=undecodable"
                         : named + " ticket encrypted=" + std::to_string(ticket->encrypted) +
                               " deleted=" + std::to_string(ticket->explicit_delete) +
                               " sha1_match=" + std::to_string(ticket->sha1 == decision.remote_fact.sha1) +
                               " raw_size=" + std::to_string(ticket->raw_file_size) + " listed_size=" +
                               std::to_string(decision.remote_fact.size) + " file_size=" + std::to_string(ticket->file_size);
        failure = ticket && ticket->file_size > kMaxHttpResponseBytes ? "A Cloud save is too large to download yet."
                                                                       : "Steam described a Cloud save Playden cannot use.";
        return std::nullopt;
      }
      const auto url = TransferUrl(ticket->use_https, ticket->host, ticket->path);
      if (!url) {
        failure = "Steam described a Cloud save Playden cannot use.";
        detail = named + " transfer-url refused host=" + ticket->host;
        return std::nullopt;
      }
      HttpRequest request;
      request.url = *url;
      request.headers = ticket->headers;
      request.timeout_ms = kMaxHttpTimeoutMs;
      request.max_response_bytes = static_cast<std::size_t>(std::max<std::uint64_t>(ticket->file_size, 1));
      const auto response = hooks_.http(request, cancel);
      if (!response.ok() || response.status_code != 200) {
        // A DNS server that blocks the storage host (a sinkhole address) fails here every time.
        failure = response.error == HttpError::kNameResolutionFailed
                      ? "The PS5 couldn't reach Steam Cloud's storage. Check its DNS settings, then try again."
                      : "A Cloud save could not be downloaded. Try again.";
        detail = named + " http error=" + HttpFailureName(response) + " status=" + std::to_string(response.status_code) +
                 " host=" + ticket->host + " file_size=" + std::to_string(ticket->file_size) +
                 " raw_size=" + std::to_string(ticket->raw_file_size) + " body=" + std::to_string(response.body.size());
        return std::nullopt;
      }
      std::vector<std::uint8_t> data = response.body;
      if (ticket->file_size != ticket->raw_file_size) {
        std::vector<std::uint8_t> unpacked;
        if (!UnzipFirstEntry(response.body, InflateRaw, &unpacked)) {
          failure = "A downloaded Cloud save was damaged.";
          detail = named + " unzip failed body=" + std::to_string(response.body.size());
          return std::nullopt;
        }
        data = std::move(unpacked);
      }
      if (data.size() != decision.remote_fact.size || Sha1(data) != decision.remote_fact.sha1) {
        failure = "A downloaded Cloud save was damaged.";
        detail = named + " content size=" + std::to_string(data.size()) + " listed_size=" +
                 std::to_string(decision.remote_fact.size) + " sha1_match=0";
        return std::nullopt;
      }
      return data;
    };

    for (const auto& decision : downloads) {
      const auto fetched = fetch(decision);
      if (!fetched) return CmError::kProtocol;
      const auto& data = *fetched;
      const auto target = Absolute(location, decision.local);
      if (!backup(decision) || !WriteFileAtomic(target, data)) {
        failure = "A Cloud save could not be written on this device.";
        detail = "file=" + decision.remote_name + " write failed errno=" + std::to_string(errno);
        return CmError::kProtocol;
      }
      SetModified(target, decision.remote_fact.time);
      ++outcome.downloaded;
      count_bytes(decision.remote_fact.size);
    }
    for (const auto& decision : local_deletes) {
      if (!backup(decision)) {
        failure = "A local save could not be backed up.";
        return CmError::kProtocol;
      }
      std::remove(Absolute(location, decision.local).c_str());
      ++outcome.removed_local;
    }

    if (!uploads.empty() || !remote_deletes.empty()) {
      // The files must still be what the check saw.
      std::vector<std::vector<std::uint8_t>> contents;
      for (const auto& decision : uploads) {
        auto data = ReadFile(Absolute(location, decision.local), kMaxRemoteFileBytes);
        if (!data || data->size() != decision.local_fact.size || Sha1(*data) != decision.local_fact.sha1) {
          failure = "A local save changed since the check. Check again.";
          return CmError::kRejected;
        }
        contents.push_back(std::move(*data));
      }
      // Keep the Cloud copies this upload replaces or removes, next to the local backups.
      for (const auto* list : {&uploads, &remote_deletes})
        for (const auto& decision : *list) {
          if (!decision.remote_fact.present) continue;
          PLAYDEN_CLOUD_TRACE("backup-cloud");
          const auto fetched = fetch(decision);
          if (!fetched) return CmError::kProtocol;
          backed_up = true;
          if (!WriteFileAtomic(backup_root + "/cloud/" + (decision.local.root == SaveRoot::kGame ? "game/" : "prefix/") +
                                   decision.local.path,
                               *fetched)) {
            failure = "Playden could not keep a copy of the Cloud saves it would replace.";
            return CmError::kProtocol;
          }
        }
      PLAYDEN_CLOUD_TRACE("upload-batch");
      std::vector<std::string> upload_names, delete_names;
      for (const auto& decision : uploads) upload_names.push_back(decision.remote_name);
      for (const auto& decision : remote_deletes) delete_names.push_back(decision.remote_name);
      const auto client_id = ClientId(location.state_root);
      PLAYDEN_CLOUD_TRACE("begin-batch");
      reply.clear();
      if (call(kBeginAppUploadBatch, EncodeBeginBatchRequest(location.app_id, kMachineName, upload_names, delete_names,
                                                             client_id, 0),
               &reply, nullptr) != CmError::kNone) {
        failure = "Steam refused the Cloud save upload.";
        return CmError::kRejected;
      }
      const auto batch = DecodeBeginBatchResponse(reply);
      if (!batch) {
        failure = "Steam refused the Cloud save upload.";
        return CmError::kProtocol;
      }
      const auto abort_batch = [&](const std::string& why, CmError error) {
        std::vector<std::uint8_t> ignored;
        call(kCompleteAppUploadBatchBlocking, EncodeCompleteBatchRequest(location.app_id, batch->batch_id, false), &ignored, nullptr);
        failure = why;
        return error;
      };
      if (batch->app_change_number != 0 && batch->app_change_number != plan.change_number + 1)
        return abort_batch("Steam Cloud changed since the check. Check again.", CmError::kRejected);
      for (std::size_t index = 0; index < uploads.size(); ++index) {
        const auto& decision = uploads[index];
        const auto& data = contents[index];
        const auto time_stamp = decision.local_fact.time;
        PLAYDEN_CLOUD_TRACE("upload-file");
        reply.clear();
        int eresult = 0;
        if (call(kClientBeginFileUpload,
                 EncodeBeginFileUploadRequest(location.app_id, data.size(), decision.local_fact.sha1, time_stamp,
                                              decision.remote_name, batch->batch_id),
                 &reply, &eresult) != CmError::kNone) {
          detail = "file=" + decision.remote_name + " upload-request eresult=" + std::to_string(eresult);
          return abort_batch("Steam refused a Cloud save upload (" + std::to_string(eresult) + ").", CmError::kRejected);
        }
        const auto upload = DecodeBeginFileUploadResponse(reply);
        if (!upload || upload->encrypt_file) return abort_batch("Steam asked for an upload Playden cannot make.", CmError::kProtocol);
        bool transferred = true;
        for (const auto& block : upload->blocks) {
          const auto url = TransferUrl(block.use_https, block.host, block.path);
          if (!url || (block.http_method != 3 && block.http_method != 4) ||
              (!block.explicit_body && block.offset + block.length > data.size())) {
            detail = "file=" + decision.remote_name + " upload block refused method=" + std::to_string(block.http_method) +
                     " host=" + block.host;
            transferred = false;
            break;
          }
          HttpRequest request;
          request.method = block.http_method == 4 ? HttpMethod::kPut : HttpMethod::kPost;
          request.url = *url;
          request.headers = block.headers;
          request.timeout_ms = kMaxHttpTimeoutMs;
          request.body = block.explicit_body ? *block.explicit_body
                                             : std::vector<std::uint8_t>(data.begin() + static_cast<std::ptrdiff_t>(block.offset),
                                                                         data.begin() + static_cast<std::ptrdiff_t>(block.offset + block.length));
          const auto response = hooks_.http(request, cancel);
          if (!response.ok() || response.status_code < 200 || response.status_code >= 300) {
            detail = "file=" + decision.remote_name + " upload http error=" + HttpFailureName(response) +
                     " status=" + std::to_string(response.status_code) + " host=" + block.host +
                     " bytes=" + std::to_string(request.body.size());
            transferred = false;
            break;
          }
        }
        reply.clear();
        const auto committed = call(kClientCommitFileUpload,
                                    EncodeCommitFileUploadRequest(transferred, location.app_id, decision.local_fact.sha1,
                                                                  decision.remote_name),
                                    &reply, nullptr);
        const auto committed_ok = committed == CmError::kNone ? DecodeCommitFileUploadResponse(reply) : std::nullopt;
        if (!transferred || !committed_ok || !*committed_ok) {
          if (transferred)
            detail = "file=" + decision.remote_name + " commit " +
                     (committed != CmError::kNone ? "request failed" : !committed_ok ? "reply undecodable" : "refused");
          return abort_batch("A Cloud save could not be uploaded. Try again.", CmError::kNetwork);
        }
        ++outcome.uploaded;
        count_bytes(decision.local_fact.size);
      }
      for (const auto& decision : remote_deletes) {
        reply.clear();
        if (call(kClientDeleteFile, EncodeDeleteFileRequest(location.app_id, decision.remote_name, batch->batch_id), &reply,
                 nullptr) != CmError::kNone)
          return abort_batch("Steam refused to remove a Cloud save.", CmError::kRejected);
        ++outcome.removed_remote;
      }
      PLAYDEN_CLOUD_TRACE("complete-batch");
      reply.clear();
      if (call(kCompleteAppUploadBatchBlocking, EncodeCompleteBatchRequest(location.app_id, batch->batch_id, true), &reply,
               nullptr) != CmError::kNone) {
        failure = "Steam did not finish the Cloud save upload.";
        return CmError::kRejected;
      }
    }
    PLAYDEN_CLOUD_TRACE("verify");
    final_list = list();
    if (!final_list) {
      failure = "Steam did not confirm the Cloud saves. Check again.";
      return CmError::kProtocol;
    }
    return CmError::kNone;
  });
  if (backed_up) PruneBackups(location.state_root);
  if (result != CmError::kNone) return fail(failure.empty() ? "Steam is not reachable. Try again." : failure, detail);

  // The verified common state becomes the next baseline: every Cloud file that maps locally and
  // now matches its local copy.
  std::vector<std::pair<std::string, FileFact>> agreed;
  std::map<std::string, const Decision*> by_name;
  for (const auto& decision : plan.decisions) by_name[Lower(decision.remote_name)] = &decision;
  for (const auto& file : final_list->files) {
    if (file.state != RemoteState::kPresent) continue;
    const auto found = by_name.find(Lower(file.name));
    if (found == by_name.end()) continue;
    const auto local = Fingerprint(Absolute(location, found->second->local));
    const FileFact cloud{true, file.sha1, file.raw_size, file.time_stamp};
    if (local && SameContent(*local, cloud)) agreed.emplace_back(file.name, cloud);
  }
  const auto before = LoadBaseline(location.state_root);
  const bool uploaded_before = before.loaded && before.account_key == plan.account_key && before.uploaded;
  SaveBaseline(location.state_root, plan.account_key, final_list->change_number, agreed,
               uploaded_before || outcome.uploaded > 0 || outcome.removed_remote > 0);
  // What each side holds now, for the game's Saves page.
  for (const auto& file : final_list->files) {
    if (file.state != RemoteState::kPresent || !by_name.count(Lower(file.name))) continue;
    ++outcome.remote_files;
    outcome.remote_bytes += file.raw_size;
    outcome.remote_latest = std::max<std::uint64_t>(outcome.remote_latest, file.time_stamp);
  }
  for (const auto& decision : plan.decisions) {
    if (decision.action == Action::kUnavailable) continue;
    if (const auto local = Fingerprint(Absolute(location, decision.local))) {
      ++outcome.local_files;
      outcome.local_bytes += local->size;
      outcome.local_latest = std::max<std::uint64_t>(outcome.local_latest, local->time);
    }
  }
  // The index the quick check compares with: the local files as they are now, after the transfers.
  if (const auto index = LoadIndex(location.state_root); index.loaded && index.account_key == plan.account_key) {
    std::vector<LocalSavePath> known = Paths(index.files);
    for (const auto& decision : plan.decisions) known.push_back(decision.local);
    const SavePaths paths(SaveMapping{index.rules, {}});
    WriteIndex(location, plan.account_key, final_list->change_number, index.rules, ScanLocal(location, paths, known));
  }
  outcome.ok = true;
  const std::size_t moved = outcome.downloaded + outcome.uploaded + outcome.removed_local + outcome.removed_remote;
  outcome.message = moved == 0 ? "Saves are up to date"
                    : outcome.downloaded > 0 && outcome.uploaded > 0
                        ? Plural(outcome.downloaded, "save downloaded", "saves downloaded") + ", " +
                              Plural(outcome.uploaded, "uploaded", "uploaded")
                    : outcome.downloaded > 0 ? Plural(outcome.downloaded, "save downloaded", "saves downloaded")
                    : outcome.uploaded > 0   ? Plural(outcome.uploaded, "save uploaded", "saves uploaded")
                                             : "Saves updated";
  return outcome;
}

std::vector<Drift> CloudSync::QuickCheck(const StoredSession& session, const std::vector<GameSaveLocation>& locations,
                                         const std::atomic<bool>& cancel) {
  std::vector<Drift> drift(locations.size(), Drift::kUnknown);
  std::vector<SaveIndex> indexes(locations.size());
  std::vector<bool> local_changed(locations.size(), false);
  bool any = false;
  PLAYDEN_CLOUD_TRACE("quick-local");
  for (std::size_t index = 0; index < locations.size(); ++index) {
    indexes[index] = LoadIndex(locations[index].state_root);
    // Folders that moved (a reinstall, a new bottle) leave nothing to compare with.
    if (indexes[index].loaded && (indexes[index].game_root != locations[index].game_root ||
                                  indexes[index].prefix_root != locations[index].prefix_root))
      indexes[index].loaded = false;
    if (!indexes[index].loaded) continue;
    any = true;
    const SavePaths paths(SaveMapping{indexes[index].rules, {}});
    local_changed[index] = !SameFiles(ScanLocal(locations[index], paths, Paths(indexes[index].files)), indexes[index].files);
  }
  if (!any) return drift;
  PLAYDEN_CLOUD_TRACE("quick-remote");
  hooks_.session(session, kSessionDeadlineMs, cancel, [&](const CmClient::ServiceCall& call, std::uint64_t steam_id) {
    const auto account = AccountKey(steam_id);
    for (std::size_t index = 0; index < locations.size(); ++index) {
      if (cancel.load()) break;
      const auto& known = indexes[index];
      if (!known.loaded || known.account_key != account) continue;
      std::vector<std::uint8_t> reply;
      if (call(kGetAppFileChangelist, EncodeChangelistRequest(locations[index].app_id, known.change_number), &reply, nullptr) !=
          CmError::kNone)
        continue;
      const auto current = DecodeChangelistChangeNumber(reply);
      if (!current) continue;
      const bool remote = *current != known.change_number;
      drift[index] = remote && local_changed[index] ? Drift::kBoth
                     : remote                       ? Drift::kRemote
                     : local_changed[index]         ? Drift::kLocal
                                                    : Drift::kUnchanged;
    }
    return CmError::kNone;
  });
  return drift;
}

bool CloudSync::RemoveLocal(const GameSaveLocation& location, std::string* message) {
  const auto index = LoadIndex(location.state_root);
  if (!index.loaded) {
    *message = "Check this game's saves first.";
    return false;
  }
  const SavePaths paths(SaveMapping{index.rules, {}});
  const auto files = ScanLocal(location, paths, Paths(index.files));
  const auto backup_root = location.state_root + "/" + kBackupFolder + "/" +
                           std::to_string(hooks_.now ? hooks_.now() : std::chrono::duration_cast<std::chrono::seconds>(
                                              std::chrono::system_clock::now().time_since_epoch()).count());
  for (const auto& [key, entry] : files) {
    const auto source = Absolute(location, entry.first);
    const auto data = ReadFile(source, kMaxRemoteFileBytes);
    if (!data || !WriteFileAtomic(backup_root + "/" + (entry.first.root == SaveRoot::kGame ? "game/" : "prefix/") +
                                      entry.first.path, *data)) {
      *message = "A save could not be backed up, so nothing was deleted.";
      return false;
    }
  }
  std::size_t removed = 0;
  for (const auto& [key, entry] : files)
    if (std::remove(Absolute(location, entry.first).c_str()) == 0) ++removed;
  if (!files.empty()) PruneBackups(location.state_root);
  *message = removed == 0 ? "No saves to delete on this console." : Plural(removed, "save deleted", "saves deleted");
  return removed == files.size();
}

}  // namespace playden::steam::cloud
