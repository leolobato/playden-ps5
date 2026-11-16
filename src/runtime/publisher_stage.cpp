#include "runtime/publisher_stage.h"

#include "runtime/artifact_hash.h"
#include "steam/backend/json.h"
#include "steam/secret.h"

#include <cerrno>
#include <cstring>
#include <map>
#include <set>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace playden::runtime {
namespace {

using steam::ZeroizeSecret;

constexpr std::size_t kMaxSourceBytes = 64 * 1024 * 1024;
constexpr std::size_t kMaxRecordBytes = 64 * 1024;

// Closes its descriptor when it goes.
class Fd {
 public:
  explicit Fd(int fd = -1) : fd_(fd) {}
  ~Fd() { Reset(); }
  Fd(Fd&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
  Fd& operator=(Fd&& other) noexcept {
    if (this != &other) {
      Reset();
      fd_ = other.fd_;
      other.fd_ = -1;
    }
    return *this;
  }
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
  int get() const { return fd_; }
  bool ok() const { return fd_ >= 0; }
  void Reset() {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
  }

 private:
  int fd_;
};

std::vector<std::string> Parts(const std::string& path) {
  std::vector<std::string> parts;
  for (std::size_t begin = 0;;) {
    const auto end = path.find('/', begin);
    parts.push_back(path.substr(begin, end == std::string::npos ? std::string::npos : end - begin));
    if (end == std::string::npos) return parts;
    begin = end + 1;
  }
}

// A folder known to be a plain folder (never a link), addressed by path: the PS5 title can't use
// the descriptor-relative calls (openat and friends), so every step is a path-based call.
struct Folder {
  std::string path;
  bool good = false;
  int error = 0;  // errno of the step that failed
  bool ok() const { return good; }
  const std::string& get() const { return path; }
};

std::string Why(int error) { return error ? std::string(" (") + std::strerror(error) + ")" : std::string(); }

std::string Join(const std::string& dir, const std::string& name) {
  return !dir.empty() && dir.back() == '/' ? dir + name : dir + "/" + name;
}

Folder FolderFailed(int error) {
  Folder folder;
  folder.error = error;
  return folder;
}

// Checks that path is a folder and not a link.
Folder OpenFolder(const std::string& path) {
  struct stat info {};
  if (::lstat(path.c_str(), &info) != 0) return FolderFailed(errno);
  if (!S_ISDIR(info.st_mode)) return FolderFailed(S_ISLNK(info.st_mode) ? ELOOP : ENOTDIR);
  Folder folder;
  folder.path = path;
  folder.good = true;
  return folder;
}

// Opens the folder that holds path (relative to root), one part at a time without following
// links. With create, missing folders are made 0755. *missing tells a folder that isn't there
// (without create) from one that can't be used.
Folder OpenParent(const Folder& root, const std::string& path, bool create, std::string* name,
                  bool* missing = nullptr) {
  if (missing) *missing = false;
  const auto parts = Parts(path);
  *name = parts.back();
  if (!root.ok()) return FolderFailed(root.error);
  Folder dir = root;
  for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
    const auto& part = parts[i];
    if (part.empty() || part == "." || part == "..") return FolderFailed(EINVAL);
    const auto next = Join(dir.path, part);
    Folder opened = OpenFolder(next);
    if (!opened.ok() && opened.error == ENOENT) {
      if (!create) {
        if (missing) *missing = true;
        return opened;
      }
      if (::mkdir(next.c_str(), 0755) != 0 && errno != EEXIST) return FolderFailed(errno);
      opened = OpenFolder(next);
      // A folder made now gets 0755 whatever the umask; existing folders keep their mode.
      if (opened.ok() && ::chmod(next.c_str(), 0755) != 0) return FolderFailed(errno);
    }
    if (!opened.ok()) return opened;
    dir = std::move(opened);
  }
  return dir;
}

enum class Found { kMissing, kSame, kDifferent, kNotFile };

// Reads a regular file (no link) of at most limit bytes.
bool ReadAt(const std::string& dir, const std::string& name, std::size_t limit, std::string* bytes) {
  Fd fd(::open(Join(dir, name).c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
  struct stat info {};
  if (!fd.ok() || ::fstat(fd.get(), &info) != 0 || !S_ISREG(info.st_mode) ||
      static_cast<std::uint64_t>(info.st_size) > limit)
    return false;
  bytes->assign(static_cast<std::size_t>(info.st_size), '\0');
  std::size_t at = 0;
  while (at < bytes->size()) {
    const auto count = ::read(fd.get(), bytes->data() + at, bytes->size() - at);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) break;
    at += static_cast<std::size_t>(count);
  }
  return at == bytes->size();
}

// What dir/name holds compared with bytes. Anything but a regular file is kNotFile.
Found Compare(const std::string& dir, const std::string& name, const std::string& bytes) {
  struct stat info {};
  if (::lstat(Join(dir, name).c_str(), &info) != 0)
    return errno == ENOENT ? Found::kMissing : Found::kNotFile;
  if (!S_ISREG(info.st_mode)) return Found::kNotFile;
  if (static_cast<std::uint64_t>(info.st_size) != bytes.size()) return Found::kDifferent;
  std::string current;
  if (!ReadAt(dir, name, bytes.size(), &current)) return Found::kNotFile;
  const bool same = current == bytes;
  return same ? Found::kSame : Found::kDifferent;
}

bool RegularOrMissing(const std::string& dir, const std::string& name, bool* present) {
  struct stat info {};
  if (::lstat(Join(dir, name).c_str(), &info) != 0) {
    *present = false;
    return errno == ENOENT;
  }
  *present = true;
  return S_ISREG(info.st_mode);
}

// Writes bytes to dir/name through a temporary file renamed over it.
bool WriteAt(const std::string& dir, const std::string& name, const std::string& bytes, mode_t mode,
             std::string* why = nullptr) {
  const auto temporary = "." + name + ".playden-new";
  const auto temporary_path = Join(dir, temporary);
  const auto target_path = Join(dir, name);
  ::unlink(temporary_path.c_str());
  Fd fd(::open(temporary_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, mode));
  if (!fd.ok()) {
    if (why) *why = Why(errno);
    return false;
  }
  std::size_t at = 0;
  while (at < bytes.size()) {
    const auto count = ::write(fd.get(), bytes.data() + at, bytes.size() - at);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) break;
    at += static_cast<std::size_t>(count);
  }
  bool ok = at == bytes.size() && ::fchmod(fd.get(), mode) == 0 && ::fsync(fd.get()) == 0;
  int error = errno;
  fd.Reset();
  if (ok) {
    ok = ::rename(temporary_path.c_str(), target_path.c_str()) == 0;
    error = errno;
  }
  if (!ok) {
    if (why) *why = Why(error);
    ::unlink(temporary_path.c_str());
  }
  return ok;
}

// The bytes a files entry places: the pack file or the literal text.
bool Content(const PublisherStageFile& file, const std::string& pack, std::string* bytes, std::string* reason) {
  if (file.text) {
    *bytes = *file.text;
    return true;
  }
  std::string name;
  const Folder dir = OpenParent(Folder{pack, true, 0}, *file.source, false, &name);
  errno = 0;
  if (dir.ok() && ReadAt(dir.get(), name, kMaxSourceBytes, bytes)) return true;
  *reason = "the pack file " + *file.source + " is missing or unreadable" + Why(dir.ok() ? errno : dir.error);
  return false;
}

std::string IniBytes(const std::vector<std::string>& lines) {
  std::string bytes;
  for (const auto& line : lines) bytes += line + "\r\n";
  return bytes;
}

// The SHA-256 of a regular file below dir ("" when it can't be read).
std::string HashAt(const std::string& dir, const std::string& name) {
  std::string bytes;
  if (!ReadAt(dir, name, kMaxSourceBytes, &bytes)) return {};
  return HashArtifactBytes(bytes).value_or(std::string());
}

// What Playden placed: target -> SHA-256 of the bytes it wrote, and the release they came from.
struct StageRecord {
  std::string pack;
  std::map<std::string, std::string> placed;
  std::map<std::string, std::string> kept;  // target -> the keep_original its game file moved to
  std::string credential;                   // the credential file's name, to take it away without the manifest
};

// The record in the title folder; empty when missing, damaged or not a plain file.
StageRecord ReadRecord(const std::string& root) {
  StageRecord record;
  std::string text;
  if (!ReadAt(root, kPublisherStageRecordName, kMaxRecordBytes, &text)) return record;
  const auto parsed = steam::json::Parse(text);
  if (!parsed || !parsed->is_object()) return record;
  const auto* version = parsed->Find("version");
  const auto* pack = parsed->Find("pack");
  const auto* placed = parsed->Find("placed");
  if (!version || version->AsUint64() != std::optional<std::uint64_t>(1) || !placed || !placed->is_object()) return record;
  if (pack && pack->is_string()) record.pack = pack->text();
  for (const auto& [target, hash] : placed->object())
    if (hash.is_string() && hash.text().size() == 64) record.placed[target] = hash.text();
  const auto* credential = parsed->Find("credential");
  if (credential && credential->is_string()) record.credential = credential->text();
  const auto* kept = parsed->Find("kept");
  if (kept && kept->is_object())
    for (const auto& [target, name] : kept->object())
      if (name.is_string() && !name.text().empty()) record.kept[target] = name.text();
  return record;
}

std::string JsonString(const std::string& text) {
  static const char kHex[] = "0123456789abcdef";
  std::string out = "\"";
  for (const unsigned char c : text) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += static_cast<char>(c);
    } else if (c < 0x20) {
      out += "\\u00";
      out += kHex[c >> 4];
      out += kHex[c & 15];
    } else {
      out += static_cast<char>(c);
    }
  }
  return out + "\"";
}

