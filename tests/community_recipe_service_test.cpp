// community_recipe_service_test.cpp - Fetching, checking and keeping community recipes with a fake
// network. Built twice: as a release build (community_recipe_service_test: the configured endpoints
// are ignored, GitHub and api.playden.club only, no auth) and with dev endpoints
// (community_recipe_service_dev_test: a local server over plain http with an auth header that only
// the recipes server gets).
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "community/recipe_service.h"

#include <stdlib.h>
#include <strings.h>
#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <future>
#include <thread>
#include <cstdio>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "miniz.h"

using namespace playden;
using namespace playden::community;
namespace fs = std::filesystem;

namespace {

int failures = 0;
#define CHECK(cond)                                                                 \
  do {                                                                              \
    if (!(cond)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                                   \
    }                                                                               \
  } while (0)

std::string Slurp(const std::string& path) {
  std::string text;
  if (std::FILE* file = std::fopen(path.c_str(), "rb")) {
    char buffer[4096];
    std::size_t count = 0;
    while ((count = std::fread(buffer, 1, sizeof buffer, file)) > 0) text.append(buffer, count);
    std::fclose(file);
  }
  return text;
}

std::string Replace(std::string text, const std::string& from, const std::string& to) {
  for (auto at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size()))
    text.replace(at, from.size(), to);
  return text;
}

// ---- zips, built in memory (stored or deflated entries) ----

struct Entry {
  std::string name;
  std::string data;
  bool deflate = false;
};

void Put16(std::string& out, unsigned value) {
  out += static_cast<char>(value & 0xff);
  out += static_cast<char>((value >> 8) & 0xff);
}
void Put32(std::string& out, std::uint32_t value) {
  Put16(out, value & 0xffff);
  Put16(out, value >> 16);
}

std::string Zip(const std::vector<Entry>& entries) {
  std::string out, central;
  for (const auto& entry : entries) {
    const auto crc = static_cast<std::uint32_t>(
        mz_crc32(MZ_CRC32_INIT, reinterpret_cast<const unsigned char*>(entry.data.data()), entry.data.size()));
    std::string stored = entry.data;
    unsigned method = 0;
    if (entry.deflate) {
      std::size_t size = 0;
      void* packed = tdefl_compress_mem_to_heap(entry.data.data(), entry.data.size(), &size, TDEFL_DEFAULT_MAX_PROBES);
      stored.assign(static_cast<const char*>(packed), size);
      mz_free(packed);
      method = 8;
    }
    const auto offset = static_cast<std::uint32_t>(out.size());
    Put32(out, 0x04034b50); Put16(out, 20); Put16(out, 0); Put16(out, method); Put16(out, 0); Put16(out, 0x21);
    Put32(out, crc); Put32(out, stored.size()); Put32(out, entry.data.size());
    Put16(out, entry.name.size()); Put16(out, 0);
    out += entry.name;
    out += stored;
    Put32(central, 0x02014b50); Put16(central, (3u << 8) | 20); Put16(central, 20);
    Put16(central, 0); Put16(central, method); Put16(central, 0); Put16(central, 0x21);
    Put32(central, crc); Put32(central, stored.size()); Put32(central, entry.data.size());
    Put16(central, entry.name.size()); Put16(central, 0); Put16(central, 0); Put16(central, 0); Put16(central, 0);
    Put32(central, static_cast<std::uint32_t>(entry.name.back() == '/' ? 040755u : 0100644u) << 16);
    Put32(central, offset);
    central += entry.name;
  }
  const auto central_offset = static_cast<std::uint32_t>(out.size());
  out += central;
  Put32(out, 0x06054b50); Put16(out, 0); Put16(out, 0); Put16(out, entries.size()); Put16(out, entries.size());
  Put32(out, central.size()); Put32(out, central_offset); Put16(out, 0);
  return out;
}

// ---- the fake network ----

struct Route {
  long status = 200;
  std::string body;
  std::vector<std::pair<std::string, std::string>> headers;
};

struct Network {
  std::mutex mutex;
  std::map<std::string, Route> routes;
  std::vector<steam::HttpRequest> seen;
  bool offline = false;
  // Holds a request to `hold` until released (two refreshes at once).
  std::string hold;
  bool held = false, release = false;
  std::condition_variable changed;

