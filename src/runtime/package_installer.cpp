#include "runtime/package_installer.h"
#include "runtime/kit_trust.h"
#include "runtime/private_directories.h"
#include "platform/managed_directory.h"
#include "pw_title_publication_native.h"
#include "pw_title_publication.h"

#include "steam/backend/json.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <array>
#include <cerrno>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <vector>
#include <system_error>
#include <thread>
#include <utility>

#include <sys/stat.h>
#include <sys/file.h>

#if defined(__PROSPERO__) || (defined(PLAYDEN_PLATFORM_PS5) && PLAYDEN_PLATFORM_PS5)
extern "C" int playden_ps5_file_lock(int descriptor, int operation);
#endif
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <memory>

namespace playden::runtime {
namespace fs = std::filesystem;

struct FilesystemPackageInstaller::Mapping {
  const KitFile* file = nullptr;
  std::string source_relative;
  std::string destination_relative;
  // Absolute source. Kit files come from the verified kit; a game's own title
  // metadata (name and icon) comes from its private title-metadata folder, and
  // then owned describes it with the kit file's destination mode.
  fs::path source;
  std::shared_ptr<const KitFile> owned;
};

namespace {
// The title files a game replaces with its own metadata, and the marker that
// says its metadata was generated.
constexpr const char* kTitleMetadataFiles[] = {"sce_sys/param.json", "sce_sys/icon0.png"};
constexpr const char* kTitleMetadataMarker = ".generated";
}  // namespace

struct FilesystemPackageInstaller::ResolvedPaths {
  fs::path title;
  fs::path game;
  fs::path library;
  fs::path bottle;
  fs::path title_staging;
  fs::path bottle_staging;
  PublicationPaths publication;
};

namespace {

constexpr std::uint64_t kCopyOverheadFloor = 1024 * 1024;
// Larger reads and writes per call: package copies run at about 11 MB/s on console
// storage with 64 KiB.
constexpr std::size_t kCopyBufferBytes = 1024 * 1024;

// Time spent in each named part of a package copy or check, for the console's
// runtime log ("source_hash_ms=… copy_ms=…", in the order the parts first ran).
class PartTimes {
 public:
  using Clock = std::chrono::steady_clock;
  void Add(const char* part, Clock::time_point since) {
    const auto spent = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - since).count();
    for (auto& [name, micros] : parts_)
      if (name == part) { micros += static_cast<std::uint64_t>(spent); return; }
    parts_.emplace_back(part, static_cast<std::uint64_t>(spent));
  }
  std::string Summary(Clock::time_point started) const {
    std::string text = "total_ms=" + Millis(Clock::now() - started);
    for (const auto& [name, micros] : parts_) text += " " + name + "_ms=" + std::to_string(micros / 1000);
    return text;
  }
 private:
  static std::string Millis(Clock::duration spent) {
    return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(spent).count());
  }
  std::vector<std::pair<std::string, std::uint64_t>> parts_;
};
constexpr std::size_t kMaxPublicationBytes = 8192;
constexpr char kTitleOwner[] = ".playden-title-owner";
constexpr char kTemplateOwner[] = ".playden-template-owner";
constexpr char kStageOwner[] = ".playden-stage-owner";

PackageActionResult Complete() {
  return {PackageActionState::kComplete, {}, {}};
}

PackageActionResult Failed(std::string code, std::string message) {
  return {PackageActionState::kFailed, std::move(code), std::move(message)};
}

PackageActionResult Canceled() {
  return {PackageActionState::kCanceled, "canceled", "package installation was canceled"};
}

bool IsHexDigest(const std::string& value) {
  return value.size() == 64 &&
         std::all_of(value.begin(), value.end(), [](unsigned char byte) {
           return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f');
         });
}

bool IsManagedTitle(const std::string& value) {
  if (value.size() != 9 || value.compare(0, 4, "FAKE") != 0) return false;
  if (!std::all_of(value.begin() + 4, value.end(), [](unsigned char byte) {
        return byte >= '0' && byte <= '9';
      }))
    return false;
  return value.compare("FAKE78000") >= 0 && value.compare("FAKE78999") <= 0;
}

bool IsSafeRelative(const std::string& value) {
  if (value.empty() || value.find('\\') != std::string::npos) return false;
  const fs::path path(value);
  if (path.is_absolute() || path.root_name() != fs::path{}) return false;
  for (const auto& part : path)
    if (part.empty() || part == "." || part == "..") return false;
  return path.generic_string() == value;
}

bool HasPrefix(const std::string& path, const std::string& prefix,
               std::string* suffix = nullptr) {
  if (path.size() <= prefix.size() || path.compare(0, prefix.size(), prefix) != 0 ||
      path[prefix.size()] != '/')
    return false;
  if (suffix) *suffix = path.substr(prefix.size() + 1);
  return true;
}

bool IsRegularNoSymlink(const fs::path& path) {
  std::error_code error;
  const auto status = fs::symlink_status(path, error);
  return !error && fs::is_regular_file(status) && !fs::is_symlink(status);
}

bool ExistsNoThrow(const fs::path& path, bool* known = nullptr) {
  std::error_code error;
  const auto status = fs::symlink_status(path, error);
  if (error) {
    if (known) *known = error == std::errc::no_such_file_or_directory;
    return false;
  }
  if (known) *known = true;
  return status.type() != fs::file_type::not_found;
}

bool IsDirectoryNoSymlink(const fs::path& path) {
  std::error_code error;
  const auto status = fs::symlink_status(path, error);
  return !error && fs::is_directory(status) && !fs::is_symlink(status);
}

bool NativeSync(const fs::path& path, bool directory) {
  int flags = O_RDONLY;
#ifdef O_DIRECTORY
  if (directory) flags |= O_DIRECTORY;
#else
  (void)directory;
#endif
  const int descriptor = ::open(path.c_str(), flags);
  if (descriptor < 0) return false;
  const bool ok = ::fsync(descriptor) == 0;
  const bool closed = ::close(descriptor) == 0;
  return ok && closed;
}

bool PathComponentsHaveNoSymlink(const fs::path& root, const fs::path& relative,
                                 bool include_leaf) {
  fs::path current = root;
  std::size_t index = 0;
  const std::size_t count = static_cast<std::size_t>(std::distance(relative.begin(),
                                                                   relative.end()));
  for (const auto& part : relative) {
    current /= part;
    ++index;
    if (!include_leaf && index == count) break;
    std::error_code error;
    const auto status = fs::symlink_status(current, error);
    if (!error && fs::is_symlink(status)) return false;
    if (error && error != std::errc::no_such_file_or_directory) return false;
  }
  return true;
}

unsigned FileMode(const fs::path& path, bool* ok) {
  struct stat status {};
  if (::lstat(path.c_str(), &status) != 0 || !S_ISREG(status.st_mode)) {
    *ok = false;
    return 0;
  }
  *ok = true;
  return static_cast<unsigned>(status.st_mode & 07777);
}

std::optional<std::string> ReadBounded(const fs::path& path, std::size_t limit) {
  if (!IsRegularNoSymlink(path)) return std::nullopt;
  std::error_code error;
  const auto size = fs::file_size(path, error);
  if (error || size > limit) return std::nullopt;
  // POSIX reads: the console's libc lacks the locale support iostreams need.
  const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) return std::nullopt;
  std::string bytes(static_cast<std::size_t>(size), '\0');
  std::size_t done = 0;
  while (done < bytes.size()) {
    const auto count = ::read(fd, bytes.data() + done, bytes.size() - done);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) { ::close(fd); return std::nullopt; }
    done += static_cast<std::size_t>(count);
  }
  ::close(fd);
  return bytes;
}

bool WriteFile(const fs::path& path, const std::string& bytes, unsigned mode) {
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, mode);
  if (fd < 0) return false;
  struct stat info{};
  bool ok = ::fchmod(fd, mode) == 0 && ::fstat(fd, &info) == 0 &&
            S_ISREG(info.st_mode) && info.st_uid == ::geteuid() &&
            (info.st_mode & 07777) == mode;
  std::size_t offset = 0;
  while (ok && offset < bytes.size()) {
    const auto written = ::write(fd, bytes.data() + offset, bytes.size() - offset);
    if (written < 0 && errno == EINTR) continue;
    if (written <= 0) { ok = false; break; }
    offset += static_cast<std::size_t>(written);
  }
  if (::close(fd) != 0) ok = false;
  return ok;
}

// Only the just-created directory and its initial marker may be removed here.
// An interrupted initialization must not strand an unidentifiable staging tree.
void CleanInitialStage(const fs::path& staging, const struct stat& created) {
  using platform::DirectoryPolicy;
  using platform::ManagedDirectory;
  auto directory = ManagedDirectory::Open(staging, DirectoryPolicy::kApprovedTree);
  auto parent = ManagedDirectory::Open(staging.parent_path(), DirectoryPolicy::kApprovedTree);
  if (!directory || !parent) return;
  struct stat current{};
  if (::fstat(directory->fd(), &current) != 0 || current.st_dev != created.st_dev ||
      current.st_ino != created.st_ino || current.st_uid != ::geteuid()) return;
  const int listing_fd = ::dup(directory->fd());
  if (listing_fd < 0) return;
  DIR* listing = ::fdopendir(listing_fd);
  if (!listing) { ::close(listing_fd); return; }
  bool safe = true;
  errno = 0;
  while (const auto* entry = ::readdir(listing)) {
    const std::string name = entry->d_name;
    if (name != "." && name != ".." && name != kStageOwner) safe = false;
  }
  if (errno != 0) safe = false;
  ::closedir(listing);
  if (!safe || !directory->Validate()) return;
  struct stat marker{};
  if (directory->InspectChild(kStageOwner, &marker)) {
    if (!S_ISREG(marker.st_mode) || marker.st_uid != ::geteuid() ||
        (marker.st_mode & 07777) != 0600 || !directory->RemoveRegularFile(kStageOwner) ||
        !directory->Sync()) return;
  } else if (errno != ENOENT) return;
  if (!directory->Validate() || !parent->InspectChild(staging.filename().string(), &current) ||
      current.st_dev != created.st_dev || current.st_ino != created.st_ino) return;
  if (parent->RemoveEmptyDirectory(staging.filename().string())) parent->Sync();
}

std::map<std::string, std::string> ParseMarker(const std::string& bytes) {
  std::map<std::string, std::string> values;
  std::size_t start = 0;
  while (start < bytes.size()) {
    const auto end = bytes.find('\n', start);
    if (end == std::string::npos || end == start) return {};
    const auto equal = bytes.find('=', start);
    if (equal == std::string::npos || equal >= end || equal == start) return {};
    const auto key = bytes.substr(start, equal - start);
    const auto value = bytes.substr(equal + 1, end - equal - 1);
    if (!values.emplace(key, value).second) return {};
    start = end + 1;
  }
  return values;
}

