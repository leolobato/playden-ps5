// SPDX-License-Identifier: GPL-3.0-or-later
#include "web/service.h"

#include <arpa/inet.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
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
std::string Get(std::uint16_t port, const std::string& head) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (fd < 0 || ::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof address) != 0) {
    if (fd >= 0) ::close(fd);
    return {};
  }
  const std::string request = head + " HTTP/1.1\r\nHost: x\r\n\r\n";
  CHECK(::send(fd, request.data(), request.size(), 0) == static_cast<ssize_t>(request.size()));
  std::string answer;
  char buffer[8192];
  ssize_t count;
  while ((count = ::recv(fd, buffer, sizeof buffer, 0)) > 0) answer.append(buffer, static_cast<std::size_t>(count));
  ::close(fd);
  return answer;
}

std::string Body(const std::string& answer) {
  const auto end = answer.find("\r\n\r\n");
  return end == std::string::npos ? std::string() : answer.substr(end + 4);
}

void Put(const fs::path& path, const std::string& text) {
  fs::create_directories(path.parent_path());
  FILE* file = std::fopen(path.c_str(), "wb");
  CHECK(file != nullptr);
  std::fwrite(text.data(), 1, text.size(), file);
  std::fclose(file);
}

ServiceSnapshot WaitFor(const Service& service, ServiceSnapshot::State state) {
  for (int i = 0; i < 400; ++i) {
    const auto snapshot = service.Snapshot();
    if (snapshot.state == state) return snapshot;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  CHECK(!"the service did not reach the state");
  return {};
}
}  // namespace

