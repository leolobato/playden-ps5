#include "update/file_ops.h"
#include "update/update_service.h"
#include "zip_fixture.h"

#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace playden::update;
using playden::steam::HttpRequest;
using playden::steam::HttpResponse;
using zip_fixture::Entry;
#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #value); std::exit(1); } } while(false)

namespace {
const std::string kLatest = "https://api.github.com/repos/o/r/releases/latest";
const std::string kReleases = "https://api.github.com/repos/o/r/releases?per_page=100";
const std::string kDownload = "https://github.com/o/r/releases/download/v0.3.0/";
const std::string kKitSha = std::string(64, 'c');

std::string Read(const std::string& path) {
  std::string text;
  return ReadSmallFile(path, &text) ? text : "<missing>";
}
// A stand-in for SHA-256 (FNV-1a, 64 hex digits): the service only compares digests.
std::string FakeSha(const std::string& text) {
  std::uint64_t hash = 1469598103934665603ull;
  for (unsigned char c : text) hash = (hash ^ c) * 1099511628211ull;
  char hex[17];
  std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(hash));
  return std::string(hex) + hex + hex + hex;
}
void Put(const std::string& path, const std::string& text) {
  CHECK(MakeParents(path));
  CHECK(zip_fixture::Write(path, text));
}

// Fake GitHub: a release, its update manifest and two zips, served with Range support.
struct Server {
  std::map<std::string, std::string> files;
  std::string latest;
  std::string releases;  // the list, newest first
  int kit_downloads = 0;
  int requests = 0;

  HttpResponse operator()(const HttpRequest& request) {
    HttpResponse response;
    std::string range;
    for (const auto& [key, value] : request.headers)
      if (key == "Range") range = value;
    ++requests;
    if (request.url == kLatest || request.url == kReleases) {
      const std::string& body = request.url == kLatest ? latest : releases;
      response.status_code = 200;
      response.body.assign(body.begin(), body.end());
      return response;
    }
    const auto found = files.find(request.url);
    if (found == files.end()) {
      response.status_code = 404;
      return response;
    }
    const std::string& body = found->second;
    if (range.empty()) {
      response.status_code = 200;
      response.body.assign(body.begin(), body.end());
      return response;
    }
    if (request.url.find("kit") != std::string::npos && range != "bytes=0-0") ++kit_downloads;
    const auto dash = range.find('-');
    const std::size_t from = std::stoull(range.substr(6, dash - 6));
    const std::size_t to = std::min<std::size_t>(std::stoull(range.substr(dash + 1)), body.size() - 1);
    response.status_code = 206;
    response.body.assign(body.begin() + from, body.begin() + to + 1);
    response.headers.push_back({"Content-Range", "bytes " + std::to_string(from) + "-" + std::to_string(to) + "/" +
                                                     std::to_string(body.size())});
    return response;
  }
};

std::string AppZip(const std::string& version_text, const std::string& content_version, const std::string& full_version = "") {
  // The app folder at the top, as ProsperoStore installs it, with the tools next to it (0.7.1 and older zips had
  // both one folder down).
  return zip_fixture::Build({{"tools/console_title_deploy.py", "py"},
                             {"FAKE77031/LICENSE", "license"},
                             {"FAKE77031/eboot.bin", "eboot " + version_text},
                             {"FAKE77031/playden-version", full_version.empty() ? version_text + "\n" : full_version},
                             {"FAKE77031/sce_sys/param.json",
                              R"({"titleId":"FAKE77031","contentVersion":")" + content_version + R"("})"},
                             {"FAKE77031/assets/a.txt", "asset " + version_text},
                             {"FAKE77031/logs/", "", 040755}});
}

std::string KitZip() {
  return zip_fixture::Build({{"playden-kit-h-r9/LICENSES/x", "license"},
                             {"playden-kit-h-r9/kitid/manifest.json", kKitSha},
                             {"playden-kit-h-r9/kitid/runtime/wine.bin", std::string(100, 'w')}});
}

void Publish(Server& server, const std::string& tag, const std::string& min_from = "0.2.0",
             const std::string& content_version = "00.003.000", bool with_kit = true, bool with_digests = true,
             const std::string& kit_revision = "r9", const std::string& full_version = "") {
  const std::string version_text = tag.substr(1);
  const std::string app = AppZip(version_text, content_version, full_version);
  const std::string kit = KitZip();
  const std::string manifest =
      R"({"format":1,"app":{"version":")" + version_text + R"(","title_id":"FAKE77031","min_from":")" + min_from +
      R"(","asset":"playden-ps5-)" + version_text + R"(.zip","prefix":"FAKE77031/"},"kit":{"kit_id":"kitid","revision":")" + kit_revision + R"(","manifest_sha256":")" + kKitSha +
      R"(","asset":"playden-kit-h-r9.zip","prefix":"playden-kit-h-r9/kitid/"},"notes":"New things."})";
  server.files[kDownload + "playden-update.json"] = manifest;
  server.files[kDownload + "playden-ps5-" + version_text + ".zip"] = app;
  server.files[kDownload + "playden-kit-h-r9.zip"] = kit;
  auto asset = [&](const std::string& name, const std::string& body) {
    return R"({"name":")" + name + R"(","size":)" + std::to_string(body.size()) +
           (with_digests ? R"(,"digest":"sha256:)" + FakeSha(body) + "\"" : std::string()) +
           R"(,"browser_download_url":")" + kDownload + name + R"("})";
  };
  server.latest = R"({"tag_name":")" + tag + R"(","draft":false,"prerelease":false,"assets":[)" +
                  asset("playden-update.json", manifest) + "," + asset("playden-ps5-" + version_text + ".zip", app) +
                  (with_kit ? "," + asset("playden-kit-h-r9.zip", kit) : std::string()) + "]}";
}

struct Fixture {
  std::string root;
  Server server;
  std::uint64_t free = 1ull << 40;
  UpdateConfig Config(Version current) {
    UpdateConfig config;
    config.latest_release_url = kLatest;
    config.releases_page = "https://github.com/o/r/releases";
    config.installed_root = root + "/homebrew/FAKE77031";
    config.work_root = root + "/playden/self-update";
    config.kits_root = root + "/playden/kits";
    config.appmeta_root = root + "/user/appmeta/FAKE77031";
    config.registered_app_root = root + "/user/app/FAKE77031";
    config.current = current;
    config.fetch = [this](const HttpRequest& request, const std::atomic<bool>&) { return server(request); };
    config.hash_file = [](const std::string& path) -> std::optional<std::string> {
      std::string text;
      if (!ReadSmallFile(path, &text, 64 << 20)) return std::nullopt;
      return FakeSha(text);
    };
    config.verify_kit = [](const std::string& kit_root) -> std::optional<std::string> {
      std::string sha;
      if (!ReadSmallFile(kit_root + "/manifest.json", &sha) || !Exists(kit_root + "/runtime/wine.bin")) return std::nullopt;
      return sha;
    };
    config.free_bytes = [this](const std::string&) -> std::optional<std::uint64_t> { return free; };
    config.wait_before_retry = [](int) {};
    config.now = [] { return std::int64_t{1000}; };
    return config;
  }
};

