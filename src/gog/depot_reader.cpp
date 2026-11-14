// SPDX-License-Identifier: GPL-3.0-or-later
#include "gog/depot_reader.h"

#include "gog/hash.h"
#include "steam/backend/json.h"

#include <algorithm>
#include <utility>

namespace playden::gog {
namespace {

using steam::json::Value;

// The structural stack outside items[] never needs to be deep: {"depot":{"items":[…]}} is three levels.
constexpr std::size_t kMaxScanDepth = 64;
// Keys compared against "depot" and "items"; a longer key is simply none of them.
constexpr std::size_t kMaxKeyBytes = 32;
// A number or literal outside items[] (the manifest's version).
constexpr std::size_t kMaxScalarBytes = 64;
// An item buffer that grew past this is freed after its item, so one large item doesn't pin memory.
constexpr std::size_t kKeepItemCapacity = 1024 * 1024;

bool Whitespace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

bool ScalarCharacter(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '+' || c == '-' ||
         c == '.';
}

// A JSON integer from 1 to `max`.
bool PositiveSize(const Value* value, std::uint64_t max, std::uint64_t* out) {
  if (!value || !value->is_number()) return false;
  const std::string& text = value->text();
  if (text.empty() || text.size() > 16 || text.front() == '0') return false;
  std::uint64_t result = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') return false;
    result = result * 10 + static_cast<std::uint64_t>(c - '0');
  }
  if (result > max) return false;
  *out = result;
  return true;
}

bool Hash(const Value* value, bool (*valid)(std::string_view), std::string* out) {
  if (!value || !value->is_string() || !valid(value->text())) return false;
  *out = value->text();
  return true;
}

}  // namespace

bool ParseDepotItemV2(const Value& value, DepotItem* item) {
  if (!value.is_object()) return false;
  const Value* type = value.Find("type");
  if (!type || !type->is_string()) return false;
  if (type->text() == "DepotFile") item->type = DepotItem::Type::kFile;
  else if (type->text() == "DepotDirectory") item->type = DepotItem::Type::kDirectory;
  else if (type->text() == "DepotLink") item->type = DepotItem::Type::kLink;
  else item->type = DepotItem::Type::kOther;
  const Value* path = value.Find("path");
  if (!path || !path->is_string()) return false;
  item->path = path->text();
  if (const Value* flags = value.Find("flags")) {
    if (!flags->is_array()) return false;
    for (const auto& flag : flags->array()) {
      if (!flag.is_string()) return false;
      if (flag.text() == "support") item->support = true;
      else if (flag.text() == "executable") item->executable = true;
    }
  }
  if (item->type != DepotItem::Type::kFile) return true;

  if (const Value* chunks = value.Find("chunks")) {
    if (!chunks->is_array()) return false;
    item->chunks.reserve(chunks->array().size());
    for (const auto& entry : chunks->array()) {
      if (!entry.is_object()) return false;
      DepotChunk chunk;
      if (!Hash(entry.Find("compressedMd5"), &IsMd5Hex, &chunk.compressed_md5) ||
          !Hash(entry.Find("md5"), &IsMd5Hex, &chunk.md5) ||
          !PositiveSize(entry.Find("compressedSize"), kMaxChunkBytes, &chunk.compressed_size) ||
          !PositiveSize(entry.Find("size"), kMaxChunkBytes, &chunk.size))
        return false;
      item->size += chunk.size;
      if (item->size > kMaxFileBytes) return false;
      item->chunks.push_back(std::move(chunk));
    }
  }
  if (const Value* md5 = value.Find("md5"); md5 && !Hash(md5, &IsMd5Hex, &item->md5)) return false;
  if (const Value* sha256 = value.Find("sha256"); sha256 && !Hash(sha256, &IsSha256Hex, &item->sha256)) return false;
  if (item->md5.empty() && item->chunks.size() == 1) item->md5 = item->chunks.front().md5;
  return true;
}

DepotItemReader::DepotItemReader(Callback callback) : callback_(std::move(callback)) {}

