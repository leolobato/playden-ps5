// SPDX-License-Identifier: GPL-3.0-or-later
// The saved gamesdb entries under <user root>/gog/products: the file format, 0666 files in 0777 folders (#868; 0600 ones from before are read), links and
// unsafe files refused, a corrupt file ignored, and Clear removing every regular file (temporary ones too) without
// following a link. Built twice: on the host's descriptor-relative paths, and with the console's absolute-path policy
// (gog_product_cache_native_policy). Synthetic data only.
#include "gog/product_cache.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(condition)                                                                  \
  do {                                                                                    \
    if (!(condition)) {                                                                   \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
      std::exit(1);                                                                       \
    }                                                                                     \
  } while (false)

using namespace playden;
using gog::CachedProduct;
using gog::ProductCache;
namespace fs = std::filesystem;

namespace {

fs::path g_base;
int g_roots = 0;

fs::path FreshRoot() {
  const fs::path root = g_base / ("root-" + std::to_string(++g_roots));
  fs::create_directories(root);
  CHECK(::chmod(root.c_str(), 0755) == 0);
  return root;
}

mode_t ModeOf(const fs::path& path) {
  struct stat info{};
  CHECK(::lstat(path.c_str(), &info) == 0);
  return info.st_mode & 07777;
}

bool Exists(const fs::path& path) {
  struct stat info{};
  return ::lstat(path.c_str(), &info) == 0;
}

void WriteBytes(const fs::path& path, const std::string& bytes, mode_t mode = 0600) {
  FILE* file = std::fopen(path.c_str(), "wb");
  CHECK(file != nullptr);
  CHECK(std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size());
  CHECK(std::fclose(file) == 0);
  CHECK(::chmod(path.c_str(), mode) == 0);
}

const std::string kCover = "https://images.gog.com/" + std::string(64, 'a') + ".jpg?namespace=gamesdb";
const std::string kHero =
    "https://images.gog.com/" + std::string(64, 'b') + "_bg_crop_1920x655.jpg?namespace=gamesdb";

CachedProduct Game() {
  CachedProduct product;
  product.etag = "W/\"0123abcd\"";
  product.kind = gog::ProductInfo::Kind::kGame;
  product.title = "Synthetic Game";
  product.summary = "A synthetic summary.";
  product.developer = "Synthetic Studio";
  product.cover = kCover;
  product.hero = kHero;
  return product;
}

bool Same(const CachedProduct& a, const CachedProduct& b) {
  return a.etag == b.etag && a.kind == b.kind && a.title == b.title && a.summary == b.summary &&
         a.developer == b.developer && a.cover == b.cover && a.hero == b.hero;
}

void Format() {
  const auto bytes = gog::EncodeProduct(Game());
  CHECK(!bytes.empty());
  CHECK(std::string(bytes.begin(), bytes.begin() + 8) == std::string("PDGOGP\0\1", 8));
  CachedProduct decoded;
  CHECK(gog::DecodeProduct(bytes, &decoded) && Same(decoded, Game()));
  CachedProduct hidden;
  hidden.kind = gog::ProductInfo::Kind::kHidden;
  CHECK(gog::DecodeProduct(gog::EncodeProduct(hidden), &decoded) && Same(decoded, hidden));
  // Every byte flipped, every truncation and a trailing byte are refused.
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    auto bad = bytes;
    bad[i] ^= 0x01;
    CHECK(!gog::DecodeProduct(bad, &decoded));
  }
  for (std::size_t size = 0; size < bytes.size(); ++size)
    CHECK(!gog::DecodeProduct(Bytes(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(size)), &decoded));
  auto longer = bytes;
  longer.push_back(0);
  CHECK(!gog::DecodeProduct(longer, &decoded));
  // Out-of-range fields are never written.
  auto bad = Game();
  bad.cover = "https://example.invalid/a.jpg";
  CHECK(gog::EncodeProduct(bad).empty());
  bad = Game();
  bad.cover = kHero;  // each slot takes its own shape
  CHECK(gog::EncodeProduct(bad).empty());
  bad = Game();
  bad.hero = kCover;
  CHECK(gog::EncodeProduct(bad).empty());
  bad = Game();
  bad.etag = "W/\"a\"\r\n";
  CHECK(gog::EncodeProduct(bad).empty());
  bad = Game();
  bad.title = " ";
  CHECK(gog::EncodeProduct(bad).empty());
  bad = Game();
  bad.kind = gog::ProductInfo::Kind::kInvalid;
  CHECK(gog::EncodeProduct(bad).empty());
  bad = Game();
  bad.summary.assign(gog::kMaxSummaryBytes + 1, 's');
  CHECK(gog::EncodeProduct(bad).empty());
}

