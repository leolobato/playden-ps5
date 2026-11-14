// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// A GOG build's `__redist` dependencies (MSVC, DirectX, XNA, .NET…): where their files go in the game folder, and the
// list that tells Play which package to run with which arguments.
//
// Every `__redist` dependency a build names is downloaded with the game into .playden-gog/redist/<ID>/ (path_rules.h
// kRedistFolder), the ones Wine already provides too: Play decides which to run. ListGamePrograms skips dot folders,
// so "Run launcher or setup" never lists them: a package Play can't install is logged (not-run), and a player who wants
// to run it uploads a copy into the game folder (docs/gog-store.md). The installer writes .playden-gog/redist.list with
// each plan:
//
//   gog-redist<TAB>v1
//   P<TAB><dependency id><TAB><program, relative to the game folder, '/'><TAB><arguments>
//   S<TAB><dependency id><TAB><missing|no_program|refused>
//
// A P line is a package whose program file is among its planned files. An S line is a dependency the build names that
// Play can't run on its own: `missing` (the dependency store doesn't list it), `no_program` (its manifest has no file
// at its executable.path), `refused` (its executable.path can't be placed, or its arguments are not usable). The files
// of `no_program` and `refused` packages are still downloaded.
//
// Plain absolute-path POSIX calls only (no descriptor-relative calls, no streams), so the console runs the same code.

#include "gog/content.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace playden::gog {

inline constexpr char kRedistListPath[] = ".playden-gog/redist.list";
inline constexpr char kRedistListHeader[] = "gog-redist\tv1";
inline constexpr std::size_t kMaxRedistListBytes = 64 * 1024;
inline constexpr std::size_t kMaxRedistEntries = 64;

// An installer kept under .playden-gog/redist/: !GameFolderDependency.
bool RedistDependency(const Dependency& dependency);

// Where a redist depot's path (or a dependency's executable.path) goes in the game folder: NormalizePath, a leading
// `__redist/` dropped (any case; `\` or `/`), then kRedistFolder in front. `__redist/XNA/xnafx31_redist.msi` becomes
// `.playden-gog/redist/XNA/xnafx31_redist.msi`; a path without the prefix gets the prefix too. Nothing for a path
// NormalizePath refuses, or whose rest it refuses (`__redist/.playden-x`), or that grows past kMaxPathBytes.
std::optional<std::string> RedistPlace(std::string_view raw);

enum class RedistSkip { kMissing, kNoProgram, kRefused };
const char* RedistSkipName(RedistSkip skip);

struct RedistEntry {
  std::string id;
  bool package = false;               // a P line; otherwise an S line
  std::string program;                // P: placed (IsRedistPlacement)
  std::string arguments;              // P: printable ASCII, at most kMaxDependencyArgumentsBytes
  RedistSkip skip = RedistSkip::kMissing;  // S
};

// Writes the list for `root` (an absolute game folder whose .playden-gog exists): a temporary file, synced, renamed
// over kRedistListPath. 0, or the errno of the failure; EINVAL when an entry is one ReadRedistList would refuse, or
// the list would pass kMaxRedistEntries or kMaxRedistListBytes.
int WriteRedistList(const std::string& root, const std::vector<RedistEntry>& entries);
// The same at `list_path` (absolute, its folder made): the temporary is `list_path` + ".tmp". WriteRedistList writes
// <root>/kRedistListPath through it.
int WriteRedistListFile(const std::string& list_path, const std::vector<RedistEntry>& entries);

struct RedistList {
  enum class Status {
    kOk,
    kMissing,  // no list (a Phase 3 install, or no .playden-gog): no redists
    kRefused,  // a link, too large, too many entries, a torn last line, a bad header or entry
  };
  Status status = Status::kMissing;
  std::vector<RedistEntry> entries;
};

// Reads `root`'s list: the file opened with O_NOFOLLOW (and .playden-gog a real folder), at most kMaxRedistListBytes
// and kMaxRedistEntries, every line complete and free of control characters other than its tabs, each ID a
// ValidDependencyId and each program an IsRedistPlacement. Works signed out and offline: no network.
RedistList ReadRedistList(const std::string& root);

}  // namespace playden::gog
