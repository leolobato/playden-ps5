// SPDX-License-Identifier: GPL-3.0-or-later
// The GOG gen 2 downloader (gog/chunk_fetch, gog/installer) against a stub CDN (gog_cdn_stub.h): a synthetic game made
// here (random files, chunks compressed with miniz, its manifests, meta and builds), plus GOG's public dependency chunk
// and open link answers from fixtures/gog. Usage: gog_download_test <fixtures/gog> <scratch folder>.

#include "gog/api.h"
#include "gog/chunk_fetch.h"
#include "gog/content.h"
#include "gog/installer.h"
#include "gog_cdn_stub.h"
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

constexpr std::uint64_t kBase = 1900000001;
constexpr std::uint64_t kDlc = 1900000002;
constexpr std::int64_t kNow = 1800000000;  // well before the fixture link's expires_at (1900000000)

std::string RandomBytes(std::size_t size, std::uint32_t seed) {
  std::string bytes(size, '\0');
  std::uint32_t state = seed * 2654435761u + 1;
  for (auto& byte : bytes) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    byte = static_cast<char>(state & 0xff);
  }
  return bytes;
}

std::string Json(const std::string& text) {
  std::string out = "\"";
  for (const char c : text) {
    if (c == '\\' || c == '"') out += '\\';
    out += c;
  }
  return out + "\"";
}

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

// ---- A synthetic game ---------------------------------------------------------------------------------------------

struct GameFile {
  std::string type = "DepotFile";  // DepotDirectory, DepotLink
  std::string path;                // raw, as the manifest writes it
  std::string bytes;
  std::vector<std::size_t> pieces;  // chunk sizes; empty: one chunk (none for an empty file)
  bool support = false;
  bool file_md5 = true;
  bool sha256 = false;
  int level = 6;
  int lie_chunk = -1;  // this chunk's md5 in the manifest is wrong
  bool lie_file_md5 = false;  // the whole file's md5 in the manifest is wrong
};

struct GameDepot {
  std::uint64_t product = kBase;
  std::vector<std::string> languages{"*"};
  bool gog = false;
  std::vector<GameFile> files;
};

struct Game {
  std::string build_id = "1000000000000000101";
  std::vector<GameDepot> depots;
  std::vector<std::string> dependencies;
};

struct Published {
  std::string meta_id;
  std::map<std::string, std::vector<DepotChunk>> chunks;  // by raw path
};

std::string ChunkJson(const DepotChunk& chunk) {
  return "{\"compressedMd5\":" + Json(chunk.compressed_md5) + ",\"compressedSize\":" +
         std::to_string(chunk.compressed_size) + ",\"md5\":" + Json(chunk.md5) +
         ",\"size\":" + std::to_string(chunk.size) + "}";
}

// The manifest JSON of a depot; its chunks go into the stub.
std::string DepotManifest(CdnStub& stub, const std::vector<GameFile>& files, Published* published,
                          std::uint64_t* size, std::uint64_t* compressed) {
  std::string items;
  for (const auto& file : files) {
    if (!items.empty()) items += ",";
    if (file.type == "DepotDirectory") {
      items += "{\"type\":\"DepotDirectory\",\"path\":" + Json(file.path) + "}";
      continue;
    }
    if (file.type == "DepotLink") {
      items += "{\"type\":\"DepotLink\",\"path\":" + Json(file.path) + ",\"target\":\"bin/Game.exe\"}";
      continue;
    }
    std::vector<std::size_t> pieces = file.pieces;
    if (pieces.empty() && !file.bytes.empty()) pieces.push_back(file.bytes.size());
    std::vector<DepotChunk> chunks;
    std::size_t at = 0;
    std::string chunk_json;
    for (std::size_t i = 0; i < pieces.size(); ++i) {
      const auto raw = file.bytes.substr(at, pieces[i]);
      at += pieces[i];
      const auto zlib = Zlib(raw, file.level);
      DepotChunk chunk;
      chunk.compressed_md5 = stub.Put(zlib);
      chunk.compressed_size = zlib.size();
      chunk.md5 = static_cast<int>(i) == file.lie_chunk ? Md5Hex(raw + "lie") : Md5Hex(raw);
      chunk.size = raw.size();
      *size += chunk.size;
      *compressed += chunk.compressed_size;
      if (!chunk_json.empty()) chunk_json += ",";
      chunk_json += ChunkJson(chunk);
      chunks.push_back(chunk);
    }
    CHECK(at == file.bytes.size());
    published->chunks[file.path] = chunks;
    items += "{\"type\":\"DepotFile\",\"path\":" + Json(file.path) + ",\"chunks\":[" + chunk_json + "]";
    if (file.file_md5 && !file.bytes.empty())
      items += ",\"md5\":" + Json(Md5Hex(file.lie_file_md5 ? file.bytes + "lie" : file.bytes));
    if (file.sha256) items += ",\"sha256\":" + Json(Sha256Hex(file.bytes));
    if (file.support) items += ",\"flags\":[\"support\"]";
    items += "}";
  }
  return "{\"depot\":{\"items\":[" + items + "]},\"version\":2}";
}

