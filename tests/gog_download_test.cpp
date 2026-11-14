// SPDX-License-Identifier: GPL-3.0-or-later
// The GOG downloader (gog/chunk_fetch, gog/installer) against a stub CDN (gog_cdn_stub.h): synthetic gen 2 and gen 1
// games (gog_synthetic_game.h: random files, chunks compressed with miniz or ranges of a main.bin, their manifests,
// meta or repository, and builds), plus GOG's public dependency chunk and open link answers from fixtures/gog. Usage: gog_download_test <fixtures/gog> <scratch folder>.

#include "gog/api.h"
#include "gog/chunk_fetch.h"
#include "gog/content.h"
#include "gog/dependencies.h"
#include "gog/installer.h"
#include "gog_cdn_stub.h"
#include "gog_synthetic_game.h"
#include "steam/backend/depot_installer.h"
#include "steam/install_volumes.h"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

using namespace gog_test;
using playden::gog::ChunkResult;
using playden::gog::DepotChunk;
using playden::gog::GogInstaller;
using Error = GogInstaller::Outcome::Error;
namespace fs = std::filesystem;

std::string g_fixtures;
std::string g_scratch;

constexpr std::int64_t kNow = 1800000000;  // well before the fixture link's expires_at (1900000000)

std::optional<std::string> Contents(const std::string& path) {
  FILE* file = std::fopen(path.c_str(), "rb");
  if (!file) return std::nullopt;
  std::fclose(file);
  return ReadFile(path);
}

