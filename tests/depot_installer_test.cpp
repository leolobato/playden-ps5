#include "steam/backend/depot_chunk.h"
#include "steam/backend/depot_installer.h"
#include "steam/backend/steam_install_runner.h"

#include <mbedtls/aes.h>
#include <mbedtls/sha1.h>

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
    for (const auto& piece : pieces) {
      std::vector<std::uint8_t> plain(piece.begin(), piece.end());
      file.chunks.push_back(AddChunk(plain, offset));
      offset += plain.size();
    }
    file.size = offset;
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

void TuningFileDefaultsAndValidation(const std::filesystem::path& root) {
  std::filesystem::create_directories(root);
  const auto path = (root / "tuning").string();
  auto tuning = ReadDownloadTuning(path);
  CHECK(tuning.slots == 4 && tuning.ahead_files == 256);
  std::ofstream(path) << "slots=8\nahead=12\n";
  tuning = ReadDownloadTuning(path);
  CHECK(tuning.slots == 8 && tuning.ahead_files == 12);
  std::ofstream(path) << "slots=99\n";
  CHECK(ReadDownloadTuning(path).slots == 8);
  std::ofstream(path) << "slots=0\n";
  CHECK(ReadDownloadTuning(path).slots == 1);
  for (const auto text : {"slots=8\nahead=-1\n", "slots=abc\n", "slots=8garbage\n", "unknown=1\n", "slots=8\nslots=1\n"}) {
    std::ofstream(path) << text;
    tuning = ReadDownloadTuning(path);
    CHECK(tuning.slots == 4 && tuning.ahead_files == 256);
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
  DepotInstaller installer(world.Hooks());
  installer.SetTuning({1, 256});
  std::atomic<bool> cancel{false};
  InstallProgress last;
  const auto outcome = installer.Run({}, Target(root), cancel, [&](const InstallProgress& progress) { last = progress; });
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
  // A Steam record is as it always was: no store, no game.
  CHECK(Read(root / DepotInstaller::kInstallRecord).rfind("{\"version\":1,\"app_id\":440,\"install_bytes\":", 0) == 0);
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
            "{\"version\":1,\"app_id\":4026531840,\"store\":\"local\",\"game\":\"0f8fad5b-d9cb-469f-a165-70867728950e\","
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
  CHECK(chmod((managed / "locked").c_str(), 0700) == 0);
  CHECK(RemoveManagedInstall(managed.string()));
  CHECK(!std::filesystem::exists(managed));
}

}  // namespace

// The world after a release: game.exe's second chunk changed, new.txt was added, empty.txt dropped.
void Release(World& world) {
  auto& base = world.manifests[441];
  base.gid = 21;
  base.files.clear();
  base.files.push_back(world.AddFile("game.exe", {"MZ-executable-", "second-CHANGE"}));
  ManifestFile folder;
  folder.path = "data";
  folder.flags = kDepotFileFlagDirectory;
  base.files.push_back(folder);
  base.files.push_back(world.AddFile("data/level.dat", {"level-one"}));
  base.files.push_back(world.AddFile("data/new.txt", {"brand-new"}));
  world.manifests[442].gid = 22;
}

InstallTarget UpdateTarget(const std::filesystem::path& root) {
  auto target = Target(root);
  target.depots[0].manifest_gid = 21;
  target.depots[1].manifest_gid = 22;
  target.update = true;
  return target;
}

void UpdateDownloadsOnlyWhatChanged(const std::filesystem::path& root) {
  auto world = MakeWorld();
  DepotInstaller installer(world.Hooks());
  installer.SetTuning({1, 256});
  std::atomic<bool> cancel{false};
  CHECK(installer.Run({}, Target(root), cancel).error == InstallError::kNone);
  CHECK((RecordedDepots(root.string()) == std::vector<std::pair<std::uint32_t, std::uint64_t>>{{441, 11}, {442, 12}}));
  Release(world);
  world.chunk_calls = 0;
  InstallProgress last;
  bool checked = false;
  const auto outcome = installer.Run({}, UpdateTarget(root), cancel, [&](const InstallProgress& progress) {
    checked = checked || progress.checking;
    last = progress;
  });
  CHECK(outcome.error == InstallError::kNone);
  CHECK(checked);
  CHECK(world.chunk_calls == 2);  // game.exe's changed chunk and new.txt; the rest was on disk
  CHECK(Read(root / "game.exe") == "MZ-executable-second-CHANGE");
  CHECK(Read(root / "data/level.dat") == "level-patched");
  CHECK(Read(root / "data/new.txt") == "brand-new");
  CHECK(!std::filesystem::exists(root / "empty.txt"));
  CHECK(!std::filesystem::exists(root / DepotInstaller::kUpdateDir));
  CHECK((RecordedDepots(root.string()) == std::vector<std::pair<std::uint32_t, std::uint64_t>>{{441, 21}, {442, 22}}));
  const auto list = Read(root / DepotInstaller::kFileList);
  CHECK(list.find("f\tdata/new.txt") != std::string::npos);
  CHECK(list.find("empty.txt") == std::string::npos);
  CHECK(outcome.progress.install_total == std::filesystem::file_size(root / "game.exe") + 13 + 9);
  CHECK(last.download_done == last.download_total);

  // Nothing changed: the files are only read.
  world.chunk_calls = 0;
  CHECK(installer.Run({}, UpdateTarget(root), cancel).error == InstallError::kNone);
  CHECK(world.chunk_calls == 0);
  CHECK(Read(root / "game.exe") == "MZ-executable-second-CHANGE");
}

