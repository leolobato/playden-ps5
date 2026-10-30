// program_icon.cpp - A Windows program's own icon, read from its PE resources.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "homebrew/program_icon.hpp"

#include "homebrew/artwork_backend.hpp"

#include <algorithm>
#include <cstring>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace playden::homebrew {
namespace {

constexpr std::uint32_t kGroupIconType = 14;  // RT_GROUP_ICON
constexpr std::uint32_t kIconType = 3;        // RT_ICON
constexpr std::size_t kMaxDirectoryEntries = 512;
constexpr std::size_t kMaxGroupBytes = 64 * 1024;
constexpr std::size_t kMaxSections = 96;
constexpr std::size_t kMaxIconAttempts = 8;

std::uint16_t U16(const unsigned char* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }
std::uint32_t U32(const unsigned char* p) {
  return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

// An absolute path none of whose components is a symbolic link (the game's folder could point elsewhere).
bool NoLinks(const std::string& path) {
  if (path.empty() || path.front() != '/') return false;
  for (std::size_t end = path.find('/', 1);; end = path.find('/', end + 1)) {
    struct stat info {};
    if (::lstat(path.substr(0, end).c_str(), &info) != 0 || S_ISLNK(info.st_mode)) return false;
    if (end == std::string::npos) return true;
  }
}

// Reads at file offsets, never past the end. Opened without blocking, so a FIFO in its place can't stall.
class File {
 public:
  explicit File(const std::string& path)
      : fd_(NoLinks(path) ? ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC) : -1) {
    struct stat info {};
    if (fd_ >= 0 && (::fstat(fd_, &info) != 0 || !S_ISREG(info.st_mode))) Close();
    if (fd_ >= 0) size_ = static_cast<std::uint64_t>(info.st_size);
  }
  ~File() { Close(); }
  File(const File&) = delete;
  File& operator=(const File&) = delete;

  bool Read(std::uint64_t offset, void* out, std::size_t count) const {
    if (fd_ < 0 || offset > size_ || count > size_ - offset) return false;
    auto* bytes = static_cast<unsigned char*>(out);
    for (std::size_t done = 0; done < count;) {
      const auto got = ::pread(fd_, bytes + done, count - done, static_cast<off_t>(offset + done));
      if (got <= 0) return false;
      done += static_cast<std::size_t>(got);
    }
    return true;
  }

 private:
  void Close() {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
  }
  int fd_ = -1;
  std::uint64_t size_ = 0;
};

struct Section {
  std::uint32_t address = 0, raw_size = 0, raw_offset = 0;
};

// The file offset of `count` bytes at an address relative to the image, inside one section's file data.
std::optional<std::uint64_t> FileOffset(const std::vector<Section>& sections, std::uint32_t rva, std::uint32_t count) {
  for (const auto& s : sections) {
    if (rva < s.address) continue;
    const std::uint64_t into = rva - s.address;
    if (into + count > s.raw_size) continue;
    return static_cast<std::uint64_t>(s.raw_offset) + into;
  }
  return std::nullopt;
}

struct Resources {
  const File& file;
  std::vector<Section> sections;
  std::uint32_t root = 0;  // the resource directory's address
  std::uint32_t size = 0;

  struct Entry {
    bool named = false;
    std::uint32_t id = 0;
    bool directory = false;
    std::uint32_t offset = 0;  // from the resource directory's start
  };

  // A directory's entries (named ones first, as Windows orders them), or nothing when it doesn't fit.
  std::optional<std::vector<Entry>> Directory(std::uint32_t offset) const {
    unsigned char head[16];
    if (offset > size || size - offset < sizeof(head)) return std::nullopt;
    const auto at = FileOffset(sections, root + offset, sizeof(head));
    if (!at || !file.Read(*at, head, sizeof(head))) return std::nullopt;
    const std::size_t count = static_cast<std::size_t>(U16(head + 12)) + U16(head + 14);
    if (count > kMaxDirectoryEntries || (size - offset - sizeof(head)) / 8 < count) return std::nullopt;
    std::vector<unsigned char> rows(count * 8);
    const auto rows_at = FileOffset(sections, root + offset + 16, static_cast<std::uint32_t>(rows.size()));
    if (count && (!rows_at || !file.Read(*rows_at, rows.data(), rows.size()))) return std::nullopt;
    std::vector<Entry> entries;
    for (std::size_t i = 0; i < count; ++i) {
      const std::uint32_t name = U32(&rows[i * 8]), data = U32(&rows[i * 8 + 4]);
      entries.push_back({(name & 0x80000000u) != 0, name & 0x7fffffffu, (data & 0x80000000u) != 0, data & 0x7fffffffu});
    }
    return entries;
  }

