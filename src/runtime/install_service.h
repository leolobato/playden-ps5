#pragma once

#include "runtime/game_install.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace playden::runtime {

inline constexpr const char* kAdmissionDriveMissing = "drive-missing";
inline constexpr const char* kAdmissionDriveAmbiguous = "drive-ambiguous";
// An admission that a Repair could change: not "ok", and not only about the game's USB drive.
inline bool AdmissionNeedsRepair(const std::string& admission) {
  return admission != "ok" && admission != kAdmissionDriveMissing && admission != kAdmissionDriveAmbiguous;
}


struct RecordUpdateResult {
  bool ok = false;
  std::string code;
  std::string message;
};

class InstallRecordStore {
public:
  virtual ~InstallRecordStore() = default;
  virtual std::optional<RuntimeInstall>
  Find(const std::string &install_id) const = 0;
  virtual std::vector<RuntimeInstall> Snapshot() const = 0;
  virtual RecordUpdateResult Update(const RuntimeInstall &record) = 0;
};

class RepositoryInstallRecordStore final : public InstallRecordStore {
public:
  explicit RepositoryInstallRecordStore(RuntimeInstallRepository &repository);
  std::optional<RuntimeInstall>
  Find(const std::string &install_id) const override;
  std::vector<RuntimeInstall> Snapshot() const override;
  RecordUpdateResult Update(const RuntimeInstall &record) override;

private:
  RuntimeInstallRepository &repository_;
};

struct PackageStatus {
  bool known = false;
  bool title_staged = false;
  bool manifest_matches = false;
  bool template_owned = false;
  bool mutable_bottle_present = false;
  bool mutable_bottle_owned = false;
  bool artifacts_valid = false;
  bool profile_published = false;
  std::uint64_t profile_settings_revision = 0;
  // Physical staged source inspected by the immutable package adapter. Native
  // coordination must bind these ownership facts to its same mapped namespace.
  std::string title_source_path;
};

enum class PackageActionState { kComplete, kCanceled, kFailed };
struct PackageActionResult {
  PackageActionState state = PackageActionState::kFailed;
  std::string code;
  std::string message;
};

class PackageInstaller {
public:
  virtual ~PackageInstaller() = default;
  virtual PackageStatus Inspect(const RuntimeInstall &record) = 0;
  // Whose title and bottle these are: their owner markers name this install. No walk
  // of the title's files and no kit comparison (a kit change shows as TitleCurrent
  // false, i.e. needs Update). For settings applies; staging, repair and launch keep
  // the full inspection.
  virtual PackageStatus InspectOwnership(const RuntimeInstall &record) { return Inspect(record); }
  virtual PackageActionResult StageTitle(const RuntimeInstall &record) = 0;
  virtual PackageActionResult CopyTemplate(const RuntimeInstall &record) = 0;
  virtual PackageActionResult
  ValidateArtifacts(const RuntimeInstall &record) = 0;
  virtual PackageActionResult PublishProfile(const RuntimeInstall &record) = 0;
  // Runs the wrapper's publication admission over the installed title and
  // returns "ok" or its refusal text (diagnostics only).
  // Besides "ok" and a reason, AdmissionDiagnostic can answer that the title is fine and only its USB drive is the matter.
  virtual std::string AdmissionDiagnostic(const RuntimeInstall &) { return "unsupported"; }
  // A cheap form for start-up: only what a game run changes and RepairBottle fixes
  // (the bottle registry file's mode). "ok" or the refusal text.
  virtual std::string QuickAdmission(const RuntimeInstall &record) { return AdmissionDiagnostic(record); }
  // Restores modes of app-owned bottle registry files that a game run left
  // wider than publication admission accepts (diagnostics-driven repair).
  virtual PackageActionResult RepairBottle(const RuntimeInstall &) {
    return {PackageActionState::kFailed, "repair-unsupported", "this package layer cannot repair bottles"};
  }
  // Makes the missing ones of `folders` in the bottle's Windows user folder (drive_c/users/prospero): each a path
  // relative to it that IsProfileSaveFolder admits (others fail). A folder already there under another case is the
  // one used, as Wine finds it. Existing folders keep their mode; new ones get the profile's (0777 with
  // bottle-profile-shared, else 0700). A bottle with no user folder yet is left alone. Folders made go in `added`,
  // ones that could not be made in `failed`; nothing else changes. The title must be stopped.
  virtual void AddProfileFolders(const RuntimeInstall &, const std::vector<std::string> &,
                                 std::vector<std::string> *, std::vector<std::string> *) {}
  // Sets the bottle's way to its user profile (data root, library, library/prefixes, the bottle,
  // drive_c, drive_c/users) to the kit's mode: 0711 when the kit has bottle-profile-shared, which
  // then also opens the profile (drive_c/users/prospero) to FTP, else 0700. The title must be
  // stopped, and no directory of the bottle held open across it (a mode change reads as ESTALE).
  virtual PackageActionResult SettleBottleModes(const RuntimeInstall &) {
    return {PackageActionState::kComplete, {}, {}};
  }
  // Whether a staged title was made from the admitted kit (cheap marker check).
  virtual bool TitleCurrent(const RuntimeInstall &) { return true; }
  // Whether this install's title, or its bottle, is there and was made from another kit (or
  // another build of this one): owner markers that name the install but not the admitted kit.
  // A setup stopped before Ready can't finish over it (cheap marker check).
  virtual bool SetupFromOtherKit(const RuntimeInstall &) { return false; }
  // `icon_source` names where the icon came from (empty: the store's cover or the program's own icon);
  // TitleIconSource gives it back, so a game whose source changed gets its icon again.
  virtual PackageActionResult WriteTitleMetadata(const RuntimeInstall &, const std::string &,
                                                 const std::optional<std::string> &,
                                                 const std::string & /*icon_source*/ = {}) {
    return {PackageActionState::kFailed, "unsupported", "title metadata is unsupported"};
  }
  virtual bool TitleMetadataCurrent(const RuntimeInstall &) { return true; }
  // The icon source the title metadata was written with (empty before sources were recorded); nothing
  // without readable metadata.
  virtual std::optional<std::string> TitleIconSource(const RuntimeInstall &) { return std::string(); }
  // Updates an installed title's immutable files to the admitted kit when only
  // the kit's file inventory changed (same kit id, variant, runtime, recipe).
  virtual PackageActionResult RefreshTitle(const RuntimeInstall &) {
    return {PackageActionState::kFailed, "refresh-unsupported", "this package layer cannot refresh titles"};
  }
};