// Deletes a tree with lstat/unlink/rmdir, never following links.
// std::filesystem::remove_all crashed the console title on a title folder
// (results/20261007-game-removal), so staging cleanup does not use it.
bool RemoveTreeNoFollow(const std::string& path) {
  struct stat info{};
  if (::lstat(path.c_str(), &info) != 0) return errno == ENOENT;
  if (!S_ISDIR(info.st_mode)) return ::unlink(path.c_str()) == 0;
  bool ok = true;
  if (DIR* directory = ::opendir(path.c_str())) {
    while (const dirent* entry = ::readdir(directory)) {
      const std::string name = entry->d_name;
      if (name == "." || name == "..") continue;
      ok = RemoveTreeNoFollow(path + "/" + name) && ok;
    }
    ::closedir(directory);
  } else {
    ok = false;
  }
  return ::rmdir(path.c_str()) == 0 && ok;
}

std::string OwnerMarker(const RuntimeInstall& record, const VerifiedKitView& kit,
                        const char* kind) {
  const auto* variant = KitVariantFor(kit, record.title_id);
  const std::string variant_id = variant ? variant->id : std::string();
  return std::string("schema=1\nkind=") + kind + "\ninstall_id=" + record.install_id +
         "\ntitle_id=" + record.title_id + "\nkit_id=" + kit.kit_id +
         "\nfiles_sha256=" + kit.files_sha256 +
         "\nruntime_bundle_id=" + kit.runtime.bundle_id +
         "\nruntime_revision=" + kit.runtime.artifact_revision +
         "\nruntime_path=" + kit.runtime.path + "\nvariant_id=" + variant_id +
         "\ntemplate_path=" + kit.template_path +
         "\nrecipe_id=" + record.recipe_id + "\nrecipe_version=" + record.recipe_version + "\n";
}

std::string StageMarker(const RuntimeInstall& record, const VerifiedKitView& kit,
                        const std::string& kind) {
  return OwnerMarker(record, kit, kind.c_str());
}

bool MarkerEquals(const fs::path& path, const std::string& expected) {
  const auto bytes = ReadBounded(path, 4096);
  return bytes && *bytes == expected;
}

// Whether a marker holds every wanted line.
bool MarkerHasLines(const fs::path& path, const std::vector<std::string>& wanted) {
  const auto bytes = ReadBounded(path, 4096);
  if (!bytes) return false;
  std::vector<bool> seen(wanted.size());
  std::size_t start = 0;
  while (start < bytes->size()) {
    auto end = bytes->find('\n', start);
    if (end == std::string::npos) end = bytes->size();
    const auto line = bytes->substr(start, end - start);
    for (std::size_t i = 0; i < wanted.size(); ++i) seen[i] = seen[i] || line == wanted[i];
    start = end + 1;
  }
  return std::find(seen.begin(), seen.end(), false) == seen.end();
}

// Whether a marker names this install: its schema, kind, install id and title id.
// The kit, runtime and recipe lines say which kit made the folder; they decide
// whether the title needs Update (TitleCurrent), not whose folder it is.
bool MarkerIdentifies(const fs::path& path, const RuntimeInstall& record, const char* kind) {
  return MarkerHasLines(path, {"schema=1", std::string("kind=") + kind, "install_id=" + record.install_id,
                               "title_id=" + record.title_id});
}

// Whether a staging folder's marker was written by an earlier install of this game.
// The folder is named after the install id; the title id, kit and recipe can change
// between installs (a reinstall can get another title), so they are not compared.
bool StageMarkerNamesInstall(const fs::path& path, const RuntimeInstall& record, const std::string& kind) {
  // Strict: a marker with a repeated or malformed line names no install.
  const auto bytes = ReadBounded(path, 4096);
  if (!bytes) return false;
  const auto values = ParseMarker(*bytes);
  const auto value = [&](const char* key) {
    const auto found = values.find(key);
    return found == values.end() ? std::string() : found->second;
  };
  return value("schema") == "1" && value("kind") == kind && value("install_id") == record.install_id;
}

const playden::steam::json::Value* Required(
    const playden::steam::json::Value& value, const char* name,
    playden::steam::json::Value::Type type) {
  const auto* member = value.Find(name);
  return member && member->type() == type ? member : nullptr;
}

bool DescriptorMatches(const RuntimeInstall& record, std::string_view bytes) {
  if (bytes.empty() || bytes.size() > kMaxPublicationBytes) return false;
  auto parsed = playden::steam::json::Parse(
      bytes, {kMaxPublicationBytes, 4, 32, kMaxPublicationBytes});
  if (!parsed || !parsed->is_object()) return false;
  using Type = playden::steam::json::Value::Type;
  const auto* schema = Required(*parsed, "schema_version", Type::kNumber);
  // Schema 1 infers the historical per-title root; schema 2 records the actual
  // download root, which may be an existing launcher path or a selected volume.
  if (!schema) return false;
  const auto version = schema->AsUint64();
  if (version == 1) {
    if (parsed->object().size() != 11 ||
        record.install_path != "/data/homebrew/" + record.title_id + "/games/" + std::to_string(record.runtime_number))
      return false;
  } else if (version == 2) {
    const auto* game_root = Required(*parsed, "game_root", Type::kString);
    if (parsed->object().size() != 12 || !game_root || game_root->text() != record.install_path) return false;
  } else {
    return false;
  }
  const auto* install = Required(*parsed, "install_id", Type::kString);
  const auto* title = Required(*parsed, "title_id", Type::kString);
  const auto* app = Required(*parsed, "app_id", Type::kNumber);
  const auto* root = Required(*parsed, "data_root", Type::kString);
  const auto* profile = Required(*parsed, "default_profile_id", Type::kString);
  const auto* revision = Required(*parsed, "settings_revision", Type::kNumber);
  const auto* runtime = Required(*parsed, "runtime_bundle_id", Type::kString);
  const auto* recipe = Required(*parsed, "recipe_id", Type::kString);
  const auto* recipe_version = Required(*parsed, "recipe_version", Type::kString);
  const auto* exit_policy = Required(*parsed, "exit_policy", Type::kString);
  return install && install->text() == record.install_id &&
         title && title->text() == record.title_id && app && app->AsUint64() == record.runtime_number &&
         root && root->text() == record.data_root && profile &&
         profile->text() == record.default_profile_id && revision &&
         revision->AsUint64() == record.settings_revision && runtime &&
         runtime->text() == record.runtime_bundle_id && recipe &&
         recipe->text() == record.recipe_id && recipe_version &&
         recipe_version->text() == record.recipe_version && exit_policy &&
         exit_policy->text() == record.exit_policy;
}

bool PublicationIdentityMatches(const RuntimeInstall& record,
                                const ProfilePublication& publication) {
  if (!(publication.install_id == record.install_id &&
         publication.title_id == record.title_id &&
         publication.data_root == record.data_root &&
         publication.profile_id == record.default_profile_id &&
         publication.runtime_bundle_id == record.runtime_bundle_id &&
         publication.recipe_id == record.recipe_id &&
         publication.recipe_version == record.recipe_version &&
         publication.settings_revision == record.settings_revision &&
         !publication.profile_bytes.empty() &&
         publication.profile_bytes.size() <= kMaxPublicationBytes &&
         publication.profile_references_validated &&
         DescriptorMatches(record, publication.descriptor_bytes)))
    return false;
  std::set<std::string> names;
  for (const auto& preset : publication.input_presets) {
    if (!IsSafeRelative(preset.filename) ||
        fs::path(preset.filename).parent_path() != fs::path{} ||
        fs::path(preset.filename).extension() != ".input" || preset.bytes.empty() ||
        preset.bytes.size() > kMaxPublicationBytes || !names.insert(preset.filename).second)
      return false;
  }
  std::set<std::string> required;
  for (const auto& name : publication.required_input_presets)
    if (!IsSafeRelative(name) || fs::path(name).parent_path() != fs::path{} ||
        fs::path(name).extension() != ".input" || !required.insert(name).second)
      return false;
  return names == required;
}

}  // namespace

const KitTitleVariant* KitVariantFor(const VerifiedKitView& kit, const std::string& title_id) {
  const KitTitleVariant* any = nullptr;
  for (const auto& variant : kit.variants) {
    if (variant.any_title) any = &variant;
    else if (variant.title_id == title_id) return &variant;
  }
  // The any-title shell serves every managed title except its own template ID.
  return any && IsManagedTitle(title_id) && title_id != any->title_id ? any : nullptr;
}

std::vector<std::string> KitTitleCandidates(const VerifiedKitView& kit) {
  std::vector<std::string> titles;
  bool any_title = false;
  for (const auto& variant : kit.variants) {
    if (!variant.any_title) titles.push_back(variant.title_id);
    any_title |= variant.any_title;
  }
  if (any_title)
    for (int number = 1; number <= 999; ++number) {
      char title[16];
      std::snprintf(title, sizeof(title), "%s%03d", kTitleRulePrefix, number);
      if (std::find(titles.begin(), titles.end(), title) == titles.end() && KitVariantFor(kit, title))
        titles.push_back(title);
    }
  return titles;
}

std::string UnmetRecipeRequirements(const std::map<std::string, std::string>& requirements, const VerifiedKitView& kit) {
  std::string unmet;
  for (const auto& [name, value] : requirements) {
    if (name == "audio" && value == "ps5" && kit.template_audio_ps5) continue;
    unmet += (unmet.empty() ? "" : ", ") + (name == "audio" && value == "ps5" ? std::string("PS5 audio") : name + " " + value);
  }
  return unmet;
}

FilesystemPackageInstaller::FilesystemPackageInstaller(
    VerifiedKitView kit, PackageInstallPaths paths,
    PackageInstallerOperations operations)
    : kit_(std::move(kit)), paths_(std::move(paths)),
      operations_(std::move(operations)) {}

const KitTitleVariant* FilesystemPackageInstaller::Variant(
    const RuntimeInstall& record) const {
  return KitVariantFor(kit_, record.title_id);
}

bool FilesystemPackageInstaller::ValidView() const {
  if (paths_.console_data_root != "/data" || paths_.host_data_root.empty() ||
      kit_.root.empty() || kit_.kit_id.empty() || !IsHexDigest(kit_.files_sha256) ||
      kit_.runtime.bundle_id.empty() || kit_.runtime.artifact_revision.empty() ||
      !IsSafeRelative(kit_.runtime.path) || !IsSafeRelative(kit_.template_path) ||
      !IsSafeRelative(kit_.template_drive_table) || !IsSafeRelative(kit_.recipe.path) ||
      kit_.recipe.id.empty() || kit_.recipe.version.empty() ||
      !IsHexDigest(kit_.recipe.sha256) || kit_.files.empty() || kit_.variants.empty() ||
      !operations_.hash_file || !operations_.hash_bytes ||
      (!operations_.free_bytes && !operations_.free_space_unverifiable) ||
      !operations_.validate_bottle || !paths_.host_data_root.is_absolute() ||
      paths_.host_data_root != paths_.host_data_root.lexically_normal() || paths_.host_data_root.filename() != "data")
    return false;
  std::error_code root_error;
  const auto root_status = fs::symlink_status(kit_.root, root_error);
  if (root_error || !fs::is_directory(root_status) || fs::is_symlink(root_status)) return false;
  std::set<std::string> files;
  for (const auto& file : kit_.files) {
    if (!IsSafeRelative(file.path) || !IsHexDigest(file.sha256) || file.mode > 07777 ||
        !files.insert(file.path).second)
      return false;
  }
  std::set<std::string> variant_ids;
  std::set<std::string> title_ids;
  for (const auto& variant : kit_.variants)
    if (variant.id.empty() || !IsManagedTitle(variant.title_id) ||
        !IsSafeRelative(variant.path) || !variant_ids.insert(variant.id).second ||
        !title_ids.insert(variant.title_id).second)
      return false;
  for (const auto& artifact : kit_.artifacts)
    if (artifact.artifact_id.empty() || artifact.identity.empty() ||
        !IsHexDigest(artifact.sha256) || !IsSafeRelative(artifact.path))
      return false;
  return true;
}

