// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime/workshop_mods.h"

#include "runtime/artifact_hash.h"

#include <algorithm>
#include <filesystem>
#include <cerrno>
#include <cstdio>
#include <map>
#include <optional>
#include <utility>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace playden::runtime {
namespace {

using platform::DirectoryPolicy;
using platform::ManagedDirectory;

constexpr std::size_t kMarkerMaxBytes = 4096;
constexpr std::size_t kMaxDigits = 20;  // UINT64_MAX
constexpr std::uint64_t kInt32Max = 0x7fffffffu;

bool IsFolder(const std::string& path) {
  struct stat info {};
  return ::lstat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

// Anything at all under name in folder: a link, or another kind of entry InspectChild doesn't admit, counts.
bool Exists(const ManagedDirectory& folder, const std::string& name) {
  struct stat info {};
  return folder.InspectChild(name, &info) || errno != ENOENT;
}

// Whole regular file name in folder, not through a link, at most limit bytes.
std::optional<std::string> ReadFile(const ManagedDirectory& folder, const std::string& name, std::size_t limit) {
  const int file = folder.OpenRegularFile(name);
  if (file < 0) return std::nullopt;
  struct stat info {};
  std::optional<std::string> out;
  if (::fstat(file, &info) == 0 && S_ISREG(info.st_mode) && static_cast<std::size_t>(info.st_size) <= limit) {
    std::string bytes(static_cast<std::size_t>(info.st_size), '\0');
    std::size_t done = 0;
    while (done < bytes.size()) {
      const auto count = ::read(file, bytes.data() + done, bytes.size() - done);
      if (count < 0 && (errno == EINTR || errno == EAGAIN)) continue;
      if (count <= 0) break;
      done += static_cast<std::size_t>(count);
    }
    if (done == bytes.size()) out = std::move(bytes);
  }
  ::close(file);
  return out;
}

// Writes bytes as name in folder through a temporary file beside it, synced, then renamed over it; each step
// checked by ManagedDirectory, so never through a link (a link at name fails it).
bool WriteAtomic(const ManagedDirectory& folder, const std::string& name, std::string_view bytes) {
  const auto temporary = ".playden-tmp-" + name;
  if (!folder.RemoveRegularFile(temporary) && errno != ENOENT) return false;
  const int file = folder.CreateExclusiveFile(temporary, 0644);  // whatever the umask: the game's process reads it
  if (file < 0) return false;
  std::size_t done = 0;
  bool ok = true;
  while (ok && done < bytes.size()) {
    const auto count = ::write(file, bytes.data() + done, bytes.size() - done);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) ok = false;
    else done += static_cast<std::size_t>(count);
  }
  ok = ::fsync(file) == 0 && ok;
  ok = ::close(file) == 0 && ok;
  if (!ok || !folder.RenameChild(temporary, folder, name, true)) {
    folder.RemoveRegularFile(temporary);
    return false;
  }
  return folder.Sync();
}

bool Remove(const ManagedDirectory& folder, const std::string& name) {
  return folder.RemoveRegularFile(name) || errno == ENOENT;
}

// The folder <root>/<relative>, opened from root (which the caller trusts) one real folder at a time. None when any
// is missing (missing set) or is a link, not a folder, or refused by the policy (missing clear).
std::optional<ManagedDirectory> OpenSettings(const std::string& root, DirectoryPolicy policy, const std::string& relative,
                                             bool* missing) {
  *missing = false;
  auto folder = ManagedDirectory::Open(root, policy);
  if (!folder) {
    *missing = errno == ENOENT;
    return std::nullopt;
  }
  for (const auto& part : std::filesystem::path(relative)) {
    const auto name = part.string();
    if (name.empty() || name == ".") continue;
    struct stat info {};
    if (!folder->InspectChild(name, &info)) {
      *missing = errno == ENOENT;
      return std::nullopt;
    }
    auto next = folder->OpenChild(name, policy);
    if (!next) return std::nullopt;
    folder = std::move(next);
  }
  return folder;
}

// The all-digit number in name; false when it isn't one or is past 64 bits.
bool Number(const std::string& name, std::uint64_t* value) {
  if (name.empty() || name.size() > kMaxDigits) return false;
  std::uint64_t out = 0;
  for (const char c : name) {
    if (c < '0' || c > '9') return false;
    const auto digit = static_cast<std::uint64_t>(c - '0');
    if (out > (UINT64_MAX - digit) / 10) return false;
    out = out * 10 + digit;
  }
  *value = out;
  return true;
}

// A file name with no folder in it.
bool PlainName(const std::string& name) {
  return !name.empty() && name != "." && name != ".." && name.find_first_of("/\\:") == std::string::npos &&
         name.find('\0') == std::string::npos;
}

std::vector<std::string> MarkerHashes(const ManagedDirectory& settings) {
  std::vector<std::string> hashes;
  const auto text = ReadFile(settings, kWorkshopModsMarker, kMarkerMaxBytes);
  if (!text) return hashes;
  std::size_t start = 0;
  while (start < text->size()) {
    auto end = text->find('\n', start);
    if (end == std::string::npos) end = text->size();
    const auto line = text->substr(start, end - start);
    if (line.size() == 64 && line.find_first_not_of("0123456789abcdef") == std::string::npos) hashes.push_back(line);
    start = end + 1;
  }
  return hashes;
}

bool WriteMarker(const ManagedDirectory& settings, const std::vector<std::string>& hashes) {
  std::string text;
  for (const auto& hash : hashes) text += hash + "\n";
  return WriteAtomic(settings, kWorkshopModsMarker, text);
}

}  // namespace

