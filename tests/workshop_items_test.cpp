// SPDX-License-Identifier: GPL-3.0-or-later
// Steam Workshop items in the bottle (#856): the state file, the sync plan, downloads through DepotInstaller into
// staging, the swap into content/<appid>/<id>/, resuming after an interruption, refused items, and hand-placed
// folders left alone. Built twice: as the host runs it and with the console's absolute-path ManagedDirectory.
#include "steam/backend/depot_chunk.h"
#include "steam/backend/workshop_items.h"
#include "runtime/workshop_acf.h"

#include <mbedtls/aes.h>
#include <mbedtls/sha1.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

using namespace playden;
using namespace playden::steam;
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
constexpr std::uint32_t kDepot = 1942281;

void Write(const fs::path& path, const std::string& text) {
  fs::create_directories(path.parent_path());
  FILE* file = std::fopen(path.c_str(), "wb");
  CHECK(file);
  if (!file) return;
  std::fwrite(text.data(), 1, text.size(), file);
  std::fclose(file);
}

std::string Read(const fs::path& path) {
  FILE* file = std::fopen(path.c_str(), "rb");
  if (!file) return "<missing>";
  std::string text;
  char buffer[4096];
  std::size_t got;
  while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) text.append(buffer, got);
  std::fclose(file);
  return text;
}

bool Here(const fs::path& path) {
  struct stat info{};
  return ::lstat(path.c_str(), &info) == 0;
}

mode_t Mode(const fs::path& path) {
  struct stat info{};
  return ::lstat(path.c_str(), &info) == 0 ? (info.st_mode & 07777) : 0;
}

struct Fixture {
  fs::path root, prefix, workshop, content;
  Fixture() {
    char name[] = "/tmp/playden-workshop-XXXXXX";
    CHECK(mkdtemp(name));
    root = fs::canonical(name);  // /tmp is a link on macOS; ManagedDirectory never follows one
    prefix = root / "bottle";
    fs::create_directories(prefix / "drive_c");
    CHECK(EnsureWorkshopFolder(prefix.string()));
    workshop = WorkshopFolder(prefix.string());
    content = workshop / "content" / std::to_string(kApp);
  }
  ~Fixture() {
    std::error_code ignored;
    fs::remove_all(root, ignored);
  }
  fs::path Item(std::uint64_t id) const { return content / std::to_string(id); }
  fs::path Staging(std::uint64_t id) const { return workshop / ".playden-download" / std::to_string(id); }
  // Playden's swap folders for the item, in its own workspace: ".new" or ".old".
  fs::path Swap(std::uint64_t id, const char* suffix) const { return Staging(id).string() + suffix; }
  WorkshopState State() const {
    const auto state = ReadWorkshopState(workshop.string(), kApp);
    CHECK(state.has_value());
    return state ? *state : WorkshopState{};
  }
};

WorkshopItem Item(std::uint64_t id, std::uint64_t manifest, std::uint32_t updated = 1000, std::uint64_t size = 100) {
  WorkshopItem item;
  item.id = id;
  item.manifest = manifest;
  item.time_updated = updated;
  item.file_size = size;
  item.consumer_appid = kApp;
  item.title = "Item " + std::to_string(id);
  return item;
}

// Writes what DepotInstaller leaves in a finished install: the files, the file list, the tree and the record.
struct FakeDownloads {
  std::map<std::uint64_t, std::map<std::string, std::string>> files;  // by manifest: path -> content
  std::vector<std::uint64_t> calls;
  std::map<std::uint64_t, InstallError> fail;  // by manifest
  std::set<std::uint64_t> partial;             // write the first file, then fail kNetwork
  std::vector<std::vector<std::string>> seen;  // what each call found in its staging root before writing

  WorkshopDownloader Downloader() {
    return [this](const WorkshopItem& item, const std::string& staging, const std::atomic<bool>&,
                  const WorkshopProgressFn& progress) -> WorkshopDownloadResult {
      calls.push_back(item.manifest);
      std::vector<std::string> found;
      for (const auto& entry : fs::recursive_directory_iterator(staging))
        found.push_back(fs::relative(entry.path(), staging).string());
      std::sort(found.begin(), found.end());
      seen.push_back(found);
      if (progress) progress(InstallProgress{});
      const auto& content = files[item.manifest];
      if (partial.count(item.manifest)) {
        partial.erase(item.manifest);
        Write(fs::path(staging) / content.begin()->first, content.begin()->second);
        return {InstallError::kNetwork, false, "network"};
      }
      if (const auto failure = fail.find(item.manifest); failure != fail.end()) return {failure->second, false, "failed"};
      std::string list, tree = "v1\n";
      for (const auto& [path, text] : content) {
        Write(fs::path(staging) / path, text);
        list += "f\t" + path + "\n";
        tree += "0000000000000000000000000000000000000000\t" + std::to_string(text.size()) + "\t" + path + "\n";
      }
      Write(fs::path(staging) / ".playden-files", list);
      Write(fs::path(staging) / ".playden-tree", tree);
      Write(fs::path(staging) / ".playden-install.json", "{}\n");
      return {};
    };
  }
};

WorkshopSyncRequest Request(const Fixture& fixture, FakeDownloads& downloads, std::vector<WorkshopItem> subscribed,
                            std::set<std::uint64_t> off = {}) {
  WorkshopSyncRequest request;
  request.app_id = kApp;
  request.workshop_folder = fixture.workshop.string();
  request.subscribed = std::move(subscribed);
  request.off = std::move(off);
  request.download = downloads.Downloader();
  return request;
}

const WorkshopItemOutcome* OutcomeOf(const WorkshopSyncResult& result, std::uint64_t id) {
  for (const auto& item : result.items)
    if (item.id == id) return &item;
  return nullptr;
}

void TestEnsureFolder() {
  Fixture fixture;
  const auto steam = fixture.prefix / "drive_c/Program Files (x86)/Steam";
  CHECK(fs::is_directory(fixture.workshop));
  CHECK(Mode(fixture.prefix / "drive_c/Program Files (x86)") == 0755);
  CHECK(Mode(steam / "steamapps") == 0755);
  CHECK(Mode(fixture.workshop) == 0755);
  // What the title's steam_view_setup does on every start still works.
  CHECK(::mkdir((fixture.prefix / "drive_c/Program Files (x86)").c_str(), 0777) != 0 && errno == EEXIST);
  CHECK(::mkdir((steam / "steamapps").c_str(), 0777) != 0 && errno == EEXIST);
  CHECK(::mkdir((steam / "steamapps/common").c_str(), 0777) == 0);
  CHECK(EnsureWorkshopFolder(fixture.prefix.string()));
  // A link in the chain is refused.
  const auto other = fixture.root / "other";
  fs::create_directories(other / "drive_c");
  fs::create_directories(fixture.root / "linked");
  fs::create_symlink(other / "drive_c", fixture.root / "linked/drive_c");
  CHECK(!EnsureWorkshopFolder((fixture.root / "linked").string()));
  CHECK(!Here(other / "drive_c/Program Files (x86)"));
}

void TestStateFile() {
  WorkshopState state;
  state.items.push_back({3000000001ull, 0x1122334455667788ull, 1700000100, 4096, true, "Extra, characters: \xC3\xA9"});
  state.items.push_back({12, 0, 0, 0, false, ""});
  const auto text = FormatWorkshopState(state);
  CHECK(text == "item=3000000001,1234605616436508552,1700000100,4096,on,Extra, characters: \xC3\xA9\n"
                "item=12,0,0,0,off,\n");
  const auto parsed = ParseWorkshopState(text);
  CHECK(parsed && parsed->items.size() == 2);
  if (parsed && parsed->items.size() == 2) {
    CHECK(parsed->items[0].id == 3000000001ull && parsed->items[0].manifest == 0x1122334455667788ull);
    CHECK(parsed->items[0].time_updated == 1700000100 && parsed->items[0].bytes == 4096 && parsed->items[0].on);
    CHECK(parsed->items[0].title == "Extra, characters: \xC3\xA9");
    CHECK(!parsed->items[1].on && parsed->items[1].title.empty());
  }
  CHECK(ParseWorkshopState("") && ParseWorkshopState("")->items.empty());

  // A title can't break the line: Format sanitises it.
  WorkshopState hostile;
  hostile.items.push_back({7, 1, 2, 3, true, "two\nlines\r\x01"});
  CHECK(FormatWorkshopState(hostile) == "item=7,1,2,3,on,twolines\n");

  for (const char* bad : {
           "item=1,2,3,4,on,x",             // no final newline
           "\n",                            // empty line
           "items=1,2,3,4,on,x\n",          // other key
           "item=0,2,3,4,on,x\n",           // zero ID
           "item=01,2,3,4,on,x\n",          // leading zero
           "item=-1,2,3,4,on,x\n",          // sign
           "item=1,2,3,4,yes,x\n",          // flag
           "item=1,2,3,on,x\n",             // missing field
           "item=1,2,99999999999,4,on,x\n", // time beyond 32 bits
           "item=1,18446744073709551616,3,4,on,x\n",  // manifest beyond 64 bits
           "item=1,2,3,4,on,a\tb\n",        // control character in the title
           "item=1,2,3,4,on,\xFF\n",        // invalid UTF-8
           "item=1,2,3,4,on,x\nitem=1,5,6,7,on,y\n",  // repeated ID
       })
    CHECK(!ParseWorkshopState(bad));
  std::string many;
  for (std::size_t index = 1; index <= kWorkshopMaxItems + 1; ++index) many += "item=" + std::to_string(index) + ",1,1,1,on,\n";
  CHECK(!ParseWorkshopState(many));
  CHECK(!ParseWorkshopState(std::string(kWorkshopStateMaxBytes + 1, 'x')));

  Fixture fixture;
  const auto path = fixture.workshop / WorkshopStateName(kApp);
  CHECK(WorkshopStateName(kApp) == ".playden-workshop-1942280");
  CHECK(ReadWorkshopState(fixture.workshop.string(), kApp) && ReadWorkshopState(fixture.workshop.string(), kApp)->items.empty());
  CHECK(WriteWorkshopState(fixture.workshop.string(), kApp, state));
  CHECK(Read(path) == text);
  CHECK(Mode(path) == 0644);
  CHECK(!Here(fixture.workshop / (WorkshopStateName(kApp) + ".tmp")));
  const auto back = ReadWorkshopState(fixture.workshop.string(), kApp);
  CHECK(back && back->items.size() == 2);
  Write(path, "item=1,2,3,4,maybe,x\n");
  CHECK(!ReadWorkshopState(fixture.workshop.string(), kApp));
}

