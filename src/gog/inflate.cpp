// SPDX-License-Identifier: GPL-3.0-or-later
#include "gog/inflate.h"

#include <miniz.h>

#include <climits>
#include <cstring>

namespace playden::gog {

struct ZlibStream::State {
  mz_stream stream;
  bool open = false;
  std::unique_ptr<std::uint8_t[]> buffer;
};

ZlibStream::ZlibStream(std::uint64_t max_output) : state_(new State), max_output_(max_output) {
  std::memset(&state_->stream, 0, sizeof(state_->stream));
  state_->buffer.reset(new std::uint8_t[kBufferBytes]);
  // The default window bits parse (and require) the zlib header and check the Adler-32 trailer.
  state_->open = mz_inflateInit(&state_->stream) == MZ_OK;
  if (!state_->open) error_ = Status::kCorrupt;
}

ZlibStream::~ZlibStream() {
  if (state_->open) mz_inflateEnd(&state_->stream);
}

ZlibStream::Status ZlibStream::Feed(const std::uint8_t* data, std::size_t size, const Output& out) {
  if (error_ != Status::kOk) return error_;
  if (size == 0) return Status::kOk;
  if (ended_) return error_ = Status::kTrailing;
  auto& stream = state_->stream;
  while (size > 0 || !ended_) {
    // mz_stream counts in unsigned int: a larger piece goes in parts.
    const std::size_t piece = size < static_cast<std::size_t>(UINT_MAX) ? size : static_cast<std::size_t>(UINT_MAX);
    stream.next_in = data;
    stream.avail_in = static_cast<unsigned int>(piece);
    stream.next_out = state_->buffer.get();
    stream.avail_out = static_cast<unsigned int>(kBufferBytes);
    const int result = mz_inflate(&stream, MZ_SYNC_FLUSH);
    const std::size_t used = piece - stream.avail_in;
    data += used;
    size -= used;
    const std::size_t produced = kBufferBytes - stream.avail_out;
    if (result != MZ_OK && result != MZ_STREAM_END && result != MZ_BUF_ERROR) return error_ = Status::kCorrupt;
    if (produced > 0) {
      if (produced > max_output_ - total_out_) return error_ = Status::kTooLarge;
      total_out_ += produced;
      if (!out(state_->buffer.get(), produced)) return error_ = Status::kRefused;
    }
    if (result == MZ_STREAM_END) {
      ended_ = true;
      if (size > 0) return error_ = Status::kTrailing;
      return Status::kOk;
    }
    // A full buffer may leave more output waiting in miniz's window: drain it even with no input left.
    if (produced == kBufferBytes) continue;
    // MZ_BUF_ERROR: no progress is possible without more input. It is not an error.
    if (size == 0) return Status::kOk;
    if (used == 0 && produced == 0) return error_ = Status::kCorrupt;  // input left and no progress
  }
  return Status::kOk;
}

ZlibStream::Status ZlibStream::Finish() {
  if (error_ != Status::kOk) return error_;
  return ended_ ? Status::kOk : Status::kTruncated;
}

const char* ZlibStatusName(ZlibStream::Status status) {
  switch (status) {
    case ZlibStream::Status::kOk: return "ok";
    case ZlibStream::Status::kTooLarge: return "too_large";
    case ZlibStream::Status::kCorrupt: return "corrupt";
    case ZlibStream::Status::kTruncated: return "truncated";
    case ZlibStream::Status::kTrailing: return "trailing";
    case ZlibStream::Status::kRefused: return "refused";
  }
  return "corrupt";
}

}  // namespace playden::gog
