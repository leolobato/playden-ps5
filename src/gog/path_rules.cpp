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

// A two-byte UTF-8 code point's uppercase, as the exFAT default upcase table (Unicode's simple uppercase) gives it,
// for Latin-1, Latin Extended-A, basic Greek and Cyrillic; any other code point is its own.
char32_t Upper(char32_t c) {
  if ((c >= 0xE0 && c <= 0xFE && c != 0xF7)) return c - 0x20;
  if (c == 0xFF) return 0x178;
  if (c == 0xB5) return 0x39C;  // micro sign: Greek capital mu
  if (c >= 0x100 && c <= 0x17F) {
    if (c == 0x131) return 'I';  // dotless i
    if (c == 0x17F) return 'S';  // long s
    const bool odd_lower = (c <= 0x137 && c != 0x130) || (c >= 0x14A && c <= 0x177);
    const bool even_lower = (c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E);
    if ((odd_lower && (c & 1)) || (even_lower && !(c & 1))) return c - 1;
    return c;
  }
  if ((c >= 0x3B1 && c <= 0x3C1) || (c >= 0x3C3 && c <= 0x3CB)) return c - 0x20;
  if (c == 0x3C2) return 0x3A3;  // final sigma
  if (c == 0x3AC) return 0x386;
  if (c >= 0x3AD && c <= 0x3AF) return c - 0x25;
  if (c == 0x3CC) return 0x38C;
  if (c == 0x3CD || c == 0x3CE) return c - 0x3F;
  if (c >= 0x430 && c <= 0x44F) return c - 0x20;
  if (c >= 0x450 && c <= 0x45F) return c - 0x50;
  return c;
}

// The lowercase that stands for an uppercase code point in a key (Upper's inverse, and ASCII).
char32_t LowerOfUpper(char32_t c) {
  if (c >= 'A' && c <= 'Z') return c + 0x20;
  if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return c + 0x20;
  if (c == 0x178) return 0xFF;
  if (c >= 0x100 && c <= 0x17F) {
    const bool even_upper = (c <= 0x137 && c != 0x130) || (c >= 0x14A && c <= 0x177);
    const bool odd_upper = (c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E);
    if ((even_upper && !(c & 1)) || (odd_upper && (c & 1))) return c + 1;
    return c;
  }
  if ((c >= 0x391 && c <= 0x3A1) || (c >= 0x3A3 && c <= 0x3AB)) return c + 0x20;
  if (c == 0x386) return 0x3AC;
  if (c >= 0x388 && c <= 0x38A) return c + 0x25;
  if (c == 0x38C) return 0x3CC;
  if (c == 0x38E || c == 0x38F) return c + 0x3F;
  if (c >= 0x410 && c <= 0x42F) return c + 0x20;
  if (c >= 0x400 && c <= 0x40F) return c + 0x50;
  return c;
}

void AppendUtf8(char32_t c, std::string& out) {
  if (c < 0x80) {
    out += static_cast<char>(c);
  } else {
    out += static_cast<char>(0xC0 | (c >> 6));
    out += static_cast<char>(0x80 | (c & 0x3F));
  }
}

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
  std::string folded;
  folded.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    const auto lead = static_cast<unsigned char>(text[i]);
    const auto next = i + 1 < text.size() ? static_cast<unsigned char>(text[i + 1]) : 0;
    if (lead >= 0xC2 && lead <= 0xDF && (next & 0xC0) == 0x80) {
      const char32_t code = (static_cast<char32_t>(lead & 0x1F) << 6) | (next & 0x3F);
      AppendUtf8(LowerOfUpper(Upper(code)), folded);
      ++i;
      continue;
    }
    // ASCII, longer sequences and stray bytes as they are (ASCII lowercased).
    folded += Lower(text[i]);
  }
  return folded;
}