  // The data of a type's resource (`id`, or the first one when nothing is named), in its first language.
  std::optional<std::vector<unsigned char>> Data(std::uint32_t type, std::optional<std::uint32_t> id,
                                                 std::size_t limit) const {
    const auto types = Directory(0);
    if (!types) return std::nullopt;
    const auto type_entry = std::find_if(types->begin(), types->end(), [&](const Entry& entry) {
      return !entry.named && entry.id == type && entry.directory;
    });
    if (type_entry == types->end()) return std::nullopt;
    const auto names = Directory(type_entry->offset);
    if (!names) return std::nullopt;
    const auto name_entry = std::find_if(names->begin(), names->end(), [&](const Entry& entry) {
      return entry.directory && (!id || (!entry.named && entry.id == *id));
    });
    if (name_entry == names->end()) return std::nullopt;
    const auto languages = Directory(name_entry->offset);
    if (!languages || languages->empty() || languages->front().directory) return std::nullopt;
    // A data entry: the data's address relative to the image, then its size.
    const auto leaf = languages->front().offset;
    unsigned char row[8];
    if (leaf > size || size - leaf < sizeof(row)) return std::nullopt;
    const auto row_at = FileOffset(sections, root + leaf, sizeof(row));
    if (!row_at || !file.Read(*row_at, row, sizeof(row))) return std::nullopt;
    const std::uint32_t data_rva = U32(row), data_size = U32(row + 4);
    if (data_size == 0 || data_size > limit) return std::nullopt;
    const auto data_at = FileOffset(sections, data_rva, data_size);
    std::vector<unsigned char> bytes(data_size);
    if (!data_at || !file.Read(*data_at, bytes.data(), bytes.size())) return std::nullopt;
    return bytes;
  }
};

std::optional<Resources> OpenResources(const File& file) {
  unsigned char dos[64];
  if (!file.Read(0, dos, sizeof(dos)) || dos[0] != 'M' || dos[1] != 'Z') return std::nullopt;
  const std::uint32_t pe = U32(dos + 0x3c);
  unsigned char header[24];
  if (!file.Read(pe, header, sizeof(header)) || std::memcmp(header, "PE\0\0", 4) != 0) return std::nullopt;
  const std::uint16_t section_count = U16(header + 6), optional_size = U16(header + 20);
  if (section_count == 0 || section_count > kMaxSections || optional_size < 2) return std::nullopt;
  std::vector<unsigned char> optional(optional_size);
  if (!file.Read(pe + 24ull, optional.data(), optional.size())) return std::nullopt;
  const std::uint16_t magic = U16(optional.data());
  const std::size_t counted = magic == 0x10b ? 92 : magic == 0x20b ? 108 : 0;
  if (counted == 0 || optional.size() < counted + 4 || U32(&optional[counted]) < 3) return std::nullopt;
  const std::size_t resource_directory = counted + 4 + 2 * 8;
  if (optional.size() < resource_directory + 8) return std::nullopt;
  Resources resources{file, {}, U32(&optional[resource_directory]), U32(&optional[resource_directory + 4])};
  if (resources.root == 0 || resources.size < 16 || resources.root > 0xffffffffu - resources.size) return std::nullopt;
  std::vector<unsigned char> table(section_count * 40u);
  if (!file.Read(pe + 24ull + optional_size, table.data(), table.size())) return std::nullopt;
  for (std::size_t i = 0; i < section_count; ++i) {
    const auto* row = &table[i * 40];
    resources.sections.push_back({U32(row + 12), U32(row + 16), U32(row + 20)});
  }
  return resources;
}

// A BMP icon image: a BITMAPINFOHEADER, the palette, the colour rows, then a 1-bit transparency mask,
// both bottom-up, with a height counting the two.
std::optional<ArtworkImage> DecodeDib(const std::vector<unsigned char>& bytes) {
  if (bytes.size() < 40) return std::nullopt;
  const std::uint32_t header = U32(bytes.data());
  const auto width = static_cast<std::int32_t>(U32(bytes.data() + 4));
  const auto double_height = static_cast<std::int32_t>(U32(bytes.data() + 8));
  const std::uint16_t bits = U16(bytes.data() + 14);
  const std::uint32_t compression = U32(bytes.data() + 16);
  const std::uint32_t used = U32(bytes.data() + 32);
  if (header < 40 || header > bytes.size() || width <= 0 || double_height <= 0 || double_height % 2 != 0 ||
      static_cast<std::uint32_t>(width) > kMaxProgramIconSide ||
      static_cast<std::uint32_t>(double_height / 2) > kMaxProgramIconSide)
    return std::nullopt;
  // 32-bit icons may say BI_BITFIELDS with the usual BGRA layout.
  if (compression != 0 && !(compression == 3 && bits == 32)) return std::nullopt;
  if (bits != 1 && bits != 4 && bits != 8 && bits != 24 && bits != 32) return std::nullopt;
  const std::uint32_t w = static_cast<std::uint32_t>(width), h = static_cast<std::uint32_t>(double_height / 2);
  const std::size_t colours = bits <= 8 ? (used != 0 && used <= (1u << bits) ? used : (1u << bits)) : 0;
  const std::size_t masks = compression == 3 && header == 40 ? 12 : 0;
  const std::size_t palette = header + masks;
  const std::size_t stride = ((static_cast<std::size_t>(w) * bits + 31) / 32) * 4;
  const std::size_t mask_stride = ((w + 31) / 32) * 4;
  const std::size_t pixels = palette + colours * 4;
  if (pixels > bytes.size() || (bytes.size() - pixels) / stride < h) return std::nullopt;
  const std::size_t mask = pixels + stride * h;
  const bool has_mask = bytes.size() >= mask && (bytes.size() - mask) / mask_stride >= h;
  ArtworkImage image;
  image.width = w;
  image.height = h;
  image.rgba.resize(static_cast<std::size_t>(w) * h * 4);
  bool any_alpha = false;
  for (std::uint32_t y = 0; y < h; ++y) {
    const auto* row = &bytes[pixels + (h - 1 - y) * stride];
    auto* out = &image.rgba[static_cast<std::size_t>(y) * w * 4];
    for (std::uint32_t x = 0; x < w; ++x, out += 4) {
      const unsigned char* bgr = nullptr;
      unsigned char alpha = 255;
      if (bits == 32) {
        bgr = row + x * 4;
        alpha = bgr[3];
        any_alpha = any_alpha || alpha != 0;
      } else if (bits == 24) {
        bgr = row + x * 3;
      } else {
        const std::size_t bit = static_cast<std::size_t>(x) * bits;
        const unsigned index = (row[bit / 8] >> (8 - bits - bit % 8)) & ((1u << bits) - 1);
        if (index >= colours) return std::nullopt;
        bgr = &bytes[palette + index * 4];
      }
      out[0] = bgr[2];
      out[1] = bgr[1];
      out[2] = bgr[0];
      out[3] = alpha;
    }
  }
  // Without alpha in its colours (an older 32-bit icon too), the mask says what is transparent.
  if (bits == 32 && any_alpha) return image;
  for (std::uint32_t y = 0; y < h; ++y) {
    const auto* row = has_mask ? &bytes[mask + (h - 1 - y) * mask_stride] : nullptr;
    for (std::uint32_t x = 0; x < w; ++x)
      image.rgba[(static_cast<std::size_t>(y) * w + x) * 4 + 3] =
          row && (row[x / 8] >> (7 - x % 8)) & 1 ? 0 : 255;
  }
  return image;
}

}  // namespace

