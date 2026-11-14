// SPDX-License-Identifier: GPL-3.0-or-later
#include "gog/plan_file.h"

#include "gog/chunk_fetch.h"
#include "gog/content.h"
#include "gog/hash.h"
#include "gog/path_rules.h"
#include "gog/product_id.h"

#include <cstdint>
#include <cstring>
#include <utility>

namespace playden::gog {
namespace {

constexpr std::size_t kLineBufferBytes = 4096;

std::string Md5OrDash(const std::string& hash) { return hash.empty() ? "-" : hash; }

std::string Join(const std::string& root, std::string_view relative) {
  std::string path = root;
  path += '/';
  path += relative;
  return path;
}

}  // namespace

bool ParsePlanNumber(std::string_view text, std::uint64_t* out) {
  if (text.empty() || text.size() > 20) return false;
  std::uint64_t value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') return false;
    const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
    if (value > (UINT64_MAX - digit) / 10) return false;
    value = value * 10 + digit;
  }
  *out = value;
  return true;
}

std::vector<std::string_view> SplitFields(std::string_view line, char separator) {
  std::vector<std::string_view> fields;
  std::size_t start = 0;
  for (;;) {
    const auto at = line.find(separator, start);
    if (at == std::string_view::npos) {
      fields.push_back(line.substr(start));
      return fields;
    }
    fields.push_back(line.substr(start, at - start));
    start = at + 1;
  }
}

bool ValidPlanPath(std::string_view path) {
  if (path.empty() || path.size() > kMaxPathBytes + 64 || path.front() == '/') return false;
  for (const auto part : SplitFields(path, '/'))
    if (part.empty() || part == "." || part == "..") return false;
  for (const char c : path)
    if (static_cast<unsigned char>(c) < 0x20) return false;
  return true;
}

std::string HeaderLine(const PlanHeader& header) {
  std::string line = "v2\t" + std::to_string(header.generation) + "\t" + header.build_id + "\t" + header.meta_id +
                     "\t" + header.language + "\t";
  for (std::size_t i = 0; i < header.products.size(); ++i) {
    if (i) line += ',';
    line += std::to_string(header.products[i]);
  }
  line += "\t" + std::to_string(header.total_size) + "\t" + std::to_string(header.total_compressed) + "\t" +
          (header.generation == 1 ? std::to_string(header.timestamp) : std::string("-"));
  return line;
}

bool ParseHeader(const std::string& line, PlanHeader* header) {
  auto fields = SplitFields(line, '\t');
  std::uint64_t number = 0;
  if (fields.size() == 7 && fields[0] == "v1") {
    header->generation = 2;
    header->timestamp = 0;
    fields.erase(fields.begin());
  } else if (fields.size() == 9 && fields[0] == "v2") {
    if (fields[1] == "1") {
      if (!CanonicalId(fields[8], &header->timestamp)) return false;
      header->generation = 1;
    } else if (fields[1] == "2") {
      if (fields[8] != "-") return false;
      header->generation = 2;
      header->timestamp = 0;
    } else {
      return false;
    }
    fields.erase(fields.begin(), fields.begin() + 2);
    fields.pop_back();
  } else {
    return false;
  }
  // fields: build, meta, language, products, total size, total download.
  if (!CanonicalId(fields[0], &number) || !IsMd5Hex(fields[1]) || !PlainLanguageCode(fields[2])) return false;
  header->build_id = std::string(fields[0]);
  header->meta_id = std::string(fields[1]);
  header->language = std::string(fields[2]);
  header->products.clear();
  for (const auto product : SplitFields(fields[3], ',')) {
    if (!CanonicalId(product, &number)) return false;
    header->products.push_back(number);
  }
  return ParsePlanNumber(fields[4], &header->total_size) && ParsePlanNumber(fields[5], &header->total_compressed);
}

bool DependencySource(std::string_view source) {
  const std::string_view prefix = kDependencySourcePrefix;
  return source.size() > prefix.size() && source.substr(0, prefix.size()) == prefix &&
         ValidDependencyId(source.substr(prefix.size()));
}

bool ValidSource(std::string_view source) {
  std::uint64_t id = 0;
  return CanonicalId(source, &id) || DependencySource(source);
}

std::string EntryLine(const PlanEntry& entry) {
  if (entry.directory) return "D\t" + entry.path;
  return "F\t" + entry.path + "\t" + entry.source + "\t" + std::to_string(entry.size) + "\t" + Md5OrDash(entry.md5) +
         "\t" + Md5OrDash(entry.sha256) + "\t" + std::to_string(entry.units.count());
}

std::string ChunkLine(const DepotChunk& chunk) {
  return "C\t" + chunk.compressed_md5 + "\t" + std::to_string(chunk.compressed_size) + "\t" + chunk.md5 + "\t" +
         std::to_string(chunk.size);
}

std::string RangeLine(const Range& range) {
  return "R\t" + std::to_string(range.offset) + "\t" + std::to_string(range.length);
}

std::string EntryText(const PlanEntry& entry) {
  std::string text = EntryLine(entry) + '\n';
  for (const auto& chunk : entry.units.chunks) text += ChunkLine(chunk) + '\n';
  for (const auto& range : entry.units.ranges) text += RangeLine(range) + '\n';
  return text;
}

// ---- LineReader ---------------------------------------------------------------------------------------------------

LineReader::LineReader(const std::string& path)
    : file_(std::fopen(path.c_str(), "rb")), buffer_(new char[kLineBufferBytes]) {}

LineReader::~LineReader() {
  if (file_) std::fclose(file_);
}

