// SPDX-License-Identifier: GPL-3.0-or-later
#include "steam/backend/workshop_items.h"

#include "platform/managed_directory.h"
#include "runtime/workshop_acf.h"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <filesystem>
#include <map>
#include <memory>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

namespace playden::steam {
namespace {

namespace fs = std::filesystem;
using platform::DirectoryPolicy;
using platform::ManagedDirectory;

constexpr DirectoryPolicy kTree = DirectoryPolicy::kApprovedTree;
constexpr mode_t kFolderMode = 0755;  // as Wine makes the bottle's folders
constexpr mode_t kFileMode = 0644;
constexpr std::size_t kListMaxBytes = 32u << 20;  // a staged item's .playden-tree or .playden-files
constexpr int kMaxTreeDepth = 64;
constexpr const char* kItemPrefix = "item=";
constexpr const char* kReservedPrefix = ".playden-";

template <typename Number>
bool ParseNumber(std::string_view text, Number* value) {
  if (text.empty() || (text[0] == '0' && text.size() > 1)) return false;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), *value);
  return parsed.ec == std::errc() && parsed.ptr == text.data() + text.size();
}

std::string Lower(std::string text) {
  for (auto& c : text) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
  return text;
}

bool Reserved(const std::string& name) { return Lower(name).compare(0, 9, kReservedPrefix) == 0; }

// Anything at all under `name` (a link or other entry InspectChild doesn't admit counts).
bool Exists(const ManagedDirectory& folder, const std::string& name) {
  struct stat info{};
  if (folder.InspectChild(name, &info)) return true;
  return errno != ENOENT;
}

bool ListNames(const fs::path& path, std::vector<std::string>* names) {
  DIR* directory = ::opendir(path.c_str());
  if (!directory) return false;
  while (const dirent* entry = ::readdir(directory)) {
    const std::string name = entry->d_name;
    if (name != "." && name != "..") names->push_back(name);
  }
  ::closedir(directory);
  return true;
}

// Deletes `name` in `parent` and everything in it, never following a link (a link anywhere fails it). A
// missing entry is success. The listing is read again until it is empty: a folder's readdir may skip entries
// while they are deleted.
bool RemoveManagedTree(const ManagedDirectory& parent, const std::string& name, int depth = 0) {
  struct stat info{};
  if (!parent.InspectChild(name, &info)) return errno == ENOENT;
  if (!S_ISDIR(info.st_mode)) return parent.RemoveRegularFile(name);
  if (depth > kMaxTreeDepth) return false;
  const auto child = parent.OpenChild(name, kTree);
  if (!child) return false;
  for (int pass = 0; pass < 4; ++pass) {
    std::vector<std::string> names;
    if (!ListNames(child->path(), &names)) return false;
    if (names.empty()) break;
    for (const auto& entry : names)
      if (!RemoveManagedTree(*child, entry, depth + 1)) return false;
  }
  return parent.RemoveEmptyDirectory(name);
}

bool RemoveFile(const ManagedDirectory& folder, const std::string& name) {
  return folder.RemoveRegularFile(name) || errno == ENOENT;
}

// The file, when it is at most `cap` bytes; `missing` tells a missing file from an unreadable one, `too_big` one
// over the cap.
std::optional<std::string> ReadSmallFile(const ManagedDirectory& folder, const std::string& name, std::size_t cap,
                                         bool* missing = nullptr, bool* too_big = nullptr) {
  if (missing) *missing = false;
  if (too_big) *too_big = false;
  const int fd = folder.OpenRegularFile(name);
  if (fd < 0) {
    if (missing) *missing = errno == ENOENT;
    return std::nullopt;
  }
  std::string text;
  char buffer[8192];
  bool ok = true;
  for (;;) {
    const auto got = ::read(fd, buffer, sizeof(buffer));
    if (got < 0 && errno == EINTR) continue;
    if (got < 0) { ok = false; break; }
    if (got == 0) break;
    if (text.size() + static_cast<std::size_t>(got) > cap) {
      ok = false;
      if (too_big) *too_big = true;
      break;
    }
    text.append(buffer, static_cast<std::size_t>(got));
  }
  ::close(fd);
  if (!ok) return std::nullopt;
  return text;
}

bool WriteFileAtomic(const ManagedDirectory& folder, const std::string& name, const std::string& text) {
  const std::string temporary = name + ".tmp";
  if (!RemoveFile(folder, temporary)) return false;
  const int fd = folder.CreateExclusiveFile(temporary, kFileMode);
  if (fd < 0) return false;
  bool ok = true;
  for (std::size_t written = 0; ok && written < text.size();) {
    const auto put = ::write(fd, text.data() + written, text.size() - written);
    if (put < 0 && errno == EINTR) continue;
    if (put <= 0) ok = false;
    else written += static_cast<std::size_t>(put);
  }
  ok = ::fsync(fd) == 0 && ok;
  ok = ::close(fd) == 0 && ok;
  if (!ok || !folder.RenameChild(temporary, folder, name, true)) {
    folder.RemoveRegularFile(temporary);
    return false;
  }
  return folder.Sync();
}