std::optional<ArtworkImage> ReadProgramIcon(const std::string& path) {
  File file(path);
  const auto resources = OpenResources(file);
  if (!resources) return std::nullopt;
  // GRPICONDIR: reserved, type 1, count, then 14-byte entries (width, height, colours, reserved, planes,
  // bits, bytes, the RT_ICON id); a width or height of 0 means 256.
  const auto group = resources->Data(kGroupIconType, std::nullopt, kMaxGroupBytes);
  if (!group || group->size() < 6 || U16(group->data() + 2) != 1) return std::nullopt;
  struct Choice {
    std::uint32_t area = 0, bits = 0, id = 0;
  };
  std::vector<Choice> choices;
  const std::size_t count = std::min<std::size_t>(U16(group->data() + 4), (group->size() - 6) / 14);
  for (std::size_t i = 0; i < count; ++i) {
    const auto* row = group->data() + 6 + i * 14;
    const std::uint32_t width = row[0] ? row[0] : 256, height = row[1] ? row[1] : 256;
    choices.push_back({width * height, U16(row + 6), U16(row + 12)});
  }
  std::stable_sort(choices.begin(), choices.end(), [](const Choice& a, const Choice& b) {
    return a.area != b.area ? a.area > b.area : a.bits > b.bits;
  });
  // The largest that decodes: a malformed image falls back to the next one, a few times at most.
  std::vector<std::uint32_t> tried;
  for (const auto& choice : choices) {
    if (tried.size() >= kMaxIconAttempts) break;
    if (std::find(tried.begin(), tried.end(), choice.id) != tried.end()) continue;
    tried.push_back(choice.id);
    const auto bytes = resources->Data(kIconType, choice.id, kMaxProgramIconBytes);
    if (!bytes) continue;
    static constexpr unsigned char kPng[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    std::optional<ArtworkImage> image;
    if (bytes->size() >= sizeof(kPng) && std::memcmp(bytes->data(), kPng, sizeof(kPng)) == 0) {
      // The size from its IHDR chunk first, so an oversized image is never decoded.
      const auto big_endian = [&](std::size_t at) {
        return (std::uint32_t{(*bytes)[at]} << 24) | (std::uint32_t{(*bytes)[at + 1]} << 16) |
               (std::uint32_t{(*bytes)[at + 2]} << 8) | (*bytes)[at + 3];
      };
      if (bytes->size() < 24 || std::memcmp(bytes->data() + 12, "IHDR", 4) != 0 || big_endian(16) == 0 ||
          big_endian(16) > kMaxProgramIconSide || big_endian(20) == 0 || big_endian(20) > kMaxProgramIconSide)
        continue;
      image = DecodeArtwork(std::vector<std::uint8_t>(bytes->begin(), bytes->end()));
    } else {
      image = DecodeDib(*bytes);
    }
    if (image && image->width <= kMaxProgramIconSide && image->height <= kMaxProgramIconSide) return image;
  }
  return std::nullopt;
}

std::optional<std::string> ProgramIconPath(const std::string& source) {
  const std::size_t prefix = std::strlen(kProgramIconScheme);
  if (source.size() <= prefix || source.compare(0, prefix, kProgramIconScheme) != 0) return std::nullopt;
  auto path = source.substr(prefix);
  // An absolute path to a .exe, with no "." or ".." component.
  if (path.front() != '/' || path.find('\0') != std::string::npos || path.size() < 5) return std::nullopt;
  std::string extension = path.substr(path.size() - 4);
  for (char& c : extension) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
  if (extension != ".exe") return std::nullopt;
  for (std::size_t start = 1; start <= path.size();) {
    const auto end = std::min(path.find('/', start), path.size());
    const auto part = std::string_view(path).substr(start, end - start);
    if (part.empty() || part == "." || part == "..") return std::nullopt;
    start = end + 1;
  }
  return path;
}

ProgramIconArtworkBackend::ProgramIconArtworkBackend(ArtworkBackend& next, std::string host_prefix)
    : next_(next), host_prefix_(std::move(host_prefix)) {
  while (!host_prefix_.empty() && host_prefix_.back() == '/') host_prefix_.pop_back();
}

std::optional<ArtworkImage> ProgramIconArtworkBackend::Load(const ArtworkKey& key, const std::atomic<bool>& cancel) {
  const auto path = ProgramIconPath(key.source);
  if (!path) return next_.Load(key, cancel);
  if (cancel.load(std::memory_order_acquire)) return std::nullopt;
  return ReadProgramIcon(host_prefix_ + *path);
}

}  // namespace playden::homebrew
