#include "update/asset_download.h"
#include "update/file_ops.h"
#include "zip_fixture.h"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

using namespace playden::update;
using playden::steam::HttpError;
using playden::steam::HttpRequest;
using playden::steam::HttpResponse;
#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #value); std::exit(1); } } while(false)

namespace {
const std::string kPage = "https://github.com/o/r/releases/download/v1/a.zip";
const std::string kSigned = "https://release-assets.githubusercontent.com/asset?sig=";

// A fake GitHub: the page redirects to a signed link that serves Range requests.
struct FakeGitHub {
  std::string body;
  int signature = 1;
  int expire_after = -1;     // the signed link answers 403 after this many range requests
  int fail_once_at = -1;     // one network error at this request number
  bool ignore_range = false;
  bool wrong_content_range = false;  // 206 with the right length but another range named
  std::string redirect_to;   // overrides the signed link
  int requests = 0, ranges = 0, page_hits = 0;
  std::vector<std::string> ranges_seen;

  HttpResponse operator()(const HttpRequest& request) {
    ++requests;
    HttpResponse response;
    std::string range, agent;
    for (const auto& [key, value] : request.headers) {
      if (key == "Range") range = value;
      if (key == "User-Agent") agent = value;
    }
    CHECK(!agent.empty());
    if (requests == fail_once_at) {
      response.error = HttpError::kConnectionFailed;
      return response;
    }
    if (request.url == kPage) {
      ++page_hits;
      response.status_code = 302;
      response.headers.push_back({"Location", redirect_to.empty() ? kSigned + std::to_string(signature) : redirect_to});
      return response;
    }
    if (request.url.rfind(kSigned, 0) != 0) {
      response.status_code = 404;
      return response;
    }
    if (request.url != kSigned + std::to_string(signature)) {
      response.status_code = 403;
      return response;
    }
    if (range.rfind("bytes=", 0) != 0 || ignore_range) {
      response.status_code = 200;
      response.body.assign(body.begin(), body.end());
      return response;
    }
    const auto dash = range.find('-');
    const std::size_t from = std::stoull(range.substr(6, dash - 6));
    const std::size_t to = std::min<std::size_t>(std::stoull(range.substr(dash + 1)), body.size() - 1);
    if (range != "bytes=0-0") {
      ++ranges;
      ranges_seen.push_back(range);
      if (expire_after >= 0 && ranges > expire_after) {
        ++signature;  // the old link no longer works; a new one is handed out
        expire_after = -1;
        response.status_code = 403;
        return response;
      }
    }
    response.status_code = 206;
    response.body.assign(body.begin() + from, body.begin() + to + 1);
    const std::size_t named = wrong_content_range && from > 0 ? from - 1 : from;
    response.headers.push_back({"Content-Range", "bytes " + std::to_string(named) + "-" +
                                                     std::to_string(named + to - from) + "/" + std::to_string(body.size())});
    return response;
  }
};

DownloadOptions Options(FakeGitHub& github, const std::string& good_body) {
  DownloadOptions options;
  options.fetch = [&github](const HttpRequest& request, const std::atomic<bool>&) { return github(request); };
  options.chunk_bytes = 10;
  options.wait_before_retry = [](int) {};
  options.hash_file = [good_body](const std::string& path) -> std::optional<std::string> {
    std::string text;
    if (!ReadSmallFile(path, &text)) return std::nullopt;
    return text == good_body ? std::string(64, 'a') : std::string(64, 'b');
  };
  return options;
}
}  // namespace