void TestPlan() {
  WorkshopState state;
  state.items.push_back({2, 20, 1000, 200, true, "changed manifest"});
  state.items.push_back({3, 30, 1000, 300, true, "changed time"});
  state.items.push_back({4, 40, 1000, 400, true, "current"});
  state.items.push_back({5, 50, 1000, 500, true, "turned off"});
  state.items.push_back({9, 90, 1000, 900, true, "unsubscribed"});
  state.items.push_back({10, 100, 1000, 1000, true, "folder gone"});
  state.items.push_back({11, 110, 1000, 1100, true, "now legacy"});
  const std::vector<WorkshopItem> subscribed = {
      Item(1, 10, 1000, 111), Item(2, 21, 1000, 222), Item(3, 30, 1001, 333), Item(4, 40, 1000, 444),
      Item(5, 50, 1000, 555), Item(6, 60), Item(7, 70), Item(8, 0), Item(10, 100, 1000, 1010), Item(11, 0),
      Item(1, 10)};  // a repeat is ignored
  const std::set<std::uint64_t> off = {5, 6};
  const std::set<std::uint64_t> present = {2, 3, 4, 5, 7, 9, 11};
  const auto plan = PlanWorkshopSync(subscribed, state, off, present);
  std::map<std::uint64_t, WorkshopAction> actions;
  for (const auto& entry : plan.entries) actions[entry.item.id] = entry.action;
  CHECK(plan.entries.size() == 11);
  CHECK(actions[1] == WorkshopAction::kDownload);
  CHECK(actions[2] == WorkshopAction::kUpdate);
  CHECK(actions[3] == WorkshopAction::kUpdate);
  CHECK(actions[4] == WorkshopAction::kKeep);
  CHECK(actions[5] == WorkshopAction::kRemove);
  CHECK(actions[6] == WorkshopAction::kSkipOff);
  CHECK(actions[7] == WorkshopAction::kSkipHandPlaced);
  CHECK(actions[8] == WorkshopAction::kSkipLegacy);
  CHECK(actions[9] == WorkshopAction::kRemove);
  CHECK(actions[10] == WorkshopAction::kDownload);
  CHECK(actions[11] == WorkshopAction::kKeep);
  CHECK(plan.download_bytes == 111 + 222 + 333 + 1010);
  CHECK(plan.freed_bytes == 500 + 900);
  CHECK(plan.downloads == 4 && plan.removals == 2);
  CHECK(plan.entries.back().item.id == 9 && plan.entries.back().item.title == "unsubscribed");

  // A banned item: Playden's copy is kept (also when Steam changed it), removed only when turned off; one Playden
  // doesn't hold is not planned, whatever folder is there.
  {
    WorkshopState held;
    held.items.push_back({21, 210, 1000, 2100, true, "banned, held"});
    held.items.push_back({22, 220, 1000, 2200, true, "banned, held, off"});
    auto banned = [](std::uint64_t id, std::uint64_t manifest) {
      auto item = Item(id, manifest);
      item.banned = true;
      return item;
    };
    const auto banned_plan = PlanWorkshopSync({banned(21, 211), banned(22, 220), banned(23, 230), banned(24, 240)}, held,
                                              {22}, {21, 22, 24});
    std::map<std::uint64_t, WorkshopAction> got;
    for (const auto& entry : banned_plan.entries) got[entry.item.id] = entry.action;
    CHECK(banned_plan.entries.size() == 2 && got.size() == 2);
    CHECK(got[21] == WorkshopAction::kKeep && got[22] == WorkshopAction::kRemove);
    CHECK(banned_plan.downloads == 0 && banned_plan.removals == 1 && banned_plan.freed_bytes == 2200);
  }
}

void TestSync() {
  Fixture fixture;
  FakeDownloads downloads;
  downloads.files[10] = {{"mod.zip", "first"}, {"Data/readme.txt", "hello"}};
  downloads.files[11] = {{"mod.zip", "second"}};
  downloads.files[20] = {{"other.pck", "two"}};
  // A folder the player put there by hand, for a subscribed item and for another one.
  Write(fixture.Item(7) / "mine.txt", "hand placed");
  Write(fixture.Item(8) / "mine.txt", "hand placed too");
  Write(fixture.content / "notes.txt", "player's");

  std::atomic<bool> cancel{false};
  std::vector<WorkshopSyncProgress> reports;
  auto request = Request(fixture, downloads, {Item(1, 10, 1000, 100), Item(2, 20, 1000, 50), Item(7, 70)});
  request.progress = [&reports](const WorkshopSyncProgress& progress) { reports.push_back(progress); };
  std::vector<std::string> lines;
  request.log = [&lines](const std::string& line) { lines.push_back(line); };
  auto result = SyncWorkshopItems(request, cancel);
  CHECK(result.error == InstallError::kNone);
  CHECK(OutcomeOf(result, 1) && OutcomeOf(result, 1)->outcome == WorkshopOutcome::kDownloaded);
  CHECK(OutcomeOf(result, 2) && OutcomeOf(result, 2)->outcome == WorkshopOutcome::kDownloaded);
  CHECK(OutcomeOf(result, 7) && OutcomeOf(result, 7)->outcome == WorkshopOutcome::kSkippedHandPlaced);
  CHECK(Read(fixture.Item(1) / "mod.zip") == "first");
  CHECK(Read(fixture.Item(1) / "Data/readme.txt") == "hello");
  CHECK(!Here(fixture.Item(1) / ".playden-files") && !Here(fixture.Item(1) / ".playden-tree"));
  CHECK(!Here(fixture.Item(1) / ".playden-install.json"));
  CHECK(!Here(fixture.Staging(1)) && !Here(fixture.Swap(1, ".new")) && !Here(fixture.Swap(1, ".old")));
  CHECK(Read(fixture.Item(7) / "mine.txt") == "hand placed");
  CHECK(downloads.calls == (std::vector<std::uint64_t>{10, 20}));
  CHECK(reports.size() == 2 && reports[1].index == 1 && reports[1].count == 2 && reports[1].bytes_done == 100 &&
        reports[1].bytes_total == 150 && reports[1].item_id == 2);
  CHECK(!lines.empty() && lines.back().find("workshop sync done") == 0);
  for (const auto& line : lines) CHECK(line.find("Item ") == std::string::npos);
  auto state = fixture.State();
  CHECK(state.items.size() == 2 && state.Find(1) && state.Find(1)->manifest == 10 && state.Find(1)->bytes == 100);
  CHECK(!state.Find(7));

  // Nothing changed: nothing is downloaded.
  result = SyncWorkshopItems(Request(fixture, downloads, {Item(1, 10, 1000, 100), Item(2, 20, 1000, 50), Item(7, 70)}), cancel);
  CHECK(result.error == InstallError::kNone && downloads.calls.size() == 2);
  CHECK(OutcomeOf(result, 1)->outcome == WorkshopOutcome::kKept);

  // An update replaces the whole folder (a file the new version dropped goes); item 2 is turned off; the
  // hand-placed item 7 stays when unsubscribed.
  result = SyncWorkshopItems(Request(fixture, downloads, {Item(1, 11, 1001, 60), Item(2, 20, 1000, 50)}, {2}), cancel);
  CHECK(result.error == InstallError::kNone);
  CHECK(OutcomeOf(result, 1)->outcome == WorkshopOutcome::kUpdated);
  CHECK(OutcomeOf(result, 2)->outcome == WorkshopOutcome::kRemoved);
  CHECK(result.plan.freed_bytes == 50 && result.plan.download_bytes == 60);
  CHECK(Read(fixture.Item(1) / "mod.zip") == "second");
  CHECK(!Here(fixture.Item(1) / "Data"));
  CHECK(!Here(fixture.Item(2)));
  CHECK(Read(fixture.Item(7) / "mine.txt") == "hand placed");
  state = fixture.State();
  CHECK(state.items.size() == 1 && state.Find(1)->manifest == 11 && state.Find(1)->time_updated == 1001);

  // Unsubscribed: Playden's copy goes, the player's folders and files stay.
  result = SyncWorkshopItems(Request(fixture, downloads, {}), cancel);
  CHECK(result.error == InstallError::kNone && OutcomeOf(result, 1)->outcome == WorkshopOutcome::kRemoved);
  CHECK(!Here(fixture.Item(1)));
  CHECK(Read(fixture.Item(7) / "mine.txt") == "hand placed");
  CHECK(Read(fixture.Item(8) / "mine.txt") == "hand placed too");
  CHECK(Read(fixture.content / "notes.txt") == "player's");
  CHECK(fixture.State().items.empty());
}

