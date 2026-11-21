#include "runtime/publisher_pack.h"

#include "runtime/artifact_hash.h"
#include "runtime/tzst_archive.h"
#include "steam/backend/json.h"

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <filesystem>
#include <optional>
#include <set>
#include <sys/stat.h>
#include <unistd.h>

namespace playden::runtime {
namespace {

namespace fs = std::filesystem;
namespace json = steam::json;

constexpr const char* kReleaseMarker = ".playden-release";
constexpr const char* kFilesMarker = ".playden-files.json";
constexpr const char* kStagingPrefix = ".installing-";
// Where the Steam host archive may keep its files (the macOS single-directory rule).
constexpr const char* kNestedSteamRoot = "home/xuser/.wine/drive_c/Program Files (x86)/Steam";
constexpr std::size_t kMaxMarkerBytes = 8 * 1024 * 1024;

bool Fail(std::string* reason, std::string text) {
  if (reason) *reason = std::move(text);
  return false;
}

std::string Lower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c); });
  return text;
}

std::string Stem(const std::string& archive) {
  constexpr std::string_view suffix = ".tzst";
  return archive.size() > suffix.size() && archive.compare(archive.size() - suffix.size(), suffix.size(), suffix) == 0
             ? archive.substr(0, archive.size() - suffix.size())
             : archive;
}

std::optional<std::string> ReadSmall(const fs::path& path, std::size_t limit) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) return std::nullopt;
  struct stat info {};
  std::optional<std::string> result;
  if (::fstat(fd, &info) == 0 && S_ISREG(info.st_mode) && static_cast<std::uint64_t>(info.st_size) <= limit) {
    std::string bytes(static_cast<std::size_t>(info.st_size), '\0');
    std::size_t at = 0;
    while (at < bytes.size()) {
      const auto count = ::read(fd, bytes.data() + at, bytes.size() - at);
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) break;
      at += static_cast<std::size_t>(count);
    }
    if (at == bytes.size()) result = std::move(bytes);
  }
  ::close(fd);
  return result;
}

bool WriteSmall(const fs::path& path, const std::string& bytes) {
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0644);
  if (fd < 0) return false;
  std::size_t at = 0;
  while (at < bytes.size()) {
    const auto count = ::write(fd, bytes.data() + at, bytes.size() - at);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) break;
    at += static_cast<std::size_t>(count);
  }
  const bool ok = at == bytes.size() && ::fsync(fd) == 0;
  return ::close(fd) == 0 && ok;
}

bool IsRegular(const fs::path& path) {
  struct stat info {};
  return ::lstat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
}

// The archive rules of PublisherAssets.validateArchivePaths: relative, no drive or Windows
// separators, no empty, "." or ".." parts, no control characters.
bool SafeArchivePath(const std::string& path) {
  if (path.empty() || path.front() == '/' || path.size() > 1024) return false;
  for (unsigned char c : path)
    if (c < 0x20 || c == ':' || c == '\\' || c == 0x7f) return false;
  std::size_t begin = 0;
  for (;;) {
    const auto end = path.find('/', begin);
    const auto part = path.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
    if (part.empty() || part == "." || part == "..") return false;
    if (end == std::string::npos) return true;
    begin = end + 1;
  }
}

std::string JsonEscape(const std::string& text) {
  std::string out;
  for (char c : text) {
    if (c == '"' || c == '\\') out += '\\';
    out += c;
  }
  return out;
}

// Relative path -> SHA-256 of every regular file below root (markers excluded). Fails on links,
// special files or an unreadable file.
bool Digests(const fs::path& root, std::map<std::string, std::string>* digests) {
  digests->clear();
  std::error_code error;
  fs::recursive_directory_iterator it(root, fs::directory_options::none, error), end;
  if (error) return false;
  for (; it != end; it.increment(error)) {
    if (error) return false;
    const auto status = it->symlink_status(error);
    if (error) return false;
    if (fs::is_directory(status)) continue;
    if (!fs::is_regular_file(status)) return false;
    auto relative = it->path().lexically_relative(root).generic_string();
    if (relative == kReleaseMarker || relative == kFilesMarker) continue;
    const auto digest = HashArtifactFile(it->path());
    if (!digest) return false;
    (*digests)[std::move(relative)] = *digest;
  }
  return !error;
}

