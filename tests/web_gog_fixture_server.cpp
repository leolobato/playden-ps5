// SPDX-License-Identifier: GPL-3.0-or-later
// A real web server with a stub GOG account, for tests/test_web_gog_relay_smoke.py:
// web_gog_fixture_server <empty folder>. Writes the server's and the relay's log lines to <folder>/web.log, as Playden
// writes web.log, and prints "port=<n>", then "token=<the sign-in token>". The code "FixtureGoodCode" signs in as
// "Fixture Player"; any other code fails, which gives the page a new token.
#include "web/service.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

using namespace playden::web;
namespace fs = std::filesystem;

int main(int argc, char** argv) {
  if (argc < 2) return 2;
  fs::create_directories(argv[1]);
  const auto base = fs::canonical(argv[1]);
  FILE* web_log = std::fopen((base / "web.log").c_str(), "ab");
  if (!web_log) return 1;
  auto log_mutex = std::make_shared<std::mutex>();
  const auto log = [web_log, log_mutex](const std::string& line) {
    std::lock_guard<std::mutex> lock(*log_mutex);
    std::fprintf(web_log, "%s\n", line.c_str());
    std::fflush(web_log);
  };

  GogRelayConfig relay_config;
  relay_config.parse_code = [](std::string_view paste) -> std::optional<std::string> {
    constexpr std::string_view prefix = "https://embed.gog.com/on_login_success?origin=client&code=";
    if (paste.substr(0, prefix.size()) != prefix || paste.size() == prefix.size()) return std::nullopt;
    return std::string(paste.substr(prefix.size()));
  };
  relay_config.exchange = [](std::string code, std::uint64_t request) {
    GogExchangeAnswer answer;
    if (code == "FixtureGoodCode" && request == 9) {
      answer.kind = GogExchangeAnswer::Kind::kSignedIn;
      answer.name = "Fixture Player";
    }
    return answer;
  };
  relay_config.log = log;
  auto relay = std::make_shared<GogRelay>(relay_config);

  ServiceConfig config;
  config.paths = {(base / "logs").string(), (base / "homebrew").string(), (base / "kits").string(), (base / "app").string()};
  fs::create_directories(base / "logs");
  config.server.first_port = config.server.last_port = 0;
  config.server.loopback_only = true;
  config.server.log = log;
  config.gog = relay;
  config.gog_auth_url = "https://auth.gog.com/auth?client_id=FIXTURE&response_type=code";
  Service service(std::move(config));
  service.SetEnabled(true);
  const auto token = relay->Open(9);
  if (!token) return 1;
  bool reported = false;
  while (true) {
    const auto snapshot = service.Snapshot();
    if (!reported && snapshot.state == ServiceSnapshot::State::kOn) {
      std::printf("port=%u\ntoken=%s\n", snapshot.port, token->c_str());
      std::fflush(stdout);
      reported = true;
    }
    if (snapshot.state == ServiceSnapshot::State::kFailed) {
      std::fprintf(stderr, "%s\n", snapshot.error.c_str());
      return 1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
}