DepotItemReader::DepotItemReader(Callback callback, std::string array_key, DepotElementParser parser)
    : callback_(std::move(callback)), array_key_(std::move(array_key)), parser_(parser) {
  // A key past kMaxKeyBytes is never seen whole, and no parser means no element is ever good.
  if (array_key_.empty() || array_key_.size() > kMaxKeyBytes || !parser_) error_ = Status::kMalformed;
}

DepotItemReader::Status DepotItemReader::Feed(const std::uint8_t* data, std::size_t size) {
  if (error_ != Status::kOk) return error_;
  if (size > kMaxDepotManifestBytes - total_bytes_) return Fail(Status::kTooLarge);
  total_bytes_ += size;
  for (std::size_t i = 0; i < size; ++i) {
    const Status status = Byte(static_cast<char>(data[i]));
    if (status != Status::kOk) return Fail(status);
  }
  return Status::kOk;
}

DepotItemReader::Status DepotItemReader::Finish() {
  if (error_ != Status::kOk) return error_;
  if (!done_ || lex_ != Lex::kNone || !seen_items_ || !items_closed_) return Fail(Status::kMalformed);
  return Status::kOk;
}

DepotItemReader::Status DepotItemReader::Byte(char c) {
  switch (lex_) {
    case Lex::kCapture: {
      if (item_.size() >= kMaxItemBytes) return Status::kTooLarge;
      item_.push_back(c);
      if (item_in_string_) {
        if (item_escape_) item_escape_ = false;
        else if (c == '\\') item_escape_ = true;
        else if (c == '"') item_in_string_ = false;
        return Status::kOk;
      }
      if (c == '"') {
        item_in_string_ = true;
      } else if (c == '{' || c == '[') {
        if (++item_depth_ > kMaxItemDepth + 1) return Status::kMalformed;
      } else if (c == '}' || c == ']') {
        if (--item_depth_ == 0) return EmitItem();
      }
      return Status::kOk;
    }
    case Lex::kString: {
      if (escape_) {
        escape_ = false;
        return Status::kOk;
      }
      if (c == '\\') {
        escape_ = true;
        key_plain_ = false;  // an escaped key is none Playden looks for
        return Status::kOk;
      }
      if (c == '"') {
        lex_ = Lex::kNone;
        if (string_is_key_) {
          stack_.back().expect = Expect::kColon;
          last_key_ = key_plain_ ? key_ : std::string();
        }
        return Status::kOk;
      }
      if (static_cast<unsigned char>(c) < 0x20) return Status::kMalformed;
      if (string_is_key_ && key_plain_) {
        if (key_.size() < kMaxKeyBytes) key_.push_back(c);
        else key_plain_ = false;
      }
      return Status::kOk;
    }
    case Lex::kScalar: {
      if (ScalarCharacter(c)) {
        if (scalar_.size() >= kMaxScalarBytes) return Status::kMalformed;
        scalar_.push_back(c);
        return Status::kOk;
      }
      const Status status = EndScalar();
      if (status != Status::kOk) return status;
      return Structural(c);
    }
    case Lex::kNone: return Structural(c);
  }
  return Status::kMalformed;
}

