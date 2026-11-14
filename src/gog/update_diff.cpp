// SPDX-License-Identifier: GPL-3.0-or-later
#include "gog/update_diff.h"

#include "gog/hash.h"
#include "gog/path_rules.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <memory>
#include <unordered_map>

namespace playden::gog {
namespace {

constexpr std::size_t kHashBufferBytes = 1024 * 1024;

using Md5Bytes = std::array<std::uint8_t, 16>;
using Sha256Bytes = std::array<std::uint8_t, 32>;

int HexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

template <std::size_t N>
bool FromHex(std::string_view hex, std::array<std::uint8_t, N>* out) {
  if (hex.size() != N * 2) return false;
  for (std::size_t i = 0; i < N; ++i) {
    const int high = HexValue(hex[2 * i]), low = HexValue(hex[2 * i + 1]);
    if (high < 0 || low < 0) return false;
    (*out)[i] = static_cast<std::uint8_t>((high << 4) | low);
  }
  return true;
}

// What SameContent compares, kept compact: an old plan's files stay in memory for the whole diff.
struct Print {
  enum : std::uint8_t { kMd5 = 1, kSha256 = 2, kContent = 4, kChunks = 8 };
  std::uint64_t size = 0;
  std::uint8_t known = 0;
  bool seen = false;  // the new plan has this key
  Md5Bytes md5{};
  Md5Bytes content{};
  Md5Bytes chunks{};  // the MD5 of the chunk list: "<md5>:<size>\n" per chunk
  Sha256Bytes sha256{};
};

Print PrintOf(const PlanEntry& entry) {
  Print print;
  print.size = entry.size;
  if (!entry.md5.empty() && FromHex(entry.md5, &print.md5)) print.known |= Print::kMd5;
  if (!entry.sha256.empty() && FromHex(entry.sha256, &print.sha256)) print.known |= Print::kSha256;
  const auto content = ContentMd5(entry);
  if (!content.empty() && FromHex(content, &print.content)) print.known |= Print::kContent;
  if (!entry.units.chunks.empty()) {
    Md5 hash;
    for (const auto& chunk : entry.units.chunks) {
      const auto line = chunk.md5 + ":" + std::to_string(chunk.size) + "\n";
      hash.Update(line.data(), line.size());
    }
    if (FromHex(hash.FinalHex(), &print.chunks)) print.known |= Print::kChunks;
  }
  return print;
}

bool Same(const Print& old_print, const Print& new_print) {
  if (old_print.size != new_print.size) return false;
  if (old_print.size == 0) return true;
  const auto both = [&](std::uint8_t bit) { return (old_print.known & bit) && (new_print.known & bit); };
  if (both(Print::kMd5)) return old_print.md5 == new_print.md5;
  if (both(Print::kSha256)) return old_print.sha256 == new_print.sha256;
  if (both(Print::kContent)) return old_print.content == new_print.content;
  if (both(Print::kChunks)) return old_print.chunks == new_print.chunks;
  return false;
}

// A regular file (never a link) of exactly `size` bytes.
bool PresentWithSize(const std::string& path, std::uint64_t size) {
  struct stat info {};
  return lstat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode) && static_cast<std::uint64_t>(info.st_size) == size;
}

enum class HashKind { kOk, kFailed, kCanceled };

// The file's MD5 through a heap buffer, cancel checked before every block.
HashKind HashFile(const std::string& path, const std::atomic<bool>& cancel, std::string* md5) {
  const int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) return HashKind::kFailed;
  std::unique_ptr<std::uint8_t[]> buffer(new std::uint8_t[kHashBufferBytes]);
  Md5 hash;
  HashKind kind = HashKind::kOk;
  for (;;) {
    if (cancel.load()) {
      kind = HashKind::kCanceled;
      break;
    }
    const auto got = read(fd, buffer.get(), kHashBufferBytes);
    if (got < 0) {
      if (errno == EINTR) continue;
      kind = HashKind::kFailed;
      break;
    }
    if (got == 0) break;
    hash.Update(buffer.get(), static_cast<std::size_t>(got));
  }
  close(fd);
  if (kind == HashKind::kOk) *md5 = hash.FinalHex();
  return kind;
}

}  // namespace

