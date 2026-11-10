#include "runtime/kit_manifest.h"
#include "runtime/game_settings.h"
#include "runtime/pe_architecture.h"

#include "steam/backend/json.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <fcntl.h>
#include <map>
#include <set>
#include <string>
#include <unistd.h>
#include <system_error>

namespace playden::runtime {
namespace {

namespace fs = std::filesystem;
using Json = steam::json::Value;

constexpr std::size_t kMaxManifestBytes = 8 * 1024 * 1024;
constexpr std::size_t kMaxFiles = 32768;
constexpr std::size_t kMaxVariants = 1000;
constexpr const char* kSchema = "playden-prospero-kit/1";
constexpr const char* kPublicationGuard = "playden-publication-guard-v1";
constexpr const char* kCoreDlls[] = {"ntdll.dll", "kernel32.dll", "kernelbase.dll",
                                     "mmdevapi.dll", "dinput8.dll", "rsaenh.dll"};
constexpr const char* kNativePrxs[] = {"ntdll.prx", "wineserver.prx", "win32u.prx",
                                       "wineps5.prx", "libvulkan.prx", "wowprospero.prx"};
constexpr const char* kDxvkDlls[] = {"d3d8.dll", "d3d9.dll", "d3d10core.dll",
                                     "d3d11.dll", "dxgi.dll"};

KitManifestResult Failure(KitManifestStatus status, std::string reason) {
  KitManifestResult result;
  result.status = status;
  result.reason = std::move(reason);
  return result;
}

bool Digest(const std::string& value) {
  return value.size() == 64 && std::all_of(value.begin(), value.end(), [](unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
  });
}

bool Identifier(const std::string& value) {
  if (value.empty() || value.size() > 96 ||
      !((value[0] >= 'a' && value[0] <= 'z') || std::isdigit(static_cast<unsigned char>(value[0]))))
    return false;
  return std::all_of(value.begin(), value.end(), [](unsigned char c) {
    return (c >= 'a' && c <= 'z') || std::isdigit(c) || c == '.' || c == '_' || c == '-';
  });
}

bool TitleId(const std::string& value) {
  return value.size() == 9 && value.compare(0, 6, "FAKE78") == 0 &&
         std::all_of(value.begin() + 6, value.end(), [](unsigned char c) { return std::isdigit(c); });
}

bool SafeRelative(const std::string& value) {
  if (value.empty() || value.size() > 4096 || value.front() == '/' || value.find('\\') != std::string::npos ||
      value.find('\0') != std::string::npos) return false;
  std::size_t begin = 0;
  while (begin <= value.size()) {
    const auto end = value.find('/', begin);
    const auto part = value.substr(begin, end == std::string::npos ? value.size() - begin : end - begin);
    if (part.empty() || part == "." || part == "..") return false;
    if (end == std::string::npos) break;
    begin = end + 1;
  }
  return true;
}

std::optional<std::string> Read(const fs::path& path, std::size_t limit) {
  std::error_code error;
  const auto status = fs::symlink_status(path, error);
  if (error || !fs::is_regular_file(status) || fs::is_symlink(status)) return std::nullopt;
  const auto size = fs::file_size(path, error);
  if (error || size > limit) return std::nullopt;
  // POSIX reads: the console's libc lacks the locale support iostreams need.
  const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) return std::nullopt;
  std::string result(static_cast<std::size_t>(size), '\0');
  std::size_t done = 0;
  while (done < result.size()) {
    const auto count = ::read(fd, result.data() + done, result.size() - done);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) { ::close(fd); return std::nullopt; }
    done += static_cast<std::size_t>(count);
  }
  ::close(fd);
  return result;
}

const Json* Field(const Json& value, const char* name, Json::Type type) {
  const Json* found = value.Find(name);
  return found && found->type() == type ? found : nullptr;
}

bool HasOnlyKeys(const Json& value, std::initializer_list<std::string_view> names) {
  if (!value.is_object() || value.object().size() != names.size()) return false;
  for (const auto& entry : value.object())
    if (std::find(names.begin(), names.end(), entry.first) == names.end()) return false;
  return true;
}

std::optional<Json> ParseFile(const fs::path& path, std::size_t bytes = 1024 * 1024,
                              std::size_t values = 16384) {
  const auto source = Read(path, bytes);
  if (!source) return std::nullopt;
  return steam::json::Parse(*source, {bytes, 16, values, 1024 * 1024});
}

bool RegularNoSymlink(const fs::path& root, const std::string& relative) {
  if (!SafeRelative(relative)) return false;
  fs::path cursor = root;
  for (const auto& part : fs::path(relative)) {
    cursor /= part;
    std::error_code error;
    const auto status = fs::symlink_status(cursor, error);
    if (error || fs::is_symlink(status)) return false;
  }
  std::error_code error;
  return fs::is_regular_file(fs::symlink_status(cursor, error)) && !error;
}