  steam::HttpResponse Fetch(const steam::HttpRequest& request, const std::atomic<bool>&) {
    std::unique_lock<std::mutex> lock(mutex);
    seen.push_back(request);
    if (!hold.empty() && request.url == hold) {
      held = true;
      changed.notify_all();
      changed.wait(lock, [&] { return release; });
    }
    steam::HttpResponse response;
    if (offline) {
      response.error = steam::HttpError::kConnectionFailed;
      return response;
    }
    // As HttpClient: plain http only for a request that asks for it in a dev build.
    if (request.url.rfind("http://", 0) == 0 && !(request.allow_plain_http && DevEndpointsCompiledIn())) {
      response.error = steam::HttpError::kInvalidRequest;
      return response;
    }
    const auto found = routes.find(request.url);
    if (found == routes.end()) {
      response.status_code = 404;
      return response;
    }
    response.status_code = found->second.status;
    response.headers = found->second.headers;
    if (found->second.body.size() > request.max_response_bytes) {
      response.error = steam::HttpError::kResponseTooLarge;
      return response;
    }
    response.body.assign(found->second.body.begin(), found->second.body.end());
    return response;
  }
  int Count(const std::string& url) {
    std::lock_guard<std::mutex> lock(mutex);
    int count = 0;
    for (const auto& request : seen) count += request.url == url;
    return count;
  }
  void Clear() {
    std::lock_guard<std::mutex> lock(mutex);
    seen.clear();
  }
};

bool HasHeader(const steam::HttpRequest& request, const char* name) {
  for (const auto& [key, value] : request.headers)
    if (strcasecmp(key.c_str(), name) == 0) return true;
  return false;
}

// Where things live: GitHub and api.playden.club in a release build, a local server in a dev build.
struct Site {
  Endpoints endpoints;
  std::string latest, assets, summary;
};

Site MakeSite() {
  Site site;
  // What a dev file would configure. A release build ignores it.
  site.endpoints.releases_latest_url = "http://gitea.test:3000/api/v1/repos/o/r/releases/latest";
  site.endpoints.recipes_auth_header = "token dev-secret";
  site.endpoints.api_base_url = "http://api.test:8787";
  site.endpoints.allow_plain_http = true;
  if (DevEndpointsCompiledIn()) {
    site.latest = site.endpoints.releases_latest_url;
    site.assets = "http://gitea.test:3000/o/r/releases/download/";
    site.summary = "http://api.test:8787/v1/summary.json";
  } else {
    site.latest = kRecipesReleasesLatestUrl;
    site.assets = "https://github.com/leolobato/playden-ps5-recipes/releases/download/";
    site.summary = "https://api.playden.club/v1/summary.json";
  }
  return site;
}

struct Recipe {
  std::string path;  // in the zip
  std::string id;
  std::string bytes;
  std::string listed_revision;  // empty: its own sha256; "-": not in the index
};

struct Published {
  std::string tag, index, zip;
};