bool LineReader::Next(std::string* line, bool* complete) {
  line->clear();
  *complete = false;
  if (!file_) return false;
  while (std::fgets(buffer_.get(), kLineBufferBytes, file_)) {
    const std::size_t length = std::strlen(buffer_.get());
    line->append(buffer_.get(), length);
    if (!line->empty() && line->back() == '\n') {
      line->pop_back();
      *complete = true;
      return true;
    }
    if (line->size() > kMaxPlanLineBytes) return true;  // too long: the caller refuses it
  }
  return !line->empty();
}

bool LineReader::failed() const { return file_ && std::ferror(file_); }

// ---- EntryReader --------------------------------------------------------------------------------------------------

bool EntryReader::Header(PlanHeader* header) {
  std::string line;
  bool complete = false;
  if (!(lines_.Next(&line, &complete) && complete && ParseHeader(line, header))) return false;
  generation_ = header->generation;
  return true;
}

EntryReader::Next EntryReader::Read(PlanEntry* entry) {
  std::string line;
  bool complete = false;
  if (!lines_.Next(&line, &complete)) return lines_.failed() ? Next::kBad : Next::kEnd;
  if (!complete) return Next::kBad;
  const auto fields = SplitFields(line, '\t');
  *entry = PlanEntry();
  if (fields.size() == 2 && fields[0] == "D") {
    if (!ValidPlanPath(fields[1])) return Next::kBad;
    entry->directory = true;
    entry->path = std::string(fields[1]);
    return Next::kEntry;
  }
  if (fields.size() != 7 || fields[0] != "F" || !ValidPlanPath(fields[1]) || !ValidSource(fields[2]))
    return Next::kBad;
  entry->path = std::string(fields[1]);
  entry->source = std::string(fields[2]);
  std::uint64_t count = 0;
  if (!ParsePlanNumber(fields[3], &entry->size) || !ParsePlanNumber(fields[6], &count) || count > kMaxChunksPerFile)
    return Next::kBad;
  if (fields[4] != "-") {
    if (!IsMd5Hex(fields[4])) return Next::kBad;
    entry->md5 = std::string(fields[4]);
  }
  if (fields[5] != "-") {
    if (!IsSha256Hex(fields[5])) return Next::kBad;
    entry->sha256 = std::string(fields[5]);
  }
  std::uint64_t sum = 0;
  for (std::uint64_t i = 0; i < count; ++i) {
    if (!lines_.Next(&line, &complete) || !complete) return Next::kBad;
    const auto parts = SplitFields(line, '\t');
    if (parts.size() == 3 && parts[0] == "R") {
      Range range;
      if (!entry->units.chunks.empty() || !ParsePlanNumber(parts[1], &range.offset) ||
          !ParsePlanNumber(parts[2], &range.length) || range.length == 0 || range.length > kGen1RangeBytes ||
          range.offset > UINT64_MAX - range.length)
        return Next::kBad;
      sum += range.length;
      entry->units.ranges.push_back(range);
      continue;
    }
    DepotChunk chunk;
    if (!entry->units.ranges.empty() || parts.size() != 5 || parts[0] != "C" || !IsMd5Hex(parts[1]) ||
        !IsMd5Hex(parts[3]) || !ParsePlanNumber(parts[2], &chunk.compressed_size) ||
        !ParsePlanNumber(parts[4], &chunk.size) || chunk.compressed_size == 0 || chunk.compressed_size > kMaxChunkBytes ||
        chunk.size > kMaxChunkBytes)
      return Next::kBad;
    chunk.compressed_md5 = std::string(parts[1]);
    chunk.md5 = std::string(parts[3]);
    sum += chunk.size;
    entry->units.chunks.push_back(std::move(chunk));
  }
  if (sum != entry->size) return Next::kBad;
  // Ranges come from a product's main.bin, never from the dependency store; chunks in a gen 1 plan only from it.
  const bool dependency = DependencySource(entry->source);
  if (!entry->units.ranges.empty() && (dependency || generation_ == 2)) return Next::kBad;
  if (!entry->units.chunks.empty() && generation_ == 1 && !dependency) return Next::kBad;
  return Next::kEntry;
}

std::optional<PlanHeader> ReadPlanHeader(const std::string& plan_path) {
  EntryReader reader(plan_path);
  PlanHeader header;
  if (!reader.open() || !reader.Header(&header)) return std::nullopt;
  return header;
}

// ---- Layouts ------------------------------------------------------------------------------------------------------

PlanLayout InstallLayout(const std::string& root) {
  PlanLayout layout;
  layout.plan = Join(root, ".playden-gog/files");
  layout.items = Join(root, ".playden-gog/items.tmp");
  layout.meta_dir = Join(root, ".playden-gog/meta");
  layout.manifest_dir = Join(root, ".playden-gog/manifests");
  layout.redist_list = Join(root, ".playden-gog/redist.list");
  layout.file_list = Join(root, ".playden-files");
  layout.journal = Join(root, ".playden-download/gog-journal");
  return layout;
}

PlanLayout UpdateLayout(const std::string& root) {
  const auto update = Join(root, kUpdateDirName);
  const auto gog = update + "/gog";
  PlanLayout layout;
  layout.plan = gog + "/files";
  layout.items = gog + "/items.tmp";
  layout.meta_dir = gog + "/meta";
  layout.manifest_dir = gog + "/manifests";
  layout.redist_list = gog + "/redist.list";
  layout.file_list = update + "/files";
  layout.journal = update + "/gog-journal";
  return layout;
}

PlanLayout MeasureLayout(const std::string& root) {
  const auto measure = Join(root, ".playden-gog/measure");
  PlanLayout layout;
  layout.plan = measure + "/files";
  layout.items = measure + "/items.tmp";
  layout.meta_dir = measure + "/meta";
  layout.manifest_dir = measure + "/manifests";
  layout.redist_list = measure + "/redist.list";
  layout.file_list = measure + "/file-list";
  return layout;
}

}  // namespace playden::gog
