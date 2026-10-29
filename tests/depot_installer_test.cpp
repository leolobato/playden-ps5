#include "steam/backend/depot_chunk.h"
#include "steam/backend/depot_installer.h"
#include "steam/backend/steam_install_runner.h"
#include "installs/install_record.h"
#include "steam/install_volumes.h"

#include <mbedtls/aes.h>
#include <mbedtls/sha1.h>

#include <algorithm>
#include <functional>
#include <optional>
#include <set>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <vector>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using namespace playden;
using namespace playden::steam;

#define CHECK(condition)                                                     \
  do {                                                                       \
    if (!(condition)) {                                                      \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
      std::exit(1);                                                          \
    }                                                                        \
  } while (false)

namespace {

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

// A one-entry stored zip, as Steam's "PK" chunk containers.
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

// Steam symmetric encryption: AES-256-ECB(IV) followed by AES-256-CBC(IV) with PKCS7.
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

struct World {
  std::map<std::string, std::vector<std::uint8_t>> served;  // sha hex -> encrypted container
  std::map<std::uint32_t, DepotManifest> manifests;
  int chunk_calls = 0;
  int cancel_after = -1;
  bool corrupt = false;
  int busy_calls = 0;  // the first chunk requests answer as a busy CDN would
  std::uint64_t free_bytes = 1ull << 40;
  std::atomic<bool>* cancel = nullptr;

  ManifestChunk AddChunk(const std::vector<std::uint8_t>& plain, std::uint64_t offset) {
    ManifestChunk chunk;
    chunk.sha = Sha1(plain);
    chunk.offset = offset;
    chunk.original_size = static_cast<std::uint32_t>(plain.size());
    const auto container = Encrypt(StoredZip(plain));
    chunk.compressed_size = static_cast<std::uint32_t>(container.size());
    served[HexOf(chunk.sha)] = container;
    return chunk;
  }
  ManifestFile AddFile(const std::string& path, const std::vector<std::string>& pieces) {
    ManifestFile file;
    file.path = path;
    std::uint64_t offset = 0;
    std::vector<std::uint8_t> content;
    for (const auto& piece : pieces) {
      std::vector<std::uint8_t> plain(piece.begin(), piece.end());
      file.chunks.push_back(AddChunk(plain, offset));
      offset += plain.size();
      content.insert(content.end(), plain.begin(), plain.end());
    }
    file.size = offset;
    file.sha_content = Sha1(content);
    return file;
  }