// A canceled update leaves the installed version as it was; Resume carries on, Discard drops it.
void CanceledUpdateKeepsTheInstall(const std::filesystem::path& root) {
  auto world = MakeWorld();
  DepotInstaller installer(world.Hooks());
  installer.SetTuning({1, 256});
  std::atomic<bool> cancel{false};
  CHECK(installer.Run({}, Target(root), cancel).error == InstallError::kNone);
  Release(world);
  world.chunk_calls = 0;
  world.cancel = &cancel;
  world.cancel_after = 1;
  CHECK(installer.Run({}, UpdateTarget(root), cancel).error == InstallError::kCanceled);
  CHECK(Read(root / "game.exe") == "MZ-executable-second-chunk");
  CHECK(std::filesystem::exists(root / "empty.txt"));
  CHECK((RecordedDepots(root.string()) == std::vector<std::pair<std::uint32_t, std::uint64_t>>{{441, 11}, {442, 12}}));
  CHECK(std::filesystem::exists(root / DepotInstaller::kUpdateDir));

  // Resumed: the chunk downloaded before the cancel is not fetched again.
  cancel = false;
  world.cancel_after = -1;
  world.chunk_calls = 0;
  CHECK(installer.Run({}, UpdateTarget(root), cancel).error == InstallError::kNone);
  CHECK(world.chunk_calls == 1);
  CHECK(Read(root / "game.exe") == "MZ-executable-second-CHANGE");

  // Discarded: the staging folder goes, the game stays.
  std::filesystem::create_directories(root / DepotInstaller::kUpdateDir);
  std::ofstream(root / DepotInstaller::kUpdateDir / "f0") << "partial";
  CHECK(DiscardUpdate(root.string()));
  CHECK(!std::filesystem::exists(root / DepotInstaller::kUpdateDir));
  CHECK(Read(root / "game.exe") == "MZ-executable-second-CHANGE");
}

// An update that stopped while its files were swapped in finishes without reaching Steam.
void InterruptedCommitFinishes(const std::filesystem::path& root) {
  auto world = MakeWorld();
  DepotInstaller installer(world.Hooks());
  installer.SetTuning({1, 256});
  std::atomic<bool> cancel{false};
  CHECK(installer.Run({}, Target(root), cancel).error == InstallError::kNone);
  const auto staging = root / DepotInstaller::kUpdateDir;
  std::filesystem::create_directories(staging);
  std::ofstream(staging / "f3") << "new-exe";
  std::ofstream(staging / "record") << "{\"version\":1,\"app_id\":440,\"install_bytes\":7,\"files\":1,\"depots\":[{\"id\":441,\"manifest\":\"31\"}]}\n";
  std::ofstream(staging / "files") << "f\tgame.exe\n";
  std::ofstream(staging / "commit") << "#commit 7 1\nd\tdata\nm\tf3\tgame.exe\nm\tf9\tgone.bin\nr\tempty.txt\nr\tdata/level.dat\nx\tdata\nr\t../outside\n";
  // As it stopped after the record was deleted.
  std::filesystem::remove(root / DepotInstaller::kInstallRecord);
  bool reached = false;
  auto hooks = world.Hooks();
  hooks.access = [&](const StoredSession&, std::uint32_t, const std::vector<DepotRequest>&, const std::atomic<bool>&) {
    reached = true;
    return DepotAccessResult{};
  };
  DepotInstaller finishing(hooks);
  const auto outcome = finishing.Run({}, UpdateTarget(root), cancel);
  CHECK(outcome.error == InstallError::kNone);
  CHECK(!reached);
  CHECK(outcome.progress.install_total == 7);
  CHECK(Read(root / "game.exe") == "new-exe");
  CHECK(!std::filesystem::exists(root / "empty.txt"));
  CHECK(!std::filesystem::exists(root / "data"));
  CHECK(!std::filesystem::exists(staging));
  CHECK((RecordedDepots(root.string()) == std::vector<std::pair<std::uint32_t, std::uint64_t>>{{441, 31}}));
  CHECK(Read(root / DepotInstaller::kFileList) == "f\tgame.exe\n");
  // While it swaps files in it can't be discarded.
  std::filesystem::create_directories(staging);
  std::ofstream(staging / "commit") << "#commit 7 1\n";
  CHECK(!DiscardUpdate(root.string()));
  CHECK(std::filesystem::exists(staging / "commit"));
  // A folder with only an update's staging is still Playden's to remove.
  std::filesystem::remove(root / DepotInstaller::kInstallRecord);
  std::filesystem::remove(root / DepotInstaller::kFileList);
  CHECK(RemoveManagedInstall(root.string()));
  CHECK(!std::filesystem::exists(root));
}

int main() {
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
  ParallelMergedAndFullyResumed(base / "parallel-merged");
  BorrowedDepotRefusedIsSkipped(base / "borrowed-depot");
  CountsActualUsefulCompressedBytes(base / "actual-compressed");
  StorageFailureEndsStats(base / "storage-stats");
  StatsAndRemovalLogs(base / "removal");
  RunStats(base / "stats");
  InstallsMergedDepotsAndWritesRecord(base / "install");
  LocalRecordNamesItsGame(base / "install-local");
  ResumesWithoutRefetchingFinishedChunks(base / "resume");
  ChangedManifestRefetchesSameOffset(base / "manifest-update");
  UpdateDownloadsOnlyWhatChanged(base / "update");
  CanceledUpdateKeepsTheInstall(base / "update-cancel");
  InterruptedCommitFinishes(base / "update-commit");
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
  std::filesystem::remove_all(base);
  std::puts("depot installer tests passed");
}
