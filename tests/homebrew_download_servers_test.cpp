// SPDX-License-Identifier: GPL-3.0-or-later
#include "homebrew/controller.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <thread>
#include <unistd.h>

using namespace playden;
using namespace playden::homebrew;
using namespace playden::steam;
using namespace std::chrono_literals;

namespace {
class Sessions final : public SessionRepository {
 public:
  mutable std::mutex mutex;
  mutable std::optional<StoredSession> session = StoredSession{123, "Synthetic", "refresh", "access", {}};
  Result<std::optional<StoredSession>> Load() const override {
    std::lock_guard lock(mutex);
    return Result<std::optional<StoredSession>>::Success(session);
  }
  Result<void> Save(const StoredSession& value) const override {
    std::lock_guard lock(mutex); session = value; return Result<void>::Success();
  }
  Result<void> Clear() const override {
    std::lock_guard lock(mutex); session.reset(); return Result<void>::Success();
  }
};
class Auth final : public AuthBackend {
 public:
  AuthResult Restore(const StoredSession& session, const std::atomic<bool>&) override { return {AuthError::kNone, session}; }
  AuthResult AuthenticateQr(const std::atomic<bool>&, const ChallengeCallback&) override { return {AuthError::kCanceled, {}}; }
};
class Servers final : public DownloadServerSource {
 public:
  std::atomic<int> lists{0}, preparations{0}, measurements{0}, canceled{0};
  std::atomic<bool> hold{false};
  bool List(const std::atomic<bool>&, std::vector<DownloadServerView>* out) override {
    ++lists;
    *out = {{"slow", "Slow"}, {"fast", "Fast", true}};
    return true;
  }
  DownloadServerPreparation Prepare(const StoredSession&, const std::atomic<bool>&) override {
    ++preparations;
    return {DownloadServerPreparation::Error::kNone, 228980};
  }
  DownloadServerRate Measure(const std::string& id, std::uint64_t, const std::atomic<bool>& cancel) override {
    ++measurements;
    while (hold && !cancel) std::this_thread::sleep_for(1ms);
    if (cancel) ++canceled;
    return {id, id == "fast" ? 41.2 : 3.1, false};
  }
  std::string DisplayName(const std::string& id) const override {
    return id == "missing" ? "Saved provider" : id;
  }
};
template <class Predicate> void Wait(Controller& controller, Predicate ready) {
  const auto deadline = std::chrono::steady_clock::now() + 4s;
  do {
    controller.Tick();
    if (ready()) { controller.Tick(); return; }
    std::this_thread::sleep_for(1ms);
  } while (std::chrono::steady_clock::now() < deadline);
  assert(false && "download server controller deadline");
}
const ViewRow& Row(const Simulation& model, const std::string& key) {
  const auto& rows = model.view().rows;
  const auto found = std::find_if(rows.begin(), rows.end(), [&](const ViewRow& row) { return row.key == key; });
  assert(found != rows.end());
  return *found;
}
void Choose(Simulation& model, const std::string& key) {
  assert(model.action("focus:" + key));
  assert(model.action("cross"));
}
void OpenPicker(Simulation& model) {
  for (int i = 0; i < 5 && model.state().navigation.route != Route::Settings; ++i) model.action("r1");
  assert(model.state().navigation.route == Route::Settings);
  Choose(model, "settings-download-server");
}
void ProjectionAndCancel() {
  Simulation model({}, {}, ModelMode::Live);
  Sessions sessions; Auth auth; Servers source;
  DownloadServerService servers(source, sessions);
  Controller controller(model, auth, sessions);
  controller.SetDownloadServers(&servers);
  Wait(controller, [&] { return model.state().account().authentication == AuthState::SignedIn; });
  OpenPicker(model);
  Wait(controller, [&] { return servers.Snapshot().phase == DownloadServerSnapshot::Phase::kListed; });
  assert(source.lists == 1);
  assert(Row(model, "download-server-0").title == "Slow");
  assert(Row(model, "download-server-1").subtitle == "HTTP");
  Choose(model, "download-server-test");
  Wait(controller, [&] { return servers.Snapshot().phase == DownloadServerSnapshot::Phase::kTested; });
  assert(Row(model, "download-server-1").value == "41.2 MB/s");
  assert(Row(model, "download-server-1").badge == "Fastest");
  assert(model.state().navigation.focus_key == "download-server-1");
  assert(model.state().download_server.empty());
  source.hold = true;
  const int before = source.measurements;
  Choose(model, "download-server-test");
  Wait(controller, [&] { return source.measurements > before; });
  assert(model.action("circle"));
  Wait(controller, [&] { return source.canceled == 1 && servers.Snapshot().phase == DownloadServerSnapshot::Phase::kListed; });
  assert(!model.state().navigation.modals.empty());
  assert(model.action("circle"));
  PreferencesLoadResult loaded;
  loaded.preferences.download_server = "missing";
  assert(model.ApplyPreferences(model.BeginPreferencesLoad(), loaded));
  controller.Tick();
  assert(Row(model, "settings-download-server").value == "Saved provider");
  controller.Shutdown();
}
void SignOutRaceAndInvalidation() {
  Simulation model({}, {}, ModelMode::Live);
  Sessions sessions; Auth auth; Servers source;
  DownloadServerService servers(source, sessions);
  Controller controller(model, auth, sessions);
  controller.SetDownloadServers(&servers);
  Wait(controller, [&] { return model.state().account().authentication == AuthState::SignedIn; });
  OpenPicker(model);
  Wait(controller, [&] { return servers.Snapshot().phase == DownloadServerSnapshot::Phase::kListed; });
  // The request was valid at input time; account state changes before the controller drains it.
  Choose(model, "download-server-test");
  model.UpdateAccount({AuthState::SignedOut, {}, {}, {}, {}, false});
  controller.Tick();
  assert(source.preparations == 0);
  assert(servers.Snapshot().phase != DownloadServerSnapshot::Phase::kTesting);
  model.UpdateAccount({AuthState::SignedIn, "123", "Synthetic", {}, {}, false});
  controller.Tick();
  source.hold = true;
  Choose(model, "download-server-test");
  Wait(controller, [&] { return source.measurements > 0; });
  model.UpdateAccount({AuthState::SigningOut, "123", "Synthetic", {}, {}, false});
  Wait(controller, [&] { return source.canceled == 1; });
  model.UpdateAccount({AuthState::SignedIn, "123", "Synthetic", {}, {}, false});
  controller.Tick();
  assert(model.action("circle"));
  // Sign-out invalidates completed measurements as well as any active work.
  source.hold = false;
  OpenPicker(model);
  Wait(controller, [&] { return servers.Snapshot().phase == DownloadServerSnapshot::Phase::kListed; });
  Choose(model, "download-server-test");
  Wait(controller, [&] { return servers.Snapshot().phase == DownloadServerSnapshot::Phase::kTested; });
  assert(!servers.Snapshot().rates.empty());
  assert(model.action("circle"));
  Choose(model, "settings-signout");
  Choose(model, "confirm-destructive");
  controller.Tick();
  assert(servers.Snapshot().rates.empty());
  controller.Shutdown();
}
class Download final : public InstallRunner {
 public:
  std::atomic<bool> started{false};
  Result Install(const GameId&, const std::string&, const InstallOptions&, const std::atomic<bool>& cancel,
                 const std::function<void(std::uint64_t, std::uint64_t, std::uint64_t)>&,
                 const std::function<void(InstallStep)>&) override {
    started = true;
    while (!cancel) std::this_thread::sleep_for(1ms);
    return {false, true, {}, 0};
  }
  std::string Remove(const std::string&) override { return {}; }
};
void DownloadRaceAndCancellation() {
  const auto root = std::filesystem::canonical(std::filesystem::temp_directory_path()) /
                    ("playden-download-servers-" + std::to_string(getpid()));
  std::filesystem::create_directories(root);
  {
    Simulation model({}, {}, ModelMode::Live);
    Sessions sessions; Auth auth; Servers source; Download runner;
    InstallService installs(runner, root.string());
    DownloadServerService servers(source, sessions);
    Controller controller(model, auth, sessions, nullptr, nullptr, nullptr, &installs);
    controller.SetDownloadServers(&servers);
    Wait(controller, [&] { return model.state().account().authentication == AuthState::SignedIn; });
    OpenPicker(model);
    Wait(controller, [&] { return servers.Snapshot().phase == DownloadServerSnapshot::Phase::kListed; });
    Choose(model, "download-server-test");
    // A developer request can start a transfer after input but before this Tick.
    installs.Install(GameId::Steam(42));
    const auto deadline = std::chrono::steady_clock::now() + 4s;
    while (!runner.started && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(1ms);
    assert(runner.started);
    controller.Tick();
    assert(source.preparations == 0);
    assert(!Row(model, "download-server-test").enabled);
    installs.Pause(GameId::Steam(42));
    Wait(controller, [&] { return Row(model, "download-server-test").enabled; });
    source.hold = true;
    Choose(model, "download-server-test");
    Wait(controller, [&] { return source.measurements > 0; });
    installs.Resume(GameId::Steam(42));
    Wait(controller, [&] { return source.canceled == 1; });
    assert(servers.Snapshot().phase == DownloadServerSnapshot::Phase::kListed);
    controller.Shutdown();
  }
  std::filesystem::remove_all(root);
}
}  // namespace
int main() {
  ProjectionAndCancel();
  SignOutRaceAndInvalidation();
  DownloadRaceAndCancellation();
}
