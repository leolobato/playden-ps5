// SPDX-License-Identifier: GPL-3.0-or-later
#include "web/server.h"

#include "web/router.h"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <vector>

namespace playden::web {
namespace {

#if defined(MSG_NOSIGNAL)
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

// A browser that closes the tab mid-download must not raise SIGPIPE in the title.
void NoSigpipe(int fd) {
#if defined(SO_NOSIGPIPE)
  int on = 1;
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#else
  (void)fd;
#endif
}

void SetTimeout(int fd, int option, int milliseconds) {
  timeval timeout{};
  timeout.tv_sec = milliseconds / 1000;
  timeout.tv_usec = (milliseconds % 1000) * 1000;
  setsockopt(fd, SOL_SOCKET, option, &timeout, sizeof timeout);
}

std::string ErrorText(int error) { return std::string(std::strerror(error)) + " (errno " + std::to_string(error) + ")"; }

// The path as web.log shows it: no query, except the title a report was asked for.
std::string LoggedTarget(const Request& request) {
  std::string target;
  for (const char c : request.path) target += (static_cast<unsigned char>(c) < 0x20 || c == 0x7f || c == ' ') ? '?' : c;
  if (target.size() > 120) target = target.substr(0, 120) + "...";
  if (const auto title = QueryValue(request.query, "title")) {
    std::string clean;
    for (const char c : *title) if (((c | 0x20) >= 'a' && (c | 0x20) <= 'z') || (c >= '0' && c <= '9')) clean += c;
    target += "?title=" + clean.substr(0, 16);
  }
  return target;
}

}  // namespace

Server::~Server() { Stop(); }

void Server::Log(const std::string& line) const {
  if (options_.log) options_.log(line);
}

bool Server::Start(const ServerOptions& options, Handler handler, std::string* error) {
  Stop();
  options_ = options;
  handler_ = std::move(handler);
  stop_.store(false);
  const auto fail = [&](const std::string& why) {
    if (listener_ >= 0) ::close(listener_);
    listener_ = -1;
    if (error) *error = why;
    Log("start failed: " + why);
    return false;
  };
  int last_error = 0;
  std::uint16_t tried = options.first_port;
  for (std::uint32_t port = options.first_port; port <= options.last_port; ++port) {
    tried = static_cast<std::uint16_t>(port);
    listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (options_.verbose) Log("socket fd=" + std::to_string(listener_) + (listener_ < 0 ? " " + ErrorText(errno) : ""));
    if (listener_ < 0) return fail("socket: " + ErrorText(errno));
    int on = 1;
    setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    NoSigpipe(listener_);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<std::uint16_t>(port));
    address.sin_addr.s_addr = htonl(options.loopback_only ? INADDR_LOOPBACK : INADDR_ANY);
    const int bound = ::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof address);
    last_error = bound == 0 ? 0 : errno;
    if (options_.verbose)
      Log("bind port=" + std::to_string(port) + " result=" + std::to_string(bound) + (bound ? " " + ErrorText(last_error) : ""));
    if (bound == 0) break;
    ::close(listener_);
    listener_ = -1;
    if (last_error != EADDRINUSE) return fail("port " + std::to_string(port) + ": " + ErrorText(last_error));
    if (port == 0xffff) break;
  }
  if (listener_ < 0) return fail("port " + std::to_string(tried) + ": " + ErrorText(last_error));
  const int listening = ::listen(listener_, options.backlog);
  const int listen_error = listening == 0 ? 0 : errno;
  if (options_.verbose) Log("listen result=" + std::to_string(listening) + (listening ? " " + ErrorText(listen_error) : ""));
  if (listening != 0) return fail("listen: " + ErrorText(listen_error));
  sockaddr_in bound{};
  socklen_t size = sizeof bound;
  if (::getsockname(listener_, reinterpret_cast<sockaddr*>(&bound), &size) != 0) return fail("getsockname: " + ErrorText(errno));
  port_ = ntohs(bound.sin_port);
  worker_ = std::thread([this] { Run(); });
  return true;
}

