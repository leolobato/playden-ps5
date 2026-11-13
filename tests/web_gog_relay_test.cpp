// SPDX-License-Identifier: GPL-3.0-or-later
// GOG sign-in from the phone, through Service::Handle as the server's workers call it: the page, the relay's token
// rules, the exchange outcomes, the rate limit and the checks every post gets.
#include "web/assets.h"
#include "web/service.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace playden::web;
namespace fs = std::filesystem;

#define CHECK(condition)                                                                  \
  do {                                                                                    \
    if (!(condition)) {                                                                   \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
      std::exit(1);                                                                       \
    }                                                                                     \
  } while (false)

namespace {
using Clock = std::chrono::steady_clock;
using Kind = GogExchangeAnswer::Kind;

constexpr const char* kHost = "192.168.1.20:7031";
constexpr const char* kAuthUrl =
    "https://auth.gog.com/auth?client_id=TESTCLIENT&redirect_uri=https%3A%2F%2Fembed.gog.com%2Fon_login_success"
    "%3Forigin%3Dclient&response_type=code&layout=galaxy\"<x>'";
constexpr const char* kPastePrefix = "https://embed.gog.com/on_login_success?origin=client&code=";

struct Answer {
  int status = 0;
  std::string bytes;
  std::string body;
  bool Has(const std::string& text) const { return bytes.find(text) != std::string::npos; }
};

struct Post {
  std::string body;
  std::string client = "192.168.1.50";
  std::string action = "gog-sign-in";
  std::string origin = std::string("http://") + kHost;
  std::string host = kHost;
  bool length = true;
  std::uint64_t declared = 0;  // 0: the body's size
};

Answer Ask(Service& service, Method method, const std::string& path, const Post& post) {
  Request request;
  request.method = method;
  request.method_text = method == Method::kPost ? "POST" : method == Method::kHead ? "HEAD" : "GET";
  request.path = path;
  if (!post.host.empty()) request.headers.push_back({"host", post.host});
  if (!post.origin.empty()) request.headers.push_back({"origin", post.origin});
  if (!post.action.empty()) request.headers.push_back({"x-playden-action", post.action});
  request.client = post.client;
  if (post.length) request.content_length = post.declared ? post.declared : post.body.size();
  auto offset = std::make_shared<std::size_t>(0);
  const std::string body = post.body;
  request.read_body = [body, offset](char* data, std::size_t size) -> std::ptrdiff_t {
    const auto count = std::min(size, body.size() - *offset);
    std::memcpy(data, body.data() + *offset, count);
    *offset += count;
    return static_cast<std::ptrdiff_t>(count);
  };
  Answer answer;
  Response response([&answer](const char* data, std::size_t size) { answer.bytes.append(data, size); return true; },
                    method == Method::kHead);
  service.Handle(request, response);
  answer.status = response.status();
  const auto end = answer.bytes.find("\r\n\r\n");
  if (end != std::string::npos) answer.body = answer.bytes.substr(end + 4);
  return answer;
}

std::string Json(const std::string& token, const std::string& address) {
  return "{\"token\":\"" + token + "\",\"address\":\"" + address + "\"}";
}

// The fake account behind the relay.
struct Stub {
  std::mutex mutex;
  std::condition_variable changed;
  std::atomic<long long> clock_seconds{1000};
  int minted = 0;
  bool random_works = true;
  // What exchange answers next; block: wait for release first.
  GogExchangeAnswer next{Kind::kSignedIn, "Player One"};
  bool block = false;
  bool entered = false;
  bool released = false;
  int exchanges = 0;
  std::string last_code;
  std::uint64_t last_request = 0;
  std::vector<std::string> log;