// The first path component of each entry a finished download lists: its tree (each file, written after every
// download) and its file list (also folders).
enum class Staged { kOk, kReserved, kUnreadable };
Staged StagedNames(const ManagedDirectory& staging, std::set<std::string>* names, std::string* why) {
  const auto tree = ReadSmallFile(staging, DepotInstaller::kFileTree, kListMaxBytes);
  if (!tree) { *why = "download has no file tree"; return Staged::kUnreadable; }
  bool missing = false;
  const auto list = ReadSmallFile(staging, DepotInstaller::kFileList, kListMaxBytes, &missing);
  if (!list && !missing) { *why = "cannot read file list"; return Staged::kUnreadable; }
  const auto add = [&](std::string_view path) {
    const auto name = std::string(path.substr(0, path.find('/')));
    if (name.empty() || name == "." || name == "..") return true;
    if (Reserved(name)) { *why = "item has a .playden- name"; return false; }
    names->insert(name);
    return true;
  };
  const auto lines = [](const std::string& text, const std::function<bool(std::string_view)>& each) {
    std::size_t start = 0;
    while (start < text.size()) {
      auto end = text.find('\n', start);
      if (end == std::string::npos) end = text.size();
      if (!each(std::string_view(text).substr(start, end - start))) return false;
      start = end + 1;
    }
    return true;
  };
  bool first = true;
  if (!lines(*tree, [&](std::string_view line) {
        if (first) { first = false; return true; }  // "v1"
        const auto a = line.find('\t');
        const auto b = a == std::string_view::npos ? a : line.find('\t', a + 1);
        return b == std::string_view::npos || add(line.substr(b + 1));
      }))
    return Staged::kReserved;
  if (list && !lines(*list, [&](std::string_view line) {
        return line.size() < 3 || line[1] != '\t' || add(line.substr(2));
      }))
    return Staged::kReserved;
  return Staged::kOk;
}

WorkshopDownloadResult RunItem(DepotInstaller& installer, const StoredSession& session, std::uint32_t app_id,
                               std::uint32_t depot, const WorkshopItem& item, const std::string& staging_root,
                               const std::atomic<bool>& cancel, const WorkshopProgressFn& progress,
                               const std::string* refused) {
  if (!item.downloadable()) return {InstallError::kProtocol, false, "item has no manifest"};
  InstallTarget target;
  target.app_id = app_id;
  target.install_root = staging_root;
  DepotInfo info;
  info.id = depot;
  info.manifest_gid = item.manifest;
  info.source_app = app_id;
  info.owner_app = app_id;
  target.depots = {info};
  const auto outcome = installer.Run(session, target, cancel, progress);
  if (refused && !refused->empty()) return {InstallError::kCorrupt, true, *refused};
  if (outcome.error != InstallError::kNone) return {outcome.error, false, outcome.detail};
  if (!outcome.skipped_depots.empty()) return {InstallError::kRejected, false, "workshop depot refused"};
  return {};
}

// What a failure detail says, for the log, without an ID, a path or a name from an item's content: up to three
// leading words of letters only (such as "manifest unavailable"), then "http=<3 digits>" or "transport=<letters>"
// if the detail names one.
std::string WorkshopLogDetail(std::string_view detail) {
  const auto letters = [](std::string_view word, bool digits) {
    return !word.empty() && word.size() <= 24 && std::all_of(word.begin(), word.end(), [digits](char c) {
      return (c >= 'a' && c <= 'z') || c == '-' || c == '_' || (digits && c >= '0' && c <= '9');
    });
  };
  std::vector<std::string_view> words;
  for (std::size_t start = 0; start < detail.size();) {
    auto end = detail.find(' ', start);
    if (end == std::string_view::npos) end = detail.size();
    if (end > start) words.push_back(detail.substr(start, end - start));
    start = end + 1;
  }
  std::string out;
  std::size_t lead = 0;
  while (lead < words.size() && lead < 3 && letters(words[lead], false)) {
    if (!out.empty()) out += '_';
    out += words[lead++];
  }
  for (const auto word : words) {
    const auto http = word.substr(0, 5) == "http=" && word.size() == 8 && std::all_of(word.begin() + 5, word.end(),
                                                                                       [](char c) { return c >= '0' && c <= '9'; });
    const auto transport = word.substr(0, 10) == "transport=" && letters(word.substr(10), false);
    if (!http && !transport) continue;
    if (!out.empty()) out += '_';
    out += word;
    break;
  }
  return out.empty() ? "none" : out;
}

}  // namespace

std::string WorkshopFolder(const std::string& prefix) {
  return prefix + "/drive_c/Program Files (x86)/Steam/steamapps/workshop";
}

bool EnsureWorkshopFolder(const std::string& prefix) {
  auto folder = ManagedDirectory::Open(fs::path(prefix) / "drive_c", kTree);
  for (const char* name : {"Program Files (x86)", "Steam", "steamapps", "workshop"}) {
    if (!folder) return false;
    folder = folder->OpenChild(name, kTree, true, kFolderMode);
  }
  return folder.has_value();
}

const WorkshopLocalItem* WorkshopState::Find(std::uint64_t id) const {
  for (const auto& item : items)
    if (item.id == id) return &item;
  return nullptr;
}
WorkshopLocalItem* WorkshopState::Find(std::uint64_t id) {
  for (auto& item : items)
    if (item.id == id) return &item;
  return nullptr;
}

std::string WorkshopStateName(std::uint32_t app_id) { return ".playden-workshop-" + std::to_string(app_id); }

std::optional<WorkshopState> ParseWorkshopState(std::string_view text) {
  if (text.size() > kWorkshopStateMaxBytes) return std::nullopt;
  if (!text.empty() && text.back() != '\n') return std::nullopt;
  WorkshopState state;
  std::set<std::uint64_t> ids;
  std::size_t start = 0;
  while (start < text.size()) {
    const auto end = text.find('\n', start);
    const auto line = text.substr(start, end - start);
    start = end + 1;
    if (line.compare(0, 5, kItemPrefix) != 0) return std::nullopt;
    std::string_view rest = line.substr(5);
    std::string_view fields[5];
    for (auto& field : fields) {
      const auto comma = rest.find(',');
      if (comma == std::string_view::npos) return std::nullopt;
      field = rest.substr(0, comma);
      rest = rest.substr(comma + 1);
    }
    WorkshopLocalItem item;
    if (!ParseNumber(fields[0], &item.id) || item.id == 0 || !ParseNumber(fields[1], &item.manifest) ||
        !ParseNumber(fields[2], &item.time_updated) || !ParseNumber(fields[3], &item.bytes))
      return std::nullopt;
    if (fields[4] == "on") item.on = true;
    else if (fields[4] == "off") item.on = false;
    else return std::nullopt;
    item.title = std::string(rest);
    if (SanitiseWorkshopText(item.title, kWorkshopTitleBytes) != item.title) return std::nullopt;
    if (!ids.insert(item.id).second || state.items.size() >= kWorkshopMaxItems) return std::nullopt;
    state.items.push_back(std::move(item));
  }
  return state;
}

