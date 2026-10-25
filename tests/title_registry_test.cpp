#include "platform/ps5/title_registry.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
using namespace playden::ps5;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)
int main() {
  std::string response = R"({"status":0,"games":[]})";
  std::string mutation_response = R"({"status":0})";
  std::vector<std::string> requests;
  TitleRegistry registry([&](const std::string& route, const std::string& body) -> std::optional<std::string> {
    requests.push_back(route + " " + body); return route == "/api/v1/games" ? response : mutation_response;
  });
  CHECK(registry.Query("FAKE78000").state == RegistrationState::kAbsent);
  response = R"({"status":0,"games":[{"title_id":"FAKE78000","installed":true,"managed":true,"source_available":true,"source_type":"folder","path":"/data/homebrew/FAKE78000"}]})";
  CHECK(registry.Query("FAKE78000").state == RegistrationState::kReady);
  response = R"({"status":0,"games":[{"title_id":"FAKE78000","installed":true,"managed":true,"source_available":true,"path":"/data/somebody-else"}]})";
  CHECK(registry.Query("FAKE78000").state == RegistrationState::kCollision);
  auto collisions_before = requests.size();
  CHECK(!registry.Register("FAKE78000").ok);
  CHECK(requests.size() == collisions_before + 1);
  response = "{}";
  CHECK(registry.Query("FAKE78000").state == RegistrationState::kUnavailable);
  response = R"({"status":0,"games":[{"title_id":"FAKE78000","installed":false,"managed":true,"source_available":true,"source_type":"folder","path":"/data/homebrew/FAKE78000"}]})";
  CHECK(registry.Query("FAKE78000").state == RegistrationState::kPending);
  // ShadowMount keeps removed titles as unmanaged, unavailable tombstones.
  const std::string removed = R"({"title_id":"FAKE78000","installed":false,"managed":false,"mounted":false,"source_available":false,"source_type":"folder","runtime_path":"/data/homebrew/FAKE78000","path":"/data/homebrew/FAKE78000"})";
  response = "{\"status\":0,\"games\":[" + removed + "]}";
  CHECK(registry.Query("FAKE78000").state == RegistrationState::kAbsent);
  CHECK(registry.Register("FAKE78000").ok);
  CHECK(requests[requests.size()-2] == "/api/v1/manual/add {\"path\":\"/data/homebrew/FAKE78000\"}");
  for (const auto* flag : {"installed", "managed", "mounted", "source_available"}) {
    auto live = removed;
    const std::string field = "\"" + std::string(flag) + "\":false";
    live.replace(live.find(field), field.size(), "\"" + std::string(flag) + "\":true");
    response = "{\"status\":0,\"games\":[" + live + "]}";
    CHECK(registry.Query("FAKE78000").state != RegistrationState::kAbsent);
    live = removed;
    live.erase(live.find(field), field.size() + 1);
    response = "{\"status\":0,\"games\":[" + live + "]}";
    CHECK(registry.Query("FAKE78000").state != RegistrationState::kAbsent);
    live = removed;
    live.replace(live.find(field), field.size(), "\"" + std::string(flag) + "\":0");
    response = "{\"status\":0,\"games\":[" + live + "]}";
    CHECK(registry.Query("FAKE78000").state != RegistrationState::kAbsent);
  }
  for (const auto* key : {"path", "runtime_path", "source_type"}) {
    auto foreign = removed;
    const auto value = foreign.find(":\"", foreign.find("\"" + std::string(key) + "\"")) + 2;
    foreign.replace(value, foreign.find('"', value) - value, "other");
    response = "{\"status\":0,\"games\":[" + foreign + "]}";
    CHECK(registry.Query("FAKE78000").state != RegistrationState::kAbsent);
  }
  auto incomplete = removed;
  incomplete.erase(incomplete.find("\"mounted\":false,"), std::string("\"mounted\":false,").size());
  response = "{\"status\":0,\"games\":[" + incomplete + "]}";
  CHECK(registry.Query("FAKE78000").state != RegistrationState::kAbsent);
  response = "{\"status\":0,\"games\":[" + removed + "," + removed + "]}";
  CHECK(registry.Query("FAKE78000").state == RegistrationState::kCollision);
  response = R"({"status":0,"games":[]})";
  CHECK(registry.Register("FAKE78000").ok);
  CHECK(requests[requests.size()-2] == "/api/v1/manual/add {\"path\":\"/data/homebrew/FAKE78000\"}");
  CHECK(requests.back() == "/api/v1/scan {}");
  auto before = requests.size();
  CHECK(!registry.Register("FAKE77031").ok && !registry.Register("PPSA99995").ok);
  CHECK(requests.size() == before);
  mutation_response = R"({"status":16,"error":"busy"})";
  const auto busy = registry.Register("FAKE78000");
  CHECK(!busy.ok && busy.code == 16);
  mutation_response = R"({"status":0})";
  CHECK(registry.RemoveManualSource("FAKE78000").ok);
  CHECK(requests.back() == "/api/v1/manual/remove {\"path\":\"/data/homebrew/FAKE78000\"}");
  CHECK(registry.Unregister("FAKE78000").ok);
  CHECK(requests.back() == "/api/v1/games/uninstall {\"title_id\":\"FAKE78000\"}");
  CHECK(!registry.Unregister("FAKE77031").ok);
  // A title already pending its scan is never submitted again.
  response = R"({"status":0,"games":[{"title_id":"FAKE78000","installed":false,"managed":true,"source_available":true,"source_type":"folder","path":"/data/homebrew/FAKE78000"}]})";
  before = requests.size();
  const auto pending = registry.Register("FAKE78000");
  CHECK(pending.ok && pending.stage == "pending" && requests.size() == before + 1);
  response = R"({"status":0,"games":[]})";
  // No answer to the add: uncertain, not a failure to retry.
  bool answer_add = false, answer_scan = true;
  TitleRegistry flaky([&](const std::string& route, const std::string&) -> std::optional<std::string> {
    if (route == "/api/v1/games") return response;
    if (route == "/api/v1/manual/add") return answer_add ? std::optional<std::string>(R"({"status":0})") : std::nullopt;
    return answer_scan ? std::optional<std::string>(R"({"status":0})") : std::nullopt;
  });
  const auto unanswered = flaky.Register("FAKE78000");
  CHECK(!unanswered.ok && unanswered.stage == "uncertain");
  // A lost scan request after an accepted add still counts as submitted.
  answer_add = true; answer_scan = false;
  const auto deferred = flaky.Register("FAKE78000");
  CHECK(deferred.ok && deferred.stage == "scan-deferred");
  // Another source's malformed ID says nothing about this title; one in this title's folder does.
  response = R"({"status":0,"games":[{"title_id":"homebrew","path":"/data/homebrew/other"},{"title_id":"ABCD1234"}]})";
  auto odd = registry.Query("FAKE78000");
  CHECK(odd.state == RegistrationState::kUnavailable && odd.reason == "bad-title-id");
  response = R"({"status":0,"games":[{"title_id":"homebrew","path":"/data/homebrew/other"},{"path":"/mnt/usb0/x"}]})";
  CHECK(registry.Query("FAKE78000").state == RegistrationState::kAbsent);
  response = R"({"status":0,"games":[{"title_id":"fake78000","path":"/data/homebrew/FAKE78000"}]})";
  CHECK(registry.Query("FAKE78000").reason == "bad-title-id");
  // An odd entry elsewhere never hides a real collision, in either order.
  const std::string foreign_valid = R"({"title_id":"FAKE78000","installed":true,"managed":true,"source_available":true,"source_type":"folder","path":"/mnt/usb0/x"})";
  const std::string unrelated = R"({"title_id":"odd","path":"/data/homebrew/other"})";
  response = "{\"status\":0,\"games\":[" + unrelated + "," + foreign_valid + "]}";
  CHECK(registry.Query("FAKE78000").state == RegistrationState::kCollision);
  response = "{\"status\":0,\"games\":[" + foreign_valid + "," + unrelated + "]}";
  CHECK(registry.Query("FAKE78000").state == RegistrationState::kCollision);
  response = R"({"status":0,"games":[{"title_id":"x","path":"/data/homebrew/other"},{"title_id":"FAKE78000","installed":true,"managed":true,"source_available":true,"source_type":"folder","path":"/data/homebrew/FAKE78000"}]})";
  CHECK(registry.Query("FAKE78000").state == RegistrationState::kReady);
  response = "{}";
  CHECK(registry.Query("FAKE78000").reason == "status");
  response = "not json";
  CHECK(registry.Query("FAKE78000").reason == "bad-json");
  TitleRegistry offline([](const auto&, const auto&) -> std::optional<std::string> { return {}; });
  CHECK(offline.Query("FAKE78000").state == RegistrationState::kUnavailable);
  CHECK(offline.Query("FAKE78000").reason == "no-response");
  CHECK(offline.DescribeVersion() == "shadowmount=unreachable");
  TitleRegistry versioned([&](const std::string& route, const std::string&) -> std::optional<std::string> {
    return route == "/api/v1/version" ? response : std::string("{}");
  });
  response = R"({"status":0,"api_version":1,"shadowmount_version":"1.7beta2","capabilities":["list_games"]})";
  CHECK(versioned.DescribeVersion() == "shadowmount=1.7beta2 api=1");
  response = R"({"status":0,"shadowmount_version":"1.7 beta\n5"})";
  CHECK(versioned.DescribeVersion() == "shadowmount=1.7?beta?5 api=unknown");
  response = R"({"status":2})";
  CHECK(versioned.DescribeVersion() == "shadowmount=bad-answer");
  response = "{\"status\":0,\"shadowmount_version\":\"" + std::string(100, 'v') + "\"}";
  CHECK(versioned.DescribeVersion() == "shadowmount=" + std::string(64, 'v') + " api=unknown");
  CHECK(!offline.Register("FAKE78000").ok);
  std::puts("title registry tests passed");
}
