// SPDX-License-Identifier: GPL-3.0-or-later
#include "steam/backend/download_server_source.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>

using namespace playden;
using namespace playden::steam;
using namespace std::chrono_literals;
namespace {
std::vector<std::uint8_t> Read(const std::string& path) {
  FILE* file = std::fopen(path.c_str(), "rb"); assert(file);
  std::vector<std::uint8_t> bytes;
  int c; while ((c = std::fgetc(file)) != EOF) bytes.push_back(static_cast<std::uint8_t>(c));
  std::fclose(file); return bytes;
}
HttpResponse Reply(const std::string& text) {
  HttpResponse response; response.status_code = 200;
  response.body.assign(text.begin(), text.end()); return response;
}
void ListingPolicy() {
  std::atomic<bool> cancel{false};
  for (bool mixed : {false, true}) {
    CdnClient cdn([&](const HttpRequest& request, const std::atomic<bool>&) {
      assert(request.url.find("cell_id=0") != std::string::npos);
      return Reply(std::string(R"({"response":{"servers":[{"type":"CDN","host":"http.example","https_support":"unavailable"})") +
          (mixed ? R"(,{"type":"CDN","host":"fastly.cdn.steampipe.steamcontent.com","https_support":"mandatory"})" : "") + "]}}");
    });
    CdnDownloadServerSource source(cdn, {});
    std::vector<DownloadServerView> servers;
    assert(source.List(cancel, &servers) && servers.size() == 1);
    assert(servers[0].plain_http == !mixed);
    assert(servers[0].name == (mixed ? "Fastly" : "http.example"));
  }
  // Servers of one provider are told apart by their host; a lone one keeps the provider name.
  CdnClient cdn([&](const HttpRequest&, const std::atomic<bool>&) {
    return Reply(R"({"response":{"servers":[{"type":"CDN","host":"cache5-fra1.steamcontent.com"},)"
                 R"({"type":"CDN","host":"cache2-ams1.steamcontent.com"},{"type":"CDN","host":"fastly.cdn.steampipe.steamcontent.com"},)"
                 R"({"type":"CDN","host":"google2.cdn.steampipe.steamcontent.com"},{"type":"CDN","host":"google3.cdn.steampipe.steamcontent.com"}]}})");
  });
  CdnDownloadServerSource source(cdn, {});
  std::vector<DownloadServerView> servers;
  assert(source.List(cancel, &servers) && servers.size() == 5);
  assert(servers[0].name == "Steam (Valve) · cache5-fra1" && servers[1].name == "Steam (Valve) · cache2-ams1");
  assert(servers[2].name == "Fastly");
  assert(servers[3].name == "Google Cloud · google2" && servers[4].name == "Google Cloud · google3");
}
void PayloadAndTransfers(const std::string& fixtures) {
  const auto manifest = Read(fixtures + "/manifest_encrypted_deflated.zip");
  std::atomic<bool> cancel{false};
  CdnClient cdn([&](const HttpRequest& request, const std::atomic<bool>&) {
    if (request.url.find("/manifest/") != std::string::npos) { auto r = Reply(""); r.body = manifest; return r; }
    return Reply(R"({"response":{"servers":[{"type":"CDN","host":"fastly.cdn.steampipe.steamcontent.com"},{"type":"CDN","host":"google2.cdn.steampipe.steamcontent.com"}]}})");
  });
  std::atomic<std::uint64_t> milliseconds{0}, reserved{0};
  std::atomic<int> requests{0};
  CmError primary_error = CmError::kRejected;
  int fallback_calls = 0;
  std::vector<std::uint32_t> fallback_depots;
  CdnDownloadServerSource::Hooks hooks;
  hooks.app_info = [&](const StoredSession&, std::uint32_t app, const std::atomic<bool>&) {
    AppInfoBuffers info; info.error = CmError::kNone;
    info.buffers[app] = "\"appinfo\" { \"appid\" \"" + std::to_string(app) + "\" \"depots\" { \"" +
        std::string(app == 228980 ? "228990" : "4242001") + "\" { \"manifests\" { \"public\" { \"gid\" \"7234567890123456789\" } } } } }";
    if (app != 228980) {
      auto& text = info.buffers[app];
      const auto depots = text.find("\"depots\" {");
      text.insert(depots + 10, " \"4242000\" { \"manifests\" { \"public\" { \"gid\" \"4\" } } } ");
    }
    return info;
  };
  hooks.access = [&](const StoredSession&, std::uint32_t app, const std::vector<DepotRequest>& requests, const std::atomic<bool>&) {
    DepotAccessResult result;
    result.error = app == 228980 ? primary_error : CmError::kNone;
    if (app != 228980) {
      fallback_depots.push_back(requests[0].depot_id);
      if (requests[0].depot_id == 4242000) result.error = CmError::kRejected;
    }
    if (result.ok()) result.grants.push_back({requests[0].depot_id, requests[0].manifest_gid, {}, 9});
    return result;
  };
  hooks.now = [&] { return CdnDownloadServerSource::Clock::time_point{} + std::chrono::milliseconds(milliseconds.load()); };
  hooks.perform = [&](std::size_t slot, const HttpRequest& request, const std::atomic<bool>& stop) {
    assert(slot < 4 && request.timeout_ms <= 8000 && request.max_response_bytes <= 16 * 1024 * 1024);
    assert(!request.allow_plain_http && request.url.find("/depot/4242001/chunk/") != std::string::npos);
    assert(!stop);
    reserved += request.max_response_bytes;
    ++requests;
    milliseconds += request.url.find("google2") != std::string::npos ? 200 : 100;
    auto result = Reply(""); result.body.resize(1000); return result;
  };
  CdnDownloadServerSource source(cdn, hooks, [&] { ++fallback_calls; return 440; });
  std::vector<DownloadServerView> servers;
  assert(source.List(cancel, &servers) && servers.size() == 2);
  const auto prepared = source.Prepare({}, cancel);
  assert(prepared.error == DownloadServerPreparation::Error::kNone && prepared.app_id == 440 && fallback_calls == 1);
  assert((fallback_depots == std::vector<std::uint32_t>{4242000, 4242001}));
  const auto fast = source.Measure(servers[0].id, 100'000, cancel);
  const auto fast_requests = requests.load();
  assert(!fast.failed && fast.mb_per_s > 0 && reserved <= 100'000 && fast_requests > 0);
  reserved = 0; requests = 0; milliseconds = 0;
  const auto slow = source.Measure(servers[1].id, 100'000, cancel);
  assert(!slow.failed && slow.mb_per_s < fast.mb_per_s && reserved <= 100'000);
  primary_error = CmError::kNetwork;
  assert(source.Prepare({}, cancel).error == DownloadServerPreparation::Error::kFailed && fallback_calls == 1);
  primary_error = CmError::kRejected;
  CdnDownloadServerSource no_game(cdn, hooks);
  assert(no_game.List(cancel, &servers));
  assert(no_game.Prepare({}, cancel).error == DownloadServerPreparation::Error::kNeedsInstalledGame);
  // A clock crossing the shared deadline prevents subsequent requests and gives each in-flight
  // transfer only the remaining time, independently of the full eight-second ceiling.
  hooks.perform = [&](std::size_t, const HttpRequest& request, const std::atomic<bool>&) {
    assert(request.timeout_ms <= 8000);
    milliseconds = 8000;
    auto response = Reply(""); response.body.resize(1000); return response;
  };
  CdnDownloadServerSource deadline(cdn, hooks, [] { return 440; });
  milliseconds = 0;
  assert(deadline.List(cancel, &servers));
  assert(deadline.Prepare({}, cancel).error == DownloadServerPreparation::Error::kNone);
  assert(!deadline.Measure(servers[0].id, 32'000'000, cancel).failed);
  assert(milliseconds == 8000);
  requests = 0;
  hooks.perform = [&](std::size_t, const HttpRequest&, const std::atomic<bool>&) {
    ++requests;
    HttpResponse response; response.error = HttpError::kConnectionFailed; return response;
  };
  CdnDownloadServerSource failed(cdn, hooks, [] { return 440; });
  assert(failed.List(cancel, &servers));
  assert(failed.Prepare({}, cancel).error == DownloadServerPreparation::Error::kNone);
  assert(failed.Measure(servers[0].id, 32'000'000, cancel).failed);
  assert(requests >= 2 && requests <= 5);  // two failures plus already in-flight slots

  // Every slot must be independently usable; one blocked transfer cannot serialize the others.
  std::atomic<unsigned> slots{0};
  hooks.perform = [&](std::size_t slot, const HttpRequest&, const std::atomic<bool>& stop) {
    slots.fetch_or(1u << slot);
    while (!stop) std::this_thread::sleep_for(1ms);
    HttpResponse response; response.error = HttpError::kCanceled; return response;
  };
  CdnDownloadServerSource canceled(cdn, hooks, [] { return 440; });
  assert(canceled.List(cancel, &servers));
  assert(canceled.Prepare({}, cancel).error == DownloadServerPreparation::Error::kNone);
  std::thread measurement([&] { canceled.Measure(servers[0].id, 32'000'000, cancel); });
  const auto wait_until = std::chrono::steady_clock::now() + 2s;
  while (slots != 15 && std::chrono::steady_clock::now() < wait_until) std::this_thread::sleep_for(1ms);
  assert(slots == 15);
  cancel = true;
  measurement.join();
  cancel = false;

  // A manifest returned under a different authorized GID is never used as the sample payload.
  const auto normal_info = hooks.app_info;
  hooks.app_info = [normal_info](const StoredSession& session, std::uint32_t app, const std::atomic<bool>& stop) {
    auto result = normal_info(session, app, stop);
    auto& text = result.buffers[app];
    const auto gid = text.find("7234567890123456789");
    assert(gid != std::string::npos);
    text.replace(gid, 19, "1");
    return result;
  };
  CdnDownloadServerSource mismatched(cdn, hooks, [] { return 440; });
  assert(mismatched.List(cancel, &servers));
  assert(mismatched.Prepare({}, cancel).error == DownloadServerPreparation::Error::kFailed);
}
void MetadataSharesTheTotalBudget(const std::string& fixtures) {
  const auto manifest = Read(fixtures + "/manifest_encrypted_deflated.zip");
  std::atomic<bool> cancel{false};
  for (const bool exhaust : {false, true}) {
    std::vector<std::size_t> caps;
    CdnClient cdn([&](const HttpRequest& request, const std::atomic<bool>&) {
      if (request.url.find("/manifest/") == std::string::npos)
        return Reply(R"({"response":{"servers":[{"type":"CDN","host":"a.example"},{"type":"CDN","host":"b.example"}]}})");
      caps.push_back(request.max_response_bytes);
      if (exhaust || caps.size() <= 2) {
        HttpResponse response;
        response.error = HttpError::kTimeout;
        response.timing.body_bytes = request.max_response_bytes;
        return response;
      }
      auto response = Reply(""); response.body = manifest; return response;
    });
    CdnDownloadServerSource::Hooks hooks;
    hooks.app_info = [](const StoredSession&, std::uint32_t app, const std::atomic<bool>&) {
      AppInfoBuffers result; result.error = CmError::kNone;
      auto& text = result.buffers[app];
      text = "\"appinfo\" { \"appid\" \"" + std::to_string(app) + "\" \"depots\" { ";
      if (app == 228980) text += "\"228990\" { \"manifests\" { \"public\" { \"gid\" \"1\" } } } ";
      else for (int depot = 4242000; depot < 4242010; ++depot)
        text += "\"" + std::to_string(depot) + "\" { \"manifests\" { \"public\" { \"gid\" \"7234567890123456789\" } } } ";
      text += "} }";
      return result;
    };
    hooks.access = [](const StoredSession&, std::uint32_t app, const std::vector<DepotRequest>& requests, const std::atomic<bool>&) {
      DepotAccessResult result; result.error = app == 228980 ? CmError::kRejected : CmError::kNone;
      if (result.ok()) result.grants.push_back({requests[0].depot_id, requests[0].manifest_gid, {}, 9});
      return result;
    };
    CdnDownloadServerSource source(cdn, hooks, [] { return 440; });
    std::vector<DownloadServerView> servers;
    assert(source.List(cancel, &servers));
    const auto prepared = source.Prepare({}, cancel);
    std::uint64_t reserved = 0;
    for (const auto cap : caps) { assert(cap > 0 && cap <= 16 * 1024 * 1024); reserved += cap; }
    if (exhaust) {
      assert(prepared.error == DownloadServerPreparation::Error::kFailed);
      assert(reserved == 160'000'000 - 512 * 1024);
      assert(caps.back() < 16 * 1024 * 1024);
    } else {
      assert(prepared.error == DownloadServerPreparation::Error::kNone);
      assert(prepared.measurement_byte_budget == 160'000'000 - 512 * 1024 - 2 * 16 * 1024 * 1024 - manifest.size());
    }
  }
}
}  // namespace
int main(int argc, char** argv) { assert(argc == 2); ListingPolicy(); PayloadAndTransfers(argv[1]); MetadataSharesTheTotalBudget(argv[1]); }
