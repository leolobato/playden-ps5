// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// A synthetic GOG game for the downloader's and the install runner's tests, published into the stub CDN
// (gog_cdn_stub.h). Gen 2: files with chunks compressed by miniz, its depot manifests, build meta and builds answer.
// Gen 1: its files laid out in one main.bin per product, its plain depot manifests, repository.json and a gen 1 builds
// item. Synthetic data only.

#include "gog/depot_reader.h"
#include "gog_cdn_stub.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace gog_test {

using playden::gog::DepotChunk;

inline constexpr std::uint64_t kBase = 1900000001;
inline constexpr std::uint64_t kDlc = 1900000002;

inline std::string RandomBytes(std::size_t size, std::uint32_t seed) {
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

inline std::string Json(const std::string& text) {
  std::string out = "\"";
  for (const char c : text) {
    if (c == '\\' || c == '"') out += '\\';
    out += c;
  }
  return out + "\"";
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
  bool lie_sha256 = false;    // the whole file's sha256 in the manifest is wrong
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

inline std::string ChunkJson(const DepotChunk& chunk) {
  return "{\"compressedMd5\":" + Json(chunk.compressed_md5) + ",\"compressedSize\":" +
         std::to_string(chunk.compressed_size) + ",\"md5\":" + Json(chunk.md5) +
         ",\"size\":" + std::to_string(chunk.size) + "}";
}

// The manifest JSON of a depot; its chunks go into the stub.
inline std::string DepotManifest(CdnStub& stub, const std::vector<GameFile>& files, Published* published,
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
    if (file.sha256) items += ",\"sha256\":" + Json(Sha256Hex(file.lie_sha256 ? file.bytes + "lie" : file.bytes));
    if (file.support) items += ",\"flags\":[\"support\"]";
    items += "}";
  }
  return "{\"depot\":{\"items\":[" + items + "]},\"version\":2}";
}

inline std::string BuildsJson(std::uint64_t product, const std::string& build_id, const std::string& meta_id) {
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

inline Published Publish(CdnStub& stub, const Game& game) {
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

// ---- A synthetic gen 1 game ---------------------------------------------------------------------------------------

inline constexpr std::uint64_t kTimestampV1 = 31000007;  // the gen 1 secure link fixture's

struct GameFileV1 {
  std::string path;   // raw, as the manifest writes it
  std::string bytes;
  bool support = false;
  bool directory = false;
  bool symlink = false;  // a `symlinkType` record
  std::uint64_t blob = 0;  // the product whose main.bin holds it; 0: the depot's first product
  bool lie_md5 = false;    // the manifest's hash is wrong
  bool no_url = false;     // the record has no `url` (the blob is still `blob`'s)
};

struct GameDepotV1 {
  std::vector<std::uint64_t> products{kBase};  // gameIDs
  std::vector<std::string> languages{"Neutral"};
  std::vector<std::string> systems{"Windows"};
  std::vector<GameFileV1> files;
};

struct GameV1 {
  std::string build_id = "15751";
  std::uint64_t timestamp = kTimestampV1;
  std::vector<GameDepotV1> depots;
  std::vector<std::string> redists;  // redist IDs; "__redist/<id>/setup.exe" for any but language_setup and DOSBox ones
  std::uint64_t root = kBase;        // rootGameID (another one makes a repository of another product)
  std::uint64_t repository_timestamp = 0;  // product.timestamp; 0: `timestamp`
  int support_commands = 1;
};

struct PublishedV1 {
  std::string repository_md5;
  std::string repository;
  std::map<std::uint64_t, std::string> blobs;  // main.bin per product
  std::map<std::string, std::uint64_t> offsets;  // by raw path: where the file starts in its blob
};

inline std::string BuildsJsonV1(std::uint64_t product, const std::string& build_id, std::uint64_t timestamp) {
  const auto endpoint = [&](const std::string& name, const std::string& base, int priority, bool fallback,
                            const std::string& generations) {
    const auto url = base + "/content-system/v1/manifests/" + std::to_string(product) + "/windows/" +
                     std::to_string(timestamp) + "/repository.json";
    return "{\"endpoint_name\":" + Json(name) + ",\"url\":" + Json(url) + ",\"url_format\":" + Json(url) +
           ",\"parameters\":{},\"priority\":" + std::to_string(priority) +
           ",\"max_fails\":100,\"supports_generation\":" + generations +
           ",\"fallback_only\":" + (fallback ? "true" : "false") + "}";
  };
  return "{\"total_count\":1,\"count\":1,\"items\":[{\"build_id\":\"" + build_id + "\",\"product_id\":\"" +
         std::to_string(product) + "\",\"os\":\"windows\",\"branch\":null,\"generation\":1,\"urls\":[" +
         endpoint("fastly", kFastlyBase, 10, false, "[1,2]") + "," + endpoint("gcore", kGcoreBase, 1, true, "[2]") +
         "],\"legacy_build_id\":" + std::to_string(timestamp) + "}],\"has_private_branches\":false}";
}

inline std::string StringList(const std::vector<std::string>& values) {
  std::string out;
  for (const auto& value : values) out += (out.empty() ? "" : ",") + Json(value);
  return "[" + out + "]";
}

// Publishes the game: each file's bytes appended to its blob product's main.bin (after a few bytes of padding, so
// offsets matter), the manifests and repository.json at their gen 1 paths, the builds answer for the root.
inline PublishedV1 PublishV1(CdnStub& stub, const GameV1& game) {
  PublishedV1 published;
  const std::string folder = "/windows/" + std::to_string(game.timestamp) + "/";
  std::string depots;
  std::uint32_t seed = 7000;
  for (std::size_t index = 0; index < game.depots.size(); ++index) {
    const auto& depot = game.depots[index];
    std::string files;
    std::uint64_t size = 0;
    for (const auto& file : depot.files) {
      if (!files.empty()) files += ",";
      if (file.directory) {
        files += "{\"path\":" + Json(file.path) + ",\"directory\":true}";
        continue;
      }
      if (file.symlink) {
        files += "{\"path\":" + Json(file.path) + ",\"symlinkType\":\"file\",\"target\":\"bin/Game.exe\"}";
        continue;
      }
      const auto blob_product = file.blob ? file.blob : depot.products.front();
      auto& blob = published.blobs[blob_product];
      blob += RandomBytes(17, ++seed);
      const auto offset = blob.size();
      blob += file.bytes;
      published.offsets[file.path] = offset;
      size += file.bytes.size();
      files += "{\"offset\":" + std::to_string(offset) + ",\"path\":" + Json(file.path) +
               ",\"size\":" + std::to_string(file.bytes.size()) +
               (file.support ? ",\"support\":true" : "") +
               ",\"hash\":" + Json(Md5Hex(file.lie_md5 ? file.bytes + "lie" : file.bytes)) +
               (file.no_url ? std::string() : ",\"url\":" + Json(std::to_string(blob_product) + "/main.bin")) + "}";
    }
    char name[64];
    std::snprintf(name, sizeof(name), "00000000-0000-4000-8000-%012zu.json", index + 1);
    stub.SetV1File(std::to_string(depot.products.front()) + folder + name,
                   "{\"version\":1,\"depot\":{\"name\":\"Synthetic Game\",\"files\":[" + files + "]}}");
    std::string ids;
    for (const auto product : depot.products) ids += (ids.empty() ? "" : ",") + Json(std::to_string(product));
    if (!depots.empty()) depots += ",";
    depots += "{\"languages\":" + StringList(depot.languages) + ",\"size\":" + Json(std::to_string(size)) +
              ",\"gameIDs\":[" + ids + "],\"systems\":" + StringList(depot.systems) +
              ",\"manifest\":" + Json(name) + "}";
  }
  for (const auto& id : game.redists) {
    depots += ",{\"redist\":" + Json(id);
    if (id != "language_setup" && id.rfind("DOSBox", 0) != 0) depots += ",\"executable\":" + Json("__redist/" + id + "/setup.exe");
    depots += ",\"size\":\"0\"}";
  }
  std::string commands;
  for (int i = 0; i < game.support_commands; ++i)
    commands += std::string(i ? "," : "") +
                "{\"languages\":[\"Neutral\"],\"executable\":\"/galaxy_synthetic.exe\",\"gameID\":" +
                Json(std::to_string(kBase)) + ",\"argument\":\"\",\"systems\":[\"Windows\"]}";
  const auto timestamp = game.repository_timestamp ? game.repository_timestamp : game.timestamp;
  published.repository =
      "{\"product\":{\"timestamp\":" + std::to_string(timestamp) + ",\"depots\":[" + depots +
      "],\"support_commands\":[" + commands + "],\"installDirectory\":\"Synthetic\",\"rootGameID\":" +
      Json(std::to_string(game.root)) + ",\"gameIDs\":[{\"gameID\":" + Json(std::to_string(kBase)) +
      ",\"name\":{\"en\":\"Synthetic Game\"},\"dependencies\":[],\"standalone\":true},{\"gameID\":" +
      Json(std::to_string(kDlc)) + ",\"name\":{\"en\":\"Synthetic DLC\"},\"dependencies\":[],\"standalone\":false}],"
      "\"projectName\":\"Synthetic Game\"},\"version\":1}";
  published.repository_md5 = Md5Hex(published.repository);
  stub.SetV1File(std::to_string(kBase) + folder + "repository.json", published.repository);
  for (const auto& blob : published.blobs) stub.SetBlob(blob.first, blob.second);
  stub.SetBuilds(kBase, BuildsJsonV1(kBase, game.build_id, game.timestamp));
  return published;
}

// ---- The public dependency store ----------------------------------------------------------------------------------

// One entry of the dependency repository manifest and its depot. `arguments_json` is the JSON value of
// executable.arguments as it is written (so a test can give a control character or a number).
struct StoreDependency {
  std::string id;
  std::string exe_path;  // "" for game-folder files
  std::string arguments_json = "\"\"";
  std::vector<GameFile> files;
  std::string depot_json;  // the depot manifest as it is, instead of one made from `files`
  std::uint64_t size = 0;  // with depot_json: the sizes the repository states
  std::uint64_t compressed_size = 0;
};

// The repository answer, its manifest and every dependency's depot manifest and chunks. Returns the chunks by raw path.
inline Published PublishDependencyStore(CdnStub& stub, const std::vector<StoreDependency>& dependencies) {
  Published published;
  std::string entries;
  for (const auto& dependency : dependencies) {
    std::uint64_t size = dependency.size, compressed = dependency.compressed_size;
    const auto depot = dependency.depot_json.empty()
                           ? DepotManifest(stub, dependency.files, &published, &size, &compressed)
                           : dependency.depot_json;
    const auto id = stub.Put(Zlib(depot));
    if (!entries.empty()) entries += ",";
    entries += "{\"dependencyId\":" + Json(dependency.id) + ",\"executable\":{\"path\":" + Json(dependency.exe_path) +
               ",\"arguments\":" + dependency.arguments_json + "},\"languages\":[\"*\"],\"manifest\":" + Json(id) +
               ",\"size\":" + std::to_string(size) + ",\"compressedSize\":" + std::to_string(compressed) +
               ",\"signature\":\"x\"}";
  }
  const auto repository_id = stub.Put(Zlib("{\"depots\":[" + entries + "]}"));
  stub.SetRepository("{\"repository_manifest\":" +
                     Json(kFastlyBase + "/content-system/v2/dependencies/meta/" + Galaxy(repository_id)) +
                     ",\"build_id\":\"60163282911088704\",\"generation\":2}");
  return published;
}

}  // namespace gog_test