  InstallerHooks Hooks() {
    InstallerHooks hooks;
    hooks.access = [this](const StoredSession&, std::uint32_t, const std::vector<DepotRequest>& depots,
                          const std::atomic<bool>&) {
      DepotAccessResult result;
      result.error = CmError::kNone;
      for (const auto& depot : depots) result.grants.push_back({depot.depot_id, depot.manifest_gid, kKey, 77});
      return result;
    };
    hooks.servers = [](std::uint32_t, const std::atomic<bool>&, std::vector<ContentServer>* out) {
      *out = {{"a.example.invalid", "a.example.invalid", 1}, {"b.example.invalid", "b.example.invalid", 2}};
      return CdnError::kNone;
    };
    hooks.manifest = [this](const ContentServer&, const DepotGrant& grant, const std::atomic<bool>&, DepotManifest* out) {
      *out = manifests.at(grant.depot_id);
      return InstallError::kNone;
    };
    hooks.chunk = [this](const ContentServer&, std::uint32_t, const std::string& sha, std::size_t,
                         const std::atomic<bool>&, std::vector<std::uint8_t>* out) {
      ++chunk_calls;
      if (cancel_after >= 0 && chunk_calls > cancel_after && cancel) { cancel->store(true); return CdnError::kCanceled; }
      if (busy_calls > 0) { --busy_calls; return CdnError::kNetwork; }
      *out = served.at(sha);
      if (corrupt) out->back() ^= 0xff;
      return CdnError::kNone;
    };
    hooks.free_bytes = [this](const std::string&) { return free_bytes; };
    return hooks;
  }
};

std::string Read(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

InstallTarget Target(const std::filesystem::path& root) {
  InstallTarget target;
  target.app_id = 440;
  target.install_root = root.string();
  DepotInfo base;
  base.id = 441;
  base.manifest_gid = 11;
  DepotInfo patch;
  patch.id = 442;
  patch.manifest_gid = 12;
  target.depots = {base, patch};
  return target;
}

World MakeWorld() {
  World world;
  DepotManifest base;
  base.depot_id = 441;
  base.gid = 11;
  base.files.push_back(world.AddFile("game.exe", {"MZ-executable-", "second-chunk"}));
  ManifestFile folder;
  folder.path = "data";
  folder.flags = kDepotFileFlagDirectory;
  base.files.push_back(folder);
  base.files.push_back(world.AddFile("data/level.dat", {"level-one"}));
  base.files.push_back(world.AddFile("empty.txt", {}));
  DepotManifest patch;
  patch.depot_id = 442;
  patch.gid = 12;
  patch.files.push_back(world.AddFile("DATA/level.dat", {"level-patched"}));  // later depot wins, any case
  world.manifests[441] = base;
  world.manifests[442] = patch;
  return world;
}

void OverlapsFourFetches(const std::filesystem::path& root) {
  auto world = MakeWorld();
  world.manifests[441].files = {world.AddFile("parallel", {"a", "b", "c", "d", "e", "f", "g", "h"})};
  world.manifests[442].files.clear();
  auto hooks = world.Hooks();
  std::mutex mutex;
  std::condition_variable changed;
  int active = 0, peak = 0, entered = 0;
  hooks.chunk = [&](const ContentServer&, std::uint32_t, const std::string& sha, std::size_t,
                    const std::atomic<bool>&, std::vector<std::uint8_t>* out) {
    std::unique_lock<std::mutex> lock(mutex);
    ++active; ++entered;
    peak = std::max(peak, active);
    changed.notify_all();
    changed.wait_for(lock, std::chrono::milliseconds(250), [&] { return entered >= 4; });
    lock.unlock();
    *out = world.served.at(sha);
    lock.lock();
    --active;
    return CdnError::kNone;
  };
  DepotInstaller installer(hooks);
  std::atomic<bool> cancel{false};
  const auto outcome = installer.Run({}, Target(root), cancel);
  CHECK(outcome.error == InstallError::kNone);
  CHECK(peak == 4);
  CHECK(Read(root / "parallel") == "abcdefgh");
}

struct ParallelFixture {
  World world = MakeWorld();
  std::mutex mutex;
  std::condition_variable changed;
  int entered = 0, finished = 0, active = 0, peak = 0, factories = 0, pauses = 0;
  bool release = false, hold_first = false, fail_first = false, corrupt_first = false, backoff = false;
  std::atomic<int> callbacks{0}, written{0};
  InstallStats last;
  std::string first_sha;
  ParallelFixture() {
    world.manifests[441].files = {world.AddFile("parallel", {"a", "b", "c", "d", "e", "f", "g", "h"})};
    world.manifests[442].files.clear();
    first_sha = HexOf(world.manifests[441].files[0].chunks[0].sha);
  }
  bool Wait(const std::function<bool()>& predicate) {
    std::unique_lock<std::mutex> lock(mutex);
    return changed.wait_for(lock, std::chrono::seconds(3), predicate);
  }
  void Release() {
    std::lock_guard<std::mutex> lock(mutex);
    release = true; changed.notify_all();
  }
  InstallerHooks Hooks(int invalid_factory = -1) {
    auto hooks = world.Hooks();
    hooks.make_fetcher = [this, invalid_factory] {
      const int number = ++factories;
      InstallerHooks::ChunkFetcher fetcher;
      if (number == invalid_factory) return fetcher;
      struct State {
        std::thread::id owner;
        std::uint64_t requests = 0;
        int corruptions = 0;
        ~State() { CHECK(owner == std::thread::id{} || owner == std::this_thread::get_id()); }
      };
      auto state = std::make_shared<State>();
      fetcher.fetch = [this, state](const ContentServer&, std::uint32_t, const std::string& sha, std::size_t,
                                   const std::atomic<bool>& stop, std::vector<std::uint8_t>* out) {
        if (state->owner == std::thread::id{}) state->owner = std::this_thread::get_id();
        CHECK(state->owner == std::this_thread::get_id());
        ++state->requests;
        std::unique_lock<std::mutex> lock(mutex);
        ++entered; ++active; peak = std::max(peak, active); changed.notify_all();
        const bool first = sha == first_sha;
        while (!stop.load() && !(backoff || (fail_first && first && entered >= 4) || (release && (!hold_first || !first))))
          changed.wait_for(lock, std::chrono::milliseconds(10));
        CdnError error = CdnError::kNone;
        if (stop.load()) error = CdnError::kCanceled;
        else if (backoff || (fail_first && first)) error = CdnError::kNetwork;
        else {
          *out = world.served.at(sha);
          if (corrupt_first && first && state->corruptions++ == 0) out->back() ^= 0xff;
        }
        --active; ++finished; changed.notify_all();
        return error;
      };
      fetcher.last_failure = [state] {
        CHECK(state->owner == std::this_thread::get_id());
        return std::string("http=503");
      };
      fetcher.stats = [state] {
        if (state->owner == std::thread::id{}) state->owner = std::this_thread::get_id();
        CHECK(state->owner == std::this_thread::get_id());
        CdnStats stats; stats.requests = state->requests; return stats;
      };
      return fetcher;
    };
    hooks.pause = [this](int, const std::atomic<bool>& stop) {
      if (!backoff) return;
      std::unique_lock<std::mutex> lock(mutex);
      ++pauses; changed.notify_all();
      while (!stop.load()) changed.wait_for(lock, std::chrono::milliseconds(10));
    };
    hooks.stats = [this](const char* phase, const InstallStats& stats) {
      ++callbacks;
      if (std::string(phase) == "end") { CHECK(active == 0); last = stats; }
    };
    return hooks;
  }
};

void OutOfOrderAndBoundedResults(const std::filesystem::path& root) {
  ParallelFixture fixture;
  fixture.hold_first = true;
  auto hooks = fixture.Hooks();
  const auto coordinator = std::this_thread::get_id();
  DepotInstaller installer(hooks);
  std::atomic<bool> cancel{false};
  InstallOutcome outcome;
  std::uint64_t previous = 0;
  bool block_callback = true;
  std::thread caller([&] {
    const auto owner = std::this_thread::get_id();
    outcome = installer.Run({}, Target(root), cancel, [&](const InstallProgress& progress) {
      CHECK(std::this_thread::get_id() == owner && owner != coordinator);
      CHECK(progress.download_done >= previous); previous = progress.download_done;
      ++fixture.callbacks;
      if (progress.download_done == 0) return;
      ++fixture.written;
      std::unique_lock<std::mutex> lock(fixture.mutex);
      fixture.changed.notify_all();
      while (block_callback) fixture.changed.wait(lock);
    });
  });
  CHECK(fixture.Wait([&] { return fixture.entered == 4; }));
  fixture.Release();
  CHECK(fixture.Wait([&] { return fixture.finished == 3 && fixture.written == 1; }));
  {
    std::lock_guard<std::mutex> lock(fixture.mutex);
    CHECK(fixture.entered == 4); // the writing result still owns its slot
    CHECK(Read(root / "parallel")[0] == '\0'); // first chunk remains in flight
    fixture.hold_first = false; block_callback = false; fixture.changed.notify_all();
  }
  caller.join();
  CHECK(outcome.error == InstallError::kNone);
  CHECK(Read(root / "parallel") == "abcdefgh");
  CHECK(outcome.progress.download_done == outcome.progress.download_total);
  CHECK(fixture.last.requests == 8 && fixture.last.chunks_done == 8);
  CHECK(fixture.last.peak_active == 4 && fixture.last.peak_waiting <= 4 && fixture.last.peak_waiting >= 1);
}

void EightSlotsAndShape(const std::filesystem::path& root) {
  ParallelFixture fixture;
  auto hooks = fixture.Hooks();
  const auto stats = hooks.stats;
  hooks.stats = [&](const char* phase, const InstallStats& value) {
    stats(phase, value);
    if (std::string(phase) == "shape") {
      const auto detail = FormatInstallStats(phase, value);
      CHECK(detail.find("slots=8") != std::string::npos && detail.find("ahead=5") != std::string::npos);
    }
  };
  DepotInstaller installer(hooks);
  installer.SetTuning({8, 5});
  std::atomic<bool> cancel{false};
  InstallOutcome result;
  std::thread caller([&] { result = installer.Run({}, Target(root), cancel); });
  CHECK(fixture.Wait([&] { return fixture.entered == 8; }));
  fixture.Release();
  caller.join();
  CHECK(result.error == InstallError::kNone && fixture.peak == 8 && fixture.last.peak_active == 8);
  CHECK(Read(root / "parallel") == "abcdefgh");
}

void CancelStopsAllWorkers(const std::filesystem::path& root, bool during_backoff) {
  ParallelFixture fixture;
  fixture.backoff = during_backoff;
  DepotInstaller installer(fixture.Hooks());
  std::atomic<bool> cancel{false};
  InstallOutcome outcome;
  std::thread caller([&] {
    outcome = installer.Run({}, Target(root), cancel, [&](const InstallProgress&) { ++fixture.callbacks; });
  });
  CHECK(fixture.Wait([&] { return during_backoff ? fixture.pauses == 4 : fixture.entered == 4; }));
  cancel.store(true);
  caller.join();
  CHECK(outcome.error == InstallError::kCanceled && fixture.active == 0);
  CHECK(fixture.last.chunks_done == 0);
  const auto calls = fixture.callbacks.load();
  const auto fetches = fixture.entered;
  usleep(20000);
  CHECK(fixture.callbacks == calls && fixture.entered == fetches);
  CHECK(Read(root / DepotInstaller::kJournalDir / "journal") == "#v2 441:11,442:12\n");
}

void WorkerFailureWinsAndJoins(const std::filesystem::path& root) {
  ParallelFixture fixture;
  fixture.fail_first = true;
  DepotInstaller installer(fixture.Hooks());
  std::atomic<bool> cancel{false};
  const auto result = installer.Run({}, Target(root), cancel);
  CHECK(result.error == InstallError::kNetwork && result.detail == "chunk unavailable http=503");
  CHECK(fixture.active == 0 && fixture.last.chunks_done == 0);
  CHECK(fixture.last.chunk_retries == 5);
}

void InvalidFactoryJoinsStartedWorkers(const std::filesystem::path& root) {
  ParallelFixture fixture;
  DepotInstaller installer(fixture.Hooks(3));
  std::atomic<bool> cancel{false};
  const auto result = installer.Run({}, Target(root), cancel);
  CHECK(result.error == InstallError::kNetwork && result.detail == "cannot create fetcher");
  CHECK(fixture.factories == 3 && fixture.active == 0);
}

void WriteFailureStopsWorkers(const std::filesystem::path& root) {
  ParallelFixture fixture;
  DepotInstaller installer(fixture.Hooks());
  std::atomic<bool> cancel{false};
  InstallOutcome outcome;
  std::thread caller([&] { outcome = installer.Run({}, Target(root), cancel); });
  CHECK(fixture.Wait([&] { return fixture.entered == 4; }));
  // Wait for the coordinator to prepare the file while all fetches are blocked.
  for (int tries = 0; tries < 300 && !std::filesystem::exists(root / "parallel"); ++tries) usleep(10000);
  CHECK(std::filesystem::is_regular_file(root / "parallel"));
  std::filesystem::remove(root / "parallel");
  std::filesystem::create_directory(root / "parallel");
  fixture.Release();
  caller.join();
  CHECK(outcome.error == InstallError::kStorage && outcome.detail == "cannot write file");
  CHECK(fixture.active == 0 && fixture.last.chunks_done == 0);
  CHECK(Read(root / DepotInstaller::kJournalDir / "journal") == "#v2 441:11,442:12\n");
}

void ParallelCorruptRetryAndResume(const std::filesystem::path& root) {
  ParallelFixture fixture;
  fixture.corrupt_first = true;
  fixture.Release();
  DepotInstaller installer(fixture.Hooks());
  std::atomic<bool> cancel{false};
  CHECK(installer.Run({}, Target(root), cancel).error == InstallError::kNone);
  CHECK(fixture.last.corrupt_retries == 1 && fixture.last.chunk_retries == 1 && fixture.last.requests == 9);
  CHECK(Read(root / "parallel") == "abcdefgh");
  const auto resumed_root = root / "resumed";
  std::filesystem::create_directories(resumed_root / DepotInstaller::kJournalDir);
  std::ofstream(resumed_root / "parallel", std::ios::binary) << "a_c_e_g_";
  std::ofstream journal(resumed_root / DepotInstaller::kJournalDir / "journal");
  for (int offset : {0, 2, 4, 6}) journal << "441\t" << offset << "\tparallel\n";
  journal.close();
  fixture.corrupt_first = false;
  fixture.entered = fixture.finished = 0;
  CHECK(installer.Run({}, Target(resumed_root), cancel).error == InstallError::kNone);
  CHECK(fixture.entered == 4 && fixture.last.requests == 4 && fixture.last.chunks_skipped == 4);
  CHECK(Read(resumed_root / "parallel") == "abcdefgh");
}

void FileCreationAheadIsBounded(const std::filesystem::path& root) {
  ParallelFixture fixture;
  fixture.world.manifests[441].files.clear();
  for (int index = 0; index < 20; ++index)
    fixture.world.manifests[441].files.push_back(fixture.world.AddFile("folder/f" + std::to_string(100 + index), {std::to_string(index)}));
  DepotInstaller installer(fixture.Hooks());
  installer.SetTuning({4, 3});
  std::atomic<bool> cancel{false};
  InstallOutcome outcome;
  std::thread caller([&] { outcome = installer.Run({}, Target(root), cancel); });
  CHECK(fixture.Wait([&] { return fixture.entered == 4; }));
  usleep(80000);
  int count = 0;
  for (const auto& entry : std::filesystem::directory_iterator(root / "folder")) if (entry.is_regular_file()) ++count;
  CHECK(count == 3);
  fixture.Release();
  caller.join();
  CHECK(outcome.error == InstallError::kNone && fixture.last.chunks_done == 20);
}

void ParallelMergedAndFullyResumed(const std::filesystem::path& root) {
  auto world = MakeWorld();
  auto hooks = world.Hooks();
  const auto original = hooks.chunk;
  std::mutex mutex;
  hooks.chunk = [&](const ContentServer& server, std::uint32_t depot, const std::string& sha, std::size_t cap,
                    const std::atomic<bool>& stop, std::vector<std::uint8_t>* out) {
    std::lock_guard<std::mutex> lock(mutex);
    return original(server, depot, sha, cap, stop, out);
  };
  DepotInstaller installer(hooks);
  std::atomic<bool> cancel{false};
  CHECK(installer.Run({}, Target(root), cancel).error == InstallError::kNone);
  CHECK(Read(root / "game.exe") == "MZ-executable-second-chunk");
  CHECK(Read(root / "data/level.dat") == "level-patched");
  CHECK(world.chunk_calls == 3 && std::filesystem::file_size(root / "empty.txt") == 0);
  const auto resumed = root / "resumed";
  std::filesystem::create_directories(resumed / DepotInstaller::kJournalDir);
  std::filesystem::create_directories(resumed / "data");
  std::ofstream(resumed / "game.exe") << "MZ-executable-second-chunk";
  // On a case-sensitive filesystem make the chosen mixed-case parent too.
  std::filesystem::create_directories(resumed / "DATA");
  std::ofstream(resumed / "DATA/level.dat") << "level-patched";
  std::ofstream journal(resumed / DepotInstaller::kJournalDir / "journal");
  journal << "441\t0\tgame.exe\n441\t14\tgame.exe\n442\t0\tDATA/level.dat\n";
  journal.close();
  int factories = 0;
  hooks.make_fetcher = [&] { ++factories; return InstallerHooks::ChunkFetcher{}; };
  DepotInstaller fully_resumed(hooks);
  CHECK(fully_resumed.Run({}, Target(resumed), cancel).error == InstallError::kNone);
  CHECK(factories == 0 && std::filesystem::file_size(resumed / "empty.txt") == 0);
  CHECK(Read(resumed / "game.exe") == "MZ-executable-second-chunk");
}

// A depot borrowed from another app the account doesn't own: its key is refused, and the game installs without it.
void BorrowedDepotRefusedIsSkipped(const std::filesystem::path& root) {
  auto world = MakeWorld();
  auto hooks = world.Hooks();
  bool asked_optional = false;
  hooks.access = [&](const StoredSession&, std::uint32_t, const std::vector<DepotRequest>& depots,
                     const std::atomic<bool>&) {
    DepotAccessResult result;
    result.error = CmError::kNone;
    for (const auto& depot : depots) {
      if (depot.optional) {
        asked_optional = true;
        result.skipped.push_back(depot.depot_id);
        continue;
      }
      result.grants.push_back({depot.depot_id, depot.manifest_gid, kKey, 77});
    }
    return result;
  };
  DepotInstaller installer(hooks);
  std::atomic<bool> cancel{false};
  auto target = Target(root);
  target.depots[1].depot_from_app = 877620;
  const auto outcome = installer.Run({}, target, cancel);
  CHECK(outcome.error == InstallError::kNone && asked_optional);
  CHECK(outcome.skipped_depots == std::vector<std::uint32_t>{442});
  CHECK(Read(root / "data/level.dat") == "level-one");
}

// An install whose only granted depot is a DLC's (the game's borrowed depot refused) fails instead of finishing
// without the game.
void InstallNeedsTheGamesContent(const std::filesystem::path& root) {
  auto world = MakeWorld();
  auto hooks = world.Hooks();
  hooks.access = [](const StoredSession&, std::uint32_t, const std::vector<DepotRequest>& depots, const std::atomic<bool>&) {
    DepotAccessResult result;
    result.error = CmError::kNone;
    for (const auto& depot : depots) {
      if (depot.optional) result.skipped.push_back(depot.depot_id);
      else result.grants.push_back({depot.depot_id, depot.manifest_gid, kKey, 77});
    }
    return result;
  };
  DepotInstaller installer(hooks);
  std::atomic<bool> cancel{false};
  auto target = Target(root);
  target.depots[0].depot_from_app = 877620;
  target.depots[1].owner_app = 999;
  CHECK(installer.Run({}, target, cancel).error == InstallError::kRejected);
  CHECK(!std::filesystem::exists(root / DepotInstaller::kInstallRecord));
  CHECK(!std::filesystem::exists(root / "game.exe") && !std::filesystem::exists(root / "data/level.dat"));
  CHECK(world.chunk_calls == 0);
}

// A DLC app's own depot is asked for under that app; the game's depots under the game.
void DlcAppDepotsAskUnderTheirApp(const std::filesystem::path& root) {
  auto world = MakeWorld();
  auto hooks = world.Hooks();
  const auto grant = hooks.access;
  std::vector<DepotRequest> asked;
  hooks.access = [&](const StoredSession& session, std::uint32_t app, const std::vector<DepotRequest>& depots,
                     const std::atomic<bool>& cancel) {
    asked = depots;
    return grant(session, app, depots, cancel);
  };
  DepotInstaller installer(hooks);
  std::atomic<bool> cancel{false};
  auto target = Target(root);
  target.depots[0].source_app = 440;
  target.depots[1].source_app = 5001;
  target.depots[1].owner_app = 5001;
  CHECK(installer.Run({}, target, cancel).error == InstallError::kNone);
  CHECK(asked.size() == 2 && asked[0].app_id == 440 && asked[1].app_id == 5001);
  // The public branch by default; the install's branch names every depot's manifest request.
  CHECK(asked[0].branch.empty() && asked[1].branch.empty());
}

void BranchRidesOnDepotRequests(const std::filesystem::path& root) {
  auto world = MakeWorld();
  auto hooks = world.Hooks();
  const auto grant = hooks.access;
  std::vector<DepotRequest> asked;
  hooks.access = [&](const StoredSession& session, std::uint32_t app, const std::vector<DepotRequest>& depots,
                     const std::atomic<bool>& cancel) {
    asked = depots;
    return grant(session, app, depots, cancel);
  };
  DepotInstaller installer(hooks);
  std::atomic<bool> cancel{false};
  auto target = Target(root);
  target.branch = "4.04";
  CHECK(installer.Run({}, target, cancel).error == InstallError::kNone);
  CHECK(asked.size() == 2 && asked[0].branch == "4.04" && asked[1].branch == "4.04");
}

std::string g_appinfo;

AppInfo AppFixture(std::uint32_t id, const char* name) {
  const auto text = Read(std::filesystem::path(g_appinfo) / name);
  const auto parsed = ParseAppInfo(id, text);
  CHECK(parsed.has_value());
  return *parsed;
}

// Free DLC: which DLC an install asks Steam for, and the DLC Steam refused, remembered per account.
// An update waits for the appinfo of a DLC app whose depots it has installed: without it, it would delete them.
void UpdateWaitsForUnfetchedDlc() {
  playden::installs::InstalledRecord record;
  record.depots = {{441, 11, 440, 440}, {443, 13, 500, 440}, {501, 51, 501, 501}, {601, 61, 0, 0}};
  CHECK(UpdateNeedsUnfetchedDlc(record, 440, {501}));
  CHECK(UpdateNeedsUnfetchedDlc(record, 440, {700, 501}));
  // A DLC's depot the game lists comes with the game's appinfo; an app with nothing installed doesn't matter.
  CHECK(!UpdateNeedsUnfetchedDlc(record, 440, {500, 700}));
  CHECK(!UpdateNeedsUnfetchedDlc(record, 440, {}));
  // A record from before sources (0) is the game's own listing.
  CHECK(!UpdateNeedsUnfetchedDlc(record, 440, {0}));
}

void FreeDlcCandidatesAndMemory(const std::filesystem::path& root) {
  const auto base = AppFixture(6000, "free-dlc-base-6000.vdf");
  const std::vector<AppInfo> dlc = {AppFixture(6001, "free-dlc-app-6001.vdf"), AppFixture(6002, "free-dlc-app-6002.vdf")};
  using U32s = std::vector<std::uint32_t>;
  // 6001 lists a Windows depot itself, 6003 has one the game lists; 6002 and 6005 are Mac only, 6004 has none.
  CHECK((FreeDlcCandidates(base, dlc, {6000}, {}) == U32s{6001, 6003}));
  // Without the DLC appinfo only the game's listing counts.
  CHECK((FreeDlcCandidates(base, {}, {6000}, {}) == U32s{6003}));
  // Owned or refused before: not asked for again.
  CHECK((FreeDlcCandidates(base, dlc, {6000, 6001}, {}) == U32s{6003}));
  CHECK((FreeDlcCandidates(base, dlc, {6000}, {6003}) == U32s{6001}));
  CHECK((FreeDlcCandidates(base, dlc, {6000}, {}, 1) == U32s{6001}));
  AppInfo many = base;
  many.dlc_app_ids.clear();
  for (std::uint32_t id = 7000; id < 7030; ++id) {
    many.dlc_app_ids.push_back(id);
    DepotInfo depot;
    depot.id = id + 100;
    depot.dlc_app_id = id;
    many.depots.push_back(depot);
  }
  CHECK(FreeDlcCandidates(many, {}, {6000}, {}).size() == kMaxFreeDlcClaim);

  // Once granted, the DLC app's depot is selected and asked for under the DLC app.
  SelectionResult selected;
  CHECK(SelectDepots(base, dlc, {6000, 6001, 6003}, std::set<std::uint32_t>{6000, 6011, 6031}, {}, &selected));
  CHECK(selected.depots.size() == 3 && selected.depots[0].id == 6000 && selected.depots[1].id == 6031 &&
        selected.depots[2].id == 6011);
  CHECK(selected.depots[1].source_app == 6000 && selected.depots[1].owner_app == 6003);
  CHECK(selected.depots[2].source_app == 6001 && selected.depots[2].owner_app == 6001);

  // Refused: answered without a grant and not owned after the request.
  FreeLicenseResult claim;
  claim.error = CmError::kNone;
  claim.eresult = cm::kEResultOk;
  claim.granted_apps = {6001};
  claim.granted_packages = {900};
  CHECK((FreeDlcRefusals({6001, 6003, 6007}, claim, {6000, 6007}) == U32s{6003}));
  claim.granted_apps.clear();
  CHECK(FreeDlcRefusals({6001, 6003}, claim, {6000}).empty());  // packages without apps: can't tell
  claim.granted_packages.clear();
  CHECK((FreeDlcRefusals({6001, 6003}, claim, {6000}) == U32s{6001, 6003}));
  claim.error = CmError::kTimeout;
  CHECK(FreeDlcRefusals({6001, 6003}, claim, {6000}).empty());  // transient: not remembered
  claim.error = CmError::kRejected;
  claim.eresult = cm::kEResultFail;
  CHECK(FreeDlcRefusals({6001, 6003}, claim, {6000}).empty());

  CHECK(FreeDlcMemoryName(76561197960269970ull) == "steam-free-dlc-4242.txt");
  CHECK((ParseFreeDlcMemory("6001\trefused\r\n6002\tgranted\nbad\trefused\n0\trefused\n6003\trefused") ==
         std::set<std::uint32_t>{6001, 6003}));
  CHECK(FormatFreeDlcMemory({6003, 6001}) == "6001\trefused\n6003\trefused\n");
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);
  Storage storage(root.string());
  CHECK(LoadFreeDlcMemory(storage, 76561197960269970ull).empty());
  CHECK(SaveFreeDlcMemory(storage, 76561197960269970ull, {6001, 6003}));
  CHECK((LoadFreeDlcMemory(storage, 76561197960269970ull) == std::set<std::uint32_t>{6001, 6003}));
  // Another account has its own memory.
  CHECK(LoadFreeDlcMemory(storage, 76561197960269971ull).empty());
  CHECK(Read(root / "steam-free-dlc-4242.txt") == "6001\trefused\n6003\trefused\n");
}

void TuningFileDefaultsAndValidation(const std::filesystem::path& root) {
  std::filesystem::create_directories(root);
  const auto path = (root / "tuning").string();
  auto tuning = ReadDownloadTuning(path);
  CHECK(tuning.slots == 4 && tuning.ahead_files == 256);
  std::ofstream(path) << "slots=8\nahead=12\n";
  tuning = ReadDownloadTuning(path);
  CHECK(tuning.slots == 8 && tuning.ahead_files == 12);
  std::ofstream(path) << "slots=6\nahead=32\nserver=fastly.cdn.steampipe.steamcontent.com\n";
  tuning = ReadDownloadTuning(path);
  CHECK(tuning.slots == 6 && tuning.ahead_files == 32 && tuning.preferred_server == "fastly.cdn.steampipe.steamcontent.com");
  const std::string longest(253, 'a');
  std::ofstream(path) << "server=" << longest << "\n";
  CHECK(ReadDownloadTuning(path).preferred_server == longest);
  std::ofstream(path) << "slots=99\n";
  CHECK(ReadDownloadTuning(path).slots == 8);
  std::ofstream(path) << "slots=0\n";
  CHECK(ReadDownloadTuning(path).slots == 1);
  for (const auto text : {"slots=8\nahead=-1\n", "slots=abc\n", "slots=8garbage\n", "unknown=1\n", "slots=8\nslots=1\n", "server=bad host\n",
                          "server=https://host\n", "server=host\nserver=other\n", "server=\n"}) {
    std::ofstream(path) << text;
    tuning = ReadDownloadTuning(path);
    CHECK(tuning.slots == 4 && tuning.ahead_files == 256 && tuning.preferred_server.empty());
  }
}

void PreferredServerFailover(const std::filesystem::path& base) {
  // A single slot makes the return schedule independent of thread scheduling.
  for (int mode = 0; mode < 6; ++mode) {
    auto world = MakeWorld();
    std::vector<std::string> pieces(600, "chunk");
    world.manifests[441].files = {world.AddFile("payload", pieces)};
    world.manifests[442].files.clear();
    auto hooks = world.Hooks();
    std::vector<std::string> manifests, requests;
    std::vector<int> preferred_at;
    int done = 0;
    hooks.manifest = [&](const ContentServer& server, const DepotGrant& grant, const std::atomic<bool>&, DepotManifest* out) {
      manifests.push_back(server.host); *out = world.manifests.at(grant.depot_id); return InstallError::kNone;
    };
    const auto fetch = hooks.chunk;
    std::atomic<bool> cancel{false};
    hooks.chunk = [&](const ContentServer& server, std::uint32_t depot, const std::string& sha,
                      std::size_t cap, const std::atomic<bool>& stop, std::vector<std::uint8_t>* out) {
      requests.push_back(server.host);
      const bool preferred = server.host == "b.example.invalid";
      if (preferred) preferred_at.push_back(done);
      if (mode == 5 && preferred) {
        cancel.store(true);
        while (!stop.load()) usleep(1000);
        return CdnError::kCanceled;
      }
      if ((mode == 1 && preferred && preferred_at.size() == 1) || (mode == 2 && preferred) ||
          ((mode == 3 || mode == 4) && requests.size() == 1)) return CdnError::kNetwork;
      const auto result = fetch(server, depot, sha, cap, stop, out);
      if (result == CdnError::kNone) ++done;
      return result;
    };
    std::string listed;
    hooks.server_list = [&](const std::vector<ContentServer>& servers, const char* preference) {
      listed = FormatInstallServerList(servers, preference);
    };
    InstallStats last;
    int ends = 0, server_ends = 0;
    hooks.stats = [&](const char* phase, const InstallStats& stats) {
      if (std::string(phase) == "end") { last = stats; ++ends; }
      if (std::string(phase) == "servers") ++server_ends;
    };
    DepotInstaller installer(hooks);
    DownloadTuning tuning{1, 256};
    if (mode != 3 && mode != 4) tuning.preferred_server = "b.example.invalid";
    if (mode == 4) tuning.preferred_server = "missing.example.invalid";
    installer.SetTuning(tuning);
    const auto outcome = installer.Run({}, Target(base / std::to_string(mode)), cancel);
    CHECK(ends == 1 && server_ends == 1);
    CHECK(listed.find(mode == 4 ? "pref=missing" : mode == 3 ? "pref=auto" : "pref=chosen") != std::string::npos);
    CHECK(manifests.front() == (mode == 3 || mode == 4 ? "a.example.invalid" : "b.example.invalid"));
    CHECK(requests.front() == manifests.front());
    CHECK(outcome.error == (mode == 5 ? InstallError::kCanceled : InstallError::kNone));
    CHECK(last.server_stats.size() == 2);
    std::uint64_t counted = 0, failures = 0;
    for (const auto& server : last.server_stats) { counted += server.requests; failures += server.failures; }
    CHECK(counted == requests.size() + manifests.size());
    if (mode == 0) CHECK(preferred_at.size() == 600 && failures == 0);
    if (mode == 1) {
      CHECK(preferred_at.size() == 569 && preferred_at[0] == 0 && preferred_at[1] == 32);
      CHECK(requests[1] == "a.example.invalid" && failures == 1);
    }
    if (mode == 2) {
      CHECK(preferred_at == std::vector<int>({0, 32, 96, 224, 480}));
      CHECK(failures == 5 && last.server_stats[0].requests == 7);
    }
    if (mode == 3 || mode == 4) {
      CHECK(requests.size() == 601 && failures == 1);
      CHECK(requests[0] == "a.example.invalid");
      for (std::size_t i = 1; i < requests.size(); ++i) CHECK(requests[i] == "b.example.invalid");
    }
    if (mode == 5) CHECK(failures == 1);
  }
}

void ServerLogsAreBoundedAndPrivate() {
  std::vector<ContentServer> servers{{"fastly.cdn.steampipe.steamcontent.com", "private-vhost", 0, true, 2},
                                     {"steampipe.akamaized.net", "private-vhost", 0, true, 1},
                                     {"isp.private.example", "private-vhost", 0, false, 9}};
  CHECK(FormatInstallServerList(servers, "chosen") ==
        "n=3 https=2 pref=chosen first=fastly@0 order=fastly:2,akamai:1,cache:9");
  std::vector<InstallServerStats> stats{{"fastly", 0, 812, 0, 790000000, 301200}, {"akamai", 1, 3, 0, 2000000, 900}};
  CHECK(FormatInstallServerStats(stats) == std::vector<std::string>{
        "fastly@0:req=812,fail=0,mb=790,ms=301200 akamai@1:req=3,fail=0,mb=2,ms=900"});
  for (int i = 3; i < 10; ++i) servers.push_back({"secret" + std::to_string(i) + ".example", "private-vhost", 0, true, 99});
  const auto listed = FormatInstallServerList(servers, "auto");
  CHECK(listed.find("secret") == std::string::npos && listed.find("private") == std::string::npos);
  CHECK(listed.size() < 380);
  CHECK(std::count(listed.begin(), listed.end(), ',') == 7);
  stats.clear();
  for (std::size_t i = 0; i < 9; ++i)
    stats.push_back({i == 8 ? "other" : "edgenext", i, UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX});
  const auto lines = FormatInstallServerStats(stats);
  CHECK(lines.size() > 1);
  for (const auto& line : lines) CHECK(line.size() < 380 && line.find("example") == std::string::npos);
}

void CorruptRepliesCountAsServerFailures(const std::filesystem::path& root) {
  auto world = MakeWorld();
  world.corrupt = true;
  auto hooks = world.Hooks();
  InstallStats last;
  hooks.stats = [&](const char* phase, const InstallStats& stats) { if (std::string(phase) == "end") last = stats; };
  DepotInstaller installer(hooks);
  installer.SetTuning({1, 256});
  std::atomic<bool> cancel{false};
  CHECK(installer.Run({}, Target(root), cancel).error == InstallError::kCorrupt);
  CHECK(last.server_stats.size() == 2);
  CHECK(last.server_stats[0].requests == 5 && last.server_stats[0].failures == 3);
  CHECK(last.server_stats[1].requests == 3 && last.server_stats[1].failures == 3);
}

void ExtraServersShareOneBucket(const std::filesystem::path& root) {
  auto world = MakeWorld();
  auto hooks = world.Hooks();
  hooks.servers = [](std::uint32_t, const std::atomic<bool>&, std::vector<ContentServer>* out) {
    for (int i = 0; i < 12; ++i) out->push_back({"host" + std::to_string(i) + ".example", "vhost"});
    return CdnError::kNone;
  };
  const auto fetch = hooks.chunk;
  int requests = 0;
  hooks.chunk = [&](const ContentServer& server, std::uint32_t depot, const std::string& sha, std::size_t cap,
                    const std::atomic<bool>& stop, std::vector<std::uint8_t>* out) {
    ++requests;
    usleep(600);
    // First chunk reaches index 5; the next reaches index 9, then stays there.
    if (requests != 6 && requests < 11) return CdnError::kNetwork;
    return fetch(server, depot, sha, cap, stop, out);
  };
  InstallStats last;
  hooks.stats = [&](const char* phase, const InstallStats& stats) { if (std::string(phase) == "end") last = stats; };
  DepotInstaller installer(hooks);
  std::atomic<bool> cancel{false};
  installer.SetTuning({1, 256});
  CHECK(installer.Run({}, Target(root), cancel).error == InstallError::kNone);
  CHECK(last.server_stats.size() == 9 && last.server_stats[8].label == "other" && last.server_stats[8].index == 8);
  CHECK(last.server_stats[8].requests == 3 && last.server_stats[8].failures == 1);
  CHECK(last.server_stats[8].bytes > 0 && last.server_stats[8].fetch_ms >= 1);
}

void EarlyFailuresLogServerTotals(const std::filesystem::path& root) {
  auto world = MakeWorld();
  for (int mode = 0; mode < 3; ++mode) {
    auto hooks = world.Hooks();
    std::atomic<bool> cancel{mode == 0};
    if (mode == 1) hooks.servers = [](std::uint32_t, const std::atomic<bool>&, std::vector<ContentServer>*) {
      return CdnError::kNetwork;
    };
    if (mode == 2) hooks.manifest = [](const ContentServer&, const DepotGrant&, const std::atomic<bool>&, DepotManifest*) {
      return InstallError::kNetwork;
    };
    int ends = 0, servers = 0;
    hooks.stats = [&](const char* phase, const InstallStats&) {
      if (std::string(phase) == "end") ++ends;
      if (std::string(phase) == "servers") ++servers;
    };
    DepotInstaller installer(hooks);
    CHECK(installer.Run({}, Target(root), cancel).error != InstallError::kNone);
    CHECK(ends == 1 && servers == 1);
  }
}

void CountsActualUsefulCompressedBytes(const std::filesystem::path& root) {
  auto world = MakeWorld();
  // HTTP permits the advertised size plus padding; a verified body can differ from metadata.
  for (auto& [id, manifest] : world.manifests)
    for (auto& file : manifest.files)
      for (auto& chunk : file.chunks) chunk.compressed_size += 16;
  auto hooks = world.Hooks();
  const auto fetch = hooks.chunk;
  std::uint64_t received = 0;
  hooks.chunk = [&](const ContentServer& server, std::uint32_t depot, const std::string& sha,
                    std::size_t cap, const std::atomic<bool>& cancel, std::vector<std::uint8_t>* out) {
    const auto result = fetch(server, depot, sha, cap, cancel, out);
    if (result == CdnError::kNone) received += out->size();
    return result;
  };
  InstallStats last;
  hooks.stats = [&](const char* phase, const InstallStats& stats) { if (std::string(phase) == "end") last = stats; };
  DepotInstaller installer(hooks);
  installer.SetTuning({1, 256});
  std::atomic<bool> cancel{false};
  const auto outcome = installer.Run({}, Target(root), cancel);
  CHECK(outcome.error == InstallError::kNone);
  CHECK(last.compressed_bytes == received);
  CHECK(last.compressed_bytes + 3 * 16 == outcome.progress.download_total);
}

void StorageFailureEndsStats(const std::filesystem::path& root) {
  auto world = MakeWorld();
  std::filesystem::create_directories(root);
  std::ofstream(root / "DATA") << "not a directory";
  auto hooks = world.Hooks();
  int ends = 0;
  hooks.stats = [&](const char* phase, const InstallStats&) { if (std::string(phase) == "end") ++ends; };
  DepotInstaller installer(hooks);
  installer.SetTuning({1, 256});
  std::atomic<bool> cancel{false};
  CHECK(installer.Run({}, Target(root), cancel).error == InstallError::kStorage);
  CHECK(ends == 1);
}

void StatsAndRemovalLogs(const std::filesystem::path& root) {
  InstallStats stats;
  stats.download_ms = 300000; stats.compressed_bytes = 6000000000ull;
  stats.files_total = 48000; stats.files_created = 48000;
  const auto line = FormatInstallStats("end", stats);
  CHECK(line.size() < 480);
  CHECK(line.find("dl_ms=300000") != std::string::npos && line.find("reuse=1") != std::string::npos);
  CmClient cm(CmClientHooks{}, cm::DeviceIdentity{});
  CdnClient cdn([](const HttpRequest&, const std::atomic<bool>&) { return HttpResponse{}; });
  struct NoSessions final : SessionRepository {
    Result<std::optional<StoredSession>> Load() const override { return Result<std::optional<StoredSession>>::Success(std::nullopt); }
    Result<void> Save(const StoredSession&) const override { return Result<void>::Success(); }
    Result<void> Clear() const override { return Result<void>::Success(); }
  } sessions;
  SteamInstallRunner runner(sessions, cm, cdn);
  int removals = 0;
  std::uint32_t expected_app = 440;
  const char* expected_ok = "ok=1";
  runner.SetLog([&](std::uint32_t app, const char* step, const char* result, const char* detail) {
    CHECK(app == expected_app && std::string(step) == "install-removal" && std::string(result) == "done");
    CHECK(std::string(detail) == expected_ok);
    ++removals;
  });
  CHECK(runner.Remove((root / "440").string()).empty());
  CHECK(removals == 1);
  expected_app = 441; expected_ok = "ok=0 The game's folder holds no Playden install record, so it was left as it is.";
  std::filesystem::create_directories(root / "441");
  std::ofstream(root / "441" / "keep.txt") << "unmanaged";
  CHECK(runner.Remove((root / "441").string()).find("no Playden install record") != std::string::npos);
  CHECK(removals == 2);
  CHECK(!runner.Remove((root / "account-name").string() + "/..").empty());
  CHECK(removals == 2); // no unvalidated path/name is logged
}

void RunStats(const std::filesystem::path& base) {
  for (int mode = 0; mode < 3; ++mode) {
    auto world = MakeWorld();
    world.manifests[441].files.pop_back(); // two files and three merged chunks
    world.manifests[442].files[0].path = "data/level.dat"; // stable folder count on either filesystem
    std::atomic<bool> cancel{false};
    world.cancel = &cancel;
    if (mode == 1) world.cancel_after = 1;
    if (mode == 2) world.corrupt = true;
    auto hooks = world.Hooks();
    InstallStats last;
    int ends = 0, shapes = 0;
    hooks.stats = [&](const char* phase, const InstallStats& stats) {
      if (std::string(phase) == "end") { ++ends; last = stats; }
      if (std::string(phase) == "shape") {
        ++shapes;
        if (ends == 0) CHECK(stats.chunks_pending == 3 && stats.files_pending == 2 && stats.files_total == 2);
      }
    };
    CdnStats totals;
    totals.requests = 100;
    hooks.cdn_stats = [&] { return totals; };
    auto fetch = hooks.chunk;
    hooks.chunk = [&](const ContentServer& server, std::uint32_t depot, const std::string& sha,
                      std::size_t cap, const std::atomic<bool>& canceled, std::vector<std::uint8_t>* out) {
      ++totals.requests;
      if (mode == 0) usleep(600);
      return fetch(server, depot, sha, cap, canceled, out);
    };
    DepotInstaller installer(hooks);
  installer.SetTuning({1, 256});
    auto target = Target(base / std::to_string(mode));
    const auto outcome = installer.Run({}, target, cancel);
    CHECK(ends == 1 && shapes == 1 && last.requests == static_cast<std::uint64_t>(world.chunk_calls));
    CHECK(last.slots == 1 && last.reuse == 1);
    if (mode == 0) {
      CHECK(outcome.error == InstallError::kNone && last.chunks_done == 3 && last.files_created == 2);
      CHECK(last.fetch_ms >= 1); // sub-millisecond chunks accumulate before conversion
      CHECK(last.original_bytes == 39 && last.folders_created == 1);
      CHECK(last.compressed_bytes == outcome.progress.download_total);
    } else if (mode == 1) {
      CHECK(outcome.error == InstallError::kCanceled && last.chunks_done == 1);
      CHECK(last.original_bytes > 0 && last.compressed_bytes > 0);
      cancel = false; world.cancel_after = -1;
      installer.Run({}, target, cancel);
      CHECK(ends == 2 && last.chunks_skipped == 1 && last.chunks_done == 2);
      CHECK(last.files_created <= 1 && last.requests == static_cast<std::uint64_t>(world.chunk_calls - 2));
    } else {
      CHECK(outcome.error == InstallError::kCorrupt && last.chunks_done == 0);
      CHECK(last.compressed_bytes == 0 && last.original_bytes == 0 && last.corrupt_retries == 5);
    }
  }
}

void InstallsMergedDepotsAndWritesRecord(const std::filesystem::path& root) {
  auto world = MakeWorld();
  for (std::uint8_t index = 0; index < 20; ++index) world.manifests[441].files[0].sha_content[index] = static_cast<std::uint8_t>(index * 13);
  DepotInstaller installer(world.Hooks());
  installer.SetTuning({1, 256});
  std::atomic<bool> cancel{false};
  InstallProgress last;
  auto target = Target(root);
  target.language = "german";
  target.depots[1].owner_app = 450;  // a DLC's depot, listed in the DLC's own appinfo
  target.depots[1].source_app = 450;
  const auto outcome = installer.Run({}, target, cancel, [&](const InstallProgress& progress) { last = progress; });
  CHECK(outcome.error == InstallError::kNone);
  CHECK(Read(root / "game.exe") == "MZ-executable-second-chunk");
  CHECK(Read(root / "data/level.dat") == "level-patched");
  CHECK(std::filesystem::exists(root / "empty.txt") && std::filesystem::file_size(root / "empty.txt") == 0);
  CHECK(std::filesystem::exists(root / DepotInstaller::kInstallRecord));
  CHECK(!std::filesystem::exists(root / DepotInstaller::kJournalDir));
  CHECK(Read(root / DepotInstaller::kFileList).find("f\tdata/level.dat") != std::string::npos ||
        Read(root / DepotInstaller::kFileList).find("f\tDATA/level.dat") != std::string::npos);
  CHECK(last.download_done == last.download_total && last.files_total == 3);
  CHECK(last.held_bytes == last.install_total);  // every written byte is on the drive
  CHECK(world.chunk_calls == 3);  // the shadowed base level.dat is never fetched
  // A Steam record names no store or game; version 2 adds the branch, the language and each depot's owner and
  // source.
  const auto text = Read(root / DepotInstaller::kInstallRecord);
  CHECK(text.rfind("{\"version\":2,\"app_id\":440,\"install_bytes\":", 0) == 0);
  CHECK(text.find(",\"branch\":\"public\",\"language\":\"german\",\"depots\":["
                  "{\"id\":441,\"manifest\":\"11\",\"owner\":440,\"source\":440},"
                  "{\"id\":442,\"manifest\":\"12\",\"owner\":450,\"source\":450}]}\n") != std::string::npos);
  const auto record = playden::installs::ReadInstallRecord((root / DepotInstaller::kInstallRecord).string());
  CHECK(record && record->version == 2 && record->app_id == 440 && record->install_bytes == last.install_total);
  CHECK(record->branch == "public" && record->language == "german");
  CHECK((record->depots == std::vector<playden::installs::RecordedDepot>{{441, 11, 440, 440}, {442, 12, 450, 450}}));
  // The merged tree: each file the merge kept (the patch's level.dat, in its own case), no folders, by path.
  CHECK(Read(root / DepotInstaller::kFileTree) ==
        "v1\n"
        "8eaf8755d1f863e507024462baa384b6be8b9878\t13\tDATA/level.dat\n"
        "da39a3ee5e6b4b0d3255bfef95601890afd80709\t0\tempty.txt\n"
        "000d1a2734414e5b6875828f9ca9b6c3d0ddeaf7\t26\tgame.exe\n");
  CHECK(!std::filesystem::exists(root / (std::string(DepotInstaller::kFileTree) + ".tmp")));
}

// Measure fetches the manifests an install would and merges them as it does, without writing anything: depots
// holding the same files count them once (Portal's 401 and 409 add up to twice the game).
void MeasureMatchesTheMergedInstall(const std::filesystem::path& root) {
  auto world = MakeWorld();
  DepotInstaller installer(world.Hooks());
  std::atomic<bool> cancel{false};
  const auto target = Target(root);
  const auto measured = installer.Measure({}, target.app_id, target.depots, {}, cancel);
  CHECK(measured.error == InstallError::kNone);
  CHECK(!std::filesystem::exists(root) && world.chunk_calls == 0);
  std::uint64_t summed = 0;
  for (const auto& [id, manifest] : world.manifests)
    for (const auto& file : manifest.files) summed += file.size;
  CHECK(measured.install_bytes == 26 + 13 && summed == 26 + 9 + 13);  // the base's level.dat is replaced
  InstallProgress last;
  CHECK(installer.Run({}, target, cancel, [&](const InstallProgress& progress) { last = progress; }).error == InstallError::kNone);
  CHECK(measured.install_bytes == last.install_total && measured.download_bytes == last.download_total);
  // A depot the account is refused is left out, as an install leaves it; none granted is an error.
  auto hooks = world.Hooks();
  hooks.access = [](const StoredSession&, std::uint32_t, const std::vector<DepotRequest>& depots, const std::atomic<bool>&) {
    DepotAccessResult result;
    result.error = CmError::kNone;
    result.grants.push_back({depots[0].depot_id, depots[0].manifest_gid, kKey, 77});
    result.skipped.push_back(depots[1].depot_id);
    return result;
  };
  DepotInstaller refused(hooks);
  const auto base_only = refused.Measure({}, target.app_id, target.depots, {}, cancel);
  CHECK(base_only.error == InstallError::kNone && base_only.install_bytes == 26 + 9 && base_only.partial);
  CHECK(!measured.partial);
  hooks.access = [](const StoredSession&, std::uint32_t, const std::vector<DepotRequest>&, const std::atomic<bool>&) {
    DepotAccessResult result;
    result.error = CmError::kTimeout;
    return result;
  };
  DepotInstaller offline(hooks);
  CHECK(offline.Measure({}, target.app_id, target.depots, {}, cancel).error == InstallError::kNetwork);
  // A manifest that a server failed to send, then came back broken, is a network problem (a later try may work).
  hooks = world.Hooks();
  int manifest_calls = 0;
  hooks.manifest = [&manifest_calls](const ContentServer&, const DepotGrant&, const std::atomic<bool>&, DepotManifest*) {
    return ++manifest_calls == 1 ? InstallError::kNetwork : InstallError::kCorrupt;
  };
  DepotInstaller flaky(hooks);
  CHECK(flaky.Measure({}, target.app_id, target.depots, {}, cancel).error == InstallError::kNetwork);
  // A link over a folder can't be merged, for a measure as for an install.
  ManifestFile link;
  link.path = "data";
  link.link_target = "game.exe";
  world.manifests[442].files.push_back(link);
  DepotInstaller clash(world.Hooks());
  CHECK(clash.Measure({}, target.app_id, target.depots, {}, cancel).error == InstallError::kCorrupt);
  const auto merged = MergeDepotManifests({world.manifests[441], world.manifests[442]});
  CHECK(merged.conflict == "data");
}

// A file and an empty folder of the same name in different depots keep the file, in either order; a folder
// with something inside it still conflicts.
void FileReplacesEmptyFolderFromOtherDepot(const std::filesystem::path& base) {
  ManifestFile folder;
  folder.path = "master.dat";
  folder.flags = kDepotFileFlagDirectory;
  for (const bool folder_first : {true, false}) {
    const auto root = base / (folder_first ? "folder-first" : "file-first");
    auto world = MakeWorld();
    const auto file = world.AddFile("MASTER.DAT", {"game-data"});
    world.manifests[441].files.push_back(folder_first ? folder : file);
    world.manifests[442].files.push_back(folder_first ? file : folder);
    std::vector<std::string> merges;
    auto hooks = world.Hooks();
    hooks.merge = [&](const char* result, const std::string& detail) { merges.push_back(std::string(result) + " " + detail); };
    DepotInstaller installer(std::move(hooks));
    std::atomic<bool> cancel{false};
    const auto outcome = installer.Run({}, Target(root), cancel);
    CHECK(outcome.error == InstallError::kNone);
    CHECK(std::filesystem::is_regular_file(root / "MASTER.DAT") && Read(root / "MASTER.DAT") == "game-data");
    const std::string folder_entry = "depot=" + std::string(folder_first ? "441" : "442") + " folder path=master.dat";
    const std::string file_entry = "depot=" + std::string(folder_first ? "442" : "441") + " file size=9 path=MASTER.DAT";
    CHECK(merges.size() == 1 && merges[0] == "file-over-folder " + (folder_first ? folder_entry + " | " + file_entry
                                                                                   : file_entry + " | " + folder_entry));
  }
  auto world = MakeWorld();
  world.manifests[441].files.push_back(folder);
  world.manifests[441].files.push_back(world.AddFile("master.dat/inner.txt", {"inner"}));
  world.manifests[442].files.push_back(world.AddFile("master.dat", {"game-data"}));
  std::vector<std::string> merges;
  auto hooks = world.Hooks();
  hooks.merge = [&](const char* result, const std::string& detail) { merges.push_back(std::string(result) + " " + detail); };
  DepotInstaller installer(std::move(hooks));
  std::atomic<bool> cancel{false};
  const auto outcome = installer.Run({}, Target(base / "folder-with-files"), cancel);
  CHECK(outcome.error == InstallError::kCorrupt && outcome.detail == "conflicting depots master.dat");
  CHECK(merges.size() == 2 && merges[1] == "folder-not-empty depot=442 file size=9 path=master.dat | "
                                           "depot=441 file size=5 path=master.dat/inner.txt");
  CHECK(world.chunk_calls == 0);

  // Many clashes log the first lines and then a count.
  auto many = MakeWorld();
  for (int index = 0; index < 20; ++index) {
    ManifestFile empty_folder;
    empty_folder.path = "folder" + std::to_string(index);
    empty_folder.flags = kDepotFileFlagDirectory;
    many.manifests[441].files.push_back(empty_folder);
    many.manifests[442].files.push_back(many.AddFile(empty_folder.path, {"x"}));
  }
  std::vector<std::string> many_merges;
  auto many_hooks = many.Hooks();
  many_hooks.merge = [&](const char* result, const std::string& detail) { many_merges.push_back(std::string(result) + " " + detail); };
  DepotInstaller many_installer(std::move(many_hooks));
  CHECK(many_installer.Run({}, Target(base / "many-clashes"), cancel).error == InstallError::kNone);
  CHECK(many_merges.size() == DepotInstaller::kMergeLogLimit + 1 && many_merges.back() == "more lines=20 logged=16");

  // A link does not take a folder's place.
  auto linked = MakeWorld();
  linked.manifests[441].files.push_back(folder);
  ManifestFile link;
  link.path = "master.dat";
  link.link_target = "game.exe";
  linked.manifests[442].files.push_back(link);
  DepotInstaller link_installer(linked.Hooks());
  const auto link_outcome = link_installer.Run({}, Target(base / "link-over-folder"), cancel);
  CHECK(link_outcome.error == InstallError::kCorrupt && link_outcome.detail == "conflicting depots master.dat");

  // An empty folder an earlier install left on disk makes way for the file; one with files in it does not.
  for (const bool occupied : {false, true}) {
    const auto root = base / (occupied ? "leftover-occupied" : "leftover-empty");
    std::filesystem::create_directories(root / "master.dat");
    if (occupied) std::ofstream(root / "master.dat" / "save.txt") << "kept";
    auto leftover = MakeWorld();
    leftover.manifests[441].files.push_back(folder);
    leftover.manifests[442].files.push_back(leftover.AddFile("master.dat", {"game-data"}));
    std::vector<std::string> leftover_merges;
    auto leftover_hooks = leftover.Hooks();
    leftover_hooks.merge = [&](const char* result, const std::string& detail) {
      leftover_merges.push_back(std::string(result) + " " + detail);
    };
    DepotInstaller leftover_installer(std::move(leftover_hooks));
    const auto leftover_outcome = leftover_installer.Run({}, Target(root), cancel);
    CHECK(leftover_merges.size() == 2 && leftover_merges[1].rfind(occupied ? "disk-folder-kept path=master.dat errno="
                                                                           : "disk-folder-removed path=master.dat", 0) == 0);
    if (occupied) {
      CHECK(leftover_outcome.error == InstallError::kStorage && leftover_outcome.detail == "cannot replace folder master.dat");
      CHECK(Read(root / "master.dat" / "save.txt") == "kept");
    } else {
      CHECK(leftover_outcome.error == InstallError::kNone && Read(root / "master.dat") == "game-data");
    }
  }
}

// Another store's game: app_id holds its runtime number and the record names the game.
void LocalRecordNamesItsGame(const std::filesystem::path& root) {
  auto world = MakeWorld();
  DepotInstaller installer(world.Hooks());
  installer.SetTuning({1, 256});
  std::atomic<bool> cancel{false};
  auto target = Target(root);
  target.app_id = 0xF0000000u;
  target.game = playden::GameId::Local("0f8fad5b-d9cb-469f-a165-70867728950e");
  CHECK(installer.Run({}, target, cancel).error == InstallError::kNone);
  CHECK(Read(root / DepotInstaller::kInstallRecord).rfind(
            "{\"version\":2,\"app_id\":4026531840,\"store\":\"local\",\"game\":\"0f8fad5b-d9cb-469f-a165-70867728950e\","
            "\"install_bytes\":", 0) == 0);
}

void ResumesWithoutRefetchingFinishedChunks(const std::filesystem::path& root) {
  auto world = MakeWorld();
  std::atomic<bool> cancel{false};
  world.cancel = &cancel;
  world.cancel_after = 2;
  DepotInstaller installer(world.Hooks());
  installer.SetTuning({1, 256});
  const auto first = installer.Run({}, Target(root), cancel);
  CHECK(first.error == InstallError::kCanceled);
  CHECK(std::filesystem::exists(root / DepotInstaller::kJournalDir));
  CHECK(!std::filesystem::exists(root / DepotInstaller::kInstallRecord));
  cancel = false;
  world.cancel_after = -1;
  world.chunk_calls = 0;
  const auto second = installer.Run({}, Target(root), cancel);
  CHECK(second.error == InstallError::kNone);
  CHECK(world.chunk_calls == 1);  // only the chunk interrupted by the cancel
  CHECK(Read(root / "game.exe") == "MZ-executable-second-chunk");
}

void ChangedManifestRefetchesSameOffset(const std::filesystem::path& root) {
  auto world = MakeWorld();
  std::atomic<bool> cancel{false};
  world.cancel = &cancel; world.cancel_after = 1;
  auto hooks = world.Hooks();
  std::vector<std::string> traces;
  hooks.trace = [&](const char* step) { traces.emplace_back(step); };
  DepotInstaller installer(hooks);
  installer.SetTuning({1, 256});
  CHECK(installer.Run({}, Target(root), cancel).error == InstallError::kCanceled);
  CHECK(Read(root / "DATA/level.dat") == "level-patched");
  world.manifests[442].gid = 13;
  world.manifests[442].files[0] = world.AddFile("DATA/level.dat", {"updated-level"});
  auto target = Target(root);
  target.depots[1].manifest_gid = 13;
  cancel.store(false); world.cancel_after = -1; world.chunk_calls = 0;
  CHECK(installer.Run({}, target, cancel).error == InstallError::kNone);
  CHECK(Read(root / "DATA/level.dat") == "updated-level");
  CHECK(world.chunk_calls == 3);
  CHECK(std::count(traces.begin(), traces.end(), "journal-reset") == 1);
}

void JournalHeaderModes(const std::filesystem::path& base) {
  const std::string header = "#v2 441:11,442:12\n";
  const std::string entry = "442\t0\tDATA/level.dat\n";
  const std::vector<std::string> journals = {
    header + "#comment ignored\n#verified chunks\n" + entry, entry, "", "#v2 441:11,442:99\n" + entry,
    "#v2 broken\n" + entry, header + header + entry, "#v2 442:12,441:11\n" + entry,
    "\n" + header + entry, "#v1 441:11,442:12\n" + entry, "#V2 441:11,442:12\n" + entry,
    "#comment\n" + entry, header + entry.substr(0, entry.size() - 1),
  };
  for (std::size_t mode = 0; mode < journals.size(); ++mode) {
    const auto root = base / std::to_string(mode);
    auto world = MakeWorld();
    std::atomic<bool> cancel{false}; world.cancel = &cancel; world.cancel_after = 1;
    auto hooks = world.Hooks();
    std::vector<std::string> traces;
    hooks.trace = [&](const char* step) { traces.emplace_back(step); };
    InstallStats last;
    hooks.stats = [&](const char* phase, const InstallStats& stats) { if (std::string(phase) == "end") last = stats; };
    DepotInstaller installer(hooks); installer.SetTuning({1, 256});
    CHECK(installer.Run({}, Target(root), cancel).error == InstallError::kCanceled);
    CHECK(Read(root / DepotInstaller::kJournalDir / "journal") == header + entry);
    CHECK(std::count(traces.begin(), traces.end(), "journal-reset") == 0);
    CHECK(std::count(traces.begin(), traces.end(), "journal-legacy") == 0);
    std::ofstream(root / DepotInstaller::kJournalDir / "journal") << journals[mode];
    cancel.store(false); world.cancel_after = -1; world.chunk_calls = 0; traces.clear();
    CHECK(installer.Run({}, Target(root), cancel).error == InstallError::kNone);
    const bool legacy = mode == 1 || mode == 10;
    const bool reuse = mode == 0 || mode == 11 || legacy;
    CHECK(world.chunk_calls == (reuse ? 2 : 3));
    CHECK(last.chunks_skipped == (reuse ? 1u : 0u));
    CHECK(std::count(traces.begin(), traces.end(), "journal-reset") == (mode >= 3 && mode <= 9 ? 1 : 0));
    CHECK(std::count(traces.begin(), traces.end(), "journal-legacy") == (legacy ? 1 : 0));
    CHECK(Read(root / "game.exe") == "MZ-executable-second-chunk");
    CHECK(Read(root / "DATA/level.dat") == "level-patched");
  }
}

void UnterminatedJournalGetsSeparator(const std::filesystem::path& root) {
  auto world = MakeWorld();
  std::filesystem::create_directories(root / DepotInstaller::kJournalDir);
  std::filesystem::create_directories(root / "DATA");
  std::ofstream(root / "DATA/level.dat") << "level-patched";
  std::ofstream(root / DepotInstaller::kJournalDir / "journal") << "#v2 441:11,442:12\n442\t0\tDATA/level.dat";
  std::atomic<bool> cancel{false}; world.cancel = &cancel; world.cancel_after = 1;
  DepotInstaller installer(world.Hooks()); installer.SetTuning({1, 256});
  CHECK(installer.Run({}, Target(root), cancel).error == InstallError::kCanceled);
  CHECK(Read(root / DepotInstaller::kJournalDir / "journal") ==
        "#v2 441:11,442:12\n442\t0\tDATA/level.dat\n441\t0\tgame.exe\n");
  cancel.store(false); world.chunk_calls = 0; world.cancel_after = -1;
  CHECK(installer.Run({}, Target(root), cancel).error == InstallError::kNone);
  CHECK(world.chunk_calls == 1);
}

void LongMatchingJournalHeader(const std::filesystem::path& root) {
  auto world = MakeWorld();
  world.manifests[441].files = {world.AddFile("one", {"written"})};
  auto target = Target(root);
  const auto depot = target.depots.front();
  target.depots.assign(1500, depot);
  std::string header = "#v2 ";
  for (int index = 0; index < 1500; ++index) header += (index ? ",441:11" : "441:11");
  CHECK(header.size() > 8192);
  std::filesystem::create_directories(root / DepotInstaller::kJournalDir);
  std::ofstream(root / "one") << "written";
  std::ofstream(root / DepotInstaller::kJournalDir / "journal") << header << "\n441\t0\tone\n";
  auto hooks = world.Hooks();
  InstallStats last;
  hooks.stats = [&](const char* phase, const InstallStats& stats) { if (std::string(phase) == "end") last = stats; };
  DepotInstaller installer(hooks);
  std::atomic<bool> cancel{false};
  const auto result = installer.Run({}, target, cancel);
  CHECK(result.error == InstallError::kNone);
  CHECK(world.chunk_calls == 0 && last.chunks_skipped == 1);
  CHECK(Read(root / "one") == "written");
}

void HeaderWriteFailureCannotAppend(const std::filesystem::path& root) {
  auto world = MakeWorld();
  const auto folder = root / DepotInstaller::kJournalDir;
  std::filesystem::create_directories(folder / "journal.tmp");
  const std::string stale = "#v2 441:999,442:12\n442\t0\tDATA/level.dat\n";
  std::ofstream(folder / "journal") << stale;
  DepotInstaller installer(world.Hooks());
  std::atomic<bool> cancel{false};
  const auto outcome = installer.Run({}, Target(root), cancel);
  CHECK(outcome.error == InstallError::kStorage && outcome.detail == "cannot write journal");
  CHECK(world.chunk_calls == 0 && Read(folder / "journal") == stale);
}

void JournalTraceLogging() {
  std::vector<std::string> lines;
  const InstallLogFn log = [&](std::uint32_t app, const char* step, const char* category, const char* detail) {
    lines.push_back(std::to_string(app) + " step=" + step + " result=" + category + " detail=" + detail);
  };
  LogInstallTrace(log, 440, "journal-reset");
  LogInstallTrace(log, 440, "journal-legacy");
  LogInstallTrace(log, 440, "write");
  CHECK(lines == std::vector<std::string>({"440 step=journal result=reset detail=",
                                         "440 step=journal result=legacy detail=",
                                         "440 step=trace result=write detail="}));
  LogInstallTrace({}, 440, "journal-reset");
}

void RefusesWhenSpaceIsShort(const std::filesystem::path& root) {
  auto world = MakeWorld();
  world.free_bytes = 1024;
  DepotInstaller installer(world.Hooks());
  installer.SetTuning({1, 256});
  std::atomic<bool> cancel{false};
  CHECK(installer.Run({}, Target(root), cancel).error == InstallError::kNoSpace);
  CHECK(world.chunk_calls == 0);
}

void PreservesSpaceMargin(const std::filesystem::path& root) {
  // The merged fixture downloads 14 + 12 executable bytes and 13 level bytes.
  for (const auto bytes : {64ull * 1024 * 1024 - 1, 64ull * 1024 * 1024,
                           64ull * 1024 * 1024 + 38, 64ull * 1024 * 1024 + 39}) {
    auto world = MakeWorld();
    world.free_bytes = bytes;
    DepotInstaller installer(world.Hooks());
    installer.SetTuning({1, 256});
    std::atomic<bool> cancel{false};
    const auto outcome = installer.Run({}, Target(root / std::to_string(bytes)), cancel);
    if (bytes < 64ull * 1024 * 1024 + 39) {
      CHECK(outcome.error == InstallError::kNoSpace && world.chunk_calls == 0);
    } else {
      CHECK(outcome.error == InstallError::kNone && world.chunk_calls == 3);
    }
  }
}

void PreallocatedResumeNeedsOnlyTheMargin(const std::filesystem::path& root) {
  // An interrupted install on a preallocating filesystem (exFAT) already holds its files at full
  // size. Its unwritten chunks need no further space, and the bytes count as held.
  auto world = MakeWorld();
  world.free_bytes = 64ull * 1024 * 1024;  // the margin alone; a fresh install needs 39 more bytes
  std::filesystem::create_directories(root / DepotInstaller::kJournalDir);
  std::filesystem::create_directories(root / "DATA");
  // Written zeros, not a sparse resize: the files occupy blocks as exFAT's ftruncate leaves them.
  std::ofstream(root / "game.exe") << std::string(26, '\0');
  std::ofstream(root / "DATA/level.dat") << std::string(13, '\0');
  std::ofstream(root / DepotInstaller::kJournalDir / "journal").close();
  DepotInstaller installer(world.Hooks());
  installer.SetTuning({1, 256});
  std::atomic<bool> cancel{false};
  InstallProgress first;
  bool seen = false;
  const auto outcome = installer.Run({}, Target(root), cancel, [&](const InstallProgress& progress) {
    if (!seen) { first = progress; seen = true; }
  });
  CHECK(outcome.error == InstallError::kNone && world.chunk_calls == 3);
  CHECK(seen && first.held_bytes == first.install_total);
  CHECK(Read(root / "game.exe") == "MZ-executable-second-chunk");
}

void UnknownSpacePreventsDownload(const std::filesystem::path& root) {
  auto world = MakeWorld();
  world.free_bytes = 0;  // The default query maps an unverifiable filesystem to zero.
  DepotInstaller installer(world.Hooks());
  std::atomic<bool> cancel{false};
  const auto outcome = installer.Run({}, Target(root), cancel);
  CHECK(outcome.error == InstallError::kNoSpace && world.chunk_calls == 0);
  CHECK(!std::filesystem::exists(root / "DATA" / "level.dat"));
}

void DefaultSpaceQueryFailurePreventsDownload(const std::filesystem::path& root) {
  auto world = MakeWorld();
  auto hooks = world.Hooks();
  hooks.free_bytes = {};  // Exercise the real filesystem query, not the free-space fake.
  hooks.trace = [&](const char* step) {
    if (std::string(step) != "space") return;
    // The destination ceased to be a directory after preparation. Filesystem
    // capacity alone must not allow downloads when its directory cannot be opened.
    std::filesystem::remove_all(root);
    std::ofstream(root) << "unavailable destination";
  };
  DepotInstaller installer(std::move(hooks));
  std::atomic<bool> cancel{false};
  const auto outcome = installer.Run({}, Target(root), cancel);
  CHECK(outcome.error == InstallError::kNoSpace && world.chunk_calls == 0);
}

void CorruptChunksFailAfterRetries(const std::filesystem::path& root) {
  auto world = MakeWorld();
  world.corrupt = true;
  DepotInstaller installer(world.Hooks());
  installer.SetTuning({1, 256});
  std::atomic<bool> cancel{false};
  CHECK(installer.Run({}, Target(root), cancel).error == InstallError::kCorrupt);
  CHECK(world.chunk_calls == DepotInstaller::kChunkAttempts * 2);
}

void BusyCdnIsRetriedAfterAWait(const std::filesystem::path& root) {
  auto world = MakeWorld();
  world.busy_calls = 3;
  auto hooks = world.Hooks();
  std::vector<int> waits;
  hooks.pause = [&](int milliseconds, const std::atomic<bool>&) { waits.push_back(milliseconds); };
  DepotInstaller installer(std::move(hooks));
  installer.SetTuning({1, 256});
  std::atomic<bool> cancel{false};
  CHECK(installer.Run({}, Target(root), cancel).error == InstallError::kNone);
  CHECK((waits == std::vector<int>{1000, 2000, 4000}));
}

void NoContentServersSaysWhy(const std::filesystem::path& root) {
  auto world = MakeWorld();
  auto hooks = world.Hooks();
  hooks.servers = [](std::uint32_t, const std::atomic<bool>&, std::vector<ContentServer>*) { return CdnError::kProtocol; };
  hooks.last_failure = [] { return std::string("servers=3 usable=0"); };
  DepotInstaller installer(std::move(hooks));
  std::atomic<bool> cancel{false};
  const auto outcome = installer.Run({}, Target(root), cancel);
  CHECK(outcome.error == InstallError::kNetwork && outcome.detail == "no content servers servers=3 usable=0");
  CHECK(world.chunk_calls == 0);
}

void BrokenManifestIsFetchedAgainElsewhere(const std::filesystem::path& root) {
  // A busy CDN answers 502 or sends a cut-off manifest: the next server is asked after a wait.
  auto world = MakeWorld();
  auto hooks = world.Hooks();
  const auto good = hooks.manifest;
  int calls = 0;
  hooks.manifest = [&](const ContentServer& server, const DepotGrant& grant, const std::atomic<bool>& cancel,
                       DepotManifest* out) {
    ++calls;
    if (calls == 1) return InstallError::kNetwork;
    if (calls == 2) return InstallError::kCorrupt;
    return good(server, grant, cancel, out);
  };
  std::vector<int> waits;
  hooks.pause = [&](int milliseconds, const std::atomic<bool>&) { waits.push_back(milliseconds); };
  DepotInstaller installer(std::move(hooks));
  installer.SetTuning({1, 256});
  std::atomic<bool> cancel{false};
  CHECK(installer.Run({}, Target(root / "recovers"), cancel).error == InstallError::kNone);
  CHECK((waits == std::vector<int>{1000, 2000}));

  // Never sent by one server and never readable from another: a network problem, not corrupt content.
  auto failing = world.Hooks();
  int tries = 0;
  failing.manifest = [&](const ContentServer&, const DepotGrant&, const std::atomic<bool>&, DepotManifest*) {
    return ++tries % 2 ? InstallError::kNetwork : InstallError::kCorrupt;
  };
  failing.pause = [](int, const std::atomic<bool>&) {};
  DepotInstaller stuck(std::move(failing));
  stuck.SetTuning({1, 256});
  CHECK(stuck.Run({}, Target(root / "stuck"), cancel).error == InstallError::kNetwork);
  CHECK(tries == 4);
}

void RemovesOnlyManagedFolders(const std::filesystem::path& base) {
  const auto unmanaged = base / "unmanaged";
  std::filesystem::create_directories(unmanaged);
  std::ofstream(unmanaged / "keep.txt") << "keep";
  CHECK(!RemoveManagedInstall(unmanaged.string()));
  CHECK(std::filesystem::exists(unmanaged / "keep.txt"));
  const auto managed = base / "managed";
  InstallsMergedDepotsAndWritesRecord(managed);
  CHECK(RemoveManagedInstall(managed.string()));
  CHECK(!std::filesystem::exists(managed));
  CHECK(!RemoveManagedInstall("relative/path"));

  // Files the install did not record (written by the game or other tools, in open folders) go too.
  InstallsMergedDepotsAndWritesRecord(managed);
  std::filesystem::create_directories(managed / "bottle/drive_c");
  std::filesystem::permissions(managed / "bottle", std::filesystem::perms::all);
  std::ofstream(managed / "bottle/drive_c/user.reg") << "written later";
  std::ofstream(managed / "config.ini") << "written later";
  std::string failure;
  CHECK(RemoveManagedInstall(managed.string(), &failure) && failure.empty());
  CHECK(!std::filesystem::exists(managed));

  // A folder that cannot be emptied says why and keeps its install record, so a retry is
  // still recognized as Playden's and finishes the job.
  InstallsMergedDepotsAndWritesRecord(managed);
  std::filesystem::create_directories(managed / "locked");
  std::ofstream(managed / "locked/file.bin") << "stuck";
  CHECK(chmod((managed / "locked").c_str(), 0500) == 0);
  CHECK(!RemoveManagedInstall(managed.string(), &failure));
  CHECK(failure.find("could not be deleted") != std::string::npos);
  CHECK(std::filesystem::exists(managed / DepotInstaller::kInstallRecord));
  CHECK(!std::filesystem::exists(managed / DepotInstaller::kFileTree));
  CHECK(chmod((managed / "locked").c_str(), 0700) == 0);
  CHECK(RemoveManagedInstall(managed.string()));
  CHECK(!std::filesystem::exists(managed));
}


// Updates. The base install (MakeWorld) holds game.exe, data/level.dat (the patch depot's), empty.txt.
void InstallBase(World& world, const std::filesystem::path& root) {
  std::filesystem::remove_all(root);
  DepotInstaller installer(world.Hooks());
  installer.SetTuning({1, 256});
  std::atomic<bool> cancel{false};
  CHECK(installer.Run({}, Target(root), cancel).error == InstallError::kNone);
}

InstallTarget UpdateTarget(const std::filesystem::path& root) {
  auto target = Target(root);
  target.update = true;
  return target;
}

std::string Update(const std::filesystem::path& root, const char* name) {
  return (root / DepotInstaller::kUpdateDir / name).string();
}

// A changed file is downloaded beside the game and replaces it; an unchanged one is never fetched; a file
// the new version dropped goes, files the game wrote stay; then the tree, file list and record are new.
void UpdateReplacesChangedFiles(const std::filesystem::path& root) {
  auto world = MakeWorld();
  InstallBase(world, root);
  std::ofstream(root / "save.dat") << "player progress";
  auto& base = world.manifests[441];
  base.gid = 21;
  base.files[0] = world.AddFile("game.exe", {"MZ-executable-", "patched-chunk"});
  base.files.erase(base.files.begin() + 3);  // empty.txt
  base.files.push_back(world.AddFile("new/added.txt", {"added"}));
  auto target = UpdateTarget(root);
  target.depots[0].manifest_gid = 21;
  target.language = "german";
  std::vector<std::string> folders;
  target.before_apply = [&](const std::string& folder, const std::vector<std::uint32_t>& skipped) {
    folders.push_back(folder);
    CHECK(skipped.empty());
    // The game is untouched until here.
    CHECK(Read(root / "game.exe") == "MZ-executable-second-chunk");
    CHECK(Read(std::filesystem::path(folder) / "staging" / "game.exe") == "MZ-executable-patched-chunk");
    std::ofstream(std::filesystem::path(folder) / ".playden-steam") << "depots=441,442\n";
    return true;
  };
  std::vector<InstallPhase> phases;
  InstallProgress last;
  world.chunk_calls = 0;
  DepotInstaller installer(world.Hooks());
  installer.SetTuning({1, 256});
  std::atomic<bool> cancel{false};
  const auto outcome = installer.Run({}, target, cancel, [&](const InstallProgress& progress) {
    if (phases.empty() || phases.back() != progress.phase) phases.push_back(progress.phase);
    last = progress;
  });
  CHECK(outcome.error == InstallError::kNone);
  CHECK(world.chunk_calls == 3);  // game.exe's two chunks (no reuse yet) and added.txt
  CHECK((phases == std::vector<InstallPhase>{InstallPhase::kDownloading, InstallPhase::kApplying}));
  CHECK(folders.size() == 1 && folders[0] == (root / DepotInstaller::kUpdateDir).string());
  CHECK(Read(root / "game.exe") == "MZ-executable-patched-chunk");
  CHECK(Read(root / "data/level.dat") == "level-patched");
  CHECK(Read(root / "new/added.txt") == "added");
  CHECK(!std::filesystem::exists(root / "empty.txt"));
  CHECK(Read(root / "save.dat") == "player progress");
  CHECK(Read(root / ".playden-steam") == "depots=441,442\n");
  CHECK(!std::filesystem::exists(root / DepotInstaller::kUpdateDir));
  // Progress counts only the staged chunks; the install size is the whole new version.
  std::uint64_t staged = 0;
  for (const auto& file : {base.files[0], base.files.back()})
    for (const auto& chunk : file.chunks) staged += chunk.compressed_size;
  CHECK(last.download_total == staged && last.download_done == staged);
  CHECK(outcome.progress.install_total == 27 + 13 + 5 && outcome.progress.files_total == 3);
  const auto record = playden::installs::ReadInstallRecord((root / DepotInstaller::kInstallRecord).string());
  CHECK(record && record->language == "german" && record->install_bytes == 45);
  CHECK((record->depots == std::vector<playden::installs::RecordedDepot>{{441, 21, 440, 440}, {442, 12, 440, 440}}));
  const auto tree = Read(root / DepotInstaller::kFileTree);
  CHECK(tree.find("\t5\tnew/added.txt\n") != std::string::npos && tree.find("empty.txt") == std::string::npos);
  CHECK(tree.find(HexOf(base.files[0].sha_content) + "\t27\tgame.exe\n") != std::string::npos);
  const auto list = Read(root / DepotInstaller::kFileList);
  CHECK(list.find("f\tnew/added.txt\n") != std::string::npos && list.find("empty.txt") == std::string::npos);
  // Nothing changed: nothing is fetched.
  target.before_apply = {};
  world.chunk_calls = 0;
  CHECK(installer.Run({}, target, cancel).error == InstallError::kNone);
  CHECK(world.chunk_calls == 0 && !std::filesystem::exists(root / DepotInstaller::kUpdateDir));
  CHECK(Read(root / "game.exe") == "MZ-executable-patched-chunk");
  // Not installed: no update.
  CHECK(installer.Run({}, UpdateTarget(root.string() + "-missing"), cancel).error == InstallError::kProtocol);
}

// A depot that leaves the selection gives its files back to the depot under it: the patch depot won
// level.dat; without it the base depot's level.dat must be written again (it never changed itself).
void UpdateRestoresFilesOfARemovedDepot(const std::filesystem::path& root) {
  auto world = MakeWorld();
  InstallBase(world, root);
  CHECK(Read(root / "DATA/level.dat") == "level-patched");
  auto target = UpdateTarget(root);
  target.depots.pop_back();
  world.chunk_calls = 0;
  DepotInstaller installer(world.Hooks());
  installer.SetTuning({1, 256});
  std::atomic<bool> cancel{false};
  CHECK(installer.Run({}, target, cancel).error == InstallError::kNone);
  CHECK(world.chunk_calls == 1);
  CHECK(Read(root / "data/level.dat") == "level-one");
  // The patch depot spelled it DATA/: a filesystem that tells case apart doesn't keep its copy.
  for (const auto& entry : std::filesystem::recursive_directory_iterator(root))
    if (entry.is_regular_file()) CHECK(Read(entry.path()) != "level-patched");
  const auto record = playden::installs::ReadInstallRecord((root / DepotInstaller::kInstallRecord).string());
  CHECK(record && (record->depots == std::vector<playden::installs::RecordedDepot>{{441, 11, 440, 440}}));
}

// An install without a file tree (from before trees): the files on disk are hashed. A matching one stays,
// a damaged one is downloaded again.
void UpdateWithoutTreeHashesFiles(const std::filesystem::path& root) {
  auto world = MakeWorld();
  InstallBase(world, root);
  std::filesystem::remove(root / DepotInstaller::kFileTree);
  {
    std::ofstream damaged(root / "game.exe", std::ios::binary | std::ios::trunc);
    damaged << "MZ-executable-XXXXXX-chunk";  // same size
  }
  std::vector<InstallPhase> phases;
  std::vector<std::string> traces;
  auto hooks = world.Hooks();
  hooks.trace = [&](const char* step) { traces.emplace_back(step); };
  world.chunk_calls = 0;
  DepotInstaller installer(hooks);
  installer.SetTuning({1, 256});
  std::atomic<bool> cancel{false};
  CHECK(installer.Run({}, UpdateTarget(root), cancel, [&](const InstallProgress& progress) {
          if (phases.empty() || phases.back() != progress.phase) phases.push_back(progress.phase);
        }).error == InstallError::kNone);
  CHECK((phases == std::vector<InstallPhase>{InstallPhase::kCheckingFiles, InstallPhase::kDownloading, InstallPhase::kApplying}));
  CHECK(std::count(traces.begin(), traces.end(), "update-check") == 1);
  CHECK(world.chunk_calls == 2);  // game.exe only
  CHECK(Read(root / "game.exe") == "MZ-executable-second-chunk");
  CHECK(std::filesystem::exists(root / DepotInstaller::kFileTree));
  CHECK(std::filesystem::exists(root / "empty.txt"));
}

// An update stopped while downloading leaves the game as it was and carries on without fetching again
// what it has; stopped while putting files in place, it finishes without the network.
void InterruptedUpdateResumes(const std::filesystem::path& root) {
  auto world = MakeWorld();
  InstallBase(world, root);
  auto& base = world.manifests[441];
  base.gid = 21;
  base.files[0] = world.AddFile("game.exe", {"MZ-executable-", "patched-chunk"});
  base.files.erase(base.files.begin() + 3);  // empty.txt
  base.files.push_back(world.AddFile("new/added.txt", {"added"}));
  auto target = UpdateTarget(root);
  target.depots[0].manifest_gid = 21;
  std::atomic<bool> cancel{false};
  std::string stop_at;
  auto hooks = world.Hooks();
  hooks.trace = [&](const char* step) { if (stop_at == step) cancel.store(true); };
  DepotInstaller installer(hooks);
  installer.SetTuning({1, 256});
  // Staging: the second chunk cancels.
  world.cancel = &cancel;
  world.cancel_after = 1;
  world.chunk_calls = 0;
  CHECK(installer.Run({}, target, cancel).error == InstallError::kCanceled);
  auto state = playden::installs::ReadUpdateState(root.string());
  CHECK(state && state->phase == playden::installs::UpdatePhase::kStaging);
  CHECK((state->depots == std::vector<playden::installs::RecordedDepot>{{441, 21, 0, 0}, {442, 12, 0, 0}}));
  CHECK(Read(root / "game.exe") == "MZ-executable-second-chunk" && std::filesystem::exists(root / "empty.txt"));
  CHECK(Read(Update(root, "staged")) == "game.exe\nnew/added.txt\n");
  CHECK(Read(Update(root, "paths")).find("f\tempty.txt\n") != std::string::npos);
  CHECK(Read(Update(root, "paths")).find("f\tnew/added.txt\n") != std::string::npos);
  // Replacing: stops before the first file moves.
  cancel.store(false);
  world.cancel_after = -1;
  world.chunk_calls = 0;
  stop_at = "update-replace";
  CHECK(installer.Run({}, target, cancel).error == InstallError::kCanceled);
  CHECK(world.chunk_calls == 2);  // what the canceled run had not written
  state = playden::installs::ReadUpdateState(root.string());
  CHECK(state && state->phase == playden::installs::UpdatePhase::kReplacing);
  CHECK(Read(root / "game.exe") == "MZ-executable-second-chunk");
  // Staged files can't be thrown away now.
  std::string failure;
  CHECK(!DiscardManagedUpdate(root.string(), &failure) && !failure.empty());
  CHECK(std::filesystem::exists(Update(root, "staging/game.exe")));
  // Deleting: the files are in place, the old ones not yet deleted. Run again without the network.
  cancel.store(false);
  stop_at = "update-delete";
  world.chunk_calls = 0;
  auto offline = world.Hooks();
  offline.access = [](const StoredSession&, std::uint32_t, const std::vector<DepotRequest>&, const std::atomic<bool>&) {
    DepotAccessResult result;
    result.error = CmError::kNetwork;
    return result;
  };
  offline.trace = hooks.trace;
  DepotInstaller offline_installer(offline);
  CHECK(offline_installer.Run({}, target, cancel).error == InstallError::kCanceled);
  state = playden::installs::ReadUpdateState(root.string());
  CHECK(state && state->phase == playden::installs::UpdatePhase::kDeleting);
  CHECK(Read(root / "game.exe") == "MZ-executable-patched-chunk" && std::filesystem::exists(root / "empty.txt"));
  // An uninstall in between would find the new files through "paths".
  CHECK(Read(Update(root, "paths")).find("f\tnew/added.txt\n") != std::string::npos);
  cancel.store(false);
  stop_at.clear();
  const auto finished = DepotInstaller::FinishUpdate(root.string(), cancel);
  CHECK(finished.error == InstallError::kNone && finished.progress.install_total == 45);
  CHECK(world.chunk_calls == 0);
  CHECK(!std::filesystem::exists(root / "empty.txt") && Read(root / "new/added.txt") == "added");
  CHECK(!std::filesystem::exists(root / DepotInstaller::kUpdateDir));
  CHECK(DepotInstaller::FinishUpdate(root.string(), cancel).error == InstallError::kProtocol);
}

// Playden stopped after the deletion, while the new tree, file list and record went in place: finishing
// again completes the update instead of failing on what already moved.
void UpdateFinishIsReentrant(const std::filesystem::path& base) {
  for (const bool moved : {false, true}) {
    const auto root = base / (moved ? "moved" : "deleted");
    auto world = MakeWorld();
    InstallBase(world, root);
    auto& files = world.manifests[441].files;
    world.manifests[441].gid = 21;
    files[0] = world.AddFile("game.exe", {"MZ-executable-", "patched-chunk"});
    files.erase(files.begin() + 3);  // empty.txt
    files.push_back(world.AddFile("new/added.txt", {"added"}));
    auto target = UpdateTarget(root);
    target.depots[0].manifest_gid = 21;
    std::atomic<bool> cancel{false};
    auto hooks = world.Hooks();
    hooks.trace = [&](const char* step) { if (std::string(step) == "update-delete") cancel.store(true); };
    DepotInstaller installer(hooks);
    CHECK(installer.Run({}, target, cancel).error == InstallError::kCanceled);
    // What the deletion did, then (moved) the tree, file list and record in place and junk left in staging.
    std::filesystem::remove(root / "empty.txt");
    std::filesystem::remove(Update(root, "old"));
    if (moved) {
      std::filesystem::rename(Update(root, "tree"), root / DepotInstaller::kFileTree);
      std::filesystem::rename(Update(root, "files"), root / DepotInstaller::kFileList);
      std::filesystem::rename(Update(root, "record"), root / DepotInstaller::kInstallRecord);
      std::ofstream(Update(root, "staging/leftover.bin")) << "junk";
    }
    cancel.store(false);
    const auto finished = DepotInstaller::FinishUpdate(root.string(), cancel);
    CHECK(finished.error == InstallError::kNone && finished.progress.install_total == 45);
    CHECK(!std::filesystem::exists(root / DepotInstaller::kUpdateDir));
    CHECK(Read(root / "game.exe") == "MZ-executable-patched-chunk" && Read(root / "new/added.txt") == "added");
    CHECK(Read(root / "data/level.dat") == "level-patched" && !std::filesystem::exists(root / "empty.txt"));
    CHECK(Read(root / DepotInstaller::kFileList).find("f\tnew/added.txt\n") != std::string::npos);
    const auto record = playden::installs::ReadInstallRecord((root / DepotInstaller::kInstallRecord).string());
    CHECK(record && record->depots.size() == 2 && record->depots[0].gid == 21);
  }
}

// Cancel while downloading deletes only the update's folder; an uninstall during an update deletes the
// files of both versions.
void DiscardAndUninstallDuringUpdate(const std::filesystem::path& root) {
  auto world = MakeWorld();
  InstallBase(world, root);
  const auto record_before = Read(root / DepotInstaller::kInstallRecord);
  auto& base = world.manifests[441];
  base.gid = 21;
  base.files.push_back(world.AddFile("new/added.txt", {"added", "more"}));
  auto target = UpdateTarget(root);
  target.depots[0].manifest_gid = 21;
  std::atomic<bool> cancel{false};
  world.cancel = &cancel;
  world.cancel_after = 1;
  DepotInstaller installer(world.Hooks());
  installer.SetTuning({1, 256});
  CHECK(installer.Run({}, target, cancel).error == InstallError::kCanceled);
  CHECK(std::filesystem::exists(Update(root, "staging/new/added.txt")));
  CHECK(DiscardManagedUpdate(root.string()));
  CHECK(!std::filesystem::exists(root / DepotInstaller::kUpdateDir));
  CHECK(!std::filesystem::exists(root / "new"));
  CHECK(Read(root / DepotInstaller::kInstallRecord) == record_before && Read(root / "game.exe") == "MZ-executable-second-chunk");
  CHECK(DiscardManagedUpdate(root.string()));  // nothing left to discard

  // Uninstall with the new files in place and the old ones still there.
  cancel.store(false);
  world.cancel_after = -1;
  auto hooks = world.Hooks();
  hooks.trace = [&](const char* step) { if (std::string(step) == "update-delete") cancel.store(true); };
  DepotInstaller stopping(hooks);
  CHECK(stopping.Run({}, target, cancel).error == InstallError::kCanceled);
  CHECK(Read(root / "new/added.txt") == "addedmore");
  CHECK(RemoveManagedInstall(root.string()));
  CHECK(!std::filesystem::exists(root));
}

// Space: only the staged bytes and the margin are needed.
void UpdateNeedsSpaceForChangedFilesOnly(const std::filesystem::path& root) {
  auto world = MakeWorld();
  InstallBase(world, root);
  world.manifests[441].gid = 21;
  world.manifests[441].files.push_back(world.AddFile("added.txt", {"12345"}));
  auto target = UpdateTarget(root);
  target.depots[0].manifest_gid = 21;
  std::atomic<bool> cancel{false};
  world.free_bytes = kInstallSpaceMargin + 4;
  DepotInstaller short_installer(world.Hooks());
  CHECK(short_installer.Run({}, target, cancel).error == InstallError::kNoSpace);
  CHECK(Read(root / "game.exe") == "MZ-executable-second-chunk");
  world.free_bytes = kInstallSpaceMargin + 5;
  DepotInstaller installer(world.Hooks());
  CHECK(installer.Run({}, target, cancel).error == InstallError::kNone);
  CHECK(Read(root / "added.txt") == "12345");
}

// The game runs when the changed files are downloaded: the update stops before any of the game's files change,
// keeps its downloads, and the next run puts them in place without downloading them again. An update that
// stopped while putting its files in place asks again before it carries on.
void UpdateWaitsWhileTheGameRuns(const std::filesystem::path& root) {
  auto world = MakeWorld();
  InstallBase(world, root);
  const auto record_before = Read(root / DepotInstaller::kInstallRecord);
  world.manifests[441].gid = 21;
  world.manifests[441].files[0] = world.AddFile("game.exe", {"MZ-executable-", "patched-chunk"});
  auto target = UpdateTarget(root);
  target.depots[0].manifest_gid = 21;
  int asked = 0;
  bool running = true;
  bool applied = false;
  target.may_apply = [&] { ++asked; return !running; };
  target.before_apply = [&](const std::string&, const std::vector<std::uint32_t>&) { applied = true; return true; };
  std::atomic<bool> cancel{false};
  DepotInstaller installer(world.Hooks());
  installer.SetTuning({1, 256});
  CHECK(installer.Run({}, target, cancel).error == InstallError::kGameRunning);
  CHECK(asked == 1 && !applied);
  CHECK(Read(root / "game.exe") == "MZ-executable-second-chunk");
  CHECK(Read(root / DepotInstaller::kInstallRecord) == record_before);
  const auto state = playden::installs::ReadUpdateState(root.string());
  CHECK(state && state->phase == playden::installs::UpdatePhase::kStaging);
  const auto fetched = world.chunk_calls;
  CHECK(fetched > 0);
  running = false;
  CHECK(installer.Run({}, target, cancel).error == InstallError::kNone);
  CHECK(asked == 2 && applied && world.chunk_calls == fetched);
  CHECK(Read(root / "game.exe") == "MZ-executable-patched-chunk");
  CHECK(!std::filesystem::exists(root / DepotInstaller::kUpdateDir));
  // The game's launch is set up again from the new files before it next starts.
  CHECK(std::filesystem::is_regular_file(root / playden::installs::kFilesChangedName));
  std::filesystem::remove(root / playden::installs::kFilesChangedName);

  // Stopped while replacing: the run asks before it carries on.
  world.manifests[441].gid = 22;
  world.manifests[441].files[0] = world.AddFile("game.exe", {"MZ-executable-", "third-chunk"});
  target.depots[0].manifest_gid = 22;
  target.before_apply = [&](const std::string&, const std::vector<std::uint32_t>&) {
    cancel.store(true);  // stops in the replacing phase
    return true;
  };
  CHECK(installer.Run({}, target, cancel).error == InstallError::kCanceled);
  CHECK(playden::installs::ReadUpdateState(root.string())->phase == playden::installs::UpdatePhase::kReplacing);
  cancel.store(false);
  running = true;
  asked = 0;
  CHECK(installer.Run({}, target, cancel).error == InstallError::kGameRunning);
  CHECK(asked == 1 && playden::installs::ReadUpdateState(root.string())->phase == playden::installs::UpdatePhase::kReplacing);
  running = false;
  CHECK(installer.Run({}, target, cancel).error == InstallError::kNone);
  CHECK(Read(root / "game.exe") == "MZ-executable-third-chunk");
  CHECK(std::filesystem::is_regular_file(root / playden::installs::kFilesChangedName));
}

// A folder of the game replaced by a link (to somewhere outside it): the update deletes and moves nothing
// through it. A file the new version dropped there stays where the link leads; a file it adds there stops
// the update before the game's files change.
void UpdateNeverFollowsLinks(const std::filesystem::path& root) {
  const auto outside = root.parent_path() / (root.filename().string() + "-outside");
  std::filesystem::remove_all(outside);
  auto world = MakeWorld();
  InstallBase(world, root);
  std::filesystem::create_directories(outside);
  std::filesystem::rename(root / "data/level.dat", outside / "level.dat");
  std::filesystem::remove(root / "data");
  std::filesystem::create_directory_symlink(outside, root / "data");
  // Adds data/new.txt: the folder is a link, so it can't be made.
  auto& base = world.manifests[441];
  base.gid = 21;
  base.files.push_back(world.AddFile("data/new.txt", {"new"}));
  auto target = UpdateTarget(root);
  target.depots[0].manifest_gid = 21;
  std::atomic<bool> cancel{false};
  std::vector<std::string> traces;
  auto hooks = world.Hooks();
  hooks.trace = [&](const char* step) { traces.emplace_back(step); };
  DepotInstaller installer(hooks);
  const auto blocked = installer.Run({}, target, cancel);
  CHECK(blocked.error == InstallError::kStorage && blocked.detail.find("data is in the way") != std::string::npos);
  CHECK(!std::filesystem::exists(outside / "new.txt") && Read(outside / "level.dat") == "level-patched");
  auto state = playden::installs::ReadUpdateState(root.string());
  CHECK(state && state->phase == playden::installs::UpdatePhase::kStaging);
  CHECK(DiscardManagedUpdate(root.string()));
  // Drops data/ and its level.dat (the patch depot goes too): the file behind the link stays.
  base.files.pop_back();
  base.files.erase(base.files.begin() + 1, base.files.begin() + 3);
  base.gid = 22;
  target.depots[0].manifest_gid = 22;
  target.depots.pop_back();
  CHECK(installer.Run({}, target, cancel).error == InstallError::kNone);
  CHECK(Read(outside / "level.dat") == "level-patched");
  CHECK(std::count(traces.begin(), traces.end(), "update-skip-link") == 1);
  CHECK(Read(root / "game.exe") == "MZ-executable-second-chunk" && std::filesystem::exists(root / "empty.txt"));
  std::filesystem::remove(root / "data");
  std::filesystem::remove_all(outside);
}

// A file the game wrote where the new version has a folder is never deleted: the update stops before the
// game's files change (or, written once they started changing, where it is), naming it.
void UpdateKeepsFilesTheGameWrote(const std::filesystem::path& root) {
  auto world = MakeWorld();
  InstallBase(world, root);
  std::ofstream(root / "new") << "written by the game";
  auto& base = world.manifests[441];
  base.gid = 21;
  base.files.push_back(world.AddFile("new/added.txt", {"added"}));
  auto target = UpdateTarget(root);
  target.depots[0].manifest_gid = 21;
  std::atomic<bool> cancel{false};
  auto hooks = world.Hooks();
  DepotInstaller installer(hooks);
  const auto outcome = installer.Run({}, target, cancel);
  CHECK(outcome.error == InstallError::kStorage && outcome.detail.find("new is in the way") != std::string::npos);
  CHECK(Read(root / "new") == "written by the game");
  auto state = playden::installs::ReadUpdateState(root.string());
  CHECK(state && state->phase == playden::installs::UpdatePhase::kStaging);
  // Moved away, the update goes on; written again once replacing started, it stays and the update waits.
  std::filesystem::remove(root / "new");
  hooks.trace = [&](const char* step) {
    if (std::string(step) == "update-replace") std::ofstream(root / "new") << "written again";
  };
  DepotInstaller racing(hooks);
  const auto finishing = racing.Run({}, target, cancel);
  CHECK(finishing.error == InstallError::kStorage && finishing.detail.find("new is in the way") != std::string::npos);
  CHECK(Read(root / "new") == "written again");
  state = playden::installs::ReadUpdateState(root.string());
  CHECK(state && state->phase == playden::installs::UpdatePhase::kReplacing);
  std::filesystem::remove(root / "new");
  CHECK(DepotInstaller::FinishUpdate(root.string(), cancel).error == InstallError::kNone);
  CHECK(Read(root / "new/added.txt") == "added");
}

// An interrupted update resumed for other manifests starts its staging again: what the first run staged
// (a file "foo" where the new version has a folder) and its journal go; the paths either run listed stay.
void UpdateForOtherManifestsRestagesFromScratch(const std::filesystem::path& root) {
  auto world = MakeWorld();
  InstallBase(world, root);
  auto& base = world.manifests[441];
  base.gid = 21;
  base.files.push_back(world.AddFile("foo", {"foo-file"}));
  base.files.push_back(world.AddFile("zzz.txt", {"last"}));
  auto target = UpdateTarget(root);
  target.depots[0].manifest_gid = 21;
  std::atomic<bool> cancel{false};
  world.cancel = &cancel;
  world.cancel_after = 1;
  DepotInstaller installer(world.Hooks());
  installer.SetTuning({1, 256});
  CHECK(installer.Run({}, target, cancel).error == InstallError::kCanceled);
  CHECK(std::filesystem::is_regular_file(Update(root, "staging/foo")));
  // The game moved on: "foo" is a folder now.
  cancel.store(false);
  world.cancel_after = -1;
  base.gid = 22;
  base.files.pop_back();
  base.files.pop_back();
  base.files.push_back(world.AddFile("foo/bar", {"bar-file"}));
  target.depots[0].manifest_gid = 22;
  CHECK(installer.Run({}, target, cancel).error == InstallError::kNone);
  CHECK(Read(root / "foo/bar") == "bar-file" && !std::filesystem::exists(root / "zzz.txt"));
  CHECK(!std::filesystem::exists(root / DepotInstaller::kUpdateDir));
  const auto record = playden::installs::ReadInstallRecord((root / DepotInstaller::kInstallRecord).string());
  CHECK(record && record->depots.size() == 2 && record->depots[0].gid == 22);
}

// An update whose only granted depots are DLC (the game's borrowed depot refused) changes nothing.
void UpdateNeedsTheGamesContent(const std::filesystem::path& root) {
  auto world = MakeWorld();
  InstallBase(world, root);
  const auto record_before = Read(root / DepotInstaller::kInstallRecord);
  auto hooks = world.Hooks();
  hooks.access = [](const StoredSession&, std::uint32_t, const std::vector<DepotRequest>& depots, const std::atomic<bool>&) {
    DepotAccessResult result;
    result.error = CmError::kNone;
    for (const auto& depot : depots) {
      if (depot.optional) result.skipped.push_back(depot.depot_id);
      else result.grants.push_back({depot.depot_id, depot.manifest_gid, kKey, 77});
    }
    return result;
  };
  auto target = UpdateTarget(root);
  target.depots[0].depot_from_app = 877620;
  target.depots[1].owner_app = 999;
  DepotInstaller installer(hooks);
  std::atomic<bool> cancel{false};
  CHECK(installer.Run({}, target, cancel).error == InstallError::kRejected);
  CHECK(Read(root / "game.exe") == "MZ-executable-second-chunk" && std::filesystem::exists(root / "empty.txt"));
  CHECK(Read(root / DepotInstaller::kInstallRecord) == record_before);
  CHECK(!std::filesystem::exists(root / DepotInstaller::kUpdateDir));
}

// The new version has a file where the old one had a folder of files, and a folder where it had a file.
void UpdateSwapsFilesAndFolders(const std::filesystem::path& root) {
  auto world = MakeWorld();
  world.manifests[441].files.push_back(world.AddFile("swap/inner.txt", {"inner"}));
  world.manifests[441].files.push_back(world.AddFile("plain", {"plain-file"}));
  InstallBase(world, root);
  auto& base = world.manifests[441];
  base.gid = 21;
  base.files.pop_back();
  base.files.pop_back();
  base.files.push_back(world.AddFile("swap", {"now-a-file"}));
  base.files.push_back(world.AddFile("plain/inside.txt", {"now-a-folder"}));
  auto target = UpdateTarget(root);
  target.depots[0].manifest_gid = 21;
  std::atomic<bool> cancel{false};
  DepotInstaller installer(world.Hooks());
  const auto outcome = installer.Run({}, target, cancel);
  CHECK(outcome.error == InstallError::kNone);
  CHECK(std::filesystem::is_regular_file(root / "swap") && Read(root / "swap") == "now-a-file");
  CHECK(Read(root / "plain/inside.txt") == "now-a-folder");
}
}  // namespace

