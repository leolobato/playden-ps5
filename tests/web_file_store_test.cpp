// SPDX-License-Identifier: GPL-3.0-or-later
#include "web/file_store.h"

#include "runtime/artifact_hash.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace playden::web;
namespace fs = std::filesystem;
using playden::platform::SetOwnershipProbeForTesting;

#define CHECK(condition)                                                                  \
  do {                                                                                    \
    if (!(condition)) {                                                                   \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
      std::exit(1);                                                                       \
    }                                                                                     \
  } while (false)

namespace {

void Put(const fs::path& path, const std::string& text) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << text;
}

std::string Text(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), {});
}

UploadReader Reader(const std::string& text) {
  auto offset = std::make_shared<std::size_t>(0);
  return [text, offset](char* data, std::size_t size) -> std::ptrdiff_t {
    const auto count = std::min(size, text.size() - *offset);
    std::copy_n(text.data() + *offset, count, data);
    *offset += count;
    return static_cast<std::ptrdiff_t>(count);
  };
}

FileError Send(FileStore& store, const FileRoot& root, const std::string& path, const std::string& text, bool overwrite = false, bool* replaced = nullptr,
               std::optional<std::string> hash = std::nullopt) {
  UploadOptions options;
  options.length = text.size();
  options.overwrite = overwrite;
  options.if_sha256 = std::move(hash);
  return store.Upload(root, path, options, Reader(text), replaced);
}

std::vector<std::string> Names(const fs::path& folder) {
  std::vector<std::string> names;
  for (const auto& entry : fs::directory_iterator(folder)) names.push_back(entry.path().filename().string());
  std::sort(names.begin(), names.end());
  return names;
}

FileRoot Root(const fs::path& path) {
  FileRoot root;
  root.path = path.string();
  return root;
}

const FileEntry* Entry(const FileListing& listing, const std::string& name) {
  for (const auto& entry : listing.entries)
    if (entry.name == name) return &entry;
  return nullptr;
}

}  // namespace

