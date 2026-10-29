// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "web/http.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace playden::web {

struct ServerOptions {
  // Ports tried in turn while bind says the address is in use. 0 asks for any free port (tests).
  std::uint16_t first_port = 7031;
  std::uint16_t last_port = 7039;
  bool loopback_only = false;  // 127.0.0.1 instead of every address (tests)
  int backlog = 4;             // connections waiting while one is served
  int read_timeout_ms = 10'000;
  int write_timeout_ms = 10'000;
  // How often the accept wait looks at Stop. Closing a listening socket does not reliably wake a
  // blocked accept, so the worker never blocks longer than this.
  int poll_interval_ms = 250;
  // Logs every socket, bind, listen and accept result (the console listener probe).
  bool verbose = false;
  // One line per request and each start and stop, for web.log.
  std::function<void(const std::string&)> log;
};

// A small HTTP/1.1 server: one worker accepts and serves one connection at a time, reading the
// request, routing it and writing the answer, then closing. No keep-alive.
class Server {
 public:
  using Handler = std::function<void(const Request& request, Response& response)>;
  Server() = default;
  ~Server();
  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  // Binds and listens on the caller's thread, then starts the worker. On failure, error says why
  // and which port was tried last.
  bool Start(const ServerOptions& options, Handler handler, std::string* error);
  // Stops accepting, cuts the connection being served (a slow download included), and closes the socket.
  void Stop();
  bool running() const { return worker_.joinable(); }
  std::uint16_t port() const { return port_; }

 private:
  void Run();
  void Serve(int client, const std::string& client_ip);
  void Log(const std::string& line) const;
  ServerOptions options_;
  Handler handler_;
  int listener_ = -1;
  std::uint16_t port_ = 0;
  std::atomic<bool> stop_{false};
  std::mutex client_mutex_;
  int client_ = -1;  // the connection being served, so Stop can cut it
  std::thread worker_;
};

}  // namespace playden::web
