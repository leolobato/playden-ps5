// SPDX-License-Identifier: GPL-3.0-or-later
// The Workshop items sheet's listing (#856): subscriptions next to what Playden holds in the bottle, each item's
// action with it on and off, and the worker's errors (no bottle, no sign-in, Steam out of reach), its last-request-wins
// queue and its account change. Built twice: as the host runs it and with the console's absolute-path ManagedDirectory.
#include "steam/backend/workshop_listing_service.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

using namespace playden;
using namespace playden::steam;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

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

void Write(const fs::path& path, const std::string& text) {
  fs::create_directories(path.parent_path());
  FILE* file = std::fopen(path.c_str(), "wb");
  CHECK(file);
  if (!file) return;
  std::fwrite(text.data(), 1, text.size(), file);
  std::fclose(file);
}

struct Fixture {
  fs::path root, prefix, workshop, content;
  Fixture() {
    char name[] = "/tmp/playden-workshop-listing-XXXXXX";
    CHECK(mkdtemp(name));
    root = fs::canonical(name);  // /tmp is a link on macOS; ManagedDirectory never follows one
    prefix = root / "bottle";
    fs::create_directories(prefix / "drive_c");
    workshop = WorkshopFolder(prefix.string());
    content = workshop / "content" / std::to_string(kApp);
  }
  ~Fixture() {
    std::error_code ignored;
    fs::remove_all(root, ignored);
  }
};

WorkshopItem Item(std::uint64_t id, std::uint64_t manifest, std::uint32_t updated, std::uint64_t size,
                  std::string title) {
  WorkshopItem item;
  item.id = id;
  item.manifest = manifest;
  item.time_updated = updated;
  item.file_size = size;
  item.title = std::move(title);
  item.consumer_appid = kApp;
  return item;
}

WorkshopSubscriptions Subscribed(std::vector<WorkshopItem> items) {
  WorkshopSubscriptions subscriptions;
  subscriptions.error = CmError::kNone;
  subscriptions.items = std::move(items);
  subscriptions.total = static_cast<std::uint32_t>(subscriptions.items.size());
  return subscriptions;
}

const WorkshopListingItem* Find(const WorkshopListing& listing, std::uint64_t id) {
  for (const auto& item : listing.items)
    if (item.id == id) return &item;
  return nullptr;
}

void TestNoBottle() {
  Fixture fixture;
  const auto listing = ListWorkshopItems(kApp, (fixture.root / "missing").string(), Subscribed({Item(1, 9, 1, 10, "A")}));
  CHECK(listing.error == WorkshopListingError::kNoBottle);
  CHECK(listing.items.empty());
  CHECK(ListWorkshopItems(kApp, "", Subscribed({})).error == WorkshopListingError::kNoBottle);
}

// A bottle that never had items: every downloadable item downloads when on, nothing to delete.
void TestFreshBottle() {
  Fixture fixture;
  auto legacy = Item(3, 0, 1, 30, "Legacy");
  legacy.file_url = "https://example.invalid/legacy";
  const auto listing = ListWorkshopItems(kApp, fixture.prefix.string(),
                                         Subscribed({Item(1, 11, 1, 100, "One"), Item(2, 22, 1, 200, "Two"), legacy}));
  CHECK(listing.error == WorkshopListingError::kNone);
  CHECK(listing.items.size() == 3);
  CHECK(listing.saved_off.empty());
  CHECK(!fs::exists(fixture.workshop));  // reading made nothing
  const auto* one = Find(listing, 1);
  CHECK(one && one->subscribed && one->on && one->on_action == WorkshopAction::kDownload && one->on_bytes == 100 &&
        one->off_action == WorkshopAction::kSkipOff && one->off_bytes == 0 && one->title == "One");
  const auto* old = Find(listing, 3);
  CHECK(old && old->on_action == WorkshopAction::kSkipLegacy);
}