void SaveLoadRemove() {
  const auto root = FreshRoot();
  ProductCache cache(root.string());
  // Nothing saved, nothing created.
  CHECK(!cache.Load(1351891846));
  CHECK(!Exists(root / "gog"));
  CHECK(cache.Remove(1351891846));
  CHECK(cache.Clear());
  CHECK(!Exists(root / "gog"));

  CHECK(cache.Save(1351891846, Game()));
  CHECK(ModeOf(root / "gog") == 0777 && ModeOf(root / "gog" / "products") == 0777);  // #868
  CHECK(ModeOf(root / "gog" / "products" / "1351891846") == 0666);
  auto loaded = cache.Load(1351891846);
  CHECK(loaded && Same(*loaded, Game()));
  CHECK(!cache.Load(1351891847));
  auto replaced = Game();
  replaced.etag.clear();
  replaced.title = "Replaced";
  CHECK(cache.Save(1351891846, replaced));
  CHECK(cache.Load(1351891846)->title == "Replaced");
  CHECK(cache.Remove(1351891846));
  CHECK(!Exists(root / "gog" / "products" / "1351891846"));
  CHECK(!cache.Save(0, Game()));
}

void Refusals() {
  const auto root = FreshRoot();
  ProductCache cache(root.string());
  CHECK(cache.Save(1, Game()));
  const auto products = root / "gog" / "products";
  // A corrupt file is ignored (it is fetched again and replaced).
  WriteBytes(products / "2", "not a product");
  CHECK(!cache.Load(2));
  CHECK(cache.Save(2, Game()));
  CHECK(cache.Load(2));
  // Any mode of the app user's is read (#868: a recursive chmod over FTP may leave anything).
  CHECK(::chmod((products / "2").c_str(), 0777) == 0);
  CHECK(cache.Load(2));
  // A link to a valid product is never followed.
  fs::create_symlink(products / "1", products / "3");
  CHECK(!cache.Load(3));
  // A link in place of the products folder: nothing read, written or removed through it.
  const auto other = FreshRoot();
  CHECK(ProductCache(other.string()).Save(9, Game()));
  const auto linked = FreshRoot();
  fs::create_directories(linked / "gog");
  CHECK(::chmod((linked / "gog").c_str(), 0700) == 0);
  fs::create_directory_symlink(other / "gog" / "products", linked / "gog" / "products");
  ProductCache through(linked.string());
  CHECK(!through.Load(9));
  CHECK(!through.Save(10, Game()));
  CHECK(!through.Clear());
  CHECK(Exists(other / "gog" / "products" / "9") && !Exists(other / "gog" / "products" / "10"));
  // A gog folder of the app user's at another mode is Playden's own (#868): used as it is.
  const auto open = FreshRoot();
  fs::create_directories(open / "gog");
  CHECK(::chmod((open / "gog").c_str(), 0755) == 0);
  CHECK(ProductCache(open.string()).Save(1, Game()));
  CHECK(ModeOf(open / "gog") == 0755);
  // A relative or unnormalized user root is refused.
  CHECK(!ProductCache("relative/root").Save(1, Game()));
  CHECK(!ProductCache((root / "." / "x" / "..").string()).Save(1, Game()));
}

void ClearAll() {
  const auto root = FreshRoot();
  ProductCache cache(root.string());
  for (std::uint64_t id = 1; id <= 5; ++id) CHECK(cache.Save(id, Game()));
  const auto products = root / "gog" / "products";
  WriteBytes(products / ".7.journal-1-0", "leftover");  // what a failed durable write leaves
  CHECK(cache.Clear());
  CHECK(fs::is_empty(products));
  // A link in the folder is neither followed nor removed, and the result says so; the files around it still go.
  const auto target = FreshRoot() / "outside";
  WriteBytes(target, "outside");
  CHECK(cache.Save(1, Game()));
  fs::create_symlink(target, products / "2");
  CHECK(!cache.Clear());
  CHECK(!Exists(products / "1") && fs::is_symlink(products / "2") && Exists(target));
  // The session file beside products/ is not the cache's.
  WriteBytes(root / "gog" / "session", "session");
  fs::remove(products / "2");
  CHECK(cache.Clear());
  CHECK(Exists(root / "gog" / "session"));
}

}  // namespace

int main(int argc, char** argv) {
  // A folder under the build tree, given by ctest, so no component is a link (macOS /tmp is one).
  CHECK(argc == 2);
  const fs::path parent(argv[1]);
  fs::remove_all(parent);
  fs::create_directories(parent);
  g_base = fs::canonical(parent);
  Format();
  SaveLoadRemove();
  Refusals();
  ClearAll();
  fs::remove_all(g_base);
  std::printf("gog product cache ok\n");
  return 0;
}
