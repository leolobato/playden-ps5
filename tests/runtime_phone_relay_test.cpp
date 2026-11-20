#include "runtime/phone_relay.h"

#include <arpa/inet.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <netinet/in.h>
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

using namespace playden::runtime;

namespace {

// One raw HTTP exchange with the relay; returns the whole answer ("" when refused).
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

std::string Get(std::uint16_t port, const std::string& path) {
  return Exchange(port, "GET " + path + " HTTP/1.1\r\nHost: x\r\n\r\n");
}

std::string Post(std::uint16_t port, const std::string& path, const std::string& body,
                 const std::string& type = "application/json") {
  return Exchange(port, "POST " + path + " HTTP/1.1\r\nHost: x\r\nContent-Type: " + type +
                            "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body);
}

bool Status(const std::string& answer, int code) {
  return answer.rfind("HTTP/1.1 " + std::to_string(code) + " ", 0) == 0;
}

bool Has(const std::string& answer, const std::string& text) { return answer.find(text) != std::string::npos; }

std::uint16_t PortOf(const std::string& address) {
  const auto colon = address.rfind(':');
  return static_cast<std::uint16_t>(std::stoi(address.substr(colon + 1, address.find('/', colon) - colon - 1)));
}

std::string PathOf(const std::string& address) { return address.substr(address.find('/', address.find("//") + 2)); }

bool WaitFor(const PhoneRelay& relay, bool running) {
  for (int i = 0; i < 100; ++i) {
    if (relay.Running() == running) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return false;
}

void ServesThePackPageAndTakesOneCredential() {
  int calls = 0;
  std::string received;
  PhoneRelay relay;
  std::string reason;
  CHECK(relay.Start("127.0.0.1", 18099, {"Sign in to Example <b>", "<html>pack page</html>"},
                    [&](std::string credential) {
                      ++calls;
                      received = credential;
                      PhoneRelayAnswer answer;
                      answer.ok = credential.find("\"good\"") != std::string::npos;
                      answer.display = answer.ok ? "Player \"One\" <x>" : "";
                      answer.message = answer.ok ? "" : "That sign-in has no token.";
                      return answer;
                    },
                    &reason));
  CHECK(relay.Running());
  CHECK(relay.Status().phase == PhoneRelayPhase::kWaiting);
  const auto address = relay.Address();
  CHECK(address.rfind("http://127.0.0.1:", 0) == 0);
  const auto port = PortOf(address);
  const auto path = PathOf(address);
  // A code a person types: five letters and digits, none that look alike.
  CHECK(path.size() == 1 + 5);
  for (const char c : path.substr(1))
    CHECK(std::string("23456789ABCDEFGHJKMNPQRSTUVWXYZ").find(c) != std::string::npos);
  std::string lower = path;
  for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  const std::string wrong = path.substr(0, 5) + (path[5] == '2' ? "3" : "2");

  // Wrong or missing code: nothing, and the page stays unvisited.
  CHECK(Status(Get(port, "/"), 404));
  CHECK(Status(Get(port, wrong), 404));
  CHECK(Status(Get(port, path + "/other"), 404));
  CHECK(Status(Post(port, wrong + "/credential", "{\"x\":\"good\"}"), 404));
  CHECK(relay.Status().phase == PhoneRelayPhase::kWaiting);
  CHECK(calls == 0);

  // The pack's page as is, never cached; opening it is "Phone connected".
  const auto page = Get(port, path);
  CHECK(Status(page, 200));
  CHECK(Has(page, "Content-Type: text/html"));
  CHECK(Has(page, "Cache-Control: no-store"));
  CHECK(Has(page, "Referrer-Policy: no-referrer"));
  CHECK(Has(page, "\r\n\r\n<html>pack page</html>"));
  CHECK(relay.Status().phase == PhoneRelayPhase::kConnected);
  // A query string is ignored, and the code may be typed in any case.
  CHECK(Has(Get(port, path + "?from=qr"), "pack page"));
  CHECK(Has(Get(port, lower), "pack page"));

  // Playden's paste page, escaped, posting to this code only.
  const auto paste = Get(port, path + "/paste");
  CHECK(Status(paste, 200));
  CHECK(Has(paste, "Sign in to Example &lt;b&gt;"));
  CHECK(Has(paste, "fetch('" + path + "/credential'"));
  CHECK(Has(paste, "<textarea"));
  CHECK(!Has(paste, "http://") && !Has(paste, "https://"));  // no external assets

  // Wrong method, type, size: refused before the callback.
  CHECK(Status(Get(port, path + "/credential"), 405));
  CHECK(Status(Post(port, path, "{}"), 405));
  const auto form = Post(port, path + "/credential", "a=b", "application/x-www-form-urlencoded");
  CHECK(Status(form, 415));
  CHECK(Has(form, "\"ok\":false"));
  CHECK(Status(Post(port, path + "/credential", std::string(16 * 1024 + 1, 'x')), 413));
  CHECK(Status(Exchange(port, "POST " + path + "/credential HTTP/1.1\r\nContent-Type: application/json\r\n\r\n"), 411));
  CHECK(calls == 0);

  // Refused: 4xx with the reason; the page stays open for another try.
  const auto refused = Post(port, path + "/credential", "{\"token\":\"bad\"}", "application/json; charset=utf-8");
  CHECK(Status(refused, 400));
  CHECK(Has(refused, "{\"ok\":false,\"message\":\"That sign-in has no token.\"}"));
  CHECK(calls == 1 && received == "{\"token\":\"bad\"}");
  CHECK(relay.Status().phase == PhoneRelayPhase::kRefused);
  CHECK(relay.Status().message == "That sign-in has no token.");
  CHECK(relay.Running());

  // Accepted: 200 with the name (JSON-escaped), then the nonce is spent and the page closes.
  const auto body = std::string("{\"token\":\"good\"}") + std::string(16 * 1024 - 16, ' ');
  CHECK(body.size() == 16 * 1024);
  const auto accepted = Post(port, path + "/credential", body);
  CHECK(Status(accepted, 200));
  CHECK(Has(accepted, "Content-Type: application/json"));
  CHECK(Has(accepted, "{\"ok\":true,\"display\":\"Player \\\"One\\\" \\u003cx\\u003e\"}"));
  CHECK(calls == 2 && received == body);
  CHECK(relay.Status().phase == PhoneRelayPhase::kAccepted);
  CHECK(relay.Status().display == "Player \"One\" <x>");
  CHECK(WaitFor(relay, false));
  CHECK(Get(port, path).empty());
  CHECK(relay.Status().phase == PhoneRelayPhase::kAccepted);
}

void WithoutAPackPageThePastePageAnswers() {
  PhoneRelay relay;
  std::string reason;
  CHECK(relay.Start("127.0.0.1", 18099, {"Sign in to Example", ""}, [](std::string) { return PhoneRelayAnswer{}; },
                    &reason));
  const auto page = Get(PortOf(relay.Address()), PathOf(relay.Address()));
  CHECK(Status(page, 200));
  CHECK(Has(page, "<textarea"));
  relay.Stop();
  CHECK(!relay.Running());
  CHECK(relay.Status().phase == PhoneRelayPhase::kClosed);
}

void TwoPagesGetTwoPortsAndNonces() {
  PhoneRelay first, second;
  std::string reason;
  const auto none = [](std::string) { return PhoneRelayAnswer{}; };
  CHECK(first.Start("127.0.0.1", 18099, {"A", ""}, none, &reason));
  CHECK(second.Start("127.0.0.1", 18099, {"B", ""}, none, &reason));
  CHECK(PortOf(first.Address()) != PortOf(second.Address()));
  CHECK(PathOf(first.Address()) != PathOf(second.Address()));
}

void ClosesAfterItsLifetime() {
  PhoneRelay relay;
  std::string reason;
  CHECK(relay.Start("127.0.0.1", 18099, {"A", ""}, [](std::string) { return PhoneRelayAnswer{}; }, &reason, 300));
  CHECK(WaitFor(relay, false));
  CHECK(relay.Status().phase == PhoneRelayPhase::kClosed);
}

// A short code can be guessed: ten wrong addresses close the page (the browser's icon request doesn't count).
void ClosesAfterTenWrongCodes() {
  PhoneRelay relay;
  std::string reason;
  CHECK(relay.Start("127.0.0.1", 18099, {"A", "<html>page</html>"}, [](std::string) { return PhoneRelayAnswer{}; }, &reason));
  const auto port = PortOf(relay.Address());
  const auto path = PathOf(relay.Address());
  const std::string wrong = path.substr(0, 5) + (path[5] == '2' ? "3" : "2");
  for (int i = 0; i < 5; ++i) CHECK(Status(Get(port, "/favicon.ico"), 404));
  for (int i = 0; i < 9; ++i) CHECK(Status(Get(port, wrong), 404));
  CHECK(relay.Running() && Status(Get(port, path), 200));
  CHECK(Status(Get(port, wrong), 404));
  CHECK(WaitFor(relay, false));
  CHECK(relay.Status().phase == PhoneRelayPhase::kClosed);
}

void CloseReturnsAtOnce() {
  PhoneRelay relay;
  std::string reason;
  CHECK(relay.Start("127.0.0.1", 18099, {"A", ""}, [](std::string) { return PhoneRelayAnswer{}; }, &reason));
  relay.Close();
  CHECK(WaitFor(relay, false));
  CHECK(relay.Status().phase == PhoneRelayPhase::kClosed);
}

int Connect(std::uint16_t port, int receive_buffer = 0) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (receive_buffer) ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &receive_buffer, sizeof(receive_buffer));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  CHECK(::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
  return fd;
}

// A peer that resets the connection mid-answer must not kill the process with SIGPIPE.
void SurvivesAPeerReset() {
  std::signal(SIGPIPE, SIG_DFL);
  PhoneRelay relay;
  std::string reason;
  CHECK(relay.Start("127.0.0.1", 18099, {"A", std::string(4 * 1024 * 1024, 'x')},
                    [](std::string) { return PhoneRelayAnswer{}; }, &reason, 20000));
  const auto port = PortOf(relay.Address());
  const auto path = PathOf(relay.Address());
  for (int i = 0; i < 20; ++i) {
    const int fd = Connect(port);
    const std::string request = "GET " + path + " HTTP/1.1\r\nHost: x\r\n\r\n";
    ::send(fd, request.data(), request.size(), 0);
    linger reset{1, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_LINGER, &reset, sizeof(reset));
    ::close(fd);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  CHECK(Status(Get(port, path), 200));  // still serving
  relay.Stop();
}

// A client that never reads the answer must not hold the relay past its lifetime or Stop.
void ASilentReaderDoesNotHoldTheRelay() {
  PhoneRelay relay;
  std::string reason;
  CHECK(relay.Start("127.0.0.1", 18099, {"A", std::string(8 * 1024 * 1024, 'x')},
                    [](std::string) { return PhoneRelayAnswer{}; }, &reason, 400));
  const int fd = Connect(PortOf(relay.Address()), 1024);
  const std::string request = "GET " + PathOf(relay.Address()) + " HTTP/1.1\r\nHost: x\r\n\r\n";
  ::send(fd, request.data(), request.size(), 0);
  CHECK(WaitFor(relay, false));  // the lifetime ends the stalled send (WaitFor allows 2 s)
  const auto started = std::chrono::steady_clock::now();
  relay.Stop();
  CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(1));
  ::close(fd);
}

}  // namespace

int main() {
  ServesThePackPageAndTakesOneCredential();
  WithoutAPackPageThePastePageAnswers();
  TwoPagesGetTwoPortsAndNonces();
  ClosesAfterItsLifetime();
  ClosesAfterTenWrongCodes();
  CloseReturnsAtOnce();
  SurvivesAPeerReset();
  ASilentReaderDoesNotHoldTheRelay();
  std::puts("runtime_phone_relay: ok");
  return 0;
}
