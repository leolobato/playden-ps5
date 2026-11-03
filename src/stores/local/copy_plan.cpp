#include "stores/local/copy_plan.h"

#include "platform/directory_walk.h"
#include "steam/backend/json.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>

namespace playden::stores::local {
namespace {

constexpr int kOpenAny = O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC;
constexpr std::size_t kMaxLineBytes = 16 * 1024;
constexpr const char* kFileList = ".playden-files";  // DepotInstaller::kFileList

// Names Playden writes at the top of a managed install (DepotInstaller's and the copy's own).
bool ReservedName(const std::string& name) {
  return name == kCopyJournalDir || name == kFileList || name == ".playden-files.tmp" || name == ".playden-install.json" ||
         name == ".playden-install.json.tmp" || name == ".playden-download";
}

bool Unsupported(int error) { return error == ENOSYS || error == EINVAL; }
bool IsLinkError(int error) { return error == ELOOP || error == EMLINK; }

std::string Trimmed(std::string folder) {
  while (folder.size() > 1 && folder.back() == '/') folder.pop_back();
  return folder;
}

std::string Quoted(const std::string& path) { return "\"" + path + "\""; }

std::string WithThousands(std::uint64_t value) {
  auto digits = std::to_string(value);
  for (int at = static_cast<int>(digits.size()) - 3; at > 0; at -= 3) digits.insert(static_cast<std::size_t>(at), ",");
  return digits;
}

std::string Escape(const std::string& value) {
  std::string output;
  output.reserve(value.size() + 2);
  for (const unsigned char c : value) {
    if (c == '"') output += "\\\"";
    else if (c == '\\') output += "\\\\";
    else if (c < 0x20) {
      static constexpr char kHex[] = "0123456789abcdef";
      output += "\\u00";
      output.push_back(kHex[c >> 4]);
      output.push_back(kHex[c & 15]);
    } else {
      output.push_back(static_cast<char>(c));
    }
  }
  return output;
}

std::int64_t Seconds(std::int64_t value) { return value < 0 ? 0 : value; }

struct Frame {
  int fd = -1;
  DIR* listing = nullptr;
  std::string relative;
};

void Close(Frame& frame) {
  if (frame.listing) ::closedir(frame.listing);
  if (frame.fd >= 0) ::close(frame.fd);
  frame.listing = nullptr;
  frame.fd = -1;
}

// Takes `fd`.
bool OpenFrame(int fd, std::string relative, Frame* frame) {
  const int listing_fd = ::dup(fd);
  DIR* listing = listing_fd >= 0 ? ::fdopendir(listing_fd) : nullptr;
  if (!listing) {
    const int error = errno;
    if (listing_fd >= 0) ::close(listing_fd);
    ::close(fd);
    errno = error;
    return false;
  }
  *frame = Frame{fd, listing, std::move(relative)};
  return true;
}

CopyPlanBuild Failed(std::string failure, int error = 0) {
  CopyPlanBuild build;
  build.failure = std::move(failure);
  build.error = error;
  return build;
}

std::string LinkWords(const std::string& relative) {
  return "The game's folder has a link, " + Quoted(relative) +
         ". Playden can't copy links: replace it with the files it points to, then copy the game again.";
}

std::string UnreadableWords(const std::string& relative) {
  return "Playden can't read " + Quoted(relative) + " in the game's folder.";
}

bool WriteAll(FILE* file, const std::string& text) {
  return std::fwrite(text.data(), 1, text.size(), file) == text.size();
}

// Writes `text` to `path` through a synced temporary file.
bool WriteDurably(const std::string& path, const std::string& text) {
  const auto temporary = path + ".tmp";
  FILE* file = std::fopen(temporary.c_str(), "w");
  if (!file) return false;
  bool ok = WriteAll(file, text) && std::fflush(file) == 0 && ::fsync(::fileno(file)) == 0;
  ok = std::fclose(file) == 0 && ok;
  ok = ok && std::rename(temporary.c_str(), path.c_str()) == 0;
  if (!ok) std::remove(temporary.c_str());
  return ok;
}

bool MakeFolder(const std::string& path) {
  if (::mkdir(path.c_str(), 0777) == 0) return true;
  struct stat info {};
  return errno == EEXIST && ::lstat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

// One value of the plan's line layout: the line without its end and trailing comma.
std::string Entry(std::string line) {
  while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
  if (!line.empty() && line.back() == ',') line.pop_back();
  return line;
}

std::optional<steam::json::Value> ParseLine(const std::string& text) {
  steam::json::Limits limits;
  limits.max_bytes = kMaxLineBytes;
  limits.max_depth = 2;
  limits.max_values = 16;
  limits.max_string_bytes = 8192;
  return steam::json::Parse(text, limits);
}

std::optional<std::uint64_t> NumberField(const steam::json::Value& object, const char* key) {
  const auto* value = object.Find(key);
  return value && value->is_number() ? value->AsUint64() : std::nullopt;
}

const std::string* TextField(const steam::json::Value& object, const char* key) {
  const auto* value = object.Find(key);
  return value && value->is_string() ? &value->text() : nullptr;
}

bool SafeRelative(const std::string& path) {
  if (path.empty() || path.size() > 4096 || path.front() == '/' || path.back() == '/') return false;
  if (path.find('\0') != std::string::npos) return false;
  std::size_t start = 0;
  for (;;) {
    const auto slash = path.find('/', start);
    const auto part = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
    if (part.empty() || part == "." || part == "..") return false;
    if (slash == std::string::npos) return true;
    start = slash + 1;
  }
}

}  // namespace

std::string CopyPlanPath(const std::string& root) { return root + "/" + kCopyJournalDir + "/" + kCopyPlanFile; }

bool PlanHasFile(const CopyPlan& plan, const std::string& path) {
  return std::any_of(plan.files.begin(), plan.files.end(), [&](const CopyFile& file) { return file.path == path; });
}

CopyPlanBuild BuildCopyPlan(const std::string& source_path, std::size_t max_files, std::uint64_t max_bytes) {
  const auto source = Trimmed(source_path);
  const int root = platform::OpenDirectoryWalk(source);
  if (root < 0) {
    const int error = errno;
    if (Unsupported(error)) return Failed("The game's folder can't be read on this console.", error);
    if (error == ENOENT) return Failed("The game's folder is not there. Check the drive.", error);
    if (error == ENOTDIR) return Failed("The game's folder is not a folder.", error);
    if (IsLinkError(error)) return Failed(LinkWords(source), error);
    return Failed("Playden can't read the game's folder.", error);
  }
  CopyPlanBuild build;
  build.plan.source = source;
  // One frame per level below the folder, on the heap: no depth limit and no recursion.
  std::vector<Frame> stack;
  Frame first;
  if (!OpenFrame(root, {}, &first)) return Failed("Playden can't read the game's folder.", errno);
  stack.push_back(std::move(first));
  const auto fail = [&](std::string failure, int error) {
    for (auto& frame : stack) Close(frame);
    return Failed(std::move(failure), error);
  };
  while (!stack.empty()) {
    Frame& frame = stack.back();
    errno = 0;
    const auto* entry = ::readdir(frame.listing);
    if (!entry) {
      if (errno != 0) {
        const int error = errno;
        return fail(UnreadableWords(frame.relative.empty() ? source : frame.relative), error);
      }
      Close(frame);
      stack.pop_back();
      continue;
    }
    const std::string name = entry->d_name;
    if (name == "." || name == "..") continue;
    if (frame.relative.empty() && ReservedName(name)) {
      build.skipped.push_back(name);
      continue;
    }
    // Half of a file the web page is writing, or was when a crash stopped it (web/file_policy.h).
    if (name.rfind(".playden-upload-", 0) == 0 || name.rfind(".playden-restore-", 0) == 0) continue;
    const auto relative = frame.relative.empty() ? name : frame.relative + "/" + name;
    if (entry->d_type == DT_LNK) return fail(LinkWords(relative), ELOOP);
    const int child = platform::OpenWalkChildUnprivileged(frame.fd, name.c_str(), kOpenAny, static_cast<std::uint64_t>(entry->d_fileno));
    if (child < 0) {
      const int error = errno;
      if (Unsupported(error)) return fail("The game's folder can't be read on this console.", error);
      // ELOOP/EMLINK: a link whose listing said DT_UNKNOWN (exFAT does).
      if (IsLinkError(error)) return fail(LinkWords(relative), error);
      return fail(UnreadableWords(relative), error);
    }
    struct stat info {};
    if (::fstat(child, &info) != 0) {
      const int error = errno;
      ::close(child);
      return fail(UnreadableWords(relative), error);
    }
    if (S_ISDIR(info.st_mode)) {
      if (build.plan.dirs.size() >= max_files)
        return fail("The game has more than " + WithThousands(max_files) + " folders. Playden can't copy it.", 0);
      build.plan.dirs.push_back(relative);
      Frame next;
      if (!OpenFrame(child, relative, &next)) return fail(UnreadableWords(relative), errno);
      stack.push_back(std::move(next));  // `frame` is not used after this
      continue;
    }
    ::close(child);
    if (S_ISLNK(info.st_mode)) return fail(LinkWords(relative), ELOOP);
    if (!S_ISREG(info.st_mode)) continue;
    if (build.plan.files.size() >= max_files)
      return fail("The game has more than " + WithThousands(max_files) + " files. Playden can't copy it.", 0);
    CopyFile file;
    file.path = relative;
    file.bytes = info.st_size > 0 ? static_cast<std::uint64_t>(info.st_size) : 0;
#if defined(__APPLE__)
    file.mtime = Seconds(static_cast<std::int64_t>(info.st_mtimespec.tv_sec));
    file.mtime_nsec = static_cast<std::int64_t>(info.st_mtimespec.tv_nsec);
#else
    file.mtime = Seconds(static_cast<std::int64_t>(info.st_mtim.tv_sec));
    file.mtime_nsec = static_cast<std::int64_t>(info.st_mtim.tv_nsec);
#endif
    build.plan.total += file.bytes;
    if (build.plan.total > max_bytes) {
      const auto tib = max_bytes >> 40;
      return fail(tib > 0 && (max_bytes & ((1ull << 40) - 1)) == 0
                      ? "The game is larger than " + std::to_string(tib) + " TiB. Playden can't copy it."
                      : "The game is larger than " + WithThousands(max_bytes) + " bytes. Playden can't copy it.",
                  0);
    }
    build.plan.files.push_back(std::move(file));
  }
  build.ok = true;
  return build;
}

std::string SaveCopyPlan(const CopyPlan& plan, const std::string& root) {
  const auto journal = root + "/" + kCopyJournalDir;
  if (!MakeFolder(root) || !MakeFolder(journal)) return "Could not write to the storage.";
  std::string text;
  text.reserve(256 + plan.files.size() * 96 + plan.dirs.size() * 48);
  text += "{\"version\":1,\"source\":\"" + Escape(plan.source) + "\",\"program\":\"" + Escape(plan.program) +
          "\",\"runtime_number\":" + std::to_string(plan.runtime_number) + ",\"total\":" + std::to_string(plan.total) +
          ",\"file_count\":" + std::to_string(plan.files.size()) + ",\"dir_count\":" + std::to_string(plan.dirs.size()) +
          ",\n\"dirs\":[\n";
  for (std::size_t index = 0; index < plan.dirs.size(); ++index)
    text += "\"" + Escape(plan.dirs[index]) + (index + 1 < plan.dirs.size() ? "\",\n" : "\"\n");
  text += "],\n\"files\":[\n";
  for (std::size_t index = 0; index < plan.files.size(); ++index) {
    const auto& file = plan.files[index];
    text += "{\"path\":\"" + Escape(file.path) + "\",\"bytes\":" + std::to_string(file.bytes) +
            ",\"mtime\":" + std::to_string(Seconds(file.mtime)) + ",\"mtime_nsec\":" + std::to_string(file.mtime_nsec) +
            (index + 1 < plan.files.size() ? "},\n" : "}\n");
  }
  text += "]}\n";
  if (!WriteDurably(CopyPlanPath(root), text)) return "Could not write to the storage.";
  return SaveFileList(plan, root);
}

std::string SaveFileList(const CopyPlan& plan, const std::string& root) {
  // The paths the copy may create, as the depot installer writes them before any game data.
  std::string list;
  list.reserve(plan.files.size() * 64 + plan.dirs.size() * 32);
  for (const auto& dir : plan.dirs) list += "d\t" + dir + "\n";
  for (const auto& file : plan.files) list += "f\t" + file.path + "\n";
  if (!WriteDurably(root + "/" + kFileList, list)) return "Could not write to the storage.";
  return {};
}

std::optional<CopyPlan> LoadCopyPlan(const std::string& root) {
  FILE* file = std::fopen(CopyPlanPath(root).c_str(), "r");
  if (!file) return std::nullopt;
  std::unique_ptr<char[]> buffer(new char[kMaxLineBytes]);
  bool bad = false;
  const auto next = [&](std::string* line) {
    if (bad || !std::fgets(buffer.get(), static_cast<int>(kMaxLineBytes), file)) return false;
    line->assign(buffer.get());
    if (line->empty() || line->back() != '\n') { bad = true; return false; }  // too long, or cut short
    return true;
  };
  CopyPlan plan;
  std::string line;
  std::optional<std::uint64_t> file_count, dir_count, total;
  if (next(&line)) {
    auto header = Entry(line);
    if (!header.empty()) header += "}";
    const auto value = ParseLine(header);
    const auto version = value && value->is_object() ? NumberField(*value, "version") : std::nullopt;
    const auto* source = value ? TextField(*value, "source") : nullptr;
    const auto* program = value ? TextField(*value, "program") : nullptr;
    const auto number = value ? NumberField(*value, "runtime_number") : std::nullopt;
    total = value ? NumberField(*value, "total") : std::nullopt;
    file_count = value ? NumberField(*value, "file_count") : std::nullopt;
    dir_count = value ? NumberField(*value, "dir_count") : std::nullopt;
    if (version != 1u || !source || !program || !number || *number > 0xffffffffu || !total || !file_count || !dir_count ||
        *file_count > kMaxCopyFiles || *dir_count > kMaxCopyFiles)
      bad = true;
    if (!bad) {
      plan.source = *source;
      plan.program = *program;
      plan.runtime_number = static_cast<std::uint32_t>(*number);
    }
  } else {
    bad = true;
  }
  if (!bad && !(next(&line) && line == "\"dirs\":[\n")) bad = true;
  for (std::uint64_t index = 0; !bad && dir_count && index < *dir_count; ++index) {
    if (!next(&line)) { bad = true; break; }
    const auto value = ParseLine(Entry(line));
    if (!value || !value->is_string() || !SafeRelative(value->text())) { bad = true; break; }
    plan.dirs.push_back(value->text());
  }
  if (!bad && !(next(&line) && line == "],\n")) bad = true;
  if (!bad && !(next(&line) && line == "\"files\":[\n")) bad = true;
  std::uint64_t sum = 0;
  for (std::uint64_t index = 0; !bad && file_count && index < *file_count; ++index) {
    if (!next(&line)) { bad = true; break; }
    const auto value = ParseLine(Entry(line));
    const auto* path = value && value->is_object() ? TextField(*value, "path") : nullptr;
    const auto bytes = path ? NumberField(*value, "bytes") : std::nullopt;
    const auto mtime = path ? NumberField(*value, "mtime") : std::nullopt;
    const auto nsec = path ? NumberField(*value, "mtime_nsec") : std::nullopt;
    if (!path || !SafeRelative(*path) || !bytes || !mtime || !nsec || *mtime > static_cast<std::uint64_t>(INT64_MAX) ||
        *nsec >= 1000000000u) {
      bad = true;
      break;
    }
    sum += *bytes;
    plan.files.push_back({*path, *bytes, static_cast<std::int64_t>(*mtime), static_cast<std::int64_t>(*nsec)});
  }
  if (!bad && !(next(&line) && line == "]}\n")) bad = true;
  if (!bad && std::fgets(buffer.get(), static_cast<int>(kMaxLineBytes), file)) bad = true;  // nothing after the end
  std::fclose(file);
  if (bad || !total || sum != *total) return std::nullopt;
  plan.total = sum;
  return plan;
}

}  // namespace playden::stores::local
