// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// GOG's gen 1 content system (GOG_PROTOCOL.md §5): a build's repository.json (plain JSON at
// `…/content-system/v1/manifests/<product>/windows/<timestamp>/repository.json`), which depots an install takes, and
// the depot manifests (`{"version":1,"depot":{"name":…,"files":[…]}}`, plain JSON), whose files are byte ranges of a
// product's `main.bin`. Pure parsing and selection: no network, no files.

#include "gog/depot_reader.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace playden::gog {

// A repository is a few KiB: Monkey Island 2's is 2 KiB with six depots.
inline constexpr std::size_t kMaxRepositoryV1Bytes = 1024 * 1024;
inline constexpr std::size_t kMaxRepositoryV1Values = 65'536;
inline constexpr std::size_t kMaxRepositoryV1Depth = 16;

// A depot of game files.
struct RepositoryDepotV1 {
  std::vector<std::string> languages;  // English names ("English") or "Neutral" (every language)
  std::vector<std::uint64_t> products; // gameIDs: never empty
  std::vector<std::string> systems;    // empty when absent
  std::string manifest;                // "<uuid>.json": safe in a URL and a file name
  std::uint64_t size = 0;
  // Set by SelectDepotsV1 (0 in a parsed repository): the first of `products` the install takes, which the depot
  // was selected for. Its files' source when a record has no `url`, and the product of its support files. The
  // manifest's URL still names products[0], as GOG serves it.
  std::uint64_t selected_product = 0;
};

// A redistributable the game needs, by the dependency store's ID.
struct RedistV1 {
  std::string id;          // ValidDependencyId ("MSVC2008", "language_setup")
  std::string executable;  // "__redist/MSVC2008/vcredist_x86.exe"; empty when absent (language_setup)
  std::string argument;
  std::string target_dir;
};

struct RepositoryV1 {
  std::uint64_t root_product = 0;
  std::uint64_t timestamp = 0;
  std::string install_directory;
  std::vector<std::uint64_t> products;  // gameIDs[].gameID, repository order
  std::vector<RepositoryDepotV1> depots;
  std::vector<RedistV1> redists;
  std::size_t support_commands = 0;  // counted only
  std::size_t skipped = 0;           // malformed depots and gameIDs entries
  std::size_t bad_manifests = 0;     // file depots dropped for a manifest name that isn't `[A-Za-z0-9-]{1,64}.json`
};

// A manifest name a gen 1 depot may give: 1-64 of [A-Za-z0-9-] then ".json". It comes from the server and ends up in a
// URL and a file name.
bool ValidManifestNameV1(std::string_view name);

// repository.json. IDs and sizes may be strings or integers; IDs must be canonical, sizes decimal up to
// kMaxContentBytes. A depot with `redist` is a redistributable; any other is a file depot. A malformed entry is skipped
// (and counted); a root of the wrong shape, a version other than 1, or a missing or bad rootGameID or timestamp gives
// nothing.
std::optional<RepositoryV1> ParseRepositoryV1(std::string_view body);

struct SelectionV1 {
  std::vector<RepositoryDepotV1> depots;
  std::string language;  // the GOG code installed ("en-US")
  std::vector<std::uint64_t> products;
  std::vector<std::string> dependencies;  // the redist IDs, once each, in repository order
  std::uint64_t size = 0;
  std::uint64_t compressed_size = 0;  // gen 1 files are stored whole: the same as size
};

// The depots a gen 1 install takes.
// - Products: the root, then each other gameIDs entry in `owned`, in repository order.
// - Depots: file depots naming one of those products (the first one named is the depot's selected_product), whose systems are absent or include "Windows", and whose
//   languages hold "Neutral" or the chosen language (ignoring case).
// - Language: the requested one when a Windows depot of the root carries it, else English, else the first language
//   of those depots that the table knows or that is a PlainLanguageCode. The table's GOG code is recorded for a name
//   it knows ("English" -> "en-US"); when every such depot is Neutral, the requested language's code, or en-US.
// - Dependencies: every redist depot's ID. Redist depots are never file depots.
SelectionV1 SelectDepotsV1(const RepositoryV1& repository, const std::set<std::uint64_t>& owned,
                           std::string_view steam_code);

// The gen 1 element parser for DepotItemReader (array key "files"):
// - `{path, size, offset, hash, url?, support?, executable?}` is a file: hash MD5 hex, size up to kMaxFileBytes,
//   offset + size within 64 bits, and url's first component a canonical product ID (blob_product).
// - `{directory: true, path}` is a folder.
// - A record with `symlinkType` or `target`, or with no size, is a link (refused later, as a DepotLink is).
// Every record needs a path; a file without hash or offset is malformed.
bool ParseDepotItemV1(const steam::json::Value& element, DepotItem* item);

// A reader for a gen 1 depot manifest: depot.files through ParseDepotItemV1, with DepotItemReader's caps. The bytes
// fed are the plain manifest.
DepotItemReader DepotItemReaderV1(DepotItemReader::Callback callback);

}  // namespace playden::gog
