#include "runtime/tzst_archive.h"

#include <zstd.h>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace playden::runtime {
namespace {

constexpr std::size_t kBlock = 512;
// Longest name or pax record Playden accepts from an archive.
constexpr std::uint64_t kMaxMetaBytes = 64 * 1024;

// Decompressed bytes of a .tzst file, pulled on demand.
class ZstdStream {
 public:
  ZstdStream() : context_(ZSTD_createDStream(), ZSTD_freeDStream) {}
  ~ZstdStream() {
    if (fd_ >= 0) ::close(fd_);
  }

  bool Open(const std::string& path, std::string* reason) {
    fd_ = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd_ < 0) return Fail(reason, "cannot open the archive");
    if (!context_ || ZSTD_isError(ZSTD_initDStream(context_.get()))) return Fail(reason, "zstd is unavailable");
    input_.resize(ZSTD_DStreamInSize());
    output_.resize(ZSTD_DStreamOutSize());
    return true;
  }

  // Exactly size bytes, or false at a damaged or early end.
  bool Read(char* out, std::size_t size, std::string* reason) {
    while (size > 0) {
      if (out_at_ == out_size_ && !Fill(reason)) return false;
      const auto take = std::min(size, out_size_ - out_at_);
      if (out) {
        std::memcpy(out, output_.data() + out_at_, take);
        out += take;
      }
      out_at_ += take;
      size -= take;
    }
    return true;
  }

  // Hands the next bytes (at most size) to the caller without copying them.
  bool Next(std::size_t size, const char** data, std::size_t* got, std::string* reason) {
    if (out_at_ == out_size_ && !Fill(reason)) return false;
    *data = output_.data() + out_at_;
    *got = std::min(size, out_size_ - out_at_);
    out_at_ += *got;
    return true;
  }

 private:
  bool Fail(std::string* reason, const char* text) {
    if (reason && reason->empty()) *reason = text;
    return false;
  }

  bool Fill(std::string* reason) {
    for (;;) {
      if (in_at_ == in_size_) {
        if (ended_) return Fail(reason, "the archive ends early");
        ssize_t count;
        do count = ::read(fd_, input_.data(), input_.size());
        while (count < 0 && errno == EINTR);
        if (count < 0) return Fail(reason, "cannot read the archive");
        if (count == 0) {
          ended_ = true;
          return Fail(reason, "the archive ends early");
        }
        in_size_ = static_cast<std::size_t>(count);
        in_at_ = 0;
      }
      ZSTD_inBuffer in{input_.data(), in_size_, in_at_};
      ZSTD_outBuffer out{output_.data(), output_.size(), 0};
      const auto result = ZSTD_decompressStream(context_.get(), &out, &in);
      in_at_ = in.pos;
      if (ZSTD_isError(result)) return Fail(reason, "the archive is damaged");
      if (out.pos > 0) {
        out_at_ = 0;
        out_size_ = out.pos;
        return true;
      }
    }
  }

  std::unique_ptr<ZSTD_DStream, std::size_t (*)(ZSTD_DStream*)> context_;
  int fd_ = -1;
  std::vector<char> input_, output_;
  std::size_t in_at_ = 0, in_size_ = 0, out_at_ = 0, out_size_ = 0;
  bool ended_ = false;
};

std::string Field(const char* block, std::size_t offset, std::size_t width) {
  const char* begin = block + offset;
  return std::string(begin, ::strnlen(begin, width));
}

bool Octal(const char* block, std::size_t offset, std::size_t width, std::uint64_t* value) {
  const char* at = block + offset;
  const char* end = at + width;
  while (at < end && *at == ' ') ++at;
  std::uint64_t result = 0;
  bool digits = false;
  for (; at < end && *at >= '0' && *at <= '7'; ++at) {
    if (result >> 60) return false;
    result = result * 8 + static_cast<std::uint64_t>(*at - '0');
    digits = true;
  }
  for (; at < end; ++at)
    if (*at != ' ' && *at != '\0') return false;
  *value = result;
  return digits;
}

bool ChecksumMatches(const char* block) {
  std::uint64_t stored = 0;
  if (!Octal(block, 148, 8, &stored)) return false;
  std::uint64_t sum = 0;
  for (std::size_t i = 0; i < kBlock; ++i)
    sum += (i >= 148 && i < 156) ? ' ' : static_cast<unsigned char>(block[i]);
  return sum == stored;
}