void FilesystemPackageInstaller::AcceptShippedRecipes(std::vector<std::string> recipe_ids) {
  shipped_recipe_ids_ = std::move(recipe_ids);
}

// The kit's own recipe at the kit's version, a recipe Playden ships (AcceptShippedRecipes)
// or the one Playden generated for this game from Steam's launch options
// (playden::GeneratedRecipeId: "steam-<app id>-auto", "local-<uuid>-auto"). The kit's runtime and template
// run any of them; only the launch entries and bottle values differ.
bool FilesystemPackageInstaller::RecipeAccepted(const RuntimeInstall& record) const {
  if (record.recipe_id == kit_.recipe.id) return record.recipe_version == kit_.recipe.version;
  if (record.recipe_version.empty()) return false;
  if (std::find(shipped_recipe_ids_.begin(), shipped_recipe_ids_.end(), record.recipe_id) != shipped_recipe_ids_.end())
    return true;
  if (record.runtime_number == 0) return false;
  if (record.game != GameId{}) return record.game.valid() && record.recipe_id == playden::GeneratedRecipeId(record.game);
  // A copy that doesn't carry its game (read back from a journal): the Steam game its number names,
  // or, for a number from the allocator's range, a Local game's generated recipe.
  if (record.recipe_id == playden::GeneratedRecipeId(GameId::Steam(record.runtime_number))) return true;
  constexpr std::string_view kSuffix = "-auto";
  const std::string_view id = record.recipe_id;
  if (record.runtime_number < kFirstRuntimeNumber || record.runtime_number > kLastRuntimeNumber ||
      id.size() <= kSuffix.size() || id.substr(id.size() - kSuffix.size()) != kSuffix)
    return false;
  const auto game = playden::ParseInstallId(id.substr(0, id.size() - kSuffix.size()));
  return game && game->store == Store::kLocal;
}

std::optional<FilesystemPackageInstaller::ResolvedPaths>
FilesystemPackageInstaller::Resolve(const RuntimeInstall& record) const {
  if (!ValidView() || !Variant(record) ||
      record.data_root != "/data/playden/games/" + record.install_id ||
      record.install_id.empty() || record.install_id.find('/') != std::string::npos ||
      !IsManagedTitle(record.title_id) || record.default_profile_id.empty() ||
      record.default_profile_id.find('/') != std::string::npos ||
      record.runtime_bundle_id != kit_.runtime.bundle_id ||
      !RecipeAccepted(record))
    return std::nullopt;
  const fs::path game = paths_.host_data_root / "playden/games" / record.install_id;
  const fs::path title = paths_.host_data_root / "homebrew" / record.title_id;
  const fs::path library = game / "library";
  ResolvedPaths resolved;
  resolved.title = title;
  resolved.game = game;
  resolved.library = library;
  resolved.bottle = library / "prefixes/game";
  resolved.title_staging = paths_.host_data_root / "playden/staging" /
                           ("title-" + record.install_id);
  resolved.bottle_staging = paths_.host_data_root / "playden/staging" /
                            ("bottle-" + record.install_id);
  resolved.publication.profile = library / "profiles" /
                                 (record.default_profile_id + ".profile");
  resolved.publication.catalog = library / "profiles/profiles.lst";
  resolved.publication.input_directory = library / "input";
  resolved.publication.descriptor = title / "game-launch.json";
  resolved.publication.metadata = library / "profiles/.playden-publication";
  std::error_code root_error;
  const auto root_status = fs::symlink_status(paths_.host_data_root, root_error);
  if (root_error && root_error != std::errc::no_such_file_or_directory) return std::nullopt;
  if (!root_error && root_status.type() != fs::file_type::not_found &&
      (!fs::is_directory(root_status) || fs::is_symlink(root_status)))
    return std::nullopt;
  const auto safe_target = [&](const fs::path& target) {
    const auto relative = target.lexically_relative(paths_.host_data_root);
    return !relative.empty() && PathComponentsHaveNoSymlink(
                                    paths_.host_data_root, relative, true);
  };
  if (!safe_target(resolved.title) || !safe_target(resolved.publication.descriptor) ||
      !safe_target(resolved.game) || !safe_target(resolved.library) ||
      !safe_target(resolved.bottle) ||
      !safe_target(resolved.publication.profile.parent_path()) ||
      !safe_target(resolved.publication.profile) ||
      !safe_target(resolved.publication.catalog) ||
      !safe_target(resolved.publication.input_directory) ||
      !safe_target(resolved.publication.metadata) ||
      !safe_target(resolved.title_staging) || !safe_target(resolved.bottle_staging))
    return std::nullopt;
  return resolved;
}

std::vector<FilesystemPackageInstaller::Mapping>
FilesystemPackageInstaller::TitleMappings(const RuntimeInstall& record) const {
  std::vector<Mapping> mappings;
  const auto* variant = Variant(record);
  if (!variant) return mappings;
  std::set<std::string> destinations;
  for (const auto& file : kit_.files) {
    std::string relative;
    std::string destination;
    if (HasPrefix(file.path, variant->path, &relative)) {
      destination = relative;
    } else if (HasPrefix(file.path, kit_.runtime.path, &relative)) {
      destination = "win/wine/" + relative;
    } else {
      continue;
    }
    if (!IsSafeRelative(relative) || !IsSafeRelative(destination) ||
        !destinations.insert(destination).second)
      return {};
    Mapping mapping{&file, file.path, destination, kit_.root / file.path, nullptr};
    for (const auto* metadata : kTitleMetadataFiles) {
      if (destination != metadata) continue;
      const auto own = TitleMetadataPath(record, destination);
      std::error_code error;
      if (own.empty() || !IsRegularNoSymlink(own)) continue;
      const auto size = fs::file_size(own, error);
      const auto digest = operations_.hash_file(own);
      if (error || !digest) return {};
      auto described = std::make_shared<KitFile>(KitFile{own.string(), size, *digest, file.mode});
      mapping.file = described.get();
      mapping.owned = std::move(described);
      mapping.source = own;
    }
    // The any-title shell's own param.json names its template ID: a game title only ever
    // gets the parameters Playden wrote for it.
    if (variant->any_title && destination == kTitleMetadataFiles[0] && !mapping.owned) return {};
    mappings.push_back(std::move(mapping));
  }
  return mappings;
}

std::filesystem::path FilesystemPackageInstaller::TitleMetadataPath(const RuntimeInstall& record,
                                                                    const std::string& relative) const {
  const auto paths = Resolve(record);
  if (!paths) return {};
  return relative.empty() ? paths->game / "title-metadata" : paths->game / "title-metadata" / relative;
}

bool FilesystemPackageInstaller::TitleMetadataCurrent(const RuntimeInstall& record) {
  const auto paths = Resolve(record);
  if (!paths || !IsRegularNoSymlink(TitleMetadataPath(record, kTitleMetadataMarker))) return false;
  for (const auto& mapping : TitleMappings(record)) {
    if (!mapping.owned) continue;
    const auto installed = paths->title / mapping.destination_relative;
    const auto digest = IsRegularNoSymlink(installed) ? operations_.hash_file(installed) : std::nullopt;
    if (!digest || *digest != mapping.file->sha256) return false;
  }
  return true;
}

PackageActionResult FilesystemPackageInstaller::WriteTitleMetadata(const RuntimeInstall& record,
                                                                   const std::string& param_json,
                                                                   const std::optional<std::string>& icon_png) {
  const auto paths = Resolve(record);
  if (!paths) return Failed("kit-invalid", "record or verified kit view is invalid");
  // The parameters name this title, whichever kit title they were made from.
  const auto parameters = playden::steam::json::Parse(param_json, {64 * 1024, 8, 1024, 64 * 1024});
  const auto* title = parameters && parameters->is_object()
                          ? Required(*parameters, "titleId", playden::steam::json::Value::Type::kString)
                          : nullptr;
  if (!title || title->text() != record.title_id)
    return Failed("title-metadata", "the title parameters do not name this title");
  const auto root = TitleMetadataPath(record, {});
  std::string detail;
  if (!EnsurePrivateDirectories(paths_.host_data_root, paths->game, root / "sce_sys", &detail,
                                BottleTraversal(*paths, ProfileShared(record, *paths))))
    return Failed("private-directory", detail);
  // Each file is written beside its final name, synced, then moved over it.
  const auto replace = [&](const fs::path& path, const std::string& bytes) {
    const auto temporary = path.parent_path() / ("." + path.filename().string() + ".new");
    ::unlink(temporary.c_str());
    if (!WriteFile(temporary, bytes, 0600) || !SyncPath(temporary, false) ||
        ::rename(temporary.c_str(), path.c_str()) != 0 || !SyncPath(path.parent_path(), true)) {
      ::unlink(temporary.c_str());
      return false;
    }
    return true;
  };
  if (!replace(root / kTitleMetadataFiles[0], param_json))
    return Failed("title-metadata", "the game's title name could not be written");
  const auto icon = root / kTitleMetadataFiles[1];
  if (icon_png ? !replace(icon, *icon_png) : (::unlink(icon.c_str()) != 0 && errno != ENOENT))
    return Failed("title-metadata", "the game's title icon could not be written");
  if (!replace(root / kTitleMetadataMarker, "1\n"))
    return Failed("title-metadata", "the game's title metadata could not be recorded");
  return Complete();
}

std::vector<FilesystemPackageInstaller::Mapping>
FilesystemPackageInstaller::TemplateMappings() const {
  std::vector<Mapping> mappings;
  for (const auto& file : kit_.files) {
    std::string relative;
    if (HasPrefix(file.path, kit_.template_path, &relative) && IsSafeRelative(relative))
      mappings.push_back({&file, file.path, relative, kit_.root / file.path, nullptr});
  }
  return mappings;
}

