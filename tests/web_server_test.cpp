// SPDX-License-Identifier: GPL-3.0-or-later
#include "web/router.h"
#include "web/server.h"

#include <arpa/inet.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace playden::web;

#define CHECK(condition)                                                                  \
  do {                                                                                    \
    if (!(condition)) {                                                                   \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
      std::exit(1);                                                                       \
    }                                                                                     \
  } while (false)

namespace {
int Connect(std::uint16_t port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  CHECK(fd >= 0 && ::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0);
  return fd;
}

std::string Exchange(std::uint16_t port, const std::string& request) {
  const int fd = Connect(port);
  CHECK(::send(fd, request.data(), request.size(), 0) == static_cast<ssize_t>(request.size()));
  std::string answer;
  char buffer[4096];
  ssize_t count;
  while ((count = ::recv(fd, buffer, sizeof buffer, 0)) > 0) answer.append(buffer, static_cast<std::size_t>(count));
  ::close(fd);
  return answer;
}

// A listener on a free loopback port, held so the server must move to the next one.
int Hold(std::uint16_t* port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  socklen_t size = sizeof address;
  CHECK(::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0 && ::listen(fd, 1) == 0);
  CHECK(::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &size) == 0);
  *port = ntohs(address.sin_port);
  return fd;
}
}  // namespace

