// SPDX-License-Identifier: GPL-3.0-or-later
#include "steam/backend/workshop_legacy.h"

#include "steam/backend/depot_installer.h"

#include <mbedtls/sha1.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace playden::steam {
namespace {

constexpr std::size_t kMaxNameBytes = 255;
constexpr int kPieceAttempts = 4;
constexpr std::uint32_t kStallMs = 30'000;
constexpr std::uint32_t kPieceTimeoutMs = 10 * 60 * 1000;

bool Hex40(std::string_view text) {
  return text.size() == 40 && std::all_of(text.begin(), text.end(), [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
         });
}

std::string Lower(std::string text) {
  for (auto& c : text) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
  return text;
}

// The SHA-1 the URL names, lowercase; none when the URL isn't https://<*.>steamusercontent.com/ugc/<n>/<sha1>[/].
std::optional<std::string> UrlSha1(std::string_view url) {
  constexpr std::string_view kScheme = "https://";
  if (url.substr(0, kScheme.size()) != kScheme) return std::nullopt;
  url.remove_prefix(kScheme.size());
  const auto slash = url.find('/');
  if (slash == std::string_view::npos) return std::nullopt;
  const auto host = Lower(std::string(url.substr(0, slash)));
  if (host.empty() || !std::all_of(host.begin(), host.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-';
      }))
    return std::nullopt;
  constexpr std::string_view kDomain = "steamusercontent.com";
  const bool ours = host == kDomain || (host.size() > kDomain.size() + 1 &&
                                        host.compare(host.size() - kDomain.size(), kDomain.size(), kDomain) == 0 &&
                                        host[host.size() - kDomain.size() - 1] == '.');
  if (!ours) return std::nullopt;
  auto path = url.substr(slash);
  constexpr std::string_view kUgc = "/ugc/";
  if (path.substr(0, kUgc.size()) != kUgc) return std::nullopt;
  path.remove_prefix(kUgc.size());
  const auto handle_end = path.find('/');
  if (handle_end == 0 || handle_end == std::string_view::npos ||
      !std::all_of(path.begin(), path.begin() + static_cast<std::ptrdiff_t>(handle_end),
                   [](char c) { return c >= '0' && c <= '9'; }))
    return std::nullopt;
  path.remove_prefix(handle_end + 1);
  if (!path.empty() && path.back() == '/') path.remove_suffix(1);
  if (!Hex40(path)) return std::nullopt;
  return Lower(std::string(path));
}