void Server::Stop() {
  stop_.store(true);
  {
    // Wakes a send or recv blocked on the client; the worker still owns and closes the descriptor.
    std::lock_guard<std::mutex> lock(client_mutex_);
    if (client_ >= 0) ::shutdown(client_, SHUT_RDWR);
  }
  if (worker_.joinable()) worker_.join();
  if (listener_ >= 0) ::close(listener_);
  listener_ = -1;
}

void Server::Run() {
  while (!stop_.load()) {
    pollfd waiting{};
    waiting.fd = listener_;
    waiting.events = POLLIN;
    const int ready = ::poll(&waiting, 1, options_.poll_interval_ms);
    if (ready < 0) {
      if (errno == EINTR) continue;
      Log("poll failed: " + ErrorText(errno));
      // Not a spin: wait as long as one poll would have.
      std::this_thread::sleep_for(std::chrono::milliseconds(options_.poll_interval_ms));
      continue;
    }
    if (ready == 0 || stop_.load()) continue;
    sockaddr_in peer{};
    socklen_t size = sizeof peer;
    const int client = ::accept(listener_, reinterpret_cast<sockaddr*>(&peer), &size);
    const int accept_error = client < 0 ? errno : 0;
    char ip[INET_ADDRSTRLEN] = "?";
    if (client >= 0) inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof ip);
    if (options_.verbose)
      Log("accept fd=" + std::to_string(client) + (client < 0 ? " " + ErrorText(accept_error) : std::string(" client=") + ip));
    if (client < 0) continue;
    NoSigpipe(client);
    SetTimeout(client, SO_RCVTIMEO, options_.read_timeout_ms);
    SetTimeout(client, SO_SNDTIMEO, options_.write_timeout_ms);
    {
      std::lock_guard<std::mutex> lock(client_mutex_);
      client_ = client;
    }
    if (!stop_.load()) Serve(client, ip);
    {
      std::lock_guard<std::mutex> lock(client_mutex_);
      client_ = -1;
    }
    ::shutdown(client, SHUT_RDWR);
    ::close(client);
  }
}

void Server::Serve(int client, const std::string& client_ip) {
  const auto write = [this, client](const char* data, std::size_t size) {
    while (size > 0) {
      if (stop_.load()) return false;
      const auto sent = ::send(client, data, size, kSendFlags);
      if (sent < 0 && errno == EINTR) continue;
      if (sent <= 0) return false;
      data += sent;
      size -= static_cast<std::size_t>(sent);
    }
    return true;
  };
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(options_.read_timeout_ms);
  std::string received;
  std::vector<char> buffer(2048);
  Request request;
  ParseResult parsed = ParseResult::kIncomplete;
  while (parsed == ParseResult::kIncomplete) {
    if (std::chrono::steady_clock::now() > deadline || stop_.load()) break;
    const auto count = ::recv(client, buffer.data(), buffer.size(), 0);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) break;
    received.append(buffer.data(), static_cast<std::size_t>(count));
    parsed = ParseRequestHead(received, &request);
  }
  Response response(write, request.method == Method::kHead);
  std::string target = "-";
  if (parsed == ParseResult::kOk) {
    target = LoggedTarget(request);
    handler_(request, response);
    if (!response.started()) SendError(response, 500, "No answer.");
  } else if (parsed == ParseResult::kTooLarge) {
    SendError(response, 431, "The request is too large.");
  } else if (parsed == ParseResult::kBadRequest) {
    SendError(response, 400, "Bad request.");
  } else if (!received.empty()) {
    SendError(response, 408, "The request took too long.");
  } else {
    return;  // connected and sent nothing: a browser's spare connection
  }
  Log((parsed == ParseResult::kOk ? request.method_text : std::string("-")) + " " + target + " " +
      std::to_string(response.status()) + " " + std::to_string(response.body_bytes()) + " " + client_ip +
      (response.failed() ? " cut" : ""));
}

}  // namespace playden::web
