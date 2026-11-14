// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// GOG's content system for Windows builds (GOG_PROTOCOL.md §4-5): the build list, CDN endpoints and their URLs, the gen 2
// build meta, which depots an install takes, and the public dependency store. Gen 1 repositories are in manifest_v1.h.
// Pure parsing and selection: no network, no files. The downloader and the details source build on it.

#include "steam/backend/json.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace playden::gog {

// A builds answer is a page of 1-10 items with their URLs: a few KiB.
inline constexpr std::size_t kMaxBuildsBytes = 1024 * 1024;
inline constexpr std::size_t kMaxBuildsValues = 65'536;
// An inflated build meta. Prison Architect's, with 11 DLC, is 11 KiB.
inline constexpr std::size_t kMaxMetaBytes = 4 * 1024 * 1024;
inline constexpr std::size_t kMaxMetaValues = 65'536;
// The inflated dependency repository manifest: 69 depots in 85 KiB.
inline constexpr std::size_t kMaxDependencyManifestBytes = 1024 * 1024;
inline constexpr std::size_t kMaxDependencyManifestValues = 65'536;
// Any size a meta or manifest states: nothing GOG sells comes near it.
inline constexpr std::uint64_t kMaxContentBytes = 1ull << 40;  // 1 TiB

// One CDN endpoint of a builds answer, an open link or a secure link.
struct CdnEndpoint {
  std::string name;                               // "fastly", "gcore": for logs
  std::string url_format;                         // "{base_url}/token=…{path}", or a plain URL
  std::map<std::string, std::string> parameters;  // the placeholders' values; numbers as decimal text
  int priority = 0;
  bool fallback_only = false;
  std::vector<int> generations;                   // supports_generation
  std::string url;                                // the filled URL GOG also sends
};

// The entries of a `urls` array. An entry that isn't an object of that shape (a parameter that isn't a string or an
// integer, a priority that isn't an integer, no URL at all) is skipped.
std::vector<CdnEndpoint> ParseEndpoints(const steam::json::Value& urls);

// The endpoints that support `generation` (gcore supports only 2), in the order to try them: not fallback_only ones
// first, by priority descending, then fallback_only ones by priority descending. Ties keep their order.
std::vector<CdnEndpoint> OrderEndpoints(std::vector<CdnEndpoint> endpoints, int generation);

// The URL of `append_to_path` on an endpoint. With placeholders: the parameters, with `append_to_path` added to
// `path`, fill every `{key}` of url_format in one pass (a value is never expanded again). An unknown or unclosed
// placeholder gives nothing. With none (open links, build meta links), the endpoint's url + append_to_path. A result
// that isn't https://, or holds a space or control character, gives nothing.
std::optional<std::string> FillUrl(const CdnEndpoint& endpoint, std::string_view append_to_path);

// "ab/cd/abcd…" for an MD5 in lowercase hex; nothing for anything else.
std::optional<std::string> GalaxyPath(std::string_view hash);

struct BuildChoice {
  enum class Kind {
    kGen2,      // a gen 2 build: its meta's endpoints and ID
    kGen1,      // a gen 1 build: its repository's endpoints and timestamp
    kNone,      // no default-branch Windows build
    kProtocol,  // not a builds answer, or an item without `urls` (asked without _version=2)
  };
  Kind kind = Kind::kProtocol;
  std::string build_id;
  int generation = 0;
  std::vector<CdnEndpoint> meta;  // kGen2: ordered for generation 2
  std::string meta_id;            // kGen2: the meta's MD5, the last segment of its URL
  std::uint64_t timestamp = 0;    // kGen1: legacy_build_id, the v1 "timestamp" in every gen 1 URL
  std::vector<CdnEndpoint> repository;  // kGen1: repository.json's endpoints, ordered for generation 1 (no gcore)
};

// The first item with `branch` null or absent and `os` "windows". Never items[0] as such: Jazz Jackrabbit 2 lists
// five `JJ2+ Mod` branch builds before its default one. A gen 1 item needs a canonical `product_id` and
// `legacy_build_id`, and every endpoint that supports generation 1 must fill to
// `…/content-system/v1/manifests/<product>/windows/<timestamp>/repository.json`; otherwise the answer is kProtocol.
BuildChoice ChooseBuild(std::string_view body);

