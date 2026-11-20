// Linux port: glibc spells the stat timestamps st_*tim, BSD (and the PS5) st_*timespec.
#if defined(__linux__) && !defined(st_mtimespec)
#define st_mtimespec st_mtim
#define st_atimespec st_atim
#define st_ctimespec st_ctim
#endif
#include "runtime/artifact_hash.h"
#include "platform/managed_directory.h"

#include <mbedtls/sha256.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace playden::runtime {
namespace {

class Descriptor {
 public:
  explicit Descriptor(int value) : value_(value) {}
  ~Descriptor() { if (value_ >= 0) close(value_); }
  Descriptor(const Descriptor&) = delete;
  Descriptor& operator=(const Descriptor&) = delete;
  int get() const { return value_; }
  void reset(int value) { if (value_ >= 0) close(value_); value_ = value; }
 private:
  int value_;
};

class Hash {
 public:
  Hash() { mbedtls_sha256_init(&context_); }
  ~Hash() { mbedtls_sha256_free(&context_); }
  bool Start() { return mbedtls_sha256_starts(&context_, 0) == 0; }
  bool Update(const unsigned char* bytes, std::size_t length) {
    return mbedtls_sha256_update(&context_, bytes, length) == 0;
  }
  std::optional<std::string> Finish() {
    std::array<unsigned char, 32> digest{};
    if (mbedtls_sha256_finish(&context_, digest.data()) != 0) return std::nullopt;
    constexpr char hex[] = "0123456789abcdef";
    std::string result(64, '0');
    for (std::size_t i = 0; i < digest.size(); ++i) {
      result[2 * i] = hex[digest[i] >> 4];
      result[2 * i + 1] = hex[digest[i] & 15];
    }
    return result;
  }
 private:
  mbedtls_sha256_context context_;
};

int OpenFile(const std::filesystem::path& path) {
  if (path.empty() || path.native().find('\0') != std::string::npos ||
      path.filename().empty()) return -1;
  std::error_code error;
  const auto absolute = path.is_absolute() ? path : std::filesystem::absolute(path, error);
  if (error) return -1;
  auto directory = platform::ManagedDirectory::Open(
      absolute.parent_path(), platform::DirectoryPolicy::kApprovedTree);
  if (!directory) return -1;
  return directory->OpenRegularFile(absolute.filename().string());
}

}  // namespace

std::optional<std::string> HashArtifactBytes(std::string_view bytes) {
  Hash hash;
  if (!hash.Start() || !hash.Update(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size()))
    return std::nullopt;
  return hash.Finish();
}

std::optional<std::string> HashArtifactFile(const std::filesystem::path& path) {
  Descriptor file(OpenFile(path));
  struct stat before{}, after{};
  if (file.get() < 0 || fstat(file.get(), &before) != 0 ||
      !S_ISREG(before.st_mode) || before.st_size < 0) return std::nullopt;
  Hash hash;
  if (!hash.Start()) return std::nullopt;
  // On the heap: the console's worker threads have small stacks.
  std::vector<unsigned char> buffer(64 * 1024);
  std::uint64_t total = 0;
  for (;;) {
    const auto count = read(file.get(), buffer.data(), buffer.size());
    if (count < 0) {
      if (errno == EINTR) continue;
      return std::nullopt;
    }
    if (count == 0) break;
    total += static_cast<std::uint64_t>(count);
    if (total > static_cast<std::uint64_t>(before.st_size) ||
        !hash.Update(buffer.data(), static_cast<std::size_t>(count))) return std::nullopt;
  }
  if (total != static_cast<std::uint64_t>(before.st_size) || fstat(file.get(), &after) != 0 ||
      before.st_size != after.st_size || before.st_mode != after.st_mode ||
      before.st_mtime != after.st_mtime || before.st_ctime != after.st_ctime)
    return std::nullopt;
#if defined(__APPLE__)
  if (before.st_mtimespec.tv_nsec != after.st_mtimespec.tv_nsec ||
      before.st_ctimespec.tv_nsec != after.st_ctimespec.tv_nsec) return std::nullopt;
#else
  if (before.st_mtim.tv_nsec != after.st_mtim.tv_nsec ||
      before.st_ctim.tv_nsec != after.st_ctim.tv_nsec) return std::nullopt;
#endif
  return hash.Finish();
}
}  // namespace playden::runtime