void TestResume() {
  std::atomic<bool> cancel{false};
  {
    // Stopped between moving <id> aside and putting <id>.new in place: the old copy comes back.
    Fixture fixture;
    FakeDownloads downloads;
    WorkshopState state;
    state.items.push_back({1, 10, 1000, 100, true, "a"});
    CHECK(WriteWorkshopState(fixture.workshop.string(), kApp, state));
    Write(fixture.Swap(1, ".old") / "mod.zip", "old");
    Write(fixture.Swap(1, ".new") / "mod.zip", "half");
    // Not Playden's: names in content/<appid>/ that only look like its own, also for an item it holds.
    Write(fs::path(fixture.Item(5).string() + ".new") / "keep.txt", "mine");
    Write(fs::path(fixture.Item(1).string() + ".new") / "keep.txt", "mine too");
    int asked = 0, applied = 0;
    auto request = Request(fixture, downloads, {Item(1, 10, 1000, 100)});
    request.may_apply = [&asked] { ++asked; return true; };
    request.applied = [&applied] { ++applied; };
    const auto result = SyncWorkshopItems(request, cancel);
    CHECK(result.error == InstallError::kNone && OutcomeOf(result, 1)->outcome == WorkshopOutcome::kKept);
    CHECK(Read(fixture.Item(1) / "mod.zip") == "old");
    CHECK(!Here(fixture.Swap(1, ".old")) && !Here(fixture.Swap(1, ".new")));
    CHECK(Read(fs::path(fixture.Item(5).string() + ".new") / "keep.txt") == "mine");
    CHECK(Read(fs::path(fixture.Item(1).string() + ".new") / "keep.txt") == "mine too");
    CHECK(downloads.calls.empty());
    // The restore happened with the game held, once.
    CHECK(asked == 1 && applied == 1);
  }
  {
    // Stopped after the swap, before the state was written: the old copy goes and the item downloads again.
    Fixture fixture;
    FakeDownloads downloads;
    downloads.files[11] = {{"mod.zip", "new"}};
    WorkshopState state;
    state.items.push_back({1, 10, 1000, 100, true, "a"});
    CHECK(WriteWorkshopState(fixture.workshop.string(), kApp, state));
    Write(fixture.Item(1) / "mod.zip", "new");
    Write(fixture.Swap(1, ".old") / "mod.zip", "old");
    const auto result = SyncWorkshopItems(Request(fixture, downloads, {Item(1, 11, 1000, 100)}), cancel);
    CHECK(result.error == InstallError::kNone && OutcomeOf(result, 1)->outcome == WorkshopOutcome::kUpdated);
    CHECK(Read(fixture.Item(1) / "mod.zip") == "new" && !Here(fixture.Swap(1, ".old")));
    CHECK(fixture.State().Find(1) && fixture.State().Find(1)->manifest == 11);
  }
  {
    // A download that stopped keeps its state line; the next sync starts it again in an empty staging folder.
    Fixture fixture;
    FakeDownloads downloads;
    downloads.files[10] = {{"a.bin", "aaa"}, {"b.bin", "bbb"}};
    downloads.partial.insert(10);
    auto result = SyncWorkshopItems(Request(fixture, downloads, {Item(1, 10)}), cancel);
    CHECK(result.error == InstallError::kNetwork && OutcomeOf(result, 1)->outcome == WorkshopOutcome::kFailed);
    CHECK(Here(fixture.Staging(1) / "a.bin") && !Here(fixture.Item(1)));
    auto state = fixture.State();
    CHECK(state.Find(1) && state.Find(1)->manifest == 0);
    result = SyncWorkshopItems(Request(fixture, downloads, {Item(1, 10)}), cancel);
    CHECK(result.error == InstallError::kNone && OutcomeOf(result, 1)->outcome == WorkshopOutcome::kDownloaded);
    CHECK(downloads.seen.size() == 2 && downloads.seen[1].empty());
    CHECK(Read(fixture.Item(1) / "b.bin") == "bbb");

    // Staging left for another manifest is cleared before a new download starts in it.
    downloads.files[12] = {{"c.bin", "ccc"}};
    downloads.partial.insert(12);
    SyncWorkshopItems(Request(fixture, downloads, {Item(1, 12, 1001)}), cancel);
    CHECK(Here(fixture.Staging(1) / "c.bin"));
    downloads.files[13] = {{"d.bin", "ddd"}};
    result = SyncWorkshopItems(Request(fixture, downloads, {Item(1, 13, 1002)}), cancel);
    CHECK(result.error == InstallError::kNone && downloads.seen.back().empty());
    CHECK(Read(fixture.Item(1) / "d.bin") == "ddd" && !Here(fixture.Item(1) / "c.bin"));

    // Staging of an item no longer downloaded goes.
    downloads.files[14] = {{"e.bin", "eee"}};
    downloads.partial.insert(14);
    SyncWorkshopItems(Request(fixture, downloads, {Item(1, 14, 1003)}), cancel);
    CHECK(Here(fixture.Staging(1)));
    result = SyncWorkshopItems(Request(fixture, downloads, {Item(1, 14, 1003)}, {1}), cancel);
    CHECK(OutcomeOf(result, 1)->outcome == WorkshopOutcome::kRemoved);
    CHECK(!Here(fixture.Staging(1)) && !Here(fixture.Item(1)) && fixture.State().items.empty());
  }
  {
    // Canceled: the sync stops at that item.
    Fixture fixture;
    FakeDownloads downloads;
    downloads.fail[10] = InstallError::kCanceled;
    downloads.files[20] = {{"x", "x"}};
    const auto result = SyncWorkshopItems(Request(fixture, downloads, {Item(1, 10), Item(2, 20)}), cancel);
    CHECK(result.error == InstallError::kCanceled && downloads.calls.size() == 1 && !Here(fixture.Item(2)));
    std::atomic<bool> stopped{true};
    CHECK(SyncWorkshopItems(Request(fixture, downloads, {Item(2, 20)}), stopped).error == InstallError::kCanceled);
    CHECK(downloads.calls.size() == 1);
  }
  {
    // A damaged state file is set aside and the sync starts afresh (else every sync would stop there): the folders
    // it listed are left alone, as if placed by hand.
    Fixture fixture;
    FakeDownloads downloads;
    downloads.files[30] = {{"x", "x"}};
    const auto state_path = fixture.workshop / WorkshopStateName(kApp);
    Write(state_path, "item=1,2\n");
    Write(fixture.Item(1) / "mod.zip", "x");
    std::vector<std::string> lines;
    auto request = Request(fixture, downloads, {Item(1, 10), Item(3, 30)});
    request.log = [&lines](const std::string& line) { lines.push_back(line); };
    const auto result = SyncWorkshopItems(request, cancel);
    CHECK(result.error == InstallError::kNone);
    CHECK(Read(state_path.string() + ".bad") == "item=1,2\n");
    CHECK(OutcomeOf(result, 1) && OutcomeOf(result, 1)->outcome == WorkshopOutcome::kSkippedHandPlaced);
    CHECK(Read(fixture.Item(1) / "mod.zip") == "x");
    CHECK(OutcomeOf(result, 3) && OutcomeOf(result, 3)->outcome == WorkshopOutcome::kDownloaded);
    CHECK(fixture.State().items.size() == 1 && fixture.State().Find(3));
    CHECK(std::any_of(lines.begin(), lines.end(), [](const std::string& line) {
      return line.find("workshop state damaged") != std::string::npos;
    }));
    // One over its cap goes the same way, replacing the older copy set aside.
    Write(state_path, std::string(kWorkshopStateMaxBytes + 1, 'x'));
    CHECK(SyncWorkshopItems(Request(fixture, downloads, {Item(3, 30)}), cancel).error == InstallError::kNone);
    CHECK(Read(state_path.string() + ".bad").size() == kWorkshopStateMaxBytes + 1);
  }
}

void TestOffFile() {
  CHECK(WorkshopOffName(kApp) == ".playden-workshop-1942280-off");
  const auto parsed = ParseWorkshopOff("3000000001\n7\n");
  CHECK(parsed && *parsed == (std::set<std::uint64_t>{7, 3000000001ull}));
  CHECK(ParseWorkshopOff("") && ParseWorkshopOff("")->empty());
  CHECK(FormatWorkshopOff({3000000001ull, 7}) == "7\n3000000001\n");
  for (const char* bad : {"7", "0\n", "07\n", "7\n7\n", "7\r\n", "x\n", "\n", "-7\n", " 7\n",
                          "18446744073709551616\n"})
    CHECK(!ParseWorkshopOff(bad));
  std::string many;
  for (std::uint64_t id = 1; id <= kWorkshopMaxItems + 1; ++id) many += std::to_string(id) + "\n";
  CHECK(!ParseWorkshopOff(many));

  Fixture fixture;
  const auto path = fixture.workshop / WorkshopOffName(kApp);
  CHECK(ReadWorkshopOff(fixture.workshop.string(), kApp) && ReadWorkshopOff(fixture.workshop.string(), kApp)->empty());
  CHECK(WriteWorkshopOff(fixture.workshop.string(), kApp, {9, 4}));
  CHECK(Read(path) == "4\n9\n" && Mode(path) == 0644);
  CHECK(!Here(path.string() + ".tmp"));
  CHECK(ReadWorkshopOff(fixture.workshop.string(), kApp) == (std::set<std::uint64_t>{4, 9}));
  CHECK(!WriteWorkshopOff(fixture.workshop.string(), kApp, {0, 4}));
  std::set<std::uint64_t> too_many;
  for (std::uint64_t id = 1; id <= kWorkshopMaxItems + 1; ++id) too_many.insert(id);
  CHECK(!WriteWorkshopOff(fixture.workshop.string(), kApp, too_many));
  CHECK(Read(path) == "4\n9\n");
  Write(path, "4\nfour\n");
  CHECK(!ReadWorkshopOff(fixture.workshop.string(), kApp));
}

