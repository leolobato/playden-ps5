// SPDX-License-Identifier: GPL-3.0-or-later
// Workshop items (#856) through the controller: the sheet's request reaches the listing worker, its answer (or the
// sentence for its error) reaches the sheet, and Apply becomes one Workshop job in the install service, with the whole
// choice, which Queue and the game's page name "Workshop items".
#include "homebrew/controller.hpp"
#include "installs/install_record.h"
#include "steam/backend/workshop_listing_service.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <unistd.h>

using namespace playden;
using namespace playden::homebrew;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

int failures = 0;
#define CHECK(condition)                                                              \
  do {                                                                                \
    if (!(condition)) {                                                               \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
      ++failures;                                                                     \
    }                                                                                 \
  } while (false)

constexpr std::uint32_t kApp = 1942280;

class Sessions final : public SessionRepository {
 public:
  Result<std::optional<StoredSession>> Load() const override {
    return Result<std::optional<StoredSession>>::Success(StoredSession{123, "Synthetic", "refresh", "access", {}});
  }
  Result<void> Save(const StoredSession&) const override { return Result<void>::Success(); }
  Result<void> Clear() const override { return Result<void>::Success(); }
};
class Auth final : public steam::AuthBackend {
 public:
  steam::AuthResult Restore(const StoredSession& session, const std::atomic<bool>&) override {
    return {steam::AuthError::kNone, session};
  }
  steam::AuthResult AuthenticateQr(const std::atomic<bool>&, const steam::ChallengeCallback&) override {
    return {steam::AuthError::kCanceled, {}};
  }
};

// Records the Workshop job it is asked for, and holds it until released.
class Runner final : public steam::InstallRunner {
 public:
  std::mutex mutex;
  std::optional<steam::InstallOptions> asked;
  std::atomic<bool> hold{true};
  Result Install(const GameId&, const std::string&, const steam::InstallOptions&, const std::atomic<bool>&,
                 const std::function<void(std::uint64_t, std::uint64_t, std::uint64_t)>&,
                 const std::function<void(steam::InstallStep)>&) override {
    return {false, false, "No downloads here."};
  }
  std::string Remove(const std::string&) override { return {}; }
  Result Update(const GameId&, const std::string&, const steam::InstallOptions& options, const std::atomic<bool>& cancel,
                const std::function<void(std::uint64_t, std::uint64_t, std::uint64_t)>&,
                const std::function<void(steam::InstallStep)>& step, const std::function<bool()>&) override {
    {
      std::lock_guard lock(mutex);
      asked = options;
    }
    if (step) step(steam::InstallStep::kDownloading);
    while (hold && !cancel) std::this_thread::sleep_for(1ms);
    Result result;
    result.ok = !cancel;
    result.canceled = cancel;
    return result;
  }
};

void Write(const fs::path& path, const std::string& text) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << text;
}

const ViewRow* Row(const Simulation& model, const std::string& key) {
  const auto& rows = model.view().rows;
  const auto found = std::find_if(rows.begin(), rows.end(), [&](const ViewRow& row) { return row.key == key; });
  return found == rows.end() ? nullptr : &*found;
}
bool Choose(Simulation& model, const std::string& key) { return model.action("focus:" + key) && model.action("cross"); }

struct Fixture {
  fs::path root;
  Simulation model{{}, {}, ModelMode::Live};
  Sessions sessions;
  Auth auth;
  Runner runner;
  std::unique_ptr<steam::InstallService> installs;
  std::unique_ptr<steam::WorkshopListingService> listing;
  std::unique_ptr<Controller> controller;
  bool bottle = true;