// Fastest: fake HTTP clients on a fake clock. A client's first request to a server opens a connection
// (connect_ms before the first byte); the transfer takes the chunk's declared size at the server's rate.
struct FakeNet {
  struct Spec {
    std::uint64_t connect_ms = 20;
    std::function<double(std::int64_t now_ms, int request)> rate;  // MB/s; 0 fails the request
  };
  struct Client {
    std::set<std::string> connected;
    CdnStats stats;
  };
  World* world = nullptr;
  std::mutex mutex;
  std::int64_t now_ms = 0;
  std::vector<std::string> order;  // the server list
  std::map<std::string, Spec> specs;
  std::map<std::string, int> requests;
  std::map<std::string, std::uint32_t> declared;  // sha -> compressed size the manifest claims
  std::vector<std::string> hosts, shas;           // successful requests, in order
  int cancel_after = -1;
  std::atomic<bool>* cancel = nullptr;

  void Add(const std::string& host, double mb_per_s, std::uint64_t connect_ms = 20) {
    order.push_back(host);
    specs[host] = {connect_ms, [mb_per_s](std::int64_t, int) { return mb_per_s; }};
  }
  void Advance(std::int64_t ms) {
    std::lock_guard<std::mutex> lock(mutex);
    now_ms += ms;
  }
  std::chrono::steady_clock::time_point Now() {
    std::lock_guard<std::mutex> lock(mutex);
    return std::chrono::steady_clock::time_point(std::chrono::milliseconds(now_ms));
  }
  // Large files: each chunk is declared at `bytes` compressed bytes; the served containers stay small.
  ManifestFile File(const std::string& path, int count, std::uint32_t bytes) {
    std::vector<std::string> pieces;
    for (int i = 0; i < count; ++i) pieces.push_back(path + "-" + std::to_string(i) + ";");
    auto file = world->AddFile(path, pieces);
    for (auto& chunk : file.chunks) {
      chunk.compressed_size = bytes;
      declared[HexOf(chunk.sha)] = bytes;
    }
    return file;
  }
  InstallerHooks::ChunkFetcher Fetcher() {
    auto client = std::make_shared<Client>();
    InstallerHooks::ChunkFetcher fetcher;
    fetcher.fetch = [this, client](const ContentServer& server, std::uint32_t, const std::string& sha, std::size_t,
                                   const std::atomic<bool>&, std::vector<std::uint8_t>* out) {
      std::lock_guard<std::mutex> lock(mutex);
      if (cancel_after >= 0 && static_cast<int>(shas.size()) >= cancel_after) {
        cancel->store(true);
        return CdnError::kCanceled;
      }
      const auto& spec = specs.at(server.host);
      const double rate = spec.rate(now_ms, requests[server.host]++);
      const bool fresh = client->connected.insert(server.host).second;
      const std::uint64_t before_body = (fresh ? spec.connect_ms : 0) + 2;
      ++client->stats.requests;
      client->stats.new_connections += fresh ? 1 : 0;
      if (rate <= 0) {
        client->connected.erase(server.host);
        ++client->stats.failures;
        client->stats.first_byte_ms += before_body;
        client->stats.total_ms += before_body;
        now_ms += static_cast<std::int64_t>(before_body);
        return CdnError::kNetwork;
      }
      const auto found = declared.find(sha);
      const std::uint64_t bytes = found == declared.end() ? world->served.at(sha).size() : found->second;
      const auto transfer = static_cast<std::uint64_t>(static_cast<double>(bytes) / (rate * 1000.0));
      client->stats.first_byte_ms += before_body;
      client->stats.total_ms += before_body + transfer;
      client->stats.bytes += bytes;
      now_ms += static_cast<std::int64_t>(before_body + transfer);
      hosts.push_back(server.host);
      shas.push_back(sha);
      *out = world->served.at(sha);
      return CdnError::kNone;
    };
    fetcher.stats = [this, client] {
      std::lock_guard<std::mutex> lock(mutex);
      return client->stats;
    };
    fetcher.last_failure = [] { return std::string(); };
    return fetcher;
  }
  InstallerHooks Hooks() {
    auto hooks = world->Hooks();
    hooks.servers = [this](std::uint32_t, const std::atomic<bool>&, std::vector<ContentServer>* out) {
      for (const auto& host : order) out->push_back({host, host, 0, true});
      return CdnError::kNone;
    };
    hooks.make_fetcher = [this] { return Fetcher(); };
    hooks.now = [this] { return Now(); };
    return hooks;
  }
};