// "<length> path=<value>\n" records; returns the path, if any.
bool PaxPath(std::string_view records, std::string* path) {
  while (!records.empty()) {
    const auto space = records.find(' ');
    if (space == std::string_view::npos || space == 0) return false;
    std::size_t length = 0;
    for (std::size_t i = 0; i < space; ++i) {
      if (records[i] < '0' || records[i] > '9' || length > records.size()) return false;
      length = length * 10 + static_cast<std::size_t>(records[i] - '0');
    }
    if (length <= space + 1 || length > records.size() || records[length - 1] != '\n') return false;
    const auto record = records.substr(space + 1, length - space - 2);
    const auto equals = record.find('=');
    if (equals == std::string_view::npos) return false;
    if (record.substr(0, equals) == "path") *path = std::string(record.substr(equals + 1));
    records.remove_prefix(length);
  }
  return true;
}

bool Fail(std::string* reason, std::string text) {
  if (reason && reason->empty()) *reason = std::move(text);
  return false;
}

}  // namespace

bool ReadTzst(const std::string& path, const std::function<bool(const TarEntry&)>& on_entry,
              const std::function<bool(const char*, std::size_t)>& on_data, std::string* reason) {
  if (reason) reason->clear();
  ZstdStream stream;
  if (!stream.Open(path, reason)) return false;
  std::string long_path;  // from a pax 'x' or GNU 'L' entry, for the next entry
  char block[kBlock];
  for (;;) {
    if (!stream.Read(block, kBlock, reason)) return false;
    bool zero = true;
    for (char c : block) zero = zero && c == '\0';
    if (zero) {
      // End of archive: two zero blocks.
      if (!stream.Read(block, kBlock, reason)) return false;
      for (char c : block)
        if (c != '\0') return Fail(reason, "the archive has data after its end");
      return true;
    }
    if (!ChecksumMatches(block)) return Fail(reason, "the archive has a damaged header");
    std::uint64_t size = 0;
    if (!Octal(block, 124, 12, &size)) return Fail(reason, "the archive has a damaged size");
    const char type = block[156] == '\0' ? '0' : block[156];
    const auto padded = (size + kBlock - 1) / kBlock * kBlock;
    if (type == 'x' || type == 'L') {
      if (size > kMaxMetaBytes) return Fail(reason, "the archive has an oversized header");
      std::string meta(static_cast<std::size_t>(padded), '\0');
      if (!stream.Read(meta.data(), meta.size(), reason)) return false;
      meta.resize(static_cast<std::size_t>(size));
      if (type == 'L') {
        long_path = meta.substr(0, meta.find('\0'));
      } else if (!PaxPath(meta, &long_path)) {
        return Fail(reason, "the archive has a damaged pax header");
      }
      continue;
    }
    if (type == 'g') {  // pax global header: nothing Playden uses
      if (!stream.Read(nullptr, static_cast<std::size_t>(padded), reason)) return false;
      continue;
    }
    TarEntry entry;
    if (!long_path.empty()) {
      entry.path = std::move(long_path);
      long_path.clear();
    } else {
      const auto name = Field(block, 0, 100);
      const auto prefix = std::memcmp(block + 257, "ustar", 5) == 0 ? Field(block, 345, 155) : std::string();
      entry.path = prefix.empty() ? name : prefix + "/" + name;
    }
    while (!entry.path.empty() && entry.path.back() == '/') entry.path.pop_back();
    if (entry.path.empty()) return Fail(reason, "the archive has an entry without a name");
    if (type != '0' && type != '5') return Fail(reason, "the archive holds a link or special file: " + entry.path);
    entry.type = type;
    entry.size = type == '5' ? 0 : size;
    if (!on_entry(entry)) return Fail(reason, "stopped at " + entry.path);
    std::uint64_t left = entry.size;
    while (left > 0) {
      const char* data = nullptr;
      std::size_t got = 0;
      if (!stream.Next(static_cast<std::size_t>(std::min<std::uint64_t>(left, 1u << 20)), &data, &got, reason))
        return false;
      if (!on_data(data, got)) return Fail(reason, "stopped in " + entry.path);
      left -= got;
    }
    if (type == '0' && !stream.Read(nullptr, static_cast<std::size_t>(padded - size), reason)) return false;
  }
}

}  // namespace playden::runtime