void TestGameRunning() {
  std::atomic<bool> cancel{false};
  {
    // The game runs: a removal waits, before anything downloads.
    Fixture fixture;
    FakeDownloads downloads;
    downloads.files[10] = {{"a", "a"}};
    downloads.files[20] = {{"b", "b"}};
    CHECK(SyncWorkshopItems(Request(fixture, downloads, {Item(1, 10)}), cancel).error == InstallError::kNone);
    int asked = 0, applied = 0;
    auto request = Request(fixture, downloads, {Item(1, 10), Item(2, 20)}, {1});
    request.may_apply = [&asked] { ++asked; return false; };
    request.applied = [&applied] { ++applied; };
    const auto result = SyncWorkshopItems(request, cancel);
    CHECK(result.error == InstallError::kGameRunning && asked == 1 && applied == 0);
    CHECK(Read(fixture.Item(1) / "a") == "a" && fixture.State().Find(1) && downloads.calls.size() == 1);
  }
  {
    // A download finishes but isn't put in place while the game runs; the next sync downloads it again (staging is
    // never resumed) and puts it in place.
    Fixture fixture;
    FakeDownloads downloads;
    downloads.files[20] = {{"b", "b"}};
    int asked = 0, applied = 0;
    bool running = true;
    auto request = Request(fixture, downloads, {Item(2, 20)});
    request.may_apply = [&] { ++asked; return !running; };
    request.applied = [&applied] { ++applied; };
    auto result = SyncWorkshopItems(request, cancel);
    CHECK(result.error == InstallError::kGameRunning && asked == 1 && applied == 0);
    CHECK(!Here(fixture.Item(2)) && Read(fixture.Staging(2) / "b") == "b");
    CHECK(fixture.State().Find(2) && fixture.State().Find(2)->manifest == 0);
    running = false;
    result = SyncWorkshopItems(request, cancel);
    CHECK(result.error == InstallError::kNone && asked == 2 && applied == 1 && downloads.calls.size() == 2);
    CHECK(Read(fixture.Item(2) / "b") == "b" && fixture.State().Find(2)->manifest == 20);
  }
  {
    // Removals are asked about once, together; each download put in place once.
    Fixture fixture;
    FakeDownloads downloads;
    downloads.files[10] = {{"a", "a"}};
    downloads.files[20] = {{"b", "b"}};
    downloads.files[30] = {{"c", "c"}};
    CHECK(SyncWorkshopItems(Request(fixture, downloads, {Item(1, 10), Item(2, 20)}), cancel).error == InstallError::kNone);
    int asked = 0, applied = 0;
    auto request = Request(fixture, downloads, {Item(3, 30)});
    request.may_apply = [&asked] { ++asked; return true; };
    request.applied = [&applied] { ++applied; };
    CHECK(SyncWorkshopItems(request, cancel).error == InstallError::kNone);
    CHECK(asked == 2 && applied == 2 && !Here(fixture.Item(1)) && !Here(fixture.Item(2)) && Here(fixture.Item(3)));
  }
}

void TestSwapDoneBeforeGameRuns() {
  // When the game may run again, the item's old copy is already gone.
  Fixture fixture;
  FakeDownloads downloads;
  downloads.files[10] = {{"a", "a"}};
  downloads.files[11] = {{"a", "a2"}};
  std::atomic<bool> cancel{false};
  CHECK(SyncWorkshopItems(Request(fixture, downloads, {Item(1, 10, 1000)}), cancel).error == InstallError::kNone);
  int applied = 0;
  bool clean = false;
  auto request = Request(fixture, downloads, {Item(1, 11, 1001)});
  request.may_apply = [] { return true; };
  request.applied = [&] {
    ++applied;
    clean = Read(fixture.Item(1) / "a") == "a2" && !Here(fixture.Swap(1, ".old")) && !Here(fixture.Swap(1, ".new"));
  };
  CHECK(SyncWorkshopItems(request, cancel).error == InstallError::kNone);
  CHECK(applied == 1 && clean);
}

void TestStagingStartsEmpty() {
  // A staging folder is never resumed: DepotInstaller writes in it by plain path, so a link planted there (also one
  // left beside a marker for the same manifest by an earlier Playden) would carry the download out of the bottle.
  std::atomic<bool> cancel{false};
  {
    Fixture fixture;
    FakeDownloads downloads;
    downloads.files[10] = {{"victim", "planted"}};
    WorkshopState state;
    state.items.push_back({1, 0, 0, 0, true, ""});
    CHECK(WriteWorkshopState(fixture.workshop.string(), kApp, state));
    const auto outside = fixture.root / "outside.txt";
    Write(outside, "precious");
    fs::create_directories(fixture.Staging(1));
    Write(fixture.workshop / ".playden-download/1.want", "10\n");
    fs::create_symlink(outside, fixture.Staging(1) / "victim");
    const auto result = SyncWorkshopItems(Request(fixture, downloads, {Item(1, 10)}), cancel);
    CHECK(Read(outside) == "precious");
    CHECK(downloads.calls.empty());
    CHECK(OutcomeOf(result, 1) && OutcomeOf(result, 1)->outcome == WorkshopOutcome::kFailed);
    CHECK(!Here(fixture.Item(1)));
  }
  {
    // What a stopped download left is cleared: the next one finds its staging folder empty.
    Fixture fixture;
    FakeDownloads downloads;
    downloads.files[10] = {{"a.bin", "a"}};
    WorkshopState state;
    state.items.push_back({1, 0, 0, 0, true, ""});
    CHECK(WriteWorkshopState(fixture.workshop.string(), kApp, state));
    Write(fixture.Staging(1) / "stale.bin", "stale");
    Write(fixture.workshop / ".playden-download/1.want", "10\n");
    const auto result = SyncWorkshopItems(Request(fixture, downloads, {Item(1, 10)}), cancel);
    CHECK(result.error == InstallError::kNone && downloads.seen.size() == 1 && downloads.seen[0].empty());
    CHECK(Read(fixture.Item(1) / "a.bin") == "a" && !Here(fixture.Item(1) / "stale.bin"));
  }
}

void TestPlayerSwapNames() {
  // A player's folders named like a swap ("<id>.new", "<id>.old") in content/<appid>/ are theirs: download, update,
  // recovery and removal of item <id> never touch them.
  Fixture fixture;
  FakeDownloads downloads;
  downloads.files[10] = {{"a", "a"}};
  downloads.files[11] = {{"a", "a2"}};
  std::atomic<bool> cancel{false};
  const auto mine_new = fs::path(fixture.Item(1).string() + ".new"), mine_old = fs::path(fixture.Item(1).string() + ".old");
  Write(mine_new / "mine.txt", "new");
  Write(mine_old / "mine.txt", "old");
  const auto intact = [&] { return Read(mine_new / "mine.txt") == "new" && Read(mine_old / "mine.txt") == "old"; };
  CHECK(SyncWorkshopItems(Request(fixture, downloads, {Item(1, 10, 1000)}), cancel).error == InstallError::kNone);
  CHECK(Read(fixture.Item(1) / "a") == "a" && intact());
  CHECK(SyncWorkshopItems(Request(fixture, downloads, {Item(1, 11, 1001)}), cancel).error == InstallError::kNone);
  CHECK(Read(fixture.Item(1) / "a") == "a2" && intact());
  // An interrupted swap of Playden's own (the item moved aside, the new copy not yet in place) comes back.
  fs::create_directories(fixture.Staging(1).parent_path());
  fs::rename(fixture.Item(1), fixture.Swap(1, ".old"));
  CHECK(SyncWorkshopItems(Request(fixture, downloads, {Item(1, 11, 1001)}), cancel).error == InstallError::kNone);
  CHECK(Read(fixture.Item(1) / "a") == "a2" && !Here(fixture.Swap(1, ".old")) && intact());
  CHECK(SyncWorkshopItems(Request(fixture, downloads, {}), cancel).error == InstallError::kNone);
  CHECK(!Here(fixture.Item(1)) && intact());
}

void TestStateSavedBeforeGameRuns() {
  // The game may run again only once the state file lists the item as it now is: a launch writes mods.json from it.
  Fixture fixture;
  FakeDownloads downloads;
  downloads.files[10] = {{"a", "a"}};
  downloads.files[11] = {{"a", "a2"}};
  std::atomic<bool> cancel{false};
  bool listed_new = false, listed_updated = false;
  auto request = Request(fixture, downloads, {Item(1, 10, 1000)});
  request.may_apply = [] { return true; };
  request.applied = [&] {
    const auto state = ReadWorkshopState(fixture.workshop.string(), kApp);
    listed_new = state && state->Find(1) && state->Find(1)->manifest == 10 && state->Find(1)->on;
  };
  CHECK(SyncWorkshopItems(request, cancel).error == InstallError::kNone && listed_new);
  request.subscribed = {Item(1, 11, 1001)};
  request.applied = [&] {
    const auto state = ReadWorkshopState(fixture.workshop.string(), kApp);
    listed_updated = state && state->Find(1) && state->Find(1)->manifest == 11 && state->Find(1)->time_updated == 1001;
  };
  CHECK(SyncWorkshopItems(request, cancel).error == InstallError::kNone && listed_updated);
}

void TestCutShortList() {
  // A subscription list that was cut short keeps the items it doesn't reach, unless they are turned off.
  Fixture fixture;
  FakeDownloads downloads;
  downloads.files[10] = {{"a", "a"}};
  downloads.files[20] = {{"b", "b"}};
  std::atomic<bool> cancel{false};
  CHECK(SyncWorkshopItems(Request(fixture, downloads, {Item(1, 10), Item(2, 20)}), cancel).error == InstallError::kNone);
  auto request = Request(fixture, downloads, {});
  request.complete = false;
  auto result = SyncWorkshopItems(request, cancel);
  CHECK(result.error == InstallError::kNone && result.plan.removals == 0 && Here(fixture.Item(1)) && Here(fixture.Item(2)));
  request.off = {2};
  result = SyncWorkshopItems(request, cancel);
  CHECK(result.plan.removals == 1 && Here(fixture.Item(1)) && !Here(fixture.Item(2)));
  request.complete = true;
  request.off = {};
  result = SyncWorkshopItems(request, cancel);
  CHECK(result.plan.removals == 1 && !Here(fixture.Item(1)));
}