constexpr const char* kFastly = "fastly.cdn.steampipe.steamcontent.com";
constexpr const char* kAkamai = "steampipe.akamaized.net";
constexpr const char* kGoogle = "google2.cdn.steampipe.steamcontent.com";
constexpr std::uint32_t kMegabyte = 1000 * 1000;

struct RaceRun {
  InstallOutcome outcome;
  std::vector<std::string> races;       // "<step> <result> <detail>"
  std::vector<std::string> remembered;  // remember_fastest calls
  std::string listed;
  std::string server_name;              // the last progress report's
  std::optional<std::string> first_written_name;  // the report after the first write
};

struct RaceSetup {
  int slots = 4;
  std::optional<std::string> cached;
  std::function<void()> on_progress;  // runs on the writer after each report
  bool legacy_chunk_hook = false;     // no per-request stats, as InstallerHooks::chunk
};

RaceRun RunFastest(FakeNet& net, const std::filesystem::path& root, RaceSetup setup = {},
                   std::atomic<bool>* external_cancel = nullptr) {
  auto hooks = net.Hooks();
  RaceRun run;
  if (setup.legacy_chunk_hook) {
    hooks.make_fetcher = nullptr;
    auto fetcher = std::make_shared<InstallerHooks::ChunkFetcher>(net.Fetcher());
    hooks.chunk = [fetcher](const ContentServer& server, std::uint32_t depot, const std::string& sha, std::size_t cap,
                            const std::atomic<bool>& stop, std::vector<std::uint8_t>* out) {
      return fetcher->fetch(server, depot, sha, cap, stop, out);
    };
  }
  hooks.race = [&run](const char* step, const char* result, const std::string& detail) {
    run.races.push_back(std::string(step) + " " + result + " " + detail);
  };
  hooks.cached_fastest = [&setup] { return setup.cached; };
  hooks.remember_fastest = [&run](const std::string& host) { run.remembered.push_back(host); };
  hooks.server_list = [&run](const std::vector<ContentServer>& servers, const char* preference) {
    run.listed = FormatInstallServerList(servers, preference);
  };
  DepotInstaller installer(hooks);
  DownloadTuning tuning{setup.slots, 256};
  tuning.fastest = true;
  installer.SetTuning(tuning);
  std::atomic<bool> own_cancel{false};
  auto& cancel = external_cancel ? *external_cancel : own_cancel;
  net.cancel = &cancel;
  bool written = false;
  run.outcome = installer.Run({}, Target(root), cancel, [&](const InstallProgress& progress) {
    if (!written && progress.download_done > 0) run.first_written_name = progress.server_name;
    if (progress.download_done > 0) written = true;
    run.server_name = progress.server_name;
    if (setup.on_progress) setup.on_progress();
  });
  return run;
}