std::string FormatWorkshopState(const WorkshopState& state) {
  std::string text;
  for (const auto& item : state.items)
    text += kItemPrefix + std::to_string(item.id) + "," + std::to_string(item.manifest) + "," +
            std::to_string(item.time_updated) + "," + std::to_string(item.bytes) + "," + (item.on ? "on" : "off") +
            "," + SanitiseWorkshopText(item.title, kWorkshopTitleBytes) + "\n";
  return text;
}

std::optional<WorkshopState> ReadWorkshopState(const std::string& workshop_folder, std::uint32_t app_id) {
  const auto folder = ManagedDirectory::Open(workshop_folder, kTree);
  if (!folder) return std::nullopt;
  bool missing = false;
  const auto text = ReadSmallFile(*folder, WorkshopStateName(app_id), kWorkshopStateMaxBytes, &missing);
  if (!text) return missing ? std::optional<WorkshopState>(WorkshopState{}) : std::nullopt;
  return ParseWorkshopState(*text);
}

bool WriteWorkshopState(const std::string& workshop_folder, std::uint32_t app_id, const WorkshopState& state) {
  const auto folder = ManagedDirectory::Open(workshop_folder, kTree);
  return folder && WriteFileAtomic(*folder, WorkshopStateName(app_id), FormatWorkshopState(state));
}

std::string WorkshopOffName(std::uint32_t app_id) { return WorkshopStateName(app_id) + "-off"; }

std::optional<std::set<std::uint64_t>> ParseWorkshopOff(std::string_view text) {
  if (!text.empty() && text.back() != '\n') return std::nullopt;
  std::set<std::uint64_t> off;
  std::size_t start = 0;
  while (start < text.size()) {
    const auto end = text.find('\n', start);
    std::uint64_t id = 0;
    if (!ParseNumber(text.substr(start, end - start), &id) || id == 0) return std::nullopt;
    if (!off.insert(id).second || off.size() > kWorkshopMaxItems) return std::nullopt;
    start = end + 1;
  }
  return off;
}

std::string FormatWorkshopOff(const std::set<std::uint64_t>& off) {
  std::string text;
  for (const auto id : off) text += std::to_string(id) + "\n";
  return text;
}

namespace {

// At most kWorkshopMaxItems IDs of up to 20 digits, each with its line end.
constexpr std::size_t kWorkshopOffMaxBytes = kWorkshopMaxItems * 21;

enum class SmallFile { kOk, kMissing, kDamaged, kUnreadable };
// Reads W/<name>: kDamaged when it reads but `valid` refuses it, or it is over `cap`.
SmallFile ReadChecked(const ManagedDirectory& folder, const std::string& name, std::size_t cap,
                      const std::function<bool(const std::string&)>& valid, std::string* text) {
  bool missing = false, too_big = false;
  auto read = ReadSmallFile(folder, name, cap, &missing, &too_big);
  if (missing) return SmallFile::kMissing;
  if (too_big) return SmallFile::kDamaged;
  if (!read) return SmallFile::kUnreadable;
  if (!valid(*read)) return SmallFile::kDamaged;
  *text = std::move(*read);
  return SmallFile::kOk;
}

// Renames a damaged W/<name> to "<name>.bad" (replacing an older one), so what reads it next starts afresh.
bool SetAside(const ManagedDirectory& folder, const std::string& name) {
  return folder.RenameChild(name, folder, name + ".bad", true) && folder.Sync();
}

}  // namespace

std::optional<std::set<std::uint64_t>> ReadWorkshopOff(const std::string& workshop_folder, std::uint32_t app_id) {
  const auto folder = ManagedDirectory::Open(workshop_folder, kTree);
  if (!folder) return std::nullopt;
  bool missing = false;
  const auto text = ReadSmallFile(*folder, WorkshopOffName(app_id), kWorkshopOffMaxBytes, &missing);
  if (!text) return missing ? std::optional<std::set<std::uint64_t>>(std::set<std::uint64_t>{}) : std::nullopt;
  return ParseWorkshopOff(*text);
}

bool WriteWorkshopOff(const std::string& workshop_folder, std::uint32_t app_id, const std::set<std::uint64_t>& off) {
  if (off.size() > kWorkshopMaxItems || off.count(0)) return false;
  const auto folder = ManagedDirectory::Open(workshop_folder, kTree);
  return folder && WriteFileAtomic(*folder, WorkshopOffName(app_id), FormatWorkshopOff(off));
}

const char* WorkshopActionName(WorkshopAction action) {
  switch (action) {
    case WorkshopAction::kDownload: return "download";
    case WorkshopAction::kUpdate: return "update";
    case WorkshopAction::kKeep: return "keep";
    case WorkshopAction::kRemove: return "remove";
    case WorkshopAction::kSkipLegacy: return "skip-legacy";
    case WorkshopAction::kSkipHandPlaced: return "skip-hand-placed";
    case WorkshopAction::kSkipOff: return "skip-off";
  }
  return "?";
}