std::string ContentMd5(const PlanEntry& entry) {
  if (!entry.md5.empty()) return entry.md5;
  if (entry.units.chunks.size() == 1 && entry.units.chunks.front().size == entry.size)
    return entry.units.chunks.front().md5;
  return std::string();
}

bool SameContent(const PlanEntry& old_entry, const PlanEntry& new_entry) {
  return Same(PrintOf(old_entry), PrintOf(new_entry));
}

DiffResult DiffPlans(const std::string& old_plan, const std::string& new_plan, const std::string& root,
                     const std::atomic<bool>& cancel,
                     const std::function<void(std::uint64_t index, const PlanEntry&, bool changed)>& each) {
  DiffResult result;
  std::unordered_map<std::string, Print> old_files;
  PlanEntry entry;
  {
    EntryReader reader(old_plan);
    PlanHeader header;
    bool ok = reader.open() && reader.Header(&header);
    while (ok) {
      if (cancel.load()) {
        result.canceled = true;
        return result;
      }
      const auto next = reader.Read(&entry);
      if (next == EntryReader::Next::kEnd) break;
      if (next == EntryReader::Next::kBad) {
        ok = false;
        break;
      }
      if (!entry.directory) old_files[FoldCase(entry.path)] = PrintOf(entry);
    }
    // An old plan that doesn't read whole is no plan: the disk files are hashed instead.
    if (!ok) old_files.clear();
    result.old_plan = ok;
  }

  EntryReader reader(new_plan);
  PlanHeader header;
  if (!reader.open() || !reader.Header(&header)) return result;
  result.install_bytes = header.total_size;
  for (std::uint64_t index = 0;; ++index) {
    if (cancel.load()) {
      result.canceled = true;
      return result;
    }
    const auto next = reader.Read(&entry);
    if (next == EntryReader::Next::kEnd) break;
    if (next == EntryReader::Next::kBad) return result;
    if (entry.directory) {
      if (each) each(index, entry, false);
      continue;
    }
    bool changed = true;
    const auto path = root + "/" + entry.path;
    const bool present = PresentWithSize(path, entry.size);
    if (result.old_plan) {
      const auto found = old_files.find(FoldCase(entry.path));
      if (found != old_files.end()) {
        found->second.seen = true;
        changed = !(present && Same(found->second, PrintOf(entry)));
      }
    } else if (present) {
      if (entry.size == 0) {
        changed = false;
      } else if (const auto content = ContentMd5(entry); !content.empty()) {
        ++result.hashed_files;
        std::string md5;
        const auto hashed = HashFile(path, cancel, &md5);
        if (hashed == HashKind::kCanceled) {
          result.canceled = true;
          return result;
        }
        changed = !(hashed == HashKind::kOk && md5 == content);
      }
    }
    if (changed) {
      ++result.changed_files;
      result.staged_bytes += entry.size;
      result.download_bytes += entry.download();
    } else {
      ++result.unchanged_files;
    }
    if (each) each(index, entry, changed);
  }
  for (const auto& old_file : old_files)
    if (!old_file.second.seen) ++result.removed_files;
  result.readable = true;
  return result;
}

std::uint64_t UpdateTarget(std::string_view build_id, int generation, std::vector<std::uint64_t> products) {
  std::sort(products.begin(), products.end());
  products.erase(std::unique(products.begin(), products.end()), products.end());
  std::string text = "gog\n" + std::to_string(generation) + "\n" + std::string(build_id) + "\n";
  for (std::size_t index = 0; index < products.size(); ++index)
    text += (index == 0 ? "" : ",") + std::to_string(products[index]);
  Sha256 sha;
  sha.Update(text.data(), text.size());
  const auto hex = sha.FinalHex();
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < 16; ++i) value = (value << 4) | static_cast<std::uint64_t>(HexValue(hex[i]));
  return value == 0 ? 1 : value;
}

}  // namespace playden::gog
