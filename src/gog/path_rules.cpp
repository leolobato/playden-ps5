// SPDX-License-Identifier: GPL-3.0-or-later
#include "gog/path_rules.h"

#include <vector>

namespace playden::gog {
namespace {

// Playden's own names at a game folder's top: the install record, the file list and its temp, the download and
// update journals, the GOG folder (manifests, support files), copy and move journals.
const char* const kReserved[] = {
    ".playden-install.json", ".playden-files", ".playden-files.tmp", ".playden-download", ".playden-gog",
    ".playden-copy",         ".playden-update", ".playden-tree",      ".playden-files-changed",
};

char Lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool BadNameCharacter(char c) {
  return c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|';
}

std::optional<std::string> Refuse(PathRefusal refusal, PathRefusal* why) {
  if (why) *why = refusal;
  return std::nullopt;
}

}  // namespace

const char* PathRefusalName(PathRefusal refusal) {
  switch (refusal) {
    case PathRefusal::kNone: return "none";
    case PathRefusal::kEmpty: return "empty";
    case PathRefusal::kAbsolute: return "absolute";
    case PathRefusal::kDrive: return "drive";
    case PathRefusal::kDotComponent: return "dot_component";
    case PathRefusal::kEmptyComponent: return "empty_component";
    case PathRefusal::kControl: return "control";
    case PathRefusal::kTooLong: return "too_long";
    case PathRefusal::kComponentTooLong: return "component_too_long";
    case PathRefusal::kReserved: return "reserved";
    case PathRefusal::kLink: return "link";
    case PathRefusal::kClash: return "clash";
  }
  return "none";
}

std::string FoldCase(std::string_view text) {
  std::string folded(text);
  for (char& c : folded) c = Lower(c);
  return folded;
}

bool IsReservedName(std::string_view name) {
  const auto folded = FoldCase(name);
  for (const char* reserved : kReserved)
    if (folded == reserved) return true;
  return false;
}

std::optional<std::string> NormalizePath(std::string_view raw, PathRefusal* why) {
  if (why) *why = PathRefusal::kNone;
  for (const char c : raw) {
    const auto byte = static_cast<unsigned char>(c);
    if (byte < 0x20 || byte == 0x7f) return Refuse(PathRefusal::kControl, why);
  }
  std::string path(raw);
  for (char& c : path)
    if (c == '\\') c = '/';
  if (!path.empty() && path.front() == '/') path.erase(0, 1);
  if (path.empty()) return Refuse(PathRefusal::kEmpty, why);
  if (path.front() == '/') return Refuse(PathRefusal::kAbsolute, why);
  if (path.size() > kMaxPathBytes) return Refuse(PathRefusal::kTooLong, why);
  std::size_t start = 0;
  while (true) {
    const auto slash = path.find('/', start);
    const auto end = slash == std::string::npos ? path.size() : slash;
    const std::string_view component(path.data() + start, end - start);
    if (component.empty()) return Refuse(PathRefusal::kEmptyComponent, why);
    if (component == "." || component == "..") return Refuse(PathRefusal::kDotComponent, why);
    if (component.size() > kMaxComponentBytes) return Refuse(PathRefusal::kComponentTooLong, why);
    for (const char c : component)
      if (BadNameCharacter(c)) return Refuse(PathRefusal::kDrive, why);
    // Windows can't hold such a name, and FAT-family drivers may drop the dot or space: ".playden-files." would then
    // pass the check below and land on Playden's own file.
    if (component.back() == '.' || component.back() == ' ') return Refuse(PathRefusal::kDrive, why);
    if (start == 0 && IsReservedName(component)) return Refuse(PathRefusal::kReserved, why);
    if (slash == std::string::npos) break;
    start = slash + 1;
  }
  return path;
}

std::optional<std::string> PlaceFile(std::string_view path, std::uint64_t product, bool support, PathRefusal* why) {
  if (why) *why = PathRefusal::kNone;
  if (!support) return std::string(path);
  if (path.size() > 4 && FoldCase(path.substr(0, 4)) == "app/") return NormalizePath(path.substr(4), why);
  auto placed = ".playden-gog/support/" + std::to_string(product) + "/" + std::string(path);
  if (placed.size() > kMaxPathBytes) return Refuse(PathRefusal::kTooLong, why);
  return placed;
}

PathSet::Added PathSet::Add(std::string_view raw, PathKind kind, std::uint64_t product, bool support,
                            std::size_t index) {
  Added added;
  if (kind == PathKind::kLink) {
    added.refusal = PathRefusal::kLink;
    return added;
  }
  auto normalized = NormalizePath(raw, &added.refusal);
  if (!normalized) return added;
  auto placed = PlaceFile(*normalized, product, support, &added.refusal);
  if (!placed) return added;
  std::string path = std::move(*placed);
  const auto key = FoldCase(path);

  // Check everything before recording anything: a refused path leaves no folder spelling behind.
  const auto own = entries_.find(key);
  if (own != entries_.end() && (own->second.kind == PathKind::kFile) != (kind == PathKind::kFile)) {
    added.refusal = PathRefusal::kClash;
    return added;
  }
  if (kind == PathKind::kFile && folders_.count(key)) {
    added.refusal = PathRefusal::kClash;
    return added;
  }
  // The folders above the entry, and a folder entry's own path.
  const auto folders_end = kind == PathKind::kDirectory ? path.size() : path.rfind('/');
  std::vector<std::size_t> ends;
  if (folders_end != std::string::npos) {
    for (std::size_t slash = path.find('/'); slash != std::string::npos && slash < folders_end;
         slash = path.find('/', slash + 1))
      ends.push_back(slash);
    ends.push_back(folders_end);
  }
  for (const auto end : ends) {
    const auto found = entries_.find(key.substr(0, end));
    if (found != entries_.end() && found->second.kind == PathKind::kFile) {
      added.refusal = PathRefusal::kClash;
      return added;
    }
  }

  // Each folder keeps the first spelling seen; only ASCII letters change case, so the length is the same.
  for (const auto end : ends) {
    const auto found = folders_.emplace(key.substr(0, end), path.substr(0, end)).first;
    if (path.compare(0, end, found->second) != 0) path.replace(0, end, found->second);
  }
  if (own != entries_.end()) {
    added.replaced = own->second.index;
    own->second.index = index;
  } else {
    entries_.emplace(key, Entry{kind, index});
  }
  added.path = std::move(path);
  return added;
}

}  // namespace playden::gog
