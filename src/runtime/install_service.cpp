#include "runtime/install_service.h"

#include <utility>

namespace playden::runtime {
namespace {

bool ManagedPublication(const RuntimeInstall& record) {
  return record.journal && (record.journal->operation == "settings-request" ||
                            record.journal->operation == "settings-control" ||
                            record.journal->operation == "settings-publication");
}

bool SamePreparationState(const RuntimeInstall& a, const RuntimeInstall& b) {
  return a.state == b.state && a.settings_revision == b.settings_revision &&
      a.journal.has_value() == b.journal.has_value() && a.last_failure.has_value() == b.last_failure.has_value() &&
      (!a.journal || (a.journal->operation_id == b.journal->operation_id && a.journal->operation == b.journal->operation &&
                     a.journal->step == b.journal->step)) &&
      (!a.last_failure || (a.last_failure->code == b.last_failure->code && a.last_failure->message == b.last_failure->message));
}

bool IsNativeStep(const std::string &step) {
  return step == "query-native" || step == "register" || step == "await-native";
}

bool PackageReady(const PackageStatus &status, const RuntimeInstall &record) {
  return status.known && status.title_staged && status.manifest_matches &&
         status.template_owned && status.mutable_bottle_present &&
         status.mutable_bottle_owned && status.artifacts_valid &&
         status.profile_published &&
         status.profile_settings_revision == record.settings_revision;
}

std::string ErrorCode(const std::string &code, const char *fallback) {
  return code.empty() ? fallback : code;
}

std::string ErrorMessage(const std::string &message, const char *fallback) {
  return message.empty() ? fallback : message;
}

} // namespace

RepositoryInstallRecordStore::RepositoryInstallRecordStore(
    RuntimeInstallRepository &repository)
    : repository_(repository) {}

std::optional<RuntimeInstall>
RepositoryInstallRecordStore::Find(const std::string &install_id) const {
  return repository_.Find(install_id);
}

std::vector<RuntimeInstall> RepositoryInstallRecordStore::Snapshot() const {
  return repository_.Snapshot();
}

RecordUpdateResult
RepositoryInstallRecordStore::Update(const RuntimeInstall &record) {
  const auto result = repository_.Update(record);
  return result.ok()
             ? RecordUpdateResult{true, {}, {}}
             : RecordUpdateResult{false, "repository-write", result.message()};
}

InstallServiceResult InstallServiceResult::Success() {
  return {true, InstallServiceError::kNone, {}};
}

InstallServiceResult InstallServiceResult::Failure(InstallServiceError error,
                                                   std::string message) {
  return {false, error, std::move(message)};
}

RuntimeInstallService::RuntimeInstallService(InstallRecordStore &records,
                                             PackageInstaller &package,
                                             NativeCoordinator &native,
                                             CompatibilityQuery compatibility,
                                             InstallCancellationQuery canceled,
                                             InstallPublicationDriver publication)
    : records_(records), package_(package), native_(native),
      compatibility_(std::move(compatibility)), canceled_(std::move(canceled)), publication_(std::move(publication)) {
}

InstallServiceResult RuntimeInstallService::Persist(RuntimeInstall record) {
  const auto result = records_.Update(record);
  if (!result.ok) {
    return InstallServiceResult::Failure(
        InstallServiceError::kPersistence,
        result.message.empty() ? "runtime install state could not be persisted"
                               : result.message);
  }
  return InstallServiceResult::Success();
}

InstallServiceResult RuntimeInstallService::Fail(RuntimeInstall record,
                                                 InstallServiceError error,
                                                 std::string code,
                                                 std::string message) {
  record.state = PreparationState::kFailed;
  record.last_failure = RuntimeFailure{std::move(code), message};
  const auto persisted = Persist(std::move(record));
  return persisted.ok()
             ? InstallServiceResult::Failure(error, std::move(message))
             : persisted;
}

InstallServiceResult RuntimeInstallService::Advance(RuntimeInstall record,
                                                    PreparationState state,
                                                    const std::string &step) {
  record.state = state;
  record.last_failure.reset();
  if (step.empty()) {
    record.journal.reset();
  } else if (record.journal) {
    record.journal->step = step;
  } else {
    record.journal =
        PreparationJournal{"prepare-" + record.install_id, "prepare", step};
  }
  return Persist(std::move(record));
}

InstallServiceResult
RuntimeInstallService::Prepare(const std::string &install_id) {
  auto record = records_.Find(install_id);
  if (!record) {
    return InstallServiceResult::Failure(InstallServiceError::kNotFound,
                                         "runtime install was not found");
  }
  if (record->state != PreparationState::kDownloaded) {
    return InstallServiceResult::Failure(
        InstallServiceError::kInvalidState,
        "only a downloaded runtime install can begin preparation");
  }
  const auto compatibility =
      compatibility_ ? compatibility_(*record) : CompatibilityResult{};
  if (!compatibility.compatible) {
    record->last_failure =
        RuntimeFailure{"recipe-unavailable",
                       compatibility.reason.empty()
                           ? "Playden does not know how to start this game yet."
                           : compatibility.reason};
    record->journal.reset();
    const auto persisted = Persist(*record);
    return persisted.ok() ? InstallServiceResult::Failure(
                                InstallServiceError::kIncompatible,
                                record->last_failure->message)
                          : persisted;
  }
  record->journal = PreparationJournal{"prepare-" + record->install_id,
                                       "prepare", "stage-title"};
  return Advance(*record, PreparationState::kPreparing, "stage-title");
}

InstallServiceResult
RuntimeInstallService::Retry(const std::string &install_id) {
  auto record = records_.Find(install_id);
  if (!record) {
    return InstallServiceResult::Failure(InstallServiceError::kNotFound,
                                         "runtime install was not found");
  }
  if (record->state != PreparationState::kFailed &&
      record->state != PreparationState::kUnavailable) {
    return InstallServiceResult::Failure(InstallServiceError::kInvalidState,
                                         "runtime install is not retryable");
  }
  if (ManagedPublication(*record)) {
    if (!publication_)
      return InstallServiceResult::Failure(InstallServiceError::kPackageUnavailable,
          "complete publication recovery driver is unavailable; retain the OWNER journal");
    const auto activity = native_.Activity(record->title_id);
    if (activity != NativeActivity::kStopped)
      return InstallServiceResult::Failure(activity == NativeActivity::kActive ? InstallServiceError::kActivityActive :
          InstallServiceError::kActivityUnknown, "native title must be positively stopped before publication retry");
    // A managed journal is frozen recovery authority even if a newer recipe is
    // unavailable. Preserve its UUID/operation/arm; legacy query-native retry
    // must never permit automatic repeat registration of an uncertain request.
    const bool armed = record->journal->operation == "settings-publication" &&
        (record->journal->step == "register" || record->journal->step == "await-native");
    return Advance(*record, armed ? PreparationState::kRegistering : PreparationState::kPreparing, record->journal->step);
  }
  const auto compatibility =
      compatibility_ ? compatibility_(*record) : CompatibilityResult{};
  if (!compatibility.compatible) {
    record->state = PreparationState::kDownloaded;
    record->journal.reset();
    record->last_failure =
        RuntimeFailure{"recipe-unavailable",
                       compatibility.reason.empty()
                           ? "Playden does not know how to start this game yet."
                           : compatibility.reason};
    const auto persisted = Persist(*record);
    return persisted.ok() ? InstallServiceResult::Failure(
                                InstallServiceError::kIncompatible,
                                record->last_failure->message)
                          : persisted;
  }
  if (!record->journal) {
    record->journal = PreparationJournal{"prepare-" + record->install_id,
                                         "prepare", "stage-title"};
  }
  if (record->journal->step == "register" ||
      record->journal->step == "await-native") {
    record->journal->step = "query-native";
  }
  const auto state = IsNativeStep(record->journal->step)
                         ? PreparationState::kRegistering
                         : PreparationState::kPreparing;
  return Advance(*record, state, record->journal->step);
}

InstallServiceResult
RuntimeInstallService::HandlePackageAction(RuntimeInstall record,
                                           const std::string &next_step,
                                           PackageActionResult result) {
  auto latest = records_.Find(record.install_id);
  if (!latest || !SameRuntimeInstallIdentity(*latest, record))
    return InstallServiceResult::Failure(InstallServiceError::kOwnershipConflict, "OWNER identity changed during package action; retain current state");
  if (!SamePreparationState(*latest, record)) {
    // Publication callbacks may perform the complete OWNER protocol themselves.
    // Other concurrent transitions likewise make this copied record stale.
    if (result.state == PackageActionState::kComplete && next_step == "query-native" &&
        latest->state == PreparationState::kReady && !latest->journal && !latest->last_failure &&
        latest->settings_revision >= record.settings_revision)
      return InstallServiceResult::Success();
    const auto error = result.state == PackageActionState::kCanceled ? InstallServiceError::kCanceled :
        ManagedPublication(*latest) && latest->journal->step == "await-native" ? InstallServiceError::kRegistrationPending :
        InstallServiceError::kPersistence;
    return InstallServiceResult::Failure(error, ErrorMessage(result.message, "OWNER advanced during package action; continue its current recovery journal"));
  }
  if (result.state == PackageActionState::kCanceled) {
    return Fail(std::move(record), InstallServiceError::kCanceled,
                ErrorCode(result.code, "canceled"),
                ErrorMessage(result.message, "preparation was canceled"));
  }
  if (result.state == PackageActionState::kFailed) {
    return Fail(std::move(record), InstallServiceError::kPackageFailed,
                ErrorCode(result.code, "package-step-failed"),
                ErrorMessage(result.message, "package preparation failed"));
  }
  const auto state = next_step == "query-native"
                         ? PreparationState::kRegistering
                         : PreparationState::kPreparing;
  return Advance(std::move(record), state, next_step);
}

InstallServiceResult RuntimeInstallService::StepPublication(const RuntimeInstall& record) {
  if (!publication_)
    return InstallServiceResult::Failure(InstallServiceError::kPackageUnavailable,
        "complete publication recovery driver is unavailable; retain the OWNER journal");
  const auto result = publication_(record);
  // The publication makes its folders 0700: an open bottle's are opened now (#868), also when the title's
  // registration waits for Playden to quit and nothing else runs before it. The title isn't registered or is
  // stopped (checked above). A failure only holds FTP back (logged).
  if (result.state == PackageActionState::kComplete || result.code == "publication-registration-pending")
    if (const auto latest = records_.Find(record.install_id); latest && package_.OpensBottle(*latest))
      (void)package_.SettleBottleModes(*latest);
  // The driver owns every durable transition. Even failures can follow file
  // effects or a newer registration arm; never write this older record back.
  if (result.state != PackageActionState::kComplete) {
    const auto error = result.state == PackageActionState::kCanceled ? InstallServiceError::kCanceled :
        result.code == "publication-registration-pending" ? InstallServiceError::kRegistrationPending : InstallServiceError::kPackageFailed;
    return InstallServiceResult::Failure(error, ErrorMessage(result.message, "complete publication remains unfinished"));
  }
  auto observed = records_.Find(record.install_id);
  if (!observed || !SameRuntimeInstallIdentity(*observed, record) || observed->state != PreparationState::kReady ||
      observed->journal || observed->last_failure || observed->settings_revision < record.settings_revision)
    return InstallServiceResult::Failure(InstallServiceError::kPersistence,
        "publication completion has no matching durable Ready OWNER readback");
  return InstallServiceResult::Success();
}

InstallServiceResult
RuntimeInstallService::StepNative(RuntimeInstall record,
                                  const PackageStatus &package_status) {
  if (!PackageReady(package_status, record)) {
    return Fail(
        std::move(record), InstallServiceError::kOwnershipConflict,
        "package-incoherent",
        "staged package, owned bottle, artifacts and profile are not coherent");
  }
  const std::string journal_step = record.journal ? record.journal->step : "";
  const auto native = native_.Query(record);
  if (!native.available) {
    return Fail(std::move(record), InstallServiceError::kNativeUnavailable,
                "native-unavailable",
                "native registration inventory is unavailable");
  }
  if (native.collision) {
    return Fail(std::move(record), InstallServiceError::kNativeCollision,
                "native-collision",
                "native title does not match Playden ownership");
  }
  if (native.installed && native.source_available &&
      native.owned_manifest_matches) {
    record.state = PreparationState::kReady;
    record.journal.reset();
    record.last_failure.reset();
    return Persist(std::move(record));
  }
  if (native.scan_pending) {
    if (journal_step == "await-native")
      return InstallServiceResult::Success();
    return Advance(std::move(record), PreparationState::kRegistering,
                   "await-native");
  }
  if ((native.installed || native.source_available) &&
      !native.owned_manifest_matches) {
    return Fail(std::move(record), InstallServiceError::kNativeCollision,
                "native-collision",
                "native title does not match Playden ownership");
  }
  if (native.installed && !native.source_available) {
    return Fail(std::move(record), InstallServiceError::kNativeIncoherent,
                "installed-source-missing",
                "installed native title has no matching owned source");
  }
  if (!native.source_available) {
    return Fail(std::move(record), InstallServiceError::kNativeIncoherent,
                "native-source-missing",
                "staged owned native source is not visible to registration");
  }
  if (journal_step == "register" || journal_step == "await-native") {
    return Fail(std::move(record), InstallServiceError::kRegistrationPending,
                "registration-uncertain",
                "registration completion is not visible; retry explicitly "
                "after checking the service");
  }

  record.state = PreparationState::kRegistering;
  record.journal->step = "register";
  record.last_failure.reset();
  const auto armed = Persist(record);
  if (!armed.ok())
    return armed;

  const auto submitted = native_.Register(record);
  if (submitted.state == RegistrationResultState::kStaleHook) {
    return Fail(
        std::move(record), InstallServiceError::kNativeUnavailable,
        ErrorCode(submitted.code, "stale-hook"),
        ErrorMessage(submitted.message,
                     "native registration hook is stale; restart it manually"));
  }
  if (submitted.state == RegistrationResultState::kFailed) {
    return Fail(std::move(record), InstallServiceError::kNativeUnavailable,
                ErrorCode(submitted.code, "registration-failed"),
                ErrorMessage(submitted.message, "native registration failed"));
  }
  return Advance(std::move(record), PreparationState::kRegistering,
                 "await-native");
}

InstallServiceResult
RuntimeInstallService::Step(const std::string &install_id) {
  auto record = records_.Find(install_id);
  if (!record) {
    return InstallServiceResult::Failure(InstallServiceError::kNotFound,
                                         "runtime install was not found");
  }
  const bool requested_update = record->state == PreparationState::kReady && record->journal &&
      record->journal->operation == "settings-request" && record->journal->step == "capture-update";
  if ((record->state != PreparationState::kPreparing &&
       record->state != PreparationState::kRegistering && !requested_update) ||
      !record->journal) {
    return InstallServiceResult::Failure(
        InstallServiceError::kInvalidState,
        "runtime install has no active preparation journal");
  }
  const bool managed = ManagedPublication(*record) || (publication_ && record->journal->operation == "prepare" &&
      record->journal->step == "publish-profile");
  if (canceled_ && canceled_()) {
    if (managed) return InstallServiceResult::Failure(InstallServiceError::kCanceled, "complete publication was canceled; retain recovery intent");
    return Fail(*record, InstallServiceError::kCanceled, "canceled",
                "preparation was canceled");
  }
  const auto activity = native_.Activity(record->title_id);
  if (activity == NativeActivity::kUnknown) {
    if (managed) return InstallServiceResult::Failure(InstallServiceError::kActivityUnknown, "native title activity is unknown; retain recovery intent");
    return Fail(*record, InstallServiceError::kActivityUnknown,
                "title-activity-unknown",
                "native title activity is unknown; preparation is blocked");
  }
  if (activity == NativeActivity::kActive) {
    if (managed) return InstallServiceResult::Failure(InstallServiceError::kActivityActive, "native title is active; retain recovery intent");
    return Fail(*record, InstallServiceError::kActivityActive, "title-active",
                "native title is active; stop it before preparation");
  }

  if (managed) return StepPublication(*record);
  if (record->journal->operation != "prepare")
    return InstallServiceResult::Failure(InstallServiceError::kInvalidState, "unknown preparation operation; retain recovery intent");
  const auto status = package_.Inspect(*record);
  if (!status.known) {
    return Fail(*record, InstallServiceError::kPackageUnavailable,
                "package-state-unknown", "package state is unavailable");
  }
  const auto &step = record->journal->step;
  if (IsNativeStep(step))
    return StepNative(*record, status);

  if (step == "stage-title") {
    if (status.title_staged) {
      if (!status.manifest_matches) {
        return Fail(*record, InstallServiceError::kOwnershipConflict,
                    "title-stage-collision",
                    "staged native title is not owned by this install");
      }
      return Advance(*record, PreparationState::kPreparing, "copy-template");
    }
    return HandlePackageAction(*record, "copy-template",
                               package_.StageTitle(*record));
  }
  if (step == "copy-template") {
    if (status.mutable_bottle_present) {
      if (!status.mutable_bottle_owned || !status.template_owned) {
        return Fail(
            *record, InstallServiceError::kOwnershipConflict,
            "bottle-ownership-conflict",
            "existing mutable bottle is not an owned validated template");
      }
      return Advance(*record, PreparationState::kPreparing,
                     "validate-artifacts");
    }
    return HandlePackageAction(*record, "validate-artifacts",
                               package_.CopyTemplate(*record));
  }
  if (step == "validate-artifacts") {
    if (status.artifacts_valid) {
      return Advance(*record, PreparationState::kPreparing, "publish-profile");
    }
    return HandlePackageAction(*record, "publish-profile",
                               package_.ValidateArtifacts(*record));
  }
  if (step == "publish-profile") {
    if (status.profile_published &&
        status.profile_settings_revision == record->settings_revision) {
      return Advance(*record, PreparationState::kRegistering, "query-native");
    }
    return HandlePackageAction(*record, "query-native",
                               package_.PublishProfile(*record));
  }
  return Fail(*record, InstallServiceError::kPackageFailed,
              "journal-step-invalid", "preparation journal step is invalid");
}

std::vector<RuntimeInstall> RuntimeInstallService::Snapshot() const {
  return records_.Snapshot();
}

} // namespace playden::runtime
