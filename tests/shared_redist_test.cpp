// SPDX-License-Identifier: GPL-3.0-or-later
// Steam's shared redistributables (app 228980) kept once per console: the real DepotInstaller behind canned Steam
// answers.
#include "steam/backend/depot_chunk.h"
#include "steam/backend/shared_redist.h"

#include <mbedtls/aes.h>
#include <mbedtls/sha1.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

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

constexpr std::uint32_t kDepot = 228983;
constexpr std::uint64_t kManifest = 8124929965194586177ull;

const std::array<std::uint8_t, 32> kKey = [] {
  std::array<std::uint8_t, 32> key{};
  for (std::size_t index = 0; index < key.size(); ++index) key[index] = static_cast<std::uint8_t>(index * 5 + 3);
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
  for (std::size_t index = 0; index < iv.size(); ++index) iv[index] = static_cast<std::uint8_t>(0xb0 + index);
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

std::string Read(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream text;
  text << in.rdbuf();
  return text.str();
}

// 228980's appinfo as Steam sends it, with the depot's public manifest of `size` bytes.
std::string AppInfo(std::uint64_t size) {
  return "\"appinfo\"\n{\n\"appid\" \"228980\"\n\"common\"\n{\n\"name\" \"Steamworks Common Redistributables\"\n"
         "\"type\" \"Tool\"\n}\n\"depots\"\n{\n\"" + std::to_string(kDepot) +
         "\"\n{\n\"config\"\n{\n\"oslist\" \"windows\"\n}\n\"manifests\"\n{\n\"public\"\n{\n\"gid\" \"" +
         std::to_string(kManifest) + "\"\n\"size\" \"" + std::to_string(size) + "\"\n\"download\" \"" +
         std::to_string(size) + "\"\n}\n}\n}\n}\n}\n";
}

struct Steam {
  std::map<std::string, std::vector<std::uint8_t>> served;
  DepotManifest manifest;
  std::vector<std::pair<std::uint32_t, DepotRequest>> asked;  // the call's app, the request
  bool refuse = false;
  int app_info_calls = 0, chunk_calls = 0;
  std::uint64_t listed_size = 0;

  void AddFile(const std::string& path, const std::string& text) {
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
    manifest.depot_id = kDepot;
    manifest.gid = kManifest;
    manifest.files.push_back(file);
    std::sort(manifest.files.begin(), manifest.files.end(),
              [](const ManifestFile& a, const ManifestFile& b) { return a.path < b.path; });
    listed_size += file.size;
  }

  SharedRedistCache::AppInfoFetch AppInfoHook() {
    return [this](const StoredSession&, const std::vector<std::uint32_t>& apps, const std::atomic<bool>&) {
      ++app_info_calls;
      AppInfoBuffers out;
      out.error = CmError::kNone;
      CHECK(apps == std::vector<std::uint32_t>{kCommonRedistApp});
      out.buffers[kCommonRedistApp] = AppInfo(listed_size);
      return out;
    };
  }

  InstallerHooks Hooks() {
    InstallerHooks hooks;
    hooks.access = [this](const StoredSession&, std::uint32_t app, const std::vector<DepotRequest>& depots,
                          const std::atomic<bool>&) {
      DepotAccessResult result;
      for (const auto& depot : depots) asked.emplace_back(app, depot);
      if (refuse) {
        result.error = CmError::kRejected;
        result.detail = "depot key refused";
        return result;
      }
      result.error = CmError::kNone;
      for (const auto& depot : depots) result.grants.push_back({depot.depot_id, depot.manifest_gid, kKey, 77});
      return result;
    };
    hooks.servers = [](std::uint32_t, const std::atomic<bool>&, std::vector<ContentServer>* out) {
      *out = {{"a.example.invalid", "a.example.invalid", 1}};
      return CdnError::kNone;
    };
    hooks.manifest = [this](const ContentServer&, const DepotGrant& grant, const std::atomic<bool>&, DepotManifest* out) {
      CHECK(grant.depot_id == kDepot && grant.manifest_gid == kManifest);
      *out = manifest;
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

fs::path TempRoot() {
  char pattern[] = "/tmp/playden-shared-redist-XXXXXX";
  CHECK(::mkdtemp(pattern) != nullptr);
  return fs::canonical(pattern);
}

StoredSession Session() {
  StoredSession session;
  session.account_name = "player";
  session.steam_id = 76561197960287930ull;
  session.refresh_token = "token";
  return session;
}

void DownloadsOnceThenKeeps() {
  const auto root = TempRoot();
  const auto cache = (root / "data/playden/redist/steam").string();  // not there yet
  Steam steam;
  steam.AddFile("_CommonRedist/vcredist/2010/vcredist_x86.exe", "MZ-x86");
  steam.AddFile("_CommonRedist/vcredist/2010/vcredist_x64.exe", "MZ-x64");
  DepotInstaller installer(steam.Hooks());
  SharedRedistCache redist(steam.AppInfoHook(), installer);
  std::atomic<bool> cancel{false};
  const auto session = Session();

  // Nothing kept, and this call doesn't download: nothing is asked of Steam.
  auto result = redist.Get(&session, kDepot, cache, false, cancel);
  CHECK(result.status == SharedDepotResult::Status::kNotCached && steam.app_info_calls == 0 && steam.asked.empty());
  CHECK(CachedSharedDepot(cache, kDepot) == std::nullopt);
  // Without a session it can't download.
  result = redist.Get(nullptr, kDepot, cache, true, cancel);
  CHECK(result.status == SharedDepotResult::Status::kUnavailable && steam.app_info_calls == 0);

  // Downloaded under app 228980, its public manifest, into <cache>/<depot>.
  result = redist.Get(&session, kDepot, cache, true, cancel);
  if (result.status != SharedDepotResult::Status::kReady) std::fprintf(stderr, "download: %s\n", result.detail.c_str());
  CHECK(result.status == SharedDepotResult::Status::kReady && result.downloaded);
  CHECK(result.folder == cache + "/" + std::to_string(kDepot));
  CHECK(Read(fs::path(result.folder) / "_CommonRedist/vcredist/2010/vcredist_x86.exe") == "MZ-x86");
  CHECK(steam.asked.size() == 1 && steam.asked[0].first == kCommonRedistApp && steam.asked[0].second.depot_id == kDepot &&
        steam.asked[0].second.manifest_gid == kManifest && !steam.asked[0].second.optional);
  CHECK(CachedSharedDepot(cache, kDepot) == result.folder);
  const int chunks = steam.chunk_calls;
  CHECK(chunks == 2);

  // Kept: no Steam at all the next time, download or not, signed in or not.
  for (const bool download : {false, true}) {
    result = redist.Get(nullptr, kDepot, cache, download, cancel);
    CHECK(result.status == SharedDepotResult::Status::kReady && !result.downloaded && result.folder == cache + "/228983");
  }
  CHECK(steam.app_info_calls == 1 && steam.asked.size() == 1 && steam.chunk_calls == chunks);
  // Another depot's record in that folder isn't the copy.
  CHECK(!CachedSharedDepot(cache, 228984));
  fs::remove_all(root);
}

// A file of the kept copy is the one the depot lists: by name, with its size and SHA-1. A changed or missing one is
// downloaded again; a file the depot doesn't list is never given.
void KeptFileIsChecked() {
  const auto root = TempRoot();
  const auto cache = (root / "cache").string();
  Steam steam;
  steam.AddFile("_CommonRedist/vcredist/2010/vcredist_x86.exe", "MZ-x86");
  steam.AddFile("_CommonRedist/vcredist/2010/installscript.vdf", "script");
  DepotInstaller installer(steam.Hooks());
  SharedRedistCache redist(steam.AppInfoHook(), installer);
  std::atomic<bool> cancel{false};
  const auto session = Session();
  auto result = redist.Get(&session, kDepot, cache, true, cancel, "VCREDIST_X86.EXE");
  CHECK(result.status == SharedDepotResult::Status::kReady && result.downloaded);
  const auto program = cache + "/228983/_CommonRedist/vcredist/2010/vcredist_x86.exe";
  CHECK(result.file == program);
  const int chunks = steam.chunk_calls;
  // Kept and unchanged: no Steam.
  result = redist.Get(nullptr, kDepot, cache, false, cancel, "vcredist_x86.exe");
  CHECK(result.status == SharedDepotResult::Status::kReady && !result.downloaded && result.file == program);
  // A file the depot doesn't list, even one placed beside the listed ones, is not given.
  { std::ofstream(cache + "/228983/_CommonRedist/vcredist/2010/vcredist_x64.exe") << "MZ-planted"; }
  result = redist.Get(nullptr, kDepot, cache, false, cancel, "vcredist_x64.exe");
  CHECK(result.status == SharedDepotResult::Status::kNotCached && result.file.empty());
  // Changed (same size): not given from the kept copy, and downloaded again when this call may.
  { std::ofstream(program, std::ios::trunc) << "MZ-x87"; }
  result = redist.Get(nullptr, kDepot, cache, false, cancel, "vcredist_x86.exe");
  CHECK(result.status == SharedDepotResult::Status::kNotCached &&
        result.detail.find("not the file the depot lists") != std::string::npos);
  result = redist.Get(&session, kDepot, cache, true, cancel, "vcredist_x86.exe");
  CHECK(result.status == SharedDepotResult::Status::kReady && result.downloaded && Read(program) == "MZ-x86");
  CHECK(steam.chunk_calls > chunks);
  // Missing: the same.
  fs::remove(program);
  result = redist.Get(&session, kDepot, cache, true, cancel, "vcredist_x86.exe");
  CHECK(result.status == SharedDepotResult::Status::kReady && Read(program) == "MZ-x86");
  // A name the depot doesn't have fails after the download, with nothing to give.
  result = redist.Get(&session, kDepot, cache, true, cancel, "vc_redist.x86.exe");
  CHECK(result.status != SharedDepotResult::Status::kReady && result.file.empty());
  fs::remove_all(root);
}

void RefusedTooLargeOrUnlisted() {
  const auto root = TempRoot();
  const auto cache = (root / "cache").string();
  std::atomic<bool> cancel{false};
  const auto session = Session();
  {  // Steam refuses the key: nothing kept, and the caller is told it was refused.
    Steam steam;
    steam.AddFile("_CommonRedist/vcredist/2010/vcredist_x86.exe", "MZ-x86");
    steam.refuse = true;
    DepotInstaller installer(steam.Hooks());
    SharedRedistCache redist(steam.AppInfoHook(), installer);
    const auto result = redist.Get(&session, kDepot, cache, true, cancel);
    CHECK(result.status == SharedDepotResult::Status::kRefused && !CachedSharedDepot(cache, kDepot));
    CHECK(steam.chunk_calls == 0);
  }
  {  // Larger than the cache takes (.NET's depots): not downloaded at all.
    Steam steam;
    steam.AddFile("_CommonRedist/DotNet/4.8/ndp48-x86-x64-allos-enu.exe", "MZ-net");
    steam.listed_size = kMaxSharedDepotBytes + 1;
    DepotInstaller installer(steam.Hooks());
    SharedRedistCache redist(steam.AppInfoHook(), installer);
    const auto result = redist.Get(&session, kDepot, cache, true, cancel);
    CHECK(result.status == SharedDepotResult::Status::kFailed && steam.asked.empty() && steam.chunk_calls == 0);
  }
  {  // A manifest larger than appinfo said is refused before its chunks.
    Steam steam;
    steam.AddFile("big.exe", "MZ");
    steam.manifest.files[0].size = kMaxSharedDepotBytes + 1;
    steam.listed_size = 2;
    DepotInstaller installer(steam.Hooks());
    SharedRedistCache redist(steam.AppInfoHook(), installer);
    const auto result = redist.Get(&session, kDepot, cache, true, cancel);
    CHECK(result.status == SharedDepotResult::Status::kFailed && steam.chunk_calls == 0 && !CachedSharedDepot(cache, kDepot));
  }
  {  // A depot 228980 doesn't list.
    Steam steam;
    steam.AddFile("a.exe", "MZ");
    DepotInstaller installer(steam.Hooks());
    SharedRedistCache redist(steam.AppInfoHook(), installer);
    const auto result = redist.Get(&session, 228999, cache, true, cancel);
    CHECK(result.status == SharedDepotResult::Status::kFailed && result.detail == "depot not listed" && steam.asked.empty());
  }
  {  // Steam can't be reached: try again later.
    DepotInstaller installer(InstallerHooks{});
    SharedRedistCache redist(
        [](const StoredSession&, const std::vector<std::uint32_t>&, const std::atomic<bool>&) {
          AppInfoBuffers out;
          out.error = CmError::kNetwork;
          return out;
        },
        installer);
    CHECK(redist.Get(&session, kDepot, cache, true, cancel).status == SharedDepotResult::Status::kUnavailable);
  }
  // A cache folder Playden doesn't accept.
  {
    DepotInstaller installer(InstallerHooks{});
    SharedRedistCache redist({}, installer);
    CHECK(redist.Get(&session, kDepot, "relative/cache", true, cancel).status == SharedDepotResult::Status::kFailed);
    CHECK(redist.Get(&session, kDepot, cache + "/../escape", true, cancel).status == SharedDepotResult::Status::kFailed);
  }
  fs::remove_all(root);
}

}  // namespace

int main() {
  DownloadsOnceThenKeeps();
  KeptFileIsChecked();
  RefusedTooLargeOrUnlisted();
  if (failures) std::fprintf(stderr, "%d failure(s)\n", failures);
  return failures ? 1 : 0;
}
