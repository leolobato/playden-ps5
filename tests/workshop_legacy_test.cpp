// SPDX-License-Identifier: GPL-3.0-or-later
// Old-style Steam Workshop items (#1319, Left 4 Dead 2): which ones Playden fetches, and the download of their one file
// from Steam's user content servers in Range pieces, checked against the SHA-1 their URL names.
#include "steam/backend/depot_installer.h"
#include "steam/backend/workshop_legacy.h"

#include <mbedtls/sha1.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

using namespace playden::steam;
namespace fs = std::filesystem;

namespace {

int failures = 0;
#define CHECK(condition)                                                              \
  do {                                                                                \
    if (!(condition)) {                                                               \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
      ++failures;                                                                     \
    }                                                                                 \
  } while (false)

std::string Sha1Hex(const std::string& data) {
  unsigned char digest[20];
  mbedtls_sha1(reinterpret_cast<const unsigned char*>(data.data()), data.size(), digest);
  static const char* kDigits = "0123456789abcdef";
  std::string out;
  for (unsigned char c : digest) {
    out += kDigits[c >> 4];
    out += kDigits[c & 15];
  }
  return out;
}

std::string Upper(std::string text) {
  for (auto& c : text) c = static_cast<char>(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
  return text;
}

std::string Read(const fs::path& path) {
  FILE* file = std::fopen(path.c_str(), "rb");
  if (!file) return "<missing>";
  std::string text;
  char buffer[4096];
  std::size_t got;
  while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) text.append(buffer, got);
  std::fclose(file);
  return text;
}

WorkshopItem Item(const std::string& content, const std::string& filename = "myl4d2addons/cool map.vpk") {
  WorkshopItem item;
  item.id = 3704729794;
  item.manifest = 16054162598354285682ull;  // the UGC handle
  item.file_size = content.size();
  item.filename = filename;
  item.file_url = "https://cdn.steamusercontent.com/ugc/16054162598354285682/" + Upper(Sha1Hex(content)) + "/";
  item.has_file_url = true;
  return item;
}

struct Staging {
  fs::path root;
  Staging() {
    char name[] = "/tmp/playden-legacy-XXXXXX";
    CHECK(mkdtemp(name));
    root = fs::canonical(name);
  }
  ~Staging() {
    std::error_code ignored;
    fs::remove_all(root, ignored);
  }
};

// Serves `content` the way the CDN does: a Range asks for a piece (206), none for the whole file (200).
struct FakeServer {
  std::string content;
  std::vector<std::string> ranges;
  std::function<bool(HttpResponse*)> before;  // optional: returns true to answer with *response as it is
  LegacyWorkshopPerform Perform() {
    return [this](const HttpRequest& request, const std::atomic<bool>&) {
      HttpResponse response;
      if (before && before(&response)) return response;
      std::string range;
      for (const auto& [name, value] : request.headers)
        if (name == "Range") range = value;
      ranges.push_back(range);
      std::size_t first = 0, last = content.size() - 1;
      const bool ranged = !range.empty() && std::sscanf(range.c_str(), "bytes=%zu-%zu", &first, &last) == 2;
      last = std::min(last, content.size() - 1);
      response.status_code = ranged ? 206 : 200;
      if (ranged)
        response.headers.emplace_back("Content-Range", "bytes " + std::to_string(first) + "-" + std::to_string(last) +
                                                           "/" + std::to_string(content.size()));
      const auto body = content.substr(first, last - first + 1);
      // Two blocks, as a transfer would arrive.
      const auto half = body.size() / 2;
      if (request.body_sink && (!request.body_sink(reinterpret_cast<const std::uint8_t*>(body.data()), half) ||
                                !request.body_sink(reinterpret_cast<const std::uint8_t*>(body.data()) + half,
                                                   body.size() - half)))
        response.error = HttpError::kSinkFailed;
      return response;
    };
  }
};

void TestFileOf() {
  const auto item = Item("abc");
  const auto file = LegacyWorkshopFileOf(item);
  CHECK(file && file->name == "cool map.vpk" && file->bytes == 3 && file->sha1 == Sha1Hex("abc"));
  // Other hosts on steamusercontent.com, and no trailing slash.
  auto other = item;
  other.file_url = "https://images.steamusercontent.com/ugc/1/" + Sha1Hex("abc");
  CHECK(LegacyWorkshopFileOf(other));
  // A depot item is never one.
  auto depot = item;
  depot.file_url.clear();
  depot.has_file_url = false;
  CHECK(!LegacyWorkshopFileOf(depot));
  // Not fetched: plain http, another host, a look-alike host, a port, no SHA-1, a query, a URL cut off for length.
  for (const std::string& url : std::vector<std::string>{"http://cdn.steamusercontent.com/ugc/1/" + Sha1Hex("abc") + "/",
        "https://example.com/ugc/1/" + Sha1Hex("abc") + "/",
        "https://evilsteamusercontent.com/ugc/1/" + Sha1Hex("abc") + "/",
        "https://cdn.steamusercontent.com:8443/ugc/1/" + Sha1Hex("abc") + "/",
        "https://cdn.steamusercontent.com/ugc/1/notahash/",
        "https://cdn.steamusercontent.com/ugc/1/" + Sha1Hex("abc") + "/?x=1",
        "https://cdn.steamusercontent.com/ugc/x/" + Sha1Hex("abc") + "/", std::string()}) {
    auto bad = item;
    bad.file_url = url;
    CHECK(!LegacyWorkshopFileOf(bad));
  }
  // Names: Steam's folders go, but a name Windows or Playden can't take is not fetched.
  for (const std::string& name : std::vector<std::string>{"", "dir/", "..", "a:b.vpk", "bad?.vpk", ".playden-x", "x.vpk.", "tab\tname.vpk",
                                 std::string(256, 'a')}) {
    auto bad = item;
    bad.filename = name;
    CHECK(!LegacyWorkshopFileOf(bad));
  }
  auto windows = item;
  windows.filename = "addons\\workshop\\x.vpk";
  CHECK(LegacyWorkshopFileOf(windows) && LegacyWorkshopFileOf(windows)->name == "x.vpk");
  auto no_handle = item;
  no_handle.manifest = 0;  // no UGC handle: nothing to tell versions apart by
  CHECK(!LegacyWorkshopFileOf(no_handle));
  auto biggest = item;
  biggest.file_size = 0x7fffffff;  // what the emulator's int32 size can tell the game
  CHECK(LegacyWorkshopFileOf(biggest));
  auto empty = item;
  empty.file_size = 0;
  CHECK(!LegacyWorkshopFileOf(empty));
  auto huge = item;
  huge.file_size = kLegacyWorkshopMaxBytes + 1;
  CHECK(!LegacyWorkshopFileOf(huge));
}

void TestDownload() {
  std::atomic<bool> cancel{false};
  std::string content;
  for (int i = 0; i < 1000; ++i) content += static_cast<char>('a' + i % 26);
  {
    // In pieces of 300 bytes: four Range requests, then the file, its list and its tree.
    Staging staging;
    FakeServer server{content};
    std::vector<std::uint64_t> done;
    const auto result = DownloadLegacyWorkshopItem(server.Perform(), Item(content), staging.root.string(), cancel,
                                                   [&done](const InstallProgress& p) { done.push_back(p.download_done); },
                                                   300);
    CHECK(result.error == InstallError::kNone);
    CHECK(server.ranges == (std::vector<std::string>{"bytes=0-299", "bytes=300-599", "bytes=600-899", "bytes=900-999"}));
    CHECK(Read(staging.root / "cool map.vpk") == content);
    CHECK(Read(staging.root / DepotInstaller::kFileList) == "f\tcool map.vpk\n");
    CHECK(Read(staging.root / DepotInstaller::kFileTree) == "v1\n" + Sha1Hex(content) + "\t1000\tcool map.vpk\n");
    CHECK(!done.empty() && done.back() == 1000);
  }
  {
    // A server that sends the whole file for the only piece (200) is fine.
    Staging staging;
    FakeServer server{content};
    server.before = [&server](HttpResponse*) { server.ranges.clear(); return false; };
    auto perform = server.Perform();
    LegacyWorkshopPerform whole = [&](const HttpRequest& request, const std::atomic<bool>& stop) {
      auto copy = request;
      copy.headers.clear();
      return perform(copy, stop);
    };
    CHECK(DownloadLegacyWorkshopItem(whole, Item(content), staging.root.string(), cancel, {}).error == InstallError::kNone);
    CHECK(Read(staging.root / "cool map.vpk") == content);
  }
  {
    // Different bytes than the URL's SHA-1: corrupt.
    Staging staging;
    FakeServer server{content};
    auto item = Item(content);
    item.file_url = "https://cdn.steamusercontent.com/ugc/1/" + Sha1Hex("other") + "/";
    const auto result = DownloadLegacyWorkshopItem(server.Perform(), item, staging.root.string(), cancel, {});
    CHECK(result.error == InstallError::kCorrupt && !result.refused);
    CHECK(!fs::exists(staging.root / DepotInstaller::kFileTree));
  }
  {
    // The server doesn't have it: refused at once, no retries.
    Staging staging;
    FakeServer server{content};
    int calls = 0;
    server.before = [&calls](HttpResponse* response) {
      ++calls;
      response->status_code = 404;
      return true;
    };
    const auto result = DownloadLegacyWorkshopItem(server.Perform(), Item(content), staging.root.string(), cancel, {});
    CHECK(result.error == InstallError::kRejected && result.detail == "file unavailable http=404" && calls == 1);
  }
  {
    // A busy server, then the piece: retried. Always busy: a network problem, after a few tries.
    Staging staging;
    FakeServer server{content};
    int calls = 0;
    server.before = [&calls](HttpResponse* response) {
      if (++calls > 2) return false;
      response->status_code = 503;
      return true;
    };
    CHECK(DownloadLegacyWorkshopItem(server.Perform(), Item(content), staging.root.string(), cancel, {}).error ==
          InstallError::kNone);
    Staging again;
    calls = 0;
    server.before = [&calls](HttpResponse* response) {
      ++calls;
      response->error = HttpError::kTimeout;
      return true;
    };
    const auto result = DownloadLegacyWorkshopItem(server.Perform(), Item(content), again.root.string(), cancel, {});
    CHECK(result.error == InstallError::kNetwork && calls == 4 && result.detail.find("transfer timeout") != std::string::npos);
  }
  {
    // Canceled: stops before asking.
    Staging staging;
    std::atomic<bool> stop{true};
    FakeServer server{content};
    CHECK(DownloadLegacyWorkshopItem(server.Perform(), Item(content), staging.root.string(), stop, {}).error ==
          InstallError::kCanceled);
    CHECK(server.ranges.empty());
  }
  {
    // A server that ignores the Range for a file in several pieces, or sends another piece: only this item fails
    // (corrupt), after a few tries; the sync goes on with the next one.
    Staging staging;
    FakeServer server{content};
    auto perform = server.Perform();
    LegacyWorkshopPerform no_ranges = [&](const HttpRequest& request, const std::atomic<bool>& stop) {
      auto copy = request;
      copy.headers.clear();
      return perform(copy, stop);
    };
    const auto ignored = DownloadLegacyWorkshopItem(no_ranges, Item(content), staging.root.string(), cancel, {}, 300);
    CHECK(ignored.error == InstallError::kCorrupt && ignored.detail == "file transfer ignored range");
    Staging other;
    int calls = 0;
    LegacyWorkshopPerform shifted = [&](const HttpRequest& request, const std::atomic<bool>& stop) {
      ++calls;
      auto response = perform(request, stop);
      for (auto& [name, value] : response.headers)
        if (name == "Content-Range") value = "bytes 1-300/1000";
      return response;
    };
    const auto wrong = DownloadLegacyWorkshopItem(shifted, Item(content), other.root.string(), cancel, {}, 300);
    CHECK(wrong.error == InstallError::kCorrupt && wrong.detail == "file transfer wrong range" && calls == 4);
  }
  {
    // Canceled after the transfer, while the file is checked: canceled, no file list.
    Staging staging;
    std::atomic<bool> stop{false};
    FakeServer server{content};
    const auto result = DownloadLegacyWorkshopItem(server.Perform(), Item(content), staging.root.string(), stop,
                                                   [&stop](const InstallProgress& p) {
                                                     if (p.download_done == 1000) stop = true;
                                                   });
    CHECK(result.error == InstallError::kCanceled && !fs::exists(staging.root / DepotInstaller::kFileList));
  }
  {
    // The files are 0644 whatever the umask.
    Staging staging;
    FakeServer server{content};
    const auto mask = ::umask(077);
    CHECK(DownloadLegacyWorkshopItem(server.Perform(), Item(content), staging.root.string(), cancel, {}).error ==
          InstallError::kNone);
    ::umask(mask);
    struct stat info{};
    CHECK(::stat((staging.root / "cool map.vpk").c_str(), &info) == 0 && (info.st_mode & 0777) == 0644);
    CHECK(::stat((staging.root / DepotInstaller::kFileTree).c_str(), &info) == 0 && (info.st_mode & 0777) == 0644);
  }
  {
    // The preview picture downloads after the file, checked against its own URL, and is listed beside it. A preview
    // that fails is left out; the item still installs.
    const std::string picture = "JPEG picture bytes";
    std::map<std::string, std::string> files{{"/file", content}, {"/preview", picture}};
    std::vector<std::string> asked;
    int preview_status = 206;
    LegacyWorkshopPerform server = [&](const HttpRequest& request, const std::atomic<bool>&) {
      asked.push_back(request.url);
      HttpResponse response;
      const bool preview = request.url.find(Upper(Sha1Hex(picture))) != std::string::npos;
      const auto& body = preview ? picture : content;
      response.status_code = preview ? preview_status : 206;
      if (response.status_code != 206) return response;
      response.headers.emplace_back("Content-Range", "bytes 0-" + std::to_string(body.size() - 1) + "/" +
                                                         std::to_string(body.size()));
      request.body_sink(reinterpret_cast<const std::uint8_t*>(body.data()), body.size());
      return response;
    };
    auto item = Item(content);
    item.preview_url = "https://images.steamusercontent.com/ugc/99/" + Upper(Sha1Hex(picture)) + "/";
    item.preview_size = picture.size();
    CHECK(LegacyWorkshopPreviewOf(item) && LegacyWorkshopPreviewOf(item)->name == "preview.jpg");
    Staging staging;
    CHECK(DownloadLegacyWorkshopItem(server, item, staging.root.string(), cancel, {}).error == InstallError::kNone);
    CHECK(asked.size() == 2 && Read(staging.root / "preview.jpg") == picture);
    CHECK(Read(staging.root / DepotInstaller::kFileList) == "f\tcool map.vpk\nf\tpreview.jpg\n");
    CHECK(Read(staging.root / DepotInstaller::kFileTree) == "v1\n" + Sha1Hex(content) + "\t1000\tcool map.vpk\n" +
                                                               Sha1Hex(picture) + "\t" + std::to_string(picture.size()) +
                                                               "\tpreview.jpg\n");
    Staging without;
    preview_status = 404;
    CHECK(DownloadLegacyWorkshopItem(server, item, without.root.string(), cancel, {}).error == InstallError::kNone);
    CHECK(!fs::exists(without.root / "preview.jpg"));
    CHECK(Read(without.root / DepotInstaller::kFileList) == "f\tcool map.vpk\n");
    // No preview when it's too large, has no usable URL, or the item's own file is called preview.jpg.
    auto huge = item;
    huge.preview_size = kLegacyWorkshopPreviewMaxBytes + 1;
    CHECK(!LegacyWorkshopPreviewOf(huge));
    auto elsewhere = item;
    elsewhere.preview_url = "https://example.com/ugc/99/" + Sha1Hex(picture) + "/";
    CHECK(!LegacyWorkshopPreviewOf(elsewhere));
    auto clash = item;
    clash.filename = "Preview.JPG";
    CHECK(!LegacyWorkshopPreviewOf(clash));
  }
  {
    // Not a fetchable item: refused before anything is asked.
    Staging staging;
    FakeServer server{content};
    auto item = Item(content);
    item.file_url = "https://example.com/x";
    CHECK(DownloadLegacyWorkshopItem(server.Perform(), item, staging.root.string(), cancel, {}).error ==
          InstallError::kProtocol);
    CHECK(server.ranges.empty() && fs::is_empty(staging.root));
  }
}

}  // namespace

int main() {
  TestFileOf();
  TestDownload();
  if (failures) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("workshop_legacy: ok\n");
  return 0;
}