void TestJob() {
  std::atomic<bool> cancel{false};
  const auto subscriptions = [](std::vector<WorkshopItem> items, CmError error = CmError::kNone, bool truncated = false,
                                int* calls = nullptr) {
    return [items, error, truncated, calls](const std::atomic<bool>&) {
      if (calls) ++*calls;
      WorkshopSubscriptions out;
      out.error = error;
      out.items = items;
      out.truncated = truncated;
      return out;
    };
  };
  {
    // No bottle (never prepared): nothing is asked of Steam and nothing is made.
    Fixture fixture;
    FakeDownloads downloads;
    int calls = 0;
    WorkshopJobRequest request;
    request.app_id = kApp;
    request.prefix = (fixture.root / "missing").string();
    request.off = std::set<std::uint64_t>{};
    request.subscriptions = subscriptions({Item(1, 10)}, CmError::kNone, false, &calls);
    request.download = downloads.Downloader();
    auto result = RunWorkshopJob(request, cancel);
    CHECK(!result.ok && !result.retry_later && result.failure == kWorkshopNoBottle && calls == 0);
    CHECK(!Here(fixture.root / "missing"));
    request.prefix.clear();
    CHECK(RunWorkshopJob(request, cancel).failure == kWorkshopNoBottle && calls == 0);
  }
  {
    // The choice is saved first, also when Steam is out of reach; the Workshop folder is made when missing.
    Fixture fixture;
    fs::remove_all(fixture.prefix / "drive_c/Program Files (x86)");
    FakeDownloads downloads;
    WorkshopJobRequest request;
    request.app_id = kApp;
    request.prefix = fixture.prefix.string();
    request.off = std::set<std::uint64_t>{5, 3};
    request.subscriptions = subscriptions({}, CmError::kNetwork);
    request.download = downloads.Downloader();
    auto result = RunWorkshopJob(request, cancel);
    CHECK(!result.ok && result.retry_later && !result.game_running && result.failure.find("Steam") != std::string::npos);
    CHECK(Read(fixture.workshop / WorkshopOffName(kApp)) == "3\n5\n");
    request.subscriptions = subscriptions({}, CmError::kProtocol);
    result = RunWorkshopJob(request, cancel);
    CHECK(!result.ok && !result.retry_later && !result.failure.empty());
  }
  {
    // Without a choice the saved one holds; progress ends at the plan's bytes.
    Fixture fixture;
    FakeDownloads downloads;
    downloads.files[10] = {{"a", "a"}};
    downloads.files[20] = {{"b", "b"}};
    CHECK(WriteWorkshopOff(fixture.workshop.string(), kApp, {2}));
    std::vector<std::pair<std::uint64_t, std::uint64_t>> reports;
    WorkshopJobRequest request;
    request.app_id = kApp;
    request.prefix = fixture.prefix.string();
    request.subscriptions = subscriptions({Item(1, 10, 1000, 100), Item(2, 20, 1000, 50)});
    request.download = downloads.Downloader();
    request.progress = [&reports](std::uint64_t done, std::uint64_t total) { reports.emplace_back(done, total); };
    auto result = RunWorkshopJob(request, cancel);
    CHECK(result.ok && result.failure.empty() && Here(fixture.Item(1)) && !Here(fixture.Item(2)));
    CHECK(!reports.empty() && reports.back().first == 100 && reports.back().second == 100);
    for (const auto& [done, total] : reports) CHECK(done <= total);
    // Refused and legacy items don't fail the job.
    WorkshopItem legacy = Item(4, 0);
    legacy.file_url = "https://example.invalid/item";
    downloads.files[30] = {{".playden-x", "planted"}};
    request.subscriptions = subscriptions({Item(1, 10, 1000, 100), Item(3, 30), legacy});
    result = RunWorkshopJob(request, cancel);
    CHECK(result.ok && OutcomeOf(result.sync, 3)->outcome == WorkshopOutcome::kRefused &&
          OutcomeOf(result.sync, 4)->outcome == WorkshopOutcome::kSkippedLegacy);
    // A list cut short keeps item 1, which it doesn't name.
    request.subscriptions = subscriptions({}, CmError::kNone, true);
    CHECK(RunWorkshopJob(request, cancel).ok && Here(fixture.Item(1)));
    // The game runs: the job waits.
    request.subscriptions = subscriptions({Item(1, 11, 1001, 100)});
    downloads.files[11] = {{"a", "a2"}};
    request.may_apply = [] { return false; };
    result = RunWorkshopJob(request, cancel);
    CHECK(!result.ok && result.retry_later && result.game_running && Read(fixture.Item(1) / "a") == "a");
    // A failed download fails the job, with a sentence.
    request.may_apply = nullptr;
    downloads.fail[11] = InstallError::kNoSpace;
    result = RunWorkshopJob(request, cancel);
    CHECK(!result.ok && !result.retry_later && result.failure == WorkshopFailureText(InstallError::kNoSpace));
    downloads.fail[11] = InstallError::kNetwork;
    result = RunWorkshopJob(request, cancel);
    CHECK(!result.ok && result.retry_later && !result.game_running);
  }
  {
    // A damaged saved choice is asked for again, on every try: nothing downloads.
    Fixture fixture;
    FakeDownloads downloads;
    int calls = 0;
    Write(fixture.workshop / WorkshopOffName(kApp), "nope\n");
    WorkshopJobRequest request;
    request.app_id = kApp;
    request.prefix = fixture.prefix.string();
    request.subscriptions = subscriptions({Item(1, 10)}, CmError::kNone, false, &calls);
    request.download = downloads.Downloader();
    auto result = RunWorkshopJob(request, cancel);
    CHECK(!result.ok && !result.failure.empty() && calls == 0 && downloads.calls.empty());
    result = RunWorkshopJob(request, cancel);
    CHECK(!result.ok && !result.failure.empty() && calls == 0 && downloads.calls.empty());
    CHECK(Read(fixture.workshop / WorkshopOffName(kApp)) == "nope\n");
    // A new choice replaces it.
    request.off = std::set<std::uint64_t>{1};
    result = RunWorkshopJob(request, cancel);
    CHECK(result.ok && calls == 1 && downloads.calls.empty());
  }
}

// Steam features = Online (#856): every subscribed item is on for the job, whatever was turned off for Offline
// emulation, and that saved choice stays as it was.
void TestOnlineJob() {
  std::atomic<bool> cancel{false};
  Fixture fixture;
  FakeDownloads downloads;
  downloads.files[10] = {{"a", "a"}};
  downloads.files[30] = {{"c", "c"}};
  CHECK(WriteWorkshopOff(fixture.workshop.string(), kApp, {2, 3}));
  // Playden holds 3 (an earlier Online job downloaded it), turned off in Offline emulation's choice.
  Write(fixture.Item(3) / "c", "c");
  WorkshopState state;
  state.items.push_back({3, 30, 1000, 100, true, "Item 3"});
  CHECK(WriteWorkshopState(fixture.workshop.string(), kApp, state));
  WorkshopJobRequest request;
  request.app_id = kApp;
  request.prefix = fixture.prefix.string();
  request.all_on = true;
  request.off = std::set<std::uint64_t>{1};  // ignored in Online
  request.subscriptions = [](const std::atomic<bool>&) {
    WorkshopSubscriptions out;
    out.error = CmError::kNone;
    out.items = {Item(1, 10), Item(3, 30)};
    return out;
  };
  request.download = downloads.Downloader();
  const auto result = RunWorkshopJob(request, cancel);
  CHECK(result.ok && result.sync.plan.removals == 0);
  CHECK(OutcomeOf(result.sync, 1) && OutcomeOf(result.sync, 1)->outcome == WorkshopOutcome::kDownloaded);
  CHECK(OutcomeOf(result.sync, 3) && OutcomeOf(result.sync, 3)->outcome == WorkshopOutcome::kKept);
  CHECK(Here(fixture.Item(1)) && Read(fixture.Item(3) / "c") == "c");
  CHECK(Read(fixture.workshop / WorkshopOffName(kApp)) == "2\n3\n");
  // A damaged saved choice doesn't stop an Online job either, and stays for Offline emulation to ask about.
  Write(fixture.workshop / WorkshopOffName(kApp), "nope\n");
  CHECK(RunWorkshopJob(request, cancel).ok && Read(fixture.workshop / WorkshopOffName(kApp)) == "nope\n");
}