void Wait(const UpdateService& service) {
  for (int i = 0; i < 2000 && service.Busy(); ++i) usleep(5000);
  CHECK(!service.Busy());
}

// The kit-only install for version 0.3.0, on a console with `installed` as its kit (empty: none).
struct KitOnly {
  Fixture fixture;
  std::string kits;
  explicit KitOnly(const std::string& name, const std::string& installed) {
    fixture.root = zip_fixture::TempFolder(name.c_str());
    kits = fixture.root + "/playden/kits";
    if (!installed.empty()) {
      Put(kits + "/kitid/manifest.json", installed);
      Put(kits + "/kitid/runtime/wine.bin", "old");
    }
  }
  UpdateConfig Config() {
    auto config = fixture.Config({0, 3, 0});
    config.kit_id = "kitid";
    config.kit_manifest_sha256 = kKitSha;
    return config;
  }
};

void Installed(const std::string& root) {
  Put(root + "/homebrew/FAKE77031/eboot.bin", "eboot 0.2.0");
  Put(root + "/homebrew/FAKE77031/sce_sys/param.json", "old param");
  Put(root + "/homebrew/FAKE77031/assets/a.txt", "asset 0.2.0");
  Put(root + "/homebrew/FAKE77031/playden-data/session", "session");
  Put(root + "/user/appmeta/FAKE77031/param.json", "old param");
  Put(root + "/user/app/FAKE77031/sce_sys/param.json", "old param");
  Put(root + "/playden/kits/kitid/manifest.json", std::string(64, 'b'));
  Put(root + "/playden/kits/kitid/runtime/wine.bin", "old");
  Put(root + "/playden/kits/.previous-kitid-1/manifest.json", std::string(64, '7'));
}
}  // namespace