bool Has(const std::vector<std::string>& lines, const std::string& prefix) {
  return std::any_of(lines.begin(), lines.end(), [&](const std::string& line) { return line.rfind(prefix, 0) == 0; });
}

void FastestWorld(FakeNet& net, World& world) {
  net.world = &world;
  world.manifests[441].files.clear();
  world.manifests[442].files.clear();
}

void RaceWinsAndLaterChunksUseIt(const std::filesystem::path& root) {
  auto world = MakeWorld();
  FakeNet net;
  FastestWorld(net, world);
  net.Add(kFastly, 10);
  net.Add(kAkamai, 40);
  net.Add(kGoogle, 20);
  world.manifests[441].files = {net.File("pak", 300, kMegabyte)};
  const auto run = RunFastest(net, root);
  CHECK(run.outcome.error == InstallError::kNone);
  CHECK(run.listed.find("pref=fastest") != std::string::npos);
  CHECK(run.races.size() == 1 && Has(run.races, "race won winner=akamai#1 rates=fastly#0:10.0,akamai#1:40.0,google#2:20.0 "));
  CHECK(std::stoull(run.races[0].substr(run.races[0].find("samples=") + 8)) >= 3 * DepotInstaller::kRaceSamples);
  CHECK(run.races[0].find("steamcontent") == std::string::npos && run.races[0].find("akamaized") == std::string::npos);
  CHECK(run.remembered == std::vector<std::string>{kAkamai});
  // The name is the last written chunk's server; with four workers that can be a raced one (single-worker
  // tests check it exactly).
  CHECK(!run.server_name.empty());
  // Workers that were fetching when the race ended finish there; everything after goes to the winner.
  for (std::size_t i = 60; i < net.hosts.size(); ++i) CHECK(net.hosts[i] == kAkamai);
}

