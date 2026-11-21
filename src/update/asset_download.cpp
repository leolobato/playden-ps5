#include "update/asset_download.h"
#include "update/release.h"

#include <fcntl.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>

namespace playden::update {
namespace {

constexpr int kMaxRedirects = 5;

std::string Header(const steam::HttpResponse& response, const char* name) {
  for (const auto& [key, value] : response.headers)
    if (strcasecmp(key.c_str(), name) == 0) return value;
  return {};
}

bool Redirect(long status) { return status == 301 || status == 302 || status == 303 || status == 307 || status == 308; }

std::string Host(const std::string& url) {
  const std::string scheme = "https://";
  if (url.compare(0, scheme.size(), scheme) != 0) return {};
  const auto end = url.find_first_of("/?#", scheme.size());
  std::string host = url.substr(scheme.size(), end == std::string::npos ? std::string::npos : end - scheme.size());
  // No user info or ports.
  if (host.find_first_of("@:") != std::string::npos) return {};
  for (auto& c : host) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
  return host;
}

void Log(const DownloadOptions& options, const std::string& line) {
  if (options.log) options.log(line);
}

bool Cancelled(const DownloadOptions& options) { return options.cancel && options.cancel->load(); }

steam::HttpResponse Get(const DownloadOptions& options, const std::string& url, const std::string& range,
                        std::size_t limit, const std::string& accept = "*/*") {
  steam::HttpRequest request;
  request.url = url;
  request.headers = {{"User-Agent", options.user_agent}, {"Accept", accept}};
  if (!range.empty()) request.headers.emplace_back("Range", "bytes=" + range);
  request.timeout_ms = 120000;
  request.max_response_bytes = limit;
  static const std::atomic<bool> never{false};
  return options.fetch(request, options.cancel ? *options.cancel : never);
}

std::string Describe(const steam::HttpResponse& response) {
  return "status " + std::to_string(response.status_code) + " error " + std::to_string(static_cast<int>(response.error));
}

// Follows redirects from url until a server answers the probe range. Returns the final URL.
std::optional<std::string> Resolve(const DownloadOptions& options, std::string url, std::string* error) {
  for (int hop = 0; hop <= kMaxRedirects; ++hop) {
    if (!AllowedDownloadUrl(url, options)) {
      *error = "download host not allowed";
      return std::nullopt;
    }
    const auto response = Get(options, url, "0-0", 64 << 10);
    if (Redirect(response.status_code)) {
      url = Header(response, "location");
      continue;
    }
    if (response.status_code == 206 || response.status_code == 200) return url;
    *error = "download link failed (" + Describe(response) + ")";
    return std::nullopt;
  }
  *error = "too many redirects";
  return std::nullopt;
}

void Wait(const DownloadOptions& options, int attempt) {
  if (options.wait_before_retry) {
    options.wait_before_retry(attempt);
    return;
  }
  for (int step = 0; step < 20 * attempt && !Cancelled(options); ++step) usleep(100000);
}

}  // namespace

bool AllowedDownloadUrl(const std::string& url) {
  const std::string host = Host(url);
  if (host.empty()) return false;
  if (host == "github.com" || host == "api.github.com") return true;
  const std::string suffix = ".githubusercontent.com";
  return host.size() > suffix.size() && host.compare(host.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool AllowedDownloadUrl(const std::string& url, const DownloadOptions& options) {
  if (options.allowed_url_prefix.empty()) return AllowedDownloadUrl(url);
  const auto& prefix = options.allowed_url_prefix;
  // The prefix names its host with a "/" after it, so "https://host.evil/" and "https://host@evil/" don't match; a
  // URL that could climb out of the prefix ("..", escapes, backslashes) is refused.
  const auto host_end = prefix.find('/', 8);
  return prefix.compare(0, 8, "https://") == 0 && host_end != std::string::npos && host_end > 8 &&
         url.compare(0, prefix.size(), prefix) == 0 && url.find_first_of("\\@%") == std::string::npos &&
         url.find("/..") == std::string::npos;
}

std::optional<std::string> FetchSmall(const std::string& url, std::size_t limit, const DownloadOptions& options,
                                      const std::string& accept, std::string* error) {
  std::string target = url;
  for (int hop = 0; hop <= kMaxRedirects; ++hop) {
    if (!AllowedDownloadUrl(target, options)) {
      *error = "host not allowed";
      return std::nullopt;
    }
    const auto response = Get(options, target, "", limit, accept);
    if (Redirect(response.status_code)) {
      target = Header(response, "location");
      continue;
    }
    if (response.status_code != 200 || !response.ok()) {
      *error = Describe(response);
      return std::nullopt;
    }
    return std::string(response.body.begin(), response.body.end());
  }
  *error = "too many redirects";
  return std::nullopt;
}

DownloadResult DownloadAsset(const std::string& url, std::uint64_t size, const std::string& sha256,
                             const std::string& path, const DownloadOptions& options) {
  DownloadResult result;
  auto fail = [&](std::string reason) {
    result.error = std::move(reason);
    Log(options, "download " + path + " failed: " + result.error);
    return result;
  };
  if (!size) return fail("empty asset");
  // GitHub's digest is the only check on the bytes: without one, nothing is installed.
  if (!IsSha256(sha256)) return fail("no checksum for this download");
  const std::string part = path + ".part";
  struct stat st{};
  // A partial that isn't a plain file (a link put there, say) is never written through: it goes.
  if (lstat(part.c_str(), &st) == 0 && !S_ISREG(st.st_mode)) unlink(part.c_str());
  std::uint64_t done = lstat(part.c_str(), &st) == 0 ? static_cast<std::uint64_t>(st.st_size) : 0;
  if (done > size) {
    unlink(part.c_str());
    done = 0;
  }
  if (done) Log(options, "download " + path + " resumes at " + std::to_string(done));
  const int fd = open(part.c_str(), O_CREAT | O_WRONLY | O_NOFOLLOW | O_CLOEXEC, 0666);
  if (fd < 0) return fail("cannot create " + part + " errno=" + std::to_string(errno));
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
    close(fd);
    return fail("cannot create " + part + ": not a file");
  }
  fchmod(fd, 0666);
  if (ftruncate(fd, static_cast<off_t>(done)) != 0) {
    close(fd);
    return fail("cannot resize " + part);
  }
  std::string target;
  int attempts = 0;
  while (done < size) {
    if (Cancelled(options)) {
      close(fd);
      result.cancelled = true;
      return fail("cancelled");
    }
    if (target.empty()) {
      std::string error;
      const auto resolved = Resolve(options, url, &error);
      if (!resolved) {
        if (Cancelled(options)) continue;
        if (++attempts >= options.max_attempts) { close(fd); return fail(error); }
        Wait(options, attempts);
        continue;
      }
      target = *resolved;
    }
    const std::uint64_t end = std::min(size, done + options.chunk_bytes) - 1;
    const auto response = Get(options, target, std::to_string(done) + "-" + std::to_string(end),
                              static_cast<std::size_t>(end - done + 1 + 1024));
    // The bytes asked for, and only those: "Content-Range: bytes <done>-<end>/<size>".
    const std::string range = "bytes " + std::to_string(done) + "-" + std::to_string(end) + "/";
    if (response.status_code == 206 && response.ok() && response.body.size() == end - done + 1 &&
        Header(response, "content-range").rfind(range, 0) == 0) {
      std::size_t written = 0;
      while (written < response.body.size()) {
        const ssize_t n = pwrite(fd, response.body.data() + written, response.body.size() - written,
                                 static_cast<off_t>(done + written));
        if (n <= 0) { close(fd); return fail("write failed errno=" + std::to_string(errno)); }
        written += static_cast<std::size_t>(n);
      }
      done += response.body.size();
      attempts = 0;
      if (options.progress) options.progress(done, size);
      continue;
    }
    if (Cancelled(options)) continue;
    Log(options, "download chunk at " + std::to_string(done) + ": " + Describe(response));
    // An expired signed link or a server that ignores Range: ask GitHub for a new link.
    if (response.status_code == 401 || response.status_code == 403 || response.status_code == 404 ||
        response.status_code == 200 || response.status_code == 416)
      target.clear();
    if (++attempts >= options.max_attempts) { close(fd); return fail("download keeps failing (" + Describe(response) + ")"); }
    Wait(options, attempts);
  }
  const bool synced = fsync(fd) == 0;
  close(fd);
  if (!synced) return fail("cannot sync " + part);
  const auto actual = options.hash_file ? options.hash_file(part) : std::nullopt;
  if (!actual) return fail("cannot read the download to check it");  // the bytes stay for a retry
  if (*actual != sha256) {
    unlink(part.c_str());
    return fail("checksum mismatch");
  }
  if (rename(part.c_str(), path.c_str()) != 0) return fail("cannot rename " + part);
  result.ok = true;
  return result;
}

}  // namespace playden::update