namespace {
bool TitleDirectoriesPublished(const fs::path& title);

// Creates the missing folders of path with an explicit mode, whatever the umask
// (console titles run with umask 0). Existing folders are left as they are.
void CreateDirectoriesWithMode(const fs::path& path, mode_t mode, std::error_code& error) {
  error.clear();
  std::vector<fs::path> missing;
  for (auto current = path; !current.empty() && current != current.root_path(); current = current.parent_path()) {
    struct stat info{};
    if (::lstat(current.c_str(), &info) == 0) {
      if (!S_ISDIR(info.st_mode)) { error = std::make_error_code(std::errc::not_a_directory); return; }
      break;
    }
    missing.push_back(current);
  }
  for (auto it = missing.rbegin(); it != missing.rend(); ++it) {
    if (::mkdir(it->c_str(), mode) != 0 && errno != EEXIST) { error = std::error_code(errno, std::generic_category()); return; }
    struct stat info{};
    if (::lstat(it->c_str(), &info) != 0 || !S_ISDIR(info.st_mode) || info.st_uid != ::geteuid() ||
        ((info.st_mode & 07777) != mode && ::chmod(it->c_str(), mode) != 0)) {
      error = std::make_error_code(std::errc::permission_denied);
      return;
    }
  }
}
}  // namespace

bool FilesystemPackageInstaller::TitleMatches(
    const RuntimeInstall& record, const ResolvedPaths& paths) const {
  return IsDirectoryNoSymlink(paths.title) &&
         MarkerEquals(paths.title / kTitleOwner, OwnerMarker(record, kit_, "title")) &&
         TitleFilesMatch(record, paths) && TitleDirectoriesPublished(paths.title);
}

bool FilesystemPackageInstaller::TitleOwned(
    const RuntimeInstall& record, const ResolvedPaths& paths) const {
  return IsDirectoryNoSymlink(paths.title) && MarkerIdentifies(paths.title / kTitleOwner, record, "title");
}

bool FilesystemPackageInstaller::TitleFilesMatch(
    const RuntimeInstall& record, const ResolvedPaths& paths) const {
  if (!IsDirectoryNoSymlink(paths.title)) return false;
  const auto mappings = TitleMappings(record);
  if (mappings.empty()) return false;
  const auto started = PartTimes::Clock::now();
  PartTimes times;
  for (const auto& mapping : mappings) {
    const auto path = paths.title / mapping.destination_relative;
    bool mode_ok = false;
    std::error_code size_error;
    auto since = PartTimes::Clock::now();
    const auto size = fs::file_size(path, size_error);
    if (!PathComponentsHaveNoSymlink(paths.title, mapping.destination_relative, true) ||
        !IsRegularNoSymlink(path) || size_error || size != mapping.file->size ||
        FileMode(path, &mode_ok) != mapping.file->mode || !mode_ok)
      return false;
    times.Add("checks", since);
  }
  if (operations_.log)
    operations_.log("title check files=" + std::to_string(mappings.size()) + " " + times.Summary(started));
  return true;
}

bool FilesystemPackageInstaller::ProfileShared(const RuntimeInstall& record, const ResolvedPaths& paths) const {
  // A title keeps the shell it was made with after a kit swap; an older shell admits only 0700.
  return profile_shared_ && MarkerEquals(paths.title / kTitleOwner, OwnerMarker(record, kit_, "title"));
}

PrivateTraversal FilesystemPackageInstaller::BottleTraversal(const ResolvedPaths& paths, bool shared) const {
  return {{paths.game, paths.library, paths.library / "prefixes", paths.bottle}, static_cast<mode_t>(shared ? 0711 : 0700)};
}

bool FilesystemPackageInstaller::BottleOwned(
    const RuntimeInstall& record, const ResolvedPaths& paths) const {
  return IsPrivateDirectoryTree(paths.game, paths.bottle, BottleTraversal(paths)) &&
         MarkerIdentifies(paths.bottle / kTemplateOwner, record, "template") &&
         operations_.validate_bottle(paths.bottle);
}

bool FilesystemPackageInstaller::ArtifactsMatch(
    const RuntimeInstall& record, const ResolvedPaths& paths) const {
  if (record.artifacts.size() != kit_.artifacts.size()) return false;
  std::set<std::string> matched;
  for (const auto& artifact : record.artifacts) {
    const auto found = std::find_if(
        kit_.artifacts.begin(), kit_.artifacts.end(), [&](const KitArtifact& expected) {
          return expected.artifact_id == artifact.artifact_id &&
                 expected.identity == artifact.identity && expected.sha256 == artifact.sha256;
        });
    if (found == kit_.artifacts.end() || !matched.insert(found->artifact_id).second)
      return false;
    const fs::path path = found->location == ImmutableArtifactLocation::kKit
                              ? kit_.root / found->path
                              : paths.title / found->path;
    const fs::path root = found->location == ImmutableArtifactLocation::kKit
                              ? kit_.root
                              : paths.title;
    if (!PathComponentsHaveNoSymlink(root, found->path, true) ||
        !IsRegularNoSymlink(path))
      return false;
    const auto hash = operations_.hash_file(path);
    if (!hash || *hash != found->sha256) return false;
  }
  return matched.size() == kit_.artifacts.size();
}

std::string FilesystemPackageInstaller::AdmissionDiagnostic(const RuntimeInstall& record) {
  const auto prefix = paths_.host_data_root == fs::path("/data") ? std::string{} : paths_.host_data_root.parent_path().string();
  PwTitlePublicationPlatform platform{};
  platform.host_root = prefix.empty() ? nullptr : prefix.c_str(); platform.owner_uid = ::geteuid();
  platform.try_lock_shared = [](void*, int descriptor) {
#if defined(__PROSPERO__) || (defined(PLAYDEN_PLATFORM_PS5) && PLAYDEN_PLATFORM_PS5)
    return playden_ps5_file_lock(descriptor, LOCK_SH | LOCK_NB);
#else
    return ::flock(descriptor, LOCK_SH | LOCK_NB);
#endif
  };
  const auto canonical = "/data/homebrew/" + record.title_id + "/game-launch.json";
  PwTitlePublicationDescriptor descriptor{}; char error[256]{};
  if (pw_title_publication_read_descriptor(canonical.c_str(), record.title_id.c_str(), &platform,
      &descriptor, error, sizeof(error)) != PW_OK) return std::string("descriptor: ") + error;
  PwTitlePublicationLease lease{}; pw_title_publication_lease_init(&lease);
  const int status = pw_title_publication_open(&descriptor, &platform, &lease, error, sizeof(error));
  if (status != PW_OK) return "admission status=" + std::to_string(status) + ": " + error;
  pw_title_publication_close(&lease);
  return "ok";
}

std::string FilesystemPackageInstaller::QuickAdmission(const RuntimeInstall& record) {
  // The title reads user.reg at its start check (stage 7) as an app-owned, single-link
  // regular file in mode 0600 or 0644. Wine saving it with umask 0 leaves 0666, which
  // RepairBottle fixes; the rest of the check runs at Play and Update.
  const auto paths = Resolve(record);
  if (!paths) return "kit or record is invalid";
  struct stat info{};
  const auto path = paths->bottle / "user.reg";
  if (::lstat(path.c_str(), &info) != 0) return "stage 7: user.reg is missing";
  const auto mode = info.st_mode & 07777;
  if (!S_ISREG(info.st_mode) || info.st_uid != ::geteuid() || info.st_nlink != 1 || (mode != 0600 && mode != 0644)) {
    char octal[8]; std::snprintf(octal, sizeof(octal), "%04o", static_cast<unsigned>(mode));
    return std::string("stage 7: user.reg mode ") + octal + " is not admitted";
  }
  return "ok";
}

PackageActionResult FilesystemPackageInstaller::RepairBottle(const RuntimeInstall& record) {
  const auto paths = Resolve(record);
  if (!paths) return Failed("kit-invalid", "record or verified kit view is invalid");
  if (!IsPrivateDirectoryTree(paths->game, paths->bottle, BottleTraversal(*paths)))
    return Failed("repair-ownership", "the bottle is not this install's private directory");
  // Wine saves registry hives with the process umask; earlier per-game titles
  // ran with umask 0 and left them 0666. Only app-owned regular files change.
  for (const char* name : {"user.reg", "system.reg", "userdef.reg"}) {
    const auto path = paths->bottle / name;
    struct stat info{};
    if (::lstat(path.c_str(), &info) != 0) continue;
    if (!S_ISREG(info.st_mode) || info.st_uid != ::geteuid() || info.st_nlink != 1)
      return Failed("repair-ownership", std::string(name) + " is not an app-owned regular file");
    const auto mode = info.st_mode & 07777;
    if (mode != 0600 && (::chmod(path.c_str(), 0600) != 0 || !SyncPath(path, false)))
      return Failed("repair-mode", std::string(name) + " permissions could not be restored");
  }
  return SettleBottleModes(record);
}

PackageActionResult FilesystemPackageInstaller::SettleBottleModes(const RuntimeInstall& record) {
  const auto paths = Resolve(record);
  if (!paths) return Failed("kit-invalid", "record or verified kit view is invalid");
  const bool shared = ProfileShared(record, *paths);
  const auto traversal = BottleTraversal(*paths, shared);
  if (shared && !SetTraversableMode(paths->game.parent_path(), 0711))
    return Failed("repair-mode", "the games profile parent could not be opened");
  if (!IsPrivateDirectoryTree(paths->game, paths->bottle, traversal))
    return Failed("repair-ownership", "the bottle is not this install's private directory");
  // A kit without bottle-profile-shared admits only 0700 there (after a kit downgrade too), and
  // so does a title made by an older kit until Update refreshes it. The profile itself is left
  // as it is: no kit checks below the bottle.
  auto folders = traversal.folders;
  folders.push_back(paths->bottle / "drive_c");
  folders.push_back(paths->bottle / "drive_c/users");
  for (const auto& folder : folders)
    if (!SetTraversableMode(folder, traversal.created_mode))
      return Failed("repair-mode", "the permissions of " + folder.string() + " could not be set: " + strerror(errno));
  // The profile only matters to FTP: what can't be opened is logged, never a failure of the game.
  SharedTreeResult opened;
  if (shared) opened = OpenTreeToEveryone(paths->bottle / "drive_c/users/prospero", {operations_.private_profile_paths, {}});
  if (operations_.log)
    operations_.log("bottle modes install=" + record.install_id + " way=" + (shared ? "0711" : "0700") +
                    (shared ? " profile_opened=" + std::to_string(opened.changed) + " skipped=" +
                                  std::to_string(opened.skipped) + " linked=" + std::to_string(opened.linked) +
                                  " walked=" + std::to_string(opened.ok) + " truncated=" + std::to_string(opened.truncated)
                            : std::string(profile_shared_ ? " title-not-current" : "") + " profile=unchanged"));
  return Complete();
}

