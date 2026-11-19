// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime/workshop_acf.h"

#include "platform/managed_directory.h"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <limits>
#include <set>
#include <utility>

#include <unistd.h>

namespace playden::runtime {
namespace {

using platform::DirectoryPolicy;
using platform::ManagedDirectory;
using steam::VdfNode;

constexpr const char* kRootKey = "AppWorkshop";
constexpr const char* kInstalledKey = "WorkshopItemsInstalled";
constexpr const char* kDetailsKey = "WorkshopItemDetails";

bool SameKey(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    char x = a[i];
    char y = b[i];
    if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
    if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
    if (x != y) return false;
  }
  return true;
}

// The first child named `key` (any case), as VdfNode::Find finds it.
VdfNode* FindChild(VdfNode& node, std::string_view key) {
  for (auto& entry : node.children)
    if (SameKey(entry.first, key)) return &entry.second;
  return nullptr;
}

VdfNode Leaf(std::string value) {
  VdfNode node;
  node.value = std::move(value);
  return node;
}

VdfNode Dict() {
  VdfNode node;
  node.is_dict = true;
  return node;
}

// Sets the string child `key`, added at the end when missing. False when it is a dict.
bool SetLeaf(VdfNode& node, const char* key, std::string value) {
  if (VdfNode* found = FindChild(node, key)) {
    if (found->is_dict) return false;
    found->value = std::move(value);
    return true;
  }
  node.children.emplace_back(key, Leaf(std::move(value)));
  return true;
}

VdfNode InstalledEntry(const WorkshopAcfItem& item) {
  VdfNode entry = Dict();
  entry.children.emplace_back("size", Leaf(std::to_string(item.bytes)));
  entry.children.emplace_back("timeupdated", Leaf(std::to_string(item.time_updated)));
  entry.children.emplace_back("manifest", Leaf(std::to_string(item.manifest)));
  return entry;
}

// Adds the items the installed list doesn't name; the number added.
std::size_t AddItems(VdfNode& installed, const std::vector<WorkshopAcfItem>& items) {
  std::set<std::string> listed;
  for (const auto& entry : installed.children) listed.insert(entry.first);
  std::size_t added = 0;
  for (const auto& item : items) {
    if (item.id == 0) continue;
    auto key = std::to_string(item.id);
    if (!listed.insert(key).second) continue;
    installed.children.emplace_back(std::move(key), InstalledEntry(item));
    ++added;
  }
  return added;
}

std::uint64_t InstalledBytes(const VdfNode& installed) {
  std::uint64_t total = 0;
  for (const auto& entry : installed.children) {
    const auto size = entry.second.is_dict ? entry.second.Uint64("size") : std::nullopt;
    if (!size) continue;
    total = *size > std::numeric_limits<std::uint64_t>::max() - total ? std::numeric_limits<std::uint64_t>::max()
                                                                       : total + *size;
  }
  return total;
}

void AppendQuoted(std::string_view text, std::string* out) {
  out->push_back('"');
  for (const char c : text) {
    switch (c) {
      case '\\': out->append("\\\\"); break;
      case '"': out->append("\\\""); break;
      case '\n': out->append("\\n"); break;
      case '\t': out->append("\\t"); break;
      default: out->push_back(c); break;
    }
  }
  out->push_back('"');
}

void AppendEntries(const VdfNode& node, std::size_t depth, std::string* out) {
  for (const auto& [key, child] : node.children) {
    out->append(depth, '\t');
    AppendQuoted(key, out);
    if (child.is_dict) {
      out->push_back('\n');
      out->append(depth, '\t');
      out->append("{\n");
      AppendEntries(child, depth + 1, out);
      out->append(depth, '\t');
      out->append("}\n");
    } else {
      out->append("\t\t");
      AppendQuoted(child.value, out);
      out->push_back('\n');
    }
  }
}

// The text when ParseVdf reads it back as the same tree, and the next read takes it (at most kWorkshopAcfMaxBytes).
std::optional<std::string> Checked(const VdfNode& root) {
  auto text = SerializeVdf(root);
  if (text.size() > kWorkshopAcfMaxBytes) return std::nullopt;
  const auto again = steam::ParseVdf(text);
  if (!again || SerializeVdf(*again) != text) return std::nullopt;
  return text;
}

std::optional<std::string> FreshFile(std::uint32_t app_id, const std::vector<WorkshopAcfItem>& items) {
  VdfNode installed = Dict();
  if (AddItems(installed, items) == 0) return std::nullopt;
  VdfNode workshop = Dict();
  workshop.children.emplace_back("appid", Leaf(std::to_string(app_id)));
  workshop.children.emplace_back("SizeOnDisk", Leaf(std::to_string(InstalledBytes(installed))));
  workshop.children.emplace_back("NeedsUpdate", Leaf("0"));
  workshop.children.emplace_back("NeedsDownload", Leaf("0"));
  workshop.children.emplace_back("TimeLastUpdated", Leaf("0"));
  workshop.children.emplace_back("TimeLastAppRan", Leaf("0"));
  workshop.children.emplace_back("LastBuildID", Leaf("0"));
  workshop.children.emplace_back(kInstalledKey, std::move(installed));
  workshop.children.emplace_back(kDetailsKey, Dict());
  VdfNode root = Dict();
  root.children.emplace_back(kRootKey, std::move(workshop));
  return Checked(root);
}

// `existing` parsed, when it is this game's appworkshop file: one AppWorkshop dict with its app ID, and a
// WorkshopItemsInstalled that is a dict when there is one. Its AppWorkshop dict is Workshop(root).
std::optional<VdfNode> ParseAppWorkshop(std::string_view existing, std::uint32_t app_id) {
  if (existing.size() > kWorkshopAcfMaxBytes) return std::nullopt;
  auto root = steam::ParseVdf(existing);
  if (!root || root->children.size() != 1 || !SameKey(root->children[0].first, kRootKey)) return std::nullopt;
  VdfNode& node = root->children[0].second;
  if (!node.is_dict || node.Uint64("appid") != std::optional<std::uint64_t>(app_id)) return std::nullopt;
  if (const VdfNode* installed = node.Find(kInstalledKey); installed && !installed->is_dict) return std::nullopt;
  return root;
}

VdfNode& Workshop(VdfNode& root) { return root.children[0].second; }

// The file, when it is at most kWorkshopAcfMaxBytes. `missing`: there is none; `refused`: something Playden doesn't read
// (a link, a folder, too big). Neither: it couldn't be read.
std::optional<std::string> ReadAcf(const ManagedDirectory& folder, const std::string& name, bool* missing, bool* refused) {
  *missing = *refused = false;
  const int fd = folder.OpenRegularFile(name);
  if (fd < 0) {
    if (errno == ENOENT) *missing = true;
    else *refused = true;
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
    if (text.size() + static_cast<std::size_t>(got) > kWorkshopAcfMaxBytes) {
      ok = false;
      *refused = true;
      break;
    }
    text.append(buffer, static_cast<std::size_t>(got));
  }
  ::close(fd);
  if (!ok) return std::nullopt;
  return text;
}

bool WriteAtomic(const ManagedDirectory& folder, const std::string& name, const std::string& text) {
  const std::string temporary = name + ".playden-tmp";
  if (!folder.RemoveRegularFile(temporary) && errno != ENOENT) return false;
  const int fd = folder.CreateExclusiveFile(temporary, 0644);
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

}  // namespace

std::string WorkshopAcfName(std::uint32_t app_id) { return "appworkshop_" + std::to_string(app_id) + ".acf"; }

std::string SerializeVdf(const VdfNode& root) {
  std::string text;
  AppendEntries(root, 0, &text);
  return text;
}

std::optional<std::string> MergeAppWorkshopAcf(std::optional<std::string_view> existing, std::uint32_t app_id,
                                               const std::vector<WorkshopAcfItem>& items) {
  if (items.size() > kWorkshopAcfMaxItems) return std::nullopt;
  if (!existing) return FreshFile(app_id, items);
  auto root = ParseAppWorkshop(*existing, app_id);
  if (!root) return std::nullopt;
  VdfNode& workshop = Workshop(*root);
  VdfNode* installed = FindChild(workshop, kInstalledKey);
  if (!installed) {
    VdfNode empty = Dict();
    if (AddItems(empty, items) == 0) return std::string(*existing);
    workshop.children.emplace_back(kInstalledKey, std::move(empty));
    installed = &workshop.children.back().second;
  } else if (AddItems(*installed, items) == 0) {
    return std::string(*existing);
  }
  const auto bytes = InstalledBytes(*installed);
  if (!SetLeaf(workshop, "NeedsDownload", "0") || !SetLeaf(workshop, "SizeOnDisk", std::to_string(bytes)))
    return std::nullopt;
  return Checked(*root);
}

const char* WorkshopAcfOutcomeName(WorkshopAcfOutcome outcome) {
  switch (outcome) {
    case WorkshopAcfOutcome::kNoItems: return "no-items";
    case WorkshopAcfOutcome::kWritten: return "written";
    case WorkshopAcfOutcome::kUnchanged: return "unchanged";
    case WorkshopAcfOutcome::kLeftAlone: return "left-alone";
    case WorkshopAcfOutcome::kFailed: return "failed";
  }
  return "?";
}

std::optional<std::string> DropFromAppWorkshopAcf(std::string_view existing, std::uint32_t app_id,
                                                  const std::set<std::uint64_t>& ids) {
  auto root = ParseAppWorkshop(existing, app_id);
  if (!root) return std::nullopt;
  VdfNode* node = &Workshop(*root);
  VdfNode* installed = FindChild(*node, kInstalledKey);
  if (!installed) return std::string(existing);
  const auto before = installed->children.size();
  auto& children = installed->children;
  children.erase(std::remove_if(children.begin(), children.end(),
                                [&ids](const auto& entry) {
                                  std::uint64_t id = 0;
                                  const auto end = entry.first.data() + entry.first.size();
                                  const auto parsed = std::from_chars(entry.first.data(), end, id);
                                  return parsed.ec == std::errc() && parsed.ptr == end && ids.count(id) > 0;
                                }),
                 children.end());
  if (children.size() == before) return std::string(existing);
  if (!SetLeaf(*node, "SizeOnDisk", std::to_string(InstalledBytes(*installed)))) return std::nullopt;
  return Checked(*root);
}

std::vector<WorkshopAcfItem> ParseAppWorkshopInstalled(std::string_view existing, std::uint32_t app_id) {
  std::vector<WorkshopAcfItem> items;
  auto root = ParseAppWorkshop(existing, app_id);
  if (!root) return items;
  const VdfNode* node = &Workshop(*root);
  const VdfNode* installed = node->Find(kInstalledKey);
  if (!installed) return items;
  for (const auto& [key, entry] : installed->children) {
    if (!entry.is_dict || items.size() >= kWorkshopAcfMaxItems) continue;
    std::uint64_t id = 0;
    const auto end = key.data() + key.size();
    const auto parsed = std::from_chars(key.data(), end, id);
    const auto manifest = entry.Uint64("manifest");
    const auto time = entry.Uint64("timeupdated");
    if (parsed.ec != std::errc() || parsed.ptr != end || id == 0 || !manifest || *manifest == 0) continue;
    if (time && *time > std::numeric_limits<std::uint32_t>::max()) continue;
    items.push_back({id, entry.Uint64("size").value_or(0), static_cast<std::uint32_t>(time.value_or(0)), *manifest});
  }
  return items;
}

std::vector<WorkshopAcfItem> ReadAppWorkshopInstalled(const std::string& workshop_folder, std::uint32_t app_id) {
  const auto folder = ManagedDirectory::Open(workshop_folder, DirectoryPolicy::kApprovedTree);
  if (!folder) return {};
  bool missing = false, refused = false;
  const auto text = ReadAcf(*folder, WorkshopAcfName(app_id), &missing, &refused);
  return text ? ParseAppWorkshopInstalled(*text, app_id) : std::vector<WorkshopAcfItem>{};
}

WorkshopAcfOutcome RemoveFromAppWorkshopAcf(const std::string& workshop_folder, std::uint32_t app_id,
                                            const std::set<std::uint64_t>& ids) {
  if (ids.empty()) return WorkshopAcfOutcome::kNoItems;
  const auto folder = ManagedDirectory::Open(workshop_folder, DirectoryPolicy::kApprovedTree);
  if (!folder) return WorkshopAcfOutcome::kFailed;
  const auto name = WorkshopAcfName(app_id);
  bool missing = false, refused = false;
  const auto existing = ReadAcf(*folder, name, &missing, &refused);
  if (!existing) return missing ? WorkshopAcfOutcome::kNoItems : refused ? WorkshopAcfOutcome::kLeftAlone : WorkshopAcfOutcome::kFailed;
  const auto dropped = DropFromAppWorkshopAcf(*existing, app_id, ids);
  if (!dropped) return WorkshopAcfOutcome::kLeftAlone;
  if (*dropped == *existing) return WorkshopAcfOutcome::kUnchanged;
  return WriteAtomic(*folder, name, *dropped) ? WorkshopAcfOutcome::kWritten : WorkshopAcfOutcome::kFailed;
}

WorkshopAcfOutcome ApplyAppWorkshopAcf(const std::string& workshop_folder, std::uint32_t app_id,
                                       const std::vector<WorkshopAcfItem>& items) {
  if (items.empty()) return WorkshopAcfOutcome::kNoItems;
  if (items.size() > kWorkshopAcfMaxItems) return WorkshopAcfOutcome::kFailed;
  const auto folder = ManagedDirectory::Open(workshop_folder, DirectoryPolicy::kApprovedTree);
  if (!folder) return WorkshopAcfOutcome::kFailed;
  const auto name = WorkshopAcfName(app_id);
  bool missing = false, refused = false;
  const auto existing = ReadAcf(*folder, name, &missing, &refused);
  if (!existing && !missing) return refused ? WorkshopAcfOutcome::kLeftAlone : WorkshopAcfOutcome::kFailed;
  const auto merged = MergeAppWorkshopAcf(existing ? std::optional<std::string_view>(*existing) : std::nullopt, app_id, items);
  if (!merged) return existing ? WorkshopAcfOutcome::kLeftAlone : WorkshopAcfOutcome::kNoItems;
  if (existing && *merged == *existing) return WorkshopAcfOutcome::kUnchanged;
  return WriteAtomic(*folder, name, *merged) ? WorkshopAcfOutcome::kWritten : WorkshopAcfOutcome::kFailed;
}

}  // namespace playden::runtime
