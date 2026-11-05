#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Bounded Steam depot manifest decoder. The CDN serves a zip whose first entry is a sectioned binary:
// repeated [magic u32 LE][length u32 LE][blob] sections (protobuf payload, protobuf metadata, signature),
// closed by an end magic. Field numbers follow fixtures/steam/proto/content_manifest.proto and the pinned
// reference CDN.swift / ResumableDepotDownload.swift.
namespace playden::steam {

// An inflated depot manifest. Manifests compress little (chunk hashes, encrypted file names), so a zip at the
// download cap (CdnClient::kMaxManifestDownloadBytes, 64 MiB) can inflate to about twice its size.
inline constexpr std::size_t kMaxManifestBytes = 128 * 1024 * 1024;
// What UnzipFirstEntry unpacks unless the caller says otherwise (a Cloud save).
inline constexpr std::size_t kMaxUnzipBytes = 64 * 1024 * 1024;
inline constexpr std::size_t kMaxManifestFiles = 500'000;
inline constexpr std::size_t kMaxManifestChunks = 4'000'000;

inline constexpr std::uint32_t kManifestPayloadMagic = 0x71F617D0u;
inline constexpr std::uint32_t kManifestMetadataMagic = 0x1F4812BEu;
inline constexpr std::uint32_t kManifestSignatureMagic = 0x1B81B817u;
inline constexpr std::uint32_t kManifestEndMagic = 0x32C415ABu;

// EDepotFileFlag bits used by the reference.
inline constexpr std::uint32_t kDepotFileFlagExecutable = 32;
inline constexpr std::uint32_t kDepotFileFlagDirectory = 64;

struct ManifestChunk {
  std::array<std::uint8_t, 20> sha{};
  std::uint32_t crc = 0;
  std::uint64_t offset = 0;
  std::uint32_t original_size = 0;
  std::uint32_t compressed_size = 0;
};

struct ManifestFile {
  std::string path;  // backslashes already converted to '/' (once decrypted)
  std::uint64_t size = 0;
  std::uint32_t flags = 0;
  std::array<std::uint8_t, 20> sha_content{};
  std::string link_target;
  std::vector<ManifestChunk> chunks;  // sorted by offset

  bool is_directory() const { return (flags & kDepotFileFlagDirectory) != 0; }
  bool is_symlink() const { return !link_target.empty(); }
};

struct DepotManifest {
  std::uint32_t depot_id = 0;
  std::uint64_t gid = 0;
  bool filenames_encrypted = false;  // path/link_target hold base64 ciphertext until decrypted
  std::uint64_t original_size = 0;
  std::vector<ManifestFile> files;  // sorted by path (byte order) as parsed
};

// Parses the raw manifest bytes after zip extraction. Requires payload, metadata and end sections;
// the signature section is accepted but not verified. File names are left encrypted when the
// metadata says so; call DecryptManifestFilenames next.
bool ParseDepotManifest(const std::vector<std::uint8_t>& data, DepotManifest* out);

// Symmetric decrypt callback (Steam AES-256 scheme) so this file does not depend on a crypto library.
using SymmetricDecryptFn = bool (*)(const std::uint8_t* data, std::size_t size,
                                    const std::array<std::uint8_t, 32>& key,
                                    std::vector<std::uint8_t>* out);
// Decrypts encrypted file names and link targets in place (base64 -> decrypt -> trailing NULs
// stripped -> backslashes to '/'), re-sorts files by path and clears filenames_encrypted. No-op
// success when names are not encrypted.
bool DecryptManifestFilenames(DepotManifest* manifest, const std::array<std::uint8_t, 32>& key,
                              SymmetricDecryptFn decrypt);

// Port of the reference validateManifest: safe relative paths, no case-insensitive duplicates, no entry
// written through a file or link, contiguous in-bounds chunks, bounded symlink targets. `reason` is optional.
bool ValidateDepotManifest(const DepotManifest& manifest, std::string* reason);

// Raw-inflate callback (zip method 8); `expected` is the uncompressed size from the central directory.
using InflateRawFn = bool (*)(const std::uint8_t* data, std::size_t size, std::size_t expected,
                              std::vector<std::uint8_t>* out);
// Extracts the first zip entry (method 0 stored, or 8 deflate via `inflate`). Result is capped at
// max_bytes (kMaxManifestBytes for a depot manifest).
bool UnzipFirstEntry(const std::vector<std::uint8_t>& zip, InflateRawFn inflate,
                     std::vector<std::uint8_t>* out, std::size_t max_bytes = kMaxUnzipBytes);

}  // namespace playden::steam