void TinyChunksSkipTheRace(const std::filesystem::path& root) {
  auto world = MakeWorld();
  FakeNet net;
  FastestWorld(net, world);
  net.Add(kFastly, 10);
  net.Add(kAkamai, 40);
  net.Add(kGoogle, 20);
  // 204 MB, but only five chunks of at least 256 KB: fewer than two per server.
  world.manifests[441].files = {net.File("big", 5, kMegabyte), net.File("small", 800, 255 * 1000)};
  const auto run = RunFastest(net, root, {1});
  CHECK(run.outcome.error == InstallError::kNone);
  CHECK(run.races == std::vector<std::string>{"race skipped-no-large-chunks winner=none"});
  CHECK(run.remembered.empty() && run.server_name == "Fastly");
  for (const auto& host : net.hosts) CHECK(host == kFastly);  // Steam's order
  // No reorder: chunks come in file order.
  const auto& big = world.manifests[441].files[0];
  CHECK(net.shas.front() == HexOf(big.chunks.front().sha));
  // Under 200 MB pending.
  FakeNet small_net;
  auto small_world = MakeWorld();
  FastestWorld(small_net, small_world);
  small_net.Add(kFastly, 10);
  small_net.Add(kAkamai, 40);
  small_world.manifests[441].files = {small_net.File("pak", 150, kMegabyte)};
  const auto small = RunFastest(small_net, root.string() + "-small");
  CHECK(small.outcome.error == InstallError::kNone && small.races == std::vector<std::string>{"race skipped-small winner=none"});
  // One server.
  FakeNet one_net;
  auto one_world = MakeWorld();
  FastestWorld(one_net, one_world);
  one_net.Add(kFastly, 10);
  one_world.manifests[441].files = {one_net.File("pak", 250, kMegabyte)};
  const auto one = RunFastest(one_net, root.string() + "-one");
  CHECK(one.outcome.error == InstallError::kNone && one.races == std::vector<std::string>{"race one-server winner=none"});
}