int main() {
  const auto base = fs::canonical(fs::temp_directory_path()) / ("playden-web-files-" + std::to_string(::getpid()));
  fs::remove_all(base);
  fs::create_directories(base);
  const auto folder = base / "root";
  fs::create_directories(folder);
  FileRoot root = Root(folder);
  std::vector<std::string> steps;
  FileStore::Options options;
  std::string fail_at;
  options.fault = [&](const std::string& step) {
    steps.push_back(step);
    return step != fail_at;
  };
  FileStore store(options);

  // Listing: folders first, hidden names and links out, unusable names counted.
  Put(folder / "b.sav", "bb");
  Put(folder / "A.sav", "a");
  Put(folder / "Sub/inner.txt", "inner");
  Put(folder / ".playden-install.json", "{}");
  Put(folder / "playden_online.txt", "token");
  Put(folder / "steam_settings/configs.user.ini", "name");
  Put(base / "outside.txt", "secret");
  fs::create_symlink(base / "outside.txt", folder / "link.txt");
  fs::create_directory_symlink(base, folder / "linkdir");
  FileListing listing;
  CHECK(store.List(root, "", &listing).ok() && listing.exists && !listing.read_only);
  CHECK(listing.entries.size() == 4);
  CHECK(listing.entries[0].directory && listing.entries[2].name == "A.sav" && listing.entries[3].name == "b.sav" && listing.entries[3].size == 2);
  CHECK(Entry(listing, "steam_settings")->read_only && !Entry(listing, "Sub")->read_only);
  CHECK(!Entry(listing, "link.txt") && !Entry(listing, "linkdir") && !Entry(listing, "playden_online.txt"));

  // A request can't reach out of the root, into a link, or a hidden name.
  OpenedFile opened;
  for (const char* path : {"../outside.txt", "link.txt", "linkdir/outside.txt", ".playden-install.json", "playden_online.txt", "PLAYDEN_ONLINE.TXT", "/etc/hosts", "Sub/../../outside.txt"})
    CHECK(store.OpenForRead(root, path, false, &opened).status != 0);
  FileListing refused;
  CHECK(store.List(root, "linkdir", &refused).status != 0 && store.List(root, "..", &refused).status != 0);
  CHECK(!Send(store, root, "../escape.txt", "x").ok() && !fs::exists(base / "escape.txt"));
  CHECK(!Send(store, root, "linkdir/escape.txt", "x").ok() && !fs::exists(base / "escape.txt"));
  CHECK(!Send(store, root, "link.txt", "x", true).ok() && Text(base / "outside.txt") == "secret");
  CHECK(!store.Delete(root, "link.txt").ok() && fs::is_symlink(folder / "link.txt"));
  CHECK(!store.MakeFolder(root, "../made").ok() && !fs::exists(base / "made"));
  fs::remove(folder / "link.txt");
  fs::remove(folder / "linkdir");

  // A file with another hard link can hold a secret under an innocent name: hidden everywhere.
  fs::create_hard_link(base / "outside.txt", folder / "innocent.txt");
  CHECK(store.List(root, "", &listing).ok() && !Entry(listing, "innocent.txt") && listing.unshown == 1);
  CHECK(store.OpenForRead(root, "innocent.txt", false, &opened).status == 404);
  FileText text;
  CHECK(store.ReadText(root, "innocent.txt", &text).status == 404);
  CHECK(store.Delete(root, "innocent.txt").status == 404);
  fs::remove(folder / "innocent.txt");

  // A download.
  CHECK(store.OpenForRead(root, "Sub/inner.txt", false, &opened).ok() && opened.size == 5 && opened.name == "inner.txt");
  ::close(opened.fd);
  // The descriptor reads as blocking from here on.
  CHECK(store.OpenForRead(root, "Sub/inner.txt", false, &opened).ok() && (::fcntl(opened.fd, F_GETFL) & O_NONBLOCK) == 0);
  ::close(opened.fd);
  CHECK(store.OpenForRead(root, "Sub", false, &opened).status == 404);

  // Private paths are hidden, too.
  {
    FileRoot secret = root;
    secret.private_paths = {"Sub"};
    CHECK(store.List(secret, "", &listing).ok() && !Entry(listing, "Sub"));
    CHECK(store.List(secret, "Sub", &listing).status == 404 && store.OpenForRead(secret, "Sub/inner.txt", false, &opened).status == 404);
    CHECK(!Send(store, secret, "Sub/new.txt", "x").ok() && !store.Delete(secret, "Sub/inner.txt").ok());
    bool started = false;
    std::string zip;
    CHECK(store.WriteZip(secret, "", 0, [&](const char* d, std::size_t n) { zip.append(d, n); return true; }, [] { return true; }, &started).ok());
    CHECK(zip.find("inner.txt") == std::string::npos);
  }

  // A new file: no previous version, and no temporary left.
  bool replaced = true;
  CHECK(Send(store, root, "new.sav", "first", false, &replaced).ok() && !replaced);
  CHECK(Text(folder / "new.sav") == "first");
  for (const auto& name : Names(folder)) CHECK(name.rfind(".playden-upload-", 0) != 0 && name.rfind(".playden-bak", 0) != 0);
  CHECK((fs::status(folder / "new.sav").permissions() & fs::perms::all) == (fs::perms::owner_read | fs::perms::owner_write));

  // Taken: refused unless asked to replace; a folder is never replaced.
  CHECK(Send(store, root, "new.sav", "x").status == 409 && Text(folder / "new.sav") == "first");
  CHECK(Send(store, root, "Sub", "x", true).status == 409 && fs::is_directory(folder / "Sub"));

  // Replacing keeps one previous version, listed on the file's row.
  CHECK(Send(store, root, "new.sav", "second", true, &replaced).ok() && replaced);
  CHECK(Text(folder / "new.sav") == "second" && Text(folder / ".playden-bak.new.sav") == "first");
  CHECK(Send(store, root, "new.sav", "third", true).ok());
  CHECK(Text(folder / "new.sav") == "third" && Text(folder / ".playden-bak.new.sav") == "second");
  CHECK(store.List(root, "", &listing).ok());
  CHECK(Entry(listing, "new.sav")->previous && Entry(listing, "new.sav")->previous->size == 6 && !Entry(listing, "new.sav")->recoverable);
  CHECK(!Entry(listing, ".playden-bak.new.sav"));
  CHECK(store.OpenForRead(root, "new.sav", true, &opened).ok() && opened.size == 6);
  ::close(opened.fd);

  // Restore swaps the two, and again.
  CHECK(store.Restore(root, "new.sav").ok());
  CHECK(Text(folder / "new.sav") == "second" && Text(folder / ".playden-bak.new.sav") == "third");
  CHECK(store.Restore(root, "new.sav").ok());
  CHECK(Text(folder / "new.sav") == "third" && Text(folder / ".playden-bak.new.sav") == "second");
  CHECK(store.Restore(root, "b.sav").status == 404);

  // Read-only folders.
  CHECK(Send(store, root, "steam_settings/x", "x").status == 403 && Send(store, root, "Steam_Settings/configs.user.ini", "x", true).status == 403);
  CHECK(store.Delete(root, "steam_settings/configs.user.ini").status == 403 && store.Delete(root, "steam_settings").status == 403);
  CHECK(store.MakeFolder(root, "steam_settings/x").status == 403 && store.Restore(root, "steam_settings/x").status == 403);
  CHECK(store.List(root, "steam_settings", &listing).ok() && listing.read_only && Entry(listing, "configs.user.ini")->read_only);
  CHECK(store.OpenForRead(root, "steam_settings/configs.user.ini", false, &opened).ok());
  ::close(opened.fd);

  // Each step of a replace failing: the file or its previous version always remains, named by a state.
  for (const char* step : {"sync-temporary", "remove-backup", "move-current", "sync-backup", "publish", "sync-publish"}) {
    Put(folder / "fault.sav", "old");
    Put(folder / ".playden-bak.fault.sav", "older");
    fail_at = step;
    const auto error = Send(store, root, "fault.sav", "new", true);
    fail_at.clear();
    CHECK(!error.ok());
    const bool current = fs::exists(folder / "fault.sav");
    const bool backup = fs::exists(folder / ".playden-bak.fault.sav");
    CHECK(current || backup);
    const std::string kept = current ? Text(folder / "fault.sav") : Text(folder / ".playden-bak.fault.sav");
    CHECK(kept == "old" || kept == "new");
    // Everything visible after the failure can be listed, and whatever the state, a retry succeeds.
    CHECK(store.List(root, "", &listing).ok() && Entry(listing, "fault.sav"));
    CHECK(Send(store, root, "fault.sav", "again", true).ok() && Text(folder / "fault.sav") == "again");
    fs::remove(folder / "fault.sav");
    fs::remove(folder / ".playden-bak.fault.sav");
    for (const auto& name : Names(folder))
      if (name.rfind(".playden-upload-", 0) == 0) fs::remove(folder / name);
  }

  // Crash states, as a fresh store finds them on disk.
  {
    FileStore fresh{FileStore::Options{}};
    // Only the previous version: Recoverable, downloadable, restorable.
    Put(folder / ".playden-bak.crash.sav", "saved");
    CHECK(fresh.List(root, "", &listing).ok());
    CHECK(Entry(listing, "crash.sav") && Entry(listing, "crash.sav")->recoverable && Entry(listing, "crash.sav")->size == 5);
    CHECK(fresh.OpenForRead(root, "crash.sav", true, &opened).ok());
    ::close(opened.fd);
    CHECK(fresh.Restore(root, "crash.sav").ok() && Text(folder / "crash.sav") == "saved" && !fs::exists(folder / ".playden-bak.crash.sav"));
    fs::remove(folder / "crash.sav");
    // Delete of only a previous version.
    Put(folder / ".playden-bak.crash.sav", "saved");
    CHECK(fresh.Delete(root, "crash.sav").ok() && !fs::exists(folder / ".playden-bak.crash.sav"));
    // A restore's leftovers stay hidden until old; a previous version with them is still restorable.
    Put(folder / ".playden-restore-abc", "aside");
    Put(folder / ".playden-bak.crash.sav", "saved");
    CHECK(fresh.List(root, "", &listing).ok() && !Entry(listing, ".playden-restore-abc") && Entry(listing, "crash.sav")->recoverable);
    CHECK(fresh.Restore(root, "crash.sav").ok() && Text(folder / "crash.sav") == "saved");
    fs::remove(folder / "crash.sav");
    fs::remove(folder / ".playden-restore-abc");
  }

  // A restore failing at each step keeps the data, under a name a later request finds or the sweep puts back.
  for (const char* step : {"restore-aside", "restore-back", "restore-sync", "restore-keep"}) {
    Put(folder / "swap.sav", "cur");
    Put(folder / ".playden-bak.swap.sav", "bak");
    fail_at = step;
    CHECK(!store.Restore(root, "swap.sav").ok());
    fail_at.clear();
    std::set<std::string> texts;
    for (const auto& name : Names(folder))
      if (name.find("swap.sav") != std::string::npos || name.rfind(".playden-restore-", 0) == 0) texts.insert(Text(folder / name));
    CHECK(texts.count("bak") == 1 && texts.count("cur") == 1);
    fs::remove(folder / "swap.sav");
    fs::remove(folder / ".playden-bak.swap.sav");
    for (const auto& name : Names(folder))
      if (name.rfind(".playden-restore-", 0) == 0) fs::remove(folder / name);
  }

  // Leftovers: old ones go at the next upload; a new one stays, and so does an upload's own temporary.
  {
    std::int64_t now = 100000;
    FileStore::Options timed;
    timed.now = [&] { return now; };
    FileStore clock(timed);
    now = ::time(nullptr) + 7200;
    Put(folder / ".playden-upload-old", "x");
    Put(folder / ".playden-restore-old", "x");
    CHECK(Send(clock, root, "sweep.sav", "x").ok());
    CHECK(!fs::exists(folder / ".playden-upload-old") && !fs::exists(folder / ".playden-restore-old"));
    now = ::time(nullptr);
    Put(folder / ".playden-upload-new", "x");
    CHECK(Send(clock, root, "sweep.sav", "y", true).ok() && fs::exists(folder / ".playden-upload-new"));
    fs::remove(folder / ".playden-upload-new");
    // Opening a folder removes what a cut-off upload left, and it doesn't keep a folder that looks empty from going.
    now = ::time(nullptr) + 7200;
    Put(folder / ".playden-upload-cut", "x");
    {
      FileListing swept;
      CHECK(clock.List(root, "", &swept).ok() && !fs::exists(folder / ".playden-upload-cut"));
    }
    Put(folder / "looks-empty/.playden-upload-cut", "x");
    CHECK(clock.Delete(root, "looks-empty").ok() && !fs::exists(folder / "looks-empty"));
    fs::create_directories(folder / "not-empty");
    Put(folder / "not-empty/.playden-bak.keep", "x");
    CHECK(clock.Delete(root, "not-empty").status == 409 && fs::exists(folder / "not-empty/.playden-bak.keep"));
    fs::remove_all(folder / "not-empty");
    now = ::time(nullptr);
    // An active upload is never swept, however old: the clock jumps while it streams, and another upload sweeps.
    now = ::time(nullptr) + 7200;
    std::atomic<int> phase{0};
    UploadOptions slow;
    slow.length = 4;
    std::thread uploading([&] {
      std::size_t sent = 0;
      const auto error = clock.Upload(root, "slow.sav", slow, [&](char* data, std::size_t) -> std::ptrdiff_t {
        if (sent++ == 1) {
          phase = 1;
          while (phase != 2) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (sent > 4) return 0;
        data[0] = 'z';
        return 1;
      }, nullptr);
      CHECK(error.ok());
    });
    while (phase != 1) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(Send(clock, root, "other.sav", "x").ok());
    phase = 2;
    uploading.join();
    CHECK(Text(folder / "slow.sav") == "zzzz");
    fs::remove(folder / "slow.sav");
    fs::remove(folder / "other.sav");
    fs::remove(folder / "sweep.sav");
    fs::remove(folder / ".playden-bak.sweep.sav");
  }

  // A restore that stopped, on a save last changed long ago: its temporary is the player's file, and stays until
  // the temporary itself is old. Then it goes back where it belongs.
  {
    std::int64_t later = ::time(nullptr);
    std::string stop_at = "restore-back";
    FileStore::Options timed;
    timed.now = [&] { return later; };
    timed.fault = [&](const std::string& step) { return step != stop_at; };
    FileStore clock(timed);
    Put(folder / "old.sav", "newest");
    Put(folder / ".playden-bak.old.sav", "older");
    fs::last_write_time(folder / "old.sav", fs::file_time_type::clock::now() - std::chrono::hours(48));
    CHECK(!clock.Restore(root, "old.sav").ok());
    std::string aside;
    for (const auto& name : Names(folder))
      if (name.rfind(".playden-restore-", 0) == 0) aside = name;
    CHECK(!aside.empty() && Text(folder / aside) == "newest");
    stop_at.clear();
    CHECK(Send(clock, root, "other.sav", "x").ok());
    CHECK(fs::exists(folder / aside));
    later += 7200;
    CHECK(Send(clock, root, "other.sav", "y", true).ok());
    CHECK(!fs::exists(folder / aside) && Text(folder / "old.sav") == "newest" && Text(folder / ".playden-bak.old.sav") == "older");
    // Stopped after the previous version came back: the swap finishes.
    stop_at = "restore-keep";
    CHECK(!clock.Restore(root, "old.sav").ok());
    stop_at.clear();
    CHECK(Text(folder / "old.sav") == "older" && !fs::exists(folder / ".playden-bak.old.sav"));
    later += 7200;
    {
      FileListing swept;
      CHECK(clock.List(root, "", &swept).ok());
    }
    CHECK(Text(folder / "old.sav") == "older" && Text(folder / ".playden-bak.old.sav") == "newest");
    // The file and its previous version are both back: the temporary was replaced as usual.
    Put(folder / ".playden-restore-1-x.old.sav", "oldest");
    CHECK(Send(clock, root, "other.sav", "z", true).ok());
    CHECK(!fs::exists(folder / ".playden-restore-1-x.old.sav") && Text(folder / "old.sav") == "older" && Text(folder / ".playden-bak.old.sav") == "newest");
    fs::remove(folder / "old.sav");
    fs::remove(folder / "other.sav");
    fs::remove(folder / ".playden-bak.other.sav");
    fs::remove(folder / ".playden-bak.old.sav");
  }

  // A cut upload: the old file stays, and its temporary goes.
  {
    Put(folder / "cut.sav", "old");
    UploadOptions cut;
    cut.length = 100;
    cut.overwrite = true;
    int reads = 0;
    CHECK(store.Upload(root, "cut.sav", cut, [&](char* data, std::size_t) -> std::ptrdiff_t {
      if (reads++ > 0) return -1;
      data[0] = 'n';
      return 1;
    }, nullptr).status == 408);
    CHECK(Text(folder / "cut.sav") == "old");
    for (const auto& name : Names(folder)) CHECK(name.rfind(".playden-upload-", 0) != 0);
    CHECK(!fs::exists(folder / ".playden-bak.cut.sav"));
    fs::remove(folder / "cut.sav");
  }

  // The path lock: never held while bytes move; busy answers 503; a failed upload's cleanup waits for it.
  {
    std::atomic<bool> busy{false};
    std::atomic<int> holds_live{0};
    FileStore::Options locked;
    locked.hold_tries = 2;
    locked.hold_retry_ms = 1;
    locked.try_hold = [&]() -> std::shared_ptr<void> {
      if (busy) return nullptr;
      ++holds_live;
      return std::shared_ptr<void>(new int(0), [&](void* held) { delete static_cast<int*>(held); --holds_live; });
    };
    FileStore guarded(locked);
    busy = true;
    CHECK(guarded.List(root, "", &listing).status == 503);
    CHECK(guarded.OpenForRead(root, "b.sav", false, &opened).status == 503);
    CHECK(Send(guarded, root, "held.sav", "x").status == 503);
    CHECK(guarded.Delete(root, "b.sav").status == 503 && guarded.MakeFolder(root, "held").status == 503 && guarded.Restore(root, "b.sav").status == 503);
    busy = false;
    CHECK(guarded.OpenForRead(root, "b.sav", false, &opened).ok() && holds_live == 0);
    ::close(opened.fd);
    UploadOptions watched;
    watched.length = 4;
    int reads = 0;
    CHECK(guarded.Upload(root, "held.sav", watched, [&](char* data, std::size_t) -> std::ptrdiff_t {
      CHECK(holds_live == 0);
      if (++reads > 4) return 0;
      data[0] = 'h';
      return 1;
    }, nullptr).ok());
    CHECK(Text(folder / "held.sav") == "hhhh" && holds_live == 0);
    fs::remove(folder / "held.sav");
    // The lock taken during the upload, and the client goes away: the temporary waits, then goes.
    watched.overwrite = false;
    reads = 0;
    CHECK(guarded.Upload(root, "held.sav", watched, [&](char* data, std::size_t) -> std::ptrdiff_t {
      if (reads++ == 1) { busy = true; return -1; }
      data[0] = 'h';
      return 1;
    }, nullptr).status == 408);
    CHECK(guarded.DeferredCleanups() == 1);
    bool left = false;
    for (const auto& name : Names(folder)) left = left || name.rfind(".playden-upload-", 0) == 0;
    CHECK(left);
    busy = false;
    CHECK(guarded.List(root, "", &listing).ok() && guarded.DeferredCleanups() == 0);
    for (const auto& name : Names(folder)) CHECK(name.rfind(".playden-upload-", 0) != 0);
    // Busy at the commit, after the bytes arrived: the old file stays, and the upload says busy.
    Put(folder / "late.sav", "old");
    watched.overwrite = true;
    reads = 0;
    CHECK(guarded.Upload(root, "late.sav", watched, [&](char* data, std::size_t) -> std::ptrdiff_t {
      if (++reads > 4) return 0;
      if (reads == 4) busy = true;
      data[0] = 'n';
      return 1;
    }, nullptr).status == 503);
    CHECK(Text(folder / "late.sav") == "old");
    busy = false;
    CHECK(guarded.List(root, "", &listing).ok());
    fs::remove(folder / "late.sav");
  }

  // Two uploads racing for a free name: one wins, one gets 409, and the winner's bytes are whole.
  {
    for (int round = 0; round < 20; ++round) {
      const auto name = "race" + std::to_string(round) + ".sav";
      std::atomic<int> ready{0};
      FileError results[2];
      std::thread racers[2];
      for (int i = 0; i < 2; ++i)
        racers[i] = std::thread([&, i] {
          const std::string body(1024, static_cast<char>('a' + i));
          UploadOptions race;
          race.length = body.size();
          auto next = Reader(body);
          bool waited = false;
          results[i] = store.Upload(root, name, race, [&](char* data, std::size_t size) -> std::ptrdiff_t {
            if (!waited) {
              waited = true;
              ++ready;
              while (ready != 2) std::this_thread::yield();
            }
            return next(data, size);
          }, nullptr);
        });
      for (auto& racer : racers) racer.join();
      CHECK(results[0].ok() != results[1].ok());
      CHECK((results[0].ok() ? results[1] : results[0]).status == 409);
      const auto saved = Text(folder / name);
      CHECK(saved == std::string(1024, results[0].ok() ? 'a' : 'b'));
      fs::remove(folder / name);
    }
    for (const auto& name : Names(folder)) CHECK(name.rfind(".playden-upload-", 0) != 0);
  }
  // Restore and delete racing: at the end the data is either gone by request or whole, never half swapped.
  {
    for (int round = 0; round < 20; ++round) {
      Put(folder / "both.sav", "cur");
      Put(folder / ".playden-bak.both.sav", "bak");
      std::thread restoring([&] { store.Restore(root, "both.sav"); });
      std::thread deleting([&] { store.Delete(root, "both.sav"); });
      restoring.join();
      deleting.join();
      CHECK(!fs::exists(folder / "both.sav") && !fs::exists(folder / ".playden-bak.both.sav"));
      for (const auto& name : Names(folder)) CHECK(name.rfind(".playden-restore-", 0) != 0);
    }
  }

  // The hash an editor saw: refused once the file changed, with the same size and time too.
  {
    Put(folder / "conf.ini", "a=1\n");
    CHECK(store.ReadText(root, "conf.ini", &text).ok() && text.text == "a=1\n");
    const auto hash = text.sha256;
    CHECK(Send(store, root, "conf.ini", "a=2\n", true, nullptr, hash).ok() && Text(folder / "conf.ini") == "a=2\n");
    const auto written = fs::last_write_time(folder / "conf.ini");
    Put(folder / "conf.ini", "a=3\n");
    fs::last_write_time(folder / "conf.ini", written);
    CHECK(Send(store, root, "conf.ini", "a=4\n", true, nullptr, hash).status == 409 && Text(folder / "conf.ini") == "a=3\n");
    // Changed while the upload was on its way: refused at the commit, and no lock held for the read.
    CHECK(store.ReadText(root, "conf.ini", &text).ok());
    {
      UploadOptions during;
      during.length = 4;
      during.overwrite = true;
      during.if_sha256 = text.sha256;
      const std::string payload = "a=5\n";
      std::size_t sent = 0;
      CHECK(store.Upload(root, "conf.ini", during, [&](char* data, std::size_t) -> std::ptrdiff_t {
        if (sent == 0) Put(folder / "conf.ini", "a=9\n");
        if (sent >= payload.size()) return 0;
        data[0] = payload[sent++];
        return 1;
      }, nullptr).status == 409);
      CHECK(Text(folder / "conf.ini") == "a=9\n");
    }
    fs::remove(folder / ".playden-bak.conf.ini");
    fs::remove(folder / "conf.ini");
    CHECK(Send(store, root, "conf.ini", "a=4\n", true, nullptr, hash).status == 409 && !fs::exists(folder / "conf.ini"));
    Put(folder / "binary.dat", std::string("\xff\xfe\0", 3));
    CHECK(store.ReadText(root, "binary.dat", &text).status == 415);
    Put(folder / "large.txt", std::string(kMaxTextBytes + 1, 'a'));
    CHECK(store.ReadText(root, "large.txt", &text).status == 413);
    fs::remove(folder / "binary.dat");
    fs::remove(folder / "large.txt");
  }

  // New folders and deletes.
  CHECK(store.MakeFolder(root, "made").ok() && fs::is_directory(folder / "made"));
  CHECK(store.MakeFolder(root, "made").status == 409 && store.MakeFolder(root, "a/b").status == 404);
  CHECK(store.MakeFolder(root, ".playden-x").status == 400 && store.MakeFolder(root, "bad\\name").status == 400);
  // A whole path at once, for a folder upload: what's there stays, a file in the way says so.
  {
    CHECK(store.MakeFolder(root, "tree/a/b", true).ok() && fs::is_directory(folder / "tree/a/b"));
    Put(folder / "tree/a/keep.txt", "kept");
    CHECK(store.MakeFolder(root, "tree/a/b", true).ok() && Text(folder / "tree/a/keep.txt") == "kept");
    CHECK(store.MakeFolder(root, "tree/a/c", true).ok() && fs::is_directory(folder / "tree/a/c"));
    const auto blocked = store.MakeFolder(root, "tree/a/keep.txt/d", true);
    CHECK(blocked.status == 409 && blocked.message.find("keep.txt") != std::string::npos && Text(folder / "tree/a/keep.txt") == "kept");
    CHECK(store.MakeFolder(root, "tree/a/keep.txt", true).status == 409);
    CHECK(store.MakeFolder(root, "tree/.playden-x/d", true).status == 400 && !fs::exists(folder / "tree/.playden-x"));
    CHECK(store.MakeFolder(root, "tree/playden_online.txt", true).status == 400);
    CHECK(store.MakeFolder(root, "tree/steam_settings/d", true).status == 403 && !fs::exists(folder / "tree/steam_settings"));
    CHECK(store.MakeFolder(root, "", true).status == 400);
    const std::string too_long(kMaxNewNameBytes + 1, 'n');
    CHECK(store.MakeFolder(root, "tree/" + too_long, true).status == 400);
    fs::create_directories(base / "elsewhere");
    fs::create_symlink(base / "elsewhere", folder / "tree/link");
    CHECK(!store.MakeFolder(root, "tree/link/d", true).ok() && !fs::exists(base / "elsewhere/d"));
    fs::remove_all(folder / "tree");
    fs::remove_all(base / "elsewhere");
  }
  CHECK(Send(store, root, "made/f.txt", "x").ok());
  CHECK(store.Delete(root, "made").status == 409 && fs::exists(folder / "made/f.txt"));
  CHECK(Send(store, root, "made/f.txt", "y", true).ok() && fs::exists(folder / "made/.playden-bak.f.txt"));
  CHECK(store.Delete(root, "made/f.txt").ok() && !fs::exists(folder / "made/f.txt") && !fs::exists(folder / "made/.playden-bak.f.txt"));
  CHECK(store.Delete(root, "made").ok() && !fs::exists(folder / "made"));
  CHECK(store.Delete(root, "made").status == 404);

  // A root not made yet: empty to read, made by the first write, from an existing folder only.
  {
    const auto bottle = base / "bottle";
    fs::create_directories(bottle / "drive_c");
    FileRoot steam;
    steam.path = (bottle / "drive_c/Program Files (x86)/Steam/userdata/0").string();
    steam.create_from = bottle.string();
    CHECK(store.List(steam, "", &listing).ok() && !listing.exists && listing.entries.empty());
    CHECK(Send(store, steam, "sub/x.sav", "x").status == 404);
    CHECK(Send(store, steam, "x.sav", "x").ok() && Text(steam.path + "/x.sav") == "x");
    CHECK((fs::status(steam.path).permissions() & fs::perms::all) == fs::perms::owner_all);
    FileRoot never = steam;
    never.path = (base / "nothing/here").string();
    never.create_from.clear();
    CHECK(Send(store, never, "x.sav", "x").status == 404 && !fs::exists(base / "nothing"));
    // Bottle modes: a shared profile and a private one both work.
    for (const auto mode : {fs::perms::owner_all, fs::perms::all}) {
      const auto profile = bottle / "drive_c/users/prospero";
      fs::create_directories(profile);
      fs::permissions(profile, mode);
      fs::permissions(bottle / "drive_c/users", fs::perms::owner_all | fs::perms::group_exec | fs::perms::others_exec);
      CHECK(store.List(Root(profile), "", &listing).ok());
      CHECK(Send(store, Root(profile), "m.sav", "x", true).ok());
    }
  }

  // A game folder with no owners (exFAT): the write asks once; a reader never makes it ask.
  {
    static int probes = 0;
    SetOwnershipProbeForTesting([](const fs::path&) -> std::optional<bool> { ++probes; return false; });
    FileStore game_store{FileStore::Options{}};
    const auto game = base / "game";
    Put(game / "bin/game.exe", "exe");
    Put(game / "empty.dat", "");
    FileRoot game_root = Root(game);
    game_root.game_folder = true;
    game_root.file_mode = 0666;
    game_root.folder_mode = 0777;
    CHECK(game_store.List(game_root, "", &listing).ok() && Entry(listing, "empty.dat")->size == 0);
    CHECK(game_store.OpenForRead(game_root, "empty.dat", false, &opened).ok());
    ::close(opened.fd);
    CHECK(probes == 0);
    CHECK(Send(game_store, game_root, "bin/mod.dll", "m").ok() && probes == 1);
    CHECK(Send(game_store, game_root, "bin/mod.dll", "n", true).ok() && probes == 1);
    CHECK(Text(game / "bin/mod.dll") == "n" && Text(game / "bin/.playden-bak.mod.dll") == "m");
    CHECK(game_store.Restore(game_root, "bin/mod.dll").ok() && Text(game / "bin/mod.dll") == "m");
    CHECK(game_store.MakeFolder(game_root, "_Redist").ok());
    SetOwnershipProbeForTesting(nullptr);
    // Keeping owners: the modes of a game folder.
    FileStore owned{FileStore::Options{}};
    CHECK(Send(owned, game_root, "bin/own.dll", "o").ok());
    CHECK((fs::status(game / "bin/own.dll").permissions() & fs::perms::all) ==
          (fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read | fs::perms::group_write | fs::perms::others_read | fs::perms::others_write));
    CHECK(owned.MakeFolder(game_root, "owned").ok());
    CHECK((fs::status(game / "owned").permissions() & fs::perms::all) == fs::perms::all);

    // Redistributable folders, by Play's rules: at the top and one level down.
    Put(game / "_Redist/PhysX_9.09.msi", "p");
    Put(game / "Inner/DirectX/DXSETUP.exe", "d");
    Put(game / "Inner/Bin/PhysX_9.exe", "not in a redist folder");
    Put(game / "Inner/Other/Support/deep.txt", "too far down");
    std::vector<RedistFolderInfo> redist;
    CHECK(owned.RedistFolders(game_root, &redist).ok() && redist.size() == 2);
    CHECK(redist[0].path == "_Redist" && redist[0].physx_installer == "_Redist/PhysX_9.09.msi" && redist[0].entries == 1);
    CHECK(redist[1].path == "Inner/DirectX" && redist[1].physx_installer.empty() && redist[1].sample[0] == "DXSETUP.exe");
  }

  // A file another user owns can't be replaced or deleted, in plain words. Only root can arrange it.
  if (::geteuid() == 0) {
    Put(folder / "theirs.sav", "x");
    CHECK(::chown((folder / "theirs.sav").c_str(), 12345, 12345) == 0);
    CHECK(Send(store, root, "theirs.sav", "y", true).status == 403);
    CHECK(store.Delete(root, "theirs.sav").status == 403);
  }

  // A folder as a zip.
  {
    const auto zipped = base / "zipped";
    Put(zipped / "a.txt", "alpha");
    Put(zipped / "deep/er/b.txt", "bravo");
    Put(zipped / ".playden-bak.a.txt", "hidden");
    Put(zipped / "playden_online.txt", "token");
    Put(base / "outside2.txt", "secret");
    fs::create_symlink(base / "outside2.txt", zipped / "link.txt");
    fs::create_hard_link(base / "outside2.txt", zipped / "hard.txt");
    std::string zip;
    bool started = false, began = false;
    const auto error = store.WriteZip(Root(zipped), "", 1700000000, [&](const char* data, std::size_t size) { zip.append(data, size); return true; },
                                      [&] { began = true; return true; }, &started);
    CHECK(error.ok() && began && started);
    CHECK(zip.find("zipped/a.txt") != std::string::npos && zip.find("zipped/deep/er/b.txt") != std::string::npos);
    CHECK(zip.find("alpha") != std::string::npos && zip.find("bravo") != std::string::npos);
    for (const char* absent : {"hidden", "token", "secret", "link.txt", "hard.txt", ".playden"}) CHECK(zip.find(absent) == std::string::npos);
    const auto archive = base / "out.zip";
    std::ofstream(archive, std::ios::binary) << zip;
    const auto check = "python3 -I -c \"import zipfile,sys; z=zipfile.ZipFile(sys.argv[1]); assert z.testzip() is None; assert sorted(z.namelist())==['zipped/a.txt','zipped/deep/er/b.txt']; "
                       "assert z.read('zipped/a.txt')==b'alpha'; assert all(i.flag_bits & 8 for i in z.infolist())\" '" + archive.string() + "'";
    CHECK(std::system(check.c_str()) == 0);
    // A subfolder is named from itself.
    zip.clear();
    CHECK(store.WriteZip(Root(zipped), "deep", 0, [&](const char* data, std::size_t size) { zip.append(data, size); return true; }, [] { return true; }, &started).ok());
    CHECK(zip.find("deep/er/b.txt") != std::string::npos && zip.find("zipped/") == std::string::npos);
    // A folder deeper than a zip goes is refused before the first byte, not left out.
    {
      auto deep = base / "deep";
      for (int i = 0; i < 40; ++i) deep /= "d";
      Put(deep / "lost.txt", "x");
      began = false;
      CHECK(store.WriteZip(Root(base / "deep"), "", 0, [&](const char*, std::size_t) { return true; }, [&] { began = true; return true; }, &started).status == 413);
      CHECK(!began && !started);
    }
    // Nothing goes out when it won't fit: a sparse file past the limit is 413 before the first byte.
    const auto large = base / "large";
    fs::create_directories(large);
    { std::ofstream(large / "big.bin"); }
    fs::resize_file(large / "big.bin", 0x80000000ull);
    began = false;
    CHECK(store.WriteZip(Root(large), "", 0, [&](const char*, std::size_t) { return true; }, [&] { began = true; return true; }, &started).status == 413);
    CHECK(!began && !started);
  }

  fs::remove_all(base);
  std::puts("web_file_store: ok");
}