  GogRelayConfig Config() {
    GogRelayConfig config;
    config.parse_code = [](std::string_view paste) -> std::optional<std::string> {
      const std::string_view prefix = kPastePrefix;
      if (paste.substr(0, prefix.size()) != prefix || paste.size() == prefix.size()) return std::nullopt;
      return std::string(paste.substr(prefix.size()));
    };
    config.exchange = [this](std::string code, std::uint64_t request) {
      std::unique_lock<std::mutex> lock(mutex);
      ++exchanges;
      last_code = code;
      last_request = request;
      entered = true;
      changed.notify_all();
      if (block) changed.wait(lock, [this] { return released; });
      return next;
    };
    config.random_token = [this]() -> std::optional<std::string> {
      std::lock_guard<std::mutex> lock(mutex);
      if (!random_works) return std::nullopt;
      char text[40];
      std::snprintf(text, sizeof text, "%032x", 0xab0000 + ++minted);
      return std::string(text);
    };
    config.now = [this] { return Clock::time_point(std::chrono::seconds(clock_seconds.load())); };
    config.log = [this](const std::string& line) {
      std::lock_guard<std::mutex> lock(mutex);
      log.push_back(line);
    };
    return config;
  }
  void Block() {
    std::lock_guard<std::mutex> lock(mutex);
    block = true;
    entered = released = false;
  }
  void WaitEntered() {
    std::unique_lock<std::mutex> lock(mutex);
    CHECK(changed.wait_for(lock, std::chrono::seconds(5), [this] { return entered; }));
  }
  void Release() {
    std::lock_guard<std::mutex> lock(mutex);
    released = true;
    block = false;
    changed.notify_all();
  }
};

std::string ExtractToken(const std::string& body) {
  const std::string key = "\"token\":\"";
  const auto at = body.find(key);
  if (at == std::string::npos) return {};
  return body.substr(at + key.size(), 32);
}

ServiceConfig BaseConfig(const fs::path& root) {
  ServiceConfig config;
  config.paths = {(root / "logs").string(), (root / "homebrew").string(), (root / "kits").string(),
                  (root / "homebrew/FAKE77031").string()};
  config.server.first_port = config.server.last_port = 0;
  config.server.loopback_only = true;
  return config;
}
}  // namespace