bool FilesystemPackageInstaller::PublicationMatches(
    const RuntimeInstall& record, const ResolvedPaths& paths,
    std::uint64_t* revision) const {
  const auto prefix = paths_.host_data_root == fs::path("/data") ? std::string{} : paths_.host_data_root.parent_path().string();
  PwTitlePublicationPlatform platform{};
  platform.host_root = prefix.empty() ? nullptr : prefix.c_str(); platform.owner_uid = ::geteuid();
  platform.try_lock_shared = [](void*, int descriptor) {
#if defined(__PROSPERO__) || (defined(PLAYDEN_PLATFORM_PS5) && PLAYDEN_PLATFORM_PS5)
    return playden_ps5_file_lock(descriptor, LOCK_SH | LOCK_NB);
#else
    return ::flock(descriptor, LOCK_SH | LOCK_NB);
#endif
  };
  const auto canonical = "/data/homebrew/" + record.title_id + "/game-launch.json";
  PwTitlePublicationDescriptor descriptor{}; char error[256]{};
  if (pw_title_publication_read_descriptor(canonical.c_str(), record.title_id.c_str(), &platform,
      &descriptor, error, sizeof(error)) != PW_OK) return false;
  const auto& launch = descriptor.launch;
  if (launch.install_id != record.install_id || launch.title_id != record.title_id || launch.app_id != record.runtime_number ||
      launch.data_root != record.data_root || launch.default_profile_id != record.default_profile_id ||
      launch.runtime_bundle_id != record.runtime_bundle_id || launch.recipe_id != record.recipe_id ||
      launch.recipe_version != record.recipe_version || launch.settings_revision != record.settings_revision ||
      launch.exit_policy != record.exit_policy) return false;
  // Same canonical file semantics and qualified shared lease as Wine admission.
  // Inspector retains the lease through the independent current-kit binding.
  PwTitlePublicationLease lease{}; pw_title_publication_lease_init(&lease);
  if (pw_title_publication_open(&descriptor, &platform, &lease, error, sizeof(error)) != PW_OK) return false;
  struct LeaseScope {
    PwTitlePublicationLease* lease;
    ~LeaseScope() { pw_title_publication_close(lease); }
  } held{&lease};
  auto control = platform::ManagedDirectory::Open(paths.game / ".playden-control", platform::DirectoryPolicy::kPrivate);
  if (!control) return false;
  const int fd = control->OpenRegularFile("publication-state.v1", true, 0600);
  if (fd < 0) return false;
  struct stat before{}, after{}, named{};
  bool valid = ::fstat(fd, &before) == 0 && before.st_nlink == 1 && before.st_size > 0 &&
      before.st_size <= PW_TITLE_PUBLICATION_MAX_BYTES;
  std::string bytes(valid ? static_cast<std::size_t>(before.st_size) : 0, '\0');
  std::size_t offset = 0;
  while (valid && offset < bytes.size()) {
    const auto count = ::read(fd, bytes.data() + offset, bytes.size() - offset);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) { valid = false; break; }
    offset += static_cast<std::size_t>(count);
  }
  valid = valid && ::fstat(fd, &after) == 0 && control->InspectChild("publication-state.v1", &named) &&
      before.st_dev == after.st_dev && before.st_ino == after.st_ino && before.st_size == after.st_size &&
      before.st_mode == after.st_mode && before.st_uid == after.st_uid && after.st_nlink == 1 &&
      after.st_dev == named.st_dev && after.st_ino == named.st_ino && after.st_size == named.st_size &&
      after.st_mode == named.st_mode && after.st_uid == named.st_uid && named.st_nlink == 1;
  const bool closed = ::close(fd) == 0;
  PwTitlePublication receipt{};
  if (!valid || !closed || pw_title_publication_parse(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), &receipt) != PW_OK ||
      receipt.phase != PW_TITLE_PUBLICATION_COMMITTED ||
      (receipt.kit_id != kit_.kit_id && !KitIdRenamedTo(receipt.kit_id, kit_.kit_id)) ||
      receipt.install_id != record.install_id || receipt.title_id != record.title_id ||
      receipt.runtime_bundle_id != record.runtime_bundle_id || receipt.recipe_id != record.recipe_id ||
      receipt.recipe_version != record.recipe_version || receipt.settings_revision != record.settings_revision)
    return false;
  if (revision) *revision = receipt.settings_revision;
  return true;
}

bool FilesystemPackageInstaller::SyncPath(const fs::path& path, bool directory) const {
  return operations_.sync_path ? operations_.sync_path(path, directory)
                               : NativeSync(path, directory);
}

bool FilesystemPackageInstaller::SyncTree(const fs::path& root, bool sync_files) const {
  if (!IsDirectoryNoSymlink(root)) return false;
  std::vector<fs::path> files;
  std::vector<fs::path> directories{root};
  std::error_code error;
  fs::recursive_directory_iterator iterator(root, fs::directory_options::none, error);
  const fs::recursive_directory_iterator end;
  while (!error && iterator != end) {
    const auto path = iterator->path();
    const auto status = iterator->symlink_status(error);
    if (error || fs::is_symlink(status)) return false;
    if (fs::is_directory(status)) {
      directories.push_back(path);
    } else if (fs::is_regular_file(status)) {
      files.push_back(path);
    } else {
      return false;
    }
    iterator.increment(error);
  }
  if (error) return false;
  if (sync_files)
    for (const auto& file : files)
      if (!SyncPath(file, false)) return false;
  std::sort(directories.begin(), directories.end(), [](const fs::path& left,
                                                        const fs::path& right) {
    return std::distance(left.begin(), left.end()) >
           std::distance(right.begin(), right.end());
  });
  for (const auto& directory : directories)
    if (!SyncPath(directory, true)) return false;
  return true;
}

bool FilesystemPackageInstaller::SyncAncestors(const fs::path& directory) const {
  auto current = directory;
  while (true) {
    const auto relative = current.lexically_relative(paths_.host_data_root);
    if ((relative.empty() && current != paths_.host_data_root) ||
        (!relative.empty() && *relative.begin() == ".."))
      return false;
    if (!IsDirectoryNoSymlink(current) || !SyncPath(current, true)) return false;
    if (current == paths_.host_data_root) return true;
    const auto parent = current.parent_path();
    if (parent == current) return false;
    current = parent;
  }
}

bool FilesystemPackageInstaller::PublicationPathsSafe(
    const ProfilePublication& publication, const PublicationPaths& paths) const {
  const std::array<fs::path, 5> fixed = {paths.profile, paths.catalog,
                                         paths.input_directory, paths.descriptor,
                                         paths.metadata};
  for (const auto& path : fixed) {
    const auto relative = path.lexically_relative(paths_.host_data_root);
    if (relative.empty() || *relative.begin() == ".." ||
        !PathComponentsHaveNoSymlink(paths_.host_data_root, relative, true))
      return false;
  }
  for (const auto& preset : publication.input_presets) {
    const auto path = paths.input_directory / preset.filename;
    const auto relative = path.lexically_relative(paths_.host_data_root);
    if (relative.empty() || *relative.begin() == ".." ||
        !PathComponentsHaveNoSymlink(paths_.host_data_root, relative, true))
      return false;
  }
  return true;
}

PackageStatus FilesystemPackageInstaller::Inspect(const RuntimeInstall& record) {
  return InspectWith(record, true);
}

PackageStatus FilesystemPackageInstaller::InspectOwnership(const RuntimeInstall& record) {
  return InspectWith(record, false);
}

PackageStatus FilesystemPackageInstaller::InspectWith(const RuntimeInstall& record, bool walk_title) {
  PackageStatus status;
  try {
    const auto paths = Resolve(record);
    if (!paths) return status;
    status.title_source_path = paths->title.string();
    bool title_known = false;
    bool bottle_known = false;
    status.title_staged = ExistsNoThrow(paths->title, &title_known);
    status.mutable_bottle_present = ExistsNoThrow(paths->bottle, &bottle_known);
    if (!title_known || !bottle_known) return status;
    status.known = true;
    const auto started = PartTimes::Clock::now();
    PartTimes times;
    auto since = PartTimes::Clock::now();
    status.manifest_matches = status.title_staged &&
                              (walk_title ? TitleMatches(record, *paths) : TitleOwned(record, *paths));
    times.Add("title", since); since = PartTimes::Clock::now();
    status.template_owned = status.mutable_bottle_present && BottleOwned(record, *paths);
    status.mutable_bottle_owned = status.template_owned;
    times.Add("bottle", since); since = PartTimes::Clock::now();
    // Ownership needs no kit artifacts: after a kit change they differ until Update,
    // which TitleCurrent and the full inspection (launch) report.
    status.artifacts_valid = walk_title && status.manifest_matches && ArtifactsMatch(record, *paths);
    times.Add("artifacts", since); since = PartTimes::Clock::now();
    // Whether the settings are published is a launch question; applies prove it from
    // their own committed receipt.
    status.profile_published =
        walk_title && status.manifest_matches && PublicationMatches(record, *paths,
                                                                    &status.profile_settings_revision);
    times.Add("publication", since);
    if (operations_.log)
      operations_.log(std::string(walk_title ? "package inspect " : "package inspect ownership ") +
                      times.Summary(started));
  } catch (const fs::filesystem_error&) {
    return PackageStatus{};
  }
  return status;
}