bool PeArchitecture(const fs::path& path, bool pe64) {
  return ExecutableArchitecture(path.string()) == (pe64 ? "pe64" : "pe32");
}

bool ContainsLine(const std::string& bytes, const std::string& line) {
  std::size_t offset = 0;
  while ((offset = bytes.find(line, offset)) != std::string::npos) {
    const bool start = offset == 0 || bytes[offset - 1] == '\n';
    const auto end = offset + line.size();
    if (start && (end == bytes.size() || bytes[end] == '\n' || bytes[end] == '\r')) return true;
    ++offset;
  }
  return false;
}

bool RegistrySectionValue(const std::string& registry, const std::string& section,
                          const std::string& assignment) {
  const std::string header = "[" + section + "]";
  const auto begin = registry.find(header);
  if (begin == std::string::npos || registry.find(header, begin + header.size()) != std::string::npos) return false;
  const auto body = registry.find('\n', begin + header.size());
  if (body == std::string::npos) return false;
  const auto end = registry.find("\n[", body);
  return ContainsLine(registry.substr(body + 1, end == std::string::npos ? end : end - body - 1), assignment);
}

// Wine Type 001 names the selected provider; TypeName ("RSA Full...") is
// descriptive metadata and must never select the provider registration.
std::optional<std::string> RegistryStringValue(const std::string& registry,
                                             const std::string& section,
                                             const std::string& key) {
  const std::string header = "[" + section + "]";
  const auto begin = registry.find(header);
  if (begin == std::string::npos || (begin != 0 && registry[begin - 1] != '\n') ||
      registry.find(header, begin + header.size()) != std::string::npos) return std::nullopt;
  const auto body = registry.find('\n', begin + header.size());
  if (body == std::string::npos) return std::nullopt;
  const auto end = registry.find("\n[", body);
  const std::string lines_text = registry.substr(body + 1, end == std::string::npos ? end : end - body - 1);
  const std::string assignment = "\"" + key + "\"=";
  std::optional<std::string> value;
  for (std::size_t start = 0; start < lines_text.size();) {
    const auto stop = lines_text.find('\n', start);
    std::string line = lines_text.substr(start, stop == std::string::npos ? std::string::npos : stop - start);
    start = stop == std::string::npos ? lines_text.size() : stop + 1;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.compare(0, assignment.size(), assignment) != 0) continue;
    if (value || line.size() < assignment.size() + 2 ||
        line[assignment.size()] != '\"' || line.back() != '\"') return std::nullopt;
    value = line.substr(assignment.size() + 1, line.size() - assignment.size() - 2);
  }
  return value;
}