const char* WorkshopOutcomeName(WorkshopOutcome outcome) {
  switch (outcome) {
    case WorkshopOutcome::kDownloaded: return "downloaded";
    case WorkshopOutcome::kUpdated: return "updated";
    case WorkshopOutcome::kKept: return "kept";
    case WorkshopOutcome::kRemoved: return "removed";
    case WorkshopOutcome::kSkippedLegacy: return "skipped-legacy";
    case WorkshopOutcome::kSkippedHandPlaced: return "skipped-hand-placed";
    case WorkshopOutcome::kSkippedOff: return "skipped-off";
    case WorkshopOutcome::kRefused: return "refused";
    case WorkshopOutcome::kFailed: return "failed";
  }
  return "?";
}

WorkshopPlan PlanWorkshopSync(const std::vector<WorkshopItem>& subscribed, const WorkshopState& state,
                              const std::set<std::uint64_t>& off, const std::set<std::uint64_t>& present,
                              bool complete) {
  WorkshopPlan plan;
  std::set<std::uint64_t> seen;
  const auto add = [&plan](WorkshopAction action, const WorkshopItem& item, std::uint64_t bytes) {
    plan.entries.push_back({action, item, bytes});
    if (action == WorkshopAction::kDownload || action == WorkshopAction::kUpdate) {
      plan.download_bytes += bytes;
      ++plan.downloads;
    } else if (action == WorkshopAction::kRemove) {
      plan.freed_bytes += bytes;
      ++plan.removals;
    }
  };
  for (const auto& item : subscribed) {
    if (item.id == 0 || !seen.insert(item.id).second) continue;
    const auto* local = state.Find(item.id);
    // A legacy item listed with no manifest is a download that never finished (a Playden that took the item's UGC
    // handle for a manifest, #1319). It holds nothing of Playden's: a folder there was put by hand and is never
    // removed. The sync drops the entry (UnfinishedLegacyItems).
    if (local && local->manifest == 0 && item.legacy()) local = nullptr;
    const bool here = present.count(item.id) > 0;
    if (item.banned) {
      // Steam took it down: nothing downloads or updates. A copy Playden holds stays until the player turns it off
      // or unsubscribes, as one a cut-short list doesn't reach; without one the item isn't planned at all.
      if (local) add(off.count(item.id) ? WorkshopAction::kRemove : WorkshopAction::kKeep, item, off.count(item.id) ? local->bytes : 0);
    } else if (off.count(item.id)) {
      if (local) add(WorkshopAction::kRemove, item, local->bytes);
      else add(WorkshopAction::kSkipOff, item, 0);
    } else if (item.legacy()) {
      add(local ? WorkshopAction::kKeep : WorkshopAction::kSkipLegacy, item, 0);
    } else if (!local) {
      add(here ? WorkshopAction::kSkipHandPlaced : WorkshopAction::kDownload, item, here ? 0 : item.file_size);
    } else if (!here) {
      add(WorkshopAction::kDownload, item, item.file_size);
    } else if (local->manifest != item.manifest || local->time_updated != item.time_updated) {
      add(WorkshopAction::kUpdate, item, item.file_size);
    } else {
      add(WorkshopAction::kKeep, item, 0);
    }
  }
  for (const auto& local : state.items) {
    if (seen.count(local.id)) continue;
    WorkshopItem item;
    item.id = local.id;
    item.title = local.title;
    item.manifest = local.manifest;
    item.time_updated = local.time_updated;
    item.file_size = local.bytes;
    // A cut-short list may just not reach it: kept, unless the player turned it off.
    if (!complete && !off.count(local.id)) add(WorkshopAction::kKeep, item, 0);
    else add(WorkshopAction::kRemove, item, local.bytes);
  }
  return plan;
}

bool WorkshopManifestRefused(const DepotManifest& manifest, std::string* reason) {
  for (const auto& file : manifest.files) {
    if (file.is_symlink()) {
      if (reason) *reason = "item has a link";
      return true;
    }
    std::string first = file.path.substr(0, file.path.find_first_of("/\\"));
    if (Reserved(first)) {
      if (reason) *reason = "item has a .playden- name";
      return true;
    }
  }
  return false;
}

namespace {

// Refuses a Workshop manifest before any chunk is fetched. A refusal stays latched for the rest of the run: Run
// tries other servers after a bad manifest.
std::function<InstallError(const DepotManifest&)> RefusingCheck(std::shared_ptr<std::string> refused) {
  return [refused = std::move(refused)](const DepotManifest& manifest) {
    if (!refused->empty()) return InstallError::kCorrupt;
    std::string reason;
    if (!WorkshopManifestRefused(manifest, &reason)) return InstallError::kNone;
    *refused = reason.empty() ? std::string("refused") : reason;
    return InstallError::kCorrupt;
  };
}

WorkshopDownloader RefusingDownloader(std::shared_ptr<DepotInstaller> installer, StoredSession session,
                                      std::uint32_t app_id, std::uint32_t workshop_depot) {
  auto refused = std::make_shared<std::string>();
  installer->SetManifestCheck(RefusingCheck(refused));
  return [installer, refused, session, app_id, workshop_depot](const WorkshopItem& item, const std::string& staging,
                                                               const std::atomic<bool>& cancel,
                                                               const WorkshopProgressFn& progress) {
    refused->clear();
    return RunItem(*installer, session, app_id, workshop_depot, item, staging, cancel, progress, refused.get());
  };
}

}  // namespace

WorkshopDownloader MakeWorkshopDownloader(InstallerHooks hooks, StoredSession session, std::uint32_t app_id,
                                          std::uint32_t workshop_depot) {
  return RefusingDownloader(std::make_shared<DepotInstaller>(std::move(hooks)), std::move(session), app_id,
                            workshop_depot);
}

WorkshopDownloader MakeWorkshopDownloader(CmClient& cm, CdnClient& cdn, StoredSession session, std::uint32_t app_id,
                                          std::uint32_t workshop_depot) {
  return RefusingDownloader(std::make_shared<DepotInstaller>(cm, cdn), std::move(session), app_id, workshop_depot);
}