PackageActionResult FilesystemPackageInstaller::CopyMappings(
    const RuntimeInstall& record, const std::vector<Mapping>& mappings,
    const fs::path& staging, const fs::path& final,
    const std::string& stage_kind, const std::string& final_marker) {
  if (mappings.empty()) return Failed("kit-invalid", "kit mapping is empty or ambiguous");
  const auto started = PartTimes::Clock::now();
  PartTimes times;
  std::uint64_t required = 0;
  for (const auto& mapping : mappings)
    if (mapping.file && mapping.file->size <= std::numeric_limits<std::uint64_t>::max() / 4 - required)
      required += mapping.file->size;
  // Progress: the bytes copied. Sources are not hashed again here: kit admission
  // hashed every kit file when Playden started.
  const std::uint64_t work_total = required;
  std::uint64_t work_done = 0;
  const auto report = [&](std::uint64_t amount) {
    work_done += amount;
    if (operations_.progress && work_total > 0) operations_.progress(std::min(work_done, work_total), work_total);
  };
  report(0);
  required = 0;
  auto since = PartTimes::Clock::now();
  for (const auto& mapping : mappings) {
    if (!mapping.file || mapping.file->size > std::numeric_limits<std::uint64_t>::max() - required)
      return Failed("kit-invalid", "kit file sizes overflow");
    required += mapping.file->size;
    const auto& source = mapping.source;
    bool mode_ok = false;
    std::error_code error;
    // A game's own metadata file is private (0600); the title copy takes the kit mode.
    if ((mapping.owned ? false : !PathComponentsHaveNoSymlink(kit_.root, mapping.source_relative, true)) ||
        !IsRegularNoSymlink(source) || fs::file_size(source, error) != mapping.file->size || error ||
        (!mapping.owned && (FileMode(source, &mode_ok) != mapping.file->mode || !mode_ok))) {
      return Failed("kit-source-invalid", "kit source is missing, linked, or has wrong mode");
    }
  }
  times.Add("source_check", since);
  since = PartTimes::Clock::now();
  const auto overhead = std::max(kCopyOverheadFloor, required / 10);
  if (required > std::numeric_limits<std::uint64_t>::max() - overhead)
    return Failed("kit-invalid", "space requirement overflows");
  std::error_code error;
  const auto staging_parent_relative = staging.parent_path().lexically_relative(paths_.host_data_root);
  if (staging_parent_relative.empty() ||
      !PathComponentsHaveNoSymlink(paths_.host_data_root, staging_parent_relative, true))
    return Failed("staging-unavailable", "owned staging root is unavailable");
  // /data/playden and its staging folder are shared parents: 01777 when created here.
  CreateDirectoriesWithMode(staging.parent_path(), 01777, error);
  if (error)
    return Failed("staging-unavailable", "owned staging root is unavailable");
  if (!SyncAncestors(staging.parent_path()))
    return Failed("sync-failed", "created staging ancestors were not durably synced");
  const auto available = operations_.free_bytes ? operations_.free_bytes(staging.parent_path())
                                                : std::optional<std::uint64_t>{};
  if (available ? *available < required + overhead : !operations_.free_space_unverifiable)
    return Failed("space-unavailable", "insufficient verified free space for package copy");

  const auto expected_stage = StageMarker(record, kit_, stage_kind);
  bool staging_known = false;
  const bool staging_exists = ExistsNoThrow(staging, &staging_known);
  if (!staging_known)
    return Failed("staging-unavailable", "staging state is unavailable");
  if (staging_exists) {
    // A stop between creating the staging folder and writing its owner marker
    // leaves an empty folder of ours: it holds nothing and is reclaimed.
    struct stat existing{};
    std::error_code listing;
    if (::lstat(staging.c_str(), &existing) == 0 && S_ISDIR(existing.st_mode) && existing.st_uid == ::geteuid() &&
        fs::is_empty(staging, listing) && !listing && ::rmdir(staging.c_str()) == 0)
      return CopyMappings(record, mappings, staging, final, stage_kind, final_marker);
  }
  if (staging_exists) {
    if (stage_kind == "template" && !IsPrivateDirectory(staging, true))
      return Failed("staging-collision", "existing bottle staging has wrong owner or permissions");
    // An earlier install of this game (another kit, title or recipe) can leave its
    // incomplete staging behind; it is ours and holds nothing published.
    const bool current = MarkerEquals(staging / kStageOwner, expected_stage);
    if (!current && !StageMarkerNamesInstall(staging / kStageOwner, record, stage_kind))
      return Failed("staging-collision", "staging path is not owned by this install");
    if (!current && operations_.log)
      operations_.log("package stage=" + stage_kind + " reclaims staging left by an earlier install");
    // RuntimeInstallService has already established stopped activity. Only our
    // identity-marked incomplete staging directory is eligible for cleanup.
    if (!RemoveTreeNoFollow(staging.string()))
      return Failed("staging-cleanup", "owned incomplete staging could not be cleaned");
  }
  // Staging trees are private to the app user. A published title's directories
  // become 0755 afterwards so ShadowMount can read them; title files keep their
  // kit modes. A bottle's way to its user profile is 0711 with bottle-profile-shared.
  const bool template_stage = stage_kind == "template";
  const auto resolved = template_stage ? Resolve(record) : std::nullopt;
  const bool shared_profile = resolved && ProfileShared(record, *resolved);
  PrivateTraversal staging_traversal;
  if (template_stage)
    staging_traversal = {{staging, staging / "drive_c", staging / "drive_c/users"},
                         static_cast<mode_t>(shared_profile ? 0711 : 0700)};
  const fs::path profile_relative = "drive_c/users/prospero";
  {
    std::string detail;
    if (!EnsurePrivateDirectories(paths_.host_data_root, staging, staging, &detail, staging_traversal))
      return Failed("private-directory", detail);
  }
  struct stat created_stage{};
  if (error || ::lstat(staging.c_str(), &created_stage) != 0)
    return Failed("staging-unavailable", "owned staging could not be initialized");
  if (!WriteFile(staging / kStageOwner, expected_stage, 0600)) {
    CleanInitialStage(staging, created_stage);
    return Failed("staging-unavailable", "owned staging could not be initialized");
  }
  if (!SyncPath(staging / kStageOwner, false) || !SyncPath(staging, true))
    return Failed("sync-failed", "owned staging identity was not durably synced");
  if (operations_.canceled && operations_.canceled()) return Canceled();
  times.Add("setup", since);

  std::vector<char> buffer(kCopyBufferBytes);  // heap: console worker stacks are small
  std::set<fs::path> copied_directories;
  std::set<fs::path> checked_directories;  // folders created and checked once per copy
  for (const auto& mapping : mappings) {
    if (operations_.canceled && operations_.canceled()) return Canceled();
    const auto& source = mapping.source;
    const auto destination = staging / mapping.destination_relative;
    since = PartTimes::Clock::now();
    if (!checked_directories.count(destination.parent_path())) {
      if (!PathComponentsHaveNoSymlink(staging, mapping.destination_relative, false))
        return Failed("destination-invalid", "staging destination contains a symbolic link");
      std::string detail;
      if (!EnsurePrivateDirectories(paths_.host_data_root, staging, destination.parent_path(), &detail,
                                    staging_traversal))
        return Failed("private-directory", detail);
      checked_directories.insert(destination.parent_path());
    }
    if (error) return Failed("copy-failed", "staging directory could not be created");
    times.Add("dirs", since);
    since = PartTimes::Clock::now();
    const int input = ::open(source.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    // The user profile is open to FTP with bottle-profile-shared (the walk below opens its folders).
    const fs::path relative(mapping.destination_relative);
    const bool profile_file = shared_profile &&
        std::mismatch(profile_relative.begin(), profile_relative.end(), relative.begin(), relative.end()).first ==
            profile_relative.end();
    const bool private_profile_file = profile_file &&
        IsPrivateProfilePath(relative.lexically_relative(profile_relative), operations_.private_profile_paths);
    const unsigned mode = private_profile_file ? 0600 : profile_file ? (0666 | (mapping.file->mode & 0111))
                          : template_stage ? (0600 | (mapping.file->mode & 0100)) : mapping.file->mode;
    const int output = ::open(destination.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, mode);
    if (input < 0 || output < 0) {
      if (input >= 0) ::close(input);
      if (output >= 0) ::close(output);
      return Failed("copy-failed", "package file could not be opened exclusively");
    }
    struct stat destination_info{};
    if (::fchmod(output, mode) != 0 || ::fstat(output, &destination_info) != 0 ||
        !S_ISREG(destination_info.st_mode) || destination_info.st_uid != ::geteuid() ||
        (destination_info.st_mode & 07777) != mode) {
      ::close(input); ::close(output); return Failed("copy-failed", "package file mode could not be set before copying");
    }
    bool wrote = true, read_ok = true;
    std::uint64_t copied_bytes = 0;
    for (;;) {
      const auto count = ::read(input, buffer.data(), buffer.size());
      if (count < 0 && errno == EINTR) continue;
      if (count < 0) { read_ok = false; break; }
      if (count == 0) break;
      ssize_t offset = 0;
      while (offset < count) {
        const auto written = ::write(output, buffer.data() + offset, static_cast<std::size_t>(count - offset));
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) { wrote = false; break; }
        offset += written;
      }
      if (!wrote) break;
      copied_bytes += static_cast<std::uint64_t>(count);
      report(static_cast<std::uint64_t>(count));
      if (operations_.canceled && operations_.canceled()) {
        ::close(input); ::close(output);
        return Canceled();
      }
    }
    ::close(input);
    const bool closed = ::close(output) == 0;
    if (!read_ok || !wrote || !closed)
      return Failed("copy-failed", "package file copy or mode publication failed");
    if (copied_bytes != mapping.file->size)
      return Failed("copy-verification", "copied package file size does not match");
    times.Add("copy", since);
    since = PartTimes::Clock::now();
    if (!SyncPath(destination, false))
      return Failed("sync-failed", "copied package file was not durably synced");
    times.Add("file_sync", since);
    for (auto directory = destination.parent_path(); directory != staging.parent_path();
         directory = directory.parent_path()) {
      copied_directories.insert(directory);
      if (directory == staging) break;
    }
  }
  since = PartTimes::Clock::now();
  // The kit lists files only, so the profile's empty folders never arrive (the kit has no file
  // under users/prospero at all), and wineboot (which makes them on a PC) cannot run on the
  // console. Without AppData/LocalLow, SHGetKnownFolderPath fails and Unity has no persistent
  // data path: Caves of Qud stops at startup and other Unity games save beside their program,
  // where Cloud saves cannot see them.
  // With bottle-profile-shared the profile's folders are 0777 (FTP runs as root, which has no
  // permission bypass) and anything the kit put there is opened up too.
  static constexpr const char* kProfileFolders[] = {"AppData/LocalLow", "AppData/Local", "AppData/Roaming",
                                                    "Documents", "Saved Games", "Desktop", "Downloads",
                                                    "Music", "Pictures", "Videos"};
  if (template_stage) {
    const fs::path user = staging / profile_relative;
    if (shared_profile) {
      std::string detail;
      if (!EnsurePrivateDirectories(paths_.host_data_root, staging, user.parent_path(), &detail, staging_traversal))
        return Failed("private-directory", detail);
      CreateDirectoriesWithMode(user, 0777, error);
      if (error) return Failed("private-directory", "cannot create the bottle's user profile: " + user.string());
      for (const char* folder : kProfileFolders) {
        auto current = platform::ManagedDirectory::Open(user, platform::DirectoryPolicy::kApprovedTree);
        for (const auto& part : fs::path(folder)) {
          auto next = current ? current->OpenChild(part.string(), platform::DirectoryPolicy::kApprovedTree, true, 0777)
                              : std::nullopt;
          if (!next || !next->Sync() || !current->Sync())
            return Failed("private-directory", "cannot create profile folder " + (user / folder).string() + ": " +
                                                   strerror(errno));
          current = std::move(next);
        }
      }
      if (!OpenTreeToEveryone(user, {operations_.private_profile_paths, {}}).ok)
        return Failed("private-directory", "cannot open the bottle's user profile: " + user.string());
    }
    for (const char* folder : kProfileFolders) {
      if (!shared_profile) {
        std::string detail;
        if (!EnsurePrivateDirectories(paths_.host_data_root, staging, user / folder, &detail))
          return Failed("private-directory", detail);
      }
      for (auto directory = user / folder; directory != staging; directory = directory.parent_path())
        copied_directories.insert(directory);
    }
  }
  if (stage_kind == "template" && !operations_.validate_bottle(staging))
    return Failed("template-validation", "staged bottle failed semantic validation");
  if (!WriteFile(staging / final_marker, OwnerMarker(record, kit_, stage_kind.c_str()), 0600))
    return Failed("ownership-write", "package ownership marker could not be written");
  if (!SyncPath(staging / final_marker, false))
    return Failed("sync-failed", "package ownership marker was not durably synced");
  std::vector<fs::path> directories(copied_directories.begin(), copied_directories.end());
  std::sort(directories.begin(), directories.end(), [](const fs::path& left,
                                                        const fs::path& right) {
    return std::distance(left.begin(), left.end()) >
           std::distance(right.begin(), right.end());
  });
  for (const auto& directory : directories)
    if (!SyncPath(directory, true))
      return Failed("sync-failed", "staged package directory was not durably synced");
  if (!SyncPath(staging, true) || !SyncAncestors(staging.parent_path()))
    return Failed("sync-failed", "staging parent was not durably synced");
  const auto final_parent_relative = final.parent_path().lexically_relative(paths_.host_data_root);
  if (final_parent_relative.empty() ||
      !PathComponentsHaveNoSymlink(paths_.host_data_root, final_parent_relative, true))
    return Failed("destination-invalid", "final destination contains a symbolic link");
  if (stage_kind == "template") {
    std::string detail;
    const auto game = paths_.host_data_root / "playden/games" / record.install_id;
    const PrivateTraversal parents{{game, game / "library", game / "library/prefixes"},
                                   static_cast<mode_t>(shared_profile ? 0711 : 0700)};
    if (shared_profile && !SetTraversableMode(game.parent_path(), 0711))
      return Failed("repair-mode", "the games profile parent could not be opened");
    if (!EnsurePrivateDirectories(paths_.host_data_root, game, final.parent_path(), &detail, parents))
      return Failed("private-directory", detail);
  } else {
    CreateDirectoriesWithMode(final.parent_path(), 0755, error);
  }
  if (error)
    return Failed("destination-unwritable", "Playden can't create " + final.parent_path().string() +
                                                " (errno=" + std::to_string(error.value()) +
                                                " message=" + error.message() + ")");
  bool final_known = false;
  const bool final_exists = ExistsNoThrow(final, &final_known);
  if (!final_known || final_exists)
    return Failed("destination-collision", "final package path already exists");
  if (!SyncAncestors(final.parent_path()))
    return Failed("sync-failed", "destination parent was not durably synced before rename");
  // Retry brief filesystem contention without recopying the durable staging tree.
  // Cross-volume moves must still fail: copying into the watched title is not atomic.
  constexpr int kRenameAttempts = 4;
  for (int attempt = 1; attempt <= kRenameAttempts; ++attempt) {
    if (operations_.canceled && operations_.canceled()) return Canceled();
    if (attempt > 1) {
      if (!PathComponentsHaveNoSymlink(paths_.host_data_root, final_parent_relative, true))
        return Failed("destination-invalid", "final destination contains a symbolic link");
      if (ExistsNoThrow(final, &final_known) || !final_known)
        return Failed("destination-collision", "final package path appeared or became unavailable during rename retry");
    }
    error.clear();
    if (operations_.rename_path) operations_.rename_path(staging, final, error);
    else fs::rename(staging, final, error);
    if (!error) break;
    const std::string detail = "errno=" + std::to_string(error.value()) + " category=" + error.category().name() +
                               " message=" + error.message();
    if (operations_.log)
      operations_.log("package rename stage=" + stage_kind + " install=" + record.install_id +
                      " attempt=" + std::to_string(attempt) + " from=" + staging.string() +
                      " to=" + final.string() + " " + detail);
    const bool transient = error == std::errc::device_or_resource_busy || error == std::errc::interrupted ||
                           error == std::errc::resource_unavailable_try_again;
    const bool denied = error == std::errc::permission_denied || error == std::errc::operation_not_permitted;
    if (!transient || attempt == kRenameAttempts)
      return Failed("atomic-rename-failed",
                    std::string(error == std::errc::cross_device_link
                                    ? "package staging and destination must be on the same filesystem"
                                    : denied ? "Playden can't write to " + final.parent_path().string() +
                                                   " (permission denied)"
                                             : "package staging rename into " + final.parent_path().string() +
                                                   " failed") + " (" + detail + ")");
    std::this_thread::sleep_for(std::chrono::milliseconds(100 * attempt));
  }
  if (!SyncTree(final, false) || !SyncAncestors(final.parent_path()) ||
      !SyncAncestors(staging.parent_path()))
    return Failed("sync-failed", "atomic package rename was not durably synced");
  times.Add("final_sync", since);
  if (operations_.log)
    operations_.log("package copy stage=" + stage_kind + " files=" + std::to_string(mappings.size()) +
                    " bytes=" + std::to_string(required) + " " + times.Summary(started));
  return Complete();
}