bool ValidateRegistry(const fs::path& bottle, bool require_audio) {
  const auto system = Read(bottle / "system.reg", 8 * 1024 * 1024);
  const auto user = Read(bottle / "user.reg", 8 * 1024 * 1024);
  if (!system || !user) return false;
  if (RegistryStringValue(*user, "Volatile Environment", "USERNAME") != "prospero" ||
      RegistryStringValue(*user, "Volatile Environment", "USERPROFILE") != R"(C:\\users\\prospero)")
    return false;
  if (!RegistrySectionValue(*system, R"(Software\\Microsoft\\Wow64\\x86)", R"(@="wowprospero.dll")")) return false;
  for (const char* prefix : {R"(Software\\)", R"(Software\\Wow6432Node\\)"}) {
    const std::string base = std::string(prefix) + R"(Microsoft\\Cryptography\\Defaults\\)";
    const auto selected = RegistryStringValue(*system, base + R"(Provider Types\\Type 001)", "Name");
    if (!selected || selected->empty() || selected->size() > 128 ||
        selected->find_first_of("\\[]\r\n\"") != std::string::npos) return false;
    const std::string provider = base + R"(Provider\\)" + *selected;
    if (!RegistrySectionValue(*system, provider, R"("Image Path"="C:\\windows\\system32\\rsaenh.dll")") ||
        !RegistrySectionValue(*system, provider, R"("Type"=dword:00000001)")) return false;
  }
  for (const auto& item : {std::pair{R"({BCDE0395-E52F-467C-8E3D-C4579291692E})", "mmdevapi.dll"},
                           std::pair{R"({25E609E4-B259-11CF-BFC7-444553540000})", "dinput8.dll"}}) {
    const std::string base = std::string(R"(Software\\Classes\\Wow6432Node\\CLSID\\)") + item.first;
    if (system->find("[" + base + "]") == std::string::npos ||
        !RegistrySectionValue(*system, base + R"(\\InprocServer32)",
                              std::string(R"(@="C:\\windows\\system32\\)") + item.second + R"(")") ||
        !RegistrySectionValue(*system, base + R"(\\InprocServer32)", R"("ThreadingModel"="Both")")) return false;
  }
  return !require_audio || RegistrySectionValue(*user, R"(Software\\Wine\\Drivers)", R"("Audio"="ps5")");
}

class Verifier {
 public:
  Verifier(fs::path root, const KitAdmissionConfig& config)
      : root_(std::move(root)), config_(config) {}

  KitManifestResult Run() {
    if (!config_.hash_file || !config_.hash_bytes)
      return Failure(KitManifestStatus::kUnsupportedKit, "cryptographic hash callbacks are required");
    if (std::find(config_.known_capabilities.begin(), config_.known_capabilities.end(),
                  kPublicationGuard) != config_.known_capabilities.end() &&
        (!config_.publication_provider_sha256 || !Digest(*config_.publication_provider_sha256)))
      return Failure(KitManifestStatus::kUnsupportedKit,
                     "publication guard trust requires an independent provider provenance digest");
    std::error_code error;
    const auto root_status = fs::symlink_status(root_, error);
    if (error || !fs::is_directory(root_status) || fs::is_symlink(root_status))
      return Failure(KitManifestStatus::kIntegrityFailure, "kit root is not an ordinary directory");
    // Which kit this is, for display; any build of prospero-win is admitted.
    const auto manifest_hash = config_.hash_file(root_ / "manifest.json");
    if (!manifest_hash) return Failure(KitManifestStatus::kIntegrityFailure, "manifest cannot be hashed");
    result_.manifest_sha256 = *manifest_hash;
    const auto bytes = Read(root_ / "manifest.json", kMaxManifestBytes);
    if (!bytes) return Failure(KitManifestStatus::kMalformed, "manifest cannot be read within bounds");
    const auto parsed = steam::json::Parse(*bytes, {kMaxManifestBytes, 16, kMaxFiles * 8, 1024 * 1024});
    if (!parsed || !parsed->is_object()) return Failure(KitManifestStatus::kMalformed, "manifest is not bounded JSON object");
    manifest_ = &*parsed;
    const auto* schema = Field(*manifest_, "schema", Json::Type::kString);
    if (!schema || schema->text() != kSchema)
      return Failure(KitManifestStatus::kUnsupportedSchema, "unknown kit manifest schema");
    const auto* architecture = Field(*manifest_, "architecture", Json::Type::kString);
    if (!architecture || architecture->text() != config_.required_architecture)
      return Failure(KitManifestStatus::kUnsupportedArchitecture, "kit architecture is not positively supported");
    auto contract = ValidateContract(); if (contract) return *contract;
    auto sources = ValidateSources(); if (sources) return *sources;
    auto files = ValidateFiles(); if (files) return *files;
    auto payload = ValidatePayload(); if (payload) return *payload;
    result_.status = KitManifestStatus::kVerified;
    result_.reason.clear();
    result_.view = std::move(view_);
    result_.artifacts.reserve(view_artifacts_.size());
    for (const auto& item : view_artifacts_)
      result_.artifacts.push_back({item.artifact_id, item.identity, item.sha256});
    return result_;
  }

 private:
  std::optional<KitManifestResult> ValidateContract() {
    const auto* kit_id = Field(*manifest_, "kit_id", Json::Type::kString);
    const auto* firmware = Field(*manifest_, "firmware", Json::Type::kArray);
    const auto* capabilities = Field(*manifest_, "capabilities", Json::Type::kArray);
    if (!kit_id || !Identifier(kit_id->text()) || !firmware || firmware->array().empty() || !capabilities)
      return Failure(KitManifestStatus::kMalformed, "kit identity, firmware, or capabilities are malformed");
    // The firmware a kit was tested on, for display; any firmware is admitted.
    std::set<std::string> seen;
    for (const auto& item : firmware->array()) {
      if (!item.is_string() || item.text().empty() || !seen.insert(item.text()).second)
        return Failure(KitManifestStatus::kMalformed, "firmware entries must be unique non-empty strings");
      result_.firmware.push_back(item.text());
    }
    seen.clear();
    std::vector<std::string> declared;
    for (const auto& item : capabilities->array()) {
      if (!item.is_string() || !seen.insert(item.text()).second)
        return Failure(KitManifestStatus::kMalformed, "capabilities must be unique strings");
      declared.push_back(item.text());
    }
    if (!std::is_sorted(declared.begin(), declared.end()))
      return Failure(KitManifestStatus::kMalformed, "capabilities must use canonical sorted order");
    // A newer kit can do more than this build knows about: those capabilities stay unused.
    for (const auto& name : declared)
      (std::find(config_.known_capabilities.begin(), config_.known_capabilities.end(), name) ==
               config_.known_capabilities.end()
           ? result_.ignored_capabilities
           : result_.capabilities).push_back(name);
    view_.root = root_; view_.kit_id = kit_id->text();
    return std::nullopt;
  }

  std::optional<KitManifestResult> ValidateSources() {
    // What the kit was built from, reported for display.
    const auto* sources = Field(*manifest_, "sources", Json::Type::kObject);
    if (!sources) return Failure(KitManifestStatus::kMalformed, "source provenance is missing");
    for (const auto& source : sources->object()) {
      if (!source.second.is_object()) return Failure(KitManifestStatus::kMalformed, "source provenance is malformed: " + source.first);
      auto& reported = result_.sources[source.first];
      for (const auto& field : source.second.object())
        if (field.second.is_string()) reported[field.first] = field.second.text();
    }
    return std::nullopt;
  }

  std::optional<KitManifestResult> ValidateFiles() {
    const auto* files = Field(*manifest_, "files", Json::Type::kArray);
    const auto* aggregate = Field(*manifest_, "files_sha256", Json::Type::kString);
    if (!files || files->array().empty() || files->array().size() > kMaxFiles || !aggregate || !Digest(aggregate->text()))
      return Failure(KitManifestStatus::kMalformed, "file inventory is malformed or out of bounds");
    std::set<std::string> declared;
    std::string canonical;
    std::string previous;
    for (const auto& entry : files->array()) {
      if (!HasOnlyKeys(entry, {"path", "size", "sha256", "mode"}))
        return Failure(KitManifestStatus::kMalformed, "file entry has unknown or missing fields");
      const auto* path = Field(entry, "path", Json::Type::kString);
      const auto* size = Field(entry, "size", Json::Type::kNumber);
      const auto* sha = Field(entry, "sha256", Json::Type::kString);
      const auto* mode = Field(entry, "mode", Json::Type::kString);
      const auto parsed_size = size ? size->AsUint64() : std::nullopt;
      if (!path || !SafeRelative(path->text()) || path->text() == "manifest.json" || !parsed_size || !sha || !Digest(sha->text()) ||
          !mode || mode->text().size() != 4 || !std::all_of(mode->text().begin(), mode->text().end(), [](char c) { return c >= '0' && c <= '7'; }) ||
          (!previous.empty() && path->text() <= previous) || !declared.insert(path->text()).second)
        return Failure(KitManifestStatus::kMalformed, "file inventory entry is not canonical");
      previous = path->text();
      if (!RegularNoSymlink(root_, path->text()))
        return Failure(KitManifestStatus::kIntegrityFailure, "declared path is missing, non-regular, or traverses a symlink: " + path->text());
      // The manifest's hashes name the kit's version; its files are not hashed again here.
      // A size or mode that differs means the kit changed without a new manifest.
      std::error_code error;
      const fs::path actual = root_ / path->text();
      const auto actual_size = fs::file_size(actual, error);
      const auto permissions = static_cast<unsigned>(fs::status(actual, error).permissions()) & 07777;
      unsigned expected_mode = 0;
      for (char c : mode->text()) expected_mode = expected_mode * 8 + static_cast<unsigned>(c - '0');
      if (error || actual_size != *parsed_size || permissions != expected_mode)
        return Failure(KitManifestStatus::kIntegrityFailure,
                       "file size or mode differs from the manifest (regenerate it after changing the kit): " + path->text());
      declared_sha_[path->text()] = sha->text();
      canonical += path->text() + '\t' + std::to_string(*parsed_size) + '\t' + sha->text() + '\t' + mode->text() + '\n';
      view_.files.push_back({path->text(), *parsed_size, sha->text(), expected_mode});
    }
    std::set<std::string> actual;
    std::error_code walk_error;
    for (fs::recursive_directory_iterator it(root_, fs::directory_options::none, walk_error), end; it != end && !walk_error; it.increment(walk_error)) {
      const auto status = it->symlink_status(walk_error);
      if (walk_error) break;
      if (fs::is_symlink(status) || (!fs::is_directory(status) && !fs::is_regular_file(status)))
        return Failure(KitManifestStatus::kIntegrityFailure, "kit contains a symlink or non-regular payload entry");
      if (fs::is_regular_file(status) && it->path() != root_ / "manifest.json")
        actual.insert(fs::relative(it->path(), root_).generic_string());
    }
    if (walk_error || actual != declared)
      return Failure(KitManifestStatus::kIntegrityFailure, "kit file set differs from the exact manifest inventory");
    const auto computed = config_.hash_bytes(canonical);
    if (!computed || *computed != aggregate->text())
      return Failure(KitManifestStatus::kIntegrityFailure, "aggregate file digest does not match canonical inventory");
    view_.files_sha256 = aggregate->text();
    return std::nullopt;
  }

  std::optional<KitManifestResult> ValidatePayload() {
    const auto* runtime = Field(*manifest_, "runtime", Json::Type::kObject);
    const auto* bottle = Field(*manifest_, "template", Json::Type::kObject);
    const auto* recipe = Field(*manifest_, "recipe", Json::Type::kObject);
    const auto* variants = Field(*manifest_, "variants", Json::Type::kArray);
    if (!runtime || !bottle || !recipe || !variants || variants->array().empty() || variants->array().size() > kMaxVariants)
      return Failure(KitManifestStatus::kMalformed, "runtime, template, recipe, or variants are malformed");
    auto failure = ValidateRuntime(*runtime); if (failure) return failure;
    failure = ValidateRecipeAndTemplate(*recipe, *bottle); if (failure) return failure;
    failure = ValidateVariants(*variants); if (failure) return failure;
    view_.artifacts = view_artifacts_;
    return std::nullopt;
  }

  std::optional<KitManifestResult> ValidateRuntime(const Json& runtime) {
    const auto* bundle = Field(runtime, "bundle_id", Json::Type::kString);
    const auto* revision = Field(runtime, "artifact_revision", Json::Type::kString);
    const auto* wine_revision = Field(runtime, "wine_revision", Json::Type::kString);
    const auto* path = Field(runtime, "path", Json::Type::kString);
    const auto* inventory_path = Field(runtime, "inventory_manifest", Json::Type::kString);
    if (!bundle || !revision || !Identifier(bundle->text()) || !Identifier(revision->text()) || !path || !inventory_path ||
        path->text() != "runtime/" + bundle->text() + "/" + revision->text() ||
        inventory_path->text() != path->text() + "/runtime-inventory.json")
      return Failure(KitManifestStatus::kMalformed, "runtime identity paths are not canonical");
    if (!wine_revision || wine_revision->text().empty())
      return Failure(KitManifestStatus::kMalformed, "runtime Wine source is not named");
    const auto inventory = ParseFile(root_ / inventory_path->text(), 8 * 1024 * 1024, kMaxFiles * 2);
    if (!inventory || !inventory->is_object() || inventory->object().empty())
      return Failure(KitManifestStatus::kMalformed, "runtime inventory is malformed");
    std::set<std::string> declared;
    for (const auto& entry : inventory->object()) {
      if (!SafeRelative(entry.first) || !entry.second.is_string() || !Digest(entry.second.text()) || !declared.insert(entry.first).second)
        return Failure(KitManifestStatus::kMalformed, "runtime inventory entry is malformed");
      const auto* hash = Declared(path->text() + "/" + entry.first);
      if (!RegularNoSymlink(root_ / path->text(), entry.first) || !hash || *hash != entry.second.text())
        return Failure(KitManifestStatus::kIntegrityFailure, "runtime inventory hash or path differs: " + entry.first);
    }
    std::set<std::string> actual;
    std::error_code error;
    for (fs::recursive_directory_iterator it(root_ / path->text(), fs::directory_options::none, error), end; it != end && !error; it.increment(error)) {
      const auto status = it->symlink_status(error); if (error) break;
      if (fs::is_symlink(status) || (!fs::is_directory(status) && !fs::is_regular_file(status)))
        return Failure(KitManifestStatus::kIntegrityFailure, "runtime contains a symlink or non-regular entry");
      if (fs::is_regular_file(status) && it->path().filename() != "runtime-inventory.json")
        actual.insert(fs::relative(it->path(), root_ / path->text()).generic_string());
    }
    if (error || actual != declared)
      return Failure(KitManifestStatus::kIntegrityFailure, "runtime inventory is incomplete or has extra files");
    const fs::path libraries = root_ / path->text() / "lib/wine";
    for (const char* name : kCoreDlls)
      if (!PeArchitecture(libraries / "i386-windows" / name, false) ||
          !PeArchitecture(libraries / "x86_64-windows" / name, true))
        return Failure(KitManifestStatus::kIntegrityFailure, "runtime required DLL has the wrong architecture");
    if (!PeArchitecture(libraries / "x86_64-windows/wowprospero.dll", true))
      return Failure(KitManifestStatus::kIntegrityFailure, "runtime wowprospero.dll is not PE64");
    for (const char* name : kNativePrxs)
      if (!RegularNoSymlink(root_ / path->text(), "lib/wine/x86_64-unix/" + std::string(name)))
        return Failure(KitManifestStatus::kIntegrityFailure, "runtime is missing a required native PRX");
    view_.runtime = {bundle->text(), revision->text(), path->text()};
    const auto* digest = Declared(inventory_path->text());
    if (!digest) return Failure(KitManifestStatus::kIntegrityFailure, "runtime identity cannot be hashed");
    view_artifacts_.push_back({"runtime", bundle->text() + "-" + revision->text(), *digest,
                               ImmutableArtifactLocation::kStagedTitle, "win/wine/runtime-inventory.json"});
    return std::nullopt;
  }

  std::optional<KitManifestResult> ValidateRecipeAndTemplate(const Json& recipe, const Json& bottle) {
    const auto* id = Field(recipe, "id", Json::Type::kString);
    const auto* version = Field(recipe, "version", Json::Type::kString);
    const auto* recipe_path = Field(recipe, "path", Json::Type::kString);
    const auto* sha = Field(recipe, "sha256", Json::Type::kString);
    if (!id || !Identifier(id->text()) || !version || version->text().empty() || version->text().size() > 64 || !recipe_path ||
        recipe_path->text() != "recipes/" + id->text() + ".json" || !sha || !Digest(sha->text()))
      return Failure(KitManifestStatus::kMalformed, "recipe identity is malformed");
    const auto* actual = Declared(recipe_path->text());
    const auto document = ParseFile(root_ / recipe_path->text());
    if (!actual || *actual != sha->text() || !document || !document->is_object())
      return Failure(KitManifestStatus::kIntegrityFailure, "recipe bytes do not match identity");
    bool found_identity = false;
    for (const auto& keys : {std::pair{"recipe_id", "recipe_version"}, std::pair{"id", "version"}}) {
      if (!document->Find(keys.first) && !document->Find(keys.second)) continue;
      const auto* recipe_id = Field(*document, keys.first, Json::Type::kString);
      const auto* recipe_version = document->Find(keys.second);
      const bool version_match = recipe_version &&
          (recipe_version->is_string() || (recipe_version->is_number() &&
           recipe_version->text().find_first_not_of("-0123456789") == std::string::npos)) &&
          recipe_version->text() == version->text();
      if (!recipe_id || recipe_id->text() != id->text() || !version_match)
        return Failure(KitManifestStatus::kIntegrityFailure, "recipe document does not match declared identity");
      found_identity = true;
    }
    if (!found_identity)
      return Failure(KitManifestStatus::kIntegrityFailure, "recipe document has no identity");
    bool audio = false;
    if (const auto* requirements = Field(*document, "requirements", Json::Type::kObject)) {
      const auto* selected = Field(*requirements, "audio", Json::Type::kString);
      audio = selected && selected->text() == "ps5";
    }
    if (const auto* prospero = Field(*document, "prospero", Json::Type::kObject)) {
      const auto* selected = Field(*prospero, "audio", Json::Type::kString);
      audio = audio || (selected && selected->text() == "ps5");
    }
    if (const auto* selected = Field(*document, "audio", Json::Type::kString))
      audio = audio || selected->text() == "ps5";
    const auto* template_path = Field(bottle, "path", Json::Type::kString);
    const auto* user = Field(bottle, "user", Json::Type::kString);
    if (!template_path || template_path->text() != "template/bottle" || !user || user->text() != "prospero")
      return Failure(KitManifestStatus::kMalformed, "template identity is not canonical");
    const fs::path template_root = root_ / template_path->text();
    const auto links = Read(template_root / "dosdevices/.pw-symlinks", 4096);
    if (!links || links->find("c:\t../drive_c\n") == std::string::npos || links->find("z:\t/\n") == std::string::npos)
      return Failure(KitManifestStatus::kIntegrityFailure, "template console drive table lacks exact c: and z: mappings");
    for (const char* name : kCoreDlls)
      if (!PeArchitecture(template_root / "drive_c/windows/system32" / name, true) ||
          !PeArchitecture(template_root / "drive_c/windows/syswow64" / name, false))
        return Failure(KitManifestStatus::kIntegrityFailure, "template core DLL has the wrong architecture");
    if (!PeArchitecture(template_root / "drive_c/windows/system32/wowprospero.dll", true) ||
        !ValidateRegistry(template_root, audio))
      return Failure(KitManifestStatus::kIntegrityFailure, "template CPU, crypto, audio, or COM registry contract is incomplete");
    const auto* dxvk = Field(*manifest_, "dxvk", Json::Type::kObject);
    const auto* dxvk_files = dxvk ? Field(*dxvk, "files", Json::Type::kObject) : nullptr;
    if (!dxvk || !dxvk_files) return Failure(KitManifestStatus::kMalformed, "DXVK identity is malformed");
    for (const auto& pair : {std::pair{"x64", "system32"}, std::pair{"x32", "syswow64"}}) {
      const auto* table = Field(*dxvk_files, pair.first, Json::Type::kObject);
      if (!table || table->object().size() != std::size(kDxvkDlls))
        return Failure(KitManifestStatus::kMalformed, "DXVK hash table is incomplete");
      for (const char* name : kDxvkDlls) {
        const auto* expected = Field(*table, name, Json::Type::kString);
        const fs::path module = template_root / "drive_c/windows" / pair.second / name;
        const auto* hash = Declared(template_path->text() + "/drive_c/windows/" + pair.second + "/" + name);
        if (!expected || !Digest(expected->text()) || !hash || *hash != expected->text() ||
            !PeArchitecture(module, pair.first == std::string("x64")))
          return Failure(KitManifestStatus::kIntegrityFailure, "DXVK file hash or architecture differs");
      }
    }
    view_.template_path = template_path->text();
    view_.template_drive_table = template_path->text() + "/dosdevices/.pw-symlinks";
    // Whether the template has PS5 audio even when its own recipe does not ask for it: other
    // recipes Playden ships may (UnmetRecipeRequirements).
    view_.template_audio_ps5 = audio || ValidateRegistry(template_root, true);
    view_.recipe = {id->text(), version->text(), recipe_path->text(), sha->text()};
    view_artifacts_.push_back({"recipe", id->text() + "-" + version->text(), sha->text(),
                               ImmutableArtifactLocation::kKit, recipe_path->text()});
    return std::nullopt;
  }

  std::optional<KitManifestResult> ValidateVariants(const Json& variants) {
    std::set<std::string> ids, titles;
    bool any_title_seen = false;
    const bool any_title_capability = std::find(result_.capabilities.begin(), result_.capabilities.end(),
                                                kKitTitleAnyId) != result_.capabilities.end();
    for (const auto& variant : variants.array()) {
      if (!variant.is_object()) return Failure(KitManifestStatus::kMalformed, "variant is not an object");
      const auto* id = Field(variant, "id", Json::Type::kString);
      const auto* title = Field(variant, "title_id", Json::Type::kString);
      const auto* path = Field(variant, "path", Json::Type::kString);
      const auto* wrapper_revision = Field(variant, "wrapper_revision", Json::Type::kString);
      const auto* helper_revision = Field(variant, "helper_revision", Json::Type::kString);
      if (!id || !Identifier(id->text()) || !title || !TitleId(title->text()) || !path || path->text() != "titles/" + id->text() ||
          !ids.insert(id->text()).second || !titles.insert(title->text()).second ||
          !wrapper_revision || wrapper_revision->text().empty() || !helper_revision || helper_revision->text().empty())
        return Failure(KitManifestStatus::kMalformed, "variant identity or provenance is malformed");
      const std::string helper_path = path->text() + "/lapy.elf";
      const auto* manifest_path = Field(variant, "helper_manifest", Json::Type::kString);
      const auto* protocol_path = Field(variant, "helper_protocol", Json::Type::kString);
      const auto* build_path = Field(variant, "native_build_manifest", Json::Type::kString);
      if (!manifest_path || manifest_path->text() != path->text() + "/lapy-manifest.json" ||
          !protocol_path || protocol_path->text() != path->text() + "/lapy_elevation_protocol.h" ||
          !build_path || build_path->text() != path->text() + "/native-title-build.json")
        return Failure(KitManifestStatus::kMalformed, "variant metadata paths are not canonical");
      const auto* helper_hash = Declared(helper_path);
      const auto* protocol_hash = Declared(protocol_path->text());
      if (!helper_hash || !protocol_hash)
        return Failure(KitManifestStatus::kIntegrityFailure, "helper or protocol is not in the manifest");
      const auto helper_doc = ParseFile(root_ / manifest_path->text());
      const auto parameter = ParseFile(root_ / path->text() / "sce_sys/param.json");
      const auto build = ParseFile(root_ / build_path->text());
      if (!helper_doc || !parameter || !build || !helper_doc->is_object() || !parameter->is_object() || !build->is_object())
        return Failure(KitManifestStatus::kMalformed, "variant metadata JSON is malformed");
      const auto* mode = Field(*helper_doc, "mode", Json::Type::kString);
      const auto* target = Field(*helper_doc, "target_title", Json::Type::kString);
      const auto* reported_helper = Field(*helper_doc, "elf_sha256", Json::Type::kString);
      const auto* reported_protocol = Field(*helper_doc, "protocol_sha256", Json::Type::kString);
      const auto* features = Field(*helper_doc, "features", Json::Type::kArray);
      bool retry = false, rule_feature = false;
      if (features)
        for (const auto& feature : features->array()) {
          retry |= feature.is_string() && feature.text() == "root_layout_probe_retry";
          rule_feature |= feature.is_string() && feature.text() == "title_rule";
        }
      // One shell for every game: its helper serves FAKE78xxx titles listed in Playden's
      // root-owned allow-list, and the shell says it was built for that rule.
      const auto* any_title = variant.Find("any_title");
      if (any_title && any_title->type() != Json::Type::kBool)
        return Failure(KitManifestStatus::kMalformed, "variant any_title is not a boolean");
      const bool is_any = any_title && any_title->boolean();
      std::string expected_target = title->text();
      if (is_any) {
        const auto* rule = Field(*helper_doc, "title_rule", Json::Type::kObject);
        const auto* prefix = rule ? Field(*rule, "prefix", Json::Type::kString) : nullptr;
        const auto* allow_list = rule ? Field(*rule, "allow_list", Json::Type::kString) : nullptr;
        const auto* build_any = Field(*build, "any_title", Json::Type::kBool);
        const auto* build_rule = Field(*build, "title_rule", Json::Type::kObject);
        const auto* build_prefix = build_rule ? Field(*build_rule, "prefix", Json::Type::kString) : nullptr;
        const auto* build_list = build_rule ? Field(*build_rule, "allow_list", Json::Type::kString) : nullptr;
        if (any_title_seen || !any_title_capability || !rule_feature || !prefix ||
            prefix->text() != kTitleRulePrefix || !allow_list || allow_list->text() != kTitleRuleAllowList ||
            !build_any || !build_any->boolean() || !build_prefix || build_prefix->text() != prefix->text() ||
            !build_list || build_list->text() != allow_list->text())
          return Failure(KitManifestStatus::kIntegrityFailure,
                         "the any-title variant lacks its capability or Playden's title rule");
        any_title_seen = true;
        expected_target = prefix->text() + "*";
      } else if (build->Find("any_title") != nullptr) {
        return Failure(KitManifestStatus::kIntegrityFailure, "an any-title shell is listed as a fixed title");
      }
      const auto* parameter_title = Field(*parameter, "titleId", Json::Type::kString);
      if (!mode || mode->text() != "elf-helper" || !target || target->text() != expected_target ||
          !reported_helper || reported_helper->text() != *helper_hash || !reported_protocol || reported_protocol->text() != *protocol_hash ||
          !retry || !parameter_title || parameter_title->text() != title->text())
        return Failure(KitManifestStatus::kIntegrityFailure, "helper manifest or title metadata is inconsistent");
      const auto* build_schema = Field(*build, "schema", Json::Type::kString);
      const auto* build_title = Field(*build, "title_id", Json::Type::kString);
      const auto* build_revision = Field(*build, "source_revision", Json::Type::kString);
      const auto* dirty = Field(*build, "source_dirty", Json::Type::kBool);
      const auto* eboot = Field(*build, "eboot_sha256", Json::Type::kString);
      const auto* eboot_hash = Declared(path->text() + "/eboot.bin");
      if (!build_schema || build_schema->text() != "prospero-win-native-title/1" || !build_title || build_title->text() != title->text() ||
          !build_revision || build_revision->text() != wrapper_revision->text() || !dirty ||
          !eboot || !eboot_hash || eboot->text() != *eboot_hash)
        return Failure(KitManifestStatus::kIntegrityFailure, "native build manifest lacks matching source/artifact provenance");
      result_.source_dirty |= dirty->boolean();
      if (std::find(result_.capabilities.begin(), result_.capabilities.end(),
                    kPublicationGuard) != result_.capabilities.end()) {
        const auto* guard = Field(*build, "managed_publication_guard", Json::Type::kString);
        const auto* descriptor = Field(*build, "managed_descriptor_required", Json::Type::kBool);
        const auto* provider = Field(*build, "publication_provider_sha256", Json::Type::kString);
        if (!guard || guard->text() != kPublicationGuard || !descriptor || !descriptor->boolean() ||
            !provider || !config_.publication_provider_sha256 ||
            provider->text() != *config_.publication_provider_sha256)
          return Failure(KitManifestStatus::kIntegrityFailure,
                         "native title does not match the trusted publication guard/provider contract");
      }
      result_.available_variant_ids.push_back(id->text());
      view_.variants.push_back({id->text(), title->text(), path->text(), is_any});
    }
    if (any_title_capability && !any_title_seen)
      return Failure(KitManifestStatus::kIntegrityFailure, "the title-any-id capability has no any-title variant");
    return std::nullopt;
  }

  // A kit file's hash as the manifest declares it.
  const std::string* Declared(const std::string& path) const {
    const auto found = declared_sha_.find(path);
    return found == declared_sha_.end() ? nullptr : &found->second;
  }

  fs::path root_;
  const KitAdmissionConfig& config_;
  std::map<std::string, std::string> declared_sha_;
  const Json* manifest_ = nullptr;
  KitManifestResult result_;
  VerifiedKitView view_;
  std::vector<KitArtifact> view_artifacts_;
};

}  // namespace

KitManifestResult VerifyKitManifest(const std::filesystem::path& root,
                                    const KitAdmissionConfig& config) {
  return Verifier(root, config).Run();
}

}  // namespace playden::runtime