void RaceFetchesLargeChunksAtTheEnd(const std::filesystem::path& root) {
  auto world = MakeWorld();
  FakeNet net;
  FastestWorld(net, world);
  net.Add(kFastly, 10);
  net.Add(kAkamai, 40);
  std::vector<ManifestFile> files;
  for (int i = 0; i < 40; ++i) files.push_back(net.File("a-tiny-" + std::to_string(i), 1, 2000));
  files.push_back(net.File("z-big", 220, kMegabyte));
  world.manifests[441].files = files;
  const auto run = RunFastest(net, root);
  CHECK(run.outcome.error == InstallError::kNone);
  CHECK(Has(run.races, "race won winner=akamai#1 "));
  // The race chunks went first: the first 16 requests were chunks of the large file.
  std::set<std::string> big;
  for (const auto& chunk : files.back().chunks) big.insert(HexOf(chunk.sha));
  for (std::size_t i = 0; i < 16; ++i) CHECK(big.count(net.shas[i]));
  std::string expected;
  for (int i = 0; i < 220; ++i) expected += "z-big-" + std::to_string(i) + ";";
  CHECK(Read(root / "z-big") == expected);
  CHECK(Read(root / "a-tiny-7") == "a-tiny-7-0;");
  CHECK(std::filesystem::exists(root / DepotInstaller::kInstallRecord));
  CHECK(!std::filesystem::exists(root / DepotInstaller::kJournalDir));
}

void SlowWriterAndHandshakesDoNotDecide(const std::filesystem::path& root) {
  {
    // The writer holds the only worker for 3 s after each chunk; scores come from transfer time.
    auto world = MakeWorld();
    FakeNet net;
    FastestWorld(net, world);
    net.Add(kFastly, 10);
    net.Add(kAkamai, 25);
    world.manifests[441].files = {net.File("pak", 220, kMegabyte)};
    RaceSetup setup{1};
    setup.on_progress = [&net] { net.Advance(3000); };
    const auto run = RunFastest(net, root / "writer", setup);
    CHECK(run.outcome.error == InstallError::kNone);
    CHECK(Has(run.races, "race won winner=akamai#1 rates=fastly#0:10.0,akamai#1:25.0 "));
  }
  {
    // A 2 s handshake with a fast transfer beats a cheap handshake with a slow transfer.
    auto world = MakeWorld();
    FakeNet net;
    FastestWorld(net, world);
    net.Add(kFastly, 10, 5);
    net.Add(kAkamai, 40, 2000);
    world.manifests[441].files = {net.File("pak", 220, kMegabyte)};
    const auto run = RunFastest(net, root / "handshake");
    CHECK(run.outcome.error == InstallError::kNone);
    CHECK(Has(run.races, "race won winner=akamai#1 "));
  }
}

void SlowLinkEndsAtTheCap(const std::filesystem::path& root) {
  auto world = MakeWorld();
  FakeNet net;
  FastestWorld(net, world);
  net.Add(kFastly, 0.4);
  net.Add(kAkamai, 0.5);
  world.manifests[441].files = {net.File("pak", 220, kMegabyte)};
  const auto run = RunFastest(net, root, {1});
  CHECK(run.outcome.error == InstallError::kNone);
  CHECK(Has(run.races, "race won winner=akamai#1 rates=fastly#0:0.4,akamai#1:0.5 "));
  const auto& line = run.races[0];
  const auto ms = std::stoull(line.substr(line.find("race_ms=") + 8));
  const auto samples = std::stoull(line.substr(line.find("samples=") + 8));
  CHECK(ms >= 15000 && ms < 20000 && samples >= 2 && samples < 2 * DepotInstaller::kRaceSamples);
  CHECK(run.first_written_name == std::string() && run.server_name == "Akamai");  // no name while racing
}

void FailingServerNeverWins(const std::filesystem::path& root) {
  auto world = MakeWorld();
  FakeNet net;
  FastestWorld(net, world);
  net.Add(kFastly, 10);
  net.Add(kAkamai, 0);  // refuses every request
  net.Add(kGoogle, 20);
  world.manifests[441].files = {net.File("pak", 220, kMegabyte)};
  const auto run = RunFastest(net, root);
  CHECK(run.outcome.error == InstallError::kNone);
  CHECK(Has(run.races, "race won winner=google#2 rates=fastly#0:10.0,akamai#1:failed,google#2:20.0 "));
  CHECK(std::filesystem::exists(root / DepotInstaller::kInstallRecord));
}

