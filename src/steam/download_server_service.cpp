// SPDX-License-Identifier: GPL-3.0-or-later
#include "steam/download_server_service.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace playden::steam {

DownloadServerService::DownloadServerService(DownloadServerSource& source, SessionRepository& sessions)
    : source_(source), sessions_(sessions), worker_([this] { Run(); }) {}
DownloadServerService::~DownloadServerService() { Shutdown(); }
void DownloadServerService::List() { Queue(Request::kList); }
void DownloadServerService::Test() { Queue(Request::kTest); }
void DownloadServerService::Queue(Request request) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (stopping_ || pending_ != Request::kNone || (active_ != Request::kNone && !cancel_)) return;
  if (active_ == Request::kNone) cancel_.store(false);
  pending_ = request;
  pending_cancel_ = false;
  snapshot_.needs_sign_in = snapshot_.needs_installed_game = false;
  snapshot_.testing_index = 0;
  snapshot_.phase = request == Request::kList || snapshot_.servers.empty()
      ? DownloadServerSnapshot::Phase::kListing : DownloadServerSnapshot::Phase::kTesting;
  wake_.notify_all();
}
void DownloadServerService::CancelTest() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (active_ == Request::kTest) cancel_.store(true);
  if (pending_ == Request::kTest) pending_cancel_ = true;
}
void DownloadServerService::Invalidate() {
  std::lock_guard<std::mutex> lock(mutex_);
  ++revision_;
  pending_ = Request::kNone;
  cancel_.store(true);
  snapshot_.rates.clear();
  snapshot_.needs_sign_in = snapshot_.needs_installed_game = false;
  snapshot_.phase = snapshot_.servers.empty() ? DownloadServerSnapshot::Phase::kIdle : DownloadServerSnapshot::Phase::kListed;
}
DownloadServerSnapshot DownloadServerService::Snapshot() {
  std::lock_guard<std::mutex> lock(mutex_);
  return snapshot_;
}
std::string DownloadServerService::DisplayName(const std::string& id) const { return source_.DisplayName(id); }
void DownloadServerService::SetLog(LogFn log) {
  std::lock_guard<std::mutex> lock(mutex_);
  log_ = std::move(log);
}
void DownloadServerService::Shutdown() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
    cancel_.store(true);
  }
  wake_.notify_all();
  if (worker_.joinable()) worker_.join();
}
void DownloadServerService::LogResults(std::uint32_t app, const char* result, const DownloadServerSnapshot& snapshot) {
  LogFn log;
  { std::lock_guard<std::mutex> lock(mutex_); log = log_; }
  if (!log) return;
  std::string line;
  for (std::size_t i = 0; i < snapshot.rates.size(); ++i) {
    const auto& rate = snapshot.rates[i];
    auto label = source_.LogLabel(rate.id);
    if (label != "akamai" && label != "fastly" && label != "google" && label != "alibaba" &&
        label != "edgenext" && label != "valve" && label != "cache") label = "cache";
    char number[40];
    std::snprintf(number, sizeof(number), "%.1f", std::clamp(std::isfinite(rate.mb_per_s) ? rate.mb_per_s : 0.0, 0.0, 999999.9));
    const auto token = label + '@' + std::to_string(i) + (rate.failed ? ":failed" : ":mb_s=" + std::string(number));
    if (!line.empty() && line.size() + token.size() + 1 > 360) {
      log(app, "server-test", result, line.c_str()); line.clear();
    }
    if (!line.empty()) line += ' ';
    line += token;
  }
  log(app, "server-test", result, line.empty() ? "n=0" : line.c_str());
}
void DownloadServerService::Run() {
  for (;;) {
    Request request;
    std::uint64_t revision;
    DownloadServerSnapshot state;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      wake_.wait(lock, [this] { return stopping_ || pending_ != Request::kNone; });
      if (stopping_) return;
      request = active_ = pending_;
      pending_ = Request::kNone;
      revision = revision_;
      state = snapshot_;
      cancel_.store(pending_cancel_);
      pending_cancel_ = false;
    }
    std::uint32_t app = 0;
    std::uint64_t measurement_budget = 0;
    DownloadServerSnapshot measured;
    bool failed = false;
    if (request == Request::kList || state.servers.empty()) {
      std::vector<DownloadServerView> listed;
      if (!cancel_ && source_.List(cancel_, &listed) && !listed.empty()) state.servers = std::move(listed);
      else { state.servers.clear(); failed = true; }
      state.phase = failed ? DownloadServerSnapshot::Phase::kListFailed : DownloadServerSnapshot::Phase::kListed;
    }
    if (request == Request::kTest && !failed && !cancel_) {
      const auto session = sessions_.Load();
      if (!session.ok() || !session.value()) { state.needs_sign_in = true; failed = true; }
      else {
        const auto prepared = source_.Prepare(*session.value(), cancel_);
        app = prepared.app_id;
        failed = prepared.error != DownloadServerPreparation::Error::kNone;
        measurement_budget = std::min(kTotalByteLimit, prepared.measurement_byte_budget);
        failed = failed || measurement_budget == 0;
        state.needs_installed_game = prepared.error == DownloadServerPreparation::Error::kNeedsInstalledGame;
      }
      if (!failed && !cancel_) {
        state.rates.clear();
        const auto budget = std::min(kServerByteLimit, measurement_budget / state.servers.size());
        for (std::size_t i = 0; i < state.servers.size() && !cancel_; ++i) {
          state.phase = DownloadServerSnapshot::Phase::kTesting;
          state.testing_index = i;
          { std::lock_guard<std::mutex> lock(mutex_); if (revision == revision_ && !stopping_ && !cancel_) snapshot_ = state; }
          auto rate = source_.Measure(state.servers[i].id, budget, cancel_);
          if (!cancel_) {
            measured.rates.push_back(rate);
            state.rates.push_back(std::move(rate));
          }
        }
      }
      state.phase = failed ? DownloadServerSnapshot::Phase::kTestFailed : DownloadServerSnapshot::Phase::kTested;
    }
    if (cancel_) state.phase = state.servers.empty() ? DownloadServerSnapshot::Phase::kIdle : DownloadServerSnapshot::Phase::kListed;
    if (request == Request::kTest) LogResults(app, cancel_ ? "canceled" : failed ? "failed" : "done", measured);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (revision == revision_ && !stopping_ && pending_ == Request::kNone) snapshot_ = std::move(state);
      active_ = Request::kNone;
      cancel_.store(false);
      if (stopping_) return;
    }
  }
}

}  // namespace playden::steam