int main() {
  const fs::path root = fs::canonical(fs::temp_directory_path()) / ("playden-web-service-" + std::to_string(::getpid()));
  fs::remove_all(root);
  Put(root / "logs/steam.log", "signed in as Bob (76561198000000002)\n");
  Put(root / "logs/title-FAKE78012.log",
      "PW_REPORT/1 profile=playden-steam-1332010 time=300\n(L\"SteamUser\" L\"Alice\") C:/users/Alice\n");
  Put(root / "homebrew/FAKE78012/sce_sys/param.json",
      R"json({"localizedParameters":{"defaultLanguage":"en-US","en-US":{"titleName":"Stray (Steam)"}}})json");
  Put(root / "homebrew/FAKE77031/playden-version", "0.9.0\n");

  std::mutex lines_mutex;
  std::vector<std::string> lines;
  ServiceConfig config;
  config.paths = {(root / "logs").string(), (root / "homebrew").string(), (root / "kits").string(),
                  (root / "homebrew/FAKE77031").string()};
  config.server.first_port = config.server.last_port = 0;
  config.server.loopback_only = true;
  config.server.poll_interval_ms = 20;
  config.server.read_timeout_ms = 1000;
  config.server.log = [&](const std::string& line) { std::lock_guard<std::mutex> lock(lines_mutex); lines.push_back(line); };
  config.now = [] { return std::int64_t{1791601941}; };
  Service service(config);
  CHECK(service.Snapshot().state == ServiceSnapshot::State::kOff);
  service.SetAccount({"Bob", 76561198000000002ull});
  service.SetEnabled(true);
  const auto on = WaitFor(service, ServiceSnapshot::State::kOn);
  CHECK(on.port != 0 && on.error.empty());
  CHECK(on.url.empty() == on.address.empty());
  if (!on.address.empty()) CHECK(on.url == "http://" + on.address + ":" + std::to_string(on.port) + "/");

  // The page and its fonts come from the binary; nothing else under /assets/.
  auto answer = Get(on.port, "GET /");
  CHECK(answer.rfind("HTTP/1.1 200 OK\r\n", 0) == 0 && answer.find("Content-Type: text/html; charset=utf-8\r\n") != std::string::npos);
  CHECK(Body(answer).find("Download report (.zip)") != std::string::npos);
  CHECK(Body(answer).find("https://") == std::string::npos);  // works with no internet
  answer = Get(on.port, "GET /assets/barlow-regular.ttf");
  CHECK(answer.find("Content-Type: font/ttf\r\n") != std::string::npos && Body(answer).size() == 104068);
  CHECK(Get(on.port, "GET /assets/barlow-semibold.ttf").rfind("HTTP/1.1 200", 0) == 0);
  CHECK(Get(on.port, "GET /assets/../logs/steam.log").rfind("HTTP/1.1 404", 0) == 0);
  CHECK(Get(on.port, "GET /assets/index.html").rfind("HTTP/1.1 404", 0) == 0);

  // JSON routes, never cached.
  answer = Get(on.port, "GET /api/status");
  CHECK(answer.find("Cache-Control: no-store\r\n") != std::string::npos);
  CHECK(Body(answer).find("\"version\":\"0.9.0\"") != std::string::npos);
  CHECK(Body(answer).find("\"collected\":\"2026-10-10 03:12 UTC\"") != std::string::npos);
  answer = Get(on.port, "GET /api/logs");
  CHECK(Body(answer).find("{\"title\":\"FAKE78012\",\"app\":1332010,\"name\":\"Stray\",\"last_run\":300,") != std::string::npos);
  CHECK(Body(answer).find("{\"name\":\"steam.log\",") != std::string::npos);

  // One log, redacted with the account Playden is signed in with.
  answer = Get(on.port, "GET /api/logs/steam.log");
  CHECK(answer.find("Content-Type: text/plain; charset=utf-8\r\n") != std::string::npos);
  CHECK(Body(answer) == "signed in as <steam-user> (<steam-id>)\n");
  CHECK(Body(Get(on.port, "GET /api/logs/title-FAKE78012.log")).find("Alice") == std::string::npos);
  CHECK(Get(on.port, "GET /api/logs/..%2Fhomebrew%2FFAKE77031%2Fplayden-version").rfind("HTTP/1.1 404", 0) == 0);
  CHECK(Get(on.port, "POST /api/logs").rfind("HTTP/1.1 405", 0) == 0);

  // The report: a download with its name; HEAD has no body.
  answer = Get(on.port, "HEAD /api/report.zip?title=FAKE78012");
  CHECK(answer.find("Content-Disposition: attachment; filename=\"playden-logs-20261010-031221.zip\"\r\n") != std::string::npos);
  CHECK(Body(answer).empty());
  answer = Get(on.port, "GET /api/report.zip?title=FAKE78012");
  CHECK(answer.find("Content-Type: application/zip\r\n") != std::string::npos);
  CHECK(answer.find("Content-Length:") == std::string::npos);
  const auto zip = Body(answer);
  CHECK(zip.rfind("PK\x03\x04", 0) == 0 && zip.find("logs/title-FAKE78012.log") != std::string::npos);
  CHECK(zip.find("Alice") == std::string::npos && zip.find("Bob") == std::string::npos);
  {
    std::lock_guard<std::mutex> lock(lines_mutex);
    bool logged = false;
    for (const auto& line : lines) logged = logged || line == "GET /api/report.zip?title=FAKE78012 200 " + std::to_string(zip.size()) + " 127.0.0.1";
    CHECK(logged);
  }

  // Quit: POST with the header only; the caller takes the request once.
  CHECK(Get(on.port, "GET /api/quit").rfind("HTTP/1.1 405", 0) == 0);
  CHECK(Get(on.port, "POST /api/quit").rfind("HTTP/1.1 403", 0) == 0 && !service.TakeQuitRequest());
  {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(on.port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0);
    const std::string request = "POST /api/quit HTTP/1.1\r\nX-Playden-Action: quit\r\nContent-Length: 0\r\n\r\n";
    CHECK(::send(fd, request.data(), request.size(), 0) == static_cast<ssize_t>(request.size()));
    std::string answer;
    char buffer[1024];
    ssize_t count;
    while ((count = ::recv(fd, buffer, sizeof buffer, 0)) > 0) answer.append(buffer, static_cast<std::size_t>(count));
    ::close(fd);
    CHECK(answer.rfind("HTTP/1.1 202 Accepted\r\n", 0) == 0 && Body(answer) == "{\"quitting\":true}");
  }
  CHECK(service.TakeQuitRequest() && !service.TakeQuitRequest());

  // Off closes the port; on again opens it.
  service.SetEnabled(false);
  WaitFor(service, ServiceSnapshot::State::kOff);
  CHECK(Get(on.port, "GET /").empty());
  service.SetEnabled(true);
  const auto again = WaitFor(service, ServiceSnapshot::State::kOn);
  CHECK(Get(again.port, "GET /api/status").rfind("HTTP/1.1 200", 0) == 0);

  // A port another program holds: the service says why, and tries again only when turned off and on.
  {
    Server holder;
    std::string error;
    ServerOptions hold;
    hold.first_port = hold.last_port = 0;
    hold.loopback_only = true;
    CHECK(holder.Start(hold, [](const Request&, Response& response) { response.Send(200, "text/plain", "x"); }, &error));
    ServiceConfig busy = config;
    busy.server.first_port = busy.server.last_port = holder.port();
    Service refused(busy);
    refused.SetEnabled(true);
    const auto failed = WaitFor(refused, ServiceSnapshot::State::kFailed);
    CHECK(failed.error.find("port " + std::to_string(holder.port())) == 0 && failed.url.empty());
    holder.Stop();
    refused.SetEnabled(false);
    WaitFor(refused, ServiceSnapshot::State::kOff);
    refused.SetEnabled(true);
    WaitFor(refused, ServiceSnapshot::State::kOn);
    refused.Shutdown();
  }

  service.Shutdown();
  service.Shutdown();
  CHECK(Get(again.port, "GET /").empty());
  fs::remove_all(root);
  std::puts("web_service: ok");
}
