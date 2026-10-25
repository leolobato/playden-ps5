#pragma once
#include "platform/ps5/title_database.h"
#include "platform/ps5/title_registry.h"
#include "runtime/install_service.h"
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>

namespace playden::ps5 {
struct TitleSourceObservation {
  runtime::TitleAvailability availability = runtime::TitleAvailability::kUnknown;
  bool directory = false;
  // Owned by this user and not writable by others (0700 or 0755).
  bool owned_directory = false;
};
// Read-only point observation in a controlled, stable /data/homebrew namespace.
// Missing/inaccessible parents and rejected symlinks never imply a free slot.
TitleSourceObservation ReadTitleSource(const std::filesystem::path& homebrew_root, const std::string& title_id);
struct NativeCoordinatorOperations {
  std::function<TitleDatabaseObservation(const std::string&)> database;
  std::function<runtime::NativeActivity(const std::string&)> activity;
  // Receives why the default activity reader could not tell whether a title runs.
  std::function<void(const std::string&)> log;
};
class Ps5NativeCoordinator final : public runtime::NativeCoordinator {
 public:
  Ps5NativeCoordinator(runtime::InstallRecordStore& records, runtime::PackageInstaller& package,
      TitleRegistry& registry, std::filesystem::path homebrew_root = "/data/homebrew",
      NativeCoordinatorOperations operations = {});
  runtime::NativeTitleAvailability Availability(const std::string& title_id);
  // What each source says about a title. Availability() needs every source to say free.
  struct AvailabilityReport {
    runtime::TitleAvailability database = runtime::TitleAvailability::kUnknown;
    runtime::TitleAvailability folder = runtime::TitleAvailability::kUnknown;
    TitleRegistration shadowmount;
    runtime::NativeActivity activity = runtime::NativeActivity::kUnknown;
    // For logs, e.g. "app.db=free folder=free shadowmount=unavailable(no-response) activity=stopped".
    std::string Text() const;
  };
  AvailabilityReport ReportAvailability(const std::string& title_id);
  // The console's own inventory (app.db) only: ShadowMount's cache is left out
  // because it does not refresh while any title, Playden included, runs.
  runtime::TitleAvailability Inventory(const std::string& title_id);
  runtime::NativeActivity Activity(const std::string& title_id) override;
  runtime::NativeStatus Query(const runtime::RuntimeInstall& record) override;
  runtime::NativeStatus QueryOwnership(const runtime::RuntimeInstall& record) override;
  // Caller owns the complete publication lease/receipt gate and a durably armed
  // OWNER submission. This adapter never rearms or mutates OWNER on uncertain retry.
  runtime::RegistrationResult Register(const runtime::RuntimeInstall& record) override;
 private:
  runtime::NativeStatus QueryWith(const runtime::RuntimeInstall& record, bool full_package);
  runtime::InstallRecordStore& records_;
  runtime::PackageInstaller& package_;
  TitleRegistry& registry_;
  std::filesystem::path root_;
  NativeCoordinatorOperations operations_;
};
// The default activity reader: an unknown result is logged when its reason changes,
// and once more when the title's activity is known again.
std::function<runtime::NativeActivity(const std::string&)> LoggedTitleActivity(
    std::function<void(const std::string&)> log);
}  // namespace playden::ps5