struct MetaDepot {
  std::uint64_t product = 0;
  std::vector<std::string> languages;
  std::string manifest;  // MD5 hex
  std::uint64_t size = 0;
  std::uint64_t compressed_size = 0;
  std::vector<std::string> bitness;  // osBitness; empty when absent
  bool gog_depot = false;            // isGogDepot: goggame-<id>.info, .script, .hashdb
};

struct Meta {
  std::uint64_t base_product = 0;
  std::string build_id;
  std::string install_directory;
  std::vector<std::string> dependencies;                        // dependency IDs ("DOSBox074_2CS")
  std::vector<std::pair<std::uint64_t, std::string>> products;  // product ID and name, meta order
  std::vector<MetaDepot> depots;
};

// An inflated gen 2 build meta. Product IDs must be canonical, manifests MD5 hex, sizes integers up to 1 TiB, and the
// version 2 when stated. offlineDepot is ignored; clientId and clientSecret are never kept.
std::optional<Meta> ParseMeta(std::string_view inflated);

struct Selection {
  std::vector<MetaDepot> depots;
  std::string language;  // the GOG code installed ("en-US")
  std::vector<std::uint64_t> products;
  std::uint64_t size = 0;
  std::uint64_t compressed_size = 0;
};

// A language code an install can record: 1 to 16 letters, digits, '-' or '_' ("en-US", "en", "English").
bool PlainLanguageCode(std::string_view code);

// The depots an install takes.
// - Products: the base and every other meta product in `owned`, in meta order: every owned DLC is installed.
// - Language: the requested one when a base depot carries it, else English, else the first language of the base
//   depots that the language table knows or that is a PlainLanguageCode (any other is never chosen, as if the depot
//   named "*"); the table's own code is recorded for a name it knows. Non-gog depots
//   that name a language decide; "*" carries none in particular.
// - Each kept depot names "*" or the language, gog depots too: each language has its own goggame .info and .script.
// - Bitness, per product: depots whose osBitness is absent or has "*" or "64". A depot of another bitness ("32") goes
//   only when its product has a depot that names "64"; otherwise it stays (Wine runs 32-bit programs).
// - Order: the base product's depots in meta order, then each DLC's.
Selection SelectDepots(const Meta& meta, const std::set<std::uint64_t>& owned, std::string_view steam_code);

// The dependency store (content-system.gog.com/dependencies/repository?generation=2): public and unsigned here.
struct DependencyRepository {
  std::string manifest_url;  // zlib JSON on the CDN
  std::string build_id;
  int generation = 0;
};
std::optional<DependencyRepository> ParseDependencyRepository(std::string_view body);

// A dependency's arguments as Playden keeps them: at most this many bytes of printable ASCII.
inline constexpr std::size_t kMaxDependencyArgumentsBytes = 1024;

struct Dependency {
  std::string id;        // dependencyId
  std::string exe_path;  // executable.path: "__redist/…" for an installer, empty for game-folder files
  // executable.arguments, verbatim (MSVC2005's nested quotes included). Cleared, with usable_arguments false, when
  // they are not a string, are longer than kMaxDependencyArgumentsBytes, or hold anything but printable ASCII (a
  // control character, a tab, a non-ASCII byte).
  std::string arguments;
  bool usable_arguments = true;
  std::vector<std::string> bitness;  // osBitness ("64"); empty when absent or malformed
  std::string manifest;  // the depot manifest's MD5
  std::uint64_t size = 0;
  std::uint64_t compressed_size = 0;
};

// The repository manifest's depots[]. An entry without a usable ID, manifest or size is skipped; the signature is
// ignored. Unusable arguments or bitness never drop an entry.
std::optional<std::vector<Dependency>> ParseDependencyManifest(std::string_view inflated);

// Whether a dependency's files go into the game folder (DOSBox, ScummVM, nGlide…): its executable.path does not start
// with `__redist`. The others are installers, kept under .playden-gog/redist/ (gog/dependencies.h).
bool GameFolderDependency(const Dependency& dependency);

// A dependency ID as the meta and repository write it: 1-64 of [A-Za-z0-9_.-].
bool ValidDependencyId(std::string_view id);

}  // namespace playden::gog
