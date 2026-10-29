// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "steam/session_store.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace playden::steam {

inline constexpr std::uint64_t kDownloadServerTestByteLimit = 160'000'000;

struct DownloadServerView { std::string id; std::string name; bool plain_http = false; };
struct DownloadServerRate { std::string id; double mb_per_s = 0; bool failed = false; };
struct DownloadServerSnapshot {
  enum class Phase { kIdle, kListing, kListed, kListFailed, kTesting, kTested, kTestFailed };
  Phase phase = Phase::kIdle;
  std::vector<DownloadServerView> servers;
  std::vector<DownloadServerRate> rates;
  std::size_t testing_index = 0;
  bool needs_sign_in = false;
  bool needs_installed_game = false;
};

struct DownloadServerPreparation {
  enum class Error { kNone, kFailed, kNeedsInstalledGame } error = Error::kFailed;
  std::uint32_t app_id = 0;
  // Remaining after directory and manifest preparation, including failed transfers.
  std::uint64_t measurement_byte_budget = kDownloadServerTestByteLimit;
};

// Network methods run on the service worker. The name/label helpers must be thread safe.
class DownloadServerSource {
 public:
  virtual ~DownloadServerSource() = default;
  virtual bool List(const std::atomic<bool>& cancel, std::vector<DownloadServerView>* out) = 0;
  virtual DownloadServerPreparation Prepare(const StoredSession&, const std::atomic<bool>& cancel) = 0;
  virtual DownloadServerRate Measure(const std::string& id, std::uint64_t byte_budget,
                                     const std::atomic<bool>& cancel) = 0;
  virtual std::string DisplayName(const std::string& id) const { return id; }
  virtual std::string LogLabel(const std::string&) const { return "cache"; }
};

// One worker; source and repository must outlive the service. Results are session memory only.
class DownloadServerService {
 public:
  using LogFn = std::function<void(std::uint32_t, const char*, const char*, const char*)>;
  DownloadServerService(DownloadServerSource& source, SessionRepository& sessions);
  ~DownloadServerService();
  DownloadServerService(const DownloadServerService&) = delete;
  DownloadServerService& operator=(const DownloadServerService&) = delete;
  void List();
  void Test();
  void CancelTest();
  void Invalidate();
  DownloadServerSnapshot Snapshot();
  std::string DisplayName(const std::string& id) const;
  void SetLog(LogFn log);
  void Shutdown();

  static constexpr std::uint64_t kServerByteLimit = 32'000'000;
  static constexpr std::uint64_t kTotalByteLimit = kDownloadServerTestByteLimit;

 private:
  enum class Request { kNone, kList, kTest };
  void Run();
  void Queue(Request request);
  void LogResults(std::uint32_t app, const char* result, const DownloadServerSnapshot& snapshot);
  DownloadServerSource& source_;
  SessionRepository& sessions_;
  std::mutex mutex_;
  std::condition_variable wake_;
  DownloadServerSnapshot snapshot_;
  LogFn log_;
  Request pending_ = Request::kNone;
  Request active_ = Request::kNone;
  bool pending_cancel_ = false;
  std::uint64_t revision_ = 0;
  bool stopping_ = false;
  std::atomic<bool> cancel_{false};
  std::thread worker_;
};

}  // namespace playden::steam