bool WriteRecord(const std::string& root, const StageRecord& record) {
  std::string text = "{\"version\":1,\"pack\":" + JsonString(record.pack) + ",\"placed\":{";
  bool first = true;
  for (const auto& [target, hash] : record.placed) {
    text += (first ? "" : ",") + JsonString(target) + ":" + JsonString(hash);
    first = false;
  }
  text += "}";
  if (!record.credential.empty()) text += ",\"credential\":" + JsonString(record.credential);
  if (!record.kept.empty()) {
    text += ",\"kept\":{";
    first = true;
    for (const auto& [target, name] : record.kept) {
      text += (first ? "" : ",") + JsonString(target) + ":" + JsonString(name);
      first = false;
    }
    text += "}";
  }
  text += "}\n";
  std::string current;
  if (ReadAt(root, kPublisherStageRecordName, kMaxRecordBytes, &current) && current == text) return true;
  return WriteAt(root, kPublisherStageRecordName, text, 0644);
}

std::string BaseName(std::string path) {
  while (path.size() > 1 && path.back() == '/') path.pop_back();
  const auto slash = path.rfind('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

PublisherStageResult Failed(PublisherStageResult result, std::string message) {
  result.ok = false;
  result.message = std::move(message);
  return result;
}

bool ArchMatches(const PublisherStageFile& file, const PublisherTitleSpec* title) {
  return !title || !file.arch || *file.arch == title->arch;
}

// Removes a regular file below root; a missing one (or folder) is nothing to do.
bool RemoveRegular(const Folder& root, const std::string& path, std::vector<std::string>* changed, std::string* reason) {
  std::string name;
  bool missing = false;
  const Folder dir = OpenParent(root, path, false, &name, &missing);
  if (missing) return true;
  bool present = false;
  if (!dir.ok() || !RegularOrMissing(dir.get(), name, &present)) {
    *reason = path + " is not a plain file" + Why(dir.ok() ? 0 : dir.error);
    return false;
  }
  if (!present) return true;
  if (::unlink(Join(dir.get(), name).c_str()) != 0 && errno != ENOENT) {
    *reason = path + " could not be removed" + Why(errno);
    return false;
  }
  if (changed) changed->push_back(path);
  return true;
}

// Takes back one target of the record: its kept original goes back over it (when the target is
// still Playden's or gone), or Playden's own bytes are removed. A target the game changed stays.
bool RestoreRecorded(const Folder& root, const std::string& target, const std::string& hash,
                     const std::string& kept_path, std::vector<std::string>* changed, std::string* reason) {
  std::string name, kept_name;
  bool missing = false, kept_present = false;
  Folder kept;
  if (!kept_path.empty()) {
    bool kept_missing = false;
    kept = OpenParent(root, kept_path, false, &kept_name, &kept_missing);
    if (!kept.ok() && !kept_missing) {
      *reason = "the folder of " + kept_path + " can't be used" + Why(kept.error);
      return false;
    }
    if (kept.ok() && !RegularOrMissing(kept.get(), kept_name, &kept_present)) {
      *reason = kept_path + " is not a plain file";
      return false;
    }
  }
  Folder dir = OpenParent(root, target, false, &name, &missing);
  if (!dir.ok() && !missing) {
    *reason = "the folder of " + target + " can't be used" + Why(dir.error);
    return false;
  }
  bool present = false;
  if (dir.ok() && !RegularOrMissing(dir.get(), name, &present)) {
    *reason = target + " is not a plain file";
    return false;
  }
  const bool ours = present && HashAt(dir.get(), name) == hash;
  if (kept_present && (!present || ours)) {
    if (missing) dir = OpenParent(root, target, true, &name);
    if (!dir.ok() || ::rename(Join(kept.get(), kept_name).c_str(), Join(dir.get(), name).c_str()) != 0) {
      *reason = kept_path + " could not be put back as " + target + Why(dir.ok() ? errno : dir.error);
      return false;
    }
    changed->push_back(target);
  } else if (ours) {
    if (::unlink(Join(dir.get(), name).c_str()) != 0 && errno != ENOENT) {
      *reason = target + " could not be removed" + Why(errno);
      return false;
    }
    changed->push_back(target);
  }
  return true;
}

// Undoes what the record lists for targets outside listed (all of them without a manifest).
bool RestoreUnlisted(const Folder& root, const StageRecord& record, const std::set<std::string>& listed,
                     std::vector<std::string>* changed, std::string* reason) {
  for (const auto& [target, hash] : record.placed) {
    if (listed.count(target)) continue;
    const auto kept = record.kept.find(target);
    if (!RestoreRecorded(root, target, hash, kept == record.kept.end() ? std::string() : kept->second, changed,
                         reason))
      return false;
  }
  return true;
}

}  // namespace

PublisherStageResult ApplyPublisherStage(const PublisherStageManifest& manifest, const std::string& pack_dir,
                                         const std::string& title_folder, const std::string& app_id,
                                         const std::string& runtime_path) {
  PublisherStageResult result;
  const auto title = manifest.titles.find(app_id);
  if (title == manifest.titles.end()) return Failed(std::move(result), "the pack doesn't list app " + app_id);
  const Folder root = OpenFolder(title_folder);
  if (!root.ok()) return Failed(std::move(result), "the title folder can't be opened" + Why(root.error));
  const Folder pack = OpenFolder(pack_dir);
  if (!pack.ok()) return Failed(std::move(result), "the unpacked pack can't be opened" + Why(pack.error));
  auto record = ReadRecord(root.get());
  record.credential = manifest.credential_file;
  for (const auto& file : manifest.files) {
    if (!ArchMatches(file, &title->second)) continue;
    if (file.target == kPublisherStageRecordName || file.keep_original == kPublisherStageRecordName)
      return Failed(std::move(result), file.target + " is Playden's own record");
    std::string bytes, reason, name;
    if (!Content(file, pack.get(), &bytes, &reason)) return Failed(std::move(result), reason);
    const auto hash = HashArtifactBytes(bytes).value_or(std::string());
    if (hash.empty()) return Failed(std::move(result), "the pack file for " + file.target + " can't be hashed");
    const Folder dir = OpenParent(root, file.target, true, &name);
    if (!dir.ok()) return Failed(std::move(result), "the folder of " + file.target + " can't be used" + Why(dir.error));
    const auto found = Compare(dir.get(), name, bytes);
    if (found == Found::kNotFile) return Failed(std::move(result), file.target + " is not a plain file");
    if (found == Found::kSame) {
      // Bytes already there are Playden's only when the record says so, or a kept original proves
      // it; otherwise they are the game's own file and restore must leave them alone.
      bool ours = record.placed.count(file.target) > 0;
      if (!ours && !file.keep_original.empty()) {
        std::string kept_name;
        const Folder kept = OpenParent(root, file.keep_original, false, &kept_name);
        bool kept_present = false;
        ours = kept.ok() && RegularOrMissing(kept.get(), kept_name, &kept_present) && kept_present;
      }
      if (ours) record.placed[file.target] = hash;
      continue;
    }
    if (found == Found::kDifferent && !file.keep_original.empty()) {
      std::string kept_name;
      const Folder kept = OpenParent(root, file.keep_original, true, &kept_name);
      bool kept_present = false;
      if (!kept.ok() || !RegularOrMissing(kept.get(), kept_name, &kept_present))
        return Failed(std::move(result), file.keep_original + " is not a plain file");
      // Ours: what the record says Playden placed (an older pack's file), or, without a record of
      // it, any file beside an existing kept original (never replace that without proof that the
      // game updated it). Anything else is the game's own file, or its update: it is kept aside,
      // replacing an older kept copy.
      const auto recorded = record.placed.find(file.target);
      const bool ours = recorded != record.placed.end() ? HashAt(dir.get(), name) == recorded->second : kept_present;
      if (!ours) {
        if (::rename(Join(dir.get(), name).c_str(), Join(kept.get(), kept_name).c_str()) != 0)
          return Failed(std::move(result), file.target + " could not be kept as " + file.keep_original + Why(errno));
        result.changed.push_back(file.keep_original);
      }
    }
    std::string why;
    if (!WriteAt(dir.get(), name, bytes, 0644, &why)) {
      record.placed.erase(file.target);  // what is there now is unknown
      WriteRecord(root.get(), record);
      return Failed(std::move(result), file.target + " could not be written" + why);
    }
    record.placed[file.target] = hash;
    if (!file.keep_original.empty()) record.kept[file.target] = file.keep_original;
    // Recorded at once: a stage that fails later still knows this file is Playden's.
    if (!WriteRecord(root.get(), record)) return Failed(std::move(result), "Playden's stage record could not be written");
    result.changed.push_back(file.target);
  }
  if (manifest.ini) {
    std::vector<std::string> lines;
    std::string reason, name;
    if (!ExpandPublisherStageIni(manifest, title->second, runtime_path, &lines, &reason))
      return Failed(std::move(result), reason);
    const auto bytes = IniBytes(lines);
    const Folder dir = OpenParent(root, manifest.ini->target, true, &name);
    if (!dir.ok())
      return Failed(std::move(result), "the folder of " + manifest.ini->target + " can't be used" + Why(dir.error));
    const auto found = Compare(dir.get(), name, bytes);
    if (found == Found::kNotFile) return Failed(std::move(result), manifest.ini->target + " is not a plain file");
    if (found != Found::kSame) {
      std::string why;
      if (!WriteAt(dir.get(), name, bytes, 0644, &why))
        return Failed(std::move(result), manifest.ini->target + " could not be written" + why);
      result.changed.push_back(manifest.ini->target);
      const auto ini_hash = HashArtifactBytes(bytes).value_or(std::string());
      if (!ini_hash.empty()) record.placed[manifest.ini->target] = ini_hash;
    }
  }
  record.pack = BaseName(pack_dir);
  if (!WriteRecord(root.get(), record)) return Failed(std::move(result), "Playden's stage record could not be written");
  result.ok = true;
  return result;
}

PublisherStageResult RestorePublisherStage(const PublisherStageManifest& manifest, const std::string& pack_dir,
                                           const std::string& title_folder, const std::string& app_id) {
  PublisherStageResult result;
  const auto listed = manifest.titles.find(app_id);
  const PublisherTitleSpec* title = listed == manifest.titles.end() ? nullptr : &listed->second;
  const Folder root = OpenFolder(title_folder);
  if (!root.ok()) {
    if (root.error == ENOENT) {
      result.ok = true;  // no title folder: nothing was placed
      return result;
    }
    return Failed(std::move(result), "the title folder can't be opened" + Why(root.error));
  }
  const Folder pack = OpenFolder(pack_dir);
  if (!pack.ok()) return Failed(std::move(result), "the unpacked pack can't be opened" + Why(pack.error));
  const auto record = ReadRecord(root.get());
  for (auto file = manifest.files.rbegin(); file != manifest.files.rend(); ++file) {
    if (!ArchMatches(*file, title)) continue;
    std::string bytes, reason, name;
    if (!Content(*file, pack.get(), &bytes, &reason)) return Failed(std::move(result), reason);
    bool missing = false;
    Folder dir = OpenParent(root, file->target, false, &name, &missing);
    if (!dir.ok() && !missing)
      return Failed(std::move(result), "the folder of " + file->target + " can't be used" + Why(dir.error));
    auto found = missing ? Found::kMissing : Compare(dir.get(), name, bytes);
    if (found == Found::kNotFile) return Failed(std::move(result), file->target + " is not a plain file");
    // A file an earlier pack placed (the record has its hash) is ours as well.
    if (const auto recorded = record.placed.find(file->target);
        found == Found::kDifferent && recorded != record.placed.end() && HashAt(dir.get(), name) == recorded->second)
      found = Found::kSame;
    bool kept_present = false;
    std::string kept_name;
    Folder kept;
    if (!file->keep_original.empty()) {
      bool kept_missing = false;
      kept = OpenParent(root, file->keep_original, false, &kept_name, &kept_missing);
      if (!kept.ok() && !kept_missing)
        return Failed(std::move(result), "the folder of " + file->keep_original + " can't be used" + Why(kept.error));
      if (kept.ok() && !RegularOrMissing(kept.get(), kept_name, &kept_present))
        return Failed(std::move(result), file->keep_original + " is not a plain file");
    }
    // A target with other bytes is the game's (an update since staging): it stays, with its kept copy.
    if (found == Found::kDifferent) continue;
    // Pack bytes the record doesn't know (and no kept original proves otherwise) were the game's own.
    if (found == Found::kSame && !record.placed.count(file->target) && !kept_present) continue;
    if (kept_present) {
      if (missing) dir = OpenParent(root, file->target, true, &name);
      if (!dir.ok() || ::rename(Join(kept.get(), kept_name).c_str(), Join(dir.get(), name).c_str()) != 0)
        return Failed(std::move(result), file->keep_original + " could not be put back as " + file->target +
                                             Why(dir.ok() ? errno : dir.error));
      result.changed.push_back(file->target);
    } else if (found == Found::kSame) {
      if (::unlink(Join(dir.get(), name).c_str()) != 0 && errno != ENOENT)
        return Failed(std::move(result), file->target + " could not be removed" + Why(errno));
      result.changed.push_back(file->target);
    }
  }
  std::string reason;
  // Targets the record lists but the manifest no longer does.
  std::set<std::string> known;
  for (const auto& file : manifest.files) known.insert(file.target);
  if (manifest.ini) known.insert(manifest.ini->target);
  if (!RestoreUnlisted(root, record, known, &result.changed, &reason)) return Failed(std::move(result), reason);
  if (manifest.ini && !RemoveRegular(root, manifest.ini->target, &result.changed, &reason))
    return Failed(std::move(result), reason);
  if (!RemoveRegular(root, manifest.credential_file, &result.changed, &reason))
    return Failed(std::move(result), reason);
  if (!record.credential.empty() && record.credential != manifest.credential_file &&
      !RemoveRegular(root, record.credential, &result.changed, &reason))
    return Failed(std::move(result), reason);
  if (!RemoveRegular(root, kPublisherStageRecordName, nullptr, &reason)) return Failed(std::move(result), reason);
  result.ok = true;
  return result;
}

PublisherStageResult RestorePublisherRecord(const std::string& title_folder) {
  PublisherStageResult result;
  const Folder root = OpenFolder(title_folder);
  if (!root.ok()) {
    if (root.error == ENOENT) {
      result.ok = true;  // no title folder: nothing was placed
      return result;
    }
    return Failed(std::move(result), "the title folder can't be opened" + Why(root.error));
  }
  const auto record = ReadRecord(root.path);
  std::string reason;
  if (!RestoreUnlisted(root, record, {}, &result.changed, &reason)) return Failed(std::move(result), reason);
  if (!record.credential.empty() && !RemoveRegular(root, record.credential, &result.changed, &reason))
    return Failed(std::move(result), reason);
  if (!RemoveRegular(root, kPublisherStageRecordName, nullptr, &reason)) return Failed(std::move(result), reason);
  result.ok = true;
  return result;
}

bool WritePublisherCredential(const PublisherStageManifest& manifest, const std::string& title_folder,
                              std::string& value, std::string* reason) {
  std::string bytes = value + "\n";
  ZeroizeSecret(value);
  const auto fail = [&](std::string text) {
    ZeroizeSecret(bytes);
    if (reason) *reason = std::move(text);
    return false;
  };
  const Folder root = OpenFolder(title_folder);
  if (!root.ok()) return fail("the title folder can't be opened" + Why(root.error));
  std::string name;
  const Folder dir = OpenParent(root, manifest.credential_file, true, &name);
  bool present = false;
  if (!dir.ok() || !RegularOrMissing(dir.get(), name, &present))
    return fail(manifest.credential_file + " is not a plain file" + Why(dir.ok() ? 0 : dir.error));
  std::string why;
  if (!WriteAt(dir.get(), name, bytes, 0600, &why)) return fail(manifest.credential_file + " could not be written" + why);
  ZeroizeSecret(bytes);
  return true;
}

bool RemovePublisherCredential(const PublisherStageManifest& manifest, const std::string& title_folder,
                               std::string* reason) {
  const Folder root = OpenFolder(title_folder);
  if (!root.ok()) {
    if (root.error == ENOENT) return true;
    if (reason) *reason = "the title folder can't be opened" + Why(root.error);
    return false;
  }
  std::string ignored;
  return RemoveRegular(root, manifest.credential_file, nullptr, reason ? reason : &ignored);
}

}  // namespace playden::runtime