std::string BuildsJson(std::uint64_t product, const std::string& build_id, const std::string& meta_id) {
  const auto endpoint = [&](const std::string& name, const std::string& base, int priority, bool fallback,
                            const std::string& generations, const std::string& meta) {
    const auto url = base + "/content-system/v2/meta/" + Galaxy(meta);
    return "{\"endpoint_name\":" + Json(name) + ",\"url\":" + Json(url) + ",\"url_format\":" + Json(url) +
           ",\"parameters\":{},\"priority\":" + std::to_string(priority) +
           ",\"max_fails\":100,\"supports_generation\":" + generations +
           ",\"fallback_only\":" + (fallback ? "true" : "false") + "}";
  };
  const std::string other_meta(32, 'e');
  return "{\"total_count\":2,\"count\":2,\"items\":["
         "{\"build_id\":\"1000000000000000999\",\"product_id\":\"" + std::to_string(product) +
         "\",\"os\":\"windows\",\"branch\":\"beta\",\"generation\":2,\"urls\":[" +
         endpoint("fastly", kFastlyBase, 10, false, "[1,2]", other_meta) + "]},"
         "{\"build_id\":\"" + build_id + "\",\"product_id\":\"" + std::to_string(product) +
         "\",\"os\":\"windows\",\"branch\":null,\"generation\":2,\"urls\":[" +
         endpoint("fastly", kFastlyBase, 10, false, "[1,2]", meta_id) + "," +
         endpoint("gcore", kGcoreBase, 1, true, "[2]", meta_id) + "]}],\"has_private_branches\":false}";
}