int main() {
  const std::string root = zip_fixture::TempFolder("download");
  const std::string body = "0123456789abcdefghijklmnopqrstuvwxyz!";  // 37 bytes = 4 chunks
  const std::string good(64, 'a');

  CHECK(AllowedDownloadUrl("https://github.com/o/r"));
  CHECK(AllowedDownloadUrl("https://api.github.com/repos/o/r/releases/latest"));
  CHECK(AllowedDownloadUrl("https://release-assets.githubusercontent.com/x"));
  CHECK(AllowedDownloadUrl("https://objects.githubusercontent.com/x"));
  for (const char* bad : {"http://github.com/x", "https://evil.com/x", "https://githubusercontent.com.evil.com/x",
                          "https://github.com.evil.com/x", "https://user@github.com/x", "https://github.com:8443/x",
                          "https://evilgithubusercontent.com/x", "ftp://github.com/x"})
    CHECK(!AllowedDownloadUrl(bad));
  // A download with its own prefix (the publisher pack): only URLs under it, over HTTPS; GitHub is not allowed then.
  {
    DownloadOptions options;
    CHECK(AllowedDownloadUrl("https://github.com/o/r", options));
    options.allowed_url_prefix = "https://builds.playden.club/packs/";
    CHECK(AllowedDownloadUrl("https://builds.playden.club/packs/1/playden-rgschost-1.tzst", options));
    for (const char* bad : {"https://github.com/o/r", "http://builds.playden.club/packs/1/a.tzst",
                            "https://builds.playden.club/kits/a.zip", "https://builds.playden.club.evil.com/packs/a",
                            "https://builds.playden.club/packs/../kits/a.zip", "https://builds.playden.club/packs/%2e%2e/a",
                            "https://builds.playden.club/packs/a\\..\\b", "https://builds.playden.club/packs/@evil.com/a"})
      CHECK(!AllowedDownloadUrl(bad, options));
    // A prefix that doesn't name its host up to a "/" allows nothing.
    for (const char* prefix : {"https://builds.playden.club", "http://builds.playden.club/packs/", "https:///packs/"}) {
      options.allowed_url_prefix = prefix;
      CHECK(!AllowedDownloadUrl("https://builds.playden.club/packs/1/a.tzst", options));
    }
  }

  // Plain download: redirect, four ranged chunks, progress, hash check, rename.
  {
    FakeGitHub github;
    github.body = body;
    auto options = Options(github, body);
    std::vector<std::uint64_t> progress;
    options.progress = [&](std::uint64_t done, std::uint64_t total) { CHECK(total == 37); progress.push_back(done); };
    const auto result = DownloadAsset(kPage, body.size(), good, root + "/a.zip", options);
    CHECK(result.ok);
    std::string text;
    CHECK(ReadSmallFile(root + "/a.zip", &text) && text == body && !Exists(root + "/a.zip.part"));
    CHECK(progress == (std::vector<std::uint64_t>{10, 20, 30, 37}));
    CHECK(github.ranges_seen.front() == "bytes=0-9" && github.ranges_seen.back() == "bytes=30-36");
  }
  // No checksum from GitHub: nothing is downloaded or installed.
  {
    FakeGitHub github;
    github.body = body;
    const auto result = DownloadAsset(kPage, body.size(), "", root + "/n.zip", Options(github, body));
    CHECK(!result.ok && github.requests == 0 && !Exists(root + "/n.zip"));
  }
  // A 206 that names other bytes than the ones asked for is not written.
  {
    FakeGitHub github;
    github.body = body;
    github.wrong_content_range = true;
    auto options = Options(github, body);
    options.max_attempts = 3;
    const auto result = DownloadAsset(kPage, body.size(), good, root + "/w.zip", options);
    std::string part;
    CHECK(!result.ok && ReadSmallFile(root + "/w.zip.part", &part) && part == body.substr(0, 10));
  }
  // Resume: an existing .part is kept and only the rest is fetched.
  {
    FakeGitHub github;
    github.body = body;
    CHECK(zip_fixture::Write(root + "/b.zip.part", body.substr(0, 20)));
    const auto result = DownloadAsset(kPage, body.size(), good, root + "/b.zip", Options(github, body));
    CHECK(result.ok && github.ranges_seen.front() == "bytes=20-29");
    // A .part longer than the asset starts over.
    CHECK(zip_fixture::Write(root + "/c.zip.part", body + "extra"));
    FakeGitHub again;
    again.body = body;
    CHECK(DownloadAsset(kPage, body.size(), good, root + "/c.zip", Options(again, body)).ok);
    CHECK(again.ranges_seen.front() == "bytes=0-9");
  }
  // An expired signed link is replaced by following the page again; one network error is retried.
  {
    FakeGitHub github;
    github.body = body;
    github.expire_after = 2;
    github.fail_once_at = 3;
    CHECK(DownloadAsset(kPage, body.size(), good, root + "/d.zip", Options(github, body)).ok);
    CHECK(github.page_hits == 2);
  }
  // Wrong content fails the hash check and leaves nothing behind.
  {
    FakeGitHub github;
    github.body = body;
    github.body[5] = 'X';
    const auto result = DownloadAsset(kPage, body.size(), good, root + "/e.zip", Options(github, body));
    CHECK(!result.ok && result.error == "checksum mismatch" && !Exists(root + "/e.zip") && !Exists(root + "/e.zip.part"));
  }
  // A download that can't be read back to hash fails without throwing the bytes away.
  {
    FakeGitHub github;
    github.body = body;
    auto options = Options(github, body);
    options.hash_file = [](const std::string&) -> std::optional<std::string> { return std::nullopt; };
    const auto result = DownloadAsset(kPage, body.size(), good, root + "/i.zip", options);
    CHECK(!result.ok && result.error.find("cannot read") != std::string::npos && Exists(root + "/i.zip.part"));
  }
  // Redirects outside GitHub or to plain HTTP are refused.
  for (const char* target : {"https://evil.example/x", "http://release-assets.githubusercontent.com/x"}) {
    FakeGitHub github;
    github.body = body;
    github.redirect_to = target;
    const auto result = DownloadAsset(kPage, body.size(), good, root + "/f.zip", Options(github, body));
    CHECK(!result.ok && result.error.find("not allowed") != std::string::npos);
  }
  // A server that ignores Range gives up after the allowed attempts.
  {
    FakeGitHub github;
    github.body = body;
    github.ignore_range = true;
    CHECK(!DownloadAsset(kPage, body.size(), good, root + "/g.zip", Options(github, body)).ok);
  }
  // Cancel stops before the next chunk and keeps the bytes for later.
  {
    FakeGitHub github;
    github.body = body;
    std::atomic<bool> cancel{false};
    auto options = Options(github, body);
    options.cancel = &cancel;
    options.progress = [&](std::uint64_t done, std::uint64_t) { if (done >= 20) cancel = true; };
    const auto result = DownloadAsset(kPage, body.size(), good, root + "/h.zip", options);
    CHECK(!result.ok && result.cancelled);
    std::string part;
    CHECK(ReadSmallFile(root + "/h.zip.part", &part) && part == body.substr(0, 20));
  }
  // FetchSmall follows the redirect and returns the body.
  {
    FakeGitHub github;
    github.body = body;
    github.ignore_range = true;
    std::string error;
    const auto text = FetchSmall(kPage, 1024, Options(github, body), "application/json", &error);
    CHECK(text && *text == body);
  }
  RemoveTree(root);
  std::puts("ok");
  return 0;
}
