#include "runtime/msi_package.h"

#include "miniz.h"
#include "runtime/lzx_decoder.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <set>

namespace playden::runtime {
namespace {

constexpr std::uint32_t kEndOfChain = 0xfffffffe;
constexpr std::uint32_t kFreeSector = 0xffffffff;
constexpr std::size_t kMaxStreamBytes = 512u * 1024u * 1024u;

std::uint16_t U16(std::string_view data, std::size_t at) {
  return static_cast<std::uint16_t>(static_cast<unsigned char>(data[at]) | (static_cast<unsigned char>(data[at + 1]) << 8));
}

std::uint32_t U32(std::string_view data, std::size_t at) {
  return static_cast<std::uint32_t>(U16(data, at)) | (static_cast<std::uint32_t>(U16(data, at + 2)) << 16);
}

std::string Lower(std::string_view text) {
  std::string out(text);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char value) { return static_cast<char>(value >= 'A' && value <= 'Z' ? value + 32 : value); });
  return out;
}

bool Ascii(std::string_view text) {
  return std::all_of(text.begin(), text.end(), [](unsigned char value) { return value >= 0x20 && value < 0x7f; });
}

// ---------------------------------------------------------------------------------------------
// Compound file (the container an .msi is): sectors chained through a FAT, small streams in a
// mini stream chained through the mini FAT, and a directory of 128-byte entries.

struct CompoundEntry {
  std::string name;  // decoded MSI stream name ("!File", "media1.cab")
  std::uint32_t start = 0;
  std::uint64_t size = 0;
};

class Compound {
 public:
  bool Open(std::string_view data, std::string* reason);
  const CompoundEntry* Find(const std::string& name) const {
    for (const auto& entry : entries_)
      if (entry.name == name) return &entry;
    return nullptr;
  }
  bool Read(const CompoundEntry& entry, std::string* out) const;
  // The stream in place when its sectors follow one another, as a large stream's usually do.
  std::optional<std::string_view> View(const CompoundEntry& entry) const;

 private:
  bool Chain(const std::vector<std::uint32_t>& table, std::uint32_t start, std::uint64_t size, bool mini,
             std::string* out) const;
  std::string_view data_;
  std::uint32_t sector_size_ = 512;
  std::uint32_t cutoff_ = 4096;
  std::vector<std::uint32_t> fat_, mini_fat_;
  std::string mini_stream_;
  std::vector<CompoundEntry> entries_;
};

