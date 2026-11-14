// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// One gen 2 chunk from GOG's CDN (GOG_PROTOCOL.md §4): fetched as a stream, checked and written at its offset. The
// compressed bytes go through an MD5 and miniz as they arrive, the inflated bytes through a second MD5 and straight to
// the file, so no chunk is held in memory whatever its size. Both hashes and both sizes must match; a body longer
// than compressedSize and a stream that inflates past `size` stop at once.
//
// A gen 1 file instead is one or more byte ranges of the build's main.bin (FetchRange), with no hash per range: the
// caller checks the whole file's MD5 once every range is written.
//
// Requests carry no Authorization header: a secure link's signature is in its URL, and the dependency store's links
// are public. Nothing here logs; the result names the endpoint (for logs) and what went wrong.

#include "gog/content.h"
#include "gog/depot_reader.h"
#include "gog/http.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace playden::gog {

inline constexpr int kChunkAttempts = 5;
inline constexpr std::uint32_t kChunkStallTimeoutMs = 30'000;
inline constexpr std::uint32_t kChunkConnectTimeoutMs = 10'000;
// Before each retry: this many milliseconds times the number of attempts made so far.
inline constexpr std::uint32_t kChunkRetryPauseMs = 2'000;
// The longest byte range of a gen 1 main.bin asked for at once.
inline constexpr std::uint64_t kGen1RangeBytes = 10 * 1024 * 1024;

struct ChunkResult {
  enum class Kind {
    kOk,
    kCorrupt,   // every attempt answered, and the last one failed a hash or size check (a range: status, length or
                // Content-Range); also a request that can't be made (a bad chunk or range)
    kExpired,   // 401 or 403: the link's signature is no longer good; fetch the link again and retry
    kNetwork,   // every attempt failed, the last one without an answer or with another status
    kCanceled,
    kStorage,   // the write failed
    kNoSpace,   // the write failed with ENOSPC
  };
  Kind kind = Kind::kNetwork;
  int attempts = 0;
  int corrupt = 0;    // attempts that failed a check
  int stalls = 0;     // attempts that ended on the stall timeout
  bool stalled = false;
  long status = 0;    // the last attempt's HTTP status; 0 without an answer
  std::string endpoint;  // the last endpoint tried: "fastly", "gcore" or "other" (the server names it; never logged raw)
};

const char* ChunkResultName(ChunkResult::Kind kind);

// "fastly" or "gcore" when the endpoint is named so, otherwise "other": the name comes from the server.
const char* EndpointLogName(const std::string& name);

// Writes `size` inflated bytes at `offset` of the chunk's file; false when it fails (errno set).
using ChunkWrite = std::function<bool(std::uint64_t offset, const std::uint8_t* data, std::size_t size)>;
// Waits `ms` milliseconds, or less when cancel is set.
using ChunkPause = std::function<void(std::uint32_t ms, const std::atomic<bool>& cancel)>;

// The default pause: sleeps in short steps and wakes early on cancel.
void SleepPause(std::uint32_t ms, const std::atomic<bool>& cancel);

// Fetches `chunk` from `endpoints` (in the order given, rotating, kChunkAttempts in all) and writes its inflated
// bytes at file_offset onward. The URL is FillUrl(endpoint, "/" + GalaxyPath(compressed_md5)). A failed attempt may
// have written part of the chunk; the next one writes it again from the start. kOk means every byte was written and
// both hashes matched.
ChunkResult FetchChunk(Transport& transport, const std::vector<CdnEndpoint>& endpoints, const DepotChunk& chunk,
                       std::uint64_t file_offset, const ChunkWrite& write, const ChunkPause& pause,
                       const std::atomic<bool>& cancel);

// Fetches bytes [blob_offset, blob_offset + length) of a gen 1 build's main.bin from `endpoints` (a gen 1 secure link's,
// in the order given, rotating, kChunkAttempts in all) and writes them at file_offset onward. The URL is
// FillUrl(endpoint, "/main.bin") with "Range: bytes=<first>-<last>". Only a 206 with exactly `length` bytes, and a
// Content-Range (when sent) naming the same first and last byte, is kOk; a 200, a short or long body or another
// Content-Range is a failed check, tried again. The response is capped at `length` bytes, so a server that ignores
// Range never streams the blob, and nothing is written past `length`. A length of 0, one above kGen1RangeBytes, or a
// range past 2^64 is kCorrupt with no request.
ChunkResult FetchRange(Transport& transport, const std::vector<CdnEndpoint>& endpoints, std::uint64_t blob_offset,
                       std::uint64_t length, std::uint64_t file_offset, const ChunkWrite& write,
                       const ChunkPause& pause, const std::atomic<bool>& cancel);

}  // namespace playden::gog