std::size_t AdoptSteamClientItems(const std::string& workshop_folder, std::uint32_t app_id,
                                  const std::vector<WorkshopItem>& subscribed, WorkshopState* state) {
  std::vector<const WorkshopItem*> candidates;
  for (const auto& item : subscribed)
    if (item.id != 0 && item.manifest != 0 && !state->Find(item.id)) candidates.push_back(&item);
  if (candidates.empty() || state->items.size() >= kWorkshopMaxItems) return 0;
  const auto installed = runtime::ReadAppWorkshopInstalled(workshop_folder, app_id);
  if (installed.empty()) return 0;
  std::map<std::uint64_t, const runtime::WorkshopAcfItem*> listed;
  for (const auto& entry : installed) listed.emplace(entry.id, &entry);
  const auto workshop = ManagedDirectory::Open(workshop_folder, kTree);
  const auto content = workshop ? workshop->OpenChild("content", kTree) : std::nullopt;
  const auto app = content ? content->OpenChild(std::to_string(app_id), kTree) : std::nullopt;
  if (!app) return 0;
  std::size_t adopted = 0;
  for (const auto* item : candidates) {
    if (state->items.size() >= kWorkshopMaxItems) break;
    const auto found = listed.find(item->id);
    if (found == listed.end()) continue;
    struct stat info{};
    if (!app->InspectChild(std::to_string(item->id), &info) || !S_ISDIR(info.st_mode)) continue;
    WorkshopLocalItem local;
    local.id = item->id;
    local.manifest = found->second->manifest;
    local.time_updated = found->second->time_updated;
    local.bytes = found->second->bytes;
    local.on = true;
    local.title = SanitiseWorkshopText(item->title, kWorkshopTitleBytes);
    state->items.push_back(std::move(local));
    ++adopted;
  }
  return adopted;
}