// Items Valve's client downloaded in Online (#856): a subscribed item whose folder is there and that the client's
// appworkshop file lists installed with a manifest becomes Playden's (kept, not downloaded again); without an entry
// there it stays placed by hand. Deleting one of Playden's items also takes it off the client's installed list.
void TestSteamClientItems() {
  std::atomic<bool> cancel{false};
  Fixture fixture;
  FakeDownloads downloads;
  const auto acf = fixture.workshop / runtime::WorkshopAcfName(kApp);
  Write(fixture.Item(5) / "mod.pck", "valve");
  Write(fixture.Item(6) / "mod.pck", "by hand");
  fs::create_directories(fixture.root / "elsewhere");
  fs::create_directory_symlink(fixture.root / "elsewhere", fixture.Item(7));
  auto text = *runtime::MergeAppWorkshopAcf(std::nullopt, kApp, {{5, 100, 1000, 50}, {7, 1, 1000, 70}});
  // Valve's details for the items stay as they are.
  text.replace(text.find("\t\"WorkshopItemDetails\"\n\t{\n\t}"), std::string("\t\"WorkshopItemDetails\"\n\t{\n\t}").size(),
               "\t\"WorkshopItemDetails\"\n\t{\n\t\t\"5\"\n\t\t{\n\t\t\t\"manifest\"\t\t\"50\"\n\t\t}\n\t}");
  Write(acf, text);
  // The listing's view first: read only.
  WorkshopState listed;
  CHECK(AdoptSteamClientItems(fixture.workshop.string(), kApp, {Item(5, 50), Item(6, 60), Item(7, 70)}, &listed) == 1);
  CHECK(listed.items.size() == 1 && listed.items[0].id == 5 && listed.items[0].manifest == 50 && listed.items[0].on);
  CHECK(!Here(fixture.workshop / WorkshopStateName(kApp)));

  auto result = SyncWorkshopItems(Request(fixture, downloads, {Item(5, 50), Item(6, 60), Item(7, 70)}), cancel);
  CHECK(result.error == InstallError::kNone && downloads.calls.empty());
  CHECK(OutcomeOf(result, 5) && OutcomeOf(result, 5)->outcome == WorkshopOutcome::kKept);
  CHECK(OutcomeOf(result, 6) && OutcomeOf(result, 6)->outcome == WorkshopOutcome::kSkippedHandPlaced);
  CHECK(OutcomeOf(result, 7) && OutcomeOf(result, 7)->outcome == WorkshopOutcome::kSkippedHandPlaced);
  auto state = fixture.State();
  CHECK(state.items.size() == 1 && state.items[0].id == 5 && state.items[0].manifest == 50 &&
        state.items[0].time_updated == 1000 && state.items[0].bytes == 100 && state.items[0].title == "Item 5");
  CHECK(Read(acf) == text);  // read only
  // A newer version on Steam: Playden updates it as its own.
  downloads.files[51] = {{"mod.pck", "new"}};
  result = SyncWorkshopItems(Request(fixture, downloads, {Item(5, 51, 1001)}), cancel);
  CHECK(OutcomeOf(result, 5) && OutcomeOf(result, 5)->outcome == WorkshopOutcome::kUpdated && Read(fixture.Item(5) / "mod.pck") == "new");
  // Turned off in Offline emulation: the folder goes, and so does its entry in the client's installed list; the
  // details and the other entry stay.
  result = SyncWorkshopItems(Request(fixture, downloads, {Item(5, 51, 1001)}, {5}), cancel);
  CHECK(OutcomeOf(result, 5) && OutcomeOf(result, 5)->outcome == WorkshopOutcome::kRemoved && !Here(fixture.Item(5)));
  const auto after = Read(acf);
  CHECK(after == *runtime::DropFromAppWorkshopAcf(text, kApp, {5}));
  CHECK(after.find("\t\t\"7\"\n") != std::string::npos && after.find("\t\t\t\"manifest\"\t\t\"50\"") != std::string::npos);
  CHECK(runtime::ParseAppWorkshopInstalled(after, kApp).size() == 1);
  CHECK(Mode(acf) == 0644);
  // Without the client's file nothing is adopted.
  fs::remove(acf);
  Write(fixture.Item(8) / "mod.pck", "x");
  result = SyncWorkshopItems(Request(fixture, downloads, {Item(8, 80)}), cancel);
  CHECK(OutcomeOf(result, 8) && OutcomeOf(result, 8)->outcome == WorkshopOutcome::kSkippedHandPlaced && !Here(acf));
}

void TestStopsAndCap() {
  std::atomic<bool> cancel{false};
  {
    // No network: the sync stops at the first item instead of trying each.
    Fixture fixture;
    FakeDownloads downloads;
    downloads.fail[10] = InstallError::kNetwork;
    const auto result = SyncWorkshopItems(Request(fixture, downloads, {Item(1, 10), Item(2, 20), Item(3, 30)}), cancel);
    CHECK(result.error == InstallError::kNetwork && downloads.calls.size() == 1 && result.items.size() == 1);
  }
  {
    // A full state file takes no new item, and removals run first so a replaced item has room.
    Fixture fixture;
    FakeDownloads downloads;
    WorkshopState state;
    std::vector<WorkshopItem> subscribed;
    for (std::uint64_t id = 1; id <= kWorkshopMaxItems; ++id) {
      state.items.push_back({id, 10, 1000, 1, true, ""});
      fs::create_directories(fixture.Item(id));
      if (id > 1) subscribed.push_back(Item(id, 10));
    }
    CHECK(WriteWorkshopState(fixture.workshop.string(), kApp, state));
    downloads.files[20] = {{"x", "x"}};
    // Item 1 is unsubscribed and listed last in the plan, after the new item 5000.
    subscribed.push_back(Item(5000, 20));
    auto result = SyncWorkshopItems(Request(fixture, downloads, subscribed), cancel);
    CHECK(result.error == InstallError::kNone);
    CHECK(result.items.front().id == 1 && result.items.front().outcome == WorkshopOutcome::kRemoved);
    CHECK(OutcomeOf(result, 5000) && OutcomeOf(result, 5000)->outcome == WorkshopOutcome::kDownloaded);
    CHECK(fixture.State().items.size() == kWorkshopMaxItems);
    subscribed.push_back(Item(5001, 20));
    result = SyncWorkshopItems(Request(fixture, downloads, subscribed), cancel);
    CHECK(OutcomeOf(result, 5001) && OutcomeOf(result, 5001)->outcome == WorkshopOutcome::kFailed);
    CHECK(!Here(fixture.Item(5001)) && !Here(fixture.Staging(5001)));
    CHECK(ReadWorkshopState(fixture.workshop.string(), kApp).has_value());
    CHECK(fixture.State().items.size() == kWorkshopMaxItems);
  }
}

void TestRefusedNames() {
  // A download that lists a top-level .playden- entry is refused, whatever the downloader said.
  Fixture fixture;
  FakeDownloads downloads;
  downloads.files[10] = {{"mod.zip", "x"}, {".Playden-files", "planted"}};
  downloads.files[20] = {{"Data/.playden-cache", "nested is fine"}};
  std::atomic<bool> cancel{false};
  const auto result = SyncWorkshopItems(Request(fixture, downloads, {Item(1, 10), Item(2, 20)}), cancel);
  CHECK(result.error == InstallError::kNone);
  CHECK(OutcomeOf(result, 1) && OutcomeOf(result, 1)->outcome == WorkshopOutcome::kRefused);
  CHECK(!Here(fixture.Item(1)) && !Here(fixture.Staging(1)));
  CHECK(OutcomeOf(result, 2) && OutcomeOf(result, 2)->outcome == WorkshopOutcome::kDownloaded);
  CHECK(Read(fixture.Item(2) / "Data/.playden-cache") == "nested is fine");
  const auto state = fixture.State();
  CHECK(!state.Find(1) && state.Find(2));
}

// ---- The real DepotInstaller behind the downloader, with canned Steam answers. ----

const std::array<std::uint8_t, 32> kKey = [] {
  std::array<std::uint8_t, 32> key{};
  for (std::size_t index = 0; index < key.size(); ++index) key[index] = static_cast<std::uint8_t>(index * 7 + 1);
  return key;
}();

void Le16(std::vector<std::uint8_t>& out, std::uint32_t value) {
  out.push_back(static_cast<std::uint8_t>(value));
  out.push_back(static_cast<std::uint8_t>(value >> 8));
}
void Le32(std::vector<std::uint8_t>& out, std::uint32_t value) {
  Le16(out, value & 0xffff);
  Le16(out, value >> 16);
}

std::vector<std::uint8_t> StoredZip(const std::vector<std::uint8_t>& data) {
  std::vector<std::uint8_t> zip;
  Le32(zip, 0x04034b50); Le16(zip, 20); Le16(zip, 0); Le16(zip, 0); Le16(zip, 0); Le16(zip, 0);
  Le32(zip, 0); Le32(zip, static_cast<std::uint32_t>(data.size())); Le32(zip, static_cast<std::uint32_t>(data.size()));
  Le16(zip, 1); Le16(zip, 0); zip.push_back('z');
  zip.insert(zip.end(), data.begin(), data.end());
  const auto central = static_cast<std::uint32_t>(zip.size());
  Le32(zip, 0x02014b50); Le16(zip, 20); Le16(zip, 20); Le16(zip, 0); Le16(zip, 0); Le16(zip, 0); Le16(zip, 0);
  Le32(zip, 0); Le32(zip, static_cast<std::uint32_t>(data.size())); Le32(zip, static_cast<std::uint32_t>(data.size()));
  Le16(zip, 1); Le16(zip, 0); Le16(zip, 0); Le16(zip, 0); Le16(zip, 0); Le32(zip, 0); Le32(zip, 0); zip.push_back('z');
  const auto central_size = static_cast<std::uint32_t>(zip.size()) - central;
  Le32(zip, 0x06054b50); Le16(zip, 0); Le16(zip, 0); Le16(zip, 1); Le16(zip, 1);
  Le32(zip, central_size); Le32(zip, central); Le16(zip, 0);
  return zip;
}

std::vector<std::uint8_t> Encrypt(std::vector<std::uint8_t> plain) {
  std::array<std::uint8_t, 16> iv{};
  for (std::size_t index = 0; index < iv.size(); ++index) iv[index] = static_cast<std::uint8_t>(0xa0 + index);
  const auto pad = static_cast<std::uint8_t>(16 - plain.size() % 16);
  plain.insert(plain.end(), pad, pad);
  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  mbedtls_aes_setkey_enc(&aes, kKey.data(), 256);
  std::vector<std::uint8_t> out(16 + plain.size());
  mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, iv.data(), out.data());
  auto chain = iv;
  mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_ENCRYPT, plain.size(), chain.data(), plain.data(), out.data() + 16);
  mbedtls_aes_free(&aes);
  return out;
}