// MSI packs stream names: characters 0x3800-0x47ff hold two name characters, 0x4800-0x483f one,
// and 0x4840 marks a table stream ("!").
std::string DecodeStreamName(const std::vector<std::uint16_t>& units) {
  static constexpr char kAlphabet[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz._";
  std::string out;
  for (const auto unit : units) {
    if (unit >= 0x3800 && unit < 0x4800) {
      out += kAlphabet[(unit - 0x3800) & 0x3f];
      out += kAlphabet[((unit - 0x3800) >> 6) & 0x3f];
    } else if (unit >= 0x4800 && unit < 0x4840) {
      out += kAlphabet[unit - 0x4800];
    } else if (unit == 0x4840) {
      out += '!';
    } else if (unit < 0x80) {
      out += static_cast<char>(unit);
    } else {
      out += '?';
    }
  }
  return out;
}

bool Compound::Chain(const std::vector<std::uint32_t>& table, std::uint32_t start, std::uint64_t size, bool mini,
                     std::string* out) const {
  out->clear();
  if (size > kMaxStreamBytes) return false;
  const std::uint32_t unit = mini ? 64 : sector_size_;
  out->reserve(static_cast<std::size_t>(size));
  std::uint32_t sector = start;
  for (std::size_t steps = 0; out->size() < size; ++steps) {
    if (sector == kEndOfChain || sector >= table.size() || steps > table.size()) return false;
    const std::size_t take = static_cast<std::size_t>(std::min<std::uint64_t>(unit, size - out->size()));
    if (mini) {
      const std::size_t at = static_cast<std::size_t>(sector) * 64;
      if (at + take > mini_stream_.size()) return false;
      out->append(mini_stream_, at, take);
    } else {
      const std::uint64_t at = (static_cast<std::uint64_t>(sector) + 1) * sector_size_;
      if (at + take > data_.size()) return false;
      out->append(data_.substr(static_cast<std::size_t>(at), take));
    }
    sector = table[sector];
  }
  return true;
}

bool Compound::Read(const CompoundEntry& entry, std::string* out) const {
  return Chain(entry.size < cutoff_ ? mini_fat_ : fat_, entry.start, entry.size, entry.size < cutoff_, out);
}

std::optional<std::string_view> Compound::View(const CompoundEntry& entry) const {
  if (entry.size < cutoff_ || entry.size == 0 || entry.size > kMaxStreamBytes) return std::nullopt;
  const std::uint64_t sectors = (entry.size + sector_size_ - 1) / sector_size_;
  const std::uint64_t at = (static_cast<std::uint64_t>(entry.start) + 1) * sector_size_;
  if (at + entry.size > data_.size()) return std::nullopt;
  // Each sector is in the FAT, as Read requires, and each but the last links to the next.
  for (std::uint64_t index = 0; index < sectors; ++index) {
    const auto sector = entry.start + index;
    if (sector >= fat_.size() || (index + 1 < sectors && fat_[sector] != sector + 1)) return std::nullopt;
  }
  return data_.substr(static_cast<std::size_t>(at), static_cast<std::size_t>(entry.size));
}

bool Compound::Open(std::string_view data, std::string* reason) {
  const auto fail = [&](const char* why) {
    if (reason) *reason = why;
    return false;
  };
  static constexpr unsigned char kSignature[] = {0xd0, 0xcf, 0x11, 0xe0, 0xa1, 0xb1, 0x1a, 0xe1};
  if (data.size() < 512 || std::memcmp(data.data(), kSignature, sizeof(kSignature)) != 0)
    return fail("not a Windows Installer package");
  data_ = data;
  const auto shift = U16(data, 0x1e);
  if (shift != 9 && shift != 12) return fail("unsupported package sector size");
  sector_size_ = 1u << shift;
  if (U16(data, 0x20) != 6) return fail("unsupported package mini sector size");
  cutoff_ = U32(data, 0x38);
  const auto fat_sectors = U32(data, 0x2c);
  const std::uint32_t total_sectors = static_cast<std::uint32_t>(data.size() / sector_size_);
  if (fat_sectors > total_sectors) return fail("damaged package");
  // The FAT's own sectors: 109 in the header, the rest in a chain of DIFAT sectors.
  std::vector<std::uint32_t> fat_list;
  for (std::size_t index = 0; index < 109 && fat_list.size() < fat_sectors; ++index)
    fat_list.push_back(U32(data, 0x4c + index * 4));
  std::uint32_t difat = U32(data, 0x44);
  for (std::uint32_t steps = 0; fat_list.size() < fat_sectors; ++steps) {
    if (difat == kEndOfChain || difat == kFreeSector || steps > total_sectors) return fail("damaged package");
    const std::uint64_t at = (static_cast<std::uint64_t>(difat) + 1) * sector_size_;
    if (at + sector_size_ > data.size()) return fail("damaged package");
    for (std::size_t index = 0; index + 1 < sector_size_ / 4 && fat_list.size() < fat_sectors; ++index)
      fat_list.push_back(U32(data, static_cast<std::size_t>(at) + index * 4));
    difat = U32(data, static_cast<std::size_t>(at) + sector_size_ - 4);
  }
  fat_.clear();
  std::set<std::uint32_t> fat_seen;
  for (const auto sector : fat_list) {
    if (!fat_seen.insert(sector).second) return fail("damaged package");
    const std::uint64_t at = (static_cast<std::uint64_t>(sector) + 1) * sector_size_;
    if (at + sector_size_ > data.size()) return fail("damaged package");
    for (std::size_t index = 0; index < sector_size_ / 4; ++index)
      fat_.push_back(U32(data, static_cast<std::size_t>(at) + index * 4));
  }
  // Chains of unknown length (directory, mini FAT) are read sector by sector until their end.
  const auto whole_chain = [&](std::uint32_t start, std::string* out) {
    out->clear();
    for (std::uint32_t sector = start, steps = 0; sector != kEndOfChain; ++steps) {
      if (sector >= fat_.size() || steps > total_sectors || out->size() + sector_size_ > data.size()) return false;
      const std::uint64_t at = (static_cast<std::uint64_t>(sector) + 1) * sector_size_;
      if (at + sector_size_ > data.size()) return false;
      out->append(data.substr(static_cast<std::size_t>(at), sector_size_));
      sector = fat_[sector];
    }
    return true;
  };
  std::string directory, mini_fat;
  if (!whole_chain(U32(data, 0x30), &directory) || directory.size() < 128) return fail("damaged package directory");
  const auto mini_fat_start = U32(data, 0x3c);
  if (mini_fat_start != kEndOfChain && !whole_chain(mini_fat_start, &mini_fat)) return fail("damaged package");
  mini_fat_.clear();
  for (std::size_t at = 0; at + 4 <= mini_fat.size(); at += 4) mini_fat_.push_back(U32(mini_fat, at));
  entries_.clear();
  for (std::size_t at = 0; at + 128 <= directory.size(); at += 128) {
    const auto type = static_cast<unsigned char>(directory[at + 0x42]);
    const auto name_bytes = std::min<std::uint16_t>(U16(directory, at + 0x40), 64);
    std::vector<std::uint16_t> units;
    for (std::size_t index = 0; index + 1 < name_bytes; index += 2) {
      const auto unit = U16(directory, at + index);
      if (unit == 0) break;
      units.push_back(unit);
    }
    CompoundEntry entry;
    entry.name = DecodeStreamName(units);
    entry.start = U32(directory, at + 0x74);
    entry.size = U32(directory, at + 0x78);
    if (sector_size_ == 4096) entry.size |= static_cast<std::uint64_t>(U32(directory, at + 0x7c)) << 32;
    if (type == 5) {  // the root entry holds the mini stream
      if (entry.size && !Chain(fat_, entry.start, entry.size, false, &mini_stream_))
        return fail("damaged package mini stream");
    } else if (type == 2) {
      entries_.push_back(std::move(entry));
    }
  }
  return true;
}

// ---------------------------------------------------------------------------------------------
// The installer database: a string pool and column-major tables described by _Columns.

struct Cell {
  bool null = true;
  std::string text;          // strings
  long long number = 0;      // integers
};

struct Table {
  std::vector<std::string> columns;
  std::vector<std::vector<Cell>> rows;
  int Column(const char* name) const {
    for (std::size_t index = 0; index < columns.size(); ++index)
      if (columns[index] == name) return static_cast<int>(index);
    return -1;
  }
};

struct ColumnInfo {
  int number = 0;
  std::string name;
  unsigned type = 0;
};

constexpr unsigned kTypeString = 0x0800;
constexpr unsigned kTypeValid = 0x0100;
constexpr unsigned kTypeNullable = 0x1000;
constexpr unsigned kTypeTemporary = 0x4000;

class Database {
 public:
  bool Open(std::string_view package, std::string* reason);
  bool ReadTable(const std::string& name, Table* table) const;
  const Compound& compound() const { return compound_; }

 private:
  const std::string& String(std::uint32_t id) const {
    static const std::string empty;
    return id < strings_.size() ? strings_[id] : empty;
  }
  bool ReadColumns(const std::string& stream, const std::vector<ColumnInfo>& columns, Table* table) const;
  Compound compound_;
  std::vector<std::string> strings_;
  std::size_t string_bytes_ = 2;
  std::map<std::string, std::vector<ColumnInfo>> columns_;
};

bool Database::Open(std::string_view package, std::string* reason) {
  const auto fail = [&](const char* why) {
    if (reason) *reason = why;
    return false;
  };
  if (!compound_.Open(package, reason)) return false;
  const auto* pool_entry = compound_.Find("!_StringPool");
  const auto* data_entry = compound_.Find("!_StringData");
  std::string pool, data;
  if (!pool_entry || !data_entry || !compound_.Read(*pool_entry, &pool) || !compound_.Read(*data_entry, &data) ||
      pool.size() < 4)
    return fail("the package has no string pool");
  // The first entry is the code page, its top bit saying string ids take three bytes.
  string_bytes_ = (U32(pool, 0) & 0x80000000u) ? 3 : 2;
  strings_.assign(1, std::string());
  std::size_t offset = 0;
  for (std::size_t at = 4; at + 4 <= pool.size();) {
    std::size_t length = U16(pool, at);
    const auto references = U16(pool, at + 2);
    if (length == 0 && references != 0) {
      // A string over 64 KiB: this entry's count is the high word, the next entry the length.
      if (at + 8 > pool.size()) return fail("damaged string pool");
      length = (static_cast<std::size_t>(U16(pool, at + 6)) << 16) | U16(pool, at + 4);
      at += 8;
    } else {
      at += 4;
    }
    if (offset + length > data.size()) return fail("damaged string pool");
    strings_.push_back(data.substr(offset, length));
    offset += length;
  }
  const auto* columns_entry = compound_.Find("!_Columns");
  std::string columns;
  if (!columns_entry || !compound_.Read(*columns_entry, &columns)) return fail("the package has no column list");
  const std::size_t row = string_bytes_ * 2 + 4;
  const std::size_t count = columns.size() / row;
  const auto string_at = [&](std::size_t at) {
    return string_bytes_ == 3 ? static_cast<std::uint32_t>(U16(columns, at)) | (static_cast<unsigned char>(columns[at + 2]) << 16)
                              : static_cast<std::uint32_t>(U16(columns, at));
  };
  for (std::size_t index = 0; index < count; ++index) {
    const auto table = String(string_at(index * string_bytes_));
    ColumnInfo info;
    info.number = static_cast<int>(U16(columns, count * string_bytes_ + index * 2) ^ 0x8000u);
    info.name = String(string_at(count * (string_bytes_ + 2) + index * string_bytes_));
    info.type = U16(columns, count * (string_bytes_ * 2 + 2) + index * 2) ^ 0x8000u;
    columns_[table].push_back(std::move(info));
  }
  for (auto& [name, list] : columns_)
    std::sort(list.begin(), list.end(), [](const ColumnInfo& a, const ColumnInfo& b) { return a.number < b.number; });
  return true;
}

bool Database::ReadColumns(const std::string& stream, const std::vector<ColumnInfo>& columns, Table* table) const {
  std::vector<std::size_t> widths;
  std::vector<const ColumnInfo*> stored;
  for (const auto& column : columns) {
    if (column.type & kTypeTemporary) continue;
    const bool binary = (column.type & ~kTypeNullable) == (kTypeString | kTypeValid);
    widths.push_back(binary ? 2 : (column.type & kTypeString) ? string_bytes_ : (column.type & 0xff) <= 2 ? 2 : 4);
    stored.push_back(&column);
  }
  std::size_t row_bytes = 0;
  for (const auto width : widths) row_bytes += width;
  if (row_bytes == 0) return false;
  const std::size_t count = stream.size() / row_bytes;
  table->columns.clear();
  for (const auto* column : stored) table->columns.push_back(column->name);
  table->rows.assign(count, std::vector<Cell>(stored.size()));
  std::size_t at = 0, text_bytes = 0;
  for (std::size_t column = 0; column < stored.size(); ++column) {
    const bool binary = (stored[column]->type & ~kTypeNullable) == (kTypeString | kTypeValid);
    const bool text = (stored[column]->type & kTypeString) && !binary;
    for (std::size_t index = 0; index < count; ++index, at += widths[column]) {
      auto& cell = table->rows[index][column];
      if (text) {
        const std::uint32_t id = widths[column] == 3
            ? static_cast<std::uint32_t>(U16(stream, at)) | (static_cast<unsigned char>(stream[at + 2]) << 16)
            : U16(stream, at);
        cell.null = id == 0;
        cell.text = String(id);
        // Cells copy pooled strings: a table repeating one long string must not multiply it without bound.
        if ((text_bytes += cell.text.size()) > kMaxStreamBytes / 4) return false;
      } else if (widths[column] == 2) {
        const auto raw = U16(stream, at);
        cell.null = raw == 0;
        cell.number = static_cast<long long>(raw) - 0x8000;
      } else {
        const auto raw = U32(stream, at);
        cell.null = raw == 0;
        cell.number = static_cast<long long>(raw) - 0x80000000ll;
      }
    }
  }
  return true;
}

bool Database::ReadTable(const std::string& name, Table* table) const {
  table->columns.clear();
  table->rows.clear();
  const auto columns = columns_.find(name);
  if (columns == columns_.end()) return true;  // the package doesn't have it
  const auto* entry = compound_.Find("!" + name);
  if (!entry) {
    for (const auto& column : columns->second) table->columns.push_back(column.name);
    return true;  // an empty table has no stream
  }
  std::string stream;
  return compound_.Read(*entry, &stream) && ReadColumns(stream, columns->second, table);
}

const Cell& At(const Table& table, const std::vector<Cell>& row, const char* column) {
  static const Cell null_cell;
  const int index = table.Column(column);
  return index < 0 ? null_cell : row[static_cast<std::size_t>(index)];
}

// "short|long" names keep the long part; "target:source" names keep the target.
std::string LongName(std::string text) {
  if (const auto colon = text.find(':'); colon != std::string::npos) text.resize(colon);
  if (const auto bar = text.find('|'); bar != std::string::npos) text.erase(0, bar + 1);
  return text;
}

// ---------------------------------------------------------------------------------------------
// Conditions.

class ConditionParser {
 public:
  ConditionParser(std::string_view text, const std::map<std::string, std::string>& properties)
      : text_(text), properties_(properties) {}
  std::optional<bool> Parse() {
    bool value = false;
    if (!Implication(&value)) return std::nullopt;
    Space();
    if (at_ != text_.size()) return std::nullopt;
    return value;
  }

 private:
  void Space() {
    while (at_ < text_.size() && (text_[at_] == ' ' || text_[at_] == '\t' || text_[at_] == '\r' || text_[at_] == '\n')) ++at_;
  }
  bool Keyword(const char* word) {
    Space();
    const std::size_t length = std::strlen(word);
    if (at_ + length > text_.size() || Lower(text_.substr(at_, length)) != Lower(word)) return false;
    const std::size_t end = at_ + length;
    if (end < text_.size() && (std::isalnum(static_cast<unsigned char>(text_[end])) || text_[end] == '_' || text_[end] == '.'))
      return false;
    at_ = end;
    return true;
  }
  bool Implication(bool* value) {
    if (!Equivalence(value)) return false;
    while (Keyword("IMP")) {
      bool right = false;
      if (!Equivalence(&right)) return false;
      *value = !*value || right;
    }
    return true;
  }
  bool Equivalence(bool* value) {
    if (!Exclusive(value)) return false;
    while (Keyword("EQV")) {
      bool right = false;
      if (!Exclusive(&right)) return false;
      *value = *value == right;
    }
    return true;
  }
  bool Exclusive(bool* value) {
    if (!Disjunction(value)) return false;
    while (Keyword("XOR")) {
      bool right = false;
      if (!Disjunction(&right)) return false;
      *value = *value != right;
    }
    return true;
  }
  bool Disjunction(bool* value) {
    if (!Conjunction(value)) return false;
    while (Keyword("OR")) {
      bool right = false;
      if (!Conjunction(&right)) return false;
      *value = *value || right;
    }
    return true;
  }
  bool Conjunction(bool* value) {
    if (!Negation(value)) return false;
    while (Keyword("AND")) {
      bool right = false;
      if (!Negation(&right)) return false;
      *value = *value && right;
    }
    return true;
  }
  bool Negation(bool* value) {
    if (Keyword("NOT")) {
      if (++depth_ > kMaxDepth) return false;
      const bool ok = Negation(value);
      --depth_;
      if (!ok) return false;
      *value = !*value;
      return true;
    }
    return Comparison(value);
  }
  // As Windows Installer: a bare property is true when it has a value ("0" included), a bare
  // integer when it isn't 0; values compare as integers when both are, else an integer against
  // text is false (only <> holds) and text compares as text.
  struct Operand {
    std::string text;
    bool integer = false;   // the text is an integer
    bool literal = false;   // an integer written in the condition
    long long number = 0;
  };
  static Operand Classify(std::string text, bool literal_string) {
    Operand operand;
    operand.text = std::move(text);
    if (!literal_string && !operand.text.empty()) {
      char* end = nullptr;
      errno = 0;
      const auto number = std::strtoll(operand.text.c_str(), &end, 10);
      if (!errno && end && !*end) {
        operand.integer = true;
        operand.number = number;
      }
    }
    return operand;
  }
  bool Value(Operand* operand) {
    Space();
    if (at_ >= text_.size()) return false;
    const char first = text_[at_];
    if (first == '"') {
      const auto end = text_.find('"', at_ + 1);
      if (end == std::string_view::npos) return false;
      *operand = Classify(std::string(text_.substr(at_ + 1, end - at_ - 1)), true);
      at_ = end + 1;
      return true;
    }
    if (first == '-' || std::isdigit(static_cast<unsigned char>(first))) {
      std::size_t end = at_ + 1;
      while (end < text_.size() && std::isdigit(static_cast<unsigned char>(text_[end]))) ++end;
      *operand = Classify(std::string(text_.substr(at_, end - at_)), false);
      operand->literal = true;
      at_ = end;
      return !operand->text.empty() && operand->text != "-";
    }
    // Environment variables and component or feature states are not known: empty.
    bool state = false;
    if (first == '%' || first == '$' || first == '&' || first == '?' || first == '!') {
      state = true;
      ++at_;
    }
    std::size_t end = at_;
    while (end < text_.size() &&
           (std::isalnum(static_cast<unsigned char>(text_[end])) || text_[end] == '_' || text_[end] == '.'))
      ++end;
    if (end == at_) return false;
    const std::string name(text_.substr(at_, end - at_));
    at_ = end;
    const auto found = properties_.find(name);
    *operand = Classify(state || found == properties_.end() ? std::string() : found->second, false);
    return true;
  }
  static bool Truth(const Operand& operand) { return operand.literal ? operand.number != 0 : !operand.text.empty(); }
  bool Comparison(bool* value) {
    Space();
    if (at_ < text_.size() && text_[at_] == '(') {
      ++at_;
      if (++depth_ > kMaxDepth) return false;
      const bool ok = Implication(value);
      --depth_;
      if (!ok) return false;
      Space();
      if (at_ >= text_.size() || text_[at_] != ')') return false;
      ++at_;
      return true;
    }
    Operand left;
    if (!Value(&left)) return false;
    Space();
    static constexpr const char* kOperators[] = {"<>", "><", "<<", ">>", "<=", ">=", "=", "<", ">"};
    bool fold = false;
    std::size_t probe = at_;
    if (probe < text_.size() && text_[probe] == '~') {
      fold = true;
      ++probe;
    }
    std::string op;
    for (const auto* candidate : kOperators) {
      const std::size_t length = std::strlen(candidate);
      if (text_.substr(probe, length) == candidate) {
        op = candidate;
        break;
      }
    }
    if (op.empty()) {
      if (fold) return false;
      *value = Truth(left);
      return true;
    }
    at_ = probe + op.size();
    Operand right;
    if (!Value(&right)) return false;
    if ((left.literal && !right.integer) || (right.literal && !left.integer)) {
      *value = op == "<>";
      return true;
    }
    if (left.integer && right.integer) {
      const auto a = left.number, b = right.number;
      *value = op == "=" ? a == b : op == "<>" ? a != b : op == "<" ? a < b : op == ">" ? a > b
             : op == "<=" ? a <= b : op == ">=" ? a >= b : op == "><" ? (a & b) != 0
             : op == "<<" ? ((a >> 16) & 0xffff) == b : ((a & 0xffff) == b);
      return true;
    }
    const auto a = fold ? Lower(left.text) : left.text, b = fold ? Lower(right.text) : right.text;
    *value = op == "=" ? a == b : op == "<>" ? a != b : op == "<" ? a < b : op == ">" ? a > b
           : op == "<=" ? a <= b : op == ">=" ? a >= b : op == "><" ? a.find(b) != std::string::npos
           : op == "<<" ? a.rfind(b, 0) == 0 : (a.size() >= b.size() && a.compare(a.size() - b.size(), b.size(), b) == 0);
    return true;
  }
  static constexpr int kMaxDepth = 32;  // the console's worker stacks are small
  std::string_view text_;
  const std::map<std::string, std::string>& properties_;
  std::size_t at_ = 0;
  int depth_ = 0;
};

// ---------------------------------------------------------------------------------------------
// Values written into the bottle.

std::string WineString(std::string_view text) {
  std::string out = "\"";
  for (const char value : text) {
    if (value == '\\' || value == '"') out += '\\';
    out += value;
  }
  return out + "\"";
}

std::string WindowsPath(const std::string& relative) {
  std::string out = "C:\\" + relative;
  std::replace(out.begin(), out.end(), '/', '\\');
  return out;
}

// The bottle folders a package's standard directories stand for (64-bit Windows, all users).
// nullopt for per-user folders, which this installer leaves alone.
std::optional<std::string> StandardFolder(const std::string& name) {
  static const std::map<std::string, std::string> kFolders = {
      {"TARGETDIR", ""},
      {"ProgramFilesFolder", "Program Files (x86)"},
      {"ProgramFiles64Folder", "Program Files"},
      {"CommonFilesFolder", "Program Files (x86)/Common Files"},
      {"CommonFiles64Folder", "Program Files/Common Files"},
      {"SystemFolder", "windows/syswow64"},
      {"System64Folder", "windows/system32"},
      {"System16Folder", "windows/system"},
      {"WindowsFolder", "windows"},
      {"WindowsVolume", ""},
      {"CommonAppDataFolder", "ProgramData"},
      {"FontsFolder", "windows/Fonts"},
  };
  const auto found = kFolders.find(name);
  if (found != kFolders.end()) return found->second;
  return std::nullopt;
}

// "C:\\a\\b\\" as a path below drive_c ("a/b"); nullopt for another drive or a ".." part.
std::optional<std::string> DrivePath(const std::string& windows) {
  if (windows.size() < 3 || (windows[0] != 'C' && windows[0] != 'c') || windows[1] != ':' || windows[2] != '\\' ||
      !Ascii(windows))
    return std::nullopt;
  std::string out;
  for (std::size_t at = 3; at < windows.size();) {
    auto end = windows.find('\\', at);
    if (end == std::string::npos) end = windows.size();
    const auto part = windows.substr(at, end - at);
    at = end + 1;
    if (part.empty() || part == ".") continue;
    if (part == ".." || part.find('/') != std::string::npos || part.find(':') != std::string::npos) return std::nullopt;
    out += (out.empty() ? "" : "/") + part;
  }
  return out;
}

bool PerUserFolder(const std::string& name) {
  static const std::set<std::string> kFolders = {
      "AppDataFolder", "LocalAppDataFolder", "PersonalFolder", "DesktopFolder", "ProgramMenuFolder",
      "StartMenuFolder", "StartupFolder", "SendToFolder", "TemplateFolder", "FavoritesFolder", "NetHoodFolder",
      "PrintHoodFolder", "RecentFolder", "MyPicturesFolder", "AdminToolsFolder", "TempFolder"};
  return kFolders.count(name) != 0;
}

class PlanBuilder {
 public:
  explicit PlanBuilder(const Database& database) : database_(database) {}
  bool Build(MsiInstallPlan* plan, std::string* reason);

 private:
  std::optional<std::string> Directory(const std::string& key, int depth = 0);
  std::string Format(std::string_view text, bool* multi = nullptr) const;
  void AddRegistry(MsiInstallPlan* plan, bool machine, const std::string& key, const std::string& name,
                   const std::string& data, bool default_value);
  const Database& database_;
  std::map<std::string, std::string> properties_;
  std::map<std::string, std::string> declared_;  // the Property table as written
  std::map<std::string, std::pair<std::string, std::string>> directory_rows_;  // key → parent, name
  std::map<std::string, std::optional<std::string>> directories_;              // resolved, relative to drive_c
  std::map<std::string, std::string> file_paths_;                              // File key → Windows path
  std::map<std::string, std::string> component_paths_;                         // Component → Windows folder
  std::map<std::string, std::size_t> registry_index_;
};

std::optional<std::string> PlanBuilder::Directory(const std::string& key, int depth) {
  if (const auto found = directories_.find(key); found != directories_.end()) return found->second;
  std::optional<std::string> result;
  const auto row = directory_rows_.find(key);
  // A directory the Property table sets ("INSTALLDIR" = "C:\\Vendor\\") is placed there.
  if (const auto declared = declared_.find(key); declared != declared_.end())
    if (const auto placed = DrivePath(declared->second)) {
      directories_[key] = placed;
      return placed;
    }
  if (const auto standard = StandardFolder(key)) {
    result = standard;
  } else if (PerUserFolder(key) || row == directory_rows_.end() || depth > 64) {
    result = std::nullopt;
  } else {
    const auto& [parent, name] = row->second;
    if (parent.empty() || parent == key) {
      result = std::string();  // another root: the system drive, like TARGETDIR
    } else if (const auto base = Directory(parent, depth + 1)) {
      const auto component = LongName(name);
      result = component == "." || component.empty() ? *base : (base->empty() ? component : *base + "/" + component);
    }
  }
  directories_[key] = result;
  return result;
}

// Formatted text: [Property], [#File], [!File], [$Component], [\x] and [~]; environment
// variables ([%NAME]) are empty. multi is set when [~] appears.
std::string PlanBuilder::Format(std::string_view text, bool* multi) const {
  std::string out;
  for (std::size_t at = 0; at < text.size();) {
    if (text[at] != '[') {
      out += text[at++];
      continue;
    }
    const auto end = text.find(']', at + 1);
    if (end == std::string_view::npos) {
      out += text.substr(at);
      break;
    }
    const std::string token(text.substr(at + 1, end - at - 1));
    at = end + 1;
    if (token == "~") {
      if (multi) *multi = true;
      out += '\0';
    } else if (token.size() == 2 && token[0] == '\\') {
      out += token[1];
    } else if (!token.empty() && (token[0] == '#' || token[0] == '!')) {
      if (const auto found = file_paths_.find(token.substr(1)); found != file_paths_.end()) out += found->second;
    } else if (!token.empty() && token[0] == '$') {
      if (const auto found = component_paths_.find(token.substr(1)); found != component_paths_.end()) out += found->second;
    } else if (!token.empty() && token[0] == '%') {
      // Environment variables are not known.
    } else if (const auto found = properties_.find(token); found != properties_.end()) {
      out += found->second;
    }
  }
  return out;
}

void PlanBuilder::AddRegistry(MsiInstallPlan* plan, bool machine, const std::string& key, const std::string& name,
                              const std::string& data, bool default_value) {
  auto& list = machine ? plan->machine : plan->user;
  const auto identity = std::string(machine ? "m:" : "u:") + Lower(key) + "\n" + (default_value ? "@" : Lower(name));
  if (const auto found = registry_index_.find(identity); found != registry_index_.end()) {
    list[found->second].data = data;  // a later row wins, as when the installer writes both
    return;
  }
  registry_index_[identity] = list.size();
  RegistryValue value;
  value.key = key;
  value.name = default_value ? std::string() : name;
  value.data = data;
  value.default_value = default_value;
  list.push_back(std::move(value));
}

bool PlanBuilder::Build(MsiInstallPlan* plan, std::string* reason) {
  const auto fail = [&](const char* why) {
    if (reason) *reason = why;
    return false;
  };
  *plan = MsiInstallPlan{};
  Table property, directory, component, feature, feature_components, condition, file, media, registry, environment,
      custom_action;
  for (const auto& [name, table] : std::initializer_list<std::pair<const char*, Table*>>{
           {"Property", &property}, {"Directory", &directory}, {"Component", &component}, {"Feature", &feature},
           {"FeatureComponents", &feature_components}, {"Condition", &condition}, {"File", &file}, {"Media", &media},
           {"Registry", &registry}, {"Environment", &environment}, {"CustomAction", &custom_action}})
    if (!database_.ReadTable(name, table)) return fail("a package table could not be read");
  plan->custom_actions = custom_action.rows.size();

  // A quiet, per-machine install on 64-bit Windows 7.
  properties_ = {{"VersionNT", "601"}, {"VersionNT64", "601"}, {"WindowsBuild", "7601"}, {"ServicePackLevel", "1"},
                 {"Intel", "6"}, {"Msix64", "6"}, {"Privileged", "1"}, {"AdminUser", "1"}, {"INSTALLLEVEL", "1"},
                 {"UILevel", "2"}};
  for (const auto& row : property.rows) {
    const auto& name = At(property, row, "Property");
    if (!name.null) properties_[name.text] = declared_[name.text] = At(property, row, "Value").text;
  }
  properties_["ALLUSERS"] = "1";
  plan->product_code = properties_["ProductCode"];
  plan->product_name = properties_["ProductName"];
  plan->product_version = properties_["ProductVersion"];

  for (const auto& row : directory.rows)
    directory_rows_[At(directory, row, "Directory").text] = {At(directory, row, "Directory_Parent").text,
                                                             At(directory, row, "DefaultDir").text};
  for (const auto& [key, unused] : directory_rows_) {
    (void)unused;
    if (const auto path = Directory(key)) properties_[key] = WindowsPath(*path) + (path->empty() ? "" : "\\");
  }

  // Features at a level from 1 to INSTALLLEVEL, after their Condition rows, under selected parents.
  const long long install_level = std::strtoll(properties_["INSTALLLEVEL"].c_str(), nullptr, 10);
  std::map<std::string, std::pair<std::string, long long>> features;  // name → parent, level
  for (const auto& row : feature.rows)
    features[At(feature, row, "Feature").text] = {At(feature, row, "Feature_Parent").text, At(feature, row, "Level").number};
  for (const auto& row : condition.rows) {
    const auto found = features.find(At(condition, row, "Feature_").text);
    if (found == features.end()) continue;
    if (EvaluateMsiCondition(At(condition, row, "Condition").text, properties_).value_or(false))
      found->second.second = At(condition, row, "Level").number;
  }
  std::set<std::string> selected_features;
  for (const auto& [name, info] : features) {
    bool selected = true;
    std::string current = name;
    for (int depth = 0; selected && !current.empty() && depth < 64; ++depth) {
      const auto found = features.find(current);
      if (found == features.end()) break;
      const auto level = found->second.second;
      selected = level >= 1 && level <= install_level;
      current = found->second.first == current ? std::string() : found->second.first;
    }
    if (selected) selected_features.insert(name);
  }
  std::set<std::string> selected_components;
  for (const auto& row : feature_components.rows)
    if (selected_features.count(At(feature_components, row, "Feature_").text))
      selected_components.insert(At(feature_components, row, "Component_").text);
  std::map<std::string, std::pair<std::string, bool>> components;  // name → directory key, 64-bit
  for (const auto& row : component.rows) {
    const auto& name = At(component, row, "Component").text;
    const auto& when = At(component, row, "Condition");
    if (!selected_components.count(name)) continue;
    // A condition this reader can't parse doesn't keep the component out.
    if (!when.null && !when.text.empty() && !EvaluateMsiCondition(when.text, properties_).value_or(true)) continue;
    components[name] = {At(component, row, "Directory_").text, (At(component, row, "Attributes").number & 0x100) != 0};
    if (const auto path = Directory(At(component, row, "Directory_").text))
      component_paths_[name] = WindowsPath(*path) + (path->empty() ? "" : "\\");
  }

  // Files, each from the cabinet of the first Media row whose last sequence reaches it.
  std::vector<std::pair<long long, std::string>> media_rows;
  for (const auto& row : media.rows)
    media_rows.emplace_back(At(media, row, "LastSequence").number, At(media, row, "Cabinet").text);
  std::sort(media_rows.begin(), media_rows.end());
  std::map<std::string, std::size_t> cabinet_index;
  for (const auto& row : file.rows) {
    const auto& key = At(file, row, "File").text;
    const auto owner = components.find(At(file, row, "Component_").text);
    const auto name = LongName(At(file, row, "FileName").text);
    if (owner == components.end()) continue;
    const auto folder = Directory(owner->second.first);
    if (folder) file_paths_[key] = WindowsPath(folder->empty() ? name : *folder + "/" + name);
    const auto sequence = At(file, row, "Sequence").number;
    const auto attributes = At(file, row, "Attributes").number;
    const auto disk = std::find_if(media_rows.begin(), media_rows.end(),
                                   [&](const auto& media_row) { return media_row.first >= sequence; });
    if (!folder) {
      ++plan->skipped_files;  // a per-user folder
      continue;
    }
    if (name.empty() || name == "." || name == ".." || name.find('/') != std::string::npos ||
        name.find('\\') != std::string::npos || !Ascii(name) || disk == media_rows.end() || disk->second.empty() ||
        (attributes & 0x2000)) {
      ++plan->unsupported_files;  // uncompressed beside the package, or a name the bottle can't hold
      continue;
    }
    const bool embedded = disk->second[0] == '#';
    const auto cabinet_name = embedded ? disk->second.substr(1) : disk->second;
    auto found = cabinet_index.find(disk->second);
    if (found == cabinet_index.end()) {
      found = cabinet_index.emplace(disk->second, plan->cabinets.size()).first;
      plan->cabinets.push_back({cabinet_name, embedded});
    }
    MsiFile entry;
    entry.target = folder->empty() ? name : *folder + "/" + name;
    entry.member = key;
    entry.cabinet = found->second;
    entry.size = static_cast<std::uint32_t>(At(file, row, "FileSize").number);
    plan->files.push_back(std::move(entry));
  }

  // Registry rows of the selected components.
  for (const auto& row : registry.rows) {
    const auto owner = components.find(At(registry, row, "Component_").text);
    if (owner == components.end()) continue;
    const bool wide = owner->second.second;
    const auto root = At(registry, row, "Root").number;
    const auto& raw_name = At(registry, row, "Name");
    const auto& raw_value = At(registry, row, "Value");
    if (raw_value.null && (raw_name.null || raw_name.text == "+" || raw_name.text == "-" || raw_name.text == "*"))
      continue;  // only creates or removes the key
    auto key = Format(At(registry, row, "Key").text);
    const auto name = raw_name.null ? std::string() : Format(raw_name.text);
    std::string data;
    const auto& value = raw_value.text;
    bool multi = false;
    if (value.rfind("#x", 0) == 0 || value.rfind("#X", 0) == 0) {
      ++plan->skipped_registry;
      continue;
    } else if (value.rfind("#%", 0) == 0) {
      data = "str(2):" + WineString(Format(value.substr(2), &multi));
    } else if (value.rfind("##", 0) == 0) {
      data = WineString("#" + Format(value.substr(2), &multi));
    } else if (value.rfind("#", 0) == 0) {
      const auto number = Format(value.substr(1), &multi);
      char* end = nullptr;
      errno = 0;
      const auto parsed = std::strtoll(number.c_str(), &end, 10);
      if (number.empty() || errno || *end || parsed < -2147483648ll || parsed > 0xffffffffll) {
        ++plan->skipped_registry;
        continue;
      }
      char text[24];
      std::snprintf(text, sizeof(text), "dword:%08x", static_cast<unsigned>(static_cast<std::uint32_t>(parsed)));
      data = text;
    } else {
      data = WineString(Format(value, &multi));
    }
    // Wine's registry text can't take these unescaped in a key or value name.
    if (multi || !Ascii(key) || !Ascii(name) || !Ascii(data) || key.empty() || name.find('"') != std::string::npos ||
        name.find('\\') != std::string::npos || key.find('[') != std::string::npos || key.find(']') != std::string::npos ||
        key.find('"') != std::string::npos) {
      ++plan->skipped_registry;
      continue;
    }
    bool machine = true;
    if (root == 1) {
      machine = false;
    } else if (root == 0) {
      key = std::string(wide ? "Software\\Classes\\" : "Software\\Classes\\Wow6432Node\\") + key;
    } else if (root == 2 || root == -1) {
      const auto lower = Lower(key);
      if (!wide && lower.rfind("software\\", 0) == 0 && lower.rfind("software\\wow6432node\\", 0) != 0 &&
          lower.rfind("software\\classes\\", 0) != 0)
        key = "Software\\Wow6432Node\\" + key.substr(9);
    } else {
      ++plan->skipped_registry;
      continue;
    }
    AddRegistry(plan, machine, key, name, data, raw_name.null || name.empty());
  }

  // Environment rows: folders the package adds to the machine-wide PATH. Other variables, and
  // values that replace PATH instead of extending it, are left alone.
  for (const auto& row : environment.rows) {
    if (!components.count(At(environment, row, "Component_").text)) continue;
    auto name = At(environment, row, "Name").text;
    bool system = false, remove = false;
    while (!name.empty() && std::strchr("=+-!*", name[0])) {
      system = system || name[0] == '*';
      remove = remove || name[0] == '!';
      name.erase(0, 1);
    }
    const auto& value = At(environment, row, "Value").text;
    const bool append = value.rfind("[~];", 0) == 0;
    const bool prepend = value.size() >= 4 && value.compare(value.size() - 4, 4, ";[~]") == 0;
    if (!system || remove || Lower(name) != "path" || (!append && !prepend)) {
      ++plan->skipped_registry;
      continue;
    }
    const auto entry = Format(append ? value.substr(4) : value.substr(0, value.size() - 4));
    if (entry.empty() || !Ascii(entry) || entry.find(';') != std::string::npos) {
      ++plan->skipped_registry;
      continue;
    }
    (append ? plan->path_append : plan->path_prepend).push_back(entry);
  }
  return true;
}

}  // namespace