bool IsDirectory(const std::string& path) {
  struct stat info {};
  return lstat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

bool Exists(const std::string& path) {
  struct stat info {};
  return lstat(path.c_str(), &info) == 0;
}

std::vector<std::string> Names(const std::string& folder) {
  std::vector<std::string> names;
  for (const auto& entry : fs::directory_iterator(folder)) names.push_back(entry.path().filename().string());
  std::sort(names.begin(), names.end());
  return names;
}

std::vector<std::string> Lines(const std::string& text) {
  std::vector<std::string> lines;
  std::size_t start = 0;
  while (start < text.size()) {
    auto end = text.find('\n', start);
    if (end == std::string::npos) end = text.size();
    lines.push_back(text.substr(start, end - start));
    start = end + 1;
  }
  return lines;
}

const std::string kGameExe = RandomBytes(200 * 1024, 1);
const std::string kBig = RandomBytes(100 * 1024 + 100 * 1024 + 77 * 1024, 2);
const std::string kTheme = RandomBytes(50 * 1024, 3);
const std::string kExtra = RandomBytes(30 * 1024, 4);
const std::string kDosbox = RandomBytes(40 * 1024, 5);
const std::string kBaseShared = "shared text from the base game\n";
const std::string kDlcShared = "shared text from the DLC, which wins\n";
const std::string kDosboxConf = "[dosbox]\nmemsize=16\n";
const std::string kSave = "slot 1\n";
const std::string kEnglishInfo = "{\"language\":\"English\"}\n";
const std::string kGermanInfo = "{\"language\":\"Deutsch\"}\n";

// Base: a `\` depot in two languages, a `/` depot for every language, an English and a German gog depot; the DLC
// replaces data\shared.txt with its own spelling.
Game SyntheticGame(bool huge = false) {
  Game game;
  GameDepot main;
  main.languages = {"en-US", "de-DE"};
  main.files.push_back({"DepotFile", "bin\\Game.exe", kGameExe, {}, false, true, true});
  main.files.push_back({"DepotFile", "data\\big.dat", kBig, {100 * 1024, 100 * 1024, 77 * 1024}});
  main.files.push_back({"DepotFile", "data\\shared.txt", kBaseShared});
  main.files.push_back({"DepotFile", "empty.txt", ""});
  main.files.push_back({"DepotDirectory", "logs", ""});
  main.files.push_back({"DepotLink", "shortcut.lnk", ""});
  main.files.push_back({"DepotFile", "app\\dosbox.conf", kDosboxConf, {}, true});
  main.files.push_back({"DepotFile", "save\\slot.sav", kSave, {}, true});
  game.depots.push_back(main);
  GameDepot every;
  every.files.push_back({"DepotFile", "music/theme.ogg", kTheme});
  if (huge) {
    GameFile file{"DepotFile", "huge/huge.bin", RandomBytes(17 * 1024 * 1024 + 512 * 1024, 6)};
    file.level = 0;  // stored: the compressed chunk is as large as the file
    every.files.push_back(file);
  }
  game.depots.push_back(every);
  GameDepot english;
  english.languages = {"en-US"};
  english.gog = true;
  english.files.push_back({"DepotFile", "goggame-1900000001.info", kEnglishInfo});
  game.depots.push_back(english);
  GameDepot german;
  german.languages = {"de-DE"};
  german.gog = true;
  german.files.push_back({"DepotFile", "goggame-1900000001.info", kGermanInfo});
  game.depots.push_back(german);
  GameDepot dlc;
  dlc.product = kDlc;
  dlc.files.push_back({"DepotFile", "Data/Shared.TXT", kDlcShared});
  dlc.files.push_back({"DepotFile", "dlc/extra.pak", kExtra});
  game.depots.push_back(dlc);
  return game;
}

// ---- The harness --------------------------------------------------------------------------------------------------

struct Harness {
  CdnStub stub;
  std::atomic<bool> cancel{false};
  std::mutex mutex;
  std::vector<std::string> logs;
  std::atomic<int> token_calls{0};
  std::atomic<int> invalidations{0};
  std::atomic<int> token_number{0};
  std::atomic<std::uint64_t> free{1ull << 40};
  std::atomic<std::int64_t> now{kNow};
  std::atomic<int> pauses{0};
  int parallel = 4;
  GogInstaller::Progress last_progress;
  int progress_calls = 0;

  Harness() {
    stub.SetOwned("{\"owned\":[1900000001,1900000002]}");
    stub.SetSecureLink(ReadFile(g_fixtures + "/secure-link-200.json"));
    stub.SetSecureLinkV1(ReadFile(g_fixtures + "/secure-link-gen1-200.json"));
    stub.SetOpenLink(ReadFile(g_fixtures + "/dependencies-open-link.json"));
  }

  GogInstaller::Hooks Hooks() {
    GogInstaller::Hooks hooks;
    hooks.make_transport = [this] { return stub.Transport(); };
    hooks.token = [this](const std::atomic<bool>&) {
      ++token_calls;
      playden::gog::AccessToken token;
      token.status = playden::gog::AccessToken::Status::kOk;
      token.token = AccessText(token_number.load());
      token.generation = 1;
      return token;
    };
    hooks.invalidate = [this](std::uint64_t, std::string_view token) {
      ++invalidations;
      if (token == AccessText(token_number.load())) ++token_number;
    };
    hooks.free_bytes = [this](const std::string&) { return free.load(); };
    hooks.pause = [this](std::uint32_t, const std::atomic<bool>&) { ++pauses; };
    hooks.now_seconds = [this] { return now.load(); };
    hooks.log = [this](const std::string& line) {
      std::lock_guard<std::mutex> lock(mutex);
      logs.push_back(line);
    };
    hooks.parallel = parallel;
    return hooks;
  }

  GogInstaller::Outcome Run(const std::string& root, const std::string& language = "english") {
    GogInstaller installer(Hooks());
    GogInstaller::Target target;
    target.product = kBase;
    target.root = root;
    target.language = language;
    return installer.Run(target, cancel, [this](const GogInstaller::Progress& progress) {
      CHECK(progress.done <= progress.total);
      last_progress = progress;
      ++progress_calls;
    });
  }

  bool Logged(const std::string& text) {
    std::lock_guard<std::mutex> lock(mutex);
    for (const auto& line : logs)
      if (line.find(text) != std::string::npos) return true;
    return false;
  }

  // Requests carry a Bearer token only to the owned list and secure links; log lines name no URL, token or link.
  void CheckClean() {
    for (const auto& request : stub.requests()) {
      if (request.kind == Kind::kOwned || request.kind == Kind::kSecureLink) {
        CHECK(request.authorization.rfind("Bearer synthetic-access-", 0) == 0);
      } else {
        CHECK(request.authorization.empty());
      }
    }
    std::lock_guard<std::mutex> lock(mutex);
    for (const auto& line : logs)
      for (const char* secret : {"https://", "http://", "token=", "wsSecret", "wsTime", "nva=", "synthetic-",
                                 "signature", "Bearer", "example.invalid", "gog.com", "access"})
        if (line.find(secret) != std::string::npos) {
          std::fprintf(stderr, "log line names %s: %s\n", secret, line.c_str());
          CHECK(false);
        }
  }
};

std::string Root(const std::string& name) { return g_scratch + "/" + name; }

// The plan index of an entry (D and F lines count), or -1.
long PlanIndex(const std::string& root, const std::string& path) {
  long index = 0;
  for (const auto& line : Lines(ReadFile(root + "/.playden-gog/files"))) {
    if (line.rfind("D\t", 0) != 0 && line.rfind("F\t", 0) != 0) continue;
    const auto end = line.find('\t', 2);
    if (line.substr(2, end == std::string::npos ? std::string::npos : end - 2) == path) return index;
    ++index;
  }
  return -1;
}

// ---- FetchChunk on its own ----------------------------------------------------------------------------------------

std::vector<playden::gog::CdnEndpoint> FixtureLink(int generation = 2) {
  auto link = playden::gog::ParseLink(ReadFile(g_fixtures + "/secure-link-200.json"));
  CHECK(link.kind == playden::gog::LinkResult::Kind::kOk);
  if (generation == 2) return link.endpoints;
  // A gen 1 download takes the endpoints that support gen 1: gcore doesn't.
  auto copy = link.endpoints;
  return playden::gog::OrderEndpoints(copy, generation);
}

struct Sink {
  std::string bytes;
  std::uint64_t max_end = 0;
  playden::gog::ChunkWrite Write() {
    return [this](std::uint64_t offset, const std::uint8_t* data, std::size_t size) {
      if (bytes.size() < offset + size) bytes.resize(offset + size);
      std::memcpy(&bytes[offset], data, size);
      max_end = std::max<std::uint64_t>(max_end, offset + size);
      return true;
    };
  }
};

DepotChunk PutChunk(CdnStub& stub, const std::string& raw, std::string* zlib_out = nullptr) {
  const auto zlib = Zlib(raw);
  DepotChunk chunk;
  chunk.compressed_md5 = stub.Put(zlib);
  chunk.compressed_size = zlib.size();
  chunk.md5 = Md5Hex(raw);
  chunk.size = raw.size();
  if (zlib_out) *zlib_out = zlib;
  return chunk;
}

void NoPause(std::uint32_t, const std::atomic<bool>&) {}

void TestLinkFixtures() {
  const auto link = playden::gog::ParseLink(ReadFile(g_fixtures + "/secure-link-200.json"));
  CHECK(link.kind == playden::gog::LinkResult::Kind::kOk);
  CHECK(link.endpoints.size() == 2);
  CHECK(link.endpoints[0].name == "fastly" && link.endpoints[1].name == "gcore");
  CHECK(link.expires_at == 1900000000);  // fastly's; gcore states none
  CHECK(FixtureLink(1).size() == 1 && FixtureLink(1)[0].name == "fastly");

  const auto open = playden::gog::ParseLink(ReadFile(g_fixtures + "/dependencies-open-link.json"));
  CHECK(open.kind == playden::gog::LinkResult::Kind::kOk && open.endpoints.size() == 2 && open.expires_at == 0);
  CHECK(playden::gog::FillUrl(open.endpoints[0], "/ab/cd/x") ==
        std::optional<std::string>("https://gog-cdn-fastly.gog.com/content-system/v2/dependencies/store/ab/cd/x"));

  // GOG's real answer to a secure link request without a good token.
  CdnStub stub;
  stub.SetFault([](const Request& request) -> std::optional<Fault> {
    if (request.kind != Kind::kSecureLink) return std::nullopt;
    Fault fault;
    fault.response = Answer(401, ReadFile(g_fixtures + "/secure-link-401.json"));
    return fault;
  });
  auto transport = stub.Transport();
  std::atomic<bool> cancel{false};
  const auto refused = playden::gog::FetchSecureLink(transport, "synthetic-access-1", kBase, cancel);
  CHECK(refused.kind == playden::gog::LinkResult::Kind::kRejected && refused.status == 401);
  CHECK(stub.requests().at(0).authorization == "Bearer synthetic-access-1");
  stub.SetFault({});
  stub.SetSecureLink(ReadFile(g_fixtures + "/secure-link-200.json"));
  const auto good = playden::gog::FetchSecureLink(transport, "synthetic-access-1", 1900000077, cancel);
  CHECK(good.kind == playden::gog::LinkResult::Kind::kOk);
  CHECK(good.endpoints[0].parameters.at("path") == "/content-system/v2/store/1900000077");

  // Builds and the dependency repository go without a token.
  stub.SetBuilds(kBase, "{}");
  stub.SetRepository(ReadFile(g_fixtures + "/dependencies-repository.json"));
  CHECK(playden::gog::FetchBuilds(transport, kBase, cancel).status == 200);
  CHECK(playden::gog::FetchDependencyRepository(transport, cancel).status == 200);
  for (const auto& request : stub.requests())
    if (request.kind != Kind::kSecureLink) CHECK(request.authorization.empty());
}

void TestRealDependencyChunk() {
  CdnStub stub;
  const auto zlib = ReadFile(g_fixtures + "/dependency-chunk-ede30538132b194f6a477a932441df95.zlib");
  CHECK(stub.Put(zlib) == "ede30538132b194f6a477a932441df95");
  const auto expected = ReadFile(g_fixtures + "/dependency-chunk.expected.json");
  CHECK(expected.find("\"md5\": \"699c1f3f9448f6db90735e1b00f2c954\"") != std::string::npos);
  CHECK(expected.find("\"size\": 264") != std::string::npos);
  CHECK(expected.find("\"compressedSize\": 213") != std::string::npos);
  DepotChunk chunk;
  chunk.compressed_md5 = "ede30538132b194f6a477a932441df95";
  chunk.compressed_size = 213;
  chunk.md5 = "699c1f3f9448f6db90735e1b00f2c954";
  chunk.size = 264;
  const auto open = playden::gog::ParseLink(ReadFile(g_fixtures + "/dependencies-open-link.json"));
  auto transport = stub.Transport();
  std::atomic<bool> cancel{false};
  Sink sink;
  stub.SetBlock(17);
  const auto result = playden::gog::FetchChunk(transport, open.endpoints, chunk, 0, sink.Write(), NoPause, cancel);
  CHECK(result.kind == ChunkResult::Kind::kOk && result.attempts == 1 && result.endpoint == "fastly");
  CHECK(sink.bytes.size() == 264 && Md5Hex(sink.bytes) == chunk.md5);
  const auto request = stub.requests().at(0);
  CHECK(request.kind == Kind::kDependencyChunk && request.host == "gog-cdn-fastly.gog.com");
  CHECK(request.authorization.empty() && request.streamed && request.max_bytes == 214);
}

void TestBadHashesAndLimits() {
  CdnStub stub;
  auto transport = stub.Transport();
  std::atomic<bool> cancel{false};
  const auto endpoints = FixtureLink();
  std::string zlib;
  const auto raw = RandomBytes(300 * 1024, 11);
  const auto chunk = PutChunk(stub, raw, &zlib);

  // A bad compressed body once, then the right one.
  {
    std::atomic<int> calls{0};
    stub.SetFault([&](const Request&) -> std::optional<Fault> {
      if (calls++ > 0) return std::nullopt;
      Fault fault;
      fault.body = zlib;
      (*fault.body)[fault.body->size() / 2] ^= 0x5a;
      return fault;
    });
    Sink sink;
    const auto result = playden::gog::FetchChunk(transport, endpoints, chunk, 4096, sink.Write(), NoPause, cancel);
    CHECK(result.kind == ChunkResult::Kind::kOk && result.attempts == 2 && result.corrupt == 1);
    CHECK(sink.bytes.substr(4096) == raw);
  }
  // A body of the same size that inflates to the right bytes but isn't the one the manifest names: only the
  // compressed MD5 tells (the zlib header's FLEVEL bits differ; inflate ignores them).
  {
    std::string other = zlib;
    const auto cmf = static_cast<unsigned char>(other[0]);
    const auto flg = static_cast<unsigned char>(other[1]);
    unsigned char level = static_cast<unsigned char>(((flg >> 6) + 1) & 3);
    unsigned char changed = static_cast<unsigned char>(level << 6);
    changed = static_cast<unsigned char>(changed + (31 - ((cmf * 256u + changed) % 31)) % 31);
    CHECK(changed != flg && (cmf * 256u + changed) % 31 == 0);
    other[1] = static_cast<char>(changed);
    std::atomic<int> calls{0};
    stub.SetFault([&](const Request&) -> std::optional<Fault> {
      if (calls++ > 0) return std::nullopt;
      Fault fault;
      fault.body = other;
      return fault;
    });
    Sink sink;
    const auto result = playden::gog::FetchChunk(transport, endpoints, chunk, 0, sink.Write(), NoPause, cancel);
    CHECK(result.kind == ChunkResult::Kind::kOk && result.attempts == 2 && result.corrupt == 1);
    CHECK(sink.bytes == raw);
  }
  // A body longer than compressedSize: by one byte the sink stops it; by more the client's cap does. Both are corrupt.
  for (const std::size_t extra : {std::size_t(1), std::size_t(20000)}) {
    std::atomic<int> calls{0};
    stub.SetFault([&](const Request&) -> std::optional<Fault> {
      if (calls++ > 0) return std::nullopt;
      Fault fault;
      fault.body = zlib + std::string(extra, 'x');
      return fault;
    });
    Sink sink;
    const auto result = playden::gog::FetchChunk(transport, endpoints, chunk, 0, sink.Write(), NoPause, cancel);
    CHECK(result.kind == ChunkResult::Kind::kOk && result.corrupt == 1);
    CHECK(sink.bytes == raw);
  }
  // A body shorter than compressedSize.
  {
    stub.SetFault([&](const Request&) -> std::optional<Fault> {
      Fault fault;
      fault.body = zlib.substr(0, zlib.size() - 10);
      return fault;
    });
    Sink sink;
    const auto result = playden::gog::FetchChunk(transport, endpoints, chunk, 0, sink.Write(), NoPause, cancel);
    CHECK(result.kind == ChunkResult::Kind::kCorrupt && result.attempts == 5 && result.corrupt == 5);
  }
  // A decompression bomb: a whole, well-formed stream that inflates far past `size`. Nothing is written past size.
  {
    stub.SetFault({});
    const auto bomb = Zlib(std::string(8 * 1024 * 1024, '\0'), 9);
    DepotChunk tiny;
    tiny.compressed_md5 = stub.Put(bomb);
    tiny.compressed_size = bomb.size();
    tiny.md5 = Md5Hex(std::string(1000, '\0'));
    tiny.size = 1000;
    Sink sink;
    const auto result = playden::gog::FetchChunk(transport, endpoints, tiny, 0, sink.Write(), NoPause, cancel);
    CHECK(result.kind == ChunkResult::Kind::kCorrupt && result.corrupt == 5);
    CHECK(sink.max_end <= 1000);
  }
  // A chunk whose manifest md5 is wrong: every attempt inflates whole and fails the second hash.
  {
    DepotChunk wrong = chunk;
    wrong.md5 = Md5Hex("something else");
    Sink sink;
    const auto result = playden::gog::FetchChunk(transport, endpoints, wrong, 0, sink.Write(), NoPause, cancel);
    CHECK(result.kind == ChunkResult::Kind::kCorrupt && result.attempts == 5);
  }
  // A write that fails: storage, or no space, at once.
  {
    for (const int error : {EIO, ENOSPC}) {
      stub.ClearRequests();
      const auto result = playden::gog::FetchChunk(
          transport, endpoints, chunk, 0,
          [error](std::uint64_t, const std::uint8_t*, std::size_t) {
            errno = error;
            return false;
          },
          NoPause, cancel);
      CHECK(result.kind == (error == ENOSPC ? ChunkResult::Kind::kNoSpace : ChunkResult::Kind::kStorage));
      CHECK(result.attempts == 1 && stub.requests().size() == 1);
    }
  }
}

void TestFailover() {
  CdnStub stub;
  auto transport = stub.Transport();
  std::atomic<bool> cancel{false};
  const auto raw = RandomBytes(64 * 1024, 21);
  const auto chunk = PutChunk(stub, raw);
  for (const bool connection_error : {false, true}) {
    stub.ClearRequests();
    stub.SetFault([&](const Request& request) -> std::optional<Fault> {
      if (request.host != "fastly") return std::nullopt;
      Fault fault;
      fault.response = connection_error ? Failure(HttpError::kConnectionFailed) : Answer(503, "busy");
      return fault;
    });
    Sink sink;
    std::atomic<int> pauses{0};
    const auto result = playden::gog::FetchChunk(
        transport, FixtureLink(), chunk, 0, sink.Write(),
        [&pauses](std::uint32_t ms, const std::atomic<bool>&) {
          CHECK(ms == 2000u * static_cast<std::uint32_t>(pauses.load() + 1));
          ++pauses;
        },
        cancel);
    CHECK(result.kind == ChunkResult::Kind::kOk && result.endpoint == "gcore" && result.attempts == 2);
    CHECK(pauses == 1 && sink.bytes == raw);
    const auto requests = stub.requests();
    CHECK(requests.size() == 2 && requests[0].host == "fastly" && requests[1].host == "gcore");
    CHECK(requests[0].kind == Kind::kChunk && requests[0].product == kBase);
    CHECK(requests[1].url.find("wsSecret=") != std::string::npos);
  }
  // A gen 1 list has no gcore: five tries on fastly, then a network failure.
  stub.ClearRequests();
  stub.SetFault([](const Request& request) -> std::optional<Fault> {
    if (request.host != "fastly") return std::nullopt;
    Fault fault;
    fault.response = Answer(503, "busy");
    return fault;
  });
  Sink sink;
  const auto result = playden::gog::FetchChunk(transport, FixtureLink(1), chunk, 0, sink.Write(), NoPause, cancel);
  CHECK(result.kind == ChunkResult::Kind::kNetwork && result.attempts == 5 && result.status == 503);
  for (const auto& request : stub.requests()) CHECK(request.host == "fastly");
  // A 403 or 401 is a refused link: no retries.
  for (const long status : {403L, 401L}) {
    stub.ClearRequests();
    stub.SetFault([status](const Request&) -> std::optional<Fault> {
      Fault fault;
      fault.response = Answer(status, "");
      return fault;
    });
    const auto refused = playden::gog::FetchChunk(transport, FixtureLink(), chunk, 0, sink.Write(), NoPause, cancel);
    CHECK(refused.kind == ChunkResult::Kind::kExpired && refused.attempts == 1 && refused.status == status);
  }
  // A small chunk refused with an error page longer than the chunk (and than the client keeps): still a refused
  // link, not an overlong chunk.
  {
    const auto tiny = PutChunk(stub, "68 bytes is a real GOG chunk size");
    for (const std::size_t length : {std::size_t{2000}, playden::steam::kMaxHttpStreamErrorBodyBytes + 100}) {
      stub.ClearRequests();
      stub.SetFault([length](const Request&) -> std::optional<Fault> {
        Fault fault;
        fault.status = 403;
        fault.body = std::string(length, 'x');
        return fault;
      });
      Sink tiny_sink;
      const auto refused = playden::gog::FetchChunk(transport, FixtureLink(), tiny, 0, tiny_sink.Write(), NoPause, cancel);
      CHECK(refused.kind == ChunkResult::Kind::kExpired && refused.attempts == 1 && refused.status == 403);
    }
    // A CDN 404 page longer than the chunk is a network problem, tried again, not a corrupt chunk.
    stub.ClearRequests();
    stub.SetFault([](const Request&) -> std::optional<Fault> {
      Fault fault;
      fault.status = 404;
      fault.body = std::string(4096, 'x');
      return fault;
    });
    Sink tiny_sink;
    const auto missing = playden::gog::FetchChunk(transport, FixtureLink(), tiny, 0, tiny_sink.Write(), NoPause, cancel);
    CHECK(missing.kind == ChunkResult::Kind::kNetwork && missing.status == 404);
  }
  // A stall is an attempt and is reported.
  stub.SetFault([](const Request& request) -> std::optional<Fault> {
    if (request.host != "fastly") return std::nullopt;
    Fault fault;
    fault.stall_after = 1000;
    return fault;
  });
  Sink stalled;
  const auto slow = playden::gog::FetchChunk(transport, FixtureLink(), chunk, 0, stalled.Write(), NoPause, cancel);
  CHECK(slow.kind == ChunkResult::Kind::kOk && slow.stalled && slow.stalls == 1 && stalled.bytes == raw);
}

// ---- FetchRange on its own (gen 1 main.bin) ------------------------------------------------------------------------

constexpr std::uint64_t kTimestamp = 31000007;

// The gen 1 link's endpoints through FetchSecureLinkV1: fastly only.
std::vector<playden::gog::CdnEndpoint> Gen1Link(CdnStub& stub) {
  auto transport = stub.Transport();
  std::atomic<bool> cancel{false};
  auto link = playden::gog::FetchSecureLinkV1(transport, "synthetic-access-1", kBase, kTimestamp, cancel);
  CHECK(link.kind == playden::gog::LinkResult::Kind::kOk);
  return link.endpoints;
}

// The gen 1 fixture with gcore listing generation 1 too: two endpoints to rotate over.
std::vector<playden::gog::CdnEndpoint> TwoGen1Endpoints() {
  std::string body = ReadFile(g_fixtures + "/secure-link-gen1-200.json");
  const std::string only2 = "\"supports_generation\": [\n                2\n            ]";
  const auto at = body.find(only2);
  CHECK(at != std::string::npos);
  body.replace(at, only2.size(), "\"supports_generation\": [1, 2]");
  auto link = playden::gog::ParseLink(body, 1);
  CHECK(link.kind == playden::gog::LinkResult::Kind::kOk && link.endpoints.size() == 2);
  return link.endpoints;
}

void TestRanges() {
  using playden::gog::FetchRange;
  using playden::gog::kGen1RangeBytes;
  CdnStub stub;
  stub.SetSecureLinkV1(ReadFile(g_fixtures + "/secure-link-gen1-200.json"));
  const auto blob = RandomBytes(1024 * 1024, 31);
  stub.SetBlob(kBase, blob);
  auto transport = stub.Transport();
  std::atomic<bool> cancel{false};
  const auto endpoints = Gen1Link(stub);
  CHECK(endpoints.size() == 1 && endpoints[0].name == "fastly");
  const std::uint64_t first = 123456, length = 300000, file_offset = 5000;
  const std::string want = blob.substr(first, length);
  const auto counting = [](std::atomic<int>& pauses) {
    return [&pauses](std::uint32_t ms, const std::atomic<bool>&) {
      CHECK(ms == 2000u * static_cast<std::uint32_t>(pauses.load() + 1));
      ++pauses;
    };
  };

  // The exact bytes at file_offset, from a 206 in several blocks; the request names the range and carries no token.
  {
    stub.ClearRequests();
    Sink sink;
    const auto result = FetchRange(transport, endpoints, first, length, file_offset, sink.Write(), NoPause, cancel);
    CHECK(result.kind == ChunkResult::Kind::kOk && result.attempts == 1 && result.corrupt == 0);
    CHECK(result.endpoint == "fastly" && result.status == 206);
    CHECK(sink.bytes.substr(file_offset) == want && sink.max_end == file_offset + length);
    const auto requests = stub.requests();
    CHECK(requests.size() == 1);
    const auto& request = requests[0];
    CHECK(request.kind == Kind::kRange && request.product == kBase && request.timestamp == kTimestamp);
    CHECK(request.range == "bytes=123456-423455" && request.host == "fastly");
    CHECK(request.authorization.empty() && request.streamed && request.max_bytes == length);
    CHECK(request.url.find("/content-system/v1/depots/1900000001/windows/31000007/main.bin") != std::string::npos);
    CHECK(request.sent == length);
    // The blob's last bytes, and a range of exactly kGen1RangeBytes.
    Sink tail;
    CHECK(FetchRange(transport, endpoints, blob.size() - 10, 10, 0, tail.Write(), NoPause, cancel).kind ==
          ChunkResult::Kind::kOk);
    CHECK(tail.bytes == blob.substr(blob.size() - 10));
    const auto big = RandomBytes(kGen1RangeBytes + 4096, 32);
    stub.SetBlob(1900000077, big);
    auto big_link = endpoints;
    big_link[0].parameters["path"] = "/content-system/v1/depots/1900000077/windows/31000007";
    Sink whole;
    CHECK(FetchRange(transport, big_link, 4096, kGen1RangeBytes, 0, whole.Write(), NoPause, cancel).kind ==
          ChunkResult::Kind::kOk);
    CHECK(whole.bytes == big.substr(4096));
  }
  // A server that ignores Range answers 200 with the whole blob: the cap stops it after `length` bytes, nothing lands
  // past the range, and it is retried as a failed check.
  {
    stub.ClearRequests();
    stub.SetFault([](const Request&) -> std::optional<Fault> {
      Fault fault;
      fault.whole_blob = true;
      return fault;
    });
    Sink sink;
    std::atomic<int> pauses{0};
    const auto result =
        FetchRange(transport, endpoints, first, length, file_offset, sink.Write(), counting(pauses), cancel);
    CHECK(result.kind == ChunkResult::Kind::kCorrupt && result.attempts == 5 && result.corrupt == 5 && pauses == 4);
    CHECK(sink.max_end <= file_offset + length);
    // Each attempt stopped at the cap, far short of the 1 MiB blob.
    CHECK(stub.requests().size() == 5);
    for (const auto& request : stub.requests()) CHECK(request.sent <= length && request.sent < blob.size());
    CHECK(stub.Sent(Kind::kRange) <= 5 * length);
  }
  // A 200 whose whole blob is exactly the range's size: only the status tells.
  {
    const auto small = RandomBytes(5000, 33);
    stub.SetBlob(1900000078, small);
    auto small_link = endpoints;
    small_link[0].parameters["path"] = "/content-system/v1/depots/1900000078/windows/31000007";
    Sink sink;
    const auto result = FetchRange(transport, small_link, 0, small.size(), 0, sink.Write(), NoPause, cancel);
    CHECK(result.kind == ChunkResult::Kind::kCorrupt && result.attempts == 5 && result.status == 200);
    stub.SetFault({});
    Sink good;
    CHECK(FetchRange(transport, small_link, 0, small.size(), 0, good.Write(), NoPause, cancel).kind ==
          ChunkResult::Kind::kOk);
    CHECK(good.bytes == small);
  }
  // A short body once: tried again, then whole.
  {
    std::atomic<int> calls{0};
    stub.SetFault([&calls](const Request&) -> std::optional<Fault> {
      if (calls++ > 0) return std::nullopt;
      Fault fault;
      fault.short_by = 100;
      return fault;
    });
    Sink sink;
    const auto result = FetchRange(transport, endpoints, first, length, file_offset, sink.Write(), NoPause, cancel);
    CHECK(result.kind == ChunkResult::Kind::kOk && result.attempts == 2 && result.corrupt == 1);
    CHECK(sink.bytes.substr(file_offset) == want);
  }
  // A Content-Range for other bytes, with the right status and length: a failed check every time.
  for (const std::string wrong : {"bytes 123457-423456/1048576", "bytes 123456-423455", "bytes 0-1023/1048576",
                                  "items 123456-423455/1048576", "bytes 123456-423455/x"}) {
    stub.SetFault([wrong](const Request&) -> std::optional<Fault> {
      Fault fault;
      fault.content_range = wrong;
      return fault;
    });
    Sink sink;
    const auto result = FetchRange(transport, endpoints, first, length, file_offset, sink.Write(), NoPause, cancel);
    CHECK(result.kind == ChunkResult::Kind::kCorrupt && result.attempts == 5 && result.corrupt == 5);
    CHECK(result.status == 206);
  }
  // An unknown total and the unit in another case are the same range.
  for (const std::string same : {"bytes 123456-423455/*", "Bytes 123456-423455/1048576"}) {
    stub.SetFault([same](const Request&) -> std::optional<Fault> {
      Fault fault;
      fault.content_range = same;
      return fault;
    });
    Sink sink;
    CHECK(FetchRange(transport, endpoints, first, length, file_offset, sink.Write(), NoPause, cancel).kind ==
          ChunkResult::Kind::kOk);
  }
  // 403 (once) or 401: the link expired, at once.
  for (const long status : {403L, 401L}) {
    stub.ClearRequests();
    std::atomic<int> calls{0};
    stub.SetFault([&calls, status](const Request&) -> std::optional<Fault> {
      if (calls++ > 0) return std::nullopt;
      Fault fault;
      fault.status = status;
      fault.body = "refused";
      return fault;
    });
    Sink sink;
    const auto result = FetchRange(transport, endpoints, first, length, file_offset, sink.Write(), NoPause, cancel);
    CHECK(result.kind == ChunkResult::Kind::kExpired && result.attempts == 1 && result.status == status);
    CHECK(stub.requests().size() == 1 && sink.bytes.empty());
  }
  // Gen 1 uses fastly only: a busy fastly is tried five times, and gcore never.
  {
    stub.ClearRequests();
    stub.SetFault([](const Request&) -> std::optional<Fault> {
      Fault fault;
      fault.response = Answer(503, "busy");
      return fault;
    });
    Sink sink;
    std::atomic<int> pauses{0};
    const auto result =
        FetchRange(transport, endpoints, first, length, file_offset, sink.Write(), counting(pauses), cancel);
    CHECK(result.kind == ChunkResult::Kind::kNetwork && result.attempts == 5 && result.status == 503 && pauses == 4);
    for (const auto& request : stub.requests()) CHECK(request.host == "fastly");
    // No answer, a missing blob (404) and a start past the end (416) are network failures too.
    for (const int which : {0, 1, 2}) {
      stub.SetFault([which](const Request&) -> std::optional<Fault> {
        if (which == 1) return std::nullopt;
        Fault fault;
        if (which == 0) fault.response = Failure(HttpError::kConnectionFailed);
        else fault.status = 404;
        return fault;
      });
      const std::uint64_t at = which == 1 ? blob.size() : first;
      Sink none;
      const auto failed = FetchRange(transport, endpoints, at, 10, 0, none.Write(), NoPause, cancel);
      CHECK(failed.kind == ChunkResult::Kind::kNetwork && failed.attempts == 5);
      CHECK(failed.status == (which == 0 ? 0 : which == 1 ? 416 : 404));
    }
    stub.SetFault({});
  }
  // A stall is an attempt, counted, and the next one goes to the next endpoint.
  {
    stub.ClearRequests();
    stub.SetFault([](const Request& request) -> std::optional<Fault> {
      if (request.host != "fastly") return std::nullopt;
      Fault fault;
      fault.stall_after = 1000;
      return fault;
    });
    Sink sink;
    const auto result =
        FetchRange(transport, TwoGen1Endpoints(), first, length, file_offset, sink.Write(), NoPause, cancel);
    CHECK(result.kind == ChunkResult::Kind::kOk && result.stalled && result.stalls == 1 && result.attempts == 2);
    CHECK(result.endpoint == "gcore" && sink.bytes.substr(file_offset) == want);
    const auto requests = stub.requests();
    CHECK(requests.size() == 2 && requests[0].host == "fastly" && requests[1].host == "gcore");
    CHECK(requests[1].kind == Kind::kRange && requests[1].range == "bytes=123456-423455");
    stub.SetFault({});
  }
  // Cancel mid-body.
  {
    stub.ClearRequests();
    std::atomic<bool> stop{false};
    Sink sink;
    auto write = sink.Write();
    const auto result = FetchRange(
        transport, endpoints, first, length, file_offset,
        [&](std::uint64_t offset, const std::uint8_t* data, std::size_t size) {
          stop = true;
          return write(offset, data, size);
        },
        NoPause, stop);
    CHECK(result.kind == ChunkResult::Kind::kCanceled && result.attempts == 1 && stub.requests().size() == 1);
    CHECK(sink.max_end < file_offset + length);
  }
  // A write that fails: storage, or no space, at once.
  for (const int error : {EIO, ENOSPC}) {
    stub.ClearRequests();
    const auto result = FetchRange(
        transport, endpoints, first, length, file_offset,
        [error](std::uint64_t, const std::uint8_t*, std::size_t) {
          errno = error;
          return false;
        },
        NoPause, cancel);
    CHECK(result.kind == (error == ENOSPC ? ChunkResult::Kind::kNoSpace : ChunkResult::Kind::kStorage));
    CHECK(result.attempts == 1 && stub.requests().size() == 1);
  }
  // Ranges that can't be asked for: empty, above kGen1RangeBytes, past 2^64. No request.
  {
    stub.ClearRequests();
    Sink sink;
    for (const auto& [at, size] : std::vector<std::pair<std::uint64_t, std::uint64_t>>{
             {0, 0}, {0, kGen1RangeBytes + 1}, {UINT64_MAX, 2}, {UINT64_MAX - 9, 11}}) {
      const auto result = FetchRange(transport, endpoints, at, size, 0, sink.Write(), NoPause, cancel);
      CHECK(result.kind == ChunkResult::Kind::kCorrupt && result.attempts == 0);
    }
    CHECK(stub.requests().empty() && sink.bytes.empty());
    // No endpoint: nothing to ask.
    CHECK(FetchRange(transport, {}, 0, 10, 0, sink.Write(), NoPause, cancel).kind == ChunkResult::Kind::kNetwork);
    CHECK(stub.requests().empty());
  }
  // The gen 1 link request itself carried the Bearer token; no range request did.
  stub.ClearRequests();
  Gen1Link(stub);
  const auto requests = stub.requests();
  CHECK(requests.size() == 1 && requests[0].kind == Kind::kSecureLink && requests[0].generation == 1);
  CHECK(requests[0].timestamp == kTimestamp && requests[0].authorization == "Bearer synthetic-access-1");
}

// ---- The installer ------------------------------------------------------------------------------------------------

void CheckSyntheticFiles(const std::string& root, bool huge, const std::string& info) {
  CHECK(Contents(root + "/bin/Game.exe") == kGameExe);
  CHECK(Contents(root + "/data/big.dat") == kBig);
  CHECK(Contents(root + "/empty.txt") == std::string());
  CHECK(IsDirectory(root + "/logs"));
  CHECK(!Exists(root + "/shortcut.lnk"));
  CHECK(Contents(root + "/dosbox.conf") == kDosboxConf);  // app/ support goes to the root
  CHECK(!Exists(root + "/app"));
  CHECK(Contents(root + "/.playden-gog/support/1900000001/save/slot.sav") == kSave);
  CHECK(!Exists(root + "/save"));
  CHECK(Contents(root + "/music/theme.ogg") == kTheme);
  CHECK(Contents(root + "/dlc/extra.pak") == kExtra);
  // The DLC's file wins over the base game's, in the folder's first spelling ("data") and the DLC's own name.
  CHECK(Names(root + "/data") == (std::vector<std::string>{"Shared.TXT", "big.dat"}));
  CHECK(Contents(root + "/data/Shared.TXT") == kDlcShared);
  CHECK(Contents(root + "/goggame-1900000001.info") == info);
  if (huge) CHECK(Exists(root + "/huge/huge.bin"));
}

void TestGoodInstall() {
  Harness harness;
  auto game = SyntheticGame(true);
  const auto published = Publish(harness.stub, game);
  const auto root = Root("good");
  const auto& huge = published.chunks.at("huge/huge.bin");
  CHECK(huge.size() == 1 && huge[0].compressed_size > 16u * 1024 * 1024);  // above HttpClient's buffered cap
  std::atomic<bool> first_checked{false};
  harness.stub.SetFault([&](const Request& request) -> std::optional<Fault> {
    if (!first_checked.exchange(true)) {
      CHECK(request.kind == Kind::kBuilds);
      CHECK(IsDirectory(root + "/.playden-download"));  // before any request, so before any game file
    }
    return std::nullopt;
  });
  const auto outcome = harness.Run(root);
  CHECK(outcome.error == Error::kNone);
  CHECK(outcome.build_id == game.build_id && outcome.meta_id == published.meta_id);
  CHECK(outcome.products == (std::vector<std::uint64_t>{kBase, kDlc}));
  CHECK(outcome.language == "en-US");
  CHECK(outcome.files == 10);
  const std::uint64_t expected_size = kGameExe.size() + kBig.size() + kDosboxConf.size() + kSave.size() +
                                      kTheme.size() + (17 * 1024 * 1024 + 512 * 1024) + kEnglishInfo.size() +
                                      kDlcShared.size() + kExtra.size();
  CHECK(outcome.install_bytes == expected_size);
  CheckSyntheticFiles(root, true, kEnglishInfo);
  CHECK(Contents(root + "/huge/huge.bin")->size() == 17 * 1024 * 1024 + 512 * 1024);
  CHECK(harness.last_progress.done == harness.last_progress.total && harness.last_progress.held == 0);
  CHECK(harness.stub.Count(Kind::kSecureLink) == 2);  // the base game and the DLC
  CHECK(harness.stub.Count(Kind::kOpenLink) == 0);
  CHECK(harness.Logged("gog plan entries="));

  // .playden-files names every placed path; the plan and the saved manifests are kept.
  const auto listed = Lines(ReadFile(root + "/.playden-files"));
  const std::set<std::string> listed_set(listed.begin(), listed.end());
  const std::set<std::string> expected{"f\tbin/Game.exe",   "f\tdata/big.dat",   "f\tempty.txt",
                                       "d\tlogs",           "f\tdosbox.conf",    "f\t.playden-gog/support/1900000001/save/slot.sav",
                                       "f\tmusic/theme.ogg", "f\thuge/huge.bin", "f\tgoggame-1900000001.info",
                                       "f\tdata/Shared.TXT", "f\tdlc/extra.pak"};
  CHECK(listed_set == expected && listed.size() == expected.size());
  CHECK(Exists(root + "/.playden-gog/meta/" + published.meta_id));
  const auto header = Lines(ReadFile(root + "/.playden-gog/files")).at(0);
  CHECK(header.rfind("v2\t2\t" + game.build_id + "\t" + published.meta_id + "\ten-US\t1900000001,1900000002\t", 0) == 0);
  CHECK(header.size() > 2 && header.substr(header.size() - 2) == "\t-");
  CHECK(outcome.generation == 2 && outcome.timestamp == 0);
  CHECK(!Exists(root + "/.playden-gog/items.tmp"));
  harness.CheckClean();

  // A second run finds everything finished: no request at all.
  harness.stub.ClearRequests();
  const auto again = harness.Run(root);
  CHECK(again.error == Error::kNone && harness.stub.requests().empty());
  CHECK(harness.last_progress.held == expected_size);
}

void TestLanguage() {
  Harness harness;
  Publish(harness.stub, SyntheticGame());
  const auto root = Root("german");
  const auto outcome = harness.Run(root, "german");
  CHECK(outcome.error == Error::kNone && outcome.language == "de-DE");
  CheckSyntheticFiles(root, false, kGermanInfo);
  CHECK(outcome.files == 9);
}

// A base depot whose first language is a code the plan can't hold: the next one is installed, and the plan reads
// back (a run that resumes it finds the same language).
void TestOddLanguage() {
  Harness harness;
  auto game = SyntheticGame();
  game.depots[0].languages = {"abcdefghijklmnopq", "de-DE"};
  Publish(harness.stub, game);
  const auto root = Root("odd-language");
  const auto outcome = harness.Run(root, "english");
  CHECK(outcome.error == Error::kNone && outcome.language == "de-DE");
  CheckSyntheticFiles(root, false, kGermanInfo);
}

void TestCorruptRetried() {
  Harness harness;
  const auto published = Publish(harness.stub, SyntheticGame());
  const auto target = published.chunks.at("data\\big.dat").at(0).compressed_md5;
  std::atomic<int> calls{0};
  harness.stub.SetFault([&](const Request& request) -> std::optional<Fault> {
    if (request.hash != target || calls++ > 0) return std::nullopt;
    Fault fault;
    fault.body = std::string(published.chunks.at("data\\big.dat").at(0).compressed_size, 'z');
    return fault;
  });
  const auto root = Root("corrupt-retried");
  const auto outcome = harness.Run(root);
  CHECK(outcome.error == Error::kNone && outcome.stats.corrupt_retries == 1);
  CHECK(harness.stub.CountHash(target) == 2);
  CheckSyntheticFiles(root, false, kEnglishInfo);
}

void TestInflatedHashFails() {
  Harness harness;
  harness.parallel = 1;
  auto game = SyntheticGame();
  game.depots[0].files[1].lie_chunk = 1;  // data\big.dat's second chunk
  const auto published = Publish(harness.stub, game);
  const auto bad = published.chunks.at("data\\big.dat").at(1).compressed_md5;
  const auto root = Root("inflated-hash");
  const auto outcome = harness.Run(root);
  CHECK(outcome.error == Error::kCorrupt);
  CHECK(harness.stub.CountHash(bad) == 5);
  const auto index = PlanIndex(root, "data/big.dat");
  CHECK(index >= 0);
  const auto journal = Lines(ReadFile(root + "/.playden-download/gog-journal"));
  const auto has = [&](const std::string& line) { return std::find(journal.begin(), journal.end(), line) != journal.end(); };
  CHECK(has("c\t" + std::to_string(index) + "\t0"));
  CHECK(!has("c\t" + std::to_string(index) + "\t1"));
  CHECK(!has("f\t" + std::to_string(index)));
  harness.CheckClean();
}

// A file whose chunks all pass but whose whole-file md5 doesn't: fetched once more, then corrupt. A later run finds
// every chunk journaled and no finished line, so it checks the file first and fetches it again from there.
// An empty file whose manifest sha256 isn't the empty file's: no chunk to fetch again, so the download fails as
// corrupt instead of waiting for a fetch that never comes.
void TestEmptyFileCheckFails() {
  Harness harness;
  auto game = SyntheticGame();
  GameFile empty;
  empty.path = "empty.cfg";
  empty.sha256 = true;
  empty.lie_sha256 = true;
  game.depots[0].files.push_back(empty);
  Publish(harness.stub, game);
  const auto root = Root("empty-file-check");
  const auto outcome = harness.Run(root);
  CHECK(outcome.error == Error::kCorrupt && outcome.stats.redone_files == 0);
  harness.CheckClean();
}

void TestFileCheckFails() {
  Harness harness;
  auto game = SyntheticGame();
  game.depots[0].files[1].lie_file_md5 = true;  // data\big.dat
  const auto published = Publish(harness.stub, game);
  const auto& big = published.chunks.at("data\\big.dat");
  const auto root = Root("file-check");
  const auto outcome = harness.Run(root);
  CHECK(outcome.error == Error::kCorrupt && outcome.stats.redone_files == 1);
  for (const auto& chunk : big) CHECK(harness.stub.CountHash(chunk.compressed_md5) == 2);
  const auto index = PlanIndex(root, "data/big.dat");
  const auto journal = Lines(ReadFile(root + "/.playden-download/gog-journal"));
  CHECK(std::find(journal.begin(), journal.end(), "f\t" + std::to_string(index)) == journal.end());
  CHECK(std::find(journal.begin(), journal.end(), "c\t" + std::to_string(index) + "\t2") != journal.end());

  const auto again = harness.Run(root);
  CHECK(again.error == Error::kCorrupt && again.stats.redone_files == 1);
  for (const auto& chunk : big) CHECK(harness.stub.CountHash(chunk.compressed_md5) == 3);
  harness.CheckClean();
}

void TestSlowCdn() {
  Harness harness;
  const auto published = Publish(harness.stub, SyntheticGame());
  std::atomic<int> stalls{0};
  harness.stub.SetFault([&](const Request& request) -> std::optional<Fault> {
    if (request.kind != Kind::kChunk || stalls.load() >= 3 || stalls++ >= 3) return std::nullopt;
    Fault fault;
    fault.stall_after = 512;
    return fault;
  });
  const auto root = Root("slow");
  const auto outcome = harness.Run(root);
  CHECK(outcome.error == Error::kNone);
  CHECK(outcome.stats.stalls == 3 && outcome.stats.parallel == 3);
  CHECK(harness.Logged("gog download parallel=3"));
  CheckSyntheticFiles(root, false, kEnglishInfo);
  harness.CheckClean();
}

void TestLinkExpiry() {
  // A chunk refused once: the link is fetched again, once, and the chunk comes.
  {
    Harness harness;
    Publish(harness.stub, SyntheticGame());
    std::atomic<int> refused{0};
    harness.stub.SetFault([&](const Request& request) -> std::optional<Fault> {
      if (request.kind != Kind::kChunk || request.product != kBase || refused++ > 0) return std::nullopt;
      Fault fault;
      fault.response = Answer(403, "");
      return fault;
    });
    const auto root = Root("link-403");
    const auto outcome = harness.Run(root);
    CHECK(outcome.error == Error::kNone);
    int base_links = 0;
    for (const auto& request : harness.stub.requests()) base_links += request.kind == Kind::kSecureLink && request.product == kBase;
    CHECK(base_links == 2);
    CheckSyntheticFiles(root, false, kEnglishInfo);
    harness.CheckClean();
  }
  // secure_link refuses the token, and again after it was invalidated: signed out, nothing downloaded.
  {
    Harness harness;
    Publish(harness.stub, SyntheticGame());
    harness.stub.SetFault([&](const Request& request) -> std::optional<Fault> {
      if (request.kind != Kind::kSecureLink) return std::nullopt;
      Fault fault;
      fault.response = Answer(401, ReadFile(g_fixtures + "/secure-link-401.json"));
      return fault;
    });
    const auto outcome = harness.Run(Root("link-401"));
    CHECK(outcome.error == Error::kSignedOut);
    CHECK(harness.invalidations == 1 && harness.stub.Count(Kind::kSecureLink) == 2);
    CHECK(harness.stub.Count(Kind::kChunk) == 0);
    const auto requests = harness.stub.requests();
    std::vector<std::string> tokens;
    for (const auto& request : requests)
      if (request.kind == Kind::kSecureLink) tokens.push_back(request.authorization);
    CHECK(tokens.size() == 2 && tokens[0] != tokens[1]);  // a new token after invalidate
  }
  // A 403 from secure_link: the account doesn't own it.
  {
    Harness harness;
    Publish(harness.stub, SyntheticGame());
    harness.stub.SetFault([&](const Request& request) -> std::optional<Fault> {
      if (request.kind != Kind::kSecureLink || request.product != kDlc) return std::nullopt;
      Fault fault;
      fault.response = Answer(403, "");
      return fault;
    });
    CHECK(harness.Run(Root("link-forbidden")).error == Error::kNotOwned);
    CHECK(harness.stub.Count(Kind::kChunk) == 0);
  }
  // Near its expires_at, a link is fetched again before the next chunk; one that came already near it (a wrong clock)
  // is not fetched again for every chunk.
  for (const bool late_clock : {false, true}) {
    Harness harness;
    harness.parallel = 1;
    harness.now = late_clock ? 1900000000 - 300 : 1900000000 - 700;
    Publish(harness.stub, SyntheticGame());
    std::atomic<bool> moved{false};
    harness.stub.SetFault([&](const Request& request) -> std::optional<Fault> {
      if (request.kind == Kind::kChunk && !moved.exchange(true)) harness.now += 200;
      return std::nullopt;
    });
    const auto root = Root(late_clock ? "link-late-clock" : "link-clock");
    CHECK(harness.Run(root).error == Error::kNone);
    int base_links = 0;
    for (const auto& request : harness.stub.requests()) base_links += request.kind == Kind::kSecureLink && request.product == kBase;
    CHECK(base_links == (late_clock ? 1 : 2));
    CheckSyntheticFiles(root, false, kEnglishInfo);
  }
}

void TestResume() {
  Harness harness;
  harness.parallel = 1;
  auto game = SyntheticGame();
  const auto published = Publish(harness.stub, game);
  const auto& big = published.chunks.at("data\\big.dat");
  harness.stub.SetFault([&](const Request& request) -> std::optional<Fault> {
    if (request.hash != big.at(1).compressed_md5 || harness.cancel.load()) return std::nullopt;
    harness.cancel = true;
    Fault fault;
    fault.response = Failure(HttpError::kCanceled);
    return fault;
  });
  const auto root = Root("resume");
  CHECK(harness.Run(root).error == Error::kCanceled);
  CHECK(harness.stub.CountHash(big.at(0).compressed_md5) == 1);
  CHECK(Exists(root + "/.playden-gog/files") && Exists(root + "/.playden-download/gog-journal"));
  const int builds = harness.stub.Count(Kind::kBuilds);
  const int metas = harness.stub.Count(Kind::kMeta);
  const int owned = harness.stub.Count(Kind::kOwned);
  CHECK(builds == 1);

  // The game gets a newer build meanwhile: the resume keeps to the saved plan.
  auto newer = SyntheticGame();
  newer.build_id = "1000000000000000202";
  newer.depots[1].files[0].bytes = RandomBytes(51 * 1024, 99);
  Publish(harness.stub, newer);
  harness.stub.SetFault({});
  harness.cancel = false;
  const auto outcome = harness.Run(root);
  CHECK(outcome.error == Error::kNone && outcome.build_id == game.build_id && outcome.meta_id == published.meta_id);
  CHECK(harness.stub.Count(Kind::kBuilds) == builds && harness.stub.Count(Kind::kMeta) == metas &&
        harness.stub.Count(Kind::kOwned) == owned);
  CHECK(harness.stub.CountHash(big.at(0).compressed_md5) == 1);  // not fetched again
  CHECK(harness.stub.CountHash(big.at(1).compressed_md5) == 2);
  CHECK(harness.stub.CountHash(big.at(2).compressed_md5) == 1);
  CHECK(harness.stub.CountHash(published.chunks.at("bin\\Game.exe").at(0).compressed_md5) == 1);
  CHECK(harness.last_progress.held == kGameExe.size() + 100 * 1024);
  CheckSyntheticFiles(root, false, kEnglishInfo);

  // A torn last journal line is ignored, and a finished file that is gone is fetched again.
  {
    FILE* journal = std::fopen((root + "/.playden-download/gog-journal").c_str(), "ab");
    CHECK(journal != nullptr);
    std::fputs("c\t9", journal);
    std::fclose(journal);
  }
  std::remove((root + "/music/theme.ogg").c_str());
  harness.stub.ClearRequests();
  CHECK(harness.Run(root).error == Error::kNone);
  int chunks = 0;
  for (const auto& request : harness.stub.requests()) chunks += request.kind == Kind::kChunk;
  CHECK(chunks == 1 && Contents(root + "/music/theme.ogg") == kTheme);

  // A finished file with another size is fetched whole again, not only checked.
  CHECK(truncate((root + "/data/big.dat").c_str(), 10) == 0);
  harness.stub.ClearRequests();
  const auto healed = harness.Run(root);
  CHECK(healed.error == Error::kNone && healed.stats.redone_files == 0);
  for (const auto& chunk : big) CHECK(harness.stub.CountHash(chunk.compressed_md5) == 1);
  CHECK(Contents(root + "/data/big.dat") == kBig);
}

void TestCanceledRemoval() {
  Harness harness;
  Publish(harness.stub, SyntheticGame());
  std::atomic<int> chunks{0};
  harness.stub.SetFault([&](const Request& request) -> std::optional<Fault> {
    if (request.kind != Kind::kChunk || chunks++ < 2) return std::nullopt;
    harness.cancel = true;
    Fault fault;
    fault.response = Failure(HttpError::kCanceled);
    return fault;
  });
  const auto root = Root("canceled");
  CHECK(harness.Run(root).error == Error::kCanceled);
  CHECK(Exists(root + "/.playden-files"));
  std::string failure;
  CHECK(playden::steam::RemoveManagedInstall(root, &failure));
  CHECK(!Exists(root));
}

void TestNoSpace() {
  Harness harness;
  Publish(harness.stub, SyntheticGame());
  harness.free = 1000;
  const auto root = Root("no-space");
  CHECK(harness.Run(root).error == Error::kNoSpace);
  CHECK(harness.stub.Count(Kind::kChunk) == 0 && harness.stub.Count(Kind::kSecureLink) == 0);
  CHECK(harness.Logged("gog download space short"));
  // Just enough: the missing bytes plus the margin.
  harness.free = (kGameExe.size() + kBig.size() + kDosboxConf.size() + kSave.size() + kTheme.size() +
                  kEnglishInfo.size() + kDlcShared.size() + kExtra.size()) +
                 playden::gog::kGogSpaceMargin;
  CHECK(harness.Run(root).error == Error::kNone);
}

// The dependency store:
// - DOSBox074_2CS: a game-folder dependency with GOG's real AUTHORS.txt chunk from the public store and a synthetic
//   DOSBox.exe;
// - XNA: an .msi __redist installer, `/qn`; DirectX: DXSETUP.exe (its manifest spells the prefix `__REDIST\`) and a
//   two-chunk cab, `/silent`;
// - NoProgram: a __redist depot without the program its executable.path names; OddArgs: arguments with a control
//   character.
const std::string kXna = RandomBytes(60 * 1024, 51);
const std::string kDxSetup = RandomBytes(20 * 1024, 52);
const std::string kDxCab = RandomBytes(2 * 32 * 1024, 53);
const std::string kNoProgramReadme = "no program in this depot\n";
const std::string kOddArgsExe = RandomBytes(8 * 1024, 54);
constexpr std::uint64_t kDependencyChunks = 2 + 1 + 3 + 1 + 1;
constexpr const char* kAllDependencies[] = {"DOSBox074_2CS", "XNA", "DirectX", "NoProgram", "OddArgs", "NotInTheStore"};
const std::string kRedistList =
    "gog-redist\tv1\n"
    "P\tXNA\t.playden-gog/redist/XNA/xnafx31_redist.msi\t/qn\n"
    "P\tDirectX\t.playden-gog/redist/DirectX/DXSETUP.exe\t/silent\n"
    "S\tNoProgram\tno_program\n"
    "S\tOddArgs\trefused\n"
    "S\tNotInTheStore\tmissing\n";

std::uint64_t DosboxSize() { return kDosbox.size() + 264; }
std::uint64_t RedistSize() {
  return kXna.size() + kDxSetup.size() + kDxCab.size() + kNoProgramReadme.size() + kOddArgsExe.size();
}

Published PublishDependencyStore(Harness& harness) {
  const auto zlib = ReadFile(g_fixtures + "/dependency-chunk-ede30538132b194f6a477a932441df95.zlib");
  CHECK(harness.stub.Put(zlib) == "ede30538132b194f6a477a932441df95");
  const auto exe = PutChunk(harness.stub, kDosbox);
  StoreDependency dosbox;
  dosbox.id = "DOSBox074_2CS";
  dosbox.depot_json = "{\"depot\":{\"items\":["
                      "{\"type\":\"DepotFile\",\"path\":\"DOSBOX/DOSBox.exe\",\"chunks\":[" + ChunkJson(exe) +
                      "],\"sha256\":" + Json(Sha256Hex(kDosbox)) + "},"
                      "{\"type\":\"DepotFile\",\"path\":\"DOSBOX/AUTHORS.txt\",\"chunks\":[{\"compressedMd5\":"
                      "\"ede30538132b194f6a477a932441df95\",\"compressedSize\":213,\"md5\":"
                      "\"699c1f3f9448f6db90735e1b00f2c954\",\"size\":264}]}]},\"version\":2}";
  dosbox.size = 1000;
  dosbox.compressed_size = 900;
  StoreDependency xna;
  xna.id = "XNA";
  xna.exe_path = "__redist/XNA/xnafx31_redist.msi";
  xna.arguments_json = "\"/qn\"";
  xna.files.push_back({"DepotFile", "__redist/XNA/xnafx31_redist.msi", kXna, {}, false, true, true});
  StoreDependency directx;
  directx.id = "DirectX";
  directx.exe_path = "__redist/DirectX/DXSETUP.exe";
  directx.arguments_json = "\"/silent\"";
  directx.files.push_back({"DepotFile", "__REDIST\\DirectX\\DXSETUP.exe", kDxSetup});
  directx.files.push_back({"DepotFile", "__redist/DirectX/Jun2010_d3dx9_43_x86.cab", kDxCab, {32 * 1024, 32 * 1024}});
  StoreDependency no_program;
  no_program.id = "NoProgram";
  no_program.exe_path = "__redist/NoProgram/setup.exe";
  no_program.arguments_json = "\"/quiet\"";
  no_program.files.push_back({"DepotFile", "__redist/NoProgram/readme.txt", kNoProgramReadme});
  StoreDependency odd;
  odd.id = "OddArgs";
  odd.exe_path = "__redist/OddArgs/setup.exe";
  odd.arguments_json = "\"/q\\u0007\"";
  odd.files.push_back({"DepotFile", "__redist/OddArgs/setup.exe", kOddArgsExe});
  return gog_test::PublishDependencyStore(harness.stub, {dosbox, xna, directx, no_program, odd});
}

// The files every dependency of PublishDependencyStore leaves, and the redist list.
void CheckDependencyFiles(const std::string& root) {
  CHECK(Contents(root + "/DOSBOX/DOSBox.exe") == kDosbox);
  const auto authors = Contents(root + "/DOSBOX/AUTHORS.txt");
  CHECK(authors && authors->size() == 264 && Md5Hex(*authors) == "699c1f3f9448f6db90735e1b00f2c954");
  CHECK(Contents(root + "/.playden-gog/redist/XNA/xnafx31_redist.msi") == kXna);
  CHECK(Contents(root + "/.playden-gog/redist/DirectX/DXSETUP.exe") == kDxSetup);
  CHECK(Contents(root + "/.playden-gog/redist/DirectX/Jun2010_d3dx9_43_x86.cab") == kDxCab);
  // A package Play can't run on its own is still downloaded.
  CHECK(Contents(root + "/.playden-gog/redist/NoProgram/readme.txt") == kNoProgramReadme);
  CHECK(Contents(root + "/.playden-gog/redist/OddArgs/setup.exe") == kOddArgsExe);
  // Nothing of them in the game folder itself.
  CHECK(!Exists(root + "/__redist") && !Exists(root + "/XNA") && !Exists(root + "/DirectX"));
  CHECK(Contents(root + "/.playden-gog/redist.list") == kRedistList);
  const auto list = playden::gog::ReadRedistList(root);
  CHECK(list.status == playden::gog::RedistList::Status::kOk && list.entries.size() == 5);
  const auto listed = ReadFile(root + "/.playden-files");
  for (const char* path :
       {"DOSBOX/DOSBox.exe", ".playden-gog/redist/XNA/xnafx31_redist.msi", ".playden-gog/redist/DirectX/DXSETUP.exe",
        ".playden-gog/redist/DirectX/Jun2010_d3dx9_43_x86.cab", ".playden-gog/redist/NoProgram/readme.txt",
        ".playden-gog/redist/OddArgs/setup.exe"})
    CHECK(listed.find(std::string("f\t") + path + "\n") != std::string::npos);
  const auto plan = ReadFile(root + "/.playden-gog/files");
  CHECK(plan.find("F\t.playden-gog/redist/XNA/xnafx31_redist.msi\tdep:XNA\t") != std::string::npos);
  CHECK(plan.find("F\t.playden-gog/redist/DirectX/DXSETUP.exe\tdep:DirectX\t") != std::string::npos);
}

std::uint64_t SyntheticSize() {
  return kGameExe.size() + kBig.size() + kDosboxConf.size() + kSave.size() + kTheme.size() + kEnglishInfo.size() +
         kDlcShared.size() + kExtra.size();
}

// A gen 2 build naming game-folder files, __redist installers and an ID the store doesn't list: DOSBox goes into the
// game folder, each installer under .playden-gog/redist/<ID>/, and redist.list says which program to run with which
// arguments. The plan, .playden-files, the totals and the space check count them.
void TestDependencies() {
  Harness harness;
  auto game = SyntheticGame();
  game.dependencies.assign(std::begin(kAllDependencies), std::end(kAllDependencies));
  game.dependencies.push_back("XNA");  // named twice, downloaded once
  PublishDependencyStore(harness);
  Publish(harness.stub, game);
  const auto root = Root("dependencies");
  const auto outcome = harness.Run(root);
  CHECK(outcome.error == Error::kNone);
  CheckSyntheticFiles(root, false, kEnglishInfo);
  CheckDependencyFiles(root);
  CHECK(outcome.install_bytes == SyntheticSize() + DosboxSize() + RedistSize());
  CHECK(harness.last_progress.done == harness.last_progress.total);
  CHECK(harness.stub.Count(Kind::kOpenLink) == 1 && harness.stub.Count(Kind::kRepository) == 1);
  CHECK(harness.stub.Count(Kind::kDependencyMeta) == 1 + 5);
  std::uint64_t dependency_chunks = 0;
  for (const auto& request : harness.stub.requests())
    if (request.kind == Kind::kDependencyChunk) {
      ++dependency_chunks;
      CHECK(request.host == "gog-cdn-fastly.gog.com" && request.authorization.empty());
    }
  CHECK(dependency_chunks == kDependencyChunks);
  CHECK(harness.Logged("gog plan dependencies game_folder=1 redist=4 missing=1"));
  harness.CheckClean();
}

// A build without dependencies still writes the list's header, so a later Play knows the plan had none.
void TestNoDependencies() {
  Harness harness;
  Publish(harness.stub, SyntheticGame());
  const auto root = Root("no-dependencies");
  CHECK(harness.Run(root).error == Error::kNone);
  CHECK(Contents(root + "/.playden-gog/redist.list") == std::string("gog-redist\tv1\n"));
  CHECK(harness.stub.Count(Kind::kRepository) == 0);
}

// Free space counts the installers: short by exactly their size, the download stops before any chunk.
void TestDependencySpace() {
  Harness harness;
  auto game = SyntheticGame();
  game.dependencies.assign(std::begin(kAllDependencies), std::end(kAllDependencies));
  PublishDependencyStore(harness);
  Publish(harness.stub, game);
  const auto root = Root("dependencies-space");
  harness.free = SyntheticSize() + DosboxSize() + playden::gog::kGogSpaceMargin;
  CHECK(harness.Run(root).error == Error::kNoSpace);
  CHECK(harness.stub.Count(Kind::kChunk) == 0 && harness.stub.Count(Kind::kDependencyChunk) == 0);
  CHECK(Contents(root + "/.playden-gog/redist.list") == kRedistList);
  harness.free = SyntheticSize() + DosboxSize() + RedistSize() + playden::gog::kGogSpaceMargin;
  CHECK(harness.Run(root).error == Error::kNone);
  CheckDependencyFiles(root);
}

// Cancel mid-way through the installers: the saved plan keeps its list, and the resume finishes from it without
// planning again. A plan made again (the old one gone) writes the list again.
void TestDependencyResume() {
  Harness harness;
  harness.parallel = 1;
  auto game = SyntheticGame();
  game.dependencies.assign(std::begin(kAllDependencies), std::end(kAllDependencies));
  const auto store = PublishDependencyStore(harness);
  Publish(harness.stub, game);
  const auto& cab = store.chunks.at("__redist/DirectX/Jun2010_d3dx9_43_x86.cab");
  harness.stub.SetFault([&](const Request& request) -> std::optional<Fault> {
    if (request.hash != cab.at(1).compressed_md5 || harness.cancel.load()) return std::nullopt;
    harness.cancel = true;
    Fault fault;
    fault.response = Failure(HttpError::kCanceled);
    return fault;
  });
  const auto root = Root("dependencies-resume");
  CHECK(harness.Run(root).error == Error::kCanceled);
  CHECK(Contents(root + "/.playden-gog/redist.list") == kRedistList);
  CHECK(harness.stub.CountHash(cab.at(0).compressed_md5) == 1);
  const int repositories = harness.stub.Count(Kind::kRepository);
  harness.stub.SetFault({});
  harness.cancel = false;
  CHECK(harness.Run(root).error == Error::kNone);
  CHECK(harness.Logged("gog plan reused"));
  CHECK(harness.stub.Count(Kind::kRepository) == repositories);
  CHECK(harness.stub.CountHash(cab.at(0).compressed_md5) == 1 && harness.stub.CountHash(cab.at(1).compressed_md5) == 2);
  CheckDependencyFiles(root);

  // Planning again replaces whatever list was there.
  {
    FILE* list = std::fopen((root + "/.playden-gog/redist.list").c_str(), "wb");
    CHECK(list != nullptr);
    std::fputs("gog-redist\tv1\nS\tStale\tmissing\n", list);
    std::fclose(list);
  }
  std::remove((root + "/.playden-gog/files").c_str());
  CHECK(harness.Run(root).error == Error::kNone);
  CHECK(harness.stub.Count(Kind::kRepository) == repositories + 1);
  CheckDependencyFiles(root);
}

// RemoveManagedInstall takes the installers and the list with the game.
void TestDependencyRemoval() {
  Harness harness;
  auto game = SyntheticGame();
  game.dependencies = {"XNA"};
  PublishDependencyStore(harness);
  Publish(harness.stub, game);
  const auto root = Root("dependencies-removal");
  CHECK(harness.Run(root).error == Error::kNone);
  CHECK(Exists(root + "/.playden-gog/redist/XNA/xnafx31_redist.msi") && Exists(root + "/.playden-gog/redist.list"));
  std::string failure;
  CHECK(playden::steam::RemoveManagedInstall(root, &failure));
  CHECK(!Exists(root));
}

// ---- Gen 1 builds -------------------------------------------------------------------------------------------------

constexpr std::uint64_t kMiB = 1024 * 1024;
const std::string kV1Exe = RandomBytes(300 * 1024, 41);
const std::string kV1Big = RandomBytes(25 * kMiB, 42);
const std::string kV1BaseShared = "shared text from the gen 1 base game\n";
const std::string kV1DlcShared = "shared text from the gen 1 DLC, which wins\n";
const std::string kV1Conf = "[dosbox]\nmemsize=32\n";
const std::string kV1Helper = RandomBytes(4096, 43);
const std::string kV1English = "english words\n";
const std::string kV1German = "deutsche Woerter\n";
const std::string kV1Info = "{\"language\":\"English\",\"gen\":1}\n";
const std::string kV1Extra = RandomBytes(20 * 1024, 44);

// Base: a Neutral depot (a `\` path, a file shared with the DLC, an empty file, a folder, a symlink record, two support
// files under the product's folder, and with `big` a 25 MiB file), an English depot with the info file, a German one.
// The DLC's depot comes first in the repository and replaces data\shared.txt in its own spelling.
GameV1 SyntheticGameV1(bool big = false) {
  GameV1 game;
  GameDepotV1 dlc;
  dlc.products = {kDlc};
  dlc.files.push_back({"Data/Shared.TXT", kV1DlcShared});
  dlc.files.push_back({"dlc/extra.pak", kV1Extra});
  game.depots.push_back(dlc);
  GameDepotV1 neutral;
  neutral.files.push_back({"bin\\Game.exe", kV1Exe});
  neutral.files.push_back({"data\\shared.txt", kV1BaseShared});
  neutral.files.push_back({"empty.dat", ""});
  GameFileV1 folder{"logs"};
  folder.directory = true;
  neutral.files.push_back(folder);
  GameFileV1 link{"shortcut.lnk"};
  link.symlink = true;
  neutral.files.push_back(link);
  neutral.files.push_back({"1900000001/app/x.conf", kV1Conf, true});
  neutral.files.push_back({"1900000001/galaxy_helper.exe", kV1Helper, true});
  if (big) neutral.files.push_back({"big/big.bin", kV1Big});
  game.depots.push_back(neutral);
  GameDepotV1 english;
  english.languages = {"English"};
  english.files.push_back({"lang/english.txt", kV1English});
  english.files.push_back({"/goggame-1900000001.info", kV1Info});
  game.depots.push_back(english);
  GameDepotV1 german;
  german.languages = {"German"};
  german.files.push_back({"lang/german.txt", kV1German});
  game.depots.push_back(german);
  return game;
}

std::uint64_t SyntheticV1Size(bool big, bool dlc = true, bool german = false) {
  return kV1Exe.size() + (dlc ? kV1DlcShared.size() + kV1Extra.size() : kV1BaseShared.size()) + kV1Conf.size() +
         kV1Helper.size() + (big ? kV1Big.size() : 0) + (german ? kV1German.size() : kV1English.size() + kV1Info.size());
}

void CheckSyntheticFilesV1(const std::string& root, bool big, bool dlc = true, bool german = false) {
  CHECK(Contents(root + "/bin/Game.exe") == kV1Exe);
  CHECK(Contents(root + "/empty.dat") == std::string());
  CHECK(IsDirectory(root + "/logs"));
  CHECK(!Exists(root + "/shortcut.lnk"));
  // <product>/app/x.conf goes to the game folder, <product>/x to the support folder.
  CHECK(Contents(root + "/x.conf") == kV1Conf);
  CHECK(!Exists(root + "/app") && !Exists(root + "/1900000001"));
  CHECK(Contents(root + "/.playden-gog/support/1900000001/galaxy_helper.exe") == kV1Helper);
  if (big) CHECK(Contents(root + "/big/big.bin") == kV1Big);
  if (dlc) {
    CHECK(Names(root + "/data") == std::vector<std::string>{"Shared.TXT"});
    CHECK(Contents(root + "/data/Shared.TXT") == kV1DlcShared);
    CHECK(Contents(root + "/dlc/extra.pak") == kV1Extra);
  } else {
    CHECK(Names(root + "/data") == std::vector<std::string>{"shared.txt"});
    CHECK(Contents(root + "/data/shared.txt") == kV1BaseShared);
    CHECK(!Exists(root + "/dlc"));
  }
  if (german) {
    CHECK(Contents(root + "/lang/german.txt") == kV1German && !Exists(root + "/lang/english.txt"));
    CHECK(!Exists(root + "/goggame-1900000001.info"));
  } else {
    CHECK(Contents(root + "/lang/english.txt") == kV1English && !Exists(root + "/lang/german.txt"));
    CHECK(Contents(root + "/goggame-1900000001.info") == kV1Info);
  }
}

// The Range headers asked of a product's main.bin, in order.
std::vector<std::string> RangesAsked(const std::vector<Request>& requests, std::uint64_t product = kBase) {
  std::vector<std::string> ranges;
  for (const auto& request : requests)
    if (request.kind == Kind::kRange && request.product == product) ranges.push_back(request.range);
  return ranges;
}

std::string RangeHeader(std::uint64_t first, std::uint64_t length) {
  return "bytes=" + std::to_string(first) + "-" + std::to_string(first + length - 1);
}

void TestGen1Install() {
  Harness harness;
  const auto game = SyntheticGameV1(true);
  const auto published = PublishV1(harness.stub, game);
  const auto root = Root("gen1-good");
  const auto outcome = harness.Run(root);
  CHECK(outcome.error == Error::kNone);
  CHECK(outcome.generation == 1 && outcome.timestamp == kTimestampV1 && outcome.build_id == "15751");
  CHECK(outcome.meta_id == published.repository_md5);
  CHECK(outcome.products == (std::vector<std::uint64_t>{kBase, kDlc}));
  CHECK(outcome.language == "en-US");
  CHECK(outcome.files == 9);
  CHECK(outcome.install_bytes == SyntheticV1Size(true));
  CheckSyntheticFilesV1(root, true);
  CHECK(harness.last_progress.done == harness.last_progress.total && harness.last_progress.total == SyntheticV1Size(true));

  const auto requests = harness.stub.requests();
  // The 25 MiB file as three ranges: 10, 10 and 5 MiB.
  const auto big = published.offsets.at("big/big.bin");
  std::vector<std::string> big_ranges;
  for (const auto& range : RangesAsked(requests))
    if (range.rfind("bytes=" + std::to_string(big), 0) == 0 || range.rfind("bytes=" + std::to_string(big + 10 * kMiB), 0) == 0 ||
        range.rfind("bytes=" + std::to_string(big + 20 * kMiB), 0) == 0)
      big_ranges.push_back(range);
  std::sort(big_ranges.begin(), big_ranges.end());
  std::vector<std::string> expected_big{RangeHeader(big, 10 * kMiB), RangeHeader(big + 10 * kMiB, 10 * kMiB),
                                        RangeHeader(big + 20 * kMiB, 5 * kMiB)};
  std::sort(expected_big.begin(), expected_big.end());
  CHECK(big_ranges == expected_big);
  // One range per other file of the base (the empty file has none), one per DLC file from the DLC's own main.bin.
  const auto base_ranges = RangesAsked(requests);
  CHECK(base_ranges.size() == 3 + 5 && RangesAsked(requests, kDlc).size() == 2);
  CHECK(std::count(base_ranges.begin(), base_ranges.end(),
                   RangeHeader(published.offsets.at("bin\\Game.exe"), kV1Exe.size())) == 1);
  int links = 0;
  for (const auto& request : requests) {
    CHECK(request.host != "gcore");  // gcore lists only gen 2
    CHECK(request.kind != Kind::kChunk && request.kind != Kind::kMeta && request.kind != Kind::kOpenLink);
    if (request.kind == Kind::kSecureLink) {
      ++links;
      CHECK(request.generation == 1 && request.timestamp == kTimestampV1);
    }
  }
  CHECK(links == 2);  // the base game's main.bin and the DLC's
  CHECK(harness.stub.Count(Kind::kBuilds) == 1 && harness.stub.Count(Kind::kRepositoryV1) == 1);
  CHECK(harness.stub.Count(Kind::kManifestV1) == 3);  // DLC, Neutral, English: not German
  CHECK(harness.Logged("gog plan build generation=1 depots=3 products=2 size="));
  CHECK(harness.Logged("gog plan support_commands=1 skipped"));
  CHECK(harness.Logged("links_skipped=1"));

  // The plan: its header, a file's ranges; the repository saved by its MD5, the manifests by name.
  const auto plan = Lines(ReadFile(root + "/.playden-gog/files"));
  CHECK(plan.at(0) == "v2\t1\t15751\t" + published.repository_md5 + "\ten-US\t1900000001,1900000002\t" +
                          std::to_string(SyntheticV1Size(true)) + "\t" + std::to_string(SyntheticV1Size(true)) +
                          "\t31000007");
  const auto big_line = std::find_if(plan.begin(), plan.end(), [](const std::string& line) {
    return line.rfind("F\tbig/big.bin\t1900000001\t", 0) == 0;
  });
  CHECK(big_line != plan.end() && *big_line == "F\tbig/big.bin\t1900000001\t" + std::to_string(kV1Big.size()) + "\t" +
                                                    Md5Hex(kV1Big) + "\t-\t3");
  CHECK(*(big_line + 1) == "R\t" + std::to_string(big) + "\t" + std::to_string(10 * kMiB));
  CHECK(*(big_line + 3) == "R\t" + std::to_string(big + 20 * kMiB) + "\t" + std::to_string(5 * kMiB));
  CHECK(std::find(plan.begin(), plan.end(), "F\tempty.dat\t1900000001\t0\t" + Md5Hex("") + "\t-\t0") != plan.end());
  CHECK(ReadFile(root + "/.playden-gog/meta/" + published.repository_md5) == published.repository);
  CHECK(Exists(root + "/.playden-gog/manifests/v1-00000000-0000-4000-8000-000000000002.json"));
  CHECK(!Exists(root + "/.playden-gog/manifests/v1-00000000-0000-4000-8000-000000000004.json"));

  // .playden-files names every placed path.
  const auto listed = Lines(ReadFile(root + "/.playden-files"));
  const std::set<std::string> listed_set(listed.begin(), listed.end());
  const std::set<std::string> expected{"f\tdata/Shared.TXT", "f\tdlc/extra.pak", "f\tbin/Game.exe", "f\tempty.dat",
                                       "d\tlogs", "f\tx.conf", "f\t.playden-gog/support/1900000001/galaxy_helper.exe",
                                       "f\tbig/big.bin", "f\tlang/english.txt", "f\tgoggame-1900000001.info"};
  CHECK(listed_set == expected && listed.size() == expected.size());
  harness.CheckClean();

  // A second run finds everything finished: no request at all.
  harness.stub.ClearRequests();
  const auto again = harness.Run(root);
  CHECK(again.error == Error::kNone && again.generation == 1 && harness.stub.requests().empty());
}

// German: the German depot instead of the English one. Only the base owned: the DLC's depot and its main.bin are left
// out, and the base game's file stays.
void TestGen1LanguageAndDlc() {
  {
    Harness harness;
    PublishV1(harness.stub, SyntheticGameV1());
    const auto root = Root("gen1-german");
    const auto outcome = harness.Run(root, "german");
    CHECK(outcome.error == Error::kNone && outcome.language == "de-DE");
    CheckSyntheticFilesV1(root, false, true, true);
    CHECK(outcome.install_bytes == SyntheticV1Size(false, true, true));
  }
  {
    Harness harness;
    harness.stub.SetOwned("{\"owned\":[1900000001]}");
    PublishV1(harness.stub, SyntheticGameV1());
    const auto root = Root("gen1-no-dlc");
    const auto outcome = harness.Run(root);
    CHECK(outcome.error == Error::kNone && outcome.products == std::vector<std::uint64_t>{kBase});
    CheckSyntheticFilesV1(root, false, false);
    CHECK(RangesAsked(harness.stub.requests(), kDlc).empty());
    for (const auto& request : harness.stub.requests())
      CHECK(request.kind != Kind::kSecureLink || request.product == kBase);
    harness.CheckClean();
  }
}

// A whole-file MD5 that doesn't match, on a file of one range: fetched once more, then corrupt. An empty file whose
// MD5 isn't the empty file's is corrupt at once.
void TestGen1FileCheck() {
  {
    Harness harness;
    auto game = SyntheticGameV1();
    game.depots[1].files[0].lie_md5 = true;  // bin\Game.exe
    const auto published = PublishV1(harness.stub, game);
    const auto outcome = harness.Run(Root("gen1-file-check"));
    CHECK(outcome.error == Error::kCorrupt && outcome.stats.redone_files == 1);
    const auto asked = RangesAsked(harness.stub.requests());
    CHECK(std::count(asked.begin(), asked.end(), RangeHeader(published.offsets.at("bin\\Game.exe"), kV1Exe.size())) == 2);
    harness.CheckClean();
  }
  {
    Harness harness;
    auto game = SyntheticGameV1();
    game.depots[1].files[2].lie_md5 = true;  // empty.dat
    PublishV1(harness.stub, game);
    const auto outcome = harness.Run(Root("gen1-empty-check"));
    CHECK(outcome.error == Error::kCorrupt && outcome.stats.redone_files == 0);
  }
}

// main.bin refused once (403): the gen 1 link is fetched again, with the gen 1 query, and the range comes.
void TestGen1LinkExpiry() {
  Harness harness;
  PublishV1(harness.stub, SyntheticGameV1());
  std::atomic<int> refused{0};
  harness.stub.SetFault([&](const Request& request) -> std::optional<Fault> {
    if (request.kind != Kind::kRange || request.product != kBase || refused++ > 0) return std::nullopt;
    Fault fault;
    fault.response = Answer(403, "");
    return fault;
  });
  const auto root = Root("gen1-403");
  CHECK(harness.Run(root).error == Error::kNone);
  int base_links = 0;
  for (const auto& request : harness.stub.requests())
    if (request.kind == Kind::kSecureLink) {
      CHECK(request.generation == 1 && request.timestamp == kTimestampV1);
      base_links += request.product == kBase;
    }
  CHECK(base_links == 2);
  CHECK(harness.Logged("gog range refused status=403"));
  CheckSyntheticFilesV1(root, false);
  harness.CheckClean();
}

// Canceled inside the 25 MiB file: the next run asks only for the ranges not finished, and keeps to the saved plan.
void TestGen1Resume() {
  Harness harness;
  harness.parallel = 1;
  const auto published = PublishV1(harness.stub, SyntheticGameV1(true));
  const auto big = published.offsets.at("big/big.bin");
  const auto second = RangeHeader(big + 10 * kMiB, 10 * kMiB);
  harness.stub.SetFault([&](const Request& request) -> std::optional<Fault> {
    if (request.kind != Kind::kRange || request.range != second || harness.cancel.load()) return std::nullopt;
    harness.cancel = true;
    Fault fault;
    fault.response = Failure(HttpError::kCanceled);
    return fault;
  });
  const auto root = Root("gen1-resume");
  CHECK(harness.Run(root).error == Error::kCanceled);
  std::set<std::string> finished;  // every range answered before the canceled one
  for (const auto& request : harness.stub.requests()) {
    if (request.kind != Kind::kRange) continue;
    if (request.range == second) break;
    finished.insert(std::to_string(request.product) + " " + request.range);
  }
  CHECK(finished.count("1900000001 " + RangeHeader(big, 10 * kMiB)));
  harness.stub.SetFault({});
  harness.stub.ClearRequests();
  harness.cancel = false;
  const auto outcome = harness.Run(root);
  CHECK(outcome.error == Error::kNone && outcome.generation == 1);
  const auto requests = harness.stub.requests();
  CHECK(harness.stub.Count(Kind::kBuilds) == 0 && harness.stub.Count(Kind::kRepositoryV1) == 0 &&
        harness.stub.Count(Kind::kManifestV1) == 0 && harness.stub.Count(Kind::kOwned) == 0);
  bool asked_second = false, asked_third = false;
  for (const auto& request : requests) {
    if (request.kind != Kind::kRange) continue;
    CHECK(!finished.count(std::to_string(request.product) + " " + request.range));
    asked_second = asked_second || request.range == second;
    asked_third = asked_third || request.range == RangeHeader(big + 20 * kMiB, 5 * kMiB);
  }
  CHECK(asked_second && asked_third);
  CHECK(harness.last_progress.done == harness.last_progress.total);
  CheckSyntheticFilesV1(root, true);
  harness.CheckClean();
}

// A depot whose gameIDs name an unowned DLC first and the base game after, with records that have no `url`: its
// manifest comes from the DLC's path (gameIDs[0]), but its files are the base game's: their ranges, secure link and
// support folder are the base game's, never the DLC's, which this account can't fetch.
void TestGen1DepotOfTwoProducts() {
  Harness harness;
  harness.stub.SetOwned("{\"owned\":[1900000001]}");
  GameV1 game;
  GameDepotV1 mixed;
  mixed.products = {kDlc, kBase};
  GameFileV1 exe{"bin\\Game.exe", kV1Exe};
  exe.blob = kBase;
  exe.no_url = true;
  mixed.files.push_back(exe);
  GameFileV1 helper{"1900000001/galaxy_helper.exe", kV1Helper, true};
  helper.blob = kBase;
  helper.no_url = true;
  mixed.files.push_back(helper);
  game.depots.push_back(mixed);
  PublishV1(harness.stub, game);
  const auto root = Root("gen1-two-products");
  const auto outcome = harness.Run(root);
  CHECK(outcome.error == Error::kNone && outcome.products == std::vector<std::uint64_t>{kBase});
  CHECK(Contents(root + "/bin/Game.exe") == kV1Exe);
  CHECK(Contents(root + "/.playden-gog/support/1900000001/galaxy_helper.exe") == kV1Helper);
  CHECK(harness.stub.Count(Kind::kManifestV1) == 1);
  for (const auto& request : harness.stub.requests())
    CHECK((request.kind != Kind::kSecureLink && request.kind != Kind::kRange) || request.product == kBase);
  CHECK(RangesAsked(harness.stub.requests()).size() == 2);
  harness.CheckClean();
}

void TestGen1NoSpace() {
  Harness harness;
  PublishV1(harness.stub, SyntheticGameV1());
  harness.free = 1000;
  const auto root = Root("gen1-no-space");
  CHECK(harness.Run(root).error == Error::kNoSpace);
  CHECK(harness.stub.Count(Kind::kRange) == 0 && harness.stub.Count(Kind::kSecureLink) == 0);
  harness.free = SyntheticV1Size(false) + playden::gog::kGogSpaceMargin;
  CHECK(harness.Run(root).error == Error::kNone);
  CheckSyntheticFilesV1(root, false);
}

// A gen 1 build's redist depots come from the dependency store too (gen 2 chunks through the open link): DOSBox074_2CS
// into the game folder, the installers under .playden-gog/redist/, with the same list.
void TestGen1Dependencies() {
  Harness harness;
  auto game = SyntheticGameV1();
  game.redists.assign(std::begin(kAllDependencies), std::end(kAllDependencies));
  PublishDependencyStore(harness);
  PublishV1(harness.stub, game);
  const auto root = Root("gen1-dependencies");
  const auto outcome = harness.Run(root);
  CHECK(outcome.error == Error::kNone && outcome.generation == 1);
  CheckSyntheticFilesV1(root, false);
  CheckDependencyFiles(root);
  CHECK(harness.stub.Count(Kind::kOpenLink) == 1 && harness.stub.Count(Kind::kRepository) == 1);
  CHECK(harness.stub.Count(Kind::kDependencyChunk) == static_cast<int>(kDependencyChunks));
  CHECK(harness.Logged("gog plan dependencies game_folder=1 redist=4 missing=1"));
  const auto plan = ReadFile(root + "/.playden-gog/files");
  CHECK(plan.find("\tdep:DOSBox074_2CS\t") != std::string::npos);
  CHECK(outcome.install_bytes == SyntheticV1Size(false) + DosboxSize() + RedistSize());
  harness.CheckClean();
}

// A repository Playden can't use: of another product, of another build, or not one at all.
void TestGen1Refusals() {
  for (int variant = 0; variant < 3; ++variant) {
    Harness harness;
    auto game = SyntheticGameV1();
    if (variant == 0) game.root = 1900000009;
    if (variant == 1) game.repository_timestamp = kTimestampV1 + 1;
    PublishV1(harness.stub, game);
    if (variant == 2)
      harness.stub.SetV1File("1900000001/windows/31000007/repository.json", "{\"product\":{},\"version\":1}");
    const auto root = Root("gen1-refused-" + std::to_string(variant));
    CHECK(harness.Run(root).error == Error::kProtocol);
    CHECK(harness.stub.Count(Kind::kManifestV1) == 0 && harness.stub.Count(Kind::kRange) == 0);
    CHECK(harness.Logged(variant == 2 ? "gog plan repository result=unreadable" : "gog plan repository result=other_build"));
    CHECK(playden::steam::RemoveManagedInstall(root) && !Exists(root));
  }
  {
    Harness harness;
    harness.stub.SetOwned("{\"owned\":[1900000002]}");
    PublishV1(harness.stub, SyntheticGameV1());
    CHECK(harness.Run(Root("gen1-not-owned")).error == Error::kNotOwned);
    CHECK(harness.stub.Count(Kind::kManifestV1) == 0);
  }
}

// A plan Phase 3 saved, with its "v1" header (gen 2, no timestamp), resumes as it is.
void TestPhase3PlanResumes() {
  Harness harness;
  harness.parallel = 1;
  const auto published = Publish(harness.stub, SyntheticGame());
  const auto& big = published.chunks.at("data\\big.dat");
  harness.stub.SetFault([&](const Request& request) -> std::optional<Fault> {
    if (request.hash != big.at(1).compressed_md5 || harness.cancel.load()) return std::nullopt;
    harness.cancel = true;
    Fault fault;
    fault.response = Failure(HttpError::kCanceled);
    return fault;
  });
  const auto root = Root("phase3-plan");
  CHECK(harness.Run(root).error == Error::kCanceled);
  const auto plan_path = root + "/.playden-gog/files";
  auto plan = ReadFile(plan_path);
  const auto end = plan.find('\n');
  auto header = plan.substr(0, end);
  CHECK(header.rfind("v2\t2\t", 0) == 0 && header.substr(header.size() - 2) == "\t-");
  header = "v1\t" + header.substr(5, header.size() - 7);
  CHECK(std::count(header.begin(), header.end(), '\t') == 6);
  plan = header + plan.substr(end);
  {
    FILE* file = std::fopen(plan_path.c_str(), "wb");
    CHECK(file && std::fwrite(plan.data(), 1, plan.size(), file) == plan.size() && std::fclose(file) == 0);
  }
  harness.stub.SetFault({});
  harness.stub.ClearRequests();
  harness.cancel = false;
  const auto outcome = harness.Run(root);
  CHECK(outcome.error == Error::kNone && outcome.generation == 2 && outcome.timestamp == 0);
  CHECK(outcome.meta_id == published.meta_id && harness.Logged("gog plan reused"));
  CHECK(harness.stub.Count(Kind::kBuilds) == 0 && harness.stub.CountHash(big.at(0).compressed_md5) == 0);
  CHECK(harness.stub.CountHash(big.at(1).compressed_md5) == 1);
  for (const auto& request : harness.stub.requests()) CHECK(request.kind != Kind::kSecureLink || request.generation == 2);
  CheckSyntheticFiles(root, false, kEnglishInfo);
  harness.CheckClean();
}

// Damaged plans: ranges under a gen 2 header or for a dependency, chunks for a product under a gen 1 header, a range
// too long, ranges that don't add up. Each is planned again from GOG.
void TestDamagedPlans() {
  Harness harness;
  const auto published = PublishV1(harness.stub, SyntheticGameV1());
  const auto root = Root("gen1-damaged");
  CHECK(harness.Run(root).error == Error::kNone);
  const auto plan_path = root + "/.playden-gog/files";
  const auto good = ReadFile(plan_path);
  const auto exe = "F\tbin/Game.exe\t1900000001\t" + std::to_string(kV1Exe.size()) + "\t" + Md5Hex(kV1Exe) + "\t-\t1\n";
  const auto range = "R\t" + std::to_string(published.offsets.at("bin\\Game.exe")) + "\t" + std::to_string(kV1Exe.size()) + "\n";
  CHECK(good.find(exe + range) != std::string::npos);
  const auto edited = [&](const std::string& from, const std::string& to) {
    auto text = good;
    const auto at = text.find(from);
    CHECK(at != std::string::npos);
    text.replace(at, from.size(), to);
    return text;
  };
  const auto header_end = good.find('\n');
  const auto header = good.substr(0, header_end);
  const auto gen2_header = "v2\t2\t" + header.substr(5, header.rfind('\t') - 5) + "\t-";
  const std::vector<std::string> damaged{
      gen2_header + good.substr(header_end),
      edited(exe + range, "F\tbin/Game.exe\tdep:DOSBox074_2CS\t" + std::to_string(kV1Exe.size()) + "\t" + Md5Hex(kV1Exe) +
                              "\t-\t1\n" + range),
      edited(exe + range, exe + "C\t" + Md5Hex("x") + "\t10\t" + Md5Hex(kV1Exe) + "\t" + std::to_string(kV1Exe.size()) + "\n"),
      edited(range, "R\t0\t" + std::to_string(playden::gog::kGen1RangeBytes + 1) + "\n"),
      edited(range, "R\t5\t" + std::to_string(kV1Exe.size() - 1) + "\n"),
      edited(range, "R\t18446744073709551615\t" + std::to_string(kV1Exe.size()) + "\n"),
  };
  for (const auto& text : damaged) {
    FILE* file = std::fopen(plan_path.c_str(), "wb");
    CHECK(file && std::fwrite(text.data(), 1, text.size(), file) == text.size() && std::fclose(file) == 0);
    harness.stub.ClearRequests();
    CHECK(harness.Run(root).error == Error::kStorage);
    CHECK(harness.Logged("gog plan damaged") && !Exists(plan_path));
    CHECK(harness.Run(root).error == Error::kNone && harness.stub.Count(Kind::kRepositoryV1) == 1);
    CHECK(ReadFile(plan_path) == good);
  }
  CheckSyntheticFilesV1(root, false);
}

void TestRefusals() {
  // The base game missing from the owned list.
  {
    Harness harness;
    Publish(harness.stub, SyntheticGame());
    harness.stub.SetOwned("{\"owned\":[1900000002]}");
    CHECK(harness.Run(Root("not-owned")).error == Error::kNotOwned);
    CHECK(harness.stub.Count(Kind::kSecureLink) == 0 && harness.stub.Count(Kind::kChunk) == 0);
  }
  // A 401 on the owned list drops the token and tries once more.
  {
    Harness harness;
    Publish(harness.stub, SyntheticGame());
    std::atomic<int> calls{0};
    harness.stub.SetFault([&](const Request& request) -> std::optional<Fault> {
      if (request.kind != Kind::kOwned || calls++ > 0) return std::nullopt;
      Fault fault;
      fault.response = Answer(401, "");
      return fault;
    });
    CHECK(harness.Run(Root("owned-401")).error == Error::kNone);
    CHECK(harness.invalidations == 1 && harness.stub.Count(Kind::kOwned) == 2);
  }
  // A game whose default build is gen 1 only installs (TestGen1* cover it in full), and its folder is Playden's to
  // remove.
  {
    Harness harness;
    PublishV1(harness.stub, SyntheticGameV1());
    const auto root = Root("gen1");
    const auto outcome = harness.Run(root);
    CHECK(outcome.error == Error::kNone && outcome.generation == 1);
    CHECK(harness.stub.Count(Kind::kMeta) == 0 && harness.stub.Count(Kind::kChunk) == 0);
    CHECK(playden::steam::RemoveManagedInstall(root) && !Exists(root));
  }
  // No builds at all.
  {
    Harness harness;
    CHECK(harness.Run(Root("no-build")).error == Error::kNoBuild);
  }
  // A token that can't be had.
  {
    Harness harness;
    Publish(harness.stub, SyntheticGame());
    auto hooks = harness.Hooks();
    hooks.token = [](const std::atomic<bool>&) {
      playden::gog::AccessToken token;
      token.status = playden::gog::AccessToken::Status::kRevoked;
      return token;
    };
    GogInstaller installer(hooks);
    GogInstaller::Target target{kBase, Root("revoked"), "english"};
    CHECK(installer.Run(target, harness.cancel).error == Error::kSignedOut);
  }
}

}  // namespace