WorkshopSyncResult SyncWorkshopItems(const WorkshopSyncRequest& request, const std::atomic<bool>& cancel) {
  WorkshopSyncResult result;
  const auto log = [&request](const std::string& line) { if (request.log) request.log(line); };
  const auto fail = [&result, &log](InstallError error, const char* detail) {
    result.error = error;
    result.detail = detail;
    log(std::string("workshop sync failed error=") + InstallErrorName(error) + " detail=" + detail);
    return result;
  };
  if (request.app_id == 0 || !request.download || request.workshop_folder.empty())
    return fail(InstallError::kProtocol, "invalid workshop sync");
  const auto workshop = ManagedDirectory::Open(request.workshop_folder, kTree);
  if (!workshop) return fail(InstallError::kStorage, "cannot open workshop folder");
  WorkshopState state;
  {
    const auto name = WorkshopStateName(request.app_id);
    std::string text;
    const auto read = ReadChecked(*workshop, name, kWorkshopStateMaxBytes,
                                  [](const std::string& body) { return ParseWorkshopState(body).has_value(); }, &text);
    if (read == SmallFile::kUnreadable) return fail(InstallError::kStorage, "cannot read workshop state");
    if (read == SmallFile::kDamaged) {
      // Otherwise every sync would stop here. The items it listed stay, as folders Playden leaves alone.
      if (!SetAside(*workshop, name)) return fail(InstallError::kStorage, "cannot set damaged workshop state aside");
      log("workshop state damaged: set aside, starting empty");
    }
    if (read == SmallFile::kOk) state = *ParseWorkshopState(text);
  }
  const auto content = workshop->OpenChild("content", kTree, true, kFolderMode);
  const auto app = content ? content->OpenChild(std::to_string(request.app_id), kTree, true, kFolderMode) : std::nullopt;
  const auto downloads = workshop->OpenChild(kWorkshopDownloadDir, kTree, true, 0700);
  if (!app || !downloads) return fail(InstallError::kStorage, "cannot open workshop folders");
  const auto save = [&] { return WriteWorkshopState(request.workshop_folder, request.app_id, state); };
  // The game must not run while the items folder changes: asked before each change, told after it.
  const auto may_change = [&request] { return !request.may_apply || request.may_apply(); };
  const auto changed = [&request] { if (request.applied) request.applied(); };
  const auto held = [&result, &log] {
    result.error = InstallError::kGameRunning;
    result.detail = "game running";
    log("workshop sync held: the game runs");
    return result;
  };

  // What an interrupted swap left, for Playden's items only, all of it in Playden's own .playden-download/:
  // <id> moved aside to <id>.old but <id>.new not yet in its place gets its old copy back; a leftover old copy or
  // unfinished new one goes. Nothing else in content/<appid>/ is looked at (a player's "<id>.new" there is theirs).
  for (const auto& local : state.items) {
    const auto id = std::to_string(local.id);
    const auto old = id + ".old", fresh = id + ".new";
    if (!Exists(*downloads, old) && !Exists(*downloads, fresh)) continue;
    // Held for the game: the restore changes the items folder, and the old copy is the one the state lists.
    if (!may_change()) return held();
    bool ok = true;
    if (!Exists(*app, id) && Exists(*downloads, old))
      ok = downloads->RenameChild(old, *app, id, false) && app->Sync() && downloads->Sync();
    ok = ok && RemoveManagedTree(*downloads, old) && RemoveManagedTree(*downloads, fresh);
    changed();
    if (!ok) return fail(InstallError::kStorage, "cannot clear an interrupted item");
  }

  // Items Valve's client downloaded in Online become Playden's, as they are.
  if (const auto adopted = AdoptSteamClientItems(request.workshop_folder, request.app_id, request.subscribed, &state)) {
    if (!save()) return fail(InstallError::kStorage, "cannot write workshop state");
    log("workshop sync adopted=" + std::to_string(adopted));
  }
  std::set<std::uint64_t> present;
  for (const auto& item : request.subscribed)
    if (item.id && Exists(*app, std::to_string(item.id))) present.insert(item.id);
  for (const auto& local : state.items)
    if (Exists(*app, std::to_string(local.id))) present.insert(local.id);
  result.plan = PlanWorkshopSync(request.subscribed, state, request.off, present, request.complete);
  {
    std::map<WorkshopAction, std::size_t> counts;
    for (const auto& entry : result.plan.entries) ++counts[entry.action];
    std::string line = "workshop sync plan items=" + std::to_string(result.plan.entries.size());
    for (const auto& [action, count] : counts) line += std::string(" ") + WorkshopActionName(action) + "=" + std::to_string(count);
    log(line + " download_bytes=" + std::to_string(result.plan.download_bytes) +
        " freed_bytes=" + std::to_string(result.plan.freed_bytes));
  }

  // Staging of Playden's items that this sync doesn't download: an earlier download it no longer wants. (One it
  // does download is cleared right before that download starts.)
  std::set<std::uint64_t> fetching;
  for (const auto& entry : result.plan.entries)
    if (entry.action == WorkshopAction::kDownload || entry.action == WorkshopAction::kUpdate) fetching.insert(entry.item.id);
  for (const auto& local : state.items) {
    if (fetching.count(local.id)) continue;
    if (!RemoveManagedTree(*downloads, std::to_string(local.id)))
      return fail(InstallError::kStorage, "cannot clear a download");
  }
  // A legacy item listed for a download that never finished (PlanWorkshopSync) holds nothing, its staging just went:
  // it leaves the state, so the sheet and the launch don't take it for an item in the bottle. Its folder, if any,
  // stays: it is the player's.
  {
    std::set<std::uint64_t> unfinished;
    for (const auto& item : request.subscribed)
      if (const auto* local = state.Find(item.id); local && local->manifest == 0 && item.legacy())
        unfinished.insert(item.id);
    if (!unfinished.empty()) {
      state.items.erase(std::remove_if(state.items.begin(), state.items.end(),
                                       [&](const WorkshopLocalItem& local) { return unfinished.count(local.id) > 0; }),
                        state.items.end());
      if (!save()) return fail(InstallError::kStorage, "cannot write workshop state");
      log("workshop sync dropped-unfinished-legacy=" + std::to_string(unfinished.size()));
    }
  }

  std::size_t index = 0;
  std::uint64_t bytes_done = 0;
  const auto settle = [&result](std::uint64_t id, WorkshopOutcome outcome, InstallError error = InstallError::kNone,
                                std::string detail = {}) {
    result.items.push_back({id, outcome, error, std::move(detail)});
    if (outcome == WorkshopOutcome::kFailed && result.error == InstallError::kNone) {
      result.error = error;
      result.detail = result.items.back().detail;
    }
  };
  // Removals first: they free space, and keep the state file within its cap while new items join it.
  std::vector<const WorkshopPlanEntry*> order;
  for (const auto& entry : result.plan.entries)
    if (entry.action == WorkshopAction::kRemove) order.push_back(&entry);
  for (const auto& entry : result.plan.entries)
    if (entry.action != WorkshopAction::kRemove) order.push_back(&entry);
  bool removing = false;  // the removals were let through, and `changed` is still owed
  std::set<std::uint64_t> deleted;  // removed, not yet out of the client's appworkshop file
  const auto removals_done = [&] {
    if (!deleted.empty()) {
      // Before the game may run: Valve's client must not take a deleted folder for an installed item.
      const auto outcome = runtime::RemoveFromAppWorkshopAcf(request.workshop_folder, request.app_id, deleted);
      if (outcome != runtime::WorkshopAcfOutcome::kNoItems && outcome != runtime::WorkshopAcfOutcome::kUnchanged)
        log(std::string("workshop sync acf=") + runtime::WorkshopAcfOutcomeName(outcome) +
            " dropped=" + std::to_string(deleted.size()));
      deleted.clear();
    }
    if (removing) changed();
    removing = false;
  };
  for (const auto* planned : order) {
    const auto& entry = *planned;
    if (entry.action != WorkshopAction::kRemove) removals_done();
    if (cancel.load()) {
      result.error = InstallError::kCanceled;
      result.detail = "canceled";
      break;
    }
    const auto& item = entry.item;
    const auto id = std::to_string(item.id);
    switch (entry.action) {
      case WorkshopAction::kKeep: settle(item.id, WorkshopOutcome::kKept); continue;
      case WorkshopAction::kSkipLegacy: settle(item.id, WorkshopOutcome::kSkippedLegacy); continue;
      case WorkshopAction::kSkipHandPlaced: settle(item.id, WorkshopOutcome::kSkippedHandPlaced); continue;
      case WorkshopAction::kSkipOff: settle(item.id, WorkshopOutcome::kSkippedOff); continue;
      case WorkshopAction::kRemove: {
        if (!removing) {
          if (!may_change()) {
            held();
            break;
          }
          removing = true;
        }
        const bool removed = RemoveManagedTree(*app, id) && RemoveManagedTree(*downloads, id + ".new") &&
                             RemoveManagedTree(*downloads, id + ".old") && RemoveManagedTree(*downloads, id);
        if (!removed) {
          if (auto* local = state.Find(item.id)) local->on = !request.off.count(item.id);
          save();
          settle(item.id, WorkshopOutcome::kFailed, InstallError::kStorage, "cannot delete item files");
          continue;
        }
        state.items.erase(std::remove_if(state.items.begin(), state.items.end(),
                                         [&item](const WorkshopLocalItem& local) { return local.id == item.id; }),
                          state.items.end());
        deleted.insert(item.id);
        if (!save()) settle(item.id, WorkshopOutcome::kFailed, InstallError::kStorage, "cannot write workshop state");
        else settle(item.id, WorkshopOutcome::kRemoved);
        continue;
      }
      case WorkshopAction::kDownload:
      case WorkshopAction::kUpdate: break;
    }
    if (result.error == InstallError::kGameRunning) break;

    // Playden owns the item's folder from here: listed before anything is made for it.
    if (!state.Find(item.id)) {
      if (state.items.size() >= kWorkshopMaxItems) {
        settle(item.id, WorkshopOutcome::kFailed, InstallError::kStorage, "too many workshop items");
        continue;
      }
      WorkshopLocalItem local;
      local.id = item.id;
      local.title = item.title;
      state.items.push_back(local);
      if (!save()) {
        state.items.pop_back();
        settle(item.id, WorkshopOutcome::kFailed, InstallError::kStorage, "cannot write workshop state");
        continue;
      }
    }
    const auto drop_new_entry = [&] {
      const auto* local = state.Find(item.id);
      if (!local || local->manifest != 0 || Exists(*app, id)) return;
      state.items.erase(std::remove_if(state.items.begin(), state.items.end(),
                                       [&item](const WorkshopLocalItem& other) { return other.id == item.id; }),
                        state.items.end());
      save();
    };
    // Every download starts in an empty staging folder Playden just made: DepotInstaller writes inside it by plain
    // path, so nothing left there (a link above all) may be resumed. A tree that can't be removed (it holds a link)
    // fails the item rather than be followed.
    if (!RemoveManagedTree(*downloads, id)) {
      settle(item.id, WorkshopOutcome::kFailed, InstallError::kStorage, "cannot prepare download");
      continue;
    }
    const auto staging = downloads->OpenChild(id, kTree, true, kFolderMode);
    if (!staging || !downloads->Sync()) {
      settle(item.id, WorkshopOutcome::kFailed, InstallError::kStorage, "cannot prepare download");
      continue;
    }
    const auto clear_staging = [&] { return RemoveManagedTree(*downloads, id); };
    const WorkshopProgressFn on_progress = [&](const InstallProgress& progress) {
      if (!request.progress) return;
      WorkshopSyncProgress report;
      report.index = index;
      report.count = result.plan.downloads;
      report.item_id = item.id;
      report.item_bytes = item.file_size;
      report.item = progress;
      report.bytes_done = bytes_done;
      report.bytes_total = result.plan.download_bytes;
      request.progress(report);
    };
    const auto downloaded = request.download(item, staging->path().string(), cancel, on_progress);
    ++index;
    std::set<std::string> names;
    std::string why = downloaded.detail;
    const auto staged = downloaded.error == InstallError::kNone && !downloaded.refused
                            ? StagedNames(*staging, &names, &why) : Staged::kOk;
    if (downloaded.refused || staged == Staged::kReserved) {
      clear_staging();
      drop_new_entry();
      settle(item.id, WorkshopOutcome::kRefused, InstallError::kCorrupt, why);
      continue;
    }
    if (downloaded.error == InstallError::kCanceled) {
      result.error = InstallError::kCanceled;
      result.detail = "canceled";
      break;
    }
    if (downloaded.error != InstallError::kNone) {
      settle(item.id, WorkshopOutcome::kFailed, downloaded.error, downloaded.detail);
      // What stops this download stops the next ones too: no session, no network, a full drive.
      if (downloaded.error == InstallError::kNoSession || downloaded.error == InstallError::kNetwork ||
          downloaded.error == InstallError::kNoSpace)
        break;
      continue;
    }
    if (staged == Staged::kUnreadable) {
      settle(item.id, WorkshopOutcome::kFailed, InstallError::kStorage, why);
      continue;
    }
    // Into .playden-download/<id>.new, then in place of content/<appid>/<id>, the old copy aside as
    // .playden-download/<id>.old: the only name Playden changes in content/<appid>/ is the item's own. The download
    // stays in staging while the game runs.
    if (!may_change()) {
      held();
      break;
    }
    const auto fresh = id + ".new", old = id + ".old";
    bool moved = RemoveManagedTree(*downloads, fresh) && RemoveManagedTree(*downloads, old);
    const auto next = moved ? downloads->OpenChild(fresh, kTree, true, kFolderMode) : std::nullopt;
    moved = next.has_value();
    for (const auto& name : names) {
      if (!moved) break;
      if (!Exists(*staging, name)) continue;
      moved = staging->RenameChild(name, *next, name, false);
    }
    moved = moved && next->Sync() && downloads->Sync();
    const bool had = moved && Exists(*app, id);
    if (had) moved = app->RenameChild(id, *downloads, old, false);
    moved = moved && downloads->RenameChild(fresh, *app, id, false) && app->Sync() && downloads->Sync();
    // The old copy goes before the game may run again: a game that looks for items by folder would see it.
    if (moved) RemoveManagedTree(*downloads, old);
    if (!moved) {
      // The old copy back where it was, if it went aside; else the next sync's recovery brings it back.
      if (had && !Exists(*app, id) && Exists(*downloads, old) && downloads->RenameChild(old, *app, id, false))
        (void)(app->Sync() && downloads->Sync());
      changed();
      settle(item.id, WorkshopOutcome::kFailed, InstallError::kStorage, "cannot put item files in place");
      continue;
    }
    auto* local = state.Find(item.id);
    local->manifest = item.manifest;
    local->time_updated = item.time_updated;
    local->bytes = item.file_size;
    local->on = true;
    local->title = item.title;
    // The game may run again only once the state lists the item as it now is: a launch reads it for mods.json.
    const bool saved = save();
    changed();
    if (!saved) {
      settle(item.id, WorkshopOutcome::kFailed, InstallError::kStorage, "cannot write workshop state");
      continue;
    }
    bytes_done += item.file_size;
    clear_staging();
    settle(item.id, entry.action == WorkshopAction::kUpdate ? WorkshopOutcome::kUpdated : WorkshopOutcome::kDownloaded);
  }
  removals_done();
  std::map<WorkshopOutcome, std::size_t> counts;
  for (const auto& item : result.items) ++counts[item.outcome];
  std::string line = std::string("workshop sync ") + (result.error == InstallError::kNone ? "done" : "stopped") +
                     " error=" + InstallErrorName(result.error);
  for (const auto& [outcome, count] : counts) line += std::string(" ") + WorkshopOutcomeName(outcome) + "=" + std::to_string(count);
  if (result.error != InstallError::kNone) line += " detail=" + WorkshopLogDetail(result.detail);
  log(line);
  return result;
}