bool ReadMsiPackage(std::string_view package, MsiInstallPlan* plan, std::string* reason) {
  Database database;
  if (!database.Open(package, reason)) return false;
  PlanBuilder builder(database);
  return builder.Build(plan, reason);
}

std::optional<std::string_view> ViewMsiStream(std::string_view package, const std::string& name) {
  Compound compound;
  std::string reason;
  if (!compound.Open(package, &reason)) return std::nullopt;
  const auto* entry = compound.Find(name);
  return entry ? compound.View(*entry) : std::nullopt;
}

std::optional<std::string> ReadMsiStream(std::string_view package, const std::string& name) {
  Compound compound;
  std::string reason, out;
  if (!compound.Open(package, &reason)) return std::nullopt;
  const auto* entry = compound.Find(name);
  if (!entry || !compound.Read(*entry, &out)) return std::nullopt;
  return out;
}

std::optional<bool> EvaluateMsiCondition(std::string_view condition, const std::map<std::string, std::string>& properties) {
  std::size_t first = 0;
  while (first < condition.size() && (condition[first] == ' ' || condition[first] == '\t')) ++first;
  if (first == condition.size()) return true;
  return ConditionParser(condition, properties).Parse();
}

bool ExtractCabinet(std::string_view cabinet, const std::function<bool(const std::string&, std::string&)>& sink,
                    std::string* reason, const std::function<bool(const std::string&)>& wants) {
  const auto fail = [&](const char* why) {
    if (reason) *reason = why;
    return false;
  };
  if (cabinet.size() < 36 || cabinet.substr(0, 4) != "MSCF") return fail("not a cabinet");
  const auto files_offset = U32(cabinet, 16);
  const auto folder_count = U16(cabinet, 26);
  const auto file_count = U16(cabinet, 28);
  const auto flags = U16(cabinet, 30);
  if (flags & 0x3) return fail("cabinets that continue into another one are not supported");
  std::size_t at = 36;
  std::size_t folder_reserve = 0, data_reserve = 0;
  if (flags & 0x4) {
    if (at + 4 > cabinet.size()) return fail("damaged cabinet");
    const auto header_reserve = U16(cabinet, at);
    folder_reserve = static_cast<unsigned char>(cabinet[at + 2]);
    data_reserve = static_cast<unsigned char>(cabinet[at + 3]);
    at += 4 + header_reserve;
  }
  struct Folder {
    std::uint32_t data_offset;
    std::uint16_t blocks;
    std::uint16_t compression;
  };
  std::vector<Folder> folders;
  for (std::size_t index = 0; index < folder_count; ++index, at += 8 + folder_reserve) {
    if (at + 8 > cabinet.size()) return fail("damaged cabinet");
    folders.push_back({U32(cabinet, at), U16(cabinet, at + 4), U16(cabinet, at + 6)});
  }
  struct Member {
    std::string name;
    std::uint32_t size, offset;
    std::uint16_t folder;
  };
  std::vector<Member> members;
  at = files_offset;
  for (std::size_t index = 0; index < file_count; ++index) {
    if (at + 16 > cabinet.size()) return fail("damaged cabinet");
    Member member{{}, U32(cabinet, at), U32(cabinet, at + 4), U16(cabinet, at + 8)};
    const auto end = cabinet.find('\0', at + 16);
    if (end == std::string_view::npos) return fail("damaged cabinet");
    member.name = std::string(cabinet.substr(at + 16, end - at - 16));
    at = end + 1;
    if (member.folder >= folders.size()) return fail("cabinets that continue into another one are not supported");
    members.push_back(std::move(member));
  }
  // Each folder is one stream; MSZIP blocks are deflate streams that may refer back into the
  // previous block's 32 KiB, so that history stays in front of each block's output.
  constexpr std::size_t kWindow = 32768;
  auto buffer = std::make_unique<unsigned char[]>(kWindow * 2 + 6144);
  auto decompressor = std::make_unique<tinfl_decompressor>();
  for (std::size_t folder_index = 0; folder_index < folders.size(); ++folder_index) {
    const auto& folder = folders[folder_index];
    const auto kind = folder.compression & 0x0f;
    if (kind != 0 && kind != 1 && kind != 3) return fail("Quantum cabinets are not supported");
    std::vector<const Member*> wanted;
    for (const auto& member : members)
      if (member.folder == folder_index && (!wants || wants(member.name))) wanted.push_back(&member);
    if (wanted.empty()) continue;
    std::sort(wanted.begin(), wanted.end(), [](const Member* a, const Member* b) { return a->offset < b->offset; });
    for (std::size_t index = 0; index < wanted.size(); ++index) {
      if (wanted[index]->size > kMaxStreamBytes) return fail("a cabinet member is too large");
      if (index + 1 < wanted.size() &&
          static_cast<std::uint64_t>(wanted[index]->offset) + wanted[index]->size > wanted[index + 1]->offset)
        return fail("damaged cabinet: members overlap");
    }
    // What the folder decodes to, by its blocks' headers: a member listed past its end is refused before any
    // of it is collected, so its size is never reserved.
    std::uint64_t folder_bytes = 0;
    for (std::size_t block = 0, header = folder.data_offset; block < folder.blocks; ++block) {
      if (header + 8 + data_reserve > cabinet.size()) return fail("damaged cabinet");
      folder_bytes += U16(cabinet, header + 6);
      header += 8 + data_reserve + U16(cabinet, header + 4);
    }
    for (const auto* member : wanted)
      if (static_cast<std::uint64_t>(member->offset) + member->size > folder_bytes)
        return fail("damaged cabinet: files run past their folder");
    std::size_t next = 0;
    std::string current;  // the member being collected
    std::string empty;
    std::uint64_t position = 0;  // bytes of the folder produced so far
    std::size_t history = 0;
    std::size_t block_at = folder.data_offset;
    const auto deliver = [&](const unsigned char* bytes, std::size_t length) {
      for (std::size_t used = 0; used < length;) {
        while (next < wanted.size() && wanted[next]->size == 0 && wanted[next]->offset <= position) {
          empty.clear();
          if (!sink(wanted[next]->name, empty)) return false;
          ++next;
        }
        if (next >= wanted.size()) return true;
        const auto* member = wanted[next];
        if (position + (length - used) <= member->offset) {
          position += length - used;
          return true;
        }
        if (position < member->offset) {
          const auto skip = static_cast<std::size_t>(member->offset - position);
          used += skip;
          position += skip;
        }
        // Its exact size up front: grown a piece at a time, a large member would briefly be held twice
        // (Microsoft's VC_redist carries an 11 MB one).
        if (current.empty()) current.reserve(member->size);
        const auto want = static_cast<std::size_t>(member->size - current.size());
        const auto take = std::min(want, length - used);
        current.append(reinterpret_cast<const char*>(bytes + used), take);
        used += take;
        position += take;
        if (current.size() == member->size) {
          if (!sink(member->name, current)) return false;
          current = std::string();  // its memory goes back now, not when the next member is as large
          ++next;
        }
      }
      return true;
    };
    if (kind == 3) {
      // LZX: the folder's blocks are one stream, read in place, each block one frame of it.
      std::vector<std::string_view> stream;
      std::vector<std::uint16_t> frames;
      for (std::size_t block = 0; block < folder.blocks; ++block) {
        if (block_at + 8 + data_reserve > cabinet.size()) return fail("damaged cabinet");
        const auto packed = U16(cabinet, block_at + 4);
        const auto unpacked = U16(cabinet, block_at + 6);
        const auto data_at = block_at + 8 + data_reserve;
        if (data_at + packed > cabinet.size() || unpacked > kWindow) return fail("damaged cabinet");
        stream.push_back(cabinet.substr(data_at, packed));
        frames.push_back(unpacked);
        block_at = data_at + packed;
      }
      LzxDecoder lzx((folder.compression >> 8) & 0x1f, std::move(stream));
      for (const auto unpacked : frames) {
        std::string why;
        if (!lzx.DecodeFrame(unpacked, buffer.get(), &why)) {
          if (reason) *reason = why;
          return false;
        }
        if (!deliver(buffer.get(), unpacked)) return fail("extraction stopped");
        if (next >= wanted.size()) break;  // the rest of the folder holds nothing wanted
      }
    }
    for (std::size_t block = 0; kind != 3 && block < folder.blocks; ++block) {
      if (block_at + 8 + data_reserve > cabinet.size()) return fail("damaged cabinet");
      const auto packed = U16(cabinet, block_at + 4);
      const auto unpacked = U16(cabinet, block_at + 6);
      const auto data_at = block_at + 8 + data_reserve;
      if (data_at + packed > cabinet.size() || unpacked > kWindow) return fail("damaged cabinet");
      const auto* input = reinterpret_cast<const unsigned char*>(cabinet.data() + data_at);
      unsigned char* output = buffer.get() + history;
      if (kind == 0) {
        if (packed != unpacked) return fail("damaged cabinet");
        std::memcpy(output, input, unpacked);
      } else {
        if (packed < 2 || input[0] != 'C' || input[1] != 'K') return fail("damaged MSZIP block");
        tinfl_init(decompressor.get());
        std::size_t in_size = packed - 2u, out_size = unpacked;
        const auto status = tinfl_decompress(decompressor.get(), input + 2, &in_size, buffer.get(), output, &out_size,
                                             TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
        if (status != TINFL_STATUS_DONE || out_size != unpacked) return fail("damaged MSZIP block");
      }
      if (!deliver(output, unpacked)) return fail("extraction stopped");
      const auto total = history + unpacked;
      history = std::min(total, kWindow);
      std::memmove(buffer.get(), buffer.get() + total - history, history);
      block_at = data_at + packed;
      if (next >= wanted.size()) break;  // the rest of the folder holds nothing wanted
    }
    // Empty members at the folder's end.
    for (; next < wanted.size() && wanted[next]->size == 0 && wanted[next]->offset <= position; ++next) {
      empty.clear();
      if (!sink(wanted[next]->name, empty)) return fail("extraction stopped");
    }
    if (next < wanted.size()) return fail("damaged cabinet: files run past their folder");
  }
  return true;
}

}  // namespace playden::runtime