std::string WorkshopJsonString(std::string_view text) {
  std::string out = "\"";
  const auto byte = [&](std::size_t at) { return static_cast<unsigned char>(text[at]); };
  const auto continuation = [&](std::size_t at, unsigned char low = 0x80, unsigned char high = 0xbf) {
    return at < text.size() && byte(at) >= low && byte(at) <= high;
  };
  std::size_t at = 0;
  while (at < text.size()) {
    const unsigned char c = byte(at);
    if (c < 0x80) {
      if (c == '"') out += "\\\"";
      else if (c == '\\') out += "\\\\";
      else if (c == '\n') out += "\\n";
      else if (c == '\r') out += "\\r";
      else if (c == '\t') out += "\\t";
      else if (c < 0x20) {
        char escaped[8];
        std::snprintf(escaped, sizeof escaped, "\\u%04x", static_cast<unsigned>(c));
        out += escaped;
      } else {
        out += static_cast<char>(c);
      }
      ++at;
      continue;
    }
    // The well-formed sequences of the Unicode standard's table 3-7.
    std::size_t length = 0;
    if (c >= 0xc2 && c <= 0xdf) length = continuation(at + 1) ? 2 : 0;
    else if (c == 0xe0) length = continuation(at + 1, 0xa0) && continuation(at + 2) ? 3 : 0;
    else if ((c >= 0xe1 && c <= 0xec) || c == 0xee || c == 0xef) length = continuation(at + 1) && continuation(at + 2) ? 3 : 0;
    else if (c == 0xed) length = continuation(at + 1, 0x80, 0x9f) && continuation(at + 2) ? 3 : 0;
    else if (c == 0xf0) length = continuation(at + 1, 0x90) && continuation(at + 2) && continuation(at + 3) ? 4 : 0;
    else if (c >= 0xf1 && c <= 0xf3)
      length = continuation(at + 1) && continuation(at + 2) && continuation(at + 3) ? 4 : 0;
    else if (c == 0xf4) length = continuation(at + 1, 0x80, 0x8f) && continuation(at + 2) && continuation(at + 3) ? 4 : 0;
    if (length == 0) {
      out += "\xef\xbf\xbd";
      ++at;
    } else {
      out.append(text.substr(at, length));
      at += length;
    }
  }
  return out + "\"";
}