int main(int argc, char** argv) {
  CHECK(argc == 3);
  g_fixtures = argv[1];
  g_scratch = argv[2];
  std::error_code ignored;
  fs::remove_all(g_scratch, ignored);
  CHECK(fs::create_directories(g_scratch));
  // The names RemoveManagedInstall and the queue's space check know.
  CHECK(std::string(playden::gog::kDownloadDirName) == playden::steam::DepotInstaller::kJournalDir);
  CHECK(std::string(playden::gog::kFileListName) == playden::steam::DepotInstaller::kFileList);
  CHECK(playden::gog::kGogSpaceMargin == playden::steam::kInstallSpaceMargin);

  TestLinkFixtures();
  TestRealDependencyChunk();
  TestBadHashesAndLimits();
  TestFailover();
  TestRanges();
  TestGoodInstall();
  TestLanguage();
  TestOddLanguage();
  TestCorruptRetried();
  TestInflatedHashFails();
  TestFileCheckFails();
  TestEmptyFileCheckFails();
  TestSlowCdn();
  TestLinkExpiry();
  TestResume();
  TestCanceledRemoval();
  TestNoSpace();
  TestDependencies();
  TestNoDependencies();
  TestDependencySpace();
  TestDependencyResume();
  TestDependencyRemoval();
  TestRefusals();
  TestGen1Install();
  TestGen1LanguageAndDlc();
  TestGen1FileCheck();
  TestGen1LinkExpiry();
  TestGen1Resume();
  TestGen1NoSpace();
  TestGen1DepotOfTwoProducts();
  TestGen1Dependencies();
  TestGen1Refusals();
  TestPhase3PlanResumes();
  TestDamagedPlans();
  fs::remove_all(g_scratch, ignored);
  std::puts("gog_download: ok");
  return 0;
}
