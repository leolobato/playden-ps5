// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// A streaming zlib inflate (miniz) for GOG's chunks, build metas and depot manifests. The input arrives in pieces of
// any size and the output leaves through a callback, so no whole body is held. The output has a cap: a stream that
// would pass it stops at once, so a decompression bomb can't inflate without bound.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace playden::gog {

class ZlibStream {
 public:
  enum class Status {
    kOk,         // accepted; more may follow, or the stream has ended (ended())
    kTooLarge,   // the output would pass the cap: nothing past the cap was handed out
    kCorrupt,    // not a zlib stream (the header is required), a bad block, or a bad Adler-32
    kTruncated,  // Finish before the stream's end
    kTrailing,   // bytes after the stream's end
    kRefused,    // the output callback returned false
  };
  using Output = std::function<bool(const std::uint8_t* data, std::size_t size)>;

  // The output buffer (about 64 KiB) and miniz's state (about 43 KiB) are on the heap: worker stacks are small.
  static constexpr std::size_t kBufferBytes = 64 * 1024;

  explicit ZlibStream(std::uint64_t max_output);
  ~ZlibStream();
  ZlibStream(const ZlibStream&) = delete;
  ZlibStream& operator=(const ZlibStream&) = delete;

  // Inflates `size` bytes and hands every inflated block to `out`, in order. After an error, every call returns it.
  Status Feed(const std::uint8_t* data, std::size_t size, const Output& out);
  // kOk when the stream ended (and nothing followed), else kTruncated or the earlier error.
  Status Finish();

  bool ended() const { return ended_; }
  std::uint64_t total_out() const { return total_out_; }

 private:
  struct State;
  std::unique_ptr<State> state_;
  std::uint64_t max_output_ = 0;
  std::uint64_t total_out_ = 0;
  bool ended_ = false;
  Status error_ = Status::kOk;
};

const char* ZlibStatusName(ZlibStream::Status status);

}  // namespace playden::gog
