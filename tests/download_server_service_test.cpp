// SPDX-License-Identifier: GPL-3.0-or-later
#include "steam/download_server_service.h"

#include <cassert>
#include <chrono>
#include <thread>

using namespace playden;
using namespace playden::steam;
using namespace std::chrono_literals;

namespace {
class Sessions final : public SessionRepository {
 public:
  bool signed_in = true;
  Result<std::optional<StoredSession>> Load() const override {
    return Result<std::optional<StoredSession>>::Success(signed_in ? std::optional<StoredSession>(StoredSession{1,"","r","a",{}}) : std::nullopt);
  }
  Result<void> Save(const StoredSession&) const override { return Result<void>::Success(); }
  Result<void> Clear() const override { return Result<void>::Success(); }
};
class Source final : public DownloadServerSource {
 public:
  bool list_ok = true;
  bool installed = true;
  std::atomic<bool> block{false}, entered{false};
  std::atomic<bool> block_prepare{false}, preparing{false};
  std::uint64_t budget = 60'000'000;
  std::vector<std::uint64_t> budgets;
  bool List(const std::atomic<bool>&, std::vector<DownloadServerView>* out) override {
    for (int i = 0; i < 8; ++i) out->push_back({"private-host-" + std::to_string(i), "Server", false});
    return list_ok;
  }
  DownloadServerPreparation Prepare(const StoredSession&, const std::atomic<bool>& cancel) override {
    preparing = true;
    while (block_prepare && !cancel) std::this_thread::sleep_for(1ms);
    return {installed ? DownloadServerPreparation::Error::kNone : DownloadServerPreparation::Error::kNeedsInstalledGame, 228980, budget};
  }
  DownloadServerRate Measure(const std::string& id, std::uint64_t budget, const std::atomic<bool>& cancel) override {
    budgets.push_back(budget);
    entered = true;
    while (block && !cancel) std::this_thread::sleep_for(1ms);
    return {id, 12.5, false};
  }
  std::string LogLabel(const std::string&) const override { return "private-host-must-not-leak"; }
};
DownloadServerSnapshot Await(DownloadServerService& service, DownloadServerSnapshot::Phase phase) {
  for (int i = 0; i < 2000; ++i) {
    const auto s = service.Snapshot();
    if (s.phase == phase) return s;
    std::this_thread::sleep_for(1ms);
  }
  assert(false && "worker did not finish");
  return {};
}
void ListingAndLimits() {
  Sessions sessions;
  Source source;
  DownloadServerService service(source, sessions);
  std::vector<std::string> logs;
  service.SetLog([&](std::uint32_t app, const char* step, const char* result, const char* detail) {
    assert(app == 228980 && std::string(step) == "server-test" && std::string(result) == "done");
    logs.push_back(detail);
  });
  service.List();
  const auto listed = Await(service, DownloadServerSnapshot::Phase::kListed);
  assert(listed.servers.size() == 8 && listed.servers[0].id == "private-host-0");
  service.Test();
  const auto tested = Await(service, DownloadServerSnapshot::Phase::kTested);
  service.Shutdown();
  assert(tested.rates.size() == 8 && tested.rates[0].mb_per_s == 12.5);
  std::uint64_t total = 0;
  for (const auto budget : source.budgets) { assert(budget <= 32'000'000); total += budget; }
  assert(total <= 60'000'000);  // preparation has already consumed the other 100 MB
  assert(!logs.empty());
  for (const auto& line : logs) { assert(line.size() <= 360); assert(line.find("private-host") == std::string::npos); }
}
void FailuresAndSignIn() {
  Sessions sessions;
  Source source;
  source.list_ok = false;
  DownloadServerService service(source, sessions);
  service.List();
  assert(Await(service, DownloadServerSnapshot::Phase::kListFailed).servers.empty());
  source.list_ok = true;
  sessions.signed_in = false;
  service.Test();
  assert(Await(service, DownloadServerSnapshot::Phase::kTestFailed).needs_sign_in);
  sessions.signed_in = true;
  source.installed = false;
  service.Test();
  assert(Await(service, DownloadServerSnapshot::Phase::kTestFailed).needs_installed_game);
}
void CancellationAndInvalidation() {
  Sessions sessions;
  Source source;
  source.block = true;
  DownloadServerService service(source, sessions);
  service.Test();
  while (!source.entered) std::this_thread::sleep_for(1ms);
  service.CancelTest();
  Await(service, DownloadServerSnapshot::Phase::kListed);
  assert(source.budgets.size() == 1);
  service.Invalidate();
  assert(service.Snapshot().rates.empty());
  service.List();
  assert(Await(service, DownloadServerSnapshot::Phase::kListed).servers.size() == 8);
  source.block = false;
  service.Test();
  assert(Await(service, DownloadServerSnapshot::Phase::kTested).rates.size() == 8);
}
void LogsNeverReusePreviousSamples() {
  Sessions sessions;
  Source source;
  DownloadServerService service(source, sessions);
  std::vector<std::pair<std::string, std::string>> logs;
  service.SetLog([&](std::uint32_t, const char*, const char* result, const char* detail) {
    logs.emplace_back(result, detail);
  });
  service.Test();
  Await(service, DownloadServerSnapshot::Phase::kTested);
  source.installed = false;
  service.Test();
  Await(service, DownloadServerSnapshot::Phase::kTestFailed);
  assert(logs.back() == std::make_pair(std::string("failed"), std::string("n=0")));
  source.installed = true;
  source.preparing = false;
  source.block_prepare = true;
  service.Test();
  while (!source.preparing) std::this_thread::sleep_for(1ms);
  service.CancelTest();
  Await(service, DownloadServerSnapshot::Phase::kListed);
  service.Shutdown();
  assert(logs.back() == std::make_pair(std::string("canceled"), std::string("n=0")));
}
}  // namespace
int main() { ListingAndLimits(); FailuresAndSignIn(); CancellationAndInvalidation(); LogsNeverReusePreviousSamples(); }
