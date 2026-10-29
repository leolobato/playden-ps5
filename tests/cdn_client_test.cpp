#include "steam/backend/cdn_client.h"

#include <cassert>
#include <string>
#include <vector>

using namespace playden::steam;

namespace {

HttpResponse Body(long status, const std::string& text) {
  HttpResponse response;
  response.status_code = status;
  response.body.assign(text.begin(), text.end());
  return response;
}

void LabelsAndDisplayNames() {
  struct Case { const char* host; const char* label; const char* display; };
  for (const auto& test : std::vector<Case>{
      {"steampipe.akamaized.net", "akamai", "Akamai"},
      {"FASTLY.CDN.STEAMPIPE.STEAMCONTENT.COM", "fastly", "Fastly"},
      {"google2.cdn.steampipe.steamcontent.com", "google", "Google Cloud"},
      {"google3.cdn.steampipe.steamcontent.com", "google", "Google Cloud"},
      {"alibaba.cdn.steampipe.steamcontent.com", "alibaba", "Alibaba Cloud"},
      {"edgenext.cdn.steampipe.steamcontent.com", "edgenext", "EdgeNext"},
      {"cache1.fra.steamcontent.com", "valve", "Steam (Valve)"},
      {"isp.example", "cache", "isp.example"},
      {"fastly.cdn.steampipe.steamcontent.com.evil.example", "cache",
       "fastly.cdn.steampipe.steamcontent.com.evil.example"}}) {
    assert(CdnServerLabel(test.host) == test.label);
    assert(CdnServerDisplayName(test.host) == test.display);
  }
}

void PreferredOrderKeepsOthersAndFirstDuplicate() {
  std::vector<ContentServer> servers{{"a", "a"}, {"b", "first"}, {"c", "c"}, {"b", "second"}};
  assert(!PreferServer(&servers, "").preferred_found && servers[0].host == "a");
  assert(!PreferServer(&servers, "missing").preferred_found && servers[0].host == "a");
  const auto order = PreferServer(&servers, "b");
  assert(order.preferred_found && order.preferred_index == 1);
  assert(servers[0].vhost == "first" && servers[1].host == "a" && servers[2].host == "c" && servers[3].vhost == "second");
  assert(PreferServer(&servers, "b").preferred_index == 0);
  servers.clear();
  assert(!PreferServer(&servers, "a").preferred_found);
}

void PriorityIsBoundedWithoutReordering() {
  CdnClient client([](const HttpRequest&, const std::atomic<bool>&) {
    return Body(200, R"({"response":{"servers":[
      {"type":"CDN","host":"a","load":0,"priority_class":8},
      {"type":"CDN","host":"b","load":0,"priority_class":1},
      {"type":"CDN","host":"c","load":0,"priority_class":999},
      {"type":"CDN","host":"d","load":0,"priority_class":-1},
      {"type":"CDN","host":"e","load":0,"priority_class":"2"},
      {"type":"CDN","host":"f","load":0}]}})");
  });
  std::atomic<bool> cancel{false};
  std::vector<ContentServer> servers;
  assert(client.Servers(0, cancel, &servers) == CdnError::kNone);
  assert(servers.size() == 6 && servers[0].host == "a" && servers[1].host == "b");
  assert(servers[0].priority == 8 && servers[1].priority == 1 && servers[2].priority == 9);
  assert(servers[3].priority == 0 && servers[4].priority == 0 && servers[5].priority == 0);
}

void TotalsIncludeFailedRequests() {
  int calls = 0;
  CdnClient client([&](const HttpRequest&, const std::atomic<bool>&) {
    auto response = Body(++calls == 1 ? 200 : 503, "payload");
    response.timing = {1, 2, 3, 4, 5, 1, 7};
    return response;
  });
  std::atomic<bool> cancel{false};
  std::vector<std::uint8_t> out;
  ContentServer server{"h.example.invalid", "v.example.invalid", 1};
  assert(client.Chunk(server, 1, std::string(40, 'a'), 1024, cancel, &out) == CdnError::kNone);
  assert(client.Chunk(server, 1, std::string(40, 'a'), 1024, cancel, &out) == CdnError::kNetwork);
  const auto& stats = client.stats();
  assert(stats.requests == 2 && stats.failures == 1 && stats.bytes == 14);
  assert(stats.resolve_ms == 2 && stats.connect_ms == 4 && stats.tls_ms == 6);
  assert(stats.first_byte_ms == 8 && stats.total_ms == 10 && stats.new_connections == 2);
}

void ServersFilterAndSortByLoad() {
  std::vector<std::string> urls;
  CdnClient client([&](const HttpRequest& request, const std::atomic<bool>&) {
    urls.push_back(request.url);
    return Body(200, R"({"response":{"servers":[
      {"type":"CDN","host":"b.example.invalid","vhost":"vb.example.invalid","load":50,"https_support":"mandatory"},
      {"type":"SteamCache","host":"a.example.invalid","load":10,"https_support":"optional"},
      {"type":"CDN","host":"c.example.invalid","load":1,"https_support":"unavailable"},
      {"type":"OpenCache","host":"d.example.invalid","load":0},
      {"type":"CDN","host":"bad host/x","load":0}]}})");
  });
  std::atomic<bool> cancel{false};
  std::vector<ContentServer> servers;
  assert(client.Servers(42, cancel, &servers) == CdnError::kNone);
  // The plain-HTTP server is left out while HTTPS servers are listed, even with the lowest load.
  assert(servers.size() == 2);
  assert(servers[0].host == "a.example.invalid" && servers[0].vhost == "a.example.invalid" && servers[0].https);
  assert(servers[1].vhost == "vb.example.invalid" && servers[1].https);
  assert(urls[0].find("GetServersForSteamPipe/v1/?cell_id=42") != std::string::npos);
  assert(urls[0].rfind("https://", 0) == 0);
}