const char* WorkshopFailureText(InstallError error) {
  switch (error) {
    case InstallError::kNone: return "";
    case InstallError::kCanceled: return "";
    case InstallError::kNoSession: return "Sign in to Steam to download Workshop items.";
    case InstallError::kRejected: return "Steam refused access to this game's Workshop items.";
    case InstallError::kNetwork: return "Network problem. Resume to try again.";
    case InstallError::kCorrupt: return "Downloaded content failed verification. Resume to try again.";
    case InstallError::kNoSpace: return "Not enough free space.";
    case InstallError::kStorage: return "Could not write the Workshop items.";
    case InstallError::kProtocol: return "Steam returned unexpected data.";
    case InstallError::kGameRunning: return "Waiting for the game to close.";
  }
  return "Workshop items could not be downloaded.";
}

WorkshopJobResult RunWorkshopJob(const WorkshopJobRequest& request, const std::atomic<bool>& cancel) {
  WorkshopJobResult out;
  const auto log = [&request](const std::string& line) { if (request.log) request.log(line); };
  const auto fail = [&out, &log](const char* failure, const char* why) {
    out.failure = failure;
    log(std::string("workshop job failed reason=") + why);
    return out;
  };
  if (request.app_id == 0 || !request.subscriptions || !request.download)
    return fail("Playden can't download Workshop items for this game.", "invalid");
  // The bottle exists once the game was prepared: Playden doesn't make one for its items.
  struct stat info{};
  if (request.prefix.empty() || ::lstat((fs::path(request.prefix) / "drive_c").c_str(), &info) != 0 ||
      !S_ISDIR(info.st_mode))
    return fail(kWorkshopNoBottle, "no-bottle");
  if (!EnsureWorkshopFolder(request.prefix)) return fail(WorkshopFailureText(InstallError::kStorage), "workshop-folder");
  const auto folder = WorkshopFolder(request.prefix);
  const auto workshop = ManagedDirectory::Open(folder, kTree);
  if (!workshop) return fail(WorkshopFailureText(InstallError::kStorage), "workshop-folder");

  // The player's choice first: it holds whatever happens to the rest of the job. Online has none: every item is on,
  // and the choice saved for Offline emulation stays as it is.
  std::set<std::uint64_t> off;
  if (request.all_on) {
    log("workshop job all_on=1");
  } else if (request.off) {
    if (!WriteWorkshopOff(folder, request.app_id, *request.off))
      return fail(WorkshopFailureText(InstallError::kStorage), "off-write");
    off = *request.off;
  } else {
    const auto name = WorkshopOffName(request.app_id);
    std::string text;
    const auto read = ReadChecked(*workshop, name, kWorkshopOffMaxBytes,
                                  [](const std::string& body) { return ParseWorkshopOff(body).has_value(); }, &text);
    if (read == SmallFile::kUnreadable) return fail(WorkshopFailureText(InstallError::kStorage), "off-read");
    // Items the player turned off would download: the choice is asked for again instead. The file stays, so a
    // retry fails the same way; the next choice replaces it.
    if (read == SmallFile::kDamaged) return fail("Choose the game's Workshop items again.", "off-damaged");
    if (read == SmallFile::kOk) off = *ParseWorkshopOff(text);
  }

  auto subscribed = request.subscriptions(cancel);
  if (cancel.load() || subscribed.error == CmError::kCanceled) {
    out.canceled = true;
    return out;
  }
  if (!subscribed.ok()) {
    switch (subscribed.error) {
      case CmError::kNetwork:
      case CmError::kTimeout:
        out.retry_later = true;
        return fail("Could not reach Steam. Resume to try again.", "subscriptions-network");
      case CmError::kNoSession: return fail(WorkshopFailureText(InstallError::kNoSession), "subscriptions-session");
      default: return fail("Steam didn't send the game's Workshop items. Resume to try again.", "subscriptions");
    }
  }

  WorkshopSyncRequest sync;
  sync.app_id = request.app_id;
  sync.workshop_folder = folder;
  sync.subscribed = std::move(subscribed.items);
  sync.off = std::move(off);
  sync.complete = !subscribed.truncated;
  sync.download = request.download;
  sync.may_apply = request.may_apply;
  sync.applied = request.applied;
  sync.log = request.log;
  if (request.progress)
    sync.progress = [&request](const WorkshopSyncProgress& report) {
      // The item's share by its download, in the plan's units (file sizes); the two may disagree.
      std::uint64_t part = 0;
      if (report.item.download_total)
        part = static_cast<std::uint64_t>(static_cast<long double>(report.item_bytes) *
                                          std::min(report.item.download_done, report.item.download_total) /
                                          report.item.download_total);
      request.progress(std::min(report.bytes_done + part, report.bytes_total), report.bytes_total);
    };
  out.sync = SyncWorkshopItems(sync, cancel);
  switch (out.sync.error) {
    case InstallError::kNone: out.ok = true; break;
    case InstallError::kCanceled: out.canceled = true; break;
    case InstallError::kGameRunning: out.retry_later = out.game_running = true; break;
    case InstallError::kNetwork: out.retry_later = true; out.failure = WorkshopFailureText(out.sync.error); break;
    default: out.failure = WorkshopFailureText(out.sync.error); break;
  }
  if (out.ok && request.progress) request.progress(out.sync.plan.download_bytes, out.sync.plan.download_bytes);
  return out;
}

}  // namespace playden::steam
