#include "platform/ps5/native_coordinator.h"
#include "platform/ps5/title_activity.h"
#include "platform/managed_directory.h"
#include "steam/backend/json.h"
#include "runtime/publication_transaction.h"
#include <cerrno>
#include <unistd.h>
namespace playden::ps5 {
namespace {
using runtime::TitleAvailability;
bool RootValid(const std::filesystem::path& root) {
  return root.is_absolute() && root == root.lexically_normal() && root.filename() == "homebrew" && root.parent_path().filename() == "data";
}
// Playden owns a title directory it created: owned by this user and not
// writable by others. ShadowMount must read it, so 0755 as well as 0700.
bool OwnedDirectoryMode(const struct stat& info) {
  return S_ISDIR(info.st_mode) && info.st_uid == geteuid() &&
      ((info.st_mode & 07777) == 0700 || (info.st_mode & 07777) == 0755);
}
std::optional<std::string> ContentId(const std::filesystem::path& root, const std::string& title) {
  auto directory = platform::ManagedDirectory::Open(root / title / "sce_sys", platform::DirectoryPolicy::kApprovedTree);
  struct stat sce_sys{};
  if (!directory || lstat((root / title / "sce_sys").c_str(), &sce_sys) || !OwnedDirectoryMode(sce_sys)) return {};
  const int fd = directory->OpenRegularFile("param.json", true);
  if (fd < 0) return {};
  struct stat before{}, after{}; std::string bytes; char buffer[4096]; bool ok = true;
  if (fstat(fd, &before) || before.st_nlink != 1 || before.st_size <= 0 || before.st_size > 65536) ok = false;
  while (ok) {
    const auto count = read(fd, buffer, sizeof(buffer));
    if (count < 0 && errno == EINTR) continue;
    if (count < 0 || (count > 0 && bytes.size() + static_cast<std::size_t>(count) > 65536)) { ok = false; break; }
    if (!count) break;
    bytes.append(buffer, count);
  }
  if (fstat(fd, &after) || before.st_dev != after.st_dev || before.st_ino != after.st_ino ||
      before.st_size != after.st_size || before.st_mtime != after.st_mtime || after.st_nlink != 1) ok = false;
  if (close(fd) || !directory->Validate() || !ok) return {};
  const auto json = steam::json::Parse(bytes, {65536, 16, 4096, 65536});
  const auto* id = json ? json->Find("titleId") : nullptr;
  const auto* content = json ? json->Find("contentId") : nullptr;
  if (!id || !id->is_string() || id->text() != title || !content || !content->is_string() ||
      content->text().empty() || content->text().size() > 128 || content->text().find('\0') != std::string::npos) return {};
  return content->text();
}
}
TitleSourceObservation ReadTitleSource(const std::filesystem::path& root, const std::string& title) {
  if (!RootValid(root) || !ValidTitleId(title)) return {};
  auto parent = platform::ManagedDirectory::Open(root, platform::DirectoryPolicy::kApprovedTree);
  if (!parent) return {};
  struct stat info{}; errno = 0;
  if (!parent->InspectChild(title, &info)) {
    const auto error = errno;
    if (error == ENOENT && parent->Validate()) return {TitleAvailability::kFree, false, false};
    return {};
  }
  return {TitleAvailability::kOccupied, S_ISDIR(info.st_mode), OwnedDirectoryMode(info)};
}
Ps5NativeCoordinator::Ps5NativeCoordinator(runtime::InstallRecordStore& records, runtime::PackageInstaller& package,
    TitleRegistry& registry, std::filesystem::path root, NativeCoordinatorOperations operations)
    : records_(records), package_(package), registry_(registry), root_(std::move(root)), operations_(std::move(operations)) {
  if (!operations_.database) operations_.database = ReadNativeTitleDatabase;
  if (!operations_.activity) operations_.activity = LoggedTitleActivity(operations_.log);
}
std::function<runtime::NativeActivity(const std::string&)> LoggedTitleActivity(
    std::function<void(const std::string&)> log) {
  struct Reasons { std::mutex mutex; std::map<std::string, std::string> unknown; };
  auto reasons = std::make_shared<Reasons>();
  return [log = std::move(log), reasons](const std::string& title) {
    std::string reason;
    const auto activity = ReadTitleActivity(title, &reason);
    if (!log) return activity;
    std::string line;
    {
      std::lock_guard<std::mutex> lock(reasons->mutex);
      const auto found = reasons->unknown.find(title);
      if (activity == runtime::NativeActivity::kUnknown) {
        if (found == reasons->unknown.end() || found->second != reason) {
          reasons->unknown[title] = reason;
          line = "title activity unknown title=" + title + " reason=" + reason;
        }
      } else if (found != reasons->unknown.end()) {
        reasons->unknown.erase(found);
        line = std::string("title activity known again title=") + title +
               (activity == runtime::NativeActivity::kActive ? " active" : " stopped");
      }
    }
    if (!line.empty()) log(line);
    return activity;
  };
}
runtime::NativeTitleAvailability Ps5NativeCoordinator::Availability(const std::string& title) {
  if (!IsPlaydenGameTitle(title)) return {};
  auto database = operations_.database(title);
  auto source = ReadTitleSource(root_, title);
  const auto cache = registry_.Query(title);
  if (cache.state == RegistrationState::kUnavailable) source.availability = TitleAvailability::kUnknown;
  else if (cache.state != RegistrationState::kAbsent) source.availability = TitleAvailability::kOccupied;
  const auto activity = Activity(title);
  if (activity == runtime::NativeActivity::kActive) database.inventory = TitleAvailability::kOccupied;
  if (activity == runtime::NativeActivity::kUnknown) database.inventory = TitleAvailability::kUnknown;
  return {database.inventory, source.availability};
}
Ps5NativeCoordinator::AvailabilityReport Ps5NativeCoordinator::ReportAvailability(const std::string& title) {
  AvailabilityReport report;
  if (!IsPlaydenGameTitle(title)) return report;
  report.database = operations_.database(title).inventory;
  report.folder = ReadTitleSource(root_, title).availability;
  report.shadowmount = registry_.Query(title);
  report.activity = Activity(title);
  return report;
}
std::string Ps5NativeCoordinator::AvailabilityReport::Text() const {
  const auto text = [](TitleAvailability value) {
    return value == TitleAvailability::kFree ? "free" : value == TitleAvailability::kOccupied ? "occupied" : "unknown";
  };
  const char* state = "unavailable";
  switch (shadowmount.state) {
    case RegistrationState::kUnavailable: break;
    case RegistrationState::kAbsent: state = "absent"; break;
    case RegistrationState::kPending: state = "pending"; break;
    case RegistrationState::kReady: state = "ready"; break;
    case RegistrationState::kCollision: state = "collision"; break;
  }
  return std::string("app.db=") + text(database) + " folder=" + text(folder) + " shadowmount=" + state +
         (shadowmount.reason.empty() ? "" : "(" + shadowmount.reason + ")") + " activity=" +
         (activity == runtime::NativeActivity::kActive ? "active" : activity == runtime::NativeActivity::kStopped ? "stopped" : "unknown");
}
runtime::TitleAvailability Ps5NativeCoordinator::Inventory(const std::string& title) {
  return IsPlaydenGameTitle(title) ? operations_.database(title).inventory : TitleAvailability::kUnknown;
}
runtime::NativeActivity Ps5NativeCoordinator::Activity(const std::string& title) {
  return IsPlaydenGameTitle(title) ? operations_.activity(title) : runtime::NativeActivity::kUnknown;
}
runtime::NativeStatus Ps5NativeCoordinator::Query(const runtime::RuntimeInstall& record) {
  return QueryWith(record, true);
}
runtime::NativeStatus Ps5NativeCoordinator::QueryOwnership(const runtime::RuntimeInstall& record) {
  return QueryWith(record, false);
}
runtime::NativeStatus Ps5NativeCoordinator::QueryWith(const runtime::RuntimeInstall& record, bool full_package) {
  auto owner = records_.Find(record.install_id);
  if (!IsPlaydenGameTitle(record.title_id) || !owner || !runtime::SameRuntimeInstallIdentity(*owner, record)) return {};
  const auto database = operations_.database(record.title_id);
  const auto source = ReadTitleSource(root_, record.title_id);
  const auto cache = registry_.Query(record.title_id);
  if (database.inventory == TitleAvailability::kUnknown || source.availability == TitleAvailability::kUnknown ||
      cache.state == RegistrationState::kUnavailable) return {};
  runtime::NativeStatus status; status.available = true;
  status.source_available = source.directory;
  if (cache.state == RegistrationState::kCollision) { status.collision = true; return status; }
  if (source.availability == TitleAvailability::kFree) {
    status.collision = database.inventory == TitleAvailability::kOccupied || cache.state != RegistrationState::kAbsent;
    return status;
  }
  const auto package = full_package ? package_.Inspect(record) : package_.InspectOwnership(record);
  if (!package.known) return {};
  const auto content = source.owned_directory ? ContentId(root_, record.title_id) : std::nullopt;
  // The ownership query asks whose title this is; kit artifacts matter only to the full one (launch, registration).
  status.owned_manifest_matches = source.owned_directory && package.title_staged && package.manifest_matches &&
      (!full_package || package.artifacts_valid) &&
      package.title_source_path == (root_ / record.title_id).string() && content.has_value();
  if (!status.owned_manifest_matches) { status.collision = true; return status; }
  if (database.inventory == TitleAvailability::kOccupied) {
    if (!database.identity || database.identity->title_id != record.title_id || database.identity->content_id != *content ||
        database.identity->metadata_path != "/user/app/" + record.title_id + "/sce_sys") {
      status.collision = true; return status;
    }
    status.installed = cache.state == RegistrationState::kReady;
    status.scan_pending = !status.installed;
  } else {
    if (cache.state == RegistrationState::kReady) return {}; // Cache cannot replace native inventory.
    status.scan_pending = cache.state == RegistrationState::kPending;
  }
  return status;
}
runtime::RegistrationResult Ps5NativeCoordinator::Register(const runtime::RuntimeInstall& record) {
  using runtime::RegistrationResultState;
  auto owner = records_.Find(record.install_id);
  if (!owner || owner->state != runtime::PreparationState::kRegistering ||
      !runtime::SameRuntimeInstallIdentity(*owner, record) || owner->settings_revision != record.settings_revision ||
      record.state != owner->state || !record.journal || !owner->journal || owner->journal->step != "register" ||
      record.journal->operation_id != owner->journal->operation_id || record.journal->operation != owner->journal->operation ||
      record.journal->step != owner->journal->step ||
      (owner->journal->operation != "settings-publication" && owner->journal->operation != "prepare") ||
      (owner->journal->operation == "settings-publication" && !runtime::IsSettingsPublicationTransactionId(owner->journal->operation_id)) ||
      Activity(record.title_id) != runtime::NativeActivity::kStopped)
    return {RegistrationResultState::kFailed, "native-unarmed", "Registration requires a stopped title and durable OWNER arm."};
  const auto status = Query(record);
  if (!status.available || status.collision || !status.source_available || !status.owned_manifest_matches)
    return {RegistrationResultState::kFailed, "native-incoherent", "Native inventory/source/OWNER ownership does not agree."};
  if (status.installed) return {RegistrationResultState::kAccepted, {}, {}};
  if (status.scan_pending) return {RegistrationResultState::kScanQueued, {}, {}};
  const auto package = package_.Inspect(record);
  if (!package.known || !package.template_owned || !package.mutable_bottle_present || !package.mutable_bottle_owned)
    return {RegistrationResultState::kFailed, "native-bottle", "Registration requires the reserved owned template bottle."};
  const auto result = registry_.Register(record.title_id);
  // An unanswered submission is followed like a queued one: queries only.
  if (!result.ok && result.stage == "uncertain")
    return runtime::RegistrationResult{RegistrationResultState::kScanQueued, {}, {}};
  return result.ok ? runtime::RegistrationResult{RegistrationResultState::kScanQueued, {}, {}} :
      runtime::RegistrationResult{RegistrationResultState::kFailed, "native-registration", "ShadowMount registration submission failed."};
}
}