void PlainHttpServersWhenNoHttpsIsListed() {
  bool plain_allowed = true;
  CdnClient client([&](const HttpRequest& request, const std::atomic<bool>&) {
    plain_allowed = request.allow_plain_http;
    return Body(200, R"({"response":{"servers":[
      {"type":"CDN","host":"b.example.invalid","load":20,"https_support":"unavailable"},
      {"type":"CDN","host":"a.example.invalid","vhost":"va.example.invalid","load":5,"https_support":"unavailable"},
      {"type":"OpenCache","host":"c.example.invalid","load":0,"https_support":"mandatory"}]}})");
  });
  std::atomic<bool> cancel{false};
  std::vector<ContentServer> servers;
  assert(client.Servers(7, cancel, &servers) == CdnError::kNone);
  assert(!plain_allowed);  // the directory itself stays HTTPS-only
  assert(servers.size() == 2);
  assert(servers[0].vhost == "va.example.invalid" && !servers[0].https);
  assert(servers[1].host == "b.example.invalid" && !servers[1].https);
}

void EmptyServerListSaysWhy() {
  std::string body = R"({"response":{"servers":[
    {"type":"OpenCache","host":"a.example.invalid"},{"type":"CDN","host":"bad host/x"}]}})";
  CdnClient client([&](const HttpRequest&, const std::atomic<bool>&) { return Body(200, body); });
  std::atomic<bool> cancel{false};
  std::vector<ContentServer> servers;
  assert(client.Servers(7, cancel, &servers) == CdnError::kProtocol && servers.empty());
  assert(client.last_failure() == "servers=2 usable=0");
  body = R"({"response":{}})";
  assert(client.Servers(7, cancel, &servers) == CdnError::kProtocol);
  assert(client.last_failure() == "servers=malformed");
}

void ManifestAndChunkUrlsAndErrors() {
  std::vector<std::string> urls;
  long status = 200;
  CdnClient client([&](const HttpRequest& request, const std::atomic<bool>&) {
    urls.push_back(request.url);
    return Body(status, "payload");
  });
  std::atomic<bool> cancel{false};
  const ContentServer server{"h.example.invalid", "v.example.invalid", 1};
  std::vector<std::uint8_t> out;
  assert(client.Manifest(server, 441, 11, 99, cancel, &out) == CdnError::kNone && out.size() == 7);
  assert(urls.back() == "https://v.example.invalid/depot/441/manifest/11/5/99");
  const std::string sha(40, 'a');
  assert(client.Chunk(server, 441, sha, 1024, cancel, &out) == CdnError::kNone);
  assert(urls.back() == "https://v.example.invalid/depot/441/chunk/" + sha);
  assert(client.Chunk(server, 441, "../x", 1024, cancel, &out) == CdnError::kProtocol);
  status = 404;
  assert(client.Chunk(server, 441, sha, 1024, cancel, &out) == CdnError::kNotFound);
  status = 503;
  assert(client.Chunk(server, 441, sha, 1024, cancel, &out) == CdnError::kNetwork);
  assert(client.Manifest({"h", "bad host", 1}, 1, 1, 1, cancel, &out) == CdnError::kProtocol);
}

void PlainHttpServerUrls() {
  std::vector<HttpRequest> requests;
  CdnClient client([&](const HttpRequest& request, const std::atomic<bool>&) {
    requests.push_back(request);
    return Body(200, "payload");
  });
  std::atomic<bool> cancel{false};
  const ContentServer plain{"h.example.invalid", "v.example.invalid", 1, false};
  const ContentServer secure{"h.example.invalid", "v.example.invalid", 1};
  std::vector<std::uint8_t> out;
  const std::string sha(40, 'b');
  assert(client.Manifest(plain, 441, 11, 99, cancel, &out) == CdnError::kNone);
  assert(requests.back().url == "http://v.example.invalid/depot/441/manifest/11/5/99");
  assert(requests.back().allow_plain_http);
  assert(client.Chunk(plain, 441, sha, 1024, cancel, &out) == CdnError::kNone);
  assert(requests.back().url == "http://v.example.invalid/depot/441/chunk/" + sha);
  assert(requests.back().allow_plain_http);
  assert(client.Chunk(secure, 441, sha, 1024, cancel, &out) == CdnError::kNone);
  assert(requests.back().url == "https://v.example.invalid/depot/441/chunk/" + sha);
  assert(!requests.back().allow_plain_http);
}

}  // namespace

int main() {
  LabelsAndDisplayNames();
  PreferredOrderKeepsOthersAndFirstDuplicate();
  PriorityIsBoundedWithoutReordering();
  TotalsIncludeFailedRequests();
  ServersFilterAndSortByLoad();
  PlainHttpServersWhenNoHttpsIsListed();
  EmptyServerListSaysWhy();
  ManifestAndChunkUrlsAndErrors();
  PlainHttpServerUrls();
}
