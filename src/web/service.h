// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "web/logs_source.h"
#include "web/router.h"
#include "web/server.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace playden::web {

struct ServiceConfig {
  LogsPaths paths;
  ServerOptions server;  // ports 7031-7039 by default; its log is web.log
  // Asked when the UDP route lookup finds no address (the console's network control service).
  std::function<std::string()> address_fallback;
  // Seconds since 1970; the system clock when empty (tests pin it).
  std::function<std::int64_t()> now;
  // How often the address is looked up again while the server runs: the network can change.
  std::chrono::milliseconds address_refresh{30'000};
};

struct ServiceSnapshot {
  enum class State { kOff, kStarting, kOn, kFailed };
  State state = State::kOff;
  std::uint16_t port = 0;   // kOn: the port in use; kFailed: the last port tried
  std::string address;      // empty: unknown
  std::string url;          // http://<address>:<port>/ or empty
  std::string error;        // kFailed: why
};

// Web access, version 1: the page, the log index, single logs and the report, always redacted, and one
// action: POST /api/quit asks Playden to quit. A worker starts and stops the server, so the caller never
// waits on a socket.
class Service {
 public:
  explicit Service(ServiceConfig config);
  ~Service();
  Service(const Service&) = delete;
  Service& operator=(const Service&) = delete;

  // Turns the server on or off; it happens on the worker.
  void SetEnabled(bool enabled);
  // The signed-in account to redact exactly; empty when signed out (the patterns still apply).
  void SetAccount(Redaction account);
  ServiceSnapshot Snapshot() const;
  // True once after the page asked Playden to quit; the caller quits.
  bool TakeQuitRequest();
  // Stops the server and the worker. Safe to call twice.
  void Shutdown();

  // Routes one request, as the server's worker does (tests call it directly).
  void Handle(const Request& request, Response& response);

 private:
  void Run();
  void Log(const std::string& line) const;
  std::int64_t Now() const;
  std::string LookUpAddress() const;
  ServiceConfig config_;
  LogsSource logs_;
  Router router_;
  Server server_;
  mutable std::mutex mutex_;
  std::condition_variable wake_;
  bool enabled_ = false;
  bool stopping_ = false;
  std::uint64_t requests_ = 0;  // enable/disable changes, so a quick off-on restarts
  Redaction account_;
  bool quit_requested_ = false;
  ServiceSnapshot snapshot_;
  std::thread worker_;
};

}  // namespace playden::web