std::array<std::uint8_t, 20> Sha1(const std::vector<std::uint8_t>& data) {
  std::array<std::uint8_t, 20> digest{};
  mbedtls_sha1(data.data(), data.size(), digest.data());
  return digest;
}

std::string HexOf(const std::array<std::uint8_t, 20>& bytes) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string text;
  for (const auto byte : bytes) { text.push_back(kDigits[byte >> 4]); text.push_back(kDigits[byte & 15]); }
  return text;
}

struct Steam {
  std::map<std::string, std::vector<std::uint8_t>> served;
  std::map<std::uint64_t, DepotManifest> manifests;  // by manifest gid
  std::vector<DepotRequest> asked;
  int chunk_calls = 0;
  int manifest_calls = 0;

  ManifestFile AddFile(const std::string& path, const std::string& text) {
    ManifestFile file;
    file.path = path;
    std::vector<std::uint8_t> plain(text.begin(), text.end());
    ManifestChunk chunk;
    chunk.sha = Sha1(plain);
    chunk.original_size = static_cast<std::uint32_t>(plain.size());
    const auto container = Encrypt(StoredZip(plain));
    chunk.compressed_size = static_cast<std::uint32_t>(container.size());
    served[HexOf(chunk.sha)] = container;
    file.chunks.push_back(chunk);
    file.size = plain.size();
    file.sha_content = Sha1(plain);
    return file;
  }
  void AddManifest(std::uint64_t gid, std::vector<ManifestFile> files) {
    DepotManifest manifest;
    manifest.depot_id = kDepot;
    manifest.gid = gid;
    std::sort(files.begin(), files.end(), [](const ManifestFile& a, const ManifestFile& b) { return a.path < b.path; });
    manifest.files = std::move(files);
    manifests[gid] = manifest;
  }

  InstallerHooks Hooks() {
    InstallerHooks hooks;
    hooks.access = [this](const StoredSession&, std::uint32_t app, const std::vector<DepotRequest>& depots,
                          const std::atomic<bool>&) {
      CHECK(app == kApp);
      DepotAccessResult result;
      result.error = CmError::kNone;
      for (const auto& depot : depots) {
        asked.push_back(depot);
        result.grants.push_back({depot.depot_id, depot.manifest_gid, kKey, 77});
      }
      return result;
    };
    hooks.servers = [](std::uint32_t, const std::atomic<bool>&, std::vector<ContentServer>* out) {
      *out = {{"a.example.invalid", "a.example.invalid", 1}, {"b.example.invalid", "b.example.invalid", 2}};
      return CdnError::kNone;
    };
    hooks.manifest = [this](const ContentServer&, const DepotGrant& grant, const std::atomic<bool>&, DepotManifest* out) {
      ++manifest_calls;
      *out = manifests.at(grant.manifest_gid);
      return InstallError::kNone;
    };
    hooks.chunk = [this](const ContentServer&, std::uint32_t, const std::string& sha, std::size_t,
                         const std::atomic<bool>&, std::vector<std::uint8_t>* out) {
      ++chunk_calls;
      *out = served.at(sha);
      return CdnError::kNone;
    };
    hooks.free_bytes = [](const std::string&) { return std::uint64_t{1} << 40; };
    return hooks;
  }
};

ManifestFile Link(const std::string& path, const std::string& target) {
  ManifestFile file;
  file.path = path;
  file.link_target = target;
  return file;
}

void TestRealDownloader() {
  std::atomic<bool> cancel{false};
  {
    // A real download: the request names the Workshop depot and the item's manifest under the game.
    Fixture fixture;
    Steam steam;
    steam.AddManifest(10, {steam.AddFile("mod.zip", "zipped mod"), steam.AddFile("Data/info.txt", "info")});
    WorkshopSyncRequest request;
    request.app_id = kApp;
    request.workshop_folder = fixture.workshop.string();
    request.subscribed = {Item(1, 10, 1000, 14)};
    request.download = MakeWorkshopDownloader(steam.Hooks(), StoredSession{}, kApp, kDepot);
    const auto result = SyncWorkshopItems(request, cancel);
    CHECK(result.error == InstallError::kNone && OutcomeOf(result, 1)->outcome == WorkshopOutcome::kDownloaded);
    CHECK(steam.asked.size() == 1 && steam.asked[0].depot_id == kDepot && steam.asked[0].manifest_gid == 10 &&
          !steam.asked[0].optional);
    CHECK(Read(fixture.Item(1) / "mod.zip") == "zipped mod");
    CHECK(Read(fixture.Item(1) / "Data/info.txt") == "info");
    std::vector<std::string> names;
    for (const auto& entry : fs::directory_iterator(fixture.Item(1))) names.push_back(entry.path().filename().string());
    std::sort(names.begin(), names.end());
    CHECK(names == (std::vector<std::string>{"Data", "mod.zip"}));
    CHECK(!Here(fixture.Staging(1)));
  }
  {
    // An item with a link, or with a top-level .playden- entry, is refused before any chunk is fetched.
    Fixture fixture;
    Steam steam;
    steam.AddManifest(10, {steam.AddFile("mod.zip", "x"), Link("shortcut", "mod.zip")});
    steam.AddManifest(20, {steam.AddFile(".playden-install.json", "{\"planted\":1}")});
    steam.AddManifest(30, {steam.AddFile(".PLAYDEN-download/journal", "x")});
    WorkshopSyncRequest request;
    request.app_id = kApp;
    request.workshop_folder = fixture.workshop.string();
    request.subscribed = {Item(1, 10), Item(2, 20), Item(3, 30)};
    request.download = MakeWorkshopDownloader(steam.Hooks(), StoredSession{}, kApp, kDepot);
    const auto result = SyncWorkshopItems(request, cancel);
    CHECK(result.error == InstallError::kNone);
    for (std::uint64_t id : {1, 2, 3}) {
      CHECK(OutcomeOf(result, id) && OutcomeOf(result, id)->outcome == WorkshopOutcome::kRefused);
      CHECK(!Here(fixture.Item(id)) && !Here(fixture.Staging(id)));
    }
    CHECK(OutcomeOf(result, 1)->detail == "item has a link");
    CHECK(OutcomeOf(result, 2)->detail == "item has a .playden- name");
    CHECK(steam.chunk_calls == 0);
    CHECK(fixture.State().items.empty());
  }
  {
    // Why the wrapper is needed: DepotInstaller by itself installs such an item without its link.
    Fixture fixture;
    Steam steam;
    steam.AddManifest(10, {steam.AddFile("mod.zip", "x"), Link("shortcut", "mod.zip")});
    DepotInstaller installer(steam.Hooks());
    InstallTarget target;
    target.app_id = kApp;
    target.install_root = (fixture.root / "plain").string();
    DepotInfo depot;
    depot.id = kDepot;
    depot.manifest_gid = 10;
    target.depots = {depot};
    const auto outcome = installer.Run(StoredSession{}, target, cancel);
    CHECK(outcome.error == InstallError::kNone);
    CHECK(Read(fixture.root / "plain/mod.zip") == "x" && !Here(fixture.root / "plain/shortcut"));
    // SetManifestCheck, which the CmClient/CdnClient downloader uses, stops it before any chunk.
    Steam checked;
    checked.AddManifest(10, {checked.AddFile("mod.zip", "x"), Link("shortcut", "mod.zip")});
    DepotInstaller refusing(checked.Hooks());
    int checks = 0;
    refusing.SetManifestCheck([&checks](const DepotManifest& manifest) {
      ++checks;
      return WorkshopManifestRefused(manifest) ? InstallError::kCorrupt : InstallError::kNone;
    });
    target.install_root = (fixture.root / "checked").string();
    CHECK(refusing.Run(StoredSession{}, target, cancel).error != InstallError::kNone);
    CHECK(checks >= 1 && checked.chunk_calls == 0 && !Here(fixture.root / "checked/mod.zip"));
  }
  {
    // Stopped after DepotInstaller finished but while its files moved into <id>.new: the next sync clears both,
    // downloads the item again and puts the whole item in place.
    Fixture fixture;
    Steam steam;
    steam.AddManifest(10, {steam.AddFile("mod.zip", "zipped mod"), steam.AddFile("Data/info.txt", "info")});
    WorkshopState state;
    state.items.push_back({1, 0, 0, 0, true, ""});
    CHECK(WriteWorkshopState(fixture.workshop.string(), kApp, state));
    DepotInstaller installer(steam.Hooks());
    InstallTarget target;
    target.app_id = kApp;
    target.install_root = fixture.Staging(1).string();
    DepotInfo depot;
    depot.id = kDepot;
    depot.manifest_gid = 10;
    target.depots = {depot};
    CHECK(installer.Run(StoredSession{}, target, cancel).error == InstallError::kNone);
    fs::create_directories(fixture.Swap(1, ".new"));
    fs::rename(fixture.Staging(1) / "mod.zip", fixture.Swap(1, ".new") / "mod.zip");
    WorkshopSyncRequest request;
    request.app_id = kApp;
    request.workshop_folder = fixture.workshop.string();
    request.subscribed = {Item(1, 10, 1000, 14)};
    request.download = MakeWorkshopDownloader(steam.Hooks(), StoredSession{}, kApp, kDepot);
    const auto calls_before = steam.chunk_calls;
    const auto result = SyncWorkshopItems(request, cancel);
    CHECK(result.error == InstallError::kNone && OutcomeOf(result, 1)->outcome == WorkshopOutcome::kDownloaded);
    CHECK(steam.chunk_calls > calls_before);
    CHECK(Read(fixture.Item(1) / "mod.zip") == "zipped mod");
    CHECK(Read(fixture.Item(1) / "Data/info.txt") == "info");
    CHECK(!Here(fixture.Swap(1, ".new")) && !Here(fixture.Staging(1)));
    CHECK(!Here(fixture.Item(1) / ".playden-install.json"));
    CHECK(fixture.State().Find(1) && fixture.State().Find(1)->manifest == 10);
  }
  CHECK(WorkshopManifestRefused(DepotManifest{}) == false);
}

}  // namespace