DepotItemReader::Status DepotItemReader::Structural(char c) {
  if (Whitespace(c)) return Status::kOk;
  if (done_) return Status::kMalformed;
  if (stack_.empty()) {
    if (c != '{') return Status::kMalformed;  // the manifest is an object
    started_ = true;
    stack_.push_back({true, Role::kRoot, Expect::kKeyOrEnd});
    return Status::kOk;
  }
  Frame& frame = stack_.back();
  if (frame.object) {
    switch (frame.expect) {
      case Expect::kKeyOrEnd:
        if (c == '}') return Close();
        [[fallthrough]];
      case Expect::kKey:
        if (c != '"') return Status::kMalformed;
        lex_ = Lex::kString;
        string_is_key_ = true;
        escape_ = false;
        key_.clear();
        key_plain_ = true;
        return Status::kOk;
      case Expect::kColon:
        if (c != ':') return Status::kMalformed;
        frame.expect = Expect::kValue;
        return Status::kOk;
      case Expect::kValue: {
        Role role = Role::kOther;
        if (frame.role == Role::kRoot && last_key_ == "depot") {
          if (seen_depot_ || c != '{') return Status::kMalformed;
          seen_depot_ = true;
          role = Role::kDepot;
        } else if (frame.role == Role::kDepot && last_key_ == array_key_) {
          if (seen_items_ || c != '[') return Status::kMalformed;
          seen_items_ = true;
          role = Role::kItems;
        }
        frame.expect = Expect::kCommaOrEnd;
        return BeginValue(c, role);
      }
      case Expect::kCommaOrEnd:
        if (c == ',') {
          frame.expect = Expect::kKey;
          return Status::kOk;
        }
        if (c == '}') return Close();
        return Status::kMalformed;
      case Expect::kValueOrEnd: break;
    }
    return Status::kMalformed;
  }
  switch (frame.expect) {
    case Expect::kValueOrEnd:
      if (c == ']') return Close();
      [[fallthrough]];
    case Expect::kValue:
      frame.expect = Expect::kCommaOrEnd;
      if (frame.role == Role::kItems) {
        if (c != '{') return Status::kMalformed;  // every element of the array is an object
        lex_ = Lex::kCapture;
        item_.assign(1, c);
        item_depth_ = 1;
        item_in_string_ = false;
        item_escape_ = false;
        return Status::kOk;
      }
      return BeginValue(c, Role::kOther);
    case Expect::kCommaOrEnd:
      if (c == ',') {
        frame.expect = Expect::kValue;
        return Status::kOk;
      }
      if (c == ']') return Close();
      return Status::kMalformed;
    default: break;
  }
  return Status::kMalformed;
}

DepotItemReader::Status DepotItemReader::BeginValue(char c, Role role) {
  if (c == '{' || c == '[') {
    if (stack_.size() >= kMaxScanDepth) return Status::kMalformed;
    const bool object = c == '{';
    stack_.push_back({object, role, object ? Expect::kKeyOrEnd : Expect::kValueOrEnd});
    return Status::kOk;
  }
  if (c == '"') {
    lex_ = Lex::kString;
    string_is_key_ = false;
    escape_ = false;
    return Status::kOk;
  }
  if (c == '-' || (c >= '0' && c <= '9') || c == 't' || c == 'f' || c == 'n') {
    lex_ = Lex::kScalar;
    scalar_.assign(1, c);
    return Status::kOk;
  }
  return Status::kMalformed;
}

DepotItemReader::Status DepotItemReader::EndScalar() {
  lex_ = Lex::kNone;
  steam::json::Limits limits;
  limits.max_bytes = kMaxScalarBytes;
  limits.max_values = 1;
  return steam::json::Parse(scalar_, limits) ? Status::kOk : Status::kMalformed;
}

DepotItemReader::Status DepotItemReader::Close() {
  const Role role = stack_.back().role;
  stack_.pop_back();
  if (role == Role::kItems) items_closed_ = true;
  if (stack_.empty()) done_ = true;
  return Status::kOk;
}

DepotItemReader::Status DepotItemReader::EmitItem() {
  lex_ = Lex::kNone;
  peak_item_bytes_ = std::max(peak_item_bytes_, item_.size());
  if (items_ >= kMaxDepotItems) return Status::kTooLarge;
  steam::json::Limits limits;
  limits.max_bytes = kMaxItemBytes;
  limits.max_values = kMaxItemValues;
  limits.max_depth = kMaxItemDepth;
  DepotItem item;
  {
    const auto value = steam::json::Parse(item_, limits);
    if (!value || !parser_(*value, &item)) return Status::kMalformed;
  }
  ++items_;
  item_.clear();
  if (item_.capacity() > kKeepItemCapacity) std::string().swap(item_);
  if (!callback_(item)) return Status::kRefused;
  return Status::kOk;
}

const char* DepotReaderStatusName(DepotItemReader::Status status) {
  switch (status) {
    case DepotItemReader::Status::kOk: return "ok";
    case DepotItemReader::Status::kMalformed: return "malformed";
    case DepotItemReader::Status::kTooLarge: return "too_large";
    case DepotItemReader::Status::kRefused: return "refused";
  }
  return "malformed";
}

}  // namespace playden::gog