void RemoveStaging(const fs::path& cache_root) {
  std::error_code error;
  for (fs::directory_iterator it(cache_root, error), end; !error && it != end; it.increment(error)) {
    const auto name = it->path().filename().string();
    if (name.rfind(kStagingPrefix, 0) == 0) fs::remove_all(it->path(), error);
  }
}

// Writes one archive's entries below stage, keeping the path rules.
class Unpacker {
 public:
  explicit Unpacker(fs::path stage) : stage_(std::move(stage)) {}
  ~Unpacker() { Close(); }

  bool Entry(const TarEntry& entry, std::string* reason) {
    if (!Close()) return Fail(reason, "cannot write " + current_);
    auto path = entry.path;
    while (path.rfind("./", 0) == 0) path.erase(0, 2);
    if (path.empty() || path == ".") return true;
    // The pinned Steam archive once carried an empty directory whose name has a literal
    // backslash; it is dropped (PublisherAssetCache.prepare).
    if (entry.type == '5' && (path == "bin\\shaders" || path == std::string(kNestedSteamRoot) + "/bin\\shaders"))
      return true;
    if (!SafeArchivePath(path)) return Fail(reason, "the archive holds an unsafe path: " + path);
    if (!seen_.insert(Lower(path)).second) return Fail(reason, "the archive holds a path twice: " + path);
    const auto target = stage_ / path;
    std::error_code error;
    if (entry.type == '5') {
      fs::create_directories(target, error);
      return !error || Fail(reason, "cannot create " + path);
    }
    fs::create_directories(target.parent_path(), error);
    if (error) return Fail(reason, "cannot create the folder of " + path);
    fd_ = ::open(target.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd_ < 0 || ::fchmod(fd_, 0644) != 0) return Fail(reason, "cannot create " + path);
    current_ = path;
    return true;
  }

  bool Data(const char* data, std::size_t size) {
    while (size > 0) {
      const auto count = ::write(fd_, data, size);
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) return false;
      data += count;
      size -= static_cast<std::size_t>(count);
    }
    return true;
  }

  bool Close() {
    if (fd_ < 0) return true;
    const bool ok = ::close(fd_) == 0;
    fd_ = -1;
    return ok;
  }

 private:
  fs::path stage_;
  std::set<std::string> seen_;
  int fd_ = -1;
  std::string current_;
};

}  // namespace

const std::vector<PublisherRelease>& PinnedPublisherReleases() {
  static const std::vector<PublisherRelease> releases = {
      {Publisher::kSteamHost, "playden-steamhost-20261121.1.tzst",
       "25e381710a1d03e5191c450e1fdced0934f48678cbaee04ac1db49de4afa81db",
       "59c6cfbdca76e2cf44a2c1b7472edf0ff16fe248551de4bfed13f620e4bb1312", {"steam.exe", "steamclient64.dll"}},
      {Publisher::kEa, "playden-eahost-20261121.1.tzst",
       "db0bf6a6ae8d5747ce738618a3b64145e93ef2e7cc7dba4ce6ebe71a7190ac7c",
       "13266376838a6d85bc1c2d830c772a24ce609957b15593282fcda3e9defd8f68", {"pdstub-ea.exe", "pd-ea.properties"}},
      {Publisher::kRockstar, "playden-rgschost-20261121.1.tzst",
       "18864f63998b12310f984b9e0fff1e6f3bc4526e7a7efe6e59c675263a789f5c",
       "b56b72fadd8c632c3bb2596976204a7fe3c96d2861757c54d14fd8b9e7305901",
       {"pdstub-sc.exe", "pdstub-sc32.exe", "pdlaunch-sc.dll", "pdlaunch-sc32.dll", "pdlaunch-helper.exe", "pdscpatch.dll", "pdscpatch32.dll",
        "bink2w64.dll", "binkw32.dll", "SocialClubD3D12Renderer.dll", "SocialClubVulkanLayer.dll",
        "playden-signin-shim.js", "stage.json", "signin/index.html"}},
      {Publisher::kUbisoft, "playden-ubisoft-20261121.1.tzst",
       "e35815adc1f8e212fac43b9ecea0ee585035ba56972354ece5d39edbf7766662",
       "b28b6ce315160ab2194c990d57456c3c2c40f76b472d3f09ffa313b19812496a",
       {"pdstub-ubi.exe", "uplay_r264.dll", "uplay_r2.dll", "ubiorbitapi_r2.dll", "uplay_r1.dll", "uplay_aux_r1.dll"}},
  };
  return releases;
}

