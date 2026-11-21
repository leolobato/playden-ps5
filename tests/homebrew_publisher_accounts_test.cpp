// homebrew_publisher_accounts_test.cpp - The phone sign-in of publisher accounts, over loopback.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: MIT
#include "homebrew/publisher_accounts.hpp"

#include <arpa/inet.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <netinet/in.h>
#include <optional>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#define CHECK(condition)                                                                  \
  do {                                                                                    \
    if (!(condition)) {                                                                   \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
      std::exit(1);                                                                       \
    }                                                                                     \
  } while (false)

using namespace playden::homebrew;
using playden::runtime::PhoneRelayAnswer;
using playden::runtime::Publisher;

namespace {

std::string Exchange(std::uint16_t port, const std::string& request) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    ::close(fd);
    return {};
  }
  ::send(fd, request.data(), request.size(), 0);
  std::string answer;
  char buffer[4096];
  for (ssize_t got; (got = ::recv(fd, buffer, sizeof(buffer), 0)) > 0;) answer.append(buffer, got);
  ::close(fd);
  return answer;
}

std::uint16_t PortOf(const std::string& address) {
  const auto colon = address.rfind(':');
  return static_cast<std::uint16_t>(std::stoi(address.substr(colon + 1, address.find('/', colon) - colon - 1)));
}

std::string PathOf(const std::string& address) { return address.substr(address.find('/', address.find("//") + 2)); }

std::string Get(const std::string& address, const std::string& suffix = {}) {
  return Exchange(PortOf(address), "GET " + PathOf(address) + suffix + " HTTP/1.1\r\nHost: x\r\n\r\n");
}

std::string Post(const std::string& address, const std::string& body) {
  return Exchange(PortOf(address), "POST " + PathOf(address) + "/credential HTTP/1.1\r\nHost: x\r\n"
                                   "Content-Type: application/json\r\nContent-Length: " +
                                       std::to_string(body.size()) + "\r\n\r\n" + body);
}

bool Has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