int main() {
  Router router;
  router.Exact("/", [](const Request&, const std::string&, Response& response) { response.Send(200, "text/plain", "hello\n"); });
  router.Exact("/big", [](const Request&, const std::string&, Response& response) {
    response.Start(200, "application/octet-stream", std::nullopt);
    const std::string chunk(64 * 1024, 'z');
    for (int i = 0; i < 64 && response.Body(chunk); ++i) {}
  });
  std::mutex lines_mutex;
  std::vector<std::string> lines;
  ServerOptions options;
  options.first_port = options.last_port = 0;
  options.loopback_only = true;
  options.read_timeout_ms = 500;
  options.write_timeout_ms = 500;
  options.poll_interval_ms = 50;
  options.log = [&](const std::string& line) { std::lock_guard<std::mutex> lock(lines_mutex); lines.push_back(line); };
  Server server;
  std::string error;
  CHECK(server.Start(options, [&router](const Request& request, Response& response) { router.Handle(request, response); }, &error));
  CHECK(server.running() && server.port() != 0);

  auto answer = Exchange(server.port(), "GET / HTTP/1.1\r\nHost: x\r\n\r\n");
  CHECK(answer.rfind("HTTP/1.1 200 OK\r\n", 0) == 0 && answer.size() > 6 && answer.compare(answer.size() - 6, 6, "hello\n") == 0);
  CHECK(answer.find("Connection: close\r\n") != std::string::npos);
  answer = Exchange(server.port(), "HEAD / HTTP/1.1\r\n\r\n");
  CHECK(answer.rfind("HTTP/1.1 200 OK\r\n", 0) == 0 && answer.find("hello") == std::string::npos);
  CHECK(Exchange(server.port(), "POST / HTTP/1.1\r\nContent-Length: 3\r\n\r\nabc").rfind("HTTP/1.1 405", 0) == 0);
  CHECK(Exchange(server.port(), "GET /nope HTTP/1.1\r\n\r\n").rfind("HTTP/1.1 404", 0) == 0);
  CHECK(Exchange(server.port(), "GARBAGE\r\n\r\n").rfind("HTTP/1.1 400", 0) == 0);
  CHECK(Exchange(server.port(), "GET / HTTP/1.1\r\nX: " + std::string(9000, 'a') + "\r\n\r\n").rfind("HTTP/1.1 431", 0) == 0);
  // Half a request, then silence: answered with 408 once the read time is up.
  {
    const auto started = std::chrono::steady_clock::now();
    CHECK(Exchange(server.port(), "GET / HTTP/1.1\r\n").rfind("HTTP/1.1 408", 0) == 0);
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(3));
  }
  // A client that goes away in the middle of a 4 MB answer: the server carries on.
  {
    const int fd = Connect(server.port());
    const std::string request = "GET /big HTTP/1.1\r\n\r\n";
    CHECK(::send(fd, request.data(), request.size(), 0) > 0);
    char buffer[1024];
    CHECK(::recv(fd, buffer, sizeof buffer, 0) > 0);
    ::close(fd);
  }
  CHECK(Exchange(server.port(), "GET / HTTP/1.1\r\n\r\n").rfind("HTTP/1.1 200", 0) == 0);
  // A connection that sends nothing is closed without an answer or a log line.
  {
    const int fd = Connect(server.port());
    ::shutdown(fd, SHUT_WR);
    char byte;
    CHECK(::recv(fd, &byte, 1, 0) == 0);
    ::close(fd);
  }

  // Stop returns promptly and the port is closed.
  const auto port = server.port();
  const auto stopping = std::chrono::steady_clock::now();
  server.Stop();
  CHECK(!server.running());
  CHECK(std::chrono::steady_clock::now() - stopping < std::chrono::seconds(2));
  {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof address) != 0);
    ::close(fd);
  }
  {
    std::lock_guard<std::mutex> lock(lines_mutex);
    bool logged = false;
    for (const auto& line : lines) logged = logged || line == "GET / 200 6 127.0.0.1";
    CHECK(logged);
  }

  // A port in use: the next one in the range is taken; when all are taken, the error names the last.
  std::uint16_t held_port = 0;
  const int held = Hold(&held_port);
  options.first_port = held_port;
  options.last_port = static_cast<std::uint16_t>(held_port + 1);
  Server next;
  if (next.Start(options, [](const Request&, Response& response) { response.Send(200, "text/plain", "next"); }, &error)) {
    CHECK(next.port() == held_port + 1);
    next.Stop();
  }
  options.last_port = held_port;
  Server refused;
  CHECK(!refused.Start(options, [](const Request&, Response&) {}, &error) && !refused.running());
  CHECK(error.find("port " + std::to_string(held_port)) == 0);
  ::close(held);

  // A client that asks for a big answer and never reads it: Stop cuts it at once, not after the write time limit.
  {
    ServerOptions slow = options;
    slow.first_port = slow.last_port = 0;
    slow.write_timeout_ms = 30'000;
    Server stalled;
    CHECK(stalled.Start(slow, [&router](const Request& request, Response& response) { router.Handle(request, response); }, &error));
    const int fd = Connect(stalled.port());
    const std::string request = "GET /big HTTP/1.1\r\n\r\n";
    CHECK(::send(fd, request.data(), request.size(), 0) > 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));  // the worker is now blocked in send
    const auto cut = std::chrono::steady_clock::now();
    stalled.Stop();
    CHECK(std::chrono::steady_clock::now() - cut < std::chrono::seconds(3));
    ::close(fd);
  }
  // A client that sends half a request and waits: Stop doesn't wait for the read time limit either.
  {
    ServerOptions slow = options;
    slow.first_port = slow.last_port = 0;
    slow.read_timeout_ms = 30'000;
    Server waiting;
    CHECK(waiting.Start(slow, [](const Request&, Response& response) { response.Send(200, "text/plain", "x"); }, &error));
    const int fd = Connect(waiting.port());
    CHECK(::send(fd, "GET / HTTP/1.1\r\n", 16, 0) == 16);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const auto cut = std::chrono::steady_clock::now();
    waiting.Stop();
    CHECK(std::chrono::steady_clock::now() - cut < std::chrono::seconds(3));
    ::close(fd);
  }

  // Several connections at once: a client that stalls doesn't hold up the next.
  {
    ServerOptions pool = options;
    pool.first_port = pool.last_port = 0;
    pool.workers = 3;
    pool.read_timeout_ms = 30'000;
    pool.silent_timeout_ms = 30'000;
    Server busy;
    CHECK(busy.Start(pool, [](const Request&, Response& response) { response.Send(200, "text/plain", "x"); }, &error));
    const int stalled = Connect(busy.port());
    CHECK(::send(stalled, "GET / HTTP/1.1\r\n", 16, 0) == 16);
    const int silent = Connect(busy.port());
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    const auto started = std::chrono::steady_clock::now();
    CHECK(Exchange(busy.port(), "GET / HTTP/1.1\r\n\r\n").rfind("HTTP/1.1 200", 0) == 0);
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(1));
    // Every worker taken: the next waits in the backlog, and Stop still returns promptly.
    const int second = Connect(busy.port());
    CHECK(::send(second, "GET / HTTP/1.1\r\n", 16, 0) == 16);
    const int queued = Connect(busy.port());
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    const auto cut = std::chrono::steady_clock::now();
    busy.Stop();
    CHECK(std::chrono::steady_clock::now() - cut < std::chrono::seconds(2));
    for (const int fd : {stalled, silent, second, queued}) ::close(fd);
  }
  // A connection that sends nothing gives its worker back after the silent time.
  {
    ServerOptions quiet = options;
    quiet.first_port = quiet.last_port = 0;
    quiet.workers = 1;
    quiet.read_timeout_ms = 30'000;
    quiet.silent_timeout_ms = 200;
    Server one;
    CHECK(one.Start(quiet, [](const Request&, Response& response) { response.Send(200, "text/plain", "x"); }, &error));
    const int silent = Connect(one.port());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const auto started = std::chrono::steady_clock::now();
    CHECK(Exchange(one.port(), "GET / HTTP/1.1\r\n\r\n").rfind("HTTP/1.1 200", 0) == 0);
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(2));
    ::close(silent);
    one.Stop();
  }
  // The body: what came with the head first, split anywhere, at most the declared length.
  {
    ServerOptions bodies = options;
    bodies.first_port = bodies.last_port = 0;
    bodies.body_timeout_ms = 300;
    std::mutex read_mutex;
    std::string read_text;
    std::ptrdiff_t last = 99;
    Server reading;
    CHECK(reading.Start(bodies, [&](const Request& request, Response& response) {
      std::string text;
      char buffer[7];
      std::ptrdiff_t count;
      while ((count = request.read_body(buffer, sizeof buffer)) > 0) text.append(buffer, static_cast<std::size_t>(count));
      {
        std::lock_guard<std::mutex> lock(read_mutex);
        read_text = text;
        last = count;
      }
      response.Send(200, "text/plain", "read");
    }, &error));
    const std::string payload = "0123456789abcdefghij";
    const std::string head = "POST /u HTTP/1.1\r\nContent-Length: " + std::to_string(payload.size()) + "\r\n\r\n";
    const std::string whole = head + payload + "EXTRA";
    for (const std::size_t split : {std::size_t{5}, head.size() - 1, head.size(), head.size() + 3, head.size() + 19}) {
      const int fd = Connect(reading.port());
      CHECK(::send(fd, whole.data(), split, 0) == static_cast<ssize_t>(split));
      std::this_thread::sleep_for(std::chrono::milliseconds(30));
      CHECK(::send(fd, whole.data() + split, whole.size() - split, 0) == static_cast<ssize_t>(whole.size() - split));
      std::string answer;
      char buffer[256];
      ssize_t count;
      while ((count = ::recv(fd, buffer, sizeof buffer, 0)) > 0) answer.append(buffer, static_cast<std::size_t>(count));
      ::close(fd);
      CHECK(answer.rfind("HTTP/1.1 200", 0) == 0);
      std::lock_guard<std::mutex> lock(read_mutex);
      CHECK(read_text == payload && last == 0);
    }
    // A short body: the read fails once the client goes quiet, instead of waiting for ever.
    {
      const int fd = Connect(reading.port());
      const std::string shorter = head + "012";
      CHECK(::send(fd, shorter.data(), shorter.size(), 0) == static_cast<ssize_t>(shorter.size()));
      std::string answer;
      char buffer[256];
      ssize_t count;
      while ((count = ::recv(fd, buffer, sizeof buffer, 0)) > 0) answer.append(buffer, static_cast<std::size_t>(count));
      ::close(fd);
      std::lock_guard<std::mutex> lock(read_mutex);
      CHECK(read_text == "012" && last == -1);
    }
    reading.Stop();
  }
  // An answer before the body: the client still gets it, after sending all it had.
  {
    ServerOptions refuse = options;
    refuse.first_port = refuse.last_port = 0;
    Server refusing;
    CHECK(refusing.Start(refuse, [](const Request&, Response& response) { response.Send(409, "text/plain", "no\n"); }, &error));
    const int fd = Connect(refusing.port());
    const std::string payload(256 * 1024, 'p');
    const std::string head = "POST /u HTTP/1.1\r\nContent-Length: " + std::to_string(payload.size()) + "\r\n\r\n";
    CHECK(::send(fd, head.data(), head.size(), 0) == static_cast<ssize_t>(head.size()));
    std::size_t sent = 0;
    while (sent < payload.size()) {
      const auto count = ::send(fd, payload.data() + sent, payload.size() - sent, 0);
      if (count <= 0) break;
      sent += static_cast<std::size_t>(count);
    }
    std::string answer;
    char buffer[256];
    ssize_t count;
    while ((count = ::recv(fd, buffer, sizeof buffer, 0)) > 0) answer.append(buffer, static_cast<std::size_t>(count));
    ::close(fd);
    CHECK(answer.rfind("HTTP/1.1 409", 0) == 0);
    refusing.Stop();
  }

  // Started again after a stop.
  options.first_port = options.last_port = 0;
  CHECK(server.Start(options, [&router](const Request& request, Response& response) { router.Handle(request, response); }, &error));
  CHECK(Exchange(server.port(), "GET / HTTP/1.1\r\n\r\n").rfind("HTTP/1.1 200", 0) == 0);
  server.Stop();
  std::puts("web_server: ok");
}
