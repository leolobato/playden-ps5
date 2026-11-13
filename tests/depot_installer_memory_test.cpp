// SPDX-License-Identifier: GPL-3.0-or-later
// A large game's install stays within the console's heap (#667, #742). On the console Playden allocates from one
// 128 MiB heap (ps5-opengl's app_heap.c), about 35 MiB of it in use before an install starts; past it, malloc returns
// null after a few MiB more and operator new aborts the title. This test counts what DepotInstaller::Run allocates
// for a game shaped like DCS World (36,238 files, 278,715 chunks) and like Solo Leveling: ARISE OVERDRIVE resumed with
// every chunk already written (59,185 files, 97,621 chunks), and checks the peak and the largest single block.
#include "steam/backend/depot_installer.h"

#include <mbedtls/aes.h>
#include <mbedtls/sha1.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <mutex>
#include <new>
#include <string>
#include <unistd.h>
#include <vector>

using namespace playden;
using namespace playden::steam;

namespace {
std::atomic<bool> g_counting{false};
std::atomic<std::size_t> g_live{0}, g_peak{0}, g_largest{0};
std::mutex g_phase_mutex;
std::string g_phase = "start";
struct PhaseStats { std::size_t largest = 0, peak = 0; };
std::map<std::string, PhaseStats>* g_phases = nullptr;

void Note(std::size_t size) {
  const auto live = g_live.fetch_add(size) + size;
  auto peak = g_peak.load();
  while (live > peak && !g_peak.compare_exchange_weak(peak, live)) {}
  auto largest = g_largest.load();
  while (size > largest && !g_largest.compare_exchange_weak(largest, size)) {}
}
}  // namespace

void* operator new(std::size_t size) {
  auto* base = static_cast<char*>(std::malloc(size + 16));
  if (!base) std::abort();
  *reinterpret_cast<std::size_t*>(base) = size;
  const bool counted = g_counting.load();
  *reinterpret_cast<std::size_t*>(base + 8) = counted ? 0 : 1;
  if (counted) Note(size);
  return base + 16;
}
void operator delete(void* address) noexcept {
  if (!address) return;
  auto* base = static_cast<char*>(address) - 16;
  if (*reinterpret_cast<std::size_t*>(base + 8) == 0) g_live.fetch_sub(*reinterpret_cast<std::size_t*>(base));
  std::free(base);
}
void* operator new[](std::size_t size) { return operator new(size); }
void operator delete[](void* address) noexcept { operator delete(address); }
void operator delete(void* address, std::size_t) noexcept { operator delete(address); }
void operator delete[](void* address, std::size_t) noexcept { operator delete(address); }