// The last component of Steam's filename, when it is a name Windows and Playden accept for a file.
std::optional<std::string> PlainFileName(const std::string& filename) {
  const auto cut = filename.find_last_of("/\\");
  std::string name = cut == std::string::npos ? filename : filename.substr(cut + 1);
  if (name.empty() || name == "." || name == ".." || name.size() > kMaxNameBytes) return std::nullopt;
  if (Lower(name).compare(0, 9, ".playden-") == 0) return std::nullopt;
  for (const unsigned char c : name)
    if (c < 0x20 || c == 0x7f || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
      return std::nullopt;
  if (name.back() == ' ' || name.back() == '.') return std::nullopt;
  return name;
}

std::string ToHex(const unsigned char* bytes, std::size_t size) {
  static const char* kDigits = "0123456789abcdef";
  std::string out;
  for (std::size_t index = 0; index < size; ++index) {
    out += kDigits[bytes[index] >> 4];
    out += kDigits[bytes[index] & 15];
  }
  return out;
}

// The SHA-1 of what the descriptor holds, from its start; none when it can't be read or the cancel flag is raised.
std::optional<std::string> FileSha1(int fd, std::uint64_t* size, const std::atomic<bool>& cancel) {
  const int copy = ::dup(fd);
  if (copy < 0) return std::nullopt;
  FILE* file = ::lseek(copy, 0, SEEK_SET) == 0 ? ::fdopen(copy, "rb") : nullptr;
  if (!file) {
    ::close(copy);
    return std::nullopt;
  }
  mbedtls_sha1_context context;
  mbedtls_sha1_init(&context);
  mbedtls_sha1_starts(&context);
  std::vector<unsigned char> buffer(1u << 16);
  std::size_t read = 0;
  *size = 0;
  bool canceled = false;
  while ((read = std::fread(buffer.data(), 1, buffer.size(), file)) > 0) {
    if (cancel.load()) {
      canceled = true;
      break;
    }
    mbedtls_sha1_update(&context, buffer.data(), read);
    *size += read;
  }
  const bool failed = canceled || std::ferror(file) != 0;
  std::fclose(file);
  unsigned char digest[20];
  mbedtls_sha1_finish(&context, digest);
  mbedtls_sha1_free(&context);
  if (failed) return std::nullopt;
  return ToHex(digest, sizeof digest);
}

// A new file in the staging folder: never an existing name, never through a link, 0644 whatever the umask.
int CreateNew(const std::string& path) {
  const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
  if (fd >= 0 && ::fchmod(fd, 0644) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

bool WriteSmall(const std::string& path, const std::string& text) {
  const int fd = CreateNew(path);
  if (fd < 0) return false;
  bool ok = true;
  for (std::size_t put = 0; ok && put < text.size();) {
    const auto wrote = ::write(fd, text.data() + put, text.size() - put);
    if (wrote < 0 && errno == EINTR) continue;
    ok = wrote > 0;
    if (ok) put += static_cast<std::size_t>(wrote);
  }
  return ::close(fd) == 0 && ok;
}

// "bytes <first>-<last>/<total>" names exactly that piece of a file of that size.
bool RangeIs(const HttpResponse& response, std::uint64_t first, std::uint64_t last, std::uint64_t total) {
  for (const auto& [name, value] : response.headers) {
    std::string lower = name;
    for (auto& c : lower) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    if (lower != "content-range") continue;
    return value == "bytes " + std::to_string(first) + "-" + std::to_string(last) + "/" + std::to_string(total);
  }
  return false;
}

}  // namespace

std::optional<LegacyWorkshopFile> LegacyWorkshopPreviewOf(const WorkshopItem& item) {
  const auto file = LegacyWorkshopFileOf(item);
  if (!file || item.preview_size == 0 || item.preview_size > kLegacyWorkshopPreviewMaxBytes) return std::nullopt;
  if (Lower(file->name) == kLegacyWorkshopPreviewName) return std::nullopt;  // the item's own file has that name
  auto sha1 = UrlSha1(item.preview_url);
  if (!sha1) return std::nullopt;
  LegacyWorkshopFile preview;
  preview.url = item.preview_url;
  preview.sha1 = std::move(*sha1);
  preview.name = kLegacyWorkshopPreviewName;
  preview.bytes = item.preview_size;
  return preview;
}

std::optional<LegacyWorkshopFile> LegacyWorkshopFileOf(const WorkshopItem& item) {
  if (!item.legacy() || item.file_url.empty() || item.manifest == 0) return std::nullopt;
  if (item.file_size == 0 || item.file_size > kLegacyWorkshopMaxBytes) return std::nullopt;
  auto sha1 = UrlSha1(item.file_url);
  auto name = PlainFileName(item.filename);
  if (!sha1 || !name) return std::nullopt;
  LegacyWorkshopFile file;
  file.url = item.file_url;
  file.sha1 = std::move(*sha1);
  file.name = std::move(*name);
  file.bytes = item.file_size;
  return file;
}

namespace {

// Downloads `file` into `path` (a new name in the staging folder) in Range pieces and checks its size and SHA-1.
// `done` hears the bytes written so far.
WorkshopDownloadResult FetchVerified(const LegacyWorkshopPerform& perform, const LegacyWorkshopFile& source,
                                     const std::string& path, const std::atomic<bool>& cancel,
                                     const std::function<void(std::uint64_t)>& done_bytes, std::uint64_t piece_bytes) {
  const auto* file = &source;
  // The sync made the staging folder empty: the name is new there, and never a link.
  const int fd = CreateNew(path);
  if (fd < 0) return {InstallError::kStorage, false, "cannot create file"};
  const auto fail = [fd](InstallError error, std::string detail) {
    ::close(fd);
    return WorkshopDownloadResult{error, false, std::move(detail)};
  };
  for (std::uint64_t offset = 0; offset < file->bytes;) {
    const auto length = std::min(piece_bytes, file->bytes - offset);
    WorkshopDownloadResult last{InstallError::kNetwork, false, "file transfer failed"};
    bool done = false;
    for (int attempt = 0; attempt < kPieceAttempts && !done; ++attempt) {
      if (cancel.load()) return fail(InstallError::kCanceled, "canceled");
      std::uint64_t received = 0;
      int write_errno = 0;
      HttpRequest request;
      request.url = file->url;
      request.headers.emplace_back("Range", "bytes=" + std::to_string(offset) + "-" +
                                                std::to_string(offset + length - 1));
      request.max_response_bytes = static_cast<std::size_t>(length);
      request.stall_timeout_ms = kStallMs;
      request.timeout_ms = kPieceTimeoutMs;
      request.body_sink = [&](const std::uint8_t* data, std::size_t size) {
        if (received + size > length) return false;
        std::size_t put = 0;
        while (put < size) {
          const auto wrote = ::pwrite(fd, data + put, size - put, static_cast<off_t>(offset + received + put));
          if (wrote < 0 && errno == EINTR) continue;
          if (wrote <= 0) {
            write_errno = wrote < 0 ? errno : EIO;
            return false;
          }
          put += static_cast<std::size_t>(wrote);
        }
        received += size;
        if (done_bytes) done_bytes(offset + received);
        return true;
      };
      const auto response = perform(request, cancel);
      if (response.error == HttpError::kCanceled || cancel.load()) return fail(InstallError::kCanceled, "canceled");
      if (write_errno != 0)
        return fail(write_errno == ENOSPC ? InstallError::kNoSpace : InstallError::kStorage, "cannot write file");
      // More bytes than the piece: the server ignored the Range. Asking again may reach another server.
      if (response.error == HttpError::kSinkFailed || response.error == HttpError::kResponseTooLarge) {
        last = {InstallError::kCorrupt, false, "file transfer ignored range"};
        continue;
      }
      if (!response.ok()) {
        last = {InstallError::kNetwork, false, "file transfer " + HttpFailureName(response)};
        continue;
      }
      const auto status = response.status_code;
      const auto code = "http=" + std::to_string(status);
      if (status == 206) {
        if (received == length && RangeIs(response, offset, offset + length - 1, file->bytes)) {
          done = true;
          break;
        }
        last = {InstallError::kCorrupt, false, "file transfer wrong range"};
        continue;
      }
      if (status == 200) {
        // The whole file: fine when the piece is the whole file.
        if (offset == 0 && length == file->bytes && received == length) {
          done = true;
          break;
        }
        last = {InstallError::kCorrupt, false, "file transfer ignored range"};
        continue;
      }
      if (status >= 400 && status < 500 && status != 408 && status != 429)
        return fail(InstallError::kRejected, "file unavailable " + code);
      last = {InstallError::kNetwork, false, "file transfer " + code};
    }
    if (!done) return fail(last.error, last.detail);
    offset += length;
  }
  if (::fsync(fd) != 0) return fail(InstallError::kStorage, "cannot write file");
  std::uint64_t size = 0;
  const auto sha1 = FileSha1(fd, &size, cancel);
  if (cancel.load()) return fail(InstallError::kCanceled, "canceled");
  if (!sha1) return fail(InstallError::kStorage, "cannot read file");
  if (::close(fd) != 0) return {InstallError::kStorage, false, "cannot write file"};
  if (size != file->bytes || *sha1 != file->sha1) return {InstallError::kCorrupt, false, "file failed verification"};
  return {};
}

}  // namespace

WorkshopDownloadResult DownloadLegacyWorkshopItem(const LegacyWorkshopPerform& perform, const WorkshopItem& item,
                                                  const std::string& staging_root, const std::atomic<bool>& cancel,
                                                  const WorkshopProgressFn& progress, std::uint64_t piece_bytes) {
  const auto file = LegacyWorkshopFileOf(item);
  if (!file || !perform || piece_bytes == 0 || piece_bytes > kLegacyWorkshopPieceBytes)
    return {InstallError::kProtocol, false, "item has no file"};
  InstallProgress report;
  report.download_total = file->bytes;
  report.install_total = file->bytes;
  report.files_total = 1;
  if (progress) progress(report);
  const auto fetched = FetchVerified(perform, *file, staging_root + "/" + file->name, cancel,
                                     [&](std::uint64_t done) {
                                       report.download_done = done;
                                       if (progress) progress(report);
                                     },
                                     piece_bytes);
  if (fetched.error != InstallError::kNone) return fetched;
  std::string list = "f\t" + file->name + "\n";
  std::string tree = "v1\n" + file->sha1 + "\t" + std::to_string(file->bytes) + "\t" + file->name + "\n";
  // The preview picture, beside it: the game shows it in its add-on list. Without one the item still installs.
  if (const auto preview = LegacyWorkshopPreviewOf(item)) {
    const auto path = staging_root + "/" + preview->name;
    const auto got = FetchVerified(perform, *preview, path, cancel, {}, piece_bytes);
    if (got.error == InstallError::kCanceled) return got;
    if (got.error == InstallError::kNone) {
      list += "f\t" + preview->name + "\n";
      tree += preview->sha1 + "\t" + std::to_string(preview->bytes) + "\t" + preview->name + "\n";
    } else {
      ::unlink(path.c_str());
    }
  }
  if (!WriteSmall(staging_root + "/" + DepotInstaller::kFileList, list) ||
      !WriteSmall(staging_root + "/" + DepotInstaller::kFileTree, tree))
    return {InstallError::kStorage, false, "cannot write file list"};
  return {};
}

}  // namespace playden::steam