Published Publish(CdnStub& stub, const Game& game) {
  Published published;
  std::string depots;
  for (const auto& depot : game.depots) {
    std::uint64_t size = 0, compressed = 0;
    const auto manifest = Zlib(DepotManifest(stub, depot.files, &published, &size, &compressed));
    const auto id = stub.Put(manifest);
    std::string languages;
    for (const auto& language : depot.languages) languages += (languages.empty() ? "" : ",") + Json(language);
    if (!depots.empty()) depots += ",";
    depots += "{\"compressedSize\":" + std::to_string(compressed) + ",\"languages\":[" + languages +
              "],\"manifest\":" + Json(id) + ",\"productId\":\"" + std::to_string(depot.product) +
              "\",\"size\":" + std::to_string(size) + (depot.gog ? ",\"isGogDepot\":true" : "") + "}";
  }
  std::string dependencies;
  for (const auto& id : game.dependencies) dependencies += (dependencies.empty() ? "" : ",") + Json(id);
  const auto meta = "{\"baseProductId\":\"" + std::to_string(kBase) + "\",\"buildId\":\"" + game.build_id +
                    "\",\"clientId\":\"50000000000000001\",\"dependencies\":[" + dependencies + "],\"depots\":[" +
                    depots + "],\"installDirectory\":\"Synthetic\",\"platform\":\"windows\",\"products\":["
                    "{\"name\":\"Synthetic Game\",\"productId\":\"" + std::to_string(kBase) + "\"},"
                    "{\"name\":\"Synthetic DLC\",\"productId\":\"" + std::to_string(kDlc) + "\"}],\"version\":2}";
  published.meta_id = stub.Put(Zlib(meta));
  stub.SetBuilds(kBase, BuildsJson(kBase, game.build_id, published.meta_id));
  return published;
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
  CHECK(Lines(ReadFile(root + "/.playden-gog/files")).at(0).rfind("v1\t" + game.build_id + "\t" + published.meta_id +
                                                                  "\ten-US\t1900000001,1900000002\t", 0) == 0);
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

void TestDependencies() {
  Harness harness;
  auto game = SyntheticGame();
  game.dependencies = {"DOSBox074_2CS", "MSVC2017"};
  // A game-folder dependency with GOG's real AUTHORS.txt chunk from the public store, and a synthetic DOSBox.exe.
  const auto zlib = ReadFile(g_fixtures + "/dependency-chunk-ede30538132b194f6a477a932441df95.zlib");
  CHECK(harness.stub.Put(zlib) == "ede30538132b194f6a477a932441df95");
  const auto exe = PutChunk(harness.stub, kDosbox);
  const auto depot = Zlib("{\"depot\":{\"items\":["
                          "{\"type\":\"DepotFile\",\"path\":\"DOSBOX/DOSBox.exe\",\"chunks\":[" + ChunkJson(exe) +
                          "],\"sha256\":" + Json(Sha256Hex(kDosbox)) + "},"
                          "{\"type\":\"DepotFile\",\"path\":\"DOSBOX/AUTHORS.txt\",\"chunks\":[{\"compressedMd5\":"
                          "\"ede30538132b194f6a477a932441df95\",\"compressedSize\":213,\"md5\":"
                          "\"699c1f3f9448f6db90735e1b00f2c954\",\"size\":264}]}]},\"version\":2}");
  const auto depot_id = harness.stub.Put(depot);
  const auto repository = Zlib("{\"depots\":["
                               "{\"dependencyId\":\"DOSBox074_2CS\",\"executable\":{\"path\":\"\",\"arguments\":\"\"},"
                               "\"languages\":[\"*\"],\"manifest\":" + Json(depot_id) + ",\"size\":1000,"
                               "\"compressedSize\":900,\"signature\":\"x\"},"
                               "{\"dependencyId\":\"MSVC2017\",\"executable\":{\"path\":\"__redist/MSVC2017/VC_redist.x86.exe\","
                               "\"arguments\":\"/quiet\"},\"languages\":[\"*\"],\"manifest\":"
                               "\"00000000000000000000000000000001\",\"size\":10,\"compressedSize\":9}]}");
  const auto repository_id = harness.stub.Put(repository);
  harness.stub.SetRepository("{\"repository_manifest\":" +
                             Json(kFastlyBase + "/content-system/v2/dependencies/meta/" + Galaxy(repository_id)) +
                             ",\"build_id\":\"60163282911088704\",\"generation\":2}");
  Publish(harness.stub, game);
  const auto root = Root("dependencies");
  const auto outcome = harness.Run(root);
  CHECK(outcome.error == Error::kNone);
  CheckSyntheticFiles(root, false, kEnglishInfo);
  CHECK(Contents(root + "/DOSBOX/DOSBox.exe") == kDosbox);
  const auto authors = Contents(root + "/DOSBOX/AUTHORS.txt");
  CHECK(authors && authors->size() == 264 && Md5Hex(*authors) == "699c1f3f9448f6db90735e1b00f2c954");
  CHECK(harness.stub.Count(Kind::kOpenLink) == 1 && harness.stub.Count(Kind::kRepository) == 1);
  int dependency_chunks = 0;
  for (const auto& request : harness.stub.requests())
    if (request.kind == Kind::kDependencyChunk) {
      ++dependency_chunks;
      CHECK(request.host == "gog-cdn-fastly.gog.com" && request.authorization.empty());
    }
  CHECK(dependency_chunks == 2);
  CHECK(harness.Logged("game_folder=1 redist_skipped=1 missing=0"));
  const auto listed = ReadFile(root + "/.playden-files");
  CHECK(listed.find("f\tDOSBOX/DOSBox.exe\n") != std::string::npos);
  CHECK(ReadFile(root + "/.playden-gog/files").find("\tdep:DOSBox074_2CS\t") != std::string::npos);
  harness.CheckClean();
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
  // A gen 1 only game: nothing downloaded, and the folder is still Playden's to remove.
  {
    Harness harness;
    harness.stub.SetBuilds(kBase, ReadFile(g_fixtures + "/builds-v2-gen1-only-1425039730.json"));
    const auto root = Root("gen1");
    CHECK(harness.Run(root).error == Error::kGen1Only);
    CHECK(harness.stub.Count(Kind::kMeta) == 0 && harness.stub.Count(Kind::kOwned) == 0);
    CHECK(Names(root) == (std::vector<std::string>{".playden-download", ".playden-gog"}));
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
  TestGoodInstall();
  TestLanguage();
  TestCorruptRetried();
  TestInflatedHashFails();
  TestFileCheckFails();
  TestSlowCdn();
  TestLinkExpiry();
  TestResume();
  TestCanceledRemoval();
  TestNoSpace();
  TestDependencies();
  TestRefusals();
  fs::remove_all(g_scratch, ignored);
  std::puts("gog_download: ok");
  return 0;
}
