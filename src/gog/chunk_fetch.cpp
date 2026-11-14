// SPDX-License-Identifier: GPL-3.0-or-later
#include "gog/chunk_fetch.h"

#include "gog/hash.h"
#include "gog/inflate.h"
#include "platform/secret.h"
#include "steam/backend/http_client.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <string_view>
#include <memory>
#include <thread>

namespace playden::gog {
namespace {

enum class Failure { kNone, kCorrupt, kNetwork, kStall, kExpired, kCanceled, kStorage, kNoSpace };

// One attempt's state, on the heap: the inflate buffer and both hashes are larger than a worker should keep on its
// stack.
struct Attempt {
  explicit Attempt(std::uint64_t size) : inflate(size) {}
  ZlibStream inflate;
  Md5 compressed_md5;
  Md5 inflated_md5;
  std::uint64_t compressed = 0;
  std::uint64_t inflated = 0;
  bool overlong = false;      // more body than compressedSize
  bool bad_stream = false;    // the inflate refused the stream (a bomb included)
  bool write_failed = false;
  int write_errno = 0;
};

Failure TryOnce(Transport& transport, const std::string& url, const DepotChunk& chunk, std::uint64_t file_offset,
                const ChunkWrite& write, const std::atomic<bool>& cancel, long* status) {
  auto attempt = std::make_unique<Attempt>(chunk.size);
  steam::HttpRequest request;
  request.method = steam::HttpMethod::kGet;
  request.url = url;
  request.headers.emplace_back("User-Agent", kUserAgent);
  request.timeout_ms = 0;
  request.connect_timeout_ms = kChunkConnectTimeoutMs;
  request.stall_timeout_ms = kChunkStallTimeoutMs;
  // One byte past compressedSize reaches the sink, which stops it; more than that is the client's own refusal.
  request.max_response_bytes = static_cast<std::size_t>(
      chunk.compressed_size < steam::kMaxHttpStreamResponseBytes ? chunk.compressed_size + 1
                                                                : steam::kMaxHttpStreamResponseBytes);
  Attempt& state = *attempt;
  const ZlibStream::Output out = [&state, &write, file_offset](const std::uint8_t* data, std::size_t size) {
    state.inflated_md5.Update(data, size);
    errno = 0;
    if (!write(file_offset + state.inflated, data, size)) {
      state.write_failed = true;
      state.write_errno = errno;
      return false;
    }
    state.inflated += size;
    return true;
  };
  request.body_sink = [&state, &chunk, &out](const std::uint8_t* data, std::size_t size) {
    if (size > chunk.compressed_size - state.compressed) {
      state.overlong = true;
      return false;
    }
    state.compressed += size;
    state.compressed_md5.Update(data, size);
    const auto inflated = state.inflate.Feed(data, size, out);
    if (inflated == ZlibStream::Status::kRefused) return false;  // the write failed
    if (inflated != ZlibStream::Status::kOk) {
      state.bad_stream = true;
      return false;
    }
    return true;
  };
  auto response = transport ? transport(request, cancel) : steam::HttpResponse{steam::HttpError::kInvalidRequest};
  platform::ZeroizeSecret(request.url);
  *status = response.ok() ? response.status_code : 0;

  if (state.write_failed) return state.write_errno == ENOSPC ? Failure::kNoSpace : Failure::kStorage;
  if (cancel.load() || response.error == steam::HttpError::kCanceled) return Failure::kCanceled;
  if (state.overlong || state.bad_stream || response.error == steam::HttpError::kResponseTooLarge)
    return Failure::kCorrupt;
  if (response.error == steam::HttpError::kTimeout) return Failure::kStall;
  if (!response.ok()) return Failure::kNetwork;
  if (response.status_code == 401 || response.status_code == 403) return Failure::kExpired;
  if (response.status_code < 200 || response.status_code > 299) return Failure::kNetwork;
  if (state.compressed != chunk.compressed_size) return Failure::kCorrupt;
  if (state.inflate.Finish() != ZlibStream::Status::kOk) return Failure::kCorrupt;
  if (state.compressed_md5.FinalHex() != chunk.compressed_md5) return Failure::kCorrupt;
  if (state.inflated_md5.FinalHex() != chunk.md5) return Failure::kCorrupt;
  if (state.inflated != chunk.size) return Failure::kCorrupt;
  return Failure::kNone;
}

bool AsciiEqualFold(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    char x = a[i], y = b[i];
    if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
    if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
    if (x != y) return false;
  }
  return true;
}

// Whether a Content-Range value is "bytes <first>-<last>/<total or *>" for this range (the unit in any case).
bool ContentRangeIs(std::string_view value, const std::string& span) {
  if (value.size() < 6 || !AsciiEqualFold(value.substr(0, 6), "bytes ")) return false;
  value.remove_prefix(6);
  if (value.size() <= span.size() || value.compare(0, span.size(), span) != 0 || value[span.size()] != '/')
    return false;
  const auto total = value.substr(span.size() + 1);
  return total == "*" || (!total.empty() && total.find_first_not_of("0123456789") == std::string_view::npos);
}

Failure TryRange(Transport& transport, const std::string& url, const std::string& span, std::uint64_t length,
                 std::uint64_t file_offset, const ChunkWrite& write, const std::atomic<bool>& cancel, long* status) {
  steam::HttpRequest request;
  request.method = steam::HttpMethod::kGet;
  request.url = url;
  request.headers.reserve(2);
  request.headers.emplace_back("User-Agent", kUserAgent);
  request.headers.emplace_back("Range", "bytes=" + span);
  request.timeout_ms = 0;
  request.connect_timeout_ms = kChunkConnectTimeoutMs;
  request.stall_timeout_ms = kChunkStallTimeoutMs;
  // A server that ignores Range answers 200 with the whole blob: the client stops it after `length` bytes.
  request.max_response_bytes = static_cast<std::size_t>(length);
  std::uint64_t received = 0;
  bool overlong = false;
  bool write_failed = false;
  int write_errno = 0;
  request.body_sink = [&](const std::uint8_t* data, std::size_t size) {
    if (size > length - received) {
      overlong = true;
      return false;
    }
    errno = 0;
    if (!write(file_offset + received, data, size)) {
      write_failed = true;
      write_errno = errno;
      return false;
    }
    received += size;
    return true;
  };
  auto response = transport ? transport(request, cancel) : steam::HttpResponse{steam::HttpError::kInvalidRequest};
  platform::ZeroizeSecret(request.url);
  *status = response.ok() ? response.status_code : 0;

  if (write_failed) return write_errno == ENOSPC ? Failure::kNoSpace : Failure::kStorage;
  if (cancel.load() || response.error == steam::HttpError::kCanceled) return Failure::kCanceled;
  if (overlong || response.error == steam::HttpError::kResponseTooLarge) return Failure::kCorrupt;
  if (response.error == steam::HttpError::kTimeout) return Failure::kStall;
  if (!response.ok()) return Failure::kNetwork;
  if (response.status_code == 401 || response.status_code == 403) return Failure::kExpired;
  if (response.status_code < 200 || response.status_code > 299) return Failure::kNetwork;
  if (response.status_code != 206 || received != length) return Failure::kCorrupt;
  for (const auto& [name, value] : response.headers)
    if (AsciiEqualFold(name, "Content-Range") && !ContentRangeIs(value, span)) return Failure::kCorrupt;
  return Failure::kNone;
}

// Tries `urls` in turn (rotating, kChunkAttempts in all, pausing before each retry) until one attempt succeeds or
// fails for good, and wipes the URLs. `urls` holds (log name, URL) pairs and is not empty.
template <typename Try>
ChunkResult Attempts(std::vector<std::pair<std::string, std::string>>& urls, const ChunkPause& pause,
                     const std::atomic<bool>& cancel, Try try_once) {
  ChunkResult result;
  Failure last = Failure::kNetwork;
  for (int attempt = 0; attempt < kChunkAttempts; ++attempt) {
    if (cancel.load()) {
      last = Failure::kCanceled;
      break;
    }
    if (attempt > 0) {
      if (pause) pause(kChunkRetryPauseMs * static_cast<std::uint32_t>(attempt), cancel);
      if (cancel.load()) {
        last = Failure::kCanceled;
        break;
      }
    }
    const auto& [name, url] = urls[static_cast<std::size_t>(attempt) % urls.size()];
    result.endpoint = name;
    ++result.attempts;
    last = try_once(url, &result.status);
    if (last == Failure::kNone || last == Failure::kExpired || last == Failure::kCanceled ||
        last == Failure::kStorage || last == Failure::kNoSpace)
      break;
    if (last == Failure::kCorrupt) ++result.corrupt;
    if (last == Failure::kStall) {
      ++result.stalls;
      result.stalled = true;
    }
  }
  for (auto& entry : urls) platform::ZeroizeSecret(entry.second);
  urls.clear();
  switch (last) {
    case Failure::kNone: result.kind = ChunkResult::Kind::kOk; break;
    case Failure::kCorrupt: result.kind = ChunkResult::Kind::kCorrupt; break;
    case Failure::kExpired: result.kind = ChunkResult::Kind::kExpired; break;
    case Failure::kCanceled: result.kind = ChunkResult::Kind::kCanceled; break;
    case Failure::kStorage: result.kind = ChunkResult::Kind::kStorage; break;
    case Failure::kNoSpace: result.kind = ChunkResult::Kind::kNoSpace; break;
    case Failure::kNetwork:
    case Failure::kStall: result.kind = ChunkResult::Kind::kNetwork; break;
  }
  return result;
}

// The URLs each endpoint gives `append_to_path`; one that can't be filled is left out.
std::vector<std::pair<std::string, std::string>> Urls(const std::vector<CdnEndpoint>& endpoints,
                                                      const std::string& append_to_path) {
  std::vector<std::pair<std::string, std::string>> urls;
  for (const auto& endpoint : endpoints)
    if (auto url = FillUrl(endpoint, append_to_path)) urls.emplace_back(EndpointLogName(endpoint.name), std::move(*url));
  return urls;
}

}  // namespace