bool IsReservedName(std::string_view name) {
  // Every ".playden…" name, not only today's: no non-ASCII letter upcases to one of these, so the ASCII fold is enough,
  // and a name such as ".playden-fıles" (dotless i) that exFAT may take for Playden's own file is refused too.
  constexpr std::string_view kPrefix = ".playden";
  if (name.size() < kPrefix.size()) return false;
  std::string start(name.substr(0, kPrefix.size()));
  for (char& c : start) c = Lower(c);
  if (start == kPrefix) return true;
  for (const char* reserved : kReserved)
    if (FoldCase(name) == reserved) return true;
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

std::optional<std::string> PlaceFileV1(std::string_view path, std::uint64_t product, bool support, PathRefusal* why) {
  if (support) {
    const std::string prefix = std::to_string(product) + "/";
    if (path.size() > prefix.size() && path.compare(0, prefix.size(), prefix) == 0)
      return PlaceFile(path.substr(prefix.size()), product, support, why);
  }
  return PlaceFile(path, product, support, why);
}

bool IsRedistPlacement(std::string_view path, PathRefusal* why) {
  if (why) *why = PathRefusal::kNone;
  constexpr std::string_view kPrefix = kRedistFolder;
  if (path.size() <= kPrefix.size() || path.compare(0, kPrefix.size(), kPrefix) != 0) {
    if (why) *why = PathRefusal::kReserved;
    return false;
  }
  if (path.size() > kMaxPathBytes) {
    if (why) *why = PathRefusal::kTooLong;
    return false;
  }
  const auto rest = path.substr(kPrefix.size());
  const auto normalized = NormalizePath(rest, why);
  if (!normalized) return false;
  if (*normalized != rest) {
    // A leading separator NormalizePath would drop: not a canonical placement.
    if (why) *why = PathRefusal::kAbsolute;
    return false;
  }
  return true;
}

PathSet::Added PathSet::Add(std::string_view raw, PathKind kind, std::uint64_t product, bool support,
                            std::size_t index, int generation) {
  Added added;
  if (kind == PathKind::kLink) {
    added.refusal = PathRefusal::kLink;
    return added;
  }
  auto normalized = NormalizePath(raw, &added.refusal);
  if (!normalized) return added;
  auto placed = generation == 1 ? PlaceFileV1(*normalized, product, support, &added.refusal)
                                 : PlaceFile(*normalized, product, support, &added.refusal);
  if (!placed) return added;
  return Merge(std::move(*placed), kind, index);
}

PathSet::Added PathSet::AddRedist(std::string_view placed, PathKind kind, std::size_t index) {
  Added added;
  if (kind == PathKind::kLink) {
    added.refusal = PathRefusal::kLink;
    return added;
  }
  if (!IsRedistPlacement(placed, &added.refusal)) return added;
  return Merge(std::string(placed), kind, index);
}

void PathSet::Seed(std::string_view path, PathKind kind) {
  if (kind == PathKind::kLink || path.empty()) return;
  const std::string spelling(path);
  const auto key = FoldCase(spelling);
  // The folders above it, then the path itself; the first seed of a key wins, as Add's first spelling does.
  if (key.size() == spelling.size())
    for (auto slash = spelling.find('/'); slash != std::string::npos; slash = spelling.find('/', slash + 1))
      seeds_.emplace(key.substr(0, slash), spelling.substr(0, slash));
  seeds_.emplace(key, spelling);
}

PathSet::Added PathSet::Merge(std::string path, PathKind kind, std::size_t index) {
  Added added;
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

  // Each folder keeps the first spelling seen, or the installed one when seeded; only ASCII letters change case, so
  // the length is the same.
  for (const auto end : ends) {
    const auto folder_key = key.substr(0, end);
    auto spelling = path.substr(0, end);
    if (const auto seeded = seeds_.find(folder_key); seeded != seeds_.end() && seeded->second.size() == end)
      spelling = seeded->second;
    const auto found = folders_.emplace(folder_key, std::move(spelling)).first;
    if (path.compare(0, end, found->second) != 0) path.replace(0, end, found->second);
  }
  // A file's own name in its installed spelling.
  if (kind == PathKind::kFile) {
    const auto seeded = seeds_.find(key);
    if (seeded != seeds_.end() && seeded->second.size() == path.size()) {
      const auto name = path.rfind('/') == std::string::npos ? 0 : path.rfind('/') + 1;
      path.replace(name, std::string::npos, seeded->second, name, std::string::npos);
    }
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