enum class NativeActivity { kStopped, kActive, kUnknown };
struct NativeStatus {
  bool available = false;
  bool installed = false;
  bool source_available = false;
  bool owned_manifest_matches = false;
  bool collision = false;
  bool scan_pending = false;
};
enum class RegistrationResultState {
  kAccepted,
  kScanQueued,
  kStaleHook,
  kFailed
};
struct RegistrationResult {
  RegistrationResultState state = RegistrationResultState::kFailed;
  std::string code;
  std::string message;
};

class NativeCoordinator {
public:
  virtual ~NativeCoordinator() = default;
  virtual NativeActivity Activity(const std::string &title_id) = 0;
  virtual NativeStatus Query(const RuntimeInstall &record) = 0;
  // Query with the package's ownership-only inspection (InspectOwnership).
  virtual NativeStatus QueryOwnership(const RuntimeInstall &record) { return Query(record); }
  virtual RegistrationResult Register(const RuntimeInstall &record) = 0;
};

struct CompatibilityResult {
  bool compatible = false;
  std::string reason;
};
using CompatibilityQuery =
    std::function<CompatibilityResult(const RuntimeInstall &)>;
using InstallCancellationQuery = std::function<bool()>;

enum class InstallServiceError {
  kNone,
  kNotFound,
  kInvalidState,
  kIncompatible,
  kPersistence,
  kCanceled,
  kActivityActive,
  kActivityUnknown,
  kPackageUnavailable,
  kPackageFailed,
  kOwnershipConflict,
  kNativeUnavailable,
  kNativeCollision,
  kNativeIncoherent,
  kRegistrationPending
};

struct InstallServiceResult {
  bool success = false;
  InstallServiceError error_code = InstallServiceError::kNone;
  std::string message;
  static InstallServiceResult Success();
  static InstallServiceResult Failure(InstallServiceError error,
                                      std::string message);
  bool ok() const { return success; }
  InstallServiceError error() const { return error_code; }
};

// A complete publication driver owns durable control/publication/registration
// journals and readiness. The service must never overwrite its latest OWNER
// snapshot with the pre-callback preparation record. Captured collaborators must
// outlive the service. Empty retains legacy adapter compatibility; managed
// journals always require an explicit complete driver.
using InstallPublicationDriver = std::function<PackageActionResult(const RuntimeInstall&)>;

class RuntimeInstallService {
public:
  RuntimeInstallService(InstallRecordStore &records, PackageInstaller &package,
                        NativeCoordinator &native,
                        CompatibilityQuery compatibility,
                        InstallCancellationQuery canceled,
                        InstallPublicationDriver publication = {});
  InstallServiceResult Prepare(const std::string &install_id);
  InstallServiceResult Retry(const std::string &install_id);
  InstallServiceResult Step(const std::string &install_id);
  std::vector<RuntimeInstall> Snapshot() const;

private:
  InstallServiceResult Persist(RuntimeInstall record);
  InstallServiceResult Fail(RuntimeInstall record, InstallServiceError error,
                            std::string code, std::string message);
  InstallServiceResult Advance(RuntimeInstall record, PreparationState state,
                               const std::string &step);
  InstallServiceResult HandlePackageAction(RuntimeInstall record,
                                           const std::string &next_step,
                                           PackageActionResult result);
  InstallServiceResult StepPublication(const RuntimeInstall& record);
  InstallServiceResult StepNative(RuntimeInstall record,
                                  const PackageStatus &package_status);

  InstallRecordStore &records_;
  PackageInstaller &package_;
  NativeCoordinator &native_;
  CompatibilityQuery compatibility_;
  InstallCancellationQuery canceled_;
  InstallPublicationDriver publication_;
};

} // namespace playden::runtime