int main() {
  // Opt-in selects by version, never GitHub list order; disabling never downgrades.
  {
    Fixture fixture;
    fixture.root = zip_fixture::TempFolder("beta-track");
    Installed(fixture.root);
    Publish(fixture.server, "v0.7.1-beta.2", "0.7.0", "00.007.001");
    auto beta = fixture.server.latest;
    beta.replace(beta.find("\"prerelease\":false"), 18, "\"prerelease\":true");
    fixture.server.releases = "[{\"tag_name\":\"v99.0.0\",\"draft\":true,\"assets\":[]}," + beta +
        ",{\"tag_name\":\"nightly\",\"prerelease\":true,\"assets\":[]},"
        "{\"tag_name\":\"v0.7.0\",\"assets\":[]}]";
    fixture.server.latest = R"({"tag_name":"v0.7.0","assets":[]})";
    auto config = fixture.Config({0, 7, 0});
    config.include_betas = true;
    UpdateService service(config);
    service.Check(true); Wait(service);
    CHECK(service.Snapshot().phase == UpdatePhase::kAvailable);
    CHECK(service.Snapshot().available.ToString() == "0.7.1-beta.2");
    // A stable release wins over every beta of its version, even if listed last.
    Publish(fixture.server, "v0.7.1", "0.7.0", "00.007.001");
    fixture.server.releases = "[" + beta + "," + fixture.server.latest + "]";
    service.Check(true); Wait(service);
    CHECK(service.Snapshot().available.ToString() == "0.7.1");
    config.current = *ParseVersion("0.7.1-beta.2");
    config.include_betas = false;
    Publish(fixture.server, "v0.7.0", "0.7.0", "00.007.000");
    UpdateService stable(config);
    stable.Check(true); Wait(stable);
    CHECK(stable.Snapshot().phase == UpdatePhase::kUpToDate);
    fixture.server.latest = beta;
    stable.Check(true); Wait(stable);
    CHECK(stable.Snapshot().phase == UpdatePhase::kUpToDate);
    Publish(fixture.server, "v0.7.1-beta.3", "0.7.1-beta.2", "00.007.001");
    fixture.server.releases = "[" + fixture.server.latest + "]";
    stable.SetIncludeBetas(true);
    stable.Check(true); Wait(stable);
    CHECK(stable.Snapshot().available.ToString() == "0.7.1-beta.3");
    stable.Install(); Wait(stable);
    CHECK(stable.Snapshot().phase == UpdatePhase::kStaged);
    // The local override uses the same opt-in rule and needs no GitHub request.
    config.include_betas = true;
    config.local_release_dir = fixture.root + "/local";
    auto local_release = fixture.server.latest;
    local_release.replace(local_release.find("\"prerelease\":false"), 18, "\"prerelease\":true");
    Put(config.local_release_dir + "/release.json", local_release);
    Put(config.local_release_dir + "/playden-update.json", fixture.server.files[kDownload + "playden-update.json"]);
    const int before = fixture.server.requests;
    UpdateService local(config);
    local.Check(true); Wait(local);
    CHECK(local.Snapshot().phase == UpdatePhase::kAvailable);
    CHECK(fixture.server.requests == before);
    // The tag must match the full manifest version, including beta number.
    auto mismatch = fixture.server.files[kDownload + "playden-update.json"];
    mismatch.replace(mismatch.find("0.7.1-beta.3"), 12, "0.7.1-beta.4");
    Put(config.local_release_dir + "/playden-update.json", mismatch);
    local.Check(true); Wait(local);
    CHECK(local.Snapshot().phase == UpdatePhase::kFailed);
  }
  // Opting out during a download and cancelling must not leave a beta installable.
  {
    Fixture fixture;
    fixture.root = zip_fixture::TempFolder("beta-cancel-opt-out");
    Installed(fixture.root);
    Publish(fixture.server, "v0.7.1-beta.1", "0.7.0", "00.007.001");
    fixture.server.releases = "[" + fixture.server.latest + "]";
    fixture.server.latest = R"({"tag_name":"v0.7.0","assets":[]})";
    auto config = fixture.Config({0, 7, 0});
    config.include_betas = true;
    UpdateService* running = nullptr;
    bool cancelled = false;
    config.fetch = [&](const HttpRequest& request, const std::atomic<bool>&) {
      if (!cancelled && request.url.find(".zip") != std::string::npos) {
        cancelled = true;
        running->SetIncludeBetas(false);
        running->Check(true);
        running->Cancel();
      }
      return fixture.server(request);
    };
    UpdateService service(config);
    running = &service;
    service.Check(true); Wait(service);
    CHECK(service.Snapshot().phase == UpdatePhase::kAvailable);
    service.Install(); Wait(service);
    CHECK(cancelled && service.Snapshot().phase == UpdatePhase::kAvailable);
    service.Install(); Wait(service);
    CHECK(service.Snapshot().phase == UpdatePhase::kUpToDate);
    RemoveTree(fixture.root);
  }
  // Present full-version metadata must be readable and match, even for stable packages.
  for (const auto& metadata : {std::string(65, 'x'), std::string("0.7.1-beta.9\n")}) {
    Fixture fixture;
    fixture.root = zip_fixture::TempFolder("bad-full-version");
    Installed(fixture.root);
    Publish(fixture.server, "v0.7.1", "0.7.0", "00.007.001", true, true, "r9", metadata);
    UpdateService service(fixture.Config({0, 7, 0}));
    service.Install(); Wait(service);
    CHECK(service.Snapshot().phase == UpdatePhase::kFailed);
    RemoveTree(fixture.root);
  }
  // First run on a console without a kit: the running version's kit is found and put in place at once.
  {
    KitOnly test("kit-fresh", "");
    Publish(test.fixture.server, "v0.3.0");
    test.fixture.server.releases = "[" + test.fixture.server.latest + "]";
    UpdateService service(test.Config());
    service.Start();
    service.CheckKit();
    Wait(service);
    auto kit = service.Snapshot().kit;
    CHECK(kit.phase == KitInstallPhase::kAvailable && !kit.replaces && kit.version == "0.3.0");
    CHECK(kit.download_bytes == test.fixture.server.files[kDownload + "playden-kit-h-r9.zip"].size());
    CHECK(service.Snapshot().phase == UpdatePhase::kIdle);  // the update's own state is untouched
    service.InstallKit();
    Wait(service);
    kit = service.Snapshot().kit;
    CHECK(kit.phase == KitInstallPhase::kInstalled);
    CHECK(Read(test.kits + "/kitid/manifest.json") == kKitSha && Read(test.kits + "/kitid/runtime/wine.bin").size() == 100);
    CHECK(!Exists(test.kits + "/.next-kitid-r9") && !Exists(test.kits + "/.staging-kitid-r9"));
    CHECK(ListFolder(test.fixture.root + "/playden/self-update/downloads").empty());
    // Installed: the next check needs nothing, and asks GitHub for nothing.
    const int requests = test.fixture.server.requests;
    service.CheckKit();
    Wait(service);
    CHECK(service.Snapshot().kit.phase == KitInstallPhase::kNotNeeded && test.fixture.server.requests == requests);
    service.Shutdown();
  }
  // Another kit is installed: the right one replaces it, and the old one is left as a ".replaced-" leftover.
  {
    KitOnly test("kit-replace", std::string(64, 'b'));
    Publish(test.fixture.server, "v0.3.0");
    test.fixture.server.releases = "[" + test.fixture.server.latest + "]";
    UpdateService service(test.Config());
    service.InstallKit();  // checks first
    Wait(service);
    const auto kit = service.Snapshot().kit;
    CHECK(kit.phase == KitInstallPhase::kInstalled && kit.replaces);
    CHECK(Read(test.kits + "/kitid/manifest.json") == kKitSha);
    CHECK(Read(test.kits + "/.replaced-kitid-1000/manifest.json") == std::string(64, 'b'));
    service.Shutdown();
  }
  // Only a missing or older kit is replaced: a newer one (a development kit) or the same version built elsewhere
  // stays, and nothing is downloaded. An older one is offered as before.
  {
    const auto kit_manifest = [](const std::string& version) {
      return R"({"schema":"playden-prospero-kit/1","runtime":{"artifact_revision":"f729534-kit-)" + version + R"("}})";
    };
    for (const char* kept : {"0.13-dev.20261023.1", "0.12"}) {
      KitOnly test(std::string("kit-kept-") + kept, kit_manifest(kept));
      Publish(test.fixture.server, "v0.3.0", "0.2.0", "00.003.000", true, true, "0.12");
      test.fixture.server.releases = "[" + test.fixture.server.latest + "]";
      UpdateService service(test.Config());
      service.InstallKit();  // checks first
      Wait(service);
      CHECK(service.Snapshot().kit.phase == KitInstallPhase::kNotNeeded);
      CHECK(Read(test.kits + "/kitid/manifest.json") == kit_manifest(kept) && test.fixture.server.kit_downloads == 0);
      service.Shutdown();
    }
    KitOnly test("kit-older-version", kit_manifest("0.11"));
    Publish(test.fixture.server, "v0.3.0", "0.2.0", "00.003.000", true, true, "0.12");
    test.fixture.server.releases = "[" + test.fixture.server.latest + "]";
    UpdateService service(test.Config());
    service.CheckKit();
    Wait(service);
    CHECK(service.Snapshot().kit.phase == KitInstallPhase::kAvailable && service.Snapshot().kit.replaces);
    service.Shutdown();
  }
  // The running version's release left the kit zip out (its kit did not change): an older release has it.
  {
    KitOnly test("kit-older", "");
    Publish(test.fixture.server, "v0.2.0");
    const std::string older = test.fixture.server.latest;
    Publish(test.fixture.server, "v0.3.0", "0.2.0", "00.003.000", false);
    Publish(test.fixture.server, "v0.4.0");  // newer than this version: never its kit's source
    const std::string newer = test.fixture.server.latest;
    Publish(test.fixture.server, "v0.3.0", "0.2.0", "00.003.000", false);
    test.fixture.server.releases = "[" + newer + "," + test.fixture.server.latest + "," + older + "]";
    UpdateService service(test.Config());
    service.CheckKit();
    Wait(service);
    const auto kit = service.Snapshot().kit;
    CHECK(kit.phase == KitInstallPhase::kAvailable && kit.version == "0.2.0");
    service.Shutdown();
  }
  // A staged update brings its kit at the restart: the kit-only install leaves it alone, even when asked.
  {
    KitOnly test("kit-staged", "");
    Installed(test.fixture.root);
    unlink((test.kits + "/kitid/manifest.json").c_str());  // no complete kit
    Publish(test.fixture.server, "v0.3.0");
    test.fixture.server.releases = "[" + test.fixture.server.latest + "]";
    auto config = test.Config();
    config.current = {0, 2, 0};
    UpdateService service(std::move(config));
    service.Check(true);
    Wait(service);
    service.Install();
    Wait(service);
    CHECK(service.Snapshot().phase == UpdatePhase::kStaged && Exists(test.kits + "/.next-kitid-r9"));
    service.CheckKit();
    Wait(service);
    CHECK(service.Snapshot().kit.phase == KitInstallPhase::kIdle);
    service.InstallKit();
    Wait(service);
    CHECK(service.Snapshot().kit.phase == KitInstallPhase::kIdle && Exists(test.kits + "/.next-kitid-r9"));
    service.Shutdown();
  }
  // A replaced kit (here one uploaded over FTP) is removed at the next start.
  {
    KitOnly test("kit-leftover", "");
    Put(test.kits + "/.replaced-kitid-5/manifest.json", std::string(64, 'b'));
    Put(test.kits + "/.replaced-kitid-5/runtime/wine.bin", "old");
    UpdateService service(test.Config());
    service.Start();
    CHECK(!Exists(test.kits + "/.replaced-kitid-5"));
    service.Shutdown();
  }
  // A development build has no release, and a release made with another kit doesn't offer it.
  {
    KitOnly test("kit-none", "");
    Publish(test.fixture.server, "v0.2.0");
    test.fixture.server.releases = "[" + test.fixture.server.latest + "]";
    UpdateService service(test.Config());
    service.CheckKit();
    Wait(service);
    CHECK(service.Snapshot().kit.phase == KitInstallPhase::kUnavailable);
    auto other = test.Config();
    other.kit_manifest_sha256 = std::string(64, 'd');
    Publish(test.fixture.server, "v0.3.0");
    test.fixture.server.releases = "[" + test.fixture.server.latest + "]";
    UpdateService another(std::move(other));
    another.CheckKit();
    Wait(another);
    CHECK(another.Snapshot().kit.phase == KitInstallPhase::kUnavailable);
    CHECK(!Exists(test.kits + "/kitid"));
    service.Shutdown();
    another.Shutdown();
  }
  // Full run: check, install (kit and app), restart, swap, then confirm on the next start.
  {
    Fixture fixture;
    fixture.root = zip_fixture::TempFolder("service");
    Installed(fixture.root);
    Publish(fixture.server, "v0.3.0");
    UpdateService service(fixture.Config({0, 2, 0}));
    service.Start();
    CHECK(service.Snapshot().phase == UpdatePhase::kIdle);
    service.Check(true);
    Wait(service);
    auto snapshot = service.Snapshot();
    CHECK(snapshot.phase == UpdatePhase::kAvailable && snapshot.available == (Version{0, 3, 0}));
    CHECK(snapshot.kit_needed && snapshot.notes == "New things." && snapshot.user_check);
    CHECK(snapshot.notes_url == "https://github.com/o/r/releases/tag/v0.3.0");  // no html_url: built from the tag
    CHECK(snapshot.download_bytes == fixture.server.files[kDownload + "playden-ps5-0.3.0.zip"].size() +
                                         fixture.server.files[kDownload + "playden-kit-h-r9.zip"].size());
    service.Install();
    Wait(service);
    snapshot = service.Snapshot();
    CHECK(snapshot.phase == UpdatePhase::kStaged);
    const std::string kits = fixture.root + "/playden/kits";
    // The new kit waits next to the installed one until the restart.
    CHECK(Read(kits + "/.next-kitid-r9/manifest.json") == kKitSha && !Exists(kits + "/.staging-kitid-r9"));
    CHECK(!Exists(kits + "/.next-kitid-r9/LICENSES"));
    CHECK(Read(kits + "/kitid/manifest.json") == std::string(64, 'b'));
    CHECK(Read(fixture.root + "/playden/self-update/stage/0.3.0/eboot.bin") == "eboot 0.3.0");
    CHECK(Read(fixture.root + "/playden/self-update/state.json").find("\"staged\"") != std::string::npos);
    CHECK(ListFolder(fixture.root + "/playden/self-update/downloads").empty());  // zips removed after unpacking
    // Nothing is swapped without a restart request.
    CHECK(!service.ApplyAtQuit());
    service.RequestRestart();
    CHECK(service.Snapshot().restart_requested);
    service.Shutdown();
    CHECK(service.ApplyAtQuit());
    const std::string app = fixture.root + "/homebrew/FAKE77031";
    CHECK(Read(app + "/eboot.bin") == "eboot 0.3.0" && Read(app + "/assets/a.txt") == "asset 0.3.0");
    CHECK(Read(app + "/playden-data/session") == "session");
    CHECK(Read(fixture.root + "/user/appmeta/FAKE77031/param.json").find("00.003.000") != std::string::npos);
    CHECK(Read(fixture.root + "/user/app/FAKE77031/sce_sys/param.json").find("00.003.000") != std::string::npos);
    CHECK(Read(fixture.root + "/playden/self-update/backup/0.2.0-1000/eboot.bin") == "eboot 0.2.0");
    // The kit was swapped in the same transaction; the old one is kept for going back.
    CHECK(Read(kits + "/kitid/manifest.json") == kKitSha && !Exists(kits + "/.next-kitid-r9"));
    CHECK(Read(kits + "/.previous-kitid-1000/manifest.json") == std::string(64, 'b'));

    // The new version starts: the update is confirmed and old files are cleaned up.
    std::vector<std::thread::id> raised;
    std::mutex raised_mutex;
    const auto record_root = [&](UpdateConfig config) {
      config.as_root = [&](const std::function<bool()>& work) {
        {
          std::lock_guard<std::mutex> lock(raised_mutex);
          raised.push_back(std::this_thread::get_id());
        }
        return work();
      };
      return config;
    };
    const auto root_only_on_this_thread = [&] {
      std::lock_guard<std::mutex> lock(raised_mutex);
      for (const auto& id : raised) CHECK(id == std::this_thread::get_id());
    };
    UpdateService next(record_root(fixture.Config({0, 3, 0})));
    next.Start();
    // Root is process-wide: it is raised only while Start runs, before the runtime and the UI start,
    // and the confirm is complete by then, so a start closed early does not confirm again.
    CHECK(!raised.empty());
    root_only_on_this_thread();
    CHECK(!Exists(fixture.root + "/playden/self-update/state.json"));
    next.Check(false);  // queued behind the cleanup, not dropped
    CHECK(next.Snapshot().finished_message == "Playden was updated to 0.3.0." && !next.Snapshot().finished_failed);
    // What's new: the release notes travel with the update and stay for Settings.
    CHECK(next.Snapshot().whats_new && next.Snapshot().current_notes == "New things." &&
          next.Snapshot().current_notes_url == "https://github.com/o/r/releases/tag/v0.3.0");
    Wait(next);
    root_only_on_this_thread();  // the worker's cleanup never raised root
    CHECK(Exists(fixture.root + "/playden/self-update/backup/0.2.0-1000"));  // newest backup kept
    CHECK(!Exists(fixture.root + "/playden/self-update/state.json"));
    CHECK(Exists(kits + "/kitid") && Exists(kits + "/.previous-kitid-1000") && !Exists(kits + "/.previous-kitid-1"));
    CHECK(next.Snapshot().phase == UpdatePhase::kUpToDate);
    next.AcknowledgeFinished();
    CHECK(next.Snapshot().finished_message.empty() && !next.Snapshot().whats_new);
    {
      UpdateService later(fixture.Config({0, 3, 0}));  // a later start: notes still there, no what's new
      later.Start();
      CHECK(!later.Snapshot().whats_new && later.Snapshot().current_notes == "New things.");
    }

    // Go back: the previous app and kit return at quit, and the old version says so.
    CHECK(next.Snapshot().previous == "0.2.0");
    next.RequestRollback();
    CHECK(next.Snapshot().rollback_requested);
    next.Shutdown();
    CHECK(next.ApplyAtQuit());
    CHECK(Read(app + "/eboot.bin") == "eboot 0.2.0" && Read(app + "/assets/a.txt") == "asset 0.2.0");
    CHECK(Read(app + "/playden-data/session") == "session");
    CHECK(Read(kits + "/kitid/manifest.json") == std::string(64, 'b') && !Exists(kits + "/.previous-kitid-1000"));
    CHECK(ListFolder(fixture.root + "/playden/self-update/backup").empty());
    CHECK(Read(fixture.root + "/user/appmeta/FAKE77031/param.json") == "old param");
    UpdateService back(record_root(fixture.Config({0, 2, 0})));
    back.Start();
    CHECK(back.Snapshot().finished_message == "Playden went back to version 0.2.0." && back.Snapshot().previous.empty());
    CHECK(!Exists(fixture.root + "/playden/self-update/state.json"));
    Wait(back);
    root_only_on_this_thread();
    CHECK(!Exists(fixture.root + "/playden/self-update/replaced"));
    for (const auto& name : ListFolder(kits)) CHECK(name.rfind(".replaced-", 0) != 0);  // removed on this start
    // The version gone back from is still found, but quietly.
    back.Check(false);
    Wait(back);
    CHECK(back.Snapshot().phase == UpdatePhase::kAvailable && back.Snapshot().quiet);
    back.Shutdown();

    // A start closed before its cleanup finished: the next start, with no state left, cleans up
    // without confirming again; an empty kit folder left by going back goes during Start.
    const std::string work = fixture.root + "/playden/self-update";
    Put(work + "/stage/eboot.bin", "stage");
    Put(work + "/downloads/playden-ps5-0.3.0.zip", "zip");
    Put(kits + "/.replaced-kitid-5/runtime/wine.bin", "files");
    CHECK(MakeFolders(kits + "/.replaced-kitid-6"));
    UpdateService resumed(record_root(fixture.Config({0, 2, 0})));
    resumed.Start();
    CHECK(!Exists(kits + "/.replaced-kitid-6") && resumed.Snapshot().finished_message.empty());
    Wait(resumed);
    root_only_on_this_thread();
    CHECK(!Exists(work + "/stage") && !Exists(work + "/downloads") && !Exists(kits + "/.replaced-kitid-5"));
    CHECK(Exists(kits + "/kitid/manifest.json"));
    RemoveTree(fixture.root);
  }
  // A kit renamed between versions (0.6.0 keeps it at hl2-fw1360-dev1, 0.7.0 at playden-kit; here oldkit and kitid).
  // The old Playden's update (ReadyKit, StageKit and ApplyAtQuit are 0.6.0's) puts the new kit in next to its own
  // and leaves that one alone. The new Playden retires it as the previous kit: going back puts it back in its own
  // folder (the new kit stays), and the next update removes it.
  {
    Fixture fixture;
    fixture.root = zip_fixture::TempFolder("service-renamed-kit");
    Installed(fixture.root);
    const std::string kits = fixture.root + "/playden/kits";
    const std::string app = fixture.root + "/homebrew/FAKE77031";
    CHECK(rename((kits + "/kitid").c_str(), (kits + "/oldkit").c_str()) == 0);
    RemoveTree(kits + "/.previous-kitid-1");
    const auto previous_kits = [&] {
      std::vector<std::string> names;
      for (const auto& name : ListFolder(kits))
        if (name.rfind(".previous-", 0) == 0) names.push_back(name);
      return names;
    };
    const auto renamed = [&](Version current) {
      auto config = fixture.Config(current);
      config.kit_id = "kitid";
      config.kit_manifest_sha256 = kKitSha;
      config.renamed_kit_ids = {"oldkit"};
      return config;
    };
    Publish(fixture.server, "v0.3.0");
    {
      UpdateService old(fixture.Config({0, 2, 0}));
      old.Start();
      old.Check(true);
      Wait(old);
      CHECK(old.Snapshot().phase == UpdatePhase::kAvailable && old.Snapshot().kit_needed);
      old.Install();
      Wait(old);
      CHECK(old.Snapshot().phase == UpdatePhase::kStaged && Read(kits + "/.next-kitid-r9/manifest.json") == kKitSha);
      old.RequestRestart();
      old.Shutdown();
      CHECK(old.ApplyAtQuit());
    }
    CHECK(Read(app + "/eboot.bin") == "eboot 0.3.0" && Read(kits + "/kitid/manifest.json") == kKitSha);
    CHECK(Read(kits + "/oldkit/manifest.json") == std::string(64, 'b') && previous_kits().empty());
    // A version that has no kit under its own id yet keeps the old folder.
    {
      CHECK(rename((kits + "/kitid").c_str(), (kits + "/.aside").c_str()) == 0);
      UpdateService waiting(renamed({0, 3, 0}));
      waiting.Start();
      Wait(waiting);
      CHECK(Exists(kits + "/oldkit") && previous_kits().empty());
      CHECK(rename((kits + "/.aside").c_str(), (kits + "/kitid").c_str()) == 0);
      // The update is confirmed again below: the state was removed by this start.
      CHECK(!Exists(fixture.root + "/playden/self-update/state.json"));
    }
    UpdateService next(renamed({0, 3, 0}));
    next.Start();
    CHECK(!Exists(kits + "/oldkit") && Read(kits + "/.previous-oldkit-1000/manifest.json") == std::string(64, 'b'));
    CHECK(Read(kits + "/kitid/manifest.json") == kKitSha);
    Wait(next);
    CHECK(next.Snapshot().previous == "0.2.0");
    next.RequestRollback();
    next.Shutdown();
    CHECK(next.ApplyAtQuit());
    CHECK(Read(app + "/eboot.bin") == "eboot 0.2.0" && Read(kits + "/oldkit/manifest.json") == std::string(64, 'b'));
    CHECK(Read(kits + "/kitid/manifest.json") == kKitSha && previous_kits().empty());
    {
      UpdateService back(fixture.Config({0, 2, 0}));
      back.Start();
      CHECK(back.Snapshot().finished_message == "Playden went back to version 0.2.0.");
      Wait(back);
      CHECK(Exists(kits + "/oldkit/runtime/wine.bin") && Exists(kits + "/kitid/runtime/wine.bin"));
      // Updating again finds the new kit in place: only the app is downloaded.
      back.Check(true);
      Wait(back);
      CHECK(back.Snapshot().phase == UpdatePhase::kAvailable && !back.Snapshot().kit_needed);
      back.Install();
      Wait(back);
      back.RequestRestart();
      back.Shutdown();
      CHECK(back.ApplyAtQuit());
    }
    {
      UpdateService again(renamed({0, 3, 0}));
      again.Start();
      Wait(again);
      CHECK(!Exists(kits + "/oldkit") && previous_kits() == std::vector<std::string>{".previous-oldkit-1000"});
      // The next update removes the retired kit with the other previous ones.
      Publish(fixture.server, "v0.4.0", "0.2.0", "00.004.000");
      again.Check(true);
      Wait(again);
      CHECK(again.Snapshot().phase == UpdatePhase::kAvailable && !again.Snapshot().kit_needed);
      again.Install();
      Wait(again);
      again.RequestRestart();
      again.Shutdown();
      CHECK(again.ApplyAtQuit());
      CHECK(Read(app + "/eboot.bin") == "eboot 0.4.0" && previous_kits().empty() && !Exists(kits + "/oldkit"));
      CHECK(Read(kits + "/kitid/manifest.json") == kKitSha);
    }
    RemoveTree(fixture.root);
  }
  // Going back to a version whose kit was retired under its earlier id, when that version's update also replaced a kit
  // under the new id (the same backup time): the retired kit is the one the older app reads, so it is restored.
  {
    Fixture fixture;
    fixture.root = zip_fixture::TempFolder("service-renamed-same-time");
    Installed(fixture.root);
    const std::string kits = fixture.root + "/playden/kits";
    RemoveTree(kits + "/.previous-kitid-1");
    Put(fixture.root + "/playden/self-update/backup/0.2.0-1000/eboot.bin", "eboot 0.2.0 backup");
    Put(fixture.root + "/playden/self-update/backup/0.2.0-1000/sce_sys/param.json", "old param");
    // The earlier id is listed first, as it is on the console.
    Put(kits + "/.previous-kitid-1000/manifest.json", std::string(64, 'e'));
    Put(kits + "/.previous-hl2kit-1000/manifest.json", std::string(64, 'f'));
    auto config = fixture.Config({0, 3, 0});
    config.kit_id = "kitid";
    config.renamed_kit_ids = {"hl2kit"};
    UpdateService service(config);
    service.Start();
    Wait(service);
    CHECK(service.Snapshot().previous == "0.2.0");
    service.RequestRollback();
    service.Shutdown();
    CHECK(service.ApplyAtQuit());
    CHECK(Read(fixture.root + "/homebrew/FAKE77031/eboot.bin") == "eboot 0.2.0 backup");
    CHECK(Read(kits + "/hl2kit/manifest.json") == std::string(64, 'f'));
    CHECK(Read(kits + "/kitid/manifest.json") == std::string(64, 'b') && Exists(kits + "/.previous-kitid-1000"));
    RemoveTree(fixture.root);
  }
  // A staged update survives a restart without a swap, and a swap that did not take is reported.
  {
    Fixture fixture;
    fixture.root = zip_fixture::TempFolder("service-staged");
    Installed(fixture.root);
    Publish(fixture.server, "v0.3.0");
    {
      UpdateService service(fixture.Config({0, 2, 0}));
      service.Install();  // checks first when nothing is known
      Wait(service);
      CHECK(service.Snapshot().phase == UpdatePhase::kStaged);
    }
    UpdateService again(fixture.Config({0, 2, 0}));
    again.Start();
    CHECK(again.Snapshot().phase == UpdatePhase::kStaged && again.Snapshot().available == (Version{0, 3, 0}));
    again.Check(false);  // ignored while staged
    Wait(again);
    CHECK(again.Snapshot().phase == UpdatePhase::kStaged);
    again.RequestRestart();
    again.Shutdown();
    CHECK(again.ApplyAtQuit());
    // The old version starts again (the swap did not take): reported, and the old app is put back.
    UpdateService old(fixture.Config({0, 2, 0}));
    old.Start();
    CHECK(old.Snapshot().finished_failed);
    CHECK(old.Snapshot().finished_message.find("did not finish") != std::string::npos);
    const std::string app = fixture.root + "/homebrew/FAKE77031";
    CHECK(Read(app + "/eboot.bin") == "eboot 0.2.0" && Read(app + "/assets/a.txt") == "asset 0.2.0");
    CHECK(!Exists(fixture.root + "/playden/self-update/backup/0.2.0-1000") && old.Snapshot().previous.empty());
    // ...and the old kit is back in place for the old app; the new one waits for a retry.
    CHECK(Read(fixture.root + "/playden/kits/kitid/manifest.json") == std::string(64, 'b'));
    CHECK(!Exists(fixture.root + "/playden/kits/.previous-kitid-1000"));
    CHECK(Read(fixture.root + "/playden/kits/.next-kitid-r9/manifest.json") == kKitSha);
    RemoveTree(fixture.root);
  }
  // The process died part way through the swap at quit (the executable moves last, so the old one
  // starts): the next start puts the replaced entries and the old kit back.
  {
    Fixture fixture;
    fixture.root = zip_fixture::TempFolder("service-crash");
    const std::string app = fixture.root + "/homebrew/FAKE77031", work = fixture.root + "/playden/self-update";
    const std::string kits = fixture.root + "/playden/kits";
    Put(app + "/eboot.bin", "eboot 0.2.0");                    // not moved yet
    Put(app + "/assets/a.txt", "asset 0.3.0");                 // moved in
    Put(app + "/sce_sys/param.json", "new param");
    Put(app + "/playden-data/session", "session");
    Put(work + "/backup/0.2.0-1000/assets/a.txt", "asset 0.2.0");
    Put(work + "/backup/0.2.0-1000/sce_sys/param.json", "old param");
    Put(work + "/stage/0.3.0/eboot.bin", "eboot 0.3.0");
    Put(kits + "/kitid/manifest.json", kKitSha);               // the new kit went in first
    Put(kits + "/kitid/runtime/wine.bin", "new");
    Put(kits + "/.previous-kitid-1000/manifest.json", std::string(64, 'b'));
    Put(work + "/state.json",
        R"({"phase":"applied","version":"0.3.0","from":"0.2.0","backup":")" + work + R"(/backup/0.2.0-1000","stage":")" +
            work + R"(/stage/0.3.0","kit_id":"kitid","previous_kit":")" + kits + R"(/.previous-kitid-1000","next_kit":")" +
            kits + R"(/.next-kitid-r9"})");
    UpdateService old(fixture.Config({0, 2, 0}));
    old.Start();
    CHECK(old.Snapshot().finished_failed);
    CHECK(Read(app + "/eboot.bin") == "eboot 0.2.0" && Read(app + "/assets/a.txt") == "asset 0.2.0" &&
          Read(app + "/sce_sys/param.json") == "old param" && Read(app + "/playden-data/session") == "session");
    CHECK(Read(work + "/stage/0.3.0/assets/a.txt") == "asset 0.3.0" && !Exists(work + "/backup/0.2.0-1000"));
    CHECK(Read(kits + "/kitid/manifest.json") == std::string(64, 'b') && !Exists(kits + "/.previous-kitid-1000"));
    CHECK(Read(kits + "/.next-kitid-r9/manifest.json") == kKitSha && !Exists(work + "/state.json"));
    RemoveTree(fixture.root);
  }
  // Going back after two updates, the second without a new kit: the app returns, the kit stays
  // (it is the one that app needs), and the kit replaced by the first update is gone.
  {
    Fixture fixture;
    fixture.root = zip_fixture::TempFolder("service-twice");
    Installed(fixture.root);
    const std::string kits = fixture.root + "/playden/kits", app = fixture.root + "/homebrew/FAKE77031";
    Publish(fixture.server, "v0.3.0");
    {
      UpdateService first(fixture.Config({0, 2, 0}));
      first.Install();
      Wait(first);
      first.RequestRestart();
      first.Shutdown();
      CHECK(first.ApplyAtQuit() && Read(kits + "/kitid/manifest.json") == kKitSha);
    }
    Publish(fixture.server, "v0.4.0", "0.3.0", "00.004.000", false);
    auto config = fixture.Config({0, 3, 0});
    config.now = [] { return std::int64_t{2000}; };
    {
      UpdateService second(config);
      second.Start();
      Wait(second);
      second.Install();
      Wait(second);
      CHECK(second.Snapshot().phase == UpdatePhase::kStaged);
      second.RequestRestart();
      second.Shutdown();
      CHECK(second.ApplyAtQuit() && Read(app + "/eboot.bin") == "eboot 0.4.0");
    }
    for (const auto& name : ListFolder(kits)) CHECK(name.rfind(".previous-", 0) != 0);  // K1 went with its backup
    auto latest = fixture.Config({0, 4, 0});
    latest.now = [] { return std::int64_t{3000}; };
    UpdateService current(latest);
    current.Start();
    Wait(current);
    CHECK(current.Snapshot().previous == "0.3.0");
    current.RequestRollback();
    current.Shutdown();
    CHECK(current.ApplyAtQuit());
    CHECK(Read(app + "/eboot.bin") == "eboot 0.3.0" && Read(kits + "/kitid/manifest.json") == kKitSha);
    RemoveTree(fixture.root);
  }
  // A release without GitHub's checksums is not offered.
  {
    Fixture fixture;
    fixture.root = zip_fixture::TempFolder("service-nodigest");
    Installed(fixture.root);
    Publish(fixture.server, "v0.3.0", "0.2.0", "00.003.000", true, false);
    UpdateService service(fixture.Config({0, 2, 0}));
    service.Check(true);
    Wait(service);
    CHECK(service.Snapshot().phase == UpdatePhase::kFailed && service.Snapshot().error.find("checksums") != std::string::npos);
    RemoveTree(fixture.root);
  }
  // A release whose kit is already installed downloads only the app, even without a kit zip.
  {
    Fixture fixture;
    fixture.root = zip_fixture::TempFolder("service-kit");
    Installed(fixture.root);
    Put(fixture.root + "/playden/kits/kitid/manifest.json", kKitSha);
    Put(fixture.root + "/playden/kits/kitid/runtime/wine.bin", "w");
    Publish(fixture.server, "v0.3.0", "0.2.0", "00.003.000", false);
    UpdateService service(fixture.Config({0, 2, 0}));
    service.Check(false);
    Wait(service);
    CHECK(service.Snapshot().phase == UpdatePhase::kAvailable && !service.Snapshot().kit_needed);
    service.Install();
    Wait(service);
    CHECK(service.Snapshot().phase == UpdatePhase::kStaged && fixture.server.kit_downloads == 0);
    service.RequestRestart();
    service.Shutdown();
    CHECK(service.ApplyAtQuit());
    CHECK(Read(fixture.root + "/playden/kits/kitid/runtime/wine.bin") == "w");  // kit untouched
    CHECK(Read(fixture.root + "/homebrew/FAKE77031/eboot.bin") == "eboot 0.3.0");
    RemoveTree(fixture.root);
  }
  // Without the kit installed, a release that left its kit zip out takes it from an older release; never from a
  // newer one, and with no release that has it, the update cannot be installed.
  {
    Fixture fixture;
    fixture.root = zip_fixture::TempFolder("service-nokit");
    Installed(fixture.root);
    // Each release's kit zip at its own address: only the older one's can be downloaded.
    const auto moved = [](std::string release, const std::string& folder) {
      const std::string from = kDownload + "playden-kit-h-r9.zip";
      release.replace(release.find(from), from.size(), kDownload + folder + "/playden-kit-h-r9.zip");
      return release;
    };
    Publish(fixture.server, "v0.2.5", "0.2.0", "00.002.005");
    const std::string older = moved(fixture.server.latest, "older");
    fixture.server.files[kDownload + "older/playden-kit-h-r9.zip"] = fixture.server.files[kDownload + "playden-kit-h-r9.zip"];
    Publish(fixture.server, "v0.4.0", "0.2.0", "00.004.000");
    const std::string newer = moved(fixture.server.latest, "newer");
    Publish(fixture.server, "v0.3.0", "0.2.0", "00.003.000", false);
    fixture.server.releases = "[" + newer + "," + fixture.server.latest + "]";
    {
      UpdateService service(fixture.Config({0, 2, 0}));
      service.Check(false);
      Wait(service);
      CHECK(service.Snapshot().phase == UpdatePhase::kFailed);
      service.Shutdown();
    }
    fixture.server.releases = "[" + newer + "," + fixture.server.latest + "," + older + "]";
    UpdateService service(fixture.Config({0, 2, 0}));
    service.Check(false);
    Wait(service);
    auto snapshot = service.Snapshot();
    CHECK(snapshot.phase == UpdatePhase::kAvailable && snapshot.available == (Version{0, 3, 0}) && snapshot.kit_needed);
    CHECK(snapshot.download_bytes == fixture.server.files[kDownload + "playden-ps5-0.3.0.zip"].size() +
                                         fixture.server.files[kDownload + "playden-kit-h-r9.zip"].size());
    service.Install();
    Wait(service);
    CHECK(service.Snapshot().phase == UpdatePhase::kStaged && fixture.server.kit_downloads > 0);
    CHECK(Read(fixture.root + "/playden/kits/.next-kitid-r9/manifest.json") == kKitSha);
    service.Shutdown();
    RemoveTree(fixture.root);
  }
  // Up to date, too old to update, a wrong app inside the zip, and too little free space.
  {
    Fixture fixture;
    fixture.root = zip_fixture::TempFolder("service-edges");
    Installed(fixture.root);
    Publish(fixture.server, "v0.2.0", "0.2.0", "00.002.000");
    UpdateService current(fixture.Config({0, 2, 0}));
    current.Check(true);
    Wait(current);
    CHECK(current.Snapshot().phase == UpdatePhase::kUpToDate);

    Publish(fixture.server, "v0.3.0", "0.2.5");
    UpdateService old(fixture.Config({0, 2, 0}));
    old.Check(true);
    Wait(old);
    CHECK(old.Snapshot().phase == UpdatePhase::kFailed && old.Snapshot().error.find("by hand") != std::string::npos);

    Publish(fixture.server, "v0.3.0", "0.2.0", "00.002.009");  // the zip holds another version
    UpdateService mismatch(fixture.Config({0, 2, 0}));
    mismatch.Install();
    Wait(mismatch);
    CHECK(mismatch.Snapshot().phase == UpdatePhase::kFailed);
    CHECK(!Exists(fixture.root + "/playden/self-update/stage/0.3.0"));

    Publish(fixture.server, "v0.3.0");
    fixture.free = 10;
    UpdateService full(fixture.Config({0, 2, 0}));
    full.Install();
    Wait(full);
    CHECK(full.Snapshot().phase == UpdatePhase::kFailed && full.Snapshot().error.find("free space") != std::string::npos);
    RemoveTree(fixture.root);
  }
  // A local release folder (console testing) replaces GitHub: release, manifest and zips.
  {
    Fixture fixture;
    fixture.root = zip_fixture::TempFolder("service-local");
    Installed(fixture.root);
    Publish(fixture.server, "v0.3.0");
    const std::string local = fixture.root + "/local";
    Put(local + "/release.json", fixture.server.latest);
    for (const auto& [url, body] : fixture.server.files) Put(local + "/" + url.substr(kDownload.size()), body);
    auto config = fixture.Config({0, 2, 0});
    config.local_release_dir = local;
    config.fetch = [](const HttpRequest&, const std::atomic<bool>&) { return HttpResponse{}; };  // no network at all
    UpdateService service(config);
    service.Install();
    Wait(service);
    CHECK(service.Snapshot().phase == UpdatePhase::kStaged);
    CHECK(Exists(local + "/playden-ps5-0.3.0.zip") && Exists(local + "/playden-kit-h-r9.zip"));  // left in place
    RemoveTree(fixture.root);
  }
  // Quitting during a stalled request does not wait for the network timeout.
  {
    Fixture fixture;
    fixture.root = zip_fixture::TempFolder("service-stall");
    auto config = fixture.Config({0, 2, 0});
    config.fetch = [](const HttpRequest&, const std::atomic<bool>& cancel) {
      for (int i = 0; i < 3000 && !cancel; ++i) usleep(1000);  // a server that never answers (3 s)
      HttpResponse response;
      response.error = playden::steam::HttpError::kCanceled;
      return response;
    };
    config.wait_before_retry = nullptr;  // the real, cancellable wait
    UpdateService service(config);
    service.Check(true);
    usleep(50000);
    const auto start = std::chrono::steady_clock::now();
    service.Shutdown();
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(500));
    RemoveTree(fixture.root);
  }
  // Without a backup there is nothing to go back to.
  {
    Fixture fixture;
    fixture.root = zip_fixture::TempFolder("service-noback");
    Installed(fixture.root);
    UpdateService service(fixture.Config({0, 2, 0}));
    service.Start();
    CHECK(service.Snapshot().previous.empty());
    service.RequestRollback();
    CHECK(!service.Snapshot().rollback_requested && !service.ApplyAtQuit());
    RemoveTree(fixture.root);
  }
  // Going back never crosses 0.6.0: older versions can't read the data it moved out of the app.
  {
    Fixture fixture;
    fixture.root = zip_fixture::TempFolder("service-layout");
    Installed(fixture.root);
    Put(fixture.root + "/playden/self-update/backup/0.5.0-900/eboot.bin", "eboot 0.5.0");
    UpdateService service(fixture.Config({0, 6, 0}));
    service.Start();
    CHECK(service.Snapshot().previous.empty());
    service.RequestRollback();
    CHECK(!service.Snapshot().rollback_requested && !service.ApplyAtQuit());
    CHECK(Exists(fixture.root + "/playden/self-update/backup/0.5.0-900/eboot.bin"));
    RemoveTree(fixture.root);
  }
  // A legacy build reporting below 0.6.0 still refuses going back when the move record exists.
  {
    Fixture fixture;
    fixture.root = zip_fixture::TempFolder("service-journal");
    Installed(fixture.root);
    Put(fixture.root + "/playden/self-update/backup/0.4.2-900/eboot.bin", "eboot 0.4.2");
    const std::string journal = fixture.root + "/playden/runtime-state/layout-0.6.json";
    Put(journal, "{\"step\":\"done\"}");
    auto config = fixture.Config({0, 5, 0});
    config.layout_journal = journal;
    UpdateService refused(config);
    refused.Start();
    CHECK(refused.Snapshot().previous.empty());
    refused.RequestRollback();
    CHECK(!refused.Snapshot().rollback_requested && !refused.ApplyAtQuit());
    CHECK(Exists(fixture.root + "/playden/self-update/backup/0.4.2-900/eboot.bin"));
    CHECK(Read(fixture.root + "/homebrew/FAKE77031/eboot.bin") == "eboot 0.2.0");
    // Without the record (never moved, or reverted) it is offered.
    CHECK(unlink(journal.c_str()) == 0);
    UpdateService offered(config);
    offered.Start();
    CHECK(offered.Snapshot().previous == "0.4.2");
    offered.RequestRollback();
    CHECK(offered.Snapshot().rollback_requested);
    RemoveTree(fixture.root);
  }
  // GitHub out of reach.
  {
    Fixture fixture;
    fixture.root = zip_fixture::TempFolder("service-offline");
    UpdateService service(fixture.Config({0, 2, 0}));
    service.Check(true);
    Wait(service);
    CHECK(service.Snapshot().phase == UpdatePhase::kFailed);
    RemoveTree(fixture.root);
  }
  std::puts("ok");
  return 0;
}
