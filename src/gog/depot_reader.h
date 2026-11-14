// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// A streaming reader for depot manifests: gen 2's ({"depot":{"items":[…]}, "version":2}, inflated) and, with another
// array key and element parser, gen 1's ({"depot":{"files":[…]}, "version":1}, plain JSON; manifest_v1.h). Large games
// have tens of thousands of items in many MiB, far past steam::json's defaults, so the reader never holds the whole
// manifest: it scans the bytes for the `depot` object and its array (whatever the key order), copies one element at a
// time into a bounded buffer, parses that element alone and hands the item out. Its container stack is on the heap and
// nothing recurses but the per-item parse, whose depth is capped.
//
// The small-files container and sfcRef are skipped: every file lists its own chunks. Paths are handed out raw (`\` or
// `/`); path_rules makes them safe.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace playden::steam::json {
class Value;
}  // namespace playden::steam::json

namespace playden::gog {

inline constexpr std::size_t kMaxItemBytes = 8 * 1024 * 1024;
inline constexpr std::size_t kMaxItemValues = 262'144;
inline constexpr std::size_t kMaxItemDepth = 8;
inline constexpr std::size_t kMaxDepotItems = 500'000;
// The inflated manifest: callers pass this cap to ZlibStream; the reader checks it too.
inline constexpr std::uint64_t kMaxDepotManifestBytes = 256ull * 1024 * 1024;
inline constexpr std::uint64_t kMaxChunkBytes = 64ull * 1024 * 1024;
inline constexpr std::uint64_t kMaxFileBytes = 1ull << 40;  // 1 TiB

struct DepotChunk {
  std::string compressed_md5;
  std::string md5;
  std::uint64_t compressed_size = 0;
  std::uint64_t size = 0;
};

struct DepotItem {
  enum class Type { kFile, kDirectory, kLink, kOther };
  Type type = Type::kOther;
  std::string path;  // raw, as the manifest writes it
  std::vector<DepotChunk> chunks;  // gen 2; empty for gen 1
  std::uint64_t size = 0;  // gen 2: the sum of the chunks' sizes; gen 1: the file's size
  std::string md5;         // the whole file's; a single-chunk file without one takes its chunk's
  std::string sha256;      // empty when the manifest has none
  bool support = false;    // flags: "support" (gen 1: `support`)
  bool executable = false; // flags: "executable" (no use on the console)
  std::uint64_t offset = 0;        // gen 1: where the file starts in its blob (`main.bin`)
  std::uint64_t blob_product = 0;  // gen 1: the product of `url` ("<product>/main.bin"); 0 without a url
};

// Turns one element of the manifest's array, parsed on its own, into an item. False refuses the manifest (kMalformed).
using DepotElementParser = bool (*)(const steam::json::Value& element, DepotItem* item);

// The gen 2 element parser: `type`, `path`, `flags`, `chunks`, `md5` and `sha256`.
bool ParseDepotItemV2(const steam::json::Value& element, DepotItem* item);

class DepotItemReader {
 public:
  enum class Status {
    kOk,
    kMalformed,  // not JSON, not the manifest's shape, an item that fails its checks, or truncated (at Finish)
    kTooLarge,   // more than kMaxDepotItems items, an item over kMaxItemBytes, or a manifest over its cap
    kRefused,    // the callback returned false
  };
  using Callback = std::function<bool(const DepotItem&)>;

  // A gen 2 manifest: depot.items, ParseDepotItemV2.
  explicit DepotItemReader(Callback callback);
  // Another format: the array under `depot` named `array_key` (at most 32 bytes), each element through `parser`.
  DepotItemReader(Callback callback, std::string array_key, DepotElementParser parser);

  // Reads the next bytes of the inflated manifest. After an error, every call returns it.
  Status Feed(const std::uint8_t* data, std::size_t size);
  // kOk when the manifest was whole and had its items array.
  Status Finish();

  std::size_t items() const { return items_; }
  // The largest an item's buffer grew, to show the reader stays bounded.
  std::size_t peak_item_bytes() const { return peak_item_bytes_; }

 private:
  enum class Lex : std::uint8_t { kNone, kString, kScalar, kCapture };
  enum class Role : std::uint8_t { kOther, kRoot, kDepot, kItems };
  enum class Expect : std::uint8_t { kKeyOrEnd, kKey, kColon, kValue, kValueOrEnd, kCommaOrEnd };
  struct Frame {
    bool object;
    Role role;
    Expect expect;
  };

  Status Byte(char c);
  Status Structural(char c);
  Status BeginValue(char c, Role role);
  Status EndScalar();
  Status Close();
  Status EmitItem();
  Status Fail(Status status) { return error_ = status; }

  Callback callback_;
  std::string array_key_ = "items";
  DepotElementParser parser_ = &ParseDepotItemV2;
  Status error_ = Status::kOk;
  std::vector<Frame> stack_;
  Lex lex_ = Lex::kNone;
  bool escape_ = false;
  bool string_is_key_ = false;
  std::string key_;
  bool key_plain_ = true;   // no escape and short enough to compare
  std::string last_key_;    // the key the next value belongs to; empty when it is none Playden looks for
  std::string scalar_;
  std::string item_;        // the element of the array being copied
  std::size_t item_depth_ = 0;
  bool item_in_string_ = false;
  bool item_escape_ = false;
  bool started_ = false;
  bool done_ = false;
  bool seen_depot_ = false;
  bool seen_items_ = false;
  bool items_closed_ = false;
  std::size_t items_ = 0;
  std::size_t peak_item_bytes_ = 0;
  std::uint64_t total_bytes_ = 0;
};

const char* DepotReaderStatusName(DepotItemReader::Status status);

}  // namespace playden::gog