// Left 4 Dead 2 (#1319): an item from the old UGC system names a file_url, and its hcontent_file is the file's UGC
// handle, not a manifest. It is never handed to the depot downloader (which tried it as a manifest, failed with a
// network error on every server and stopped the sync, again on every retry); an entry such a try left in the state
// is dropped with its staging.
void TestLegacyUgcItems() {
  std::atomic<bool> cancel{false};
  // As Steam lists one: a UGC handle where a manifest would be, and a URL.
  auto ugc = [](std::uint64_t id) {
    auto item = Item(id, 13584402027613737184ull, 1000, 27609578);
    item.file_url = "https://ugc.example.invalid/" + std::to_string(id) + "/";
    item.has_file_url = true;
    return item;
  };
  {
    Fixture fixture;
    FakeDownloads downloads;
    downloads.files[20] = {{"mod.zip", "depot item"}};
    std::vector<std::string> lines;
    auto request = Request(fixture, downloads, {ugc(1), Item(2, 20), ugc(3)});
    request.log = [&lines](const std::string& line) { lines.push_back(line); };
    const auto result = SyncWorkshopItems(request, cancel);
    CHECK(result.error == InstallError::kNone);
    CHECK(downloads.calls == (std::vector<std::uint64_t>{20}));
    CHECK(OutcomeOf(result, 1) && OutcomeOf(result, 1)->outcome == WorkshopOutcome::kSkippedLegacy);
    CHECK(OutcomeOf(result, 3) && OutcomeOf(result, 3)->outcome == WorkshopOutcome::kSkippedLegacy);
    CHECK(OutcomeOf(result, 2) && OutcomeOf(result, 2)->outcome == WorkshopOutcome::kDownloaded);
    CHECK(result.plan.downloads == 1 && result.plan.download_bytes == 100);
    CHECK(!Here(fixture.Item(1)) && !Here(fixture.Staging(1)));
    const auto state = fixture.State();
    CHECK(state.items.size() == 1 && state.Find(2));
    CHECK(!lines.empty() && lines.back().find("workshop sync done") == 0 &&
          lines.back().find("skipped-legacy=2") != std::string::npos);
  }
  {
    // What a Playden that tried the handle left: the item listed with no manifest yet, a partial staging.
    Fixture fixture;
    FakeDownloads downloads;
    WorkshopState state;
    state.items.push_back({1, 0, 0, 0, true, "unfinished"});
    CHECK(WriteWorkshopState(fixture.workshop.string(), kApp, state));
    Write(fixture.Staging(1) / "addon.vpk", "partial");
    // Without a sync, the sheet already counts it as not held.
    const auto plan = PlanWorkshopSync({ugc(1)}, state, {}, {});
    CHECK(plan.entries.size() == 1 && plan.entries[0].action == WorkshopAction::kSkipLegacy && plan.downloads == 0);
    std::vector<std::string> lines;
    auto request = Request(fixture, downloads, {ugc(1)});
    request.log = [&lines](const std::string& line) { lines.push_back(line); };
    const auto result = SyncWorkshopItems(request, cancel);
    CHECK(result.error == InstallError::kNone && downloads.calls.empty());
    CHECK(OutcomeOf(result, 1) && OutcomeOf(result, 1)->outcome == WorkshopOutcome::kSkippedLegacy);
    CHECK(fixture.State().items.empty());
    CHECK(!Here(fixture.Staging(1)) && !Here(fixture.Item(1)));
    CHECK(std::find(lines.begin(), lines.end(), "workshop sync dropped-unfinished-legacy=1") != lines.end());
    // A copy Playden holds of an item that is legacy now stays.
    state.items = {{4, 40, 1000, 400, true, "held"}};
    CHECK(PlanWorkshopSync({ugc(4)}, state, {}, {4}).entries[0].action == WorkshopAction::kKeep);
  }
  {
    // The player copied the item in by hand after a failed try: the folder is theirs. Not held, not removed when
    // turned off or unsubscribed; the leftover entry goes. Also for an item Steam has since flagged.
    Fixture fixture;
    FakeDownloads downloads;
    WorkshopState state;
    state.items.push_back({1, 0, 0, 0, true, "unfinished"});
    state.items.push_back({2, 0, 0, 0, true, "unfinished, banned"});
    CHECK(WriteWorkshopState(fixture.workshop.string(), kApp, state));
    Write(fixture.Item(1) / "addon.vpk", "by hand");
    auto banned = ugc(2);
    banned.banned = true;
    const auto plan = PlanWorkshopSync({ugc(1), banned}, state, {1}, {1});
    CHECK(plan.entries.size() == 1 && plan.entries[0].action == WorkshopAction::kSkipOff && plan.removals == 0);
    CHECK(PlanWorkshopSync({ugc(1)}, state, {}, {1}).entries[0].action == WorkshopAction::kSkipLegacy);
    auto result = SyncWorkshopItems(Request(fixture, downloads, {ugc(1), banned}, {1}), cancel);
    CHECK(result.error == InstallError::kNone && downloads.calls.empty());
    CHECK(Read(fixture.Item(1) / "addon.vpk") == "by hand");
    CHECK(fixture.State().items.empty());
    result = SyncWorkshopItems(Request(fixture, downloads, {}), cancel);
    CHECK(result.error == InstallError::kNone && Read(fixture.Item(1) / "addon.vpk") == "by hand");
  }
  {
    // Valve's client installed one in Online (its installed list names it with a manifest): Playden adopts it as
    // its own and keeps it, URL or not.
    Fixture fixture;
    FakeDownloads downloads;
    Write(fixture.Item(5) / "addon.vpk", "valve");
    Write(fixture.workshop / runtime::WorkshopAcfName(kApp),
          *runtime::MergeAppWorkshopAcf(std::nullopt, kApp, {{5, 100, 1000, 77}}));
    auto item = ugc(5);
    item.manifest = 77;
    const auto result = SyncWorkshopItems(Request(fixture, downloads, {item}), cancel);
    CHECK(result.error == InstallError::kNone && downloads.calls.empty());
    CHECK(OutcomeOf(result, 5) && OutcomeOf(result, 5)->outcome == WorkshopOutcome::kKept);
    CHECK(fixture.State().Find(5) && fixture.State().Find(5)->manifest == 77);
  }
}

// The line a stopped sync logs says why, without a path from the item's content.
void TestStoppedLineDetail() {
  std::atomic<bool> cancel{false};
  const auto stopped = [&](const std::string& detail, InstallError error) {
    Fixture fixture;
    FakeDownloads downloads;
    std::vector<std::string> lines;
    auto request = Request(fixture, downloads, {Item(1, 10)});
    request.download = [&](const WorkshopItem&, const std::string&, const std::atomic<bool>&,
                           const WorkshopProgressFn&) { return WorkshopDownloadResult{error, false, detail}; };
    request.log = [&lines](const std::string& line) { lines.push_back(line); };
    CHECK(SyncWorkshopItems(request, cancel).error == error);
    return lines.empty() ? std::string() : lines.back();
  };
  auto line = stopped("manifest unavailable http=404", InstallError::kNetwork);
  CHECK(line.find("workshop sync stopped error=network") == 0);
  CHECK(line.find(" detail=manifest_unavailable_http=404") != std::string::npos);
  line = stopped("cannot create folder for Maps/My Map.bsp: x is in the way", InstallError::kStorage);
  const std::string folder = " detail=cannot_create_folder";
  CHECK(line.size() >= folder.size() && line.compare(line.size() - folder.size(), folder.size(), folder) == 0);
  CHECK(line.find("Map") == std::string::npos && line.find("for") == std::string::npos);
  // What DepotInstaller says of a manifest that doesn't match: no depot or manifest ID.
  line = stopped("manifest unavailable depot=550 bytes=1234 ids got_depot=550 got_gid=13584402027613737184",
                 InstallError::kCorrupt);
  CHECK(line.find(" detail=manifest_unavailable") != std::string::npos);
  CHECK(line.find("550") == std::string::npos && line.find("13584402027613737184") == std::string::npos);
  line = stopped("chunk unavailable transport=timeout", InstallError::kNetwork);
  CHECK(line.find(" detail=chunk_unavailable_transport=timeout") != std::string::npos);
  // A lowercase file name with spaces stays out after the third word.
  line = stopped("cannot delete my secret mod/file.vpk", InstallError::kStorage);
  CHECK(line.find("secret") == std::string::npos && line.find("mod") == std::string::npos);
  line = stopped("", InstallError::kStorage);
  CHECK(line.find(" detail=none") != std::string::npos);
}

int main() {
  TestEnsureFolder();
  TestStateFile();
  TestPlan();
  TestSync();
  TestResume();
  TestStopsAndCap();
  TestRefusedNames();
  TestOffFile();
  TestGameRunning();
  TestSwapDoneBeforeGameRuns();
  TestStagingStartsEmpty();
  TestPlayerSwapNames();
  TestStateSavedBeforeGameRuns();
  TestCutShortList();
  TestJob();
  TestOnlineJob();
  TestSteamClientItems();
  TestRealDownloader();
  TestLegacyUgcItems();
  TestStoppedLineDetail();
  if (failures) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("workshop_items: ok\n");
  return 0;
}
