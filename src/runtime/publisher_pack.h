#pragma once

#include <map>
#include <string>
#include <vector>

namespace playden::runtime {

// The publisher pack: the closed helper programs some Steam games need for their publisher's
// account (EA, Rockstar Games, Ubisoft Connect), plus the Steam client host they start
// through. Playden never ships or downloads them: the player uploads the pack to
// <pack_root>/<version>/ (tools/console_publisher_upload.py), and each Playden build accepts
// exactly one pack version, pinned below: one helper release, whose tree hashes are computed as
// the macOS Playden's PublisherAssetCache does.
enum class Publisher { kSteamHost, kEa, kRockstar, kUbisoft };

struct PublisherRelease {
  Publisher publisher = Publisher::kSteamHost;
  std::string archive;      // file name, e.g. playden-ubisoft-20261003.11.tzst
  std::string sha256;       // of the archive file
  std::string tree_sha256;  // PublisherTreeDigest of the unpacked files
  std::vector<std::string> required;  // files that must be at the unpacked root
};

inline constexpr const char* kPublisherPackVersion = "20261120.3";
inline constexpr const char* kPublisherPackRoot = "/data/playden/publisher";
const std::vector<PublisherRelease>& PinnedPublisherReleases();
const char* PublisherName(Publisher publisher);  // "Steam host", "EA", "Rockstar", "Ubisoft"

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

// Whether a publisher's games can use the pack: its own archive and the Steam host's verified.
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