std::string BuildWorkshopModsJson(std::uint32_t app_id, const std::vector<WorkshopMod>& items,
                                  const std::vector<std::string>& hand_placed) {
  // id -> the hand-placed folder name, or the item.
  std::map<std::uint64_t, std::pair<std::string, const WorkshopMod*>> entries;
  for (const auto& name : hand_placed) {
    std::uint64_t id = 0;
    if (Number(name, &id)) entries.emplace(id, std::make_pair(name, nullptr));
  }
  for (const auto& item : items)
    if (item.id != 0) entries.emplace(item.id, std::make_pair(std::to_string(item.id), &item));
  std::string out = "{";
  bool first = true;
  for (const auto& [id, entry] : entries) {
    out += first ? "\n" : ",\n";
    first = false;
    out += "  " + WorkshopJsonString(entry.first) + ": {";
    const WorkshopMod* item = entry.second;
    if (!item) {
      // No "path": gbe_fork reads steam_settings/mods/<key>, the folder as it found it before this file.
      out += "}";
      continue;
    }
    const auto key = std::to_string(id);
    std::vector<std::pair<const char*, std::string>> fields;
    if (!item->title.empty()) fields.emplace_back("title", WorkshopJsonString(item->title));
    fields.emplace_back("path", WorkshopJsonString(std::string(kWorkshopContentWindowsFolder) + "\\" +
                                                   std::to_string(app_id) + "\\" + key));
    const bool primary = !item->primary_filename.empty() && PlainName(item->primary_filename);
    if (primary) fields.emplace_back("primary_filename", WorkshopJsonString(item->primary_filename));
    fields.emplace_back("primary_filesize", std::to_string(std::min(primary ? item->primary_bytes : item->bytes, kInt32Max)));
    if (!item->preview_filename.empty() && PlainName(item->preview_filename)) {
      const auto folder = std::string(kWorkshopContentWindowsFolder) + "\\" + std::to_string(app_id) + "\\" + key;
      std::string url = "file:///" + folder + "/" + item->preview_filename;
      for (auto& c : url) if (c == '\\') c = '/';
      fields.emplace_back("preview_filename", WorkshopJsonString(folder + "\\" + item->preview_filename));
      fields.emplace_back("preview_filesize", std::to_string(std::min(item->preview_bytes, kInt32Max)));
      fields.emplace_back("preview_url", WorkshopJsonString(url));
    }
    if (item->owner != 0) fields.emplace_back("steam_id_owner", std::to_string(item->owner));
    fields.emplace_back("total_files_sizes", std::to_string(item->bytes));
    fields.emplace_back("time_updated", std::to_string(item->time_updated));
    fields.emplace_back("workshop_item_url",
                        WorkshopJsonString("https://steamcommunity.com/sharedfiles/filedetails/?id=" + key));
    for (std::size_t i = 0; i < fields.size(); ++i)
      out += std::string(i ? ",\n" : "\n") + "    \"" + fields[i].first + "\": " + fields[i].second;
    out += "\n  }";
  }
  return out + (first ? "}\n" : "\n}\n");
}

std::vector<WorkshopMod> PresentWorkshopMods(const std::string& workshop_folder, std::uint32_t app_id,
                                             std::vector<WorkshopMod> items) {
  std::vector<WorkshopMod> present;
  const auto content = workshop_folder + "/content";
  const auto app = content + "/" + std::to_string(app_id);
  if (!IsFolder(content) || !IsFolder(app)) return present;
  for (auto& item : items) {
    const auto folder = app + "/" + std::to_string(item.id);
    if (item.id == 0 || !IsFolder(folder)) continue;
    if (!item.primary_filename.empty()) {
      struct stat info {};
      if (PlainName(item.primary_filename) && ::lstat((folder + "/" + item.primary_filename).c_str(), &info) == 0 &&
          S_ISREG(info.st_mode)) {
        item.primary_bytes = static_cast<std::uint64_t>(info.st_size);
      } else {
        item.primary_filename.clear();
        item.primary_bytes = 0;
      }
    }
    if (!item.preview_filename.empty()) {
      struct stat info {};
      if (PlainName(item.preview_filename) && ::lstat((folder + "/" + item.preview_filename).c_str(), &info) == 0 &&
          S_ISREG(info.st_mode)) {
        item.preview_bytes = static_cast<std::uint64_t>(info.st_size);
      } else {
        item.preview_filename.clear();
        item.preview_bytes = 0;
      }
    }
    present.push_back(std::move(item));
  }
  return present;
}

std::vector<std::string> HandPlacedMods(const ManagedDirectory& settings) {
  std::vector<std::pair<std::uint64_t, std::string>> found;
  const auto mods = settings.OpenChild("mods", settings.policy());
  if (!mods) return {};
  DIR* directory = ::opendir(mods->path().c_str());
  if (!directory) return {};
  std::vector<std::string> listed;
  while (const dirent* entry = ::readdir(directory)) listed.emplace_back(entry->d_name);
  ::closedir(directory);
  for (const auto& name : listed) {
    std::uint64_t id = 0;
    struct stat info {};
    if (Number(name, &id) && mods->InspectChild(name, &info) && S_ISDIR(info.st_mode)) found.emplace_back(id, name);
  }
  std::sort(found.begin(), found.end());
  std::vector<std::string> names;
  for (auto& [id, name] : found) names.push_back(std::move(name));
  return names;
}