const char* PublisherName(Publisher publisher) {
  switch (publisher) {
    case Publisher::kSteamHost: return "Steam host";
    case Publisher::kEa: return "EA";
    case Publisher::kRockstar: return "Rockstar";
    case Publisher::kUbisoft: return "Ubisoft";
  }
  return "";
}

std::string PublisherTreeDigest(const std::map<std::string, std::string>& file_sha256) {
  std::string data;
  for (const auto& [name, digest] : file_sha256) {
    data += name;
    data += '\0';
    data += digest;
    data += '\0';
  }
  return HashArtifactBytes(data).value_or(std::string());
}

PublisherPackStatus CheckPublisherPack(const std::string& pack_root) {
  return CheckPublisherPack(pack_root, kPublisherPackVersion, PinnedPublisherReleases());
}

PublisherPackStatus CheckPublisherPack(const std::string& pack_root, const std::string& version,
                                       const std::vector<PublisherRelease>& releases) {
  PublisherPackStatus status;
  for (const auto& release : releases) status.verified[release.publisher] = false;
  const fs::path root(pack_root);
  const auto release_json = ReadSmall(root / version / "release.json", 64 * 1024);
  if (!release_json) {
    std::error_code error;
    for (fs::directory_iterator it(root, error), end; !error && it != end; it.increment(error)) {
      const auto name = it->path().filename().string();
      if (name.empty() || name[0] == '.' || name == "cache" || !IsRegular(it->path() / "release.json")) continue;
      status.present = true;
      status.found_version = name;
      break;
    }
    status.message = status.present
        ? "Publisher pack " + status.found_version + " is installed, but this Playden needs version " + version + "."
        : "No publisher pack is installed.";
    return status;
  }
  status.present = true;
  status.version_matches = true;
  const auto parsed = json::Parse(*release_json);
  const json::Value* artifacts = parsed && parsed->is_object() ? parsed->Find("artifacts") : nullptr;
  const json::Value* trees = parsed && parsed->is_object() ? parsed->Find("tree_sha256") : nullptr;
  const json::Value* stated = parsed && parsed->is_object() ? parsed->Find("version") : nullptr;
  if (!artifacts || !artifacts->is_object() || !stated || !stated->is_string() || stated->text() != version) {
    status.message = "The publisher pack's release.json is damaged.";
    return status;
  }
  // A pack may hold only some publishers' archives (the builds page hosts the Rockstar one alone): an archive that
  // isn't there is not installed, one that doesn't match its pin is damaged.
  std::string absent, failed;
  for (const auto& release : releases) {
    if (!IsRegular(root / version / release.archive)) {
      absent += std::string(absent.empty() ? "" : ", ") + PublisherName(release.publisher);
      continue;
    }
    const auto* listed = artifacts->Find(release.archive);
    const auto* tree = trees && trees->is_object() ? trees->Find(release.archive) : nullptr;
    bool ok = listed && listed->is_string() && listed->text() == release.sha256 &&
              (!tree || (tree->is_string() && tree->text() == release.tree_sha256));
    if (ok) {
      const auto digest = HashArtifactFile(root / version / release.archive);
      ok = digest && *digest == release.sha256;
    }
    status.verified[release.publisher] = ok;
    if (!ok) failed += std::string(failed.empty() ? "" : ", ") + PublisherName(release.publisher);
  }
  status.message = "Publisher pack " + version;
  if (failed.empty() && absent.empty()) status.message += " is ready.";
  if (!failed.empty()) status.message += ": " + failed + " damaged.";
  if (!absent.empty()) status.message += (failed.empty() ? ": " : " ") + absent + " not installed.";
  return status;
}

bool PublisherSupported(const PublisherPackStatus& status, Publisher publisher) {
  const auto has = [&](Publisher p) {
    const auto found = status.verified.find(p);
    return found != status.verified.end() && found->second;
  };
  // EA and Ubisoft games start through the Steam host; Rockstar's helpers run on their own.
  const bool needs_steam_host = publisher == Publisher::kEa || publisher == Publisher::kUbisoft;
  return status.version_matches && has(publisher) && (!needs_steam_host || has(Publisher::kSteamHost));
}