PublisherAccounts::Config Loopback(int* submissions, std::optional<Publisher>* seen) {
  PublisherAccounts::Config config;
  config.lan_address = [] { return std::optional<std::string>("127.0.0.1"); };
  config.port = 18199;
  config.now_unix = [] { return std::int64_t{1'800'000'000}; };
  config.read_page = [](const std::string& path) -> std::optional<std::string> {
    if (path == "/pack/signin/index.html") return std::string("<html>pack page</html>");
    return std::nullopt;
  };
  config.submit = [submissions, seen](Publisher publisher, std::string credential) {
    ++*submissions;
    *seen = publisher;
    PhoneRelayAnswer answer;
    answer.ok = credential == "{\"token\":\"good\"}";
    answer.display = answer.ok ? "Player One" : "";
    answer.message = answer.ok ? "" : "That sign-in has no token.";
    return answer;
  };
  return config;
}

void FollowsOneSignIn() {
  int submissions = 0;
  std::optional<Publisher> seen;
  std::string logged;
  auto config = Loopback(&submissions, &seen);
  config.log = [&logged](const std::string& line) { logged += line + "\n"; };
  PublisherAccounts accounts(std::move(config));
  CHECK(accounts.Current().state == PublisherSignInState::Idle);

  accounts.Begin(Publisher::kRockstar, "Rockstar Games", "/pack/signin/index.html");
  auto view = accounts.Current();
  CHECK(view.slug == "rockstar" && view.state == PublisherSignInState::Waiting);
  CHECK(view.address.rfind("http://127.0.0.1:", 0) == 0 && PathOf(view.address).size() == 6);
  CHECK(view.expires_at == 1'800'000'600);

  // The pack's page; the paste page names the publisher.
  CHECK(Has(Get(view.address), "<html>pack page</html>"));
  CHECK(accounts.Current().state == PublisherSignInState::Connected);
  CHECK(Has(Get(view.address, "/paste"), "Sign in to Rockstar Games"));

  CHECK(Has(Post(view.address, "{\"token\":\"bad\"}"), "That sign-in has no token."));
  view = accounts.Current();
  CHECK(view.state == PublisherSignInState::Refused && view.message == "That sign-in has no token.");
  CHECK(Has(Post(view.address, "{\"token\":\"good\"}"), "\"display\":\"Player One\""));
  view = accounts.Current();
  CHECK(view.state == PublisherSignInState::SignedIn && view.display == "Player One");
  CHECK(submissions == 2 && seen == Publisher::kRockstar);

  accounts.Cancel();
  CHECK(accounts.Current().state == PublisherSignInState::Idle && accounts.Current().address.empty());
  // The log names the publisher, never the page's address or what the phone sent.
  CHECK(Has(logged, "publisher sign-in rockstar page open (pack page)"));
  CHECK(!Has(logged, PathOf(view.address)) && !Has(logged, "token"));
}

void ANewCodeClosesTheOldPage() {
  int submissions = 0;
  std::optional<Publisher> seen;
  PublisherAccounts accounts(Loopback(&submissions, &seen));
  accounts.Begin(Publisher::kRockstar, "Rockstar Games", "");
  const auto first = accounts.Current().address;
  CHECK(Has(Get(first), "<textarea"));  // no pack page: the paste page answers
  accounts.Begin(Publisher::kRockstar, "Rockstar Games", "/missing.html");
  const auto second = accounts.Current().address;
  CHECK(second != first);
  CHECK(Has(Get(second), "<textarea"));  // an unreadable pack page: the paste page answers
  // The first page closes within its poll interval.
  bool closed = false;
  for (int i = 0; i < 50 && !closed; ++i) {
    const auto answer = Get(first);
    closed = answer.empty() || Has(answer, " 404 ");
    if (!closed) std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  CHECK(closed);
  CHECK(submissions == 0);
}

void ExpiresAfterItsLifetime() {
  int submissions = 0;
  std::optional<Publisher> seen;
  auto config = Loopback(&submissions, &seen);
  config.lifetime_ms = 300;
  PublisherAccounts accounts(std::move(config));
  accounts.Begin(Publisher::kRockstar, "Rockstar Games", "");
  CHECK(accounts.Current().state == PublisherSignInState::Waiting);
  bool expired = false;
  for (int i = 0; i < 100 && !expired; ++i) {
    expired = accounts.Current().state == PublisherSignInState::Expired;
    if (!expired) std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  CHECK(expired && accounts.Current().address.empty());
}

void WithoutANetworkNothingOpens() {
  int submissions = 0;
  std::optional<Publisher> seen;
  auto config = Loopback(&submissions, &seen);
  config.lan_address = [] { return std::optional<std::string>(); };
  PublisherAccounts accounts(std::move(config));
  accounts.Begin(Publisher::kRockstar, "Rockstar Games", "");
  const auto view = accounts.Current();
  CHECK(view.state == PublisherSignInState::NoNetwork && view.address.empty() && view.slug == "rockstar");
}

void ACheckStillRunningEndsBeforeShutdown() {
  // Cancel returns at once while the phone's credential is still being checked; the service
  // waits for that check when it goes away.
  int submissions = 0;
  std::optional<Publisher> seen;
  auto config = Loopback(&submissions, &seen);
  config.submit = [&submissions](Publisher, std::string) {
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    ++submissions;
    return PhoneRelayAnswer{true, "Player One", {}};
  };
  std::string address;
  std::thread phone;
  {
    PublisherAccounts accounts(std::move(config));
    accounts.Begin(Publisher::kRockstar, "Rockstar Games", "");
    address = accounts.Current().address;
    phone = std::thread([address] { Post(address, "{\"token\":\"good\"}"); });
    for (int i = 0; i < 100 && accounts.Current().state != PublisherSignInState::Checking; ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const auto before = std::chrono::steady_clock::now();
    accounts.Cancel();
    CHECK(std::chrono::steady_clock::now() - before < std::chrono::milliseconds(200));
    CHECK(accounts.Current().state == PublisherSignInState::Idle);
  }
  CHECK(submissions == 1);
  phone.join();
}

}  // namespace

// The sign-in page is read whole, up to 1 MiB; a missing or larger file gives nothing.
void ReadsTheSignInPage() {
  char pattern[] = "/tmp/playden-publisher-page-XXXXXX";
  CHECK(::mkdtemp(pattern));
  const std::filesystem::path root = pattern;
  const auto write = [](const std::filesystem::path& path, const std::string& bytes) {
    FILE* file = std::fopen(path.c_str(), "wb");
    CHECK(file && std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size() && std::fclose(file) == 0);
  };
  const std::string page = "<html>" + std::string(40000, 'x') + "</html>";
  write(root / "page.html", page);
  CHECK(ReadPublisherSignInPage((root / "page.html").string()) == page);
  write(root / "empty.html", "");
  CHECK(ReadPublisherSignInPage((root / "empty.html").string()) == std::string());
  write(root / "large.html", std::string(1024 * 1024 + 1, 'x'));
  CHECK(!ReadPublisherSignInPage((root / "large.html").string()));
  CHECK(!ReadPublisherSignInPage((root / "missing.html").string()));
  std::filesystem::remove_all(root);
}

int main() {
  ReadsTheSignInPage();
  FollowsOneSignIn();
  ANewCodeClosesTheOldPage();
  ExpiresAfterItsLifetime();
  WithoutANetworkNothingOpens();
  ACheckStillRunningEndsBeforeShutdown();
  std::puts("homebrew_publisher_accounts: ok");
  return 0;
}