// Every state the sheet shows: current, changed on Steam, files gone, placed by hand, turned off, unsubscribed.
void TestHeldItems() {
  Fixture fixture;
  WorkshopState state;
  state.items.push_back({1, 11, 5, 1000, true, "Current"});
  state.items.push_back({2, 22, 5, 2000, true, "Changed"});
  state.items.push_back({3, 33, 5, 3000, true, "Files gone"});
  state.items.push_back({6, 66, 5, 6000, true, "Unsubscribed"});
  fs::create_directories(fixture.workshop);
  CHECK(WriteWorkshopState(fixture.workshop.string(), kApp, state));
  CHECK(WriteWorkshopOff(fixture.workshop.string(), kApp, {5, 99}));
  for (const char* id : {"1", "2", "4", "6"}) fs::create_directories(fixture.content / id);
  Write(fixture.content / "1" / "mod.pck", "x");

  const auto listing = ListWorkshopItems(kApp, fixture.prefix.string(),
                                         Subscribed({Item(1, 11, 5, 1100, "Current"), Item(2, 23, 6, 2200, "Changed"),
                                                     Item(3, 33, 5, 3300, "Files gone"), Item(4, 44, 5, 4400, "By hand"),
                                                     Item(5, 55, 5, 5500, "Off")}));
  CHECK(listing.error == WorkshopListingError::kNone);
  CHECK(listing.items.size() == 6);
  CHECK((listing.saved_off == std::vector<std::uint64_t>{5, 99}));
  const auto* current = Find(listing, 1);
  CHECK(current && current->on_action == WorkshopAction::kKeep && current->off_action == WorkshopAction::kRemove &&
        current->off_bytes == 1000);
  const auto* changed = Find(listing, 2);
  CHECK(changed && changed->on_action == WorkshopAction::kUpdate && changed->on_bytes == 2200 &&
        changed->off_action == WorkshopAction::kRemove && changed->off_bytes == 2000);
  const auto* gone = Find(listing, 3);
  CHECK(gone && gone->on_action == WorkshopAction::kDownload && gone->on_bytes == 3300);
  const auto* hand = Find(listing, 4);
  CHECK(hand && hand->on_action == WorkshopAction::kSkipHandPlaced && hand->off_action == WorkshopAction::kSkipOff);
  const auto* off = Find(listing, 5);
  CHECK(off && !off->on && off->on_action == WorkshopAction::kDownload && off->off_action == WorkshopAction::kSkipOff);
  const auto* unsubscribed = Find(listing, 6);
  CHECK(unsubscribed && !unsubscribed->subscribed && unsubscribed->on_action == WorkshopAction::kRemove &&
        unsubscribed->off_action == WorkshopAction::kRemove && unsubscribed->on_bytes == 6000 &&
        unsubscribed->title == "Unsubscribed");
  CHECK(listing.items.back().id == 6);  // after the subscriptions

  // A list Steam cut short: an item it doesn't name may just be past the cut, and stays unless turned off.
  auto cut = Subscribed({Item(1, 11, 5, 1100, "Current")});
  cut.truncated = true;
  const auto partial = ListWorkshopItems(kApp, fixture.prefix.string(), cut);
  CHECK(partial.truncated);
  const auto* kept = Find(partial, 6);
  CHECK(kept && kept->on_action == WorkshopAction::kKeep && kept->off_action == WorkshopAction::kRemove);

  // A damaged state or choice reads as nothing held, nothing off: the sync sets the state aside the same way.
  Write(fixture.workshop / WorkshopStateName(kApp), "not a state\n");
  Write(fixture.workshop / WorkshopOffName(kApp), "007\n");
  const auto damaged = ListWorkshopItems(kApp, fixture.prefix.string(), Subscribed({Item(1, 11, 5, 1100, "Current")}));
  CHECK(damaged.error == WorkshopListingError::kNone && damaged.saved_off.empty());
  CHECK(damaged.items.size() == 1 && damaged.items[0].on_action == WorkshopAction::kSkipHandPlaced);
}

// An item Valve's client downloaded in Online (#856) shows as one Playden holds, as the next sync adopts it; reading
// writes nothing.
void TestSteamClientItem() {
  Fixture fixture;
  fs::create_directories(fixture.content / "5");
  fs::create_directories(fixture.content / "6");
  Write(fixture.workshop / "appworkshop_1942280.acf",
        "\"AppWorkshop\"\n{\n\t\"appid\"\t\t\"1942280\"\n\t\"WorkshopItemsInstalled\"\n\t{\n\t\t\"5\"\n\t\t{\n"
        "\t\t\t\"size\"\t\t\"500\"\n\t\t\t\"timeupdated\"\t\t\"5\"\n\t\t\t\"manifest\"\t\t\"55\"\n\t\t}\n\t}\n}\n");
  const auto listing = ListWorkshopItems(kApp, fixture.prefix.string(),
                                         Subscribed({Item(5, 55, 5, 500, "Valve's"), Item(6, 66, 5, 600, "By hand")}));
  const auto* valve = Find(listing, 5);
  CHECK(valve && valve->on_action == WorkshopAction::kKeep && valve->off_action == WorkshopAction::kRemove &&
        valve->off_bytes == 500);
  const auto* hand = Find(listing, 6);
  CHECK(hand && hand->on_action == WorkshopAction::kSkipHandPlaced);
  CHECK(!fs::exists(fixture.workshop / WorkshopStateName(kApp)));
}

class Sessions final : public SessionRepository {
 public:
  bool signed_in = true;
  Result<std::optional<StoredSession>> Load() const override {
    return Result<std::optional<StoredSession>>::Success(
        signed_in ? std::optional<StoredSession>(StoredSession{1, "", "r", "a", {}}) : std::nullopt);
  }
  Result<void> Save(const StoredSession&) const override { return Result<void>::Success(); }
  Result<void> Clear() const override { return Result<void>::Success(); }
};

