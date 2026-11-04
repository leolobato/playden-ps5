// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "web/http.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace playden::web {

struct ServerOptions {
  // Ports tried in turn while bind says the address is in use. 0 asks for any free port (tests).
  std::uint16_t first_port = 7031;
  std::uint16_t last_port = 7039;
  bool loopback_only = false;  // 127.0.0.1 instead of every address (tests)
  int backlog = 8;             // connections waiting while every worker is busy
  int workers = 6;             // connections served at once
  int read_timeout_ms = 10'000;
  int write_timeout_ms = 10'000;
  // A connection that has sent nothing gives up after this (a browser's spare connection), so it doesn't hold a worker.
  int silent_timeout_ms = 2'000;
  // Asked for on the listener, before it binds, so accepted sockets inherit it. The console gives at most 512 KiB, and
  // the default of 64 KiB leaves too little ready for each large write to storage.
  int receive_buffer_bytes = 4 * 1024 * 1024;
  // A request body may go quiet for this long.
  int body_timeout_ms = 30'000;
  // How often the accept wait looks at Stop. Closing a listening socket does not reliably wake a
  // blocked accept, so the worker never blocks longer than this.
  int poll_interval_ms = 250;
  // Logs every socket, bind, listen and accept result (the console listener probe).
  bool verbose = false;
  // One line per request and each start and stop, for web.log.
  std::function<void(const std::string&)> log;
};

// A small HTTP/1.1 server: one thread accepts, and a fixed pool of workers each serves one connection at a
// time, reading the request, routing it and writing the answer, then closing. No keep-alive. Handlers run on the
// workers, at the same time. Every worker busy: new connections wait in the listen backlog.
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
  bool running() const { return acceptor_.joinable(); }
  std::uint16_t port() const { return port_; }

 private:
  struct Accepted {
    int fd = -1;
    std::string ip;
  };
  void Run();
  void Work();
  void Serve(int client, const std::string& client_ip);
  void Log(const std::string& line) const;
  ServerOptions options_;
  Handler handler_;
  int listener_ = -1;
  std::uint16_t port_ = 0;
  std::atomic<bool> stop_{false};
  std::mutex mutex_;
  std::condition_variable changed_;
  std::vector<Accepted> waiting_;  // accepted, for the next free worker
  int idle_ = 0;                   // workers waiting for a connection
  std::vector<int> clients_;       // every open connection, so Stop can cut them
  std::thread acceptor_;
  std::vector<std::thread> workers_;
};

}  // namespace playden::web
