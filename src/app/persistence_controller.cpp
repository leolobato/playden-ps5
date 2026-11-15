#include "app/persistence_controller.h"

#include <utility>

namespace playden::app {
namespace {

constexpr std::size_t kEventsPerTick = 16;
constexpr std::size_t kShutdownEvents = 64;

bool PreferencesLoadFailed(PreferencesLoadStatus status) {
  return status == PreferencesLoadStatus::kDefaultCorrupt ||
         status == PreferencesLoadStatus::kDefaultReadError;
}

const char* ErrorName(StorageError error) {
  switch (error) {
    case StorageError::kNone: return "none";
    case StorageError::kInvalidPath: return "invalid path";
    case StorageError::kNotFound: return "not found";
    case StorageError::kOpenFailed: return "open failed";
    case StorageError::kReadFailed: return "read failed";
    case StorageError::kWriteFailed: return "write failed";
    case StorageError::kRenameFailed: return "rename failed";
    case StorageError::kInvalidData: return "invalid data";
    case StorageError::kPermissionFailed: return "permission failed";
    case StorageError::kRemoveFailed: return "remove failed";
    case StorageError::kSyncFailed: return "sync failed";
  }
  return "unknown";
}

std::string Failure(StorageError error, const std::string& message) {
  return message.empty() ? ErrorName(error) : std::string(ErrorName(error)) + ": " + message;
}

PreferencesLoadResult PreferenceResult(const PersistenceEvent& event) {
  PreferencesLoadResult result;
  if (event.preferences) result.preferences = *event.preferences;
  result.status = event.preferences_status;
  return result;
}

}  // namespace

PersistenceController::PersistenceController(homebrew::Simulation& model,
                                             PersistenceService& service)
    : model_(model), service_(service) {
  BeginPreferencesLoad();
  BeginOrganizationLoad();
}

void PersistenceController::BeginPreferencesLoad() {
  preferences_load_token_ = model_.BeginPreferencesLoad();
  preferences_load_request_ = service_.LoadPreferences();
  preferences_load_failed_ = false;
  if (preferences_load_request_) return;

  PreferencesLoadResult failure;
  failure.status = PreferencesLoadStatus::kDefaultReadError;
  failure.diagnostic = "Preferences are busy and could not be loaded.";
  model_.ApplyPreferences(preferences_load_token_, failure);
  preferences_load_failed_ = true;
  Report(kPreferencesLoad, failure.diagnostic);
}

void PersistenceController::BeginOrganizationLoad() {
  organization_load_token_ = model_.BeginOrganizationLoad(kDeviceOrganization);
  organization_load_request_ =
      service_.LoadOrganization(kDeviceOrganization, organization_generation_);
  organization_load_failed_ = false;
  if (organization_load_request_) return;

  model_.FailOrganizationLoad(kDeviceOrganization, organization_load_token_,
                              StorageError::kReadFailed);
  organization_load_failed_ = true;
  Report(kOrganizationLoad, "busy");
}

// Organization belongs to the device: store accounts coming and going leave it alone. The update check is
// the account's own.
void PersistenceController::Activate(std::uint64_t account) {
  if (shutting_down_ || !account || account == update_account_) return;
  if (update_account_) Reset();
  update_account_ = account;
  submitted_update_revision_ = 0;
  model_.BeginUpdateCheckLoad(account);
  update_load_request_ = service_.LoadUpdateCheck(account);
  // Nowhere to read it from: the account starts with no check.
  if (!update_load_request_) model_.ApplyUpdateCheck(account, std::nullopt);
}

void PersistenceController::Reset() {
  if (shutting_down_) return;
  ObserveAndSave();
  update_account_ = 0;
  update_load_request_ = 0;
  submitted_update_revision_ = 0;
  model_.ResetUpdateCheck();
}

void PersistenceController::Tick() {
  if (shutting_down_) return;
  Pump(kEventsPerTick);
  if (model_.TakePersistenceRetryRequest()) Retry();
  ObserveAndSave();
}

void PersistenceController::Retry() {
  if (shutting_down_) return;
  if (preferences_load_failed_) BeginPreferencesLoad();
  else submitted_preferences_revision_ = 0;

  if (organization_load_failed_) BeginOrganizationLoad();
  else submitted_organization_revision_ = 0;
  // The update check's last save may have failed too.
  submitted_update_revision_ = 0;
}

void PersistenceController::Clear() {
  // Signing out keeps the device's organization; there is nothing to delete.
  if (shutting_down_ || clear_result_) return;
  ObserveAndSave();
  clear_result_ = StorageError::kNone;
}

std::optional<StorageError> PersistenceController::TakeClearResult() {
  auto result = clear_result_;
  clear_result_.reset();
  return result;
}

void PersistenceController::Pump(std::size_t maximum) {
  for (auto& event : service_.Pump(maximum)) Apply(std::move(event));
}

void PersistenceController::Apply(PersistenceEvent event) {
  if (event.kind == PersistenceEventKind::kPreferencesLoaded) {
    if (event.request_id != preferences_load_request_) return;
    preferences_load_request_ = 0;
    const auto loaded = PreferenceResult(event);
    preferences_load_failed_ = PreferencesLoadFailed(loaded.status);
    // Defaults in place of a damaged file, which the store set aside: the load worked.
    if (!preferences_load_failed_ && !event.message.empty() && log_) log_("preferences load: " + event.message);
    Report(kPreferencesLoad, preferences_load_failed_ ? Failure(event.error, event.message) : std::string());
    model_.ApplyPreferences(preferences_load_token_, loaded);
    return;
  }
  if (event.kind == PersistenceEventKind::kPreferencesSaved) {
    Report(kPreferencesSave, event.error == StorageError::kNone ? std::string() : Failure(event.error, event.message));
    model_.PreferencesSaveResult(event.revision, event.error);
    return;
  }
  if (event.kind == PersistenceEventKind::kUpdateCheckLoaded) {
    if (event.request_id != update_load_request_ || event.account_id != update_account_) return;
    update_load_request_ = 0;
    // A damaged file starts the account over; the next check writes a new one.
    Report(kUpdateCheckLoad, event.error == StorageError::kNone ? std::string() : Failure(event.error, event.message));
    model_.ApplyUpdateCheck(event.account_id, event.error == StorageError::kNone ? std::move(event.update_check)
                                                                                 : std::nullopt);
    return;
  }
  if (event.kind == PersistenceEventKind::kUpdateCheckSaved) {
    Report(kUpdateCheckSave, event.error == StorageError::kNone ? std::string() : Failure(event.error, event.message));
    model_.UpdateCheckSaveResult(event.account_id, event.revision, event.error);
    return;
  }
  if (event.kind == PersistenceEventKind::kOrganizationCleared) return;
  if (event.account_id != kDeviceOrganization ||
      event.account_generation != organization_generation_) return;
  if (event.kind == PersistenceEventKind::kOrganizationLoaded) {
    if (event.request_id != organization_load_request_) return;
    organization_load_request_ = 0;
    if (event.error == StorageError::kNone) {
      if (!event.message.empty() && log_) log_("organization load: " + event.message);
      organization_load_failed_ = !model_.ApplyOrganization(
          kDeviceOrganization, organization_load_token_, std::move(event.organization));
      // Read fine, but holds more than the Library can show.
      Report(kOrganizationLoad, organization_load_failed_ ? "too large for the Library" : "");
    } else {
      organization_load_failed_ = true;
      Report(kOrganizationLoad, Failure(event.error, event.message));
      model_.FailOrganizationLoad(kDeviceOrganization, organization_load_token_,
                                  event.error);
    }
    return;
  }
  if (event.kind == PersistenceEventKind::kOrganizationSaved) {
    Report(kOrganizationSave, event.error == StorageError::kNone ? std::string() : Failure(event.error, event.message));
    model_.OrganizationSaveResult(event.account_id, event.revision, event.error);
  }
}

void PersistenceController::Report(Channel channel, const std::string& failure) {
  static const char* const kNames[kChannels] = {"preferences load", "preferences save", "organization load",
                                                "organization save", "update check load", "update check save"};
  auto& last = reported_[channel];
  if (failure == last) return;
  if (log_) log_(failure.empty() ? std::string(kNames[channel]) + " works again"
                                 : std::string(kNames[channel]) + " failed: " + failure);
  last = failure;
}

void PersistenceController::ObserveAndSave() {
  if (update_account_ && !update_load_request_ && model_.UpdateCheckAccount() == update_account_ &&
      model_.UpdateCheckDirty()) {
    const auto revision = model_.UpdateCheckRevision();
    if (revision != submitted_update_revision_) {
      submitted_update_revision_ = revision;
      if (!service_.SaveUpdateCheck(update_account_, revision, model_.ExportUpdateCheck()))
        model_.UpdateCheckSaveResult(update_account_, revision, StorageError::kWriteFailed);
    }
  }

  if (!preferences_load_request_ && !preferences_load_failed_ &&
      model_.PreferencesDirty()) {
    const auto revision = model_.PreferencesRevision();
    if (revision != submitted_preferences_revision_) {
      const auto request =
          service_.SavePreferences(model_.ExportPreferences(), revision);
      submitted_preferences_revision_ = revision;
      if (!request)
        model_.PreferencesSaveResult(revision, StorageError::kWriteFailed);
    }
  }

  if (organization_load_request_ ||
      organization_load_failed_ || !model_.OrganizationReadyForSave() ||
      !model_.OrganizationDirty()) return;
  const auto revision = model_.OrganizationRevision();
  if (revision == submitted_organization_revision_) return;
  const auto request = service_.SaveOrganization(
      kDeviceOrganization, organization_generation_, revision, model_.ExportOrganization());
  submitted_organization_revision_ = revision;
  if (!request)
    model_.OrganizationSaveResult(kDeviceOrganization, revision,
                                  StorageError::kWriteFailed);
}

void PersistenceController::FlushAndShutdown() {
  if (shutting_down_) return;

  // Complete required loads before stopping: their results supply untouched
  // stored fields which the model merges with edits made while IO was pending.
  ObserveAndSave();
  service_.WaitIdle();
  Pump(kShutdownEvents);
  ObserveAndSave();
  service_.WaitIdle();
  Pump(kShutdownEvents);
  service_.Shutdown();
  Pump(kShutdownEvents);
  shutting_down_ = true;
}

}  // namespace playden::app
