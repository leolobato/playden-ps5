#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace playden::runtime {

// The publisher pack: the closed helper programs Rockstar Games' Steam games need for their
// account. Playden doesn't ship them: they live at <pack_root>/<version>/ (release.json and the
// archive), put there by tools/console_publisher_upload.py, the builds page's Install button, or
// Playden itself, which downloads the pinned archive from builds.playden.club when the
// publisher-pack-download feature is on (runtime/publisher_pack_install.h). Each Playden build
// accepts exactly one pack version, pinned below: one helper release, whose tree hash is computed
// as the macOS Playden's PublisherAssetCache does.
enum class Publisher { kRockstar };
inline constexpr Publisher kPublishers[] = {Publisher::kRockstar};

struct PublisherRelease {
  Publisher publisher = Publisher::kRockstar;
  std::string archive;      // file name, e.g. playden-rgschost-20261121.1.tzst
  std::string sha256;       // of the archive file
  std::string tree_sha256;  // PublisherTreeDigest of the unpacked files
  std::vector<std::string> required;  // files that must be at the unpacked root
  std::uint64_t size = 0;             // of the archive file, for the download (0: not pinned)
};

inline constexpr const char* kPublisherPackVersion = "20261121.1";
inline constexpr const char* kPublisherPackRoot = "/data/playden/publisher";
// Playden's own publisher files: the unpacked archives in <work root>/cache, the publishers' runtimes.
inline constexpr const char* kPublisherWorkRoot = "/data/playden/publisher-work";
inline constexpr const char* kPublisherCacheRoot = "/data/playden/publisher-work/cache";
const std::vector<PublisherRelease>& PinnedPublisherReleases();
// The pin of one publisher's archive: every publisher has one.
const PublisherRelease& PinnedPublisherRelease(Publisher publisher);
const char* PublisherName(Publisher publisher);  // "Rockstar"

// SHA-256 over each file's relative path, a zero byte, its hex SHA-256 and a zero byte, in
// byte order of the paths: the macOS PublisherAssetCache.treeDigest.
std::string PublisherTreeDigest(const std::map<std::string, std::string>& file_sha256);

struct PublisherPackStatus {
  bool present = false;          // a pack of any version is on the console
  bool version_matches = false;  // and it is the version this build pins
  std::string found_version;     // the version found, when another one
  std::map<Publisher, bool> verified;  // archive present and its hash matches the pin
  std::string message;           // for Settings and logs
};

// Reads <pack_root>/<version>/release.json and checks each pinned archive's hash against both
// the pin and release.json. Hashes every archive (tens of MB): call it on a worker.
PublisherPackStatus CheckPublisherPack(const std::string& pack_root);
PublisherPackStatus CheckPublisherPack(const std::string& pack_root, const std::string& version,
                                       const std::vector<PublisherRelease>& releases);

// Whether a publisher's games can use the pack: the pinned version, with its archive verified.
bool PublisherSupported(const PublisherPackStatus& status, Publisher publisher);

// Unpacks a verified archive into <cache_root>/<archive name without .tzst>/ (through a staging
// folder renamed last), or reuses a cache that is still valid. The unpacked files must match the
// pinned tree hash. Leftover staging folders are removed first.
bool PreparePublisherRelease(const std::string& pack_root, const std::string& cache_root,
                             const PublisherRelease& release, std::string* directory, std::string* reason);
// The version folder the archives are read from; tests pass their own version through the pins.
bool PreparePublisherRelease(const std::string& pack_root, const std::string& version, const std::string& cache_root,
                             const PublisherRelease& release, std::string* directory, std::string* reason);

// The cache was unpacked from this archive and every file still matches the recorded hashes,
// with nothing added or missing.
bool PublisherCacheValid(const std::string& directory, const PublisherRelease& release);

}  // namespace playden::runtime