const char* WorkshopModsOutcomeName(WorkshopModsOutcome outcome) {
  switch (outcome) {
    case WorkshopModsOutcome::kNone: return "none";
    case WorkshopModsOutcome::kWritten: return "written";
    case WorkshopModsOutcome::kKept: return "kept";
    case WorkshopModsOutcome::kRemoved: return "removed";
    case WorkshopModsOutcome::kHandMade: return "hand-made";
    case WorkshopModsOutcome::kFailed: return "failed";
  }
  return "failed";
}

namespace {
WorkshopModsResult Apply(const ManagedDirectory& settings, std::uint32_t app_id, const std::vector<WorkshopMod>& items) {
  WorkshopModsResult result;
  const bool exists = Exists(settings, kWorkshopModsFile);
  const auto current = exists ? ReadFile(settings, kWorkshopModsFile, kWorkshopModsMaxBytes) : std::nullopt;
  const auto current_hash = current ? HashArtifactBytes(*current) : std::nullopt;
  auto hashes = MarkerHashes(settings);
  const bool ours = current_hash && std::find(hashes.begin(), hashes.end(), *current_hash) != hashes.end();
  if (exists && !ours) {
    // Someone else's (or something Playden can't read as a file): gbe_fork reads it as it is.
    result.outcome = WorkshopModsOutcome::kHandMade;
    return result;
  }
  if (items.empty()) {
    // Hand-placed folders need no file: gbe_fork finds them again once Playden's is gone.
    if (exists && !Remove(settings, kWorkshopModsFile)) {
      result.outcome = WorkshopModsOutcome::kFailed;
      return result;
    }
    if (Exists(settings, kWorkshopModsMarker) && !Remove(settings, kWorkshopModsMarker)) {
      result.outcome = WorkshopModsOutcome::kFailed;
      return result;
    }
    result.outcome = exists ? WorkshopModsOutcome::kRemoved : WorkshopModsOutcome::kNone;
    return result;
  }
  const auto hand_placed = HandPlacedMods(settings);
  const auto text = BuildWorkshopModsJson(app_id, items, hand_placed);
  const auto hash = HashArtifactBytes(text);
  if (!hash) {
    result.outcome = WorkshopModsOutcome::kFailed;
    return result;
  }
  // Hand-placed folders listed in the file: one per ID.
  {
    std::vector<std::uint64_t> ids;
    for (const auto& name : hand_placed) {
      std::uint64_t id = 0;
      if (Number(name, &id) && std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
    }
    result.hand_placed = ids.size();
  }
  if (current && *current == text) {
    if (hashes != std::vector<std::string>{*hash} && !WriteMarker(settings, {*hash})) {
      result.outcome = WorkshopModsOutcome::kFailed;
      return result;
    }
    result.outcome = WorkshopModsOutcome::kKept;
    return result;
  }
  // The marker names both files while the new one takes the old one's place: a stop in between leaves a file that is
  // still known as Playden's.
  std::vector<std::string> during;
  if (current_hash) during.push_back(*current_hash);
  during.push_back(*hash);
  if (!WriteMarker(settings, during) || !WriteAtomic(settings, kWorkshopModsFile, text) ||
      !WriteMarker(settings, {*hash})) {
    result.outcome = WorkshopModsOutcome::kFailed;
    return result;
  }
  result.outcome = WorkshopModsOutcome::kWritten;
  return result;
}

}  // namespace

WorkshopModsResult ApplyWorkshopMods(const std::string& root, DirectoryPolicy policy, const std::string& relative,
                                     std::uint32_t app_id, const std::vector<WorkshopMod>& items) {
  bool missing = false;
  const auto settings = OpenSettings(root, policy, relative, &missing);
  if (!settings) return {WorkshopModsOutcome::kFailed, 0};
  return Apply(*settings, app_id, items);
}

WorkshopModsResult RemoveWorkshopMods(const std::string& root, DirectoryPolicy policy, const std::string& relative) {
  bool missing = false;
  const auto settings = OpenSettings(root, policy, relative, &missing);
  if (!settings) return {missing ? WorkshopModsOutcome::kNone : WorkshopModsOutcome::kFailed, 0};
  return Apply(*settings, 0, {});
}

}  // namespace playden::runtime