int main() {
  const fs::path root = fs::canonical(fs::temp_directory_path()) / ("playden-web-gog-relay-" + std::to_string(::getpid()));
  fs::remove_all(root);
  fs::create_directories(root / "logs");

  // The embedded page holds no client id of its own: only the placeholder the server fills in.
  const auto* asset = FindEmbeddedAsset("/gog");
  CHECK(asset != nullptr);
  const std::string raw(reinterpret_cast<const char*>(asset->data), asset->size);
  CHECK(raw.find("client_id") == std::string::npos);
  CHECK(raw.find("%%GOG_AUTH_URL%%") != std::string::npos);
  CHECK(raw.find("%%GOG_AUTH_URL%%") == raw.rfind("%%GOG_AUTH_URL%%"));

  {
    // No relay: neither route exists.
    Service service(BaseConfig(root));
    Post get;
    CHECK(Ask(service, Method::kGet, "/gog", get).status == 404);
    Post post;
    post.body = Json(std::string(32, 'a'), std::string(kPastePrefix) + "abc");
    CHECK(Ask(service, Method::kPost, "/api/gog/sign-in", post).status == 404);
    CHECK(Ask(service, Method::kGet, "/", get).status == 200);
  }

  Stub stub;
  auto relay = std::make_shared<GogRelay>(stub.Config());
  auto config = BaseConfig(root);
  config.gog = relay;
  config.gog_auth_url = kAuthUrl;
  Service service(config);
  int next_client = 1;
  const auto fresh_client = [&next_client] { return "192.168.7." + std::to_string(next_client++); };
  const auto send = [&](const std::string& token, const std::string& address, const std::string& client) {
    Post post;
    post.body = Json(token, address);
    post.client = client;
    return Ask(service, Method::kPost, "/api/gog/sign-in", post);
  };
  const std::string good = std::string(kPastePrefix) + "CodeOne_1234567890";

  // The page: the auth address attribute-escaped, no token anywhere, never cached, no referrer.
  {
    const auto token = relay->Open(7);
    CHECK(token && token->size() == 32);
    const auto page = Ask(service, Method::kGet, "/gog", Post{});
    CHECK(page.status == 200);
    CHECK(page.Has("Content-Type: text/html; charset=utf-8\r\n"));
    CHECK(page.Has("Cache-Control: no-store\r\n"));
    CHECK(page.Has("Referrer-Policy: no-referrer\r\n"));
    CHECK(page.body.find("href=\"https://auth.gog.com/auth?client_id=TESTCLIENT&amp;redirect_uri=https%3A%2F%2Fembed.gog.com"
                         "%2Fon_login_success%3Forigin%3Dclient&amp;response_type=code&amp;layout=galaxy&quot;&lt;x&gt;&#39;\"") !=
          std::string::npos);
    CHECK(page.body.find("%%GOG_AUTH_URL%%") == std::string::npos);
    CHECK(page.body.find(*token) == std::string::npos);
    CHECK(page.body.find("<meta name=\"referrer\" content=\"no-referrer\">") != std::string::npos);
    CHECK(page.body.find("rel=\"noopener noreferrer\"") != std::string::npos);
    // Never under /assets/.
    CHECK(Ask(service, Method::kGet, "/assets/gog", Post{}).status == 404);
    CHECK(Ask(service, Method::kGet, "/assets/gog.html", Post{}).status == 404);
    CHECK(Ask(service, Method::kHead, "/gog", Post{}).status == 200);
  }

  // A good post: signed in, with the code and the TV's request; then the token is spent.
  {
    const auto token = *relay->Open(41);
    CHECK(relay->Token() == token && relay->request() == 41);
    const auto client = fresh_client();
    stub.next = {Kind::kSignedIn, "Player \"One\""};
    const auto answer = send(token, good, client);
    CHECK(answer.status == 200);
    CHECK(answer.Has("Content-Type: application/json\r\n") && answer.Has("Cache-Control: no-store\r\n"));
    CHECK(answer.body == "{\"signed_in\":true,\"name\":\"Player \\\"One\\\"\"}");
    CHECK(stub.last_code == "CodeOne_1234567890" && stub.last_request == 41);
    CHECK(!relay->Token() && !relay->Expired());
    CHECK(send(token, good, client).status == 410);
  }

  // A wrong token: 403, and the right one still works.
  {
    const auto token = *relay->Open(42);
    const auto client = fresh_client();
    const auto exchanges = stub.exchanges;
    const auto wrong = send(std::string(32, '0'), good, client);
    CHECK(wrong.status == 403 && wrong.body.find("older sign-in") != std::string::npos);
    CHECK(send(token.substr(0, 31), good, client).status == 403);
    CHECK(stub.exchanges == exchanges);
    CHECK(relay->Token() == token);
    stub.next = {Kind::kSignedIn, "P"};
    CHECK(send(token, good, client).status == 200);
  }

  // The same token while the first exchange still runs: 409 at once.
  {
    const auto token = *relay->Open(43);
    const auto client = fresh_client();
    stub.next = {Kind::kSignedIn, "P"};
    stub.Block();
    Answer first;
    std::thread worker([&] { first = send(token, good, client); });
    stub.WaitEntered();
    CHECK(!relay->Token());  // spent: the TV draws no code while it is exchanged
    CHECK(!relay->Expired());  // but its sign-in is not over
    const auto second = send(token, good, client);
    CHECK(second.status == 409 && second.body.find("already signing in") != std::string::npos);
    stub.Release();
    worker.join();
    CHECK(first.status == 200);
  }

  // Expired after 10 minutes: the TV sees it (Expired) before and after a post finds it.
  {
    const auto token = *relay->Open(44);
    CHECK(!relay->Expired());
    stub.clock_seconds += 10 * 60 + 1;
    CHECK(!relay->Token() && relay->Expired());
    const auto answer = send(token, good, fresh_client());
    CHECK(answer.status == 410 && answer.body.find("has ended") != std::string::npos);
    CHECK(!relay->Token() && relay->Expired() && !relay->Ended());
    relay->Close();
    CHECK(!relay->Expired());
  }

  // A paste of the wrong shape keeps the token.
  {
    const auto token = *relay->Open(45);
    const auto client = fresh_client();
    const auto exchanges = stub.exchanges;
    const auto wrong = send(token, "https://www.gog.com/en/", client);
    CHECK(wrong.status == 422 && wrong.body.find("isn't the address") != std::string::npos);
    CHECK(wrong.body.find("\"token\"") == std::string::npos);
    CHECK(stub.exchanges == exchanges && relay->Token() == token);
    stub.next = {Kind::kSignedIn, "P"};
    CHECK(send(token, good, client).status == 200);
  }

  // A failed exchange: a new token in the answer and on the TV; it works, and the old one is refused.
  {
    const auto token = *relay->Open(46);
    const auto client = fresh_client();
    stub.next = {Kind::kFailed, ""};
    const auto failed = send(token, good, client);
    CHECK(failed.status == 422 && failed.body.find("didn't work") != std::string::npos);
    const auto renewed = ExtractToken(failed.body);
    CHECK(renewed.size() == 32 && renewed != token);
    CHECK(relay->Token() == renewed && relay->request() == 46);
    CHECK(send(token, good, client).status == 403);
    stub.next = {Kind::kSignedIn, "P"};
    CHECK(send(renewed, std::string(kPastePrefix) + "CodeTwo", client).status == 200);
    CHECK(stub.last_code == "CodeTwo" && stub.last_request == 46);
  }

  // Close (Back) then a post: 410. Closing Playden: 503. The account calling the request stale: 410.
  {
    const auto token = *relay->Open(47);
    relay->Close();
    CHECK(!relay->Token() && relay->request() == 0);
    CHECK(send(token, good, fresh_client()).status == 410);
    CHECK(!relay->Ended() && !relay->Expired());
    // These end the sign-in on the relay's side, and the TV is not told by an account event: Ended says so (not
    // Expired), and the request is kept until the TV closes it.
    const auto again = *relay->Open(48);
    stub.next = {Kind::kClosing, ""};
    CHECK(send(again, good, fresh_client()).status == 503);
    CHECK(!relay->Token() && relay->Ended() && !relay->Expired() && relay->request() == 48);
    const auto stale = *relay->Open(49);
    CHECK(!relay->Ended());
    stub.next = {Kind::kStale, ""};
    CHECK(send(stale, good, fresh_client()).status == 410);
    CHECK(!relay->Token() && relay->Ended() && !relay->Expired() && relay->request() == 49);
    CHECK(send(stale, good, fresh_client()).status == 410);
    relay->Close();
    CHECK(!relay->Ended() && relay->request() == 0);
  }

  // A failed exchange with no randomness left for a new token: the sign-in has ended too.
  {
    const auto token = *relay->Open(57);
    stub.next = {Kind::kFailed, ""};
    stub.random_works = false;
    const auto answer = send(token, good, fresh_client());
    stub.random_works = true;
    CHECK(answer.status == 503 && answer.body.find("can't start") != std::string::npos);
    CHECK(!relay->Token() && relay->Ended() && !relay->Expired() && relay->request() == 57);
    relay->Close();
    CHECK(!relay->Ended());
  }

  // Busy and timeout: the request takes another code with a new token.
  {
    auto token = *relay->Open(50);
    const auto client = fresh_client();
    stub.next = {Kind::kTimeout, ""};
    auto answer = send(token, good, client);
    CHECK(answer.status == 504 && answer.body.find("still working") != std::string::npos);
    token = ExtractToken(answer.body);
    CHECK(relay->Token() == token);
    stub.next = {Kind::kBusy, ""};
    answer = send(token, good, client);
    CHECK(answer.status == 409);
    CHECK(relay->Token() == ExtractToken(answer.body));
    relay->Close();
  }

  // Back while the exchange runs: a failure is 410 and mints nothing; a sign-in still answers 200.
  {
    const auto token = *relay->Open(51);
    stub.next = {Kind::kFailed, ""};
    stub.Block();
    Answer first;
    std::thread worker([&] { first = send(token, good, fresh_client()); });
    stub.WaitEntered();
    relay->Close();
    stub.Release();
    worker.join();
    CHECK(first.status == 410 && first.body.find("\"token\"") == std::string::npos);
    CHECK(!relay->Token());

    const auto signing = *relay->Open(52);
    stub.next = {Kind::kSignedIn, "P"};
    stub.Block();
    std::thread signer([&] { first = send(signing, good, fresh_client()); });
    stub.WaitEntered();
    relay->Close();  // the TV closes the relay on the sign-in event, before the post is answered
    stub.Release();
    signer.join();
    CHECK(first.status == 200);
  }

  // A new sign-in screen while an old exchange runs: the old failure doesn't replace the new token.
  {
    const auto old_token = *relay->Open(53);
    stub.next = {Kind::kFailed, ""};
    stub.Block();
    Answer first;
    std::thread worker([&] { first = send(old_token, good, fresh_client()); });
    stub.WaitEntered();
    const auto newer = *relay->Open(54);
    stub.Release();
    worker.join();
    CHECK(first.status == 410);
    CHECK(relay->Token() == newer && relay->request() == 54);
    relay->Close();
  }

  // No randomness: Open gives nothing and the relay stays closed.
  {
    stub.random_works = false;
    CHECK(!relay->Open(55));
    CHECK(!relay->Token());
    stub.random_works = true;
  }

  // Rate limit: ten posts a minute per address, good or bad; another address still gets through.
  {
    const auto token = *relay->Open(56);
    const std::string a = "192.168.9.1";
    for (int i = 0; i < 10; ++i) CHECK(send(std::string(32, 'f'), good, a).status == 403);
    const auto limited = send(token, good, a);
    CHECK(limited.status == 429 && limited.body.find("Too many tries") != std::string::npos);
    stub.next = {Kind::kFailed, ""};
    const auto other = send(token, good, "192.168.9.2");
    CHECK(other.status == 422);
    const auto renewed = ExtractToken(other.body);
    stub.clock_seconds += 30;
    CHECK(send(renewed, good, a).status == 429);
    stub.clock_seconds += 31;
    stub.next = {Kind::kSignedIn, "P"};
    CHECK(send(renewed, good, a).status == 200);
  }

  // Every post gets the action header, Origin and Host checks, a length and a small JSON body.
  {
    const auto token = *relay->Open(57);
    Post post;
    post.body = Json(token, good);
    post.client = fresh_client();
    auto bad = post;
    bad.action.clear();
    CHECK(Ask(service, Method::kPost, "/api/gog/sign-in", bad).status == 403);
    bad.action = "files";
    CHECK(Ask(service, Method::kPost, "/api/gog/sign-in", bad).status == 403);
    bad = post;
    bad.origin = "http://evil.example";
    CHECK(Ask(service, Method::kPost, "/api/gog/sign-in", bad).status == 403);
    bad = post;
    bad.host = "playden.example:7031";
    bad.origin = "http://playden.example:7031";
    CHECK(Ask(service, Method::kPost, "/api/gog/sign-in", bad).status == 403);
    bad = post;
    bad.length = false;
    CHECK(Ask(service, Method::kPost, "/api/gog/sign-in", bad).status == 411);
    bad = post;
    bad.body = std::string(5000, ' ');
    CHECK(Ask(service, Method::kPost, "/api/gog/sign-in", bad).status == 413);
    for (const std::string body : {std::string("not json"), std::string("{\"token\":\"") + token + "\"}",
                                   std::string("{\"address\":\"x\"}"), std::string("{\"token\":1,\"address\":\"x\"}"),
                                   std::string("[]"), Json(token, "")}) {
      bad = post;
      bad.body = body;
      CHECK(Ask(service, Method::kPost, "/api/gog/sign-in", bad).status == 400);
    }
    // A body cut short.
    bad = post;
    bad.declared = post.body.size() + 10;
    CHECK(Ask(service, Method::kPost, "/api/gog/sign-in", bad).status == 400);
    // GET on the action.
    CHECK(Ask(service, Method::kGet, "/api/gog/sign-in", post).status == 405);
    // None of those reached the relay: the token is still good.
    CHECK(relay->Token() == token);
    stub.next = {Kind::kSignedIn, "P"};
    CHECK(Ask(service, Method::kPost, "/api/gog/sign-in", post).status == 200);
  }

  // The relay's log has outcome words only: no token, paste or code.
  {
    std::lock_guard<std::mutex> lock(stub.mutex);
    CHECK(!stub.log.empty());
    for (const auto& line : stub.log) {
      CHECK(line.rfind("gog relay result=", 0) == 0);
      for (const char c : line.substr(std::strlen("gog relay result=")))
        CHECK((c >= 'a' && c <= 'z') || c == '_');
      CHECK(line.find("Code") == std::string::npos && line.find("embed.gog.com") == std::string::npos);
    }
  }

  fs::remove_all(root);
  std::puts("web_gog_relay: ok");
  return 0;
}