std::vector<WorkshopListing> Await(WorkshopListingService& service, std::size_t count = 1) {
  std::vector<WorkshopListing> all;
  for (int i = 0; i < 3000 && all.size() < count; ++i) {
    for (auto& listing : service.Pump()) all.push_back(std::move(listing));
    if (all.size() < count) std::this_thread::sleep_for(1ms);
  }
  return all;
}

void TestService() {
  Fixture fixture;
  Sessions sessions;
  std::atomic<int> asked{0};
  std::atomic<CmError> answer{CmError::kNone};
  std::atomic<bool> block{false}, entered{false};
  std::vector<std::string> logs;
  std::mutex log_mutex;
  WorkshopListingConfig config;
  config.bottle = [&fixture](std::uint32_t app) -> std::optional<std::string> {
    if (app == kApp) return fixture.prefix.string();
    return std::nullopt;
  };
  config.subscriptions = [&](const StoredSession& session, std::uint32_t, const std::atomic<bool>& cancel) {
    ++asked;
    CHECK(session.refresh_token == "r");
    entered = true;
    while (block && !cancel) std::this_thread::sleep_for(1ms);
    WorkshopSubscriptions result;
    if (cancel) {
      result.error = CmError::kCanceled;
      return result;
    }
    result = Subscribed({Item(1, 11, 1, 100, "Secret title")});
    result.error = answer.load();
    return result;
  };
  config.log = [&](const std::string& line) {
    std::lock_guard<std::mutex> lock(log_mutex);
    logs.push_back(line);
  };
  WorkshopListingService service(sessions, config);

  // No bottle: Steam isn't asked.
  service.Request(7);
  auto done = Await(service);
  CHECK(done.size() == 1 && done[0].app_id == 7 && done[0].error == WorkshopListingError::kNoBottle);
  CHECK(asked == 0);

  service.Request(kApp);
  done = Await(service);
  CHECK(done.size() == 1 && done[0].error == WorkshopListingError::kNone && done[0].items.size() == 1);
  CHECK(asked == 1);

  sessions.signed_in = false;
  service.Request(kApp);
  done = Await(service);
  CHECK(done.size() == 1 && done[0].error == WorkshopListingError::kNoSession && asked == 1);
  sessions.signed_in = true;

  answer = CmError::kNetwork;
  service.Request(kApp);
  done = Await(service);
  CHECK(done.size() == 1 && done[0].error == WorkshopListingError::kNetwork && done[0].items.empty());
  answer = CmError::kProtocol;
  service.Request(kApp);
  done = Await(service);
  CHECK(done.size() == 1 && done[0].error == WorkshopListingError::kSteam);
  answer = CmError::kNone;

  // The account changes while Steam is asked: that answer never arrives; one asked after does.
  block = true;
  entered = false;
  service.Request(kApp);
  for (int i = 0; i < 3000 && !entered; ++i) std::this_thread::sleep_for(1ms);
  CHECK(entered);
  service.Invalidate();
  block = false;
  std::this_thread::sleep_for(20ms);
  CHECK(service.Pump().empty());
  service.Request(kApp);
  done = Await(service);
  CHECK(done.size() == 1 && done[0].error == WorkshopListingError::kNone);

  // Requests while one runs: each game waits once, oldest first; a background request for another game doesn't push
  // out the one asked before.
  block = true;
  entered = false;
  service.Request(kApp);
  for (int i = 0; i < 3000 && !entered; ++i) std::this_thread::sleep_for(1ms);
  service.Request(kApp);
  service.Request(7);
  service.Request(kApp);
  block = false;
  done = Await(service, 3);
  CHECK(done.size() == 3 && done[0].app_id == kApp && done[1].app_id == kApp && done[2].app_id == 7);
  std::this_thread::sleep_for(20ms);
  CHECK(service.Pump().empty());
  // Past the cap the oldest waiting game is dropped.
  block = true;
  entered = false;
  service.Request(kApp);
  for (int i = 0; i < 3000 && !entered; ++i) std::this_thread::sleep_for(1ms);
  for (std::uint32_t app = 100; app < 100 + kWorkshopListingMaxPending + 1; ++app) service.Request(app);
  block = false;
  done = Await(service, kWorkshopListingMaxPending + 1);
  CHECK(done.size() == kWorkshopListingMaxPending + 1 && done[0].app_id == kApp && done[1].app_id == 101 &&
        done.back().app_id == 100 + kWorkshopListingMaxPending);
  std::this_thread::sleep_for(20ms);
  CHECK(service.Pump().empty());

  service.Shutdown();
  std::lock_guard<std::mutex> lock(log_mutex);
  CHECK(!logs.empty());
  for (const auto& line : logs) CHECK(line.find("Secret") == std::string::npos && line.find(fixture.root.string()) == std::string::npos);
}

}  // namespace

int main() {
  TestNoBottle();
  TestFreshBottle();
  TestHeldItems();
  TestSteamClientItem();
  TestService();
  if (failures) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("workshop_listing_service: ok\n");
  return 0;
}
