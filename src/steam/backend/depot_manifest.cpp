#include "steam/backend/depot_manifest.h"

#include <algorithm>
#include <limits>
#include <unordered_set>

#include "steam/backend/proto_wire.h"

namespace playden::steam {
namespace {

constexpr std::size_t kMaxStringBytes = 64 * 1024;

bool IsVarint(const proto::Field& f) { return f.wire_type == proto::WireType::kVarint; }
bool IsBytes(const proto::Field& f) { return f.wire_type == proto::WireType::kLengthDelimited; }
bool IsFixed32(const proto::Field& f) { return f.wire_type == proto::WireType::kFixed32; }

std::uint32_t ReadLE32(const std::uint8_t* d) {
  return static_cast<std::uint32_t>(d[0]) | static_cast<std::uint32_t>(d[1]) << 8 |
         static_cast<std::uint32_t>(d[2]) << 16 | static_cast<std::uint32_t>(d[3]) << 24;
}
std::uint32_t ReadLE16(const std::uint8_t* d) {
  return static_cast<std::uint32_t>(d[0]) | static_cast<std::uint32_t>(d[1]) << 8;
}

bool CopySha(const proto::Field& f, std::array<std::uint8_t, 20>* out) {
  if (!IsBytes(f) || f.size != out->size()) return false;
  std::copy(f.data, f.data + f.size, out->begin());
  return true;
}

bool ParseChunk(const proto::Field& field, proto::Reader& parent, ManifestChunk* chunk) {
  proto::Reader reader;
  if (!parent.Submessage(field, &reader)) return false;
  bool have_sha = false;
  proto::Field f;
  while (reader.Next(&f)) {
    switch (f.number) {
      case 1:
        if (!CopySha(f, &chunk->sha)) return false;
        have_sha = true;
        break;
      case 2:
        if (!IsFixed32(f)) return false;
        chunk->crc = static_cast<std::uint32_t>(f.value);
        break;
      case 3:
        if (!IsVarint(f)) return false;
        chunk->offset = f.value;
        break;
      case 4:
        if (!IsVarint(f) || f.value > std::numeric_limits<std::uint32_t>::max()) return false;
        chunk->original_size = static_cast<std::uint32_t>(f.value);
        break;
      case 5:
        if (!IsVarint(f) || f.value > std::numeric_limits<std::uint32_t>::max()) return false;
        chunk->compressed_size = static_cast<std::uint32_t>(f.value);
        break;
      default:
        break;
    }
  }
  return reader.ok() && have_sha;
}

bool ParseMapping(const proto::Field& field, proto::Reader& parent, ManifestFile* file,
                  std::size_t* total_chunks) {
  proto::Reader reader;
  if (!parent.Submessage(field, &reader)) return false;
  proto::Field f;
  while (reader.Next(&f)) {
    switch (f.number) {
      case 1:
        if (!IsBytes(f) || f.size > kMaxStringBytes) return false;
        file->path = f.AsString();
        break;
      case 2:
        if (!IsVarint(f)) return false;
        file->size = f.value;
        break;
      case 3:
        if (!IsVarint(f) || f.value > std::numeric_limits<std::uint32_t>::max()) return false;
        file->flags = static_cast<std::uint32_t>(f.value);
        break;
      case 5:
        // Empty content hash means "absent" (directories, links); anything else must be SHA-1.
        if (IsBytes(f) && f.size == 0) break;
        if (!CopySha(f, &file->sha_content)) return false;
        break;
      case 6: {
        if (!IsBytes(f) || *total_chunks >= kMaxManifestChunks) return false;
        ++*total_chunks;
        ManifestChunk chunk;
        if (!ParseChunk(f, reader, &chunk)) return false;
        file->chunks.push_back(chunk);
        break;
      }
      case 7:
        if (!IsBytes(f) || f.size > kMaxStringBytes) return false;
        file->link_target = f.AsString();
        break;
      default:
        break;
    }
  }
  if (!reader.ok()) return false;
  std::stable_sort(file->chunks.begin(), file->chunks.end(),
                   [](const ManifestChunk& a, const ManifestChunk& b) { return a.offset < b.offset; });
  return true;
}

bool ParsePayload(const std::uint8_t* data, std::size_t size, std::vector<ManifestFile>* files) {
  proto::DecodeLimits limits{size + 1, size + 1, kMaxManifestFiles + 16, 8};
  proto::Reader reader(data, size, limits);
  proto::Field f;
  std::size_t total_chunks = 0;
  while (reader.Next(&f)) {
    if (f.number != 1) continue;
    if (!IsBytes(f) || files->size() >= kMaxManifestFiles) return false;
    ManifestFile file;
    // Chunk limit applies per mapping reader via the same bounds.
    if (!ParseMapping(f, reader, &file, &total_chunks)) return false;
    files->push_back(std::move(file));
  }
  return reader.ok();
}

bool ParseMetadata(const std::uint8_t* data, std::size_t size, DepotManifest* out) {
  proto::DecodeLimits limits{size + 1, size + 1, 64, 4};
  proto::Reader reader(data, size, limits);
  proto::Field f;
  while (reader.Next(&f)) {
    switch (f.number) {
      case 1:
        if (!IsVarint(f) || f.value > std::numeric_limits<std::uint32_t>::max()) return false;
        out->depot_id = static_cast<std::uint32_t>(f.value);
        break;
      case 2:
        if (!IsVarint(f)) return false;
        out->gid = f.value;
        break;
      case 4:
        if (!IsVarint(f)) return false;
        out->filenames_encrypted = f.value != 0;
        break;
      case 5:
        if (!IsVarint(f)) return false;
        out->original_size = f.value;
        break;
      default:
        break;
    }
  }
  return reader.ok();
}

void NormalizeSlashes(std::string* s) { std::replace(s->begin(), s->end(), '\\', '/'); }

int Base64Value(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

// Strict standard base64 with padding. ASCII whitespace is skipped anywhere: Steam's encrypted names
// embed newlines (the reference ignores unknown characters, SteamKit's .NET decoder skips whitespace).
bool Base64Decode(const std::string& text, std::vector<std::uint8_t>* out) {
  out->clear();
  std::uint32_t acc = 0;
  int count = 0;
  int padding = 0;
  for (char c : text) {
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
    if (c == '=') {
      if (++padding > 2) return false;
      continue;
    }
    if (padding != 0) return false;  // data after padding
    const int v = Base64Value(c);
    if (v < 0) return false;
    acc = acc << 6 | static_cast<std::uint32_t>(v);
    if (++count == 4) {
      out->push_back(static_cast<std::uint8_t>(acc >> 16));
      out->push_back(static_cast<std::uint8_t>(acc >> 8));
      out->push_back(static_cast<std::uint8_t>(acc));
      acc = 0;
      count = 0;
    }
  }
  if (count == 0) return padding == 0;
  if (count == 1 || padding != 4 - count) return false;
  if (count == 2) {
    if (acc & 0xF) return false;
    out->push_back(static_cast<std::uint8_t>(acc >> 4));
  } else {
    if (acc & 0x3) return false;
    out->push_back(static_cast<std::uint8_t>(acc >> 10));
    out->push_back(static_cast<std::uint8_t>(acc >> 2));
  }
  return true;
}

bool DecryptName(std::string* name, const std::array<std::uint8_t, 32>& key,
                 SymmetricDecryptFn decrypt) {
  std::vector<std::uint8_t> cipher;
  if (!Base64Decode(*name, &cipher) || cipher.empty()) return false;
  std::vector<std::uint8_t> clear;
  if (!decrypt(cipher.data(), cipher.size(), key, &clear)) return false;
  while (!clear.empty() && clear.back() == 0) clear.pop_back();
  name->assign(clear.begin(), clear.end());
  NormalizeSlashes(name);
  return true;
}

bool Fail(std::string* reason, const std::string& text, const std::string& path = {}) {
  if (reason) *reason = path.empty() ? text : text + ": " + path;
  return false;
}

std::string Lower(const std::string& s) {
  std::string r = s;
  for (char& c : r)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return r;
}

bool HasControl(const std::string& s) {
  for (std::size_t i = 0; i < s.size(); ++i) {
    const auto c = static_cast<std::uint8_t>(s[i]);
    if (c < 0x20 || c == 0x7F) return true;
    // U+0080..U+009F are C1 controls (UTF-8: C2 80..C2 9F).
    if (c == 0xC2 && i + 1 < s.size() && static_cast<std::uint8_t>(s[i + 1]) >= 0x80 &&
        static_cast<std::uint8_t>(s[i + 1]) <= 0x9F)
      return true;
  }
  return false;
}

std::vector<std::string> Split(const std::string& s, char sep) {
  std::vector<std::string> parts;
  std::size_t start = 0;
  for (;;) {
    const std::size_t pos = s.find(sep, start);
    if (pos == std::string::npos) {
      parts.push_back(s.substr(start));
      return parts;
    }
    parts.push_back(s.substr(start, pos - start));
    start = pos + 1;
  }
}

// Reference DownloadWorkspace.path: no empty/./.. components, no ':', no control characters, and
// not inside the downloader's ".gn-download" state directory.
bool SafeRelativePath(const std::string& raw, std::string* normalized) {
  std::string path = raw;
  NormalizeSlashes(&path);
  if (path.empty() || HasControl(path)) return false;
  const auto parts = Split(path, '/');
  for (const auto& part : parts)
    if (part.empty() || part == "." || part == ".." || part.find(':') != std::string::npos)
      return false;
  if (Lower(parts[0]) == ".gn-download") return false;
  *normalized = std::move(path);
  return true;
}

// Old depots (GUN, 2006) name files "./data/..."; Steam treats "." segments as no-ops. Empty
// segments are kept so validation still rejects "/abs" and "a//b". A "." directory entry is the
// depot root itself, so it is dropped.
void CleanManifestNames(DepotManifest* manifest) {
  for (auto& file : manifest->files) {
    NormalizeSlashes(&file.path);
    NormalizeSlashes(&file.link_target);
    std::string clean;
    bool first = true;
    for (const auto& part : Split(file.path, '/')) {
      if (part == ".") continue;
      if (!first) clean += '/';
      clean += part;
      first = false;
    }
    file.path = std::move(clean);
  }
  auto& files = manifest->files;
  files.erase(std::remove_if(files.begin(), files.end(),
                             [](const ManifestFile& f) { return f.path.empty() && f.is_directory(); }),
              files.end());
}

}  // namespace

bool ParseDepotManifest(const std::vector<std::uint8_t>& data, DepotManifest* out) {
  if (!out || data.size() > kMaxManifestBytes) return false;
  DepotManifest manifest;
  bool have_payload = false, have_metadata = false, have_signature = false, have_end = false;
  std::size_t offset = 0;
  while (offset + 4 <= data.size()) {
    const std::uint32_t magic = ReadLE32(data.data() + offset);
    offset += 4;
    if (magic == kManifestEndMagic) {
      have_end = true;
      break;
    }
    if (data.size() - offset < 4) return false;
    const std::size_t length = ReadLE32(data.data() + offset);
    offset += 4;
    if (length > data.size() - offset) return false;
    const std::uint8_t* blob = data.data() + offset;
    offset += length;
    switch (magic) {
      case kManifestPayloadMagic:
        if (have_payload || !ParsePayload(blob, length, &manifest.files)) return false;
        have_payload = true;
        break;
      case kManifestMetadataMagic:
        if (have_metadata || !ParseMetadata(blob, length, &manifest)) return false;
        have_metadata = true;
        break;
      case kManifestSignatureMagic:
        if (have_signature) return false;
        have_signature = true;
        break;
      default:
        return false;
    }
  }
  if (!have_payload || !have_metadata || !have_end) return false;
  if (!manifest.filenames_encrypted) CleanManifestNames(&manifest);
  std::stable_sort(manifest.files.begin(), manifest.files.end(),
                   [](const ManifestFile& a, const ManifestFile& b) { return a.path < b.path; });
  *out = std::move(manifest);
  return true;
}

bool DecryptManifestFilenames(DepotManifest* manifest, const std::array<std::uint8_t, 32>& key,
                              SymmetricDecryptFn decrypt) {
  if (!manifest || !decrypt) return false;
  if (!manifest->filenames_encrypted) return true;
  for (auto& file : manifest->files) {
    if (!DecryptName(&file.path, key, decrypt)) return false;
    if (!file.link_target.empty() && !DecryptName(&file.link_target, key, decrypt)) return false;
  }
  manifest->filenames_encrypted = false;
  CleanManifestNames(manifest);
  std::stable_sort(manifest->files.begin(), manifest->files.end(),
                   [](const ManifestFile& a, const ManifestFile& b) { return a.path < b.path; });
  return true;
}

bool ValidateDepotManifest(const DepotManifest& manifest, std::string* reason) {
  if (manifest.filenames_encrypted) return Fail(reason, "manifest file names are still encrypted");
  if (manifest.files.size() > kMaxManifestFiles) return Fail(reason, "manifest has too many files");
  constexpr std::uint64_t kInt64Max = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  std::unordered_set<std::string> names;
  std::unordered_set<std::string> non_directories;
  std::vector<std::string> paths;
  paths.reserve(manifest.files.size());
  std::uint64_t total = 0;
  std::size_t chunk_count = 0;
  for (const auto& file : manifest.files) {
    std::string path;
    if (!SafeRelativePath(file.path, &path)) return Fail(reason, "invalid manifest path", file.path);
    const std::string folded = Lower(path);
    if (!names.insert(folded).second) return Fail(reason, "manifest has conflicting file names", path);
    if (file.size > kInt64Max) return Fail(reason, "manifest has invalid file metadata", path);
    if (!file.is_directory() && !file.is_symlink()) {
      chunk_count += file.chunks.size();
      if (chunk_count > kMaxManifestChunks) return Fail(reason, "manifest has too many chunks");
      std::uint64_t end = 0;
      auto chunks = file.chunks;
      std::stable_sort(chunks.begin(), chunks.end(),
                       [](const ManifestChunk& a, const ManifestChunk& b) { return a.offset < b.offset; });
      for (const auto& chunk : chunks) {
        if (chunk.original_size == 0 || chunk.offset != end || chunk.original_size > file.size - end)
          return Fail(reason, "manifest has overlapping or incomplete chunks", path);
        end += chunk.original_size;
      }
      if (end != file.size) return Fail(reason, "manifest has incomplete file ranges", path);
      if (total > std::numeric_limits<std::uint64_t>::max() - file.size)
        return Fail(reason, "manifest total size overflows");
      total += file.size;
    }
    if (file.is_symlink()) {
      std::string link = file.link_target;
      NormalizeSlashes(&link);
      if (link[0] == '/' || link.find(':') != std::string::npos || HasControl(link))
        return Fail(reason, "manifest link escapes destination", path);
      auto resolved = Split(path, '/');
      resolved.pop_back();
      for (const auto& part : Split(link, '/')) {
        if (part.empty() || part == ".") continue;
        if (part == "..") {
          if (resolved.empty()) return Fail(reason, "manifest link escapes destination", path);
          resolved.pop_back();
        } else {
          resolved.push_back(part);
        }
      }
      if (resolved.empty() || Lower(resolved.front()) == ".gn-download")
        return Fail(reason, "manifest link points into download state", path);
    }
    if (!file.is_directory()) non_directories.insert(folded);
    paths.push_back(folded);
  }
  // A manifest file or link cannot also be a parent directory of another entry.
  for (const auto& folded : paths) {
    std::size_t pos = 0;
    while ((pos = folded.find('/', pos)) != std::string::npos) {
      if (non_directories.count(folded.substr(0, pos)))
        return Fail(reason, "manifest writes through a file or link", folded);
      ++pos;
    }
  }
  return true;
}

bool UnzipFirstEntry(const std::vector<std::uint8_t>& zip, InflateRawFn inflate,
                     std::vector<std::uint8_t>* out, std::size_t max_bytes) {
  if (!out || zip.size() < 22 || zip.size() > max_bytes + (1u << 20)) return false;
  const std::uint8_t* d = zip.data();
  const std::size_t n = zip.size();
  // End of central directory (PK\x05\x06), scanning back over a trailing comment.
  std::size_t eocd = n;
  const std::size_t lowest = n > 22 + 0xFFFF ? n - 22 - 0xFFFF : 0;
  for (std::size_t i = n - 22 + 1; i-- > lowest;) {
    if (d[i] == 0x50 && d[i + 1] == 0x4B && d[i + 2] == 0x05 && d[i + 3] == 0x06) {
      eocd = i;
      break;
    }
  }
  if (eocd == n) return false;
  if (ReadLE16(d + eocd + 10) == 0) return false;  // no entries
  const std::size_t cd = ReadLE32(d + eocd + 16);
  if (cd > n || n - cd < 46 || ReadLE32(d + cd) != 0x02014B50u) return false;
  const std::uint32_t flags = ReadLE16(d + cd + 8);
  const std::uint32_t method = ReadLE16(d + cd + 10);
  const std::size_t csize = ReadLE32(d + cd + 20);
  const std::size_t usize = ReadLE32(d + cd + 24);
  const std::size_t local = ReadLE32(d + cd + 42);
  if (flags & 1) return false;  // encrypted entry
  if (csize == 0xFFFFFFFFu || usize == 0xFFFFFFFFu || local == 0xFFFFFFFFu) return false;  // zip64
  if (usize > max_bytes) return false;
  if (local > n || n - local < 30 || ReadLE32(d + local) != 0x04034B50u) return false;
  const std::size_t start = local + 30 + ReadLE16(d + local + 26) + ReadLE16(d + local + 28);
  if (start > n || csize > n - start) return false;
  if (method == 0) {
    if (csize != usize) return false;
    out->assign(d + start, d + start + csize);
    return true;
  }
  if (method == 8 && inflate) {
    std::vector<std::uint8_t> result;
    if (!inflate(d + start, csize, usize, &result) || result.size() != usize) return false;
    *out = std::move(result);
    return true;
  }
  return false;
}

}  // namespace playden::steam