const char* ChunkResultName(ChunkResult::Kind kind) {
  switch (kind) {
    case ChunkResult::Kind::kOk: return "ok";
    case ChunkResult::Kind::kCorrupt: return "corrupt";
    case ChunkResult::Kind::kExpired: return "expired";
    case ChunkResult::Kind::kNetwork: return "network";
    case ChunkResult::Kind::kCanceled: return "canceled";
    case ChunkResult::Kind::kStorage: return "storage";
    case ChunkResult::Kind::kNoSpace: return "no_space";
  }
  return "network";
}

const char* EndpointLogName(const std::string& name) {
  if (name == "fastly") return "fastly";
  if (name == "gcore") return "gcore";
  return "other";
}

void SleepPause(std::uint32_t ms, const std::atomic<bool>& cancel) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (!cancel.load()) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= end) return;
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(end - now);
    std::this_thread::sleep_for(std::min(left, std::chrono::milliseconds(50)));
  }
}

ChunkResult FetchChunk(Transport& transport, const std::vector<CdnEndpoint>& endpoints, const DepotChunk& chunk,
                       std::uint64_t file_offset, const ChunkWrite& write, const ChunkPause& pause,
                       const std::atomic<bool>& cancel) {
  ChunkResult result;
  const auto galaxy = GalaxyPath(chunk.compressed_md5);
  if (!galaxy || !IsMd5Hex(chunk.md5) || chunk.size > kMaxChunkBytes || chunk.compressed_size > kMaxChunkBytes ||
      chunk.compressed_size == 0) {
    result.kind = ChunkResult::Kind::kCorrupt;
    return result;
  }
  auto urls = Urls(endpoints, "/" + *galaxy);
  if (urls.empty()) {
    result.kind = ChunkResult::Kind::kNetwork;
    return result;
  }
  return Attempts(urls, pause, cancel, [&](const std::string& url, long* status) {
    return TryOnce(transport, url, chunk, file_offset, write, cancel, status);
  });
}

ChunkResult FetchRange(Transport& transport, const std::vector<CdnEndpoint>& endpoints, std::uint64_t blob_offset,
                       std::uint64_t length, std::uint64_t file_offset, const ChunkWrite& write,
                       const ChunkPause& pause, const std::atomic<bool>& cancel) {
  ChunkResult result;
  if (length == 0 || length > kGen1RangeBytes || blob_offset > UINT64_MAX - (length - 1) ||
      file_offset > UINT64_MAX - length) {
    result.kind = ChunkResult::Kind::kCorrupt;
    return result;
  }
  auto urls = Urls(endpoints, "/main.bin");
  if (urls.empty()) {
    result.kind = ChunkResult::Kind::kNetwork;
    return result;
  }
  const std::string span = std::to_string(blob_offset) + "-" + std::to_string(blob_offset + length - 1);
  return Attempts(urls, pause, cancel, [&](const std::string& url, long* status) {
    return TryRange(transport, url, span, length, file_offset, write, cancel, status);
  });
}

}  // namespace playden::gog
