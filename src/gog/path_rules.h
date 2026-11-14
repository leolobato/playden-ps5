// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The paths a GOG manifest names, made safe to write under a game folder. A raw path goes through NormalizePath, then
// PlaceFile (support files), then a PathSet that merges every depot of the install. Nothing here touches the disk.
//
// exFAT drives ignore case, so two paths that differ only in case are one file, and a folder keeps the first spelling
// any depot gave it. Links are never created: Playden refuses symlinks under its roots and Windows builds don't need
// them.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace playden::gog {

enum class PathRefusal {
  kNone,
  kEmpty,              // nothing left after the leading separator
  kAbsolute,           // a second leading separator ("//x")
  kDrive,              // "C:", a character Windows and exFAT can't hold in a name (: * ? " < > |), or a name
                       // ending in a dot or space
  kDotComponent,       // "." or ".."
  kEmptyComponent,     // "a//b", or a trailing separator
  kControl,            // NUL or another control character
  kTooLong,            // more than kMaxPathBytes in all
  kComponentTooLong,   // a component over kMaxComponentBytes
  kReserved,           // the first component is one of Playden's own names
  kLink,               // a DepotLink: never created
  kClash,              // a file where another depot has a folder, or the reverse
};

const char* PathRefusalName(PathRefusal refusal);

inline constexpr std::size_t kMaxPathBytes = 1024;
inline constexpr std::size_t kMaxComponentBytes = 255;

// `\` becomes `/` and one leading `/` is dropped. Refused (nullopt, *why set when why is given): an empty path, a
// remaining leading `/`, a drive letter or a name Windows can't hold (a `:`, `*`, `?`, `"`, `<`, `>` or `|`, or a
// trailing dot or space), a `.` or `..` component, an empty component, a control character, a path
// over 1024 bytes or a component over 255, and a first component that starts with ".playden" in any case
// (.playden-install.json, .playden-files, …): a manifest must never overwrite Playden's record or journals.
std::optional<std::string> NormalizePath(std::string_view raw, PathRefusal* why = nullptr);

// Whether `name` is Playden's at a game folder's top: any name that starts with ".playden", ignoring ASCII case (no
// non-ASCII letter upcases to one of those letters, so a look-alike such as ".playden-fıles" is refused too).
bool IsReservedName(std::string_view name);

// Where a normalized path goes in the game folder. A file without the support flag stays where it is. A support file
// under `app/` (any case, with more after it) goes into the game folder without the prefix, as the Mac app does (DOSBox
// configs live there); any other support file goes to `.playden-gog/support/<product>/<path>`. The `app/` result is
// checked again for Playden's own names: `app/.playden-files` is refused (kReserved).
std::optional<std::string> PlaceFile(std::string_view path, std::uint64_t product, bool support,
                                     PathRefusal* why = nullptr);

// PlaceFile for a gen 1 manifest. Gen 1 support files name their product first ("1425039730/app/lang.ini",
// "1425039730/galaxy_x.exe"), so a support path whose first component is `product` in decimal, with more after it,
// loses that component before PlaceFile: `<product>/app/lang.ini` becomes `lang.ini` in the game folder and
// `<product>/galaxy_x.exe` becomes `.playden-gog/support/<product>/galaxy_x.exe`. Any other path is placed as PlaceFile
// places it. This rule is an assumption, read from Monkey Island 2's manifests alone; that game on the console is the
// check.
std::optional<std::string> PlaceFileV1(std::string_view path, std::uint64_t product, bool support,
                                       PathRefusal* why = nullptr);

// The folder under a game folder that holds the build's `__redist` installers, one folder per dependency ID
// (gog/dependencies.h places them).
inline constexpr char kRedistFolder[] = ".playden-gog/redist/";

// Whether `path` is a placed redist path: kRedistFolder, then a path that NormalizePath gives back unchanged, within
// kMaxPathBytes in all. Sets *why (kReserved without the prefix, else NormalizePath's reason) when refused.
bool IsRedistPlacement(std::string_view path, PathRefusal* why = nullptr);

enum class PathKind { kFile, kDirectory, kLink };

// The key exFAT compares names by: ASCII lowercase, and the case pairs of its upcase table in two-byte UTF-8 (Latin-1,
// Latin Extended-A, basic Greek and Cyrillic; dotless i is "i", long s is "s"). Other bytes stay as they are.
std::string FoldCase(std::string_view text);

// Every depot's entries, merged as they will sit on disk. Add them in depot order: the base product's depots first,
// then each DLC's.
class PathSet {
 public:
  struct Added {
    PathRefusal refusal = PathRefusal::kNone;
    std::string path;                     // the canonical path to write, folders in their first spelling
    std::optional<std::size_t> replaced;  // the index an earlier entry with the same key had: the caller drops it
    bool ok() const { return refusal == PathRefusal::kNone; }
  };

  // Normalizes and places `raw` (NormalizePath, PlaceFile), then merges it under the caller's `index`. The same file
  // (or folder) key again replaces the earlier entry: the last depot wins, so DLC wins over base. A file whose key is
  // a folder already, or a path under a file, is refused (kClash) and nothing of it is recorded. A link is refused
  // (kLink). The caller counts and logs refusals. `generation` 1 places with PlaceFileV1.
  Added Add(std::string_view raw, PathKind kind, std::uint64_t product, bool support, std::size_t index,
            int generation = 2);

  // A redist file or folder already placed (RedistPlace): merged like Add's paths, keyed case-folded with them, and
  // never refused for Playden's own `.playden` prefix, which Playden built. A path that is not an IsRedistPlacement is
  // refused; a link too (kLink).
  Added AddRedist(std::string_view placed, PathKind kind, std::size_t index);

  // An installed path's spelling (a placed path, as .playden-files lists it), before the first Add: a file or folder
  // added later whose key is `path`'s, or one of the folders above it, takes the seeded spelling, so a build that
  // changes only a name's case keeps the name already on the drive. A seed records a spelling only, never an entry: it
  // never causes kClash and never sets `replaced`, whatever its kind. A spelling whose byte length differs from the
  // added one's (a fold that changes length) is not taken. A link is ignored.
  void Seed(std::string_view path, PathKind kind);

  std::size_t entries() const { return entries_.size(); }

 private:
  struct Entry {
    PathKind kind;
    std::size_t index;
  };
  Added Merge(std::string path, PathKind kind, std::size_t index);
  std::unordered_map<std::string, Entry> entries_;          // folded path -> the explicit file or folder entry
  std::unordered_map<std::string, std::string> folders_;    // folded folder path -> its first spelling
  std::unordered_map<std::string, std::string> seeds_;      // folded path -> its installed spelling
};

}  // namespace playden::gog
