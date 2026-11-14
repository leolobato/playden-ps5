// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// What a GOG update changes: a new build's plan (plan_file.h) against the installed one and the files on disk.
//
// DiffPlans streams the new plan. The old plan's file entries are read once into a map keyed by FoldCase(path) (exFAT
// ignores case, so 'Data/File.dat' and 'data/file.dat' are one file), holding each one's size and hashes. A new file is
// unchanged only when the old plan has its key, SameContent says the two entries are the same bytes, and
// <root>/<new path> is a regular file (lstat: never a link) of exactly that size. Directories are never changed. Old
// files whose key the new plan lacks are counted as removed. Support (.playden-gog/support/…) and redist
// (.playden-gog/redist/…) entries are compared like any other.
//
// Without an old plan that reads, every new file whose disk file has the entry's size is hashed (a streamed MD5
// through a heap buffer, cancel checked on every block): it is unchanged when the digest is the entry's content MD5. A
// file with no content MD5 counts as changed; an empty entry against an empty disk file is unchanged.
//
// Plain absolute-path POSIX calls only (no descriptor-relative calls, no streams), so the console runs the same code.
// Nothing is logged.

#include "gog/plan_file.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace playden::gog {

struct DiffResult {
  std::uint64_t changed_files = 0;
  std::uint64_t unchanged_files = 0;
  std::uint64_t removed_files = 0;   // old file entries the new plan lacks
  std::uint64_t hashed_files = 0;    // disk files hashed for want of an old plan (the runner shows kCheckingFiles)
  std::uint64_t staged_bytes = 0;    // the changed files' sizes
  std::uint64_t download_bytes = 0;  // the changed entries' download()
  std::uint64_t install_bytes = 0;   // the new header's total_size
  bool canceled = false;
  bool readable = false;  // the new plan read whole; false: nothing above can be used
  bool old_plan = false;  // the old plan read whole and was compared against (otherwise the disk files were hashed)
};

// Whether two file entries hold the same bytes. The sizes must be equal, and two empty files are the same. Then the
// first rule both sides can answer decides: both whole-file MD5s, then both SHA-256s, then both content MD5s (the
// file's MD5, or a single chunk's when that chunk is the whole file), then both chunk lists (the same sequence of
// chunk MD5 and size). Anything else is not the same. Gen 1 ranges carry no hash of their own; a gen 1 file always has
// its MD5. This compares gen 1 with gen 2, and gen 2 files without a file MD5.
bool SameContent(const PlanEntry& old_entry, const PlanEntry& new_entry);

// The file's MD5, or its single chunk's when that chunk is the whole file; empty when neither.
std::string ContentMd5(const PlanEntry& entry);

// `old_plan` and `new_plan` absolute; `root` the game folder the new plan's paths are under. `each` (may be empty)
// gets every new entry with its plan index (the journal's: directories count) and whether it changed, in plan order.
DiffResult DiffPlans(const std::string& old_plan, const std::string& new_plan, const std::string& root,
                     const std::atomic<bool>& cancel,
                     const std::function<void(std::uint64_t index, const PlanEntry&, bool changed)>& each);

// The update a check and a measure both name: the build and the products it takes (the base game and the owned DLC
// the player didn't turn off), so a DLC bought or turned off since names another update. The first 8 bytes
// (big-endian) of SHA-256("gog\n" + generation + "\n" + build_id + "\n" + the products sorted, without repeats,
// comma-separated), 0 made 1.
std::uint64_t UpdateTarget(std::string_view build_id, int generation, std::vector<std::uint64_t> products);

}  // namespace playden::gog