// Publishes a release: index.json and recipes.zip behind a redirect to githubusercontent (as
// GitHub does), the release JSON at the latest URL.
Published Publish(Network& network, const Site& site, const std::string& tag, const std::vector<Recipe>& recipes,
                  std::vector<Entry> extra = {}, std::uint64_t zip_size_override = 0) {
  std::vector<Entry> entries;
  for (const auto& recipe : recipes) entries.push_back({recipe.path, recipe.bytes, entries.size() % 2 == 0});
  for (auto& entry : extra) entries.push_back(std::move(entry));
  const std::string zip = Zip(entries);
  std::map<std::string, std::string> apps;
  for (const auto& recipe : recipes) {
    if (recipe.listed_revision == "-") continue;
    const std::string app = recipe.id.substr(10, recipe.id.find('-', 10) - 10);
    auto& list = apps[app];
    if (!list.empty()) list += ",";
    list += R"({"id":")" + recipe.id + R"(","title":"t","status":"works","author":"a","contributors":[],)"
            R"("tested_kit":"0.12","min_kit":"","tested_firmware":"","tested_date":"","revision":")" +
            (recipe.listed_revision.empty() ? Sha256Hex(recipe.bytes) : recipe.listed_revision) + R"("})";
  }
  std::string apps_json;
  for (const auto& [app, list] : apps) apps_json += (apps_json.empty() ? "" : ",") + ("\"" + app + "\":[" + list + "]");
  const std::string index = R"({"format_version":1,"generated_at":"2026-10-22T00:00:00Z","zip_sha256":")" +
                            Sha256Hex(zip) + R"(","apps":{)" + apps_json + "}}";
  std::lock_guard<std::mutex> lock(network.mutex);
  for (const auto& [name, bytes] : {std::pair<std::string, const std::string*>{"index.json", &index}, {"recipes.zip", &zip}}) {
    const std::string url = site.assets + tag + "/" + name;
    const std::string cdn = "https://objects.githubusercontent.com/" + tag + "/" + name;
    network.routes[url] = Route{302, {}, {{"Location", cdn}}};
    network.routes[cdn] = Route{200, *bytes, {}};
  }
  const std::uint64_t zip_size = zip_size_override ? zip_size_override : zip.size();
  network.routes[site.latest] = Route{
      200,
      R"({"tag_name":")" + tag + R"(","draft":false,"prerelease":false,"assets":[)"
      R"({"name":"index.json","size":)" + std::to_string(index.size()) + R"(,"browser_download_url":")" + site.assets +
          tag + R"(/index.json"},)"
      R"({"name":"recipes.zip","size":)" + std::to_string(zip_size) + R"(,"browser_download_url":")" + site.assets + tag +
          R"(/recipes.zip"},)"
      R"({"name":"other.bin","size":5,"browser_download_url":"https://example.org/other.bin"}]})",
      {}};
  return {tag, index, zip};
}

struct Harness {
  Network network;
  Site site = MakeSite();
  std::atomic<std::int64_t> now{1'000'000};
  std::vector<std::string> log;
  std::mutex log_mutex;
  std::set<std::string> referenced;
  bool use_referenced = false;
  std::function<bool(const std::string&)> before_write;

  std::unique_ptr<CommunityRecipeService> Make(const std::string& root) {
    RecipeServiceConfig config;
    config.store_root = root;
    config.endpoints = site.endpoints;
    config.fetch = [this](const steam::HttpRequest& request, const std::atomic<bool>& cancel) {
      return network.Fetch(request, cancel);
    };
    config.log = [this](const std::string& line) {
      std::lock_guard<std::mutex> lock(log_mutex);
      log.push_back(line);
    };
    config.now = [this] { return now.load(); };
    if (use_referenced) config.referenced = [this] { return referenced; };
    config.before_write = before_write;
    config.worker_stack_bytes = 512 << 10;
    return std::make_unique<CommunityRecipeService>(std::move(config));
  }
  bool Logged(const std::string& text) {
    std::lock_guard<std::mutex> lock(log_mutex);
    for (const auto& line : log)
      if (line.find(text) != std::string::npos) return true;
    return false;
  }
};

const RecipeSummary* Find(const RecipeSnapshot& snapshot, std::uint32_t app, const std::string& id) {
  const auto* list = snapshot.For(app);
  if (!list) return nullptr;
  for (const auto& summary : *list)
    if (summary.id == id) return &summary;
  return nullptr;
}