namespace {
// ShadowMount registers a title by reading its folder, and it cannot read a
// 0700 folder owned by the app user. Owned title directories are therefore
// 0755 (still not writable by others). Only directories owned by this user
// are changed; links and foreign entries refuse.
// Every folder of the title is an owned 0755 directory (what registration reads).
bool TitleDirectoriesPublished(const fs::path& title) {
  std::vector<fs::path> pending{title};
  while (!pending.empty()) {
    const auto directory = pending.back(); pending.pop_back();
    struct stat info{};
    if (::lstat(directory.c_str(), &info) != 0 || !S_ISDIR(info.st_mode) || info.st_uid != ::geteuid() ||
        (info.st_mode & 07777) != 0755)
      return false;
    std::error_code error;
    for (fs::directory_iterator it(directory, error), end; !error && it != end; it.increment(error)) {
      struct stat child{};
      if (::lstat(it->path().c_str(), &child) != 0 || S_ISLNK(child.st_mode)) return false;
      if (S_ISDIR(child.st_mode)) pending.push_back(it->path());
    }
    if (error) return false;
  }
  return true;
}

bool PublishTitleDirectories(const fs::path& title) {
  struct stat info{};
  if (::lstat(title.c_str(), &info) != 0 || !S_ISDIR(info.st_mode) || info.st_uid != ::geteuid()) return false;
  std::vector<fs::path> pending{title};
  while (!pending.empty()) {
    const auto directory = pending.back(); pending.pop_back();
    if (::lstat(directory.c_str(), &info) != 0 || !S_ISDIR(info.st_mode) || info.st_uid != ::geteuid()) return false;
    if ((info.st_mode & 07777) != 0755 && ::chmod(directory.c_str(), 0755) != 0) return false;
    std::error_code error;
    for (fs::directory_iterator it(directory, error), end; !error && it != end; it.increment(error)) {
      struct stat child{};
      if (::lstat(it->path().c_str(), &child) != 0 || S_ISLNK(child.st_mode)) return false;
      if (S_ISDIR(child.st_mode)) pending.push_back(it->path());
    }
    if (error) return false;
  }
  return true;
}
}  // namespace

PackageActionResult FilesystemPackageInstaller::StageTitle(
    const RuntimeInstall& record) {
  const auto paths = Resolve(record);
  if (!paths) return Failed("kit-invalid", "record or verified kit view is invalid");
  // An owned title with the right files is (re)published: a stop between the
  // move into place and publishing can leave its folders private.
  if (IsDirectoryNoSymlink(paths->title) &&
      MarkerEquals(paths->title / kTitleOwner, OwnerMarker(record, kit_, "title")) &&
      TitleFilesMatch(record, *paths)) {
    const auto started = PartTimes::Clock::now();
    const bool durable = PublishTitleDirectories(paths->title) && SyncTree(paths->title) &&
                         SyncAncestors(paths->title.parent_path());
    if (operations_.log) operations_.log("title republish " + PartTimes{}.Summary(started));
    return durable ? Complete()
                   : Failed("sync-failed", "owned native title durability could not be established");
  }
  bool title_known = false;
  const bool title_exists = ExistsNoThrow(paths->title, &title_known);
  if (!title_known) return Failed("title-unavailable", "native title state is unavailable");
  if (title_exists)
    return Failed("title-collision", "native title source already exists and is not owned");
  const auto copied = CopyMappings(record, TitleMappings(record), paths->title_staging, paths->title,
                                  "title", kTitleOwner);
  if (copied.state != PackageActionState::kComplete) return copied;
  const auto started = PartTimes::Clock::now();
  const bool published = PublishTitleDirectories(paths->title) && SyncTree(paths->title, false);
  if (operations_.log) operations_.log("title publish " + PartTimes{}.Summary(started));
  return published
             ? copied
             : Failed("title-modes", "owned native title directories could not be made readable to registration");
}

namespace {
// Marker text without the lines a respin of the same kit may change (its file
// inventory, a rebuilt runtime's revision and path, and the kit title the game's
// title is made from: a fixed one, or the one shell for every game), for
// comparing identity. A kit id the kit `kit_id` was renamed from counts as it.
std::string RespinIdentity(const std::string& marker, const std::string& kit_id) {
  std::string kept;
  std::size_t start = 0;
  while (start < marker.size()) {
    auto end = marker.find('\n', start);
    if (end == std::string::npos) end = marker.size();
    const auto line = marker.substr(start, end - start);
    if (line.rfind("kit_id=", 0) == 0 && KitIdRenamedTo(line.substr(7), kit_id))
      kept += "kit_id=" + kit_id + "\n";
    else if (line.rfind("files_sha256=", 0) != 0 && line.rfind("runtime_revision=", 0) != 0 &&
             line.rfind("runtime_path=", 0) != 0 && line.rfind("variant_id=", 0) != 0)
      kept += line + "\n";
    start = end + 1;
  }
  return kept;
}

// Marker text with its files_sha256 line replaced, for comparing two kits that
// differ only in their file inventory.
std::optional<std::string> WithFilesDigest(const std::string& marker, const std::string& digest) {
  const auto at = marker.find("\nfiles_sha256=");
  if (at == std::string::npos) return std::nullopt;
  const auto end = marker.find('\n', at + 1);
  if (end == std::string::npos) return std::nullopt;
  return marker.substr(0, at) + "\nfiles_sha256=" + digest + marker.substr(end);
}
}  // namespace

bool FilesystemPackageInstaller::TitleCurrent(const RuntimeInstall& record) {
  const auto paths = Resolve(record);
  return paths && MarkerEquals(paths->title / kTitleOwner, OwnerMarker(record, kit_, "title")) &&
         MarkerEquals(paths->bottle / kTemplateOwner, OwnerMarker(record, kit_, "template"));
}

bool FilesystemPackageInstaller::SetupFromOtherKit(const RuntimeInstall& record) {
  const auto paths = Resolve(record);
  if (!paths) return false;
  // Only folders whose markers name this install count; another owner's folder is a collision, not this.
  const auto other_kit = [&](const fs::path& folder, const char* marker, const char* kind) {
    return IsDirectoryNoSymlink(folder) && MarkerIdentifies(folder / marker, record, kind) &&
           !MarkerEquals(folder / marker, OwnerMarker(record, kit_, kind));
  };
  return other_kit(paths->title, kTitleOwner, "title") || other_kit(paths->bottle, kTemplateOwner, "template");
}

