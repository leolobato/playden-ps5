// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// A GOG build's plan, as the depot installer saves it (installer.h describes the folder), and where a plan lives. The
// installer, the update and the details source read plans through this one parser.
//
// The plan, line by line: a header
//   "v2\t<generation>\t<build>\t<meta>\t<GOG language>\t<products, comma-separated>\t<total size>\t<total download>\t
//   <timestamp or ->"
// then per entry "D\t<path>", or "F\t<path>\t<source>\t<size>\t<md5|->\t<sha256|->\t<units>" followed by one line per
// unit: "C\t<compressed md5>\t<compressed size>\t<md5>\t<size>" for a gen 2 chunk, or "R\t<blob offset>\t<length>" for
// a byte range of a gen 1 main.bin (at most kGen1RangeBytes). A file's units are all chunks or all ranges, and add up
// to its size. The source is a product ID, or dep:<id> for a dependency from the public store. A Phase 3 plan's
// "v1\t<build>\t<meta>\t<language>\t<products>\t<total size>\t<total compressed>" header reads as generation 2.
// items.tmp, the planner's work file, has the same entry lines without a header.
//
// Paths in entries are relative to the game folder, '/'-separated, without an empty, "." or ".." component.
//
// Plain absolute-path stdio calls only (no descriptor-relative calls, no streams), so the console runs the same code.

#include "gog/depot_reader.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace playden::gog {

// The same name as installs::kUpdateDirName (steam_core, which playden_gog can't use): a game update's work folder.
inline constexpr char kUpdateDirName[] = ".playden-update";
inline constexpr char kDependencySourcePrefix[] = "dep:";
inline constexpr std::uint64_t kMaxChunksPerFile = 1u << 24;
inline constexpr std::size_t kMaxPlanLineBytes = 64 * 1024;

struct PlanHeader {
  int generation = 2;
  std::string build_id;
  std::string meta_id;
  std::string language;
  std::vector<std::uint64_t> products;
  std::uint64_t total_size = 0;
  std::uint64_t total_compressed = 0;  // the download: compressed chunks, and gen 1 ranges by their length
  std::uint64_t timestamp = 0;         // gen 1 only
};

// A byte range of a gen 1 main.bin.
struct Range {
  std::uint64_t offset = 0;  // in the blob
  std::uint64_t length = 0;
};

// A file's units: chunks (gen 2) or ranges (gen 1), never both.
struct Units {
  std::vector<DepotChunk> chunks;
  std::vector<Range> ranges;
  std::size_t count() const { return chunks.empty() ? ranges.size() : chunks.size(); }
  // The bytes a unit writes into the file.
  std::uint64_t size(std::size_t unit) const { return chunks.empty() ? ranges[unit].length : chunks[unit].size; }
  // The bytes a unit downloads.
  std::uint64_t download(std::size_t unit) const {
    return chunks.empty() ? ranges[unit].length : chunks[unit].compressed_size;
  }
};

struct PlanEntry {
  bool directory = false;
  std::string path;
  std::string source;
  std::uint64_t size = 0;
  std::string md5;     // empty for "-"
  std::string sha256;  // empty for "-"
  Units units;
  std::uint64_t download() const {
    std::uint64_t total = 0;
    for (std::size_t unit = 0; unit < units.count(); ++unit) total += units.download(unit);
    return total;
  }
};

// Plain text helpers the plan and the installer's journal share.
bool ParsePlanNumber(std::string_view text, std::uint64_t* out);
std::vector<std::string_view> SplitFields(std::string_view line, char separator);
// A plan path: relative, no empty, "." or ".." component, no control character.
bool ValidPlanPath(std::string_view path);

std::string HeaderLine(const PlanHeader& header);
// "v2\t<generation>\t…\t<timestamp or ->", or a Phase 3 "v1\t…" header (generation 2, no timestamp).
bool ParseHeader(const std::string& line, PlanHeader* header);
// dep:<a ValidDependencyId>.
bool DependencySource(std::string_view source);
// A product ID or a DependencySource.
bool ValidSource(std::string_view source);
std::string EntryLine(const PlanEntry& entry);
std::string ChunkLine(const DepotChunk& chunk);
std::string RangeLine(const Range& range);
// The entry line and its unit lines, each ending in a newline.
std::string EntryText(const PlanEntry& entry);

// Reads lines through a heap buffer; `complete` is false for a last line without its newline. A line past
// kMaxPlanLineBytes is returned cut, for the caller to refuse.
class LineReader {
 public:
  explicit LineReader(const std::string& path);
  ~LineReader();
  LineReader(const LineReader&) = delete;
  LineReader& operator=(const LineReader&) = delete;
  bool open() const { return file_ != nullptr; }
  bool Next(std::string* line, bool* complete);
  bool failed() const;

 private:
  FILE* file_;
  std::unique_ptr<char[]> buffer_;
};

// Reads the entries of a plan (or of items.tmp, which has the same entry lines). After Header, the units must fit the
// plan's generation: gen 2 files have chunks only; in gen 1, a product's file has ranges and a dependency's chunks. A
// torn line (no newline) is kBad.
class EntryReader {
 public:
  explicit EntryReader(const std::string& path) : lines_(path) {}
  bool open() const { return lines_.open(); }
  bool Header(PlanHeader* header);
  enum class Next { kEntry, kEnd, kBad };
  Next Read(PlanEntry* entry);

 private:
  LineReader lines_;
  int generation_ = 0;  // 0 before Header (items.tmp): either generation's units
};

// The header of the plan at `plan_path` (absolute), or nothing when it is missing or doesn't read.
std::optional<PlanHeader> ReadPlanHeader(const std::string& plan_path);

// Where a plan and the planner's other files go, every path absolute. The planner writes only there.
struct PlanLayout {
  std::string plan;          // the files plan
  std::string items;         // items.tmp, while planning
  std::string meta_dir;      // build metas, gen 1 repository.json
  std::string manifest_dir;  // depot manifests
  std::string redist_list;   // gog/dependencies.h's list
  std::string file_list;     // "d\t<path>", "f\t<path>" for every placed path
  std::string journal;       // deleted when planning again; empty: none
};

// The install's: <root>/.playden-gog/{files,items.tmp,meta,manifests,redist.list}, <root>/.playden-files and
// <root>/.playden-download/gog-journal.
PlanLayout InstallLayout(const std::string& root);
// An update's, beside the live install: <root>/.playden-update/gog/{files,items.tmp,meta,manifests,redist.list}, the
// file list <root>/.playden-update/files and the journal <root>/.playden-update/gog-journal.
PlanLayout UpdateLayout(const std::string& root);
// A measure's (an exact size without changing anything): <root>/.playden-gog/measure/{files,items.tmp,meta,manifests,
// redist.list,file-list}, no journal.
PlanLayout MeasureLayout(const std::string& root);

}  // namespace playden::gog