namespace {
const std::array<std::uint8_t, 32> kKey{};

void Le16(std::vector<std::uint8_t>& out, std::uint32_t value) {
  out.push_back(static_cast<std::uint8_t>(value));
  out.push_back(static_cast<std::uint8_t>(value >> 8));
}
void Le32(std::vector<std::uint8_t>& out, std::uint32_t value) { Le16(out, value & 0xffff); Le16(out, value >> 16); }
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

#define CHECK(condition)                                                                    \
  do {                                                                                      \
    if (!(condition)) {                                                                     \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
      std::exit(1);                                                                         \
    }                                                                                       \
  } while (false)

// Before #667's fix: 104 MiB peak and a 20 MiB block (DCS), 80 MiB and 11.5 MiB (resumed Solo Leveling).
// After it: about 40 and 45 MiB, mostly the manifests themselves, with no block over 4.25 MiB.
constexpr std::size_t kPeakBudget = 56u << 20;
constexpr std::size_t kLargestBudget = 6u << 20;

void Measure(const char* name, const std::filesystem::path& root, std::size_t files, std::size_t chunks, bool resume) {
  const std::vector<std::uint8_t> plain{'x'};
  std::array<std::uint8_t, 20> sha{};
  mbedtls_sha1(plain.data(), plain.size(), sha.data());
  const auto container = Encrypt(StoredZip(plain));
  DepotManifest manifest;
  manifest.depot_id = 2373991;
  manifest.gid = 77;
  manifest.files.reserve(files);
  std::size_t given = 0;
  for (std::size_t index = 0; index < files; ++index) {
    ManifestFile file;
    char name_text[64];
    std::snprintf(name_text, sizeof(name_text), "%08zu.bundle", index);
    file.path = "Game_Data/StreamingAssets/aa/StandaloneWindows64/folder" + std::to_string(index / 200) +
                "/defaultlocalgroup_assets_all_0123456789abcdef0123456789abcdef_" + name_text;
    const std::size_t count = chunks / files + (index < chunks % files ? 1 : 0);
    for (std::size_t piece = 0; piece < count; ++piece) {
      ManifestChunk chunk;
      chunk.sha = sha;
      chunk.offset = piece;
      chunk.original_size = 1;
      chunk.compressed_size = static_cast<std::uint32_t>(container.size());
      file.chunks.push_back(chunk);
    }
    given += count;
    file.size = count;
    file.sha_content = sha;
    manifest.files.push_back(std::move(file));
  }
  InstallerHooks hooks;
  hooks.access = [](const StoredSession&, std::uint32_t, const std::vector<DepotRequest>& depots, const std::atomic<bool>&) {
    DepotAccessResult result;
    result.error = CmError::kNone;
    for (const auto& depot : depots) result.grants.push_back({depot.depot_id, depot.manifest_gid, kKey, 77});
    return result;
  };
  hooks.servers = [](std::uint32_t, const std::atomic<bool>&, std::vector<ContentServer>* out) {
    *out = {{"a.example.invalid", "a.example.invalid", 1}};
    return CdnError::kNone;
  };
  hooks.manifest = [&](const ContentServer&, const DepotGrant&, const std::atomic<bool>&, DepotManifest* out) {
    *out = manifest;
    return InstallError::kNone;
  };
  std::atomic<std::size_t> fetched{0};
  hooks.chunk = [&](const ContentServer&, std::uint32_t, const std::string&, std::size_t, const std::atomic<bool>&,
                    std::vector<std::uint8_t>* out) {
    ++fetched;
    *out = container;
    return CdnError::kNone;
  };
  hooks.free_bytes = [](const std::string&) { return 1ull << 50; };
  std::map<std::string, PhaseStats> phases;
  g_phases = &phases;
  const auto close_phase = [&] {
    std::lock_guard<std::mutex> lock(g_phase_mutex);
    auto& stats = phases[g_phase];
    stats.largest = std::max(stats.largest, g_largest.load());
    stats.peak = std::max(stats.peak, g_peak.load());
    g_largest = 0;
    g_peak = g_live.load();
  };
  hooks.trace = [&](const char* step) { close_phase(); std::lock_guard<std::mutex> lock(g_phase_mutex); g_phase = step; };
  InstallTarget target;
  target.app_id = 2373990;
  target.install_root = root.string();
  DepotInfo depot;
  depot.id = 2373991;
  depot.manifest_gid = 77;
  target.depots = {depot};
  target.before_apply = [&](const std::string&, const std::vector<std::uint32_t>&) {
    close_phase(); std::lock_guard<std::mutex> lock(g_phase_mutex); g_phase = "finish"; return true;
  };
  DepotInstaller installer(hooks);
  std::atomic<bool> cancel{false};
  if (resume) {
    auto outcome = installer.Run({}, target, cancel);
    CHECK(outcome.error == InstallError::kNone);
    // Put the journal back as a finished download leaves it, and drop the record.
    std::filesystem::remove(root / ".playden-install.json");
    std::filesystem::create_directories(root / ".playden-download");
    FILE* journal = std::fopen((root / ".playden-download/journal").c_str(), "w");
    std::fprintf(journal, "#v2 2373991:77\n");
    for (const auto& file : manifest.files)
      for (const auto& chunk : file.chunks)
        std::fprintf(journal, "2373991\t%llu\t%s\n", static_cast<unsigned long long>(chunk.offset), file.path.c_str());
    std::fclose(journal);
  }
  fetched = 0;
  g_phase = "start";
  g_live = 0; g_peak = 0; g_largest = 0;
  g_counting = true;
  const auto outcome = installer.Run({}, target, cancel);
  close_phase();
  g_counting = false;
  std::printf("%s files=%zu chunks=%zu resume=%d error=%s\n", name, files, given, resume ? 1 : 0,
              InstallErrorName(outcome.error));
  std::size_t peak = 0, largest = 0;
  for (const auto& [phase, stats] : phases) {
    std::printf("  %-12s largest=%8.2f MiB peak=%8.2f MiB\n", phase.c_str(), stats.largest / 1048576.0,
                stats.peak / 1048576.0);
    peak = std::max(peak, stats.peak);
    largest = std::max(largest, stats.largest);
  }
  CHECK(outcome.error == InstallError::kNone);
  CHECK(peak <= kPeakBudget);
  CHECK(largest <= kLargestBudget);
  // The install is complete: its record, and a tree that lists every file.
  CHECK(std::filesystem::exists(root / DepotInstaller::kInstallRecord));
  CHECK(!std::filesystem::exists(root / DepotInstaller::kJournalDir));
  FILE* tree = std::fopen((root / DepotInstaller::kFileTree).c_str(), "r");
  CHECK(tree);
  std::size_t lines = 0;
  for (int c = std::fgetc(tree); c != EOF; c = std::fgetc(tree)) lines += c == '\n';
  std::fclose(tree);
  CHECK(lines == files + 1);
  if (resume) CHECK(fetched == 0);  // every chunk came from the journal
  std::filesystem::remove_all(root);
}
}  // namespace

int main(int argc, char** argv) {
  const auto base = std::filesystem::temp_directory_path() / ("playden-installer-memory-" + std::to_string(::getpid()));
  std::filesystem::remove_all(base);
  const std::string which = argc > 1 ? argv[1] : "all";
  if (which == "all" || which == "dcs") Measure("dcs", base / "b", 36238, 278715, false);
  if (which == "solo") Measure("solo", base / "c", 59185, 97621, false);
  if (which == "all" || which == "solo-resume") Measure("solo-resume", base / "d", 59185, 97621, true);
  std::filesystem::remove_all(base);
  return 0;
}