PackageActionResult FilesystemPackageInstaller::RefreshTitle(const RuntimeInstall& record) {
  const auto paths = Resolve(record);
  if (!paths) return Failed("kit-invalid", "record or verified kit view is invalid");
  if (record.artifacts.size() != kit_.artifacts.size())
    return Failed("refresh-artifacts", "runtime or recipe artifacts differ; a full upgrade is required");
  for (const auto& artifact : kit_.artifacts) {
    bool found = false;
    for (const auto& bound : record.artifacts)
      found |= bound.artifact_id == artifact.artifact_id && bound.identity == artifact.identity &&
               bound.sha256 == artifact.sha256;
    if (!found) return Failed("refresh-artifacts", "runtime or recipe artifacts differ; a full upgrade is required");
  }
  const auto title_marker = ReadBounded(paths->title / kTitleOwner, 4096);
  const auto bottle_marker = ReadBounded(paths->bottle / kTemplateOwner, 4096);
  const auto new_title = OwnerMarker(record, kit_, "title");
  const auto new_bottle = OwnerMarker(record, kit_, "template");
  auto old_digest = [](const std::optional<std::string>& marker) -> std::string {
    if (!marker) return {};
    const auto at = marker->find("\nfiles_sha256=");
    if (at == std::string::npos) return {};
    const auto start = at + std::strlen("\nfiles_sha256=");
    return marker->substr(start, marker->find('\n', start) - start);
  };
  const auto title_digest = old_digest(title_marker);
  if (!IsDirectoryNoSymlink(paths->title) || !IsPrivateDirectoryTree(paths->game, paths->bottle, BottleTraversal(*paths)) ||
      !title_marker || !bottle_marker || !IsHexDigest(title_digest) || old_digest(bottle_marker) != title_digest ||
      !WithFilesDigest(new_title, title_digest) || !WithFilesDigest(new_bottle, title_digest) ||
      RespinIdentity(new_title, kit_.kit_id) != RespinIdentity(*title_marker, kit_.kit_id) ||
      RespinIdentity(new_bottle, kit_.kit_id) != RespinIdentity(*bottle_marker, kit_.kit_id))
    return Failed("refresh-ownership", "installed title and bottle are not this install's from a compatible kit");
  if (title_digest == kit_.files_sha256 && TitleMatches(record, *paths)) return Complete();
  std::vector<char> buffer(kCopyBufferBytes);  // heap: console worker stacks are small
  const auto mappings = TitleMappings(record);
  // Progress: the title's bytes, checked or copied; a file already current counts as done.
  std::uint64_t work_total = 0;
  for (const auto& mapping : mappings)
    if (mapping.file) work_total += mapping.file->size;
  std::uint64_t work_done = 0;
  const auto report = [&](std::uint64_t amount) {
    work_done += amount;
    if (operations_.progress && work_total > 0) operations_.progress(std::min(work_done, work_total), work_total);
  };
  report(0);
  for (const auto& mapping : mappings) {
    if (operations_.canceled && operations_.canceled()) return Canceled();
    const auto destination = paths->title / mapping.destination_relative;
    bool mode_ok = false;
    std::error_code size_error;
    if (IsRegularNoSymlink(destination) && fs::file_size(destination, size_error) == mapping.file->size &&
        !size_error && FileMode(destination, &mode_ok) == mapping.file->mode && mode_ok) {
      const auto digest = operations_.hash_file(destination);
      if (digest && *digest == mapping.file->sha256) { report(mapping.file->size); continue; }
    }
    if (!PathComponentsHaveNoSymlink(paths->title, mapping.destination_relative, false))
      return Failed("destination-invalid", "title destination contains a symbolic link");
    std::string detail;
    // A kit may add folders to the title (kit G's win/mesa-zink): make them as a full install does.
    if (!IsDirectoryNoSymlink(destination.parent_path())) {
      std::error_code created;
      CreateDirectoriesWithMode(destination.parent_path(), 0755, created);
      if (created || !IsDirectoryNoSymlink(destination.parent_path()))
        return Failed("refresh-layout", "title directory layout changed; a full upgrade is required");
    }
    const auto temporary = destination.parent_path() / ("." + destination.filename().string() + ".refresh");
    ::unlink(temporary.c_str());
    const auto& source = mapping.source;
    const int input = ::open(source.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    const int output = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    bool ok = input >= 0 && output >= 0 && ::fchmod(output, mapping.file->mode) == 0;
    while (ok) {
      const auto count = ::read(input, buffer.data(), buffer.size());
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) { ok = count == 0; break; }
      for (ssize_t offset = 0; ok && offset < count;) {
        const auto written = ::write(output, buffer.data() + offset, static_cast<std::size_t>(count - offset));
        if (written < 0 && errno == EINTR) continue;
        ok = written > 0; offset += written > 0 ? written : 0;
      }
      if (ok) report(static_cast<std::uint64_t>(count));
    }
    ok = ok && ::fsync(output) == 0;
    if (input >= 0) ::close(input);
    if (output >= 0) ok = ::close(output) == 0 && ok;
    const auto copied = ok ? operations_.hash_file(temporary) : std::nullopt;
    if (!copied || *copied != mapping.file->sha256 || ::rename(temporary.c_str(), destination.c_str()) != 0) {
      ::unlink(temporary.c_str());
      return Failed("refresh-copy", "a title file could not be replaced");
    }
  }
  // The replaced files are durable, published and verified before the markers
  // change: until then the title is not considered current, and a stop anywhere
  // before this point leaves a refresh that runs again.
  if (!PublishTitleDirectories(paths->title) || !SyncTree(paths->title) || !TitleFilesMatch(record, *paths))
    return Failed("refresh-verify", "the refreshed title does not match the kit");
  for (const auto& [path, text] : {std::pair{paths->title / kTitleOwner, new_title},
                                   std::pair{paths->bottle / kTemplateOwner, new_bottle}}) {
    const auto temporary = path.parent_path() / (path.filename().string() + ".refresh");
    ::unlink(temporary.c_str());
    if (!WriteFile(temporary, text, 0600) || !SyncPath(temporary, false) ||
        ::rename(temporary.c_str(), path.c_str()) != 0 || !SyncPath(path.parent_path(), true))
      return Failed("refresh-marker", "an owner marker could not be rewritten");
  }
  if (!PublishTitleDirectories(paths->title) || !SyncTree(paths->title) || !TitleMatches(record, *paths))
    return Failed("refresh-verify", "the refreshed title does not match the kit");
  return Complete();
}

PackageActionResult FilesystemPackageInstaller::CopyTemplate(
    const RuntimeInstall& record) {
  const auto paths = Resolve(record);
  if (!paths) return Failed("kit-invalid", "record or verified kit view is invalid");
  const auto table = ReadBounded(kit_.root / kit_.template_drive_table, 1024);
  if (!table || *table != "c:\t../drive_c\nz:\t/\n")
    return Failed("template-links-invalid", "verified template drive table is not exact");
  bool bottle_known = false;
  const bool bottle_exists = ExistsNoThrow(paths->bottle, &bottle_known);
  if (!bottle_known) return Failed("bottle-unavailable", "mutable bottle state is unavailable");
  if (bottle_exists) {
    if (!BottleOwned(record, *paths)) return Failed("bottle-collision", "mutable bottle exists without matching ownership");
    if (!SyncTree(paths->bottle) || !SyncAncestors(paths->bottle.parent_path()))
      return Failed("sync-failed", "owned mutable bottle durability could not be established");
    return profile_shared_ ? SettleBottleModes(record) : Complete();
  }
  auto result = CopyMappings(record, TemplateMappings(), paths->bottle_staging,
                             paths->bottle, "template", kTemplateOwner);
  if (result.state != PackageActionState::kComplete) return result;
  const auto started = PartTimes::Clock::now();
  const bool owned = BottleOwned(record, *paths);
  if (operations_.log) operations_.log("template check " + PartTimes{}.Summary(started));
  if (!owned) return Failed("template-validation", "published bottle failed semantic validation");
  // Folders made before this bottle (its data root for the title metadata) get the kit's mode too.
  return profile_shared_ ? SettleBottleModes(record) : result;
}

PackageActionResult FilesystemPackageInstaller::ValidateArtifacts(
    const RuntimeInstall& record) {
  const auto paths = Resolve(record);
  if (!paths) return Failed("kit-invalid", "record or verified kit view is invalid");
  const auto started = PartTimes::Clock::now();
  PartTimes times;
  auto since = PartTimes::Clock::now();
  bool valid = TitleMatches(record, *paths);
  times.Add("title", since); since = PartTimes::Clock::now();
  valid = valid && BottleOwned(record, *paths);
  times.Add("bottle", since); since = PartTimes::Clock::now();
  valid = valid && ArtifactsMatch(record, *paths);
  times.Add("artifacts", since);
  if (operations_.log) operations_.log("package validate " + times.Summary(started));
  if (!valid)
    return Failed("artifact-invalid", "immutable artifacts or template provenance do not match");
  return Complete();
}

PackageActionResult FilesystemPackageInstaller::PublishProfile(const RuntimeInstall& record) {
  const auto paths = Resolve(record);
  if (!paths) return Failed("kit-invalid", "record or verified kit view is invalid");
  const auto started = PartTimes::Clock::now();
  PartTimes times;
  auto since = PartTimes::Clock::now();
  if (!TitleMatches(record, *paths) || !BottleOwned(record, *paths) || !ArtifactsMatch(record, *paths))
    return Failed("publication-prerequisite", "immutable title, artifacts, and bottle ownership must validate first");
  times.Add("prerequisites", since); since = PartTimes::Clock::now();
  if (!operations_.provide_publication || !operations_.publish_owned)
    return Failed("publication-unavailable", "complete OWNER publication driver is unavailable");
  const auto publication = operations_.provide_publication(record);
  if (!publication || !PublicationIdentityMatches(record, *publication))
    return Failed("publication-invalid", "owner publication bytes do not match install identity");
  if (!PublicationPathsSafe(*publication, paths->publication))
    return Failed("managed-path-invalid", "managed publication path changed or contains a symbolic link");
  // Always hand off to the full driver, including committed retries: a receipt
  // alone cannot bypass durable OWNER/native reconciliation. No directory or
  // live settings effects may precede the driver's pending receipt protocol.
  times.Add("provide", since); since = PartTimes::Clock::now();
  const auto result = operations_.publish_owned(record, paths->publication, *publication);
  times.Add("publish", since); since = PartTimes::Clock::now();
  if (result.state != PackageActionState::kComplete) {
    if (operations_.log) operations_.log("package publish " + times.Summary(started));
    return result;
  }
  const auto observed = Resolve(record);
  const bool verified = observed && PublicationMatches(record, *observed, nullptr);
  times.Add("verify", since);
  if (operations_.log) operations_.log("package publish " + times.Summary(started));
  if (!verified)
    return Failed("publication-verification", "complete committed settings publication failed independent admission");
  return Complete();
}

}  // namespace playden::runtime