void ResumeRacesOnlyWhatIsLeft(const std::filesystem::path& root) {
  auto world = MakeWorld();
  FakeNet net;
  FastestWorld(net, world);
  net.Add(kFastly, 10);
  net.Add(kAkamai, 40);
  world.manifests[441].files = {net.File("pak", 400, kMegabyte)};
  std::atomic<bool> cancel{false};
  net.cancel_after = 150;
  RaceSetup setup{1};
  CHECK(RunFastest(net, root, setup, &cancel).outcome.error == InstallError::kCanceled);
  const std::set<std::string> fetched(net.shas.begin(), net.shas.end());
  net.cancel_after = -1;
  net.shas.clear();
  cancel = false;
  const auto second = RunFastest(net, root, setup, &cancel);
  CHECK(second.outcome.error == InstallError::kNone && Has(second.races, "race won "));
  CHECK(net.shas.size() == 250);
  for (const auto& sha : net.shas) CHECK(!fetched.count(sha));
  // Under 200 MB left: no race.
  auto small_world = MakeWorld();
  FakeNet small_net;
  FastestWorld(small_net, small_world);
  small_net.Add(kFastly, 10);
  small_net.Add(kAkamai, 40);
  small_world.manifests[441].files = {small_net.File("pak", 300, kMegabyte)};
  small_net.cancel_after = 120;
  cancel = false;
  CHECK(RunFastest(small_net, root.string() + "-small", setup, &cancel).outcome.error == InstallError::kCanceled);
  small_net.cancel_after = -1;
  cancel = false;
  const auto rest = RunFastest(small_net, root.string() + "-small", setup, &cancel);
  CHECK(rest.outcome.error == InstallError::kNone);
  CHECK(Has(rest.races, "race skipped-small winner=none"));
}

void CachedWinnerSkipsTheRace(const std::filesystem::path& root) {
  auto world = MakeWorld();
  FakeNet net;
  FastestWorld(net, world);
  net.Add(kFastly, 10);
  net.Add(kAkamai, 40);
  net.Add(kGoogle, 20);
  world.manifests[441].files = {net.File("pak", 220, kMegabyte)};
  RaceSetup setup{1};
  setup.cached = kGoogle;
  const auto run = RunFastest(net, root / "hit", setup);
  CHECK(run.outcome.error == InstallError::kNone);
  CHECK(run.races == std::vector<std::string>{"race cached winner=google#2"});
  CHECK(run.remembered.empty() && run.server_name == "Google Cloud" && run.first_written_name == run.server_name);
  for (const auto& host : net.hosts) CHECK(host == kGoogle);
  CHECK(net.shas.front() == HexOf(world.manifests[441].files[0].chunks.front().sha));  // no reorder
  // A remembered winner the list no longer has is forgotten, and the race runs.
  net.hosts.clear();
  setup.cached = "gone.cdn.steampipe.steamcontent.com";
  const auto missing = RunFastest(net, root / "miss", setup);
  CHECK(missing.outcome.error == InstallError::kNone && Has(missing.races, "race won winner=akamai#1 "));
  CHECK(missing.remembered == std::vector<std::string>({"", kAkamai}));
}

void RaceWithoutRequestTimingKeepsSteamOrder(const std::filesystem::path& root) {
  auto world = MakeWorld();
  FakeNet net;
  FastestWorld(net, world);
  net.Add(kFastly, 10);
  net.Add(kAkamai, 40);
  world.manifests[441].files = {net.File("pak", 220, kMegabyte)};
  RaceSetup setup{1};
  setup.legacy_chunk_hook = true;
  const auto run = RunFastest(net, root, setup);
  CHECK(run.outcome.error == InstallError::kNone);
  CHECK(Has(run.races, "race timeout winner=none rates=none samples=0 "));
  CHECK(run.remembered.empty() && run.server_name == "Fastly" && net.hosts.back() == kFastly);
}

// A sustained sag starts a re-race on the next large chunks.
void SagReracesToAnotherServer(const std::filesystem::path& root) {
  auto world = MakeWorld();
  FakeNet net;
  FastestWorld(net, world);
  net.Add(kFastly, 10);
  net.specs[kFastly].rate = [](std::int64_t, int request) { return request < 300 ? 10.0 : 1.0; };
  net.Add(kAkamai, 5);
  world.manifests[441].files = {net.File("pak", 700, kMegabyte)};
  const auto run = RunFastest(net, root, {1});
  CHECK(run.outcome.error == InstallError::kNone);
  CHECK(run.races.size() == 2 && Has(run.races, "race won winner=fastly#0 "));
  CHECK(Has(run.races, "rerace won winner=akamai#1 rates=fastly#0:1.0,akamai#1:5.0 "));
  CHECK(run.remembered == std::vector<std::string>({kFastly, kAkamai}));
  CHECK(run.server_name == "Akamai" && net.hosts.back() == kAkamai);
  // Started on a remembered winner: the first judged window is what later windows compare against.
  auto cached_world = MakeWorld();
  FakeNet cached_net;
  FastestWorld(cached_net, cached_world);
  cached_net.Add(kFastly, 10);
  cached_net.specs[kFastly].rate = [](std::int64_t, int request) { return request < 600 ? 10.0 : 1.0; };
  cached_net.Add(kAkamai, 5);
  cached_world.manifests[441].files = {cached_net.File("pak", 700, kMegabyte)};
  RaceSetup setup{1};
  setup.cached = kFastly;
  const auto cached = RunFastest(cached_net, root.string() + "-cached", setup);
  CHECK(cached.outcome.error == InstallError::kNone);
  CHECK(cached.races.size() == 2 && cached.races[0] == "race cached winner=fastly#0");
  CHECK(Has(cached.races, "rerace won winner=akamai#1 "));
}

void SteadyRateBelowTheRaceDoesNotRerace(const std::filesystem::path& root) {
  // In the race each request has the line to itself; afterwards every worker shares it on the winner, so
  // each request gets less. That steady rate is the baseline, not the race's score.
  auto world = MakeWorld();
  FakeNet net;
  FastestWorld(net, world);
  net.Add(kFastly, 0);
  net.specs[kFastly].rate = [](std::int64_t, int request) { return request < 20 ? 40.0 : 6.0; };
  net.Add(kAkamai, 10);
  net.specs[kAkamai].rate = [](std::int64_t, int request) { return request < 20 ? 10.0 : 2.0; };
  world.manifests[441].files = {net.File("pak", 1000, kMegabyte)};
  const auto run = RunFastest(net, root, {1});
  CHECK(run.outcome.error == InstallError::kNone);
  CHECK(run.races.size() == 1 && Has(run.races, "race won winner=fastly#0 "));
  CHECK(net.now_ms > 120000);  // several judged windows
}

void TinyFilesAndSlowWritersDoNotRerace(const std::filesystem::path& root) {
  {
    // After the large file, a long stretch of tiny chunks on a slow server is never judged.
    auto world = MakeWorld();
    FakeNet net;
    FastestWorld(net, world);
    net.Add(kFastly, 10);
    net.specs[kFastly].rate = [](std::int64_t, int request) { return request < 240 ? 10.0 : 0.05; };
    net.Add(kAkamai, 5);
    world.manifests[441].files = {net.File("a-big", 220, kMegabyte), net.File("b-tiny", 400, 100 * 1000)};
    const auto run = RunFastest(net, root / "tiny", {1});
    CHECK(run.outcome.error == InstallError::kNone);
    CHECK(run.races.size() == 1 && Has(run.races, "race won winner=fastly#0 "));
    CHECK(net.now_ms > 10 * 60 * 1000);  // twenty windows of tiny chunks
  }
  {
    // A writer that takes 2 s per chunk: the transfer rate stays, nothing sags.
    auto world = MakeWorld();
    FakeNet net;
    FastestWorld(net, world);
    net.Add(kFastly, 10);
    net.Add(kAkamai, 5);
    world.manifests[441].files = {net.File("pak", 260, kMegabyte)};
    RaceSetup setup{1};
    setup.on_progress = [&net] { net.Advance(2000); };
    const auto run = RunFastest(net, root / "writer", setup);
    CHECK(run.outcome.error == InstallError::kNone);
    CHECK(run.races.size() == 1 && Has(run.races, "race won winner=fastly#0 "));
  }
}

void ReracesStopAtTheLimit(const std::filesystem::path& root) {
  auto world = MakeWorld();
  FakeNet net;
  FastestWorld(net, world);
  // Every server slows down by 2.5 times every 100 s.
  const auto rate = [](std::int64_t now_ms, int) {
    double value = 4.0;
    for (std::int64_t step = 100000; step <= now_ms && step <= 400000; step += 100000) value /= 2.5;
    return value;
  };
  net.Add(kFastly, 0);
  net.Add(kAkamai, 0);
  net.specs[kFastly].rate = rate;
  net.specs[kAkamai].rate = rate;
  world.manifests[441].files = {net.File("pak", 1350, 512 * 1000)};
  const auto run = RunFastest(net, root, {1});
  CHECK(run.outcome.error == InstallError::kNone);
  CHECK(net.now_ms > 460000);
  std::size_t reraces = 0, limits = 0;
  for (const auto& line : run.races) {
    if (line.rfind("rerace won ", 0) == 0) ++reraces;
    if (line.rfind("rerace limit ", 0) == 0) ++limits;
  }
  CHECK(reraces == 2 && limits == 1 && run.races.size() == 4);
}

void QueueNamesTheServerInUse(const std::filesystem::path& root) {
  {
    // Steam's first server refuses everything: the name follows the failover.
    auto world = MakeWorld();
    FakeNet net;
    FastestWorld(net, world);
    net.Add(kFastly, 0);
    net.Add(kAkamai, 10);
    world.manifests[441].files = {net.File("pak", 40, kMegabyte)};
    const auto run = RunFastest(net, root.string() + "-failover", {1});
    CHECK(run.outcome.error == InstallError::kNone && Has(run.races, "race skipped-small "));
    CHECK(run.first_written_name == std::string("Akamai") && run.server_name == "Akamai");
  }
  {
    // A provider without a display name is not shown by its host.
    auto world = MakeWorld();
    FakeNet net;
    FastestWorld(net, world);
    net.Add("cache1.isp.example", 10);
    world.manifests[441].files = {net.File("pak", 40, kMegabyte)};
    const auto run = RunFastest(net, root.string() + "-cache", {1});
    CHECK(run.outcome.error == InstallError::kNone && run.server_name == "Network cache");
  }
}

void FastestWinnerIsRemembered() {
  std::int64_t minutes = 0;
  FastestServerCache cache([&minutes] {
    return std::chrono::steady_clock::time_point(std::chrono::minutes(minutes));
  });
  CHECK(!cache.Get(7));
  cache.Remember(7, kFastly);
  minutes = 44;
  CHECK(cache.Get(7) == std::optional<std::string>(kFastly));
  cache.Remember(7, kAkamai);  // a new winner replaces the old one and starts its time again
  minutes = 88;
  CHECK(cache.Get(7) == std::optional<std::string>(kAkamai));
  minutes = 89;
  CHECK(!cache.Get(7));  // 45 minutes
  CHECK(!cache.Get(7));
  cache.Remember(7, kGoogle);
  CHECK(!cache.Get(8));  // another account
  CHECK(!cache.Get(7));
  cache.Remember(7, kGoogle);
  cache.Remember(7, "");  // a re-race without a winner
  CHECK(!cache.Get(7));
  cache.Remember(7, kGoogle);
  cache.Clear();
  CHECK(!cache.Get(7));
  // Signing out: the runner finds no session and forgets the winner.
  CmClient cm(CmClientHooks{}, cm::DeviceIdentity{});
  CdnClient cdn([](const HttpRequest&, const std::atomic<bool>&) { return HttpResponse{}; });
  struct NoSessions final : SessionRepository {
    Result<std::optional<StoredSession>> Load() const override { return Result<std::optional<StoredSession>>::Success(std::nullopt); }
    Result<void> Save(const StoredSession&) const override { return Result<void>::Success(); }
    Result<void> Clear() const override { return Result<void>::Success(); }
  } sessions;
  SteamInstallRunner runner(sessions, cm, cdn);
  runner.fastest_server().Remember(7, kFastly);
  std::atomic<bool> cancel{false};
  const auto result = runner.Install(GameId::Steam(440), "/nowhere/440", {}, cancel, {}, {});
  CHECK(result.retry_later && !runner.fastest_server().Get(7));
}

void FastestTuningFile(const std::filesystem::path& root) {
  std::filesystem::create_directories(root);
  const auto path = (root / "tuning").string();
  std::ofstream(path) << "slots=6\nserver=fastest\n";
  const auto tuning = ReadDownloadTuning(path);
  CHECK(tuning.slots == 6 && tuning.fastest && tuning.preferred_server.empty());
  std::ofstream(path) << "server=fastest\nserver=fastest\n";
  CHECK(!ReadDownloadTuning(path).fastest);
}

int main(int argc, char** argv) {
  CHECK(argc == 2);
  g_appinfo = argv[1];
  const auto base = std::filesystem::temp_directory_path() / ("playden-installer-test-" + std::to_string(::getpid()));
  std::filesystem::remove_all(base);
  OverlapsFourFetches(base / "parallel");
  OutOfOrderAndBoundedResults(base / "out-of-order");
  EightSlotsAndShape(base / "eight-slots");
  CancelStopsAllWorkers(base / "cancel-fetch", false);
  CancelStopsAllWorkers(base / "cancel-backoff", true);
  WorkerFailureWinsAndJoins(base / "worker-failure");
  InvalidFactoryJoinsStartedWorkers(base / "factory-failure");
  WriteFailureStopsWorkers(base / "write-failure");
  ParallelCorruptRetryAndResume(base / "parallel-resume");
  FileCreationAheadIsBounded(base / "ahead");
  TuningFileDefaultsAndValidation(base / "tuning");
  PreferredServerFailover(base / "preferred");
  ServerLogsAreBoundedAndPrivate();
  EarlyFailuresLogServerTotals(base / "early-logs");
  ExtraServersShareOneBucket(base / "server-buckets");
  CorruptRepliesCountAsServerFailures(base / "corrupt-server");
  ParallelMergedAndFullyResumed(base / "parallel-merged");
  BorrowedDepotRefusedIsSkipped(base / "borrowed-depot");
  DlcAppDepotsAskUnderTheirApp(base / "dlc-app-depot");
  BranchRidesOnDepotRequests(base / "branch-request");
  FreeDlcCandidatesAndMemory(base / "free-dlc");
  UpdateWaitsForUnfetchedDlc();
  CountsActualUsefulCompressedBytes(base / "actual-compressed");
  StorageFailureEndsStats(base / "storage-stats");
  StatsAndRemovalLogs(base / "removal");
  RunStats(base / "stats");
  InstallsMergedDepotsAndWritesRecord(base / "install");
  MeasureMatchesTheMergedInstall(base / "measure");
  LocalRecordNamesItsGame(base / "install-local");
  FileReplacesEmptyFolderFromOtherDepot(base / "file-over-folder");
  ResumesWithoutRefetchingFinishedChunks(base / "resume");
  ChangedManifestRefetchesSameOffset(base / "manifest-update");
  JournalHeaderModes(base / "header-modes");
  HeaderWriteFailureCannotAppend(base / "header-write-failure");
  LongMatchingJournalHeader(base / "long-header");
  UnterminatedJournalGetsSeparator(base / "unterminated-header");
  JournalTraceLogging();
  RefusesWhenSpaceIsShort(base / "space");
  PreservesSpaceMargin(base / "space-margin");
  PreallocatedResumeNeedsOnlyTheMargin(base / "space-preallocated");
  UnknownSpacePreventsDownload(base / "space-unknown");
  DefaultSpaceQueryFailurePreventsDownload(base / "space-default-query");
  CorruptChunksFailAfterRetries(base / "corrupt");
  BusyCdnIsRetriedAfterAWait(base / "busy");
  NoContentServersSaysWhy(base / "no-servers");
  BrokenManifestIsFetchedAgainElsewhere(base / "manifest");
  RemovesOnlyManagedFolders(base);
  UpdateReplacesChangedFiles(base / "update");
  UpdateRestoresFilesOfARemovedDepot(base / "update-removed-depot");
  UpdateWithoutTreeHashesFiles(base / "update-no-tree");
  InterruptedUpdateResumes(base / "update-resume");
  DiscardAndUninstallDuringUpdate(base / "update-discard");
  UpdateFinishIsReentrant(base / "update-reentrant");
  UpdateNeedsSpaceForChangedFilesOnly(base / "update-space");
  UpdateSwapsFilesAndFolders(base / "update-swap");
  UpdateNeedsTheGamesContent(base / "update-dlc-only");
  UpdateWaitsWhileTheGameRuns(base / "update-game-running");
  InstallNeedsTheGamesContent(base / "install-dlc-only");
  UpdateForOtherManifestsRestagesFromScratch(base / "update-retarget");
  UpdateNeverFollowsLinks(base / "update-link");
  UpdateKeepsFilesTheGameWrote(base / "update-untracked");
  FastestTuningFile(base / "fastest-tuning");
  FastestWinnerIsRemembered();
  QueueNamesTheServerInUse(base / "race-name");
  RaceWinsAndLaterChunksUseIt(base / "race-wins");
  TinyChunksSkipTheRace(base / "race-tiny");
  RaceFetchesLargeChunksAtTheEnd(base / "race-at-end");
  SlowWriterAndHandshakesDoNotDecide(base / "race-writer");
  SlowLinkEndsAtTheCap(base / "race-cap");
  FailingServerNeverWins(base / "race-failing");
  ResumeRacesOnlyWhatIsLeft(base / "race-resume");
  CachedWinnerSkipsTheRace(base / "race-cached");
  RaceWithoutRequestTimingKeepsSteamOrder(base / "race-untimed");
  SagReracesToAnotherServer(base / "rerace");
  TinyFilesAndSlowWritersDoNotRerace(base / "rerace-not");
  SteadyRateBelowTheRaceDoesNotRerace(base / "rerace-steady");
  ReracesStopAtTheLimit(base / "rerace-limit");
  std::filesystem::remove_all(base);
  std::puts("depot installer tests passed");
}