  explicit Fixture(bool with_listing) {
    char name[] = "/tmp/playden-workshop-wiring-XXXXXX";
    CHECK(mkdtemp(name));
    root = fs::canonical(name);
    const auto app = std::to_string(kApp);
    Write(root / "games" / app / installs::kInstallRecordName,
          "{\"version\":2,\"app_id\":" + app + ",\"install_bytes\":4096,\"depots\":[]}\n");
    Write(root / "games" / ".playden-games", app + "\tinternal\t4096\n");
    fs::create_directories(root / "bottle" / "drive_c");
    installs = std::make_unique<steam::InstallService>(runner, (root / "games").string());
    if (with_listing) {
      steam::WorkshopListingConfig config;
      config.bottle = [this](std::uint32_t) -> std::optional<std::string> {
        if (!bottle) return std::nullopt;
        return (root / "bottle").string();
      };
      config.subscriptions = [](const StoredSession&, std::uint32_t app_id, const std::atomic<bool>&) {
        steam::WorkshopSubscriptions subscriptions;
        subscriptions.error = steam::CmError::kNone;
        for (std::uint64_t id : {1, 2}) {
          steam::WorkshopItem item;
          item.id = id;
          item.title = id == 1 ? "First" : "Second";
          item.manifest = 100 + id;
          item.file_size = 1'000'000 * id;
          item.consumer_appid = app_id;
          subscriptions.items.push_back(item);
        }
        return subscriptions;
      };
      listing = std::make_unique<steam::WorkshopListingService>(sessions, std::move(config));
    }
    controller = std::make_unique<Controller>(model, auth, sessions, nullptr, nullptr, nullptr, installs.get());
    if (listing) controller->SetWorkshopListing(listing.get());
    Until([&] { return model.state().account().authentication == AuthState::SignedIn; });
    GameSummary game;
    game.id = GameId::Steam(kApp);
    game.title = "Brotato";
    CHECK(model.UpdateLibrary({{game}, false, {}, false, true, 100}));
    Until([&] {
      return std::any_of(model.state().installations.begin(), model.state().installations.end(),
                         [](const Installation& value) { return value.game_id == GameId::Steam(kApp); });
    });
  }
  ~Fixture() {
    runner.hold = false;
    controller->Shutdown();
    if (listing) listing->Shutdown();
    installs->Shutdown();
    fs::remove_all(root);
  }
  template <typename Done> bool Until(Done done) {
    const auto deadline = std::chrono::steady_clock::now() + 4s;
    while (std::chrono::steady_clock::now() < deadline) {
      controller->Tick();
      if (done()) return true;
      std::this_thread::sleep_for(1ms);
    }
    return false;
  }
  void OpenSheet() {
    while (!model.state().navigation.modals.empty()) model.action("circle");
    CHECK(model.action("details:" + std::to_string(kApp)));
    CHECK(Choose(model, "details-more"));
    CHECK(Choose(model, "workshop-items"));
    CHECK(model.view().surface == "workshop-items");
  }
};

void ListingAndApply() {
  Fixture fixture(true);
  fixture.OpenSheet();
  CHECK(fixture.Until([&] { return Row(fixture.model, "workshop-item-1") != nullptr; }));
  const auto* first = Row(fixture.model, "workshop-item-1");
  CHECK(first && first->title == "First" && first->selected && first->subtitle == "Downloads");
  CHECK(Choose(fixture.model, "workshop-item-1"));
  CHECK(Choose(fixture.model, "workshop-apply"));
  CHECK(fixture.Until([&] {
    std::lock_guard lock(fixture.runner.mutex);
    return fixture.runner.asked.has_value();
  }));
  {
    std::lock_guard lock(fixture.runner.mutex);
    CHECK(fixture.runner.asked && fixture.runner.asked->workshop &&
          fixture.runner.asked->workshop_off == std::optional<std::vector<std::uint64_t>>(std::vector<std::uint64_t>{1}));
  }
  // Queue and the game's page say Workshop items while it runs.
  CHECK(fixture.Until([&] {
    const auto& jobs = fixture.model.state().jobs;
    return std::any_of(jobs.begin(), jobs.end(), [](const Job& job) {
      return job.game_id == GameId::Steam(kApp) && job.stage_text == "Workshop items";
    });
  }));
  const auto* note = Row(fixture.model, "details-updating");
  CHECK(note && note->title == "Workshop items");
  fixture.runner.hold = false;
  CHECK(fixture.Until([&] {
    const auto& notices = fixture.model.state().notices;
    return std::any_of(notices.begin(), notices.end(), [](const auto& notice) { return notice.message == "Workshop items changed"; });
  }));
}

void ErrorsBecomeSentences() {
  Fixture fixture(true);
  fixture.bottle = false;
  fixture.OpenSheet();
  CHECK(fixture.Until([&] { return Row(fixture.model, "workshop-error") != nullptr; }));
  CHECK(fixture.model.view().subtitle == steam::kWorkshopNoBottle);
  // Try again, with a bottle now.
  fixture.bottle = true;
  CHECK(Choose(fixture.model, "workshop-retry"));
  CHECK(fixture.Until([&] { return Row(fixture.model, "workshop-item-2") != nullptr; }));
}

void NoListingService() {
  Fixture fixture(false);
  fixture.OpenSheet();
  CHECK(fixture.Until([&] { return Row(fixture.model, "workshop-error") != nullptr; }));
  CHECK(!Row(fixture.model, "workshop-loading"));
  CHECK(fixture.model.view().subtitle == "Playden can't list Workshop items here.");
}

}  // namespace

int main() {
  ListingAndApply();
  ErrorsBecomeSentences();
  NoListingService();
  if (failures) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("homebrew_workshop_wiring: ok\n");
  return 0;
}