std::string TempRoot(const char* tag) {
  std::string pattern = std::string("/tmp/playden-") + tag + "-XXXXXX";
  std::vector<char> buffer(pattern.begin(), pattern.end());
  buffer.push_back('\0');
  const char* made = mkdtemp(buffer.data());
  if (!made) return {};
  const auto base = fs::canonical(made);
  fs::create_directory(base / "user");
  return (base / "user" / "community-recipes").string();
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <tests/fixtures/community-recipes>\n", argv[0]);
    return 2;
  }
  const std::string fixtures = argv[1];
  const std::string good = Slurp(fixtures + "/recipes/274520/120hz-left-stick-pointer.json");
  const std::string good_id = "community-274520-120hz-left-stick-pointer";
  CHECK(!good.empty());
  const std::string second = Replace(good, good_id, "community-274520-second");
  const std::string flagged = Slurp(fixtures + "/flagged/publisher-key.json");
  const std::string hostile = Replace(Slurp(fixtures + "/hostile/wildcard-override.json"), good_id,
                                      "community-274520-hostile");
  const std::string other_app = Replace(Replace(good, good_id, "community-10-other"), "274520", "10");
  const std::string root = TempRoot("community-service");
  CHECK(!root.empty());

  const std::vector<Recipe> release1 = {
      {"recipes/274520/120hz-left-stick-pointer.json", good_id, good, ""},
      {"recipes/274520/second.json", "community-274520-second", second, ""},
      {"recipes/274520/publisher-key.json", "community-274520-publisher-key", flagged, ""},
      {"recipes/10/other.json", "community-10-other", other_app, ""},
      // Not a recipe at all.
      {"recipes/274520/broken.json", "community-274520-broken", "{not json", ""},
      // The index lists another revision for it.
      {"recipes/274520/mismatch.json", "community-274520-mismatch", second, std::string(64, 'b')},
      // In the zip, not in the index.
      {"recipes/274520/unlisted.json", "community-274520-unlisted", second, "-"},
      // The rules refuse it (a wildcard library override).
      {"recipes/274520/hostile.json", "community-274520-hostile", hostile, ""},
      // Listed under a path whose id differs from the one inside.
      {"recipes/274520/wrong-path.json", "community-274520-wrong-path", second, ""},
      // Larger than 16 KB.
      {"recipes/274520/huge.json", "community-274520-huge", good + std::string(17 << 10, ' '), ""},
  };
  const std::vector<Entry> outside = {
      {"other/274520/x.json", second},
      {"recipes/274520/", ""},
      {"recipes/../x.json", second},
      {"recipes/274520/sub/x.json", second},
      {"recipes/0274520/x.json", second},
      {"recipes/274520/Bad_Slug.json", second},
  };

  Harness h;
  h.use_referenced = true;
  const auto r1 = Publish(h.network, h.site, "r20261022.1", release1, outside);
  {
    h.network.routes[h.site.summary] = Route{
        200, R"({"generated_at":"x","recipes":{")" + good_id + R"(":{")" + Sha256Hex(good) +
                 R"(":{"up":42,"down":3,"reasons":{"slow":1,"crash":2},"kits":{},"verified":true,"author":"account"}}}})", {}};
    auto service = h.Make(root);
    service->Start();
    service->Wait();
    const auto snapshot = service->Snapshot();
    CHECK(snapshot->release_tag == "r20261022.1");
    CHECK(snapshot->last_error.empty());
    CHECK(!snapshot->refreshing);
    CHECK(snapshot->fetched_at == 1'000'000);
    const auto* list = snapshot->For(274520);
    CHECK(list && list->size() == 3);
    const auto* first = Find(*snapshot, 274520, good_id);
    CHECK(first && first->title == "120 Hz, pointer on left stick" && first->author == "TodorPenchev");
    CHECK(first && first->revision == Sha256Hex(good) && first->status == "works" && first->tested_kit == "0.12");
    CHECK(first && !first->notes.empty() && !first->flags.empty());
    CHECK(first && first->votes && first->votes->up == 42 && first->votes->down == 3);
    CHECK(first && first->votes && first->votes->reasons.size() == 2 && first->votes->reasons[0].first == "crash");
    CHECK(first && first->votes && first->votes->verified && first->votes->author == "account");
    CHECK(Find(*snapshot, 274520, "community-274520-second") && !Find(*snapshot, 274520, "community-274520-second")->votes);
    CHECK(Find(*snapshot, 274520, "community-274520-publisher-key"));
    CHECK(Find(*snapshot, 10, "community-10-other"));
    for (const char* dropped : {"broken", "mismatch", "unlisted", "hostile", "wrong-path", "huge"})
      CHECK(!Find(*snapshot, 274520, std::string("community-274520-") + dropped));
    for (const char* logged : {"community-274520-broken dropped: not a recipe", "mismatch.json dropped: does not match", "unlisted.json dropped: not listed",
                               "community-274520-hostile dropped: refused", "wrong-path dropped", "huge.json dropped",
                               "other/274520/x.json dropped", "recipes/274520/ dropped", "recipes/../x.json dropped",
                               "sub/x.json dropped", "0274520/x.json dropped", "Bad_Slug.json dropped"})
      if (!h.Logged(logged)) std::fprintf(stderr, "not logged: %s\n", logged), ++failures;
    // The recipe itself, by full revision or the 32-hex prefix an installed game's record holds.
    const auto loaded = service->LoadRecipe(Sha256Hex(good));
    CHECK(loaded && loaded->recipe.id == good_id && loaded->recipe.app_id == 274520);
    CHECK(service->LoadRecipe(Sha256Hex(good).substr(0, 32)));
    CHECK(!service->LoadRecipe(Sha256Hex(hostile)));
    CHECK(!service->LoadRecipe(std::string(64, 'c')));
    CHECK(!service->LoadRecipe("not-a-revision"));

    // Credentials: the recipes server's auth header (dev only) never reaches the API or the CDN.
    std::lock_guard<std::mutex> lock(h.network.mutex);
    bool api_seen = false, recipes_auth = false;
    for (const auto& request : h.network.seen) {
      const bool to_api = request.url == h.site.summary;
      api_seen |= to_api;
      const bool to_recipes = request.url.rfind("http://gitea.test:3000/", 0) == 0;
      if (DevEndpointsCompiledIn() && to_recipes) recipes_auth |= HasHeader(request, "Authorization");
      if (!to_recipes) CHECK(!HasHeader(request, "Authorization"));
      if (!DevEndpointsCompiledIn()) CHECK(request.url.rfind("https://", 0) == 0 && !HasHeader(request, "Authorization"));
      CHECK(request.url.find("example.org") == std::string::npos);  // other assets are never fetched
    }
    CHECK(api_seen);
    CHECK(recipes_auth == DevEndpointsCompiledIn());
  }

  // Offline: a new start loads the copy on disk; the refresh fails and says so.
  {
    h.network.offline = true;
    auto service = h.Make(root);
    service->Start();
    service->Wait();
    const auto snapshot = service->Snapshot();
    CHECK(snapshot->release_tag == "r20261022.1" && snapshot->For(274520) && snapshot->For(274520)->size() == 3);
    CHECK(!snapshot->last_error.empty());
    CHECK(Find(*snapshot, 274520, good_id) && Find(*snapshot, 274520, good_id)->votes);  // votes kept on disk
    CHECK(service->LoadRecipe(Sha256Hex(good).substr(0, 32)));
    // Not forced: waits an hour after a failure, and a day after success.
    h.network.Clear();
    service->RequestRefresh(false);
    service->Wait();
    CHECK(h.network.Count(h.site.latest) == 0);
    h.network.offline = false;
  }

  // The same release again: only the release and the votes are fetched.
  {
    auto service = h.Make(root);
    h.network.Clear();
    service->Start();
    service->Wait();
    CHECK(h.network.Count(h.site.latest) == 1);
    CHECK(h.network.Count(h.site.assets + "r20261022.1/recipes.zip") == 0);
    CHECK(service->Snapshot()->For(274520)->size() == 3);
    // Not forced within a day: nothing; a day later: a refresh.
    h.network.Clear();
    service->RequestRefresh(false);
    service->Wait();
    CHECK(h.network.Count(h.site.latest) == 0);
    h.now += CommunityRecipeService::kRefreshInterval + 1;
    service->RequestRefresh(false);
    service->Wait();
    CHECK(h.network.Count(h.site.latest) == 1);
  }

  // Switching releases: r2 changes "second"; r1's objects stay until nothing references them.
  const std::string second_v2 = Replace(second, "\"status\": \"works\"", "\"status\": \"playable\"");
  CHECK(second_v2 != second);
  {
    h.use_referenced = false;  // no garbage collection after this refresh
    Publish(h.network, h.site, "r20261023.1",
            {{"recipes/274520/120hz-left-stick-pointer.json", good_id, good, ""},
             {"recipes/274520/second.json", "community-274520-second", second_v2, ""}});
    auto service = h.Make(root);
    service->Start();
    service->Wait();
    auto snapshot = service->Snapshot();
    CHECK(snapshot->release_tag == "r20261023.1");
    CHECK(snapshot->For(274520) && snapshot->For(274520)->size() == 2);
    CHECK(Find(*snapshot, 274520, "community-274520-second")->revision == Sha256Hex(second_v2));
    CHECK(!snapshot->For(10));
    // r1's objects are still there: an installed game pinned to the old revision keeps working.
    CHECK(service->LoadRecipe(Sha256Hex(second)));
    CHECK(service->LoadRecipe(Sha256Hex(other_app)));
    // A game pinned (by prefix) to the old "second", another to "other" (full revision).
    CHECK(service->CollectGarbage({Sha256Hex(second).substr(0, 32), Sha256Hex(other_app)}) == 1);  // publisher-key
    CHECK(!service->LoadRecipe(Sha256Hex(flagged)));
    CHECK(service->LoadRecipe(Sha256Hex(second)) && service->LoadRecipe(Sha256Hex(other_app)));
    CHECK(service->CollectGarbage({}) == 2);
    CHECK(!service->LoadRecipe(Sha256Hex(second)) && !service->LoadRecipe(Sha256Hex(other_app)));
    CHECK(service->LoadRecipe(Sha256Hex(good)) && service->LoadRecipe(Sha256Hex(second_v2)));  // current: kept
  }

  // A refresh stopped before the pointer is replaced: the previous release stays in use, now and
  // after a restart.
  const std::string second_v3 = Replace(second, "\"status\": \"works\"", "\"status\": \"broken\"");
  {
    Publish(h.network, h.site, "r20261024.1", {{"recipes/274520/second.json", "community-274520-second", second_v3, ""}});
    h.before_write = [](const std::string& step) { return step != "current"; };
    auto service = h.Make(root);
    service->Start();
    service->Wait();
    CHECK(service->Snapshot()->release_tag == "r20261023.1");
    CHECK(!service->Snapshot()->last_error.empty());
    h.before_write = nullptr;
  }
  {
    h.network.offline = true;
    auto service = h.Make(root);
    service->Start();
    service->Wait();
    const auto snapshot = service->Snapshot();
    CHECK(snapshot->release_tag == "r20261023.1" && snapshot->For(274520) && snapshot->For(274520)->size() == 2);
    h.network.offline = false;
  }

  // Two refreshes at once: the second is dropped while the first runs.
  {
    auto service = h.Make(root);
    {
      std::lock_guard<std::mutex> lock(h.network.mutex);
      h.network.hold = h.site.latest;
      h.network.held = h.network.release = false;
    }
    h.network.Clear();
    service->RequestRefresh(true);
    {
      std::unique_lock<std::mutex> lock(h.network.mutex);
      h.network.changed.wait(lock, [&] { return h.network.held; });
    }
    CHECK(service->Busy());
    service->RequestRefresh(true);
    service->RequestRefresh(false);
    {
      std::lock_guard<std::mutex> lock(h.network.mutex);
      h.network.release = true;
      h.network.hold.clear();
      h.network.changed.notify_all();
    }
    service->Wait();
    CHECK(h.network.Count(h.site.latest) == 1);
    CHECK(service->Snapshot()->release_tag == "r20261024.1");
    CHECK(h.Logged("refresh already running"));
  }

  // Refused releases leave the copy in use.
  auto refused = [&](const char* what, const std::function<void()>& publish) {
    publish();
    auto service = h.Make(root);
    service->Start();
    service->Wait();
    const auto snapshot = service->Snapshot();
    if (snapshot->release_tag != "r20261024.1" || snapshot->last_error.empty())
      std::fprintf(stderr, "refused release accepted: %s\n", what), ++failures;
    CHECK(Find(*snapshot, 274520, "community-274520-second") &&
          Find(*snapshot, 274520, "community-274520-second")->revision == Sha256Hex(second_v3));
  };
  h.network.Clear();
  refused("zip larger than 2 MB (declared)", [&] {
    Publish(h.network, h.site, "r-big", {{"recipes/274520/second.json", "community-274520-second", second, ""}}, {},
            (2 << 20) + 1);
  });
  CHECK(h.network.Count(h.site.assets + "r-big/recipes.zip") == 0);  // refused before downloading
  refused("zip larger than declared", [&] {
    Publish(h.network, h.site, "r-bigger", {{"recipes/274520/second.json", "community-274520-second", second, ""}}, {}, 10);
  });
  refused("zip larger than 2 MB (sent)", [&] {
    Publish(h.network, h.site, "r-sent",
                             {{"recipes/274520/second.json", "community-274520-second", second, ""}});
    h.network.routes["https://objects.githubusercontent.com/r-sent/recipes.zip"].body += std::string(2 << 20, 'x');
  });
  refused("too many entries", [&] {
    std::vector<Entry> many;
    for (int i = 0; i < 2001; ++i) many.push_back({"recipes/10/n" + std::to_string(i) + ".json", "x"});
    Publish(h.network, h.site, "r-many", {{"recipes/274520/second.json", "community-274520-second", second, ""}}, many);
  });
  refused("zip does not match the index", [&] {
    Publish(h.network, h.site, "r-hash", {{"recipes/274520/second.json", "community-274520-second", second, ""}});
    auto& zip = h.network.routes["https://objects.githubusercontent.com/r-hash/recipes.zip"].body;
    zip[zip.size() / 2] ^= 1;
  });
  refused("unsafe tag", [&] {
    Publish(h.network, h.site, "../escape", {{"recipes/274520/second.json", "community-274520-second", second, ""}});
  });
  refused("redirect to another host", [&] {
    Publish(h.network, h.site, "r-evil", {{"recipes/274520/second.json", "community-274520-second", second, ""}});
    h.network.routes[h.site.assets + "r-evil/index.json"] = Route{302, {}, {{"Location", "https://evil.example.org/i"}}};
  });
  CHECK(h.network.Count("https://evil.example.org/i") == 0);
  // In a release build the configured (dev) endpoints were never used.
  if (!DevEndpointsCompiledIn()) CHECK(h.network.Count("http://gitea.test:3000/api/v1/repos/o/r/releases/latest") == 0);

  // Votes are optional: without summary.json the refresh still succeeds.
  {
    h.network.routes.erase(h.site.summary);
    Publish(h.network, h.site, "r20261025.1", {{"recipes/274520/second.json", "community-274520-second", second, ""}});
    auto service = h.Make(root);
    service->Start();
    service->Wait();
    const auto snapshot = service->Snapshot();
    CHECK(snapshot->release_tag == "r20261025.1" && snapshot->last_error.empty());
    CHECK(h.Logged("vote totals unavailable"));
  }

  // A refresh writing objects (parked on a latch under the store's write lock) doesn't hold up
  // LoadRecipe, which the UI thread calls. Then Playden closes mid-write: the writes stop between
  // files and the pointer stays on the previous release.
  {
    const std::string third = Replace(second, "community-274520-second", "community-274520-third");
    const std::string fourth = Replace(second, "community-274520-second", "community-274520-fourth");
    Publish(h.network, h.site, "r20261026.1",
            {{"recipes/274520/third.json", "community-274520-third", third, ""},
             {"recipes/274520/fourth.json", "community-274520-fourth", fourth, ""}});
    std::mutex latch_mutex;
    std::condition_variable latch;
    bool parked = false, go = false;
    int objects = 0;
    h.before_write = [&](const std::string& step) {
      if (step != "object") return true;
      std::unique_lock<std::mutex> lock(latch_mutex);
      if (++objects == 1) {
        parked = true;
        latch.notify_all();
        latch.wait(lock, [&] { return go; });
      }
      return true;
    };
    auto service = h.Make(root);
    h.before_write = nullptr;
    service->Start();
    {
      std::unique_lock<std::mutex> lock(latch_mutex);
      CHECK(latch.wait_for(lock, std::chrono::seconds(10), [&] { return parked; }));
    }
    auto loading = std::async(std::launch::async, [&] { return service->LoadRecipe(Sha256Hex(second)).has_value(); });
    auto by_prefix =
        std::async(std::launch::async, [&] { return service->LoadRecipe(Sha256Hex(second).substr(0, 32)).has_value(); });
    CHECK(loading.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    CHECK(by_prefix.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    // Playden closes while the first object is being written.
    std::thread closing([&] { service->Shutdown(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));  // Shutdown sets the cancel flag, then joins
    {
      std::lock_guard<std::mutex> lock(latch_mutex);
      go = true;
      latch.notify_all();
    }
    closing.join();
    CHECK(loading.get() && by_prefix.get());
    CHECK(objects == 1);  // the second object was never started
    CHECK(service->Snapshot()->release_tag == "r20261025.1");
    RecipeStore store{fs::path(root)};
    std::string ignored;
    CHECK(store.Prepare(&ignored) && store.ReadCurrent() && store.ReadCurrent()->tag == "r20261025.1");
    CHECK(!store.ReadObject(Sha256Hex(fourth)));
  }

  fs::remove_all(fs::path(root).parent_path().parent_path());
  if (failures) std::fprintf(stderr, "%d failure(s)\n", failures);
  else std::printf("community recipe service (%s) ok\n", DevEndpointsCompiledIn() ? "dev" : "release");
  return failures ? 1 : 0;
}
