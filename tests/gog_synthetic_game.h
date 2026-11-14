// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// A synthetic GOG gen 2 game for the downloader's and the install runner's tests: files with chunks compressed by
// miniz, its depot manifests, build meta and builds answer, published into the stub CDN (gog_cdn_stub.h). Synthetic
// data only.

#include "gog/depot_reader.h"
#include "gog_cdn_stub.h"

#include <cstddef>
#include <cstdint>
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

}  // namespace gog_test