bool PreparePublisherRelease(const std::string& pack_root, const std::string& cache_root,
                             const PublisherRelease& release, std::string* directory, std::string* reason) {
  return PreparePublisherRelease(pack_root, kPublisherPackVersion, cache_root, release, directory, reason);
}

bool PreparePublisherRelease(const std::string& pack_root, const std::string& version, const std::string& cache_root,
                             const PublisherRelease& release, std::string* directory, std::string* reason) {
  const auto archive = fs::path(pack_root) / version / release.archive;
  const auto target = fs::path(cache_root) / Stem(release.archive);
  const auto digest = HashArtifactFile(archive);
  if (!digest || *digest != release.sha256)
    return Fail(reason, std::string(PublisherName(release.publisher)) + " archive is missing or damaged.");
  if (PublisherCacheValid(target.string(), release)) {
    *directory = target.string();
    return true;
  }
  std::error_code error;
  fs::create_directories(cache_root, error);
  if (error) return Fail(reason, "cannot create the publisher cache");
  RemoveStaging(cache_root);
  const auto stage = fs::path(cache_root) / (kStagingPrefix + Stem(release.archive));
  fs::create_directories(stage, error);
  if (error) return Fail(reason, "cannot create the publisher cache");
  const auto abandon = [&](std::string text) {
    std::error_code ignored;
    fs::remove_all(stage, ignored);
    return Fail(reason, std::move(text));
  };

  Unpacker unpacker(stage);
  std::string why;
  const bool read = ReadTzst(
      archive.string(), [&](const TarEntry& entry) { return unpacker.Entry(entry, &why); },
      [&](const char* data, std::size_t size) { return unpacker.Data(data, size); }, &why);
  if (!read || !unpacker.Close()) return abandon(why.empty() ? "cannot unpack " + release.archive : why);

  // Required files at the root, or (Steam host) in its nested Wine folder.
  const auto has_required = [&](const fs::path& root) {
    return std::all_of(release.required.begin(), release.required.end(),
                       [&](const std::string& name) { return IsRegular(root / name); });
  };
  fs::path content = stage;
  if (!has_required(stage)) {
    content = stage / kNestedSteamRoot;
    if (!has_required(content)) return abandon(release.archive + " is incomplete.");
  }
  std::map<std::string, std::string> digests;
  if (!Digests(content, &digests)) return abandon("cannot read the unpacked " + release.archive);
  if (PublisherTreeDigest(digests) != release.tree_sha256)
    return abandon(release.archive + " does not hold the pinned files.");
  std::string files = "{";
  for (const auto& [name, sha] : digests)
    files += std::string(files.size() > 1 ? "," : "") + "\"" + JsonEscape(name) + "\":\"" + sha + "\"";
  files += "}";
  if (!WriteSmall(content / kFilesMarker, files) || !WriteSmall(content / kReleaseMarker, release.sha256))
    return abandon("cannot record the publisher cache");
  fs::remove_all(target, error);
  if (error) return abandon("cannot replace the publisher cache");
  fs::rename(content, target, error);
  if (error) return abandon("cannot finish the publisher cache");
  fs::remove_all(stage, error);
  if (!PublisherCacheValid(target.string(), release)) return Fail(reason, "the publisher cache did not verify");
  *directory = target.string();
  return true;
}

bool PublisherCacheValid(const std::string& directory, const PublisherRelease& release) {
  const fs::path root(directory);
  const auto marker = ReadSmall(root / kReleaseMarker, 128);
  if (!marker || *marker != release.sha256) return false;
  const auto recorded_text = ReadSmall(root / kFilesMarker, kMaxMarkerBytes);
  if (!recorded_text) return false;
  json::Limits limits;
  limits.max_bytes = kMaxMarkerBytes;
  limits.max_values = 200000;
  const auto parsed = json::Parse(*recorded_text, limits);
  if (!parsed || !parsed->is_object()) return false;
  std::map<std::string, std::string> recorded;
  for (const auto& [name, value] : parsed->object()) {
    if (!value.is_string()) return false;
    recorded[name] = value.text();
  }
  for (const auto& name : release.required)
    if (!recorded.count(name)) return false;
  std::map<std::string, std::string> actual;
  return Digests(root, &actual) && actual == recorded && PublisherTreeDigest(actual) == release.tree_sha256;
}

}  // namespace playden::runtime
