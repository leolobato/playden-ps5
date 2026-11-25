#pragma once

#include "runtime/publisher_manifest.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace playden::runtime {

// A publisher runtime: files a game's installer carries in a self-extracting 7z archive,
// extracted once per installer version into
//   <runtime_root>/<a.b.c.d>/<mapped folder>/...
// The version is the VERSIONINFO file version of the spec's version_file. Extraction goes
// through <runtime_root>/.staging-<version> and a rename; <version>/.complete marks a finished
// one. Kept: the version in use, the newest other one, and every version a game's record names (below).
// Supported 7z coders: LZMA, the x86 BCJ filter over LZMA,
// and BCJ2 over three LZMA streams; anything else is refused. Every file's CRC is checked, and folders are decoded to
// disk through fixed buffers (memory: the LZMA dictionary plus a few MB).
inline constexpr const char* kPublisherRuntimeCompleteMarker = ".complete";

struct PublisherRuntimeResult {
  bool ok = false;
  bool extracted = false;   // false: an earlier extraction of this version was reused
  std::string version;      // a.b.c.d
  std::string version_dir;  // <runtime_root>/<version>
  std::string reason;       // on failure
  // Set when the game kept the version it already used: what the installer holds now ("" when it
  // could not be read).
  bool kept = false;
  std::string installer_version;
};

// The game asking for the runtime, so a game update can't change the runtime under it. A game
// keeps the version its record (<runtime_root>/.game-<game>) names while the record's pack is
// this one and that version's folder is complete; the publisher's patches are made for the
// versions the pack was built against, and a game update may bring one the pack doesn't know.
// A newer pack (a new Playden) moves the game to its installer's version. game: [0-9A-Za-z_-],
// 1 to 32 characters (the Steam app id); empty: no record is read or written.
struct PublisherRuntimeHolder {
  std::string game;
  std::string pack;  // the publisher pack's version
};

// done/total in bytes of extracted file data.
using PublisherRuntimeProgress = std::function<void(std::uint64_t done, std::uint64_t total)>;

// Finds the first spec.installers file below install_root, opens the 7z archive inside it and
// makes sure its version is extracted below runtime_root. Only entries under the top folders
// of spec.folders are extracted. On failure (including cancellation) no version folder is
// left behind for the version being extracted. With a holder, the game keeps the version it
// already used (see PublisherRuntimeHolder), also when its installer can't be read any more,
// and its record names the version it gets.
PublisherRuntimeResult EnsurePublisherRuntime(const PublisherRuntimeSpec& spec, const std::string& install_root,
                                              const std::string& runtime_root,
                                              const PublisherRuntimeProgress& progress = {},
                                              const std::function<bool()>& canceled = {},
                                              const PublisherRuntimeHolder& holder = {});

// The VERSIONINFO file version (VS_FIXEDFILEINFO) of a PE image as "a.b.c.d", if it has one.
std::optional<std::string> PeFileVersion(std::string_view image);

// Whether an archive entry path may be extracted: relative, '/' separators, no empty, "." or
// ".." parts, no drive, backslash or control characters.
bool PublisherRuntimePathAllowed(std::string_view path);

}  // namespace playden::runtime
