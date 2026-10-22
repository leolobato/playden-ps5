#include "runtime/settings_publication_owner.h"
#include "platform/usb_mount.h"
#include "runtime/publication_contract.h"
#include "runtime/artifact_hash.h"
#include "platform/managed_file_lock.h"
#include "platform/storage.h"
#include <cerrno>
#include <cstring>
#include <unistd.h>
namespace playden::runtime {
namespace {
// The program a publication starts, and the recipe it was built from: Game settings may have
// chosen another program or another recipe (RuntimeHost::RepublishSettings). The captured operation
// carries them, and the record names them from the commit on, also when a commit is resumed after a
// crash. Until then the record keeps the recipe the game plays under.
void CommitLaunch(RuntimeInstall* record, const RuntimeInstall& published) {
  record->launch_entry_id = published.launch_entry_id;
  record->launch_path = published.launch_path;
  record->launch_working_directory = published.launch_working_directory;
  record->launch_in_install_root = published.launch_in_install_root;
  record->launch_arguments = published.launch_arguments;
  record->recipe_id = published.recipe_id;
  record->recipe_version = published.recipe_version;
}
PackageActionResult Failure(const char* code, const char* message) {
  return {PackageActionState::kFailed, code, message};
}
bool OwnedPackage(const PackageStatus& s) {
  // Identity only: the title and bottle markers name this install. Whether they
  // were made from the current kit is TitleCurrent's question (Update), not this one.
  return s.known && s.title_staged && s.manifest_matches && s.template_owned &&
      s.mutable_bottle_present && s.mutable_bottle_owned;
}
bool ReadyCaptureAllowed(const RuntimeInstall& record, const std::string& transaction) {
  return record.state == PreparationState::kReady && !record.last_failure &&
      (!record.journal || (record.journal->operation == "settings-request" &&
       record.journal->step == "capture-update" && record.journal->operation_id == transaction));
}
PublicationPaths OperationPaths(const SettingsPublicationOperation& operation, const std::filesystem::path& prefix) {
  auto map = [&](const std::string& value) {
    const auto path = std::filesystem::path(value);
    return prefix.empty() ? path : prefix / path.relative_path();
  };
  const auto profiles = operation.install.data_root + "/library/profiles/";
  return {map(profiles + operation.plan.profile_id + ".profile"), map(profiles + "profiles.lst"),
          map(operation.install.data_root + "/library/input"),
          map("/data/homebrew/" + operation.install.title_id + "/game-launch.json"),
          map(profiles + ".playden-publication")};
}
// A Ready update publishes a newer revision, or the same one again when the game folder
// moved (RuntimeInstall::moved_from): its publication must then name the new folder. With a
// kit that admits unowned folders, a committed owned publication (schema 2) is also published
// again as unowned; one already unowned, or unreadable, is not. The same revision is also
// published again when the title moved to another runtime and the same settings now make
// another profile (RuntimeHost::RepublishSettings, force), and when the plan is built from another
// recipe (a recipe switch whose choice fell back at its own revision and applies now): its descriptor
// and receipt name the new recipe even when the profile reads the same. Never an older revision.
bool UpdateRevisionAllowed(const RuntimeInstall& current, const SettingsLaunchPlan& plan, bool unowned_game_roots,
                           const std::filesystem::path& prefix) {
  if (plan.settings_revision > current.settings_revision) return true;
  if (plan.settings_revision != current.settings_revision) return false;
  if (!current.moved_from.empty()) return true;
  if (plan.recipe_id != current.recipe_id || plan.recipe_version != current.recipe_version) return true;
  if (unowned_game_roots && PublishedGameRootUnowned(current, prefix) == std::optional<bool>(false)) return true;
  const auto published = PublishedProfileDigest(current, prefix);
  return published && HashArtifactBytes(plan.profile_bytes) != published;
}
bool MatchingJournal(const RuntimeInstall& r, const SettingsPublicationOperation& o) {
  return r.journal && r.journal->operation == "settings-publication" && r.journal->operation_id == o.transaction_id;
}
}
SettingsPublicationOwner::SettingsPublicationOwner(InstallRecordStore& records, PackageInstaller& package,
    NativeCoordinator& native, bool unowned_game_roots, std::filesystem::path host_namespace_prefix)
    : records_(records), package_(package), native_(native), unowned_game_roots_(unowned_game_roots),
      host_namespace_prefix_(std::move(host_namespace_prefix)) {}
PackageActionResult SettingsPublicationOwner::Request(const SettingsPublicationRequest& request,
    const std::filesystem::path& directory) {
  std::string reason; auto expected = EncodeSettingsPublicationRequest(request, &reason);
  auto current = records_.Find(request.install.install_id);
  const bool update = current && current->state == PreparationState::kReady;
  const bool armed = current && current->journal && current->journal->operation == "settings-request";
  if (!expected || !current || !SameRuntimeInstallIdentity(*current, request.install) ||
      (update ? !UpdateRevisionAllowed(*current, request.plan, unowned_game_roots_, host_namespace_prefix_) :
                request.plan.settings_revision != current->settings_revision) ||
      (update ? !ReadyCaptureAllowed(*current, request.transaction_id) :
                current->state != PreparationState::kPreparing || !current->journal ||
                (current->journal->operation != "prepare" && !armed)))
    return Failure("publication-request-owner", "The install record, its revision or its journal does not allow this settings request.");
  if (armed && (current->journal->operation_id != request.transaction_id ||
      current->journal->step != (update ? "capture-update" : "capture-settings")))
    return Failure("publication-request-conflict", "A different requested operation must be reconciled first.");
  // Package ownership is checked once per apply, when the publication blocks OWNER
  // (Block). An update's existing committed publication is proven by capture, which
  // verifies the committed receipt and its complete file set before anything changes.
  auto loaded = LoadSettingsPublicationRequest(directory, request.transaction_id);
  if (armed) {
    auto observed = loaded.request ? EncodeSettingsPublicationRequest(*loaded.request, &reason) : std::nullopt;
    if (loaded.state != PublicationRecoveryState::kValid || !observed || *observed != *expected)
      return Failure("publication-request-conflict", "Durable request is missing or disagrees with the OWNER arm.");
    return {PackageActionState::kComplete, {}, {}};
  }
  if (loaded.state != PublicationRecoveryState::kAbsent)
    return Failure("publication-request-orphan", "An existing unarmed request cannot be adopted or overwritten.");
  auto written = CreateSettingsPublicationRequest(directory, request);
  if (!written.ok()) return {PackageActionState::kFailed, "publication-request-write", written.message()};
  // This record is the startup dispatch authority. A failure leaves an inert
  // immutable request, never permission to regenerate or overwrite its plan.
  auto observed = records_.Find(current->install_id);
  if (!observed || !SameRuntimeInstallIdentity(*observed, *current) || observed->state != current->state ||
      observed->settings_revision != current->settings_revision || observed->journal.has_value() != current->journal.has_value() ||
      (current->journal && (observed->journal->operation != current->journal->operation ||
       observed->journal->operation_id != current->journal->operation_id || observed->journal->step != current->journal->step)))
    return Failure("publication-request-owner", "The install record changed while the request was saved; the saved request is kept.");
  current->journal = PreparationJournal{request.transaction_id, "settings-request", update ? "capture-update" : "capture-settings"};
  current->last_failure.reset();
  return Persist(*current);
}
PackageActionResult SettingsPublicationOwner::ResumeRequested(const RuntimeInstall& record,
    const std::filesystem::path& directory, const std::filesystem::path& prefix) {
  auto current = records_.Find(record.install_id);
  if (!current || !SameRuntimeInstallIdentity(*current, record) || current->settings_revision != record.settings_revision ||
      !current->journal || !record.journal || current->journal->operation != record.journal->operation ||
      current->journal->operation_id != record.journal->operation_id || current->journal->step != record.journal->step ||
      (current->state != PreparationState::kPreparing && current->state != PreparationState::kRegistering &&
       !ReadyCaptureAllowed(*current, current->journal->operation_id)))
    return Failure("publication-request-owner", "Startup recovery requires the current durable requested OWNER arm.");
  if (current->journal->operation == "settings-publication") return Resume(*current, prefix);
  if (current->journal->operation != "settings-request" && current->journal->operation != "settings-control")
    return Failure("publication-request-owner", "No requested capture/control operation is armed.");
  auto loaded = LoadSettingsPublicationRequest(directory, current->journal->operation_id);
  if (loaded.state != PublicationRecoveryState::kValid || !loaded.request ||
      !SameRuntimeInstallIdentity(loaded.request->install, *current) ||
      (current->state == PreparationState::kReady ? !UpdateRevisionAllowed(*current, loaded.request->plan, unowned_game_roots_, prefix) :
                                                  loaded.request->plan.settings_revision != current->settings_revision))
    return Failure("publication-request-unavailable", "Requested plan is missing, altered or does not match OWNER; preserve all evidence.");
  const auto& request = *loaded.request;
  auto prepared = Prepare(request.install, request.plan, request.kit_id, request.transaction_id, request.registry_unix_time, prefix);
  if (!prepared.ok()) return {PackageActionState::kFailed, "publication-request-capture", prepared.message()};
  // Capture durably freezes snapshots/backups. Publication reacquires the
  // qualified lease, verifies prior committed semantics and rechecks OWNER
  // before any effects. The Ready request arm blocks Play during this handoff.
  auto dependencies = Dependencies(prefix, GameRootMoveOf(*current));
  return PublishSettingsOperation(prepared.value(), OperationPaths(prepared.value(), prefix), dependencies);
}
PackageActionResult SettingsPublicationOwner::DiscardRequested(const RuntimeInstall& record,
    const std::filesystem::path& directory, const std::filesystem::path& prefix) {
  if (!prefix.empty() && (!prefix.is_absolute() || prefix != prefix.lexically_normal()))
    return Failure("publication-discard-invalid", "Invalid discard namespace.");
  auto current = records_.Find(record.install_id);
  if (!current || !SameRuntimeInstallIdentity(*current, record) || current->settings_revision != record.settings_revision ||
      current->state != PreparationState::kReady || !current->journal || !record.journal ||
      current->journal->operation != "settings-request" || current->journal->step != "capture-update" ||
      current->journal->operation_id != record.journal->operation_id ||
      !IsSettingsPublicationTransactionId(current->journal->operation_id))
    return Failure("publication-discard-owner", "The install record has no requested update to drop.");
  const auto transaction = current->journal->operation_id;
  const auto canonical = std::filesystem::path(current->data_root + "/.playden-control");
  const auto control_path = prefix.empty() ? canonical : prefix / canonical.relative_path();
  auto control = platform::ManagedDirectory::Open(control_path, platform::DirectoryPolicy::kPrivate);
  auto lease = control ? platform::ManagedFileLock::Acquire(*control, "publication.lock", platform::LockMode::kPublisher) : std::nullopt;
  if (!lease) return Failure("publication-busy", "The publication lock is unavailable or held (the game may be running).");
  // The committed publication the record plays under: another transaction, at the record's
  // revision. A pending receipt, or one of this transaction, means the request got further
  // than its capture (Block then also moved the journal on): nothing is dropped.
  struct stat info{};
  if (!control->InspectChild("publication-state.v1", &info) || !S_ISREG(info.st_mode) || info.st_size <= 0 || info.st_size > 8192)
    return Failure("publication-discard-receipt", "The committed publication's receipt is missing or unreadable.");
  auto read = Storage(control_path.string()).ReadDurable("publication-state.v1", 8192);
  PwTitlePublication receipt{};
  if (!read.ok() || pw_title_publication_parse(read.value().data(), read.value().size(), &receipt) != PW_OK ||
      receipt.phase != PW_TITLE_PUBLICATION_COMMITTED || receipt.transaction_id == transaction ||
      receipt.install_id != current->install_id || receipt.title_id != current->title_id ||
      receipt.settings_revision != current->settings_revision)
    return Failure("publication-discard-receipt", "The committed publication does not prove this request never applied.");
  // An active journal of this transaction would mean the same; one that can't be read is the
  // previous operation's (the record's journal proves this one never blocked OWNER).
  auto active = LoadSettingsPublicationOperation(control_path);
  if (active.operation && active.operation->transaction_id == transaction)
    return Failure("publication-discard-conflict", "The requested operation already has an active journal.");
  auto observed = records_.Find(record.install_id);
  if (!observed || !SameRuntimeInstallIdentity(*observed, *current) || observed->state != current->state ||
      observed->settings_revision != current->settings_revision || !observed->journal ||
      observed->journal->operation != "settings-request" || observed->journal->operation_id != transaction ||
      observed->journal->step != "capture-update" || !lease->Validate(*control, "publication.lock"))
    return Failure("publication-discard-owner", "The install record or the publication lock changed while the request was dropped.");
  current->journal.reset();
  current->last_failure.reset();
  auto saved = Persist(*current);
  if (saved.state != PackageActionState::kComplete) return saved;
  // This capture's registry backup and prepared intent are unreferenced too: each retry is a new
  // transaction, so they would pile up (a backup copies user.reg). Only this transaction's, by name;
  // RemoveRegularFile admits only an owned regular file, never a link. Best effort.
  bool removed_any = false;
  for (const auto& name : {"registry-backup-" + transaction + ".v1", "settings-intent-" + transaction + ".v1"}) {
    struct stat leftover{};
    if (!control->InspectChild(name, &leftover)) {
      if (errno != ENOENT && log_) log_("settings request dropped install=" + current->install_id + ": " + name +
                                        " stays (" + std::strerror(errno) + ")");
      continue;
    }
    if (control->RemoveRegularFile(name)) removed_any = true;
    else if (log_) log_("settings request dropped install=" + current->install_id + ": " + name + " stays (" +
                        std::strerror(errno) + ")");
  }
  if (removed_any && !control->Sync() && log_)
    log_("settings request dropped install=" + current->install_id + ": control folder sync failed");
  // Unreferenced now; a request that stays is a leftover (RuntimeHost::LeftoverRequests).
  auto removed = Storage(directory.string()).RemoveDurable("settings-request-" + transaction + ".v1");
  return {PackageActionState::kComplete, {}, removed.ok() ? std::string{} : "The saved request stays as a leftover: " + removed.message()};
}
PackageActionResult SettingsPublicationOwner::InitializeControl(const RuntimeInstall& record, const std::string& transaction,
    const std::filesystem::path& prefix, const std::function<bool(PublicationControlStep)>& after_step) {
  if (!IsSettingsPublicationTransactionId(transaction) ||
      (!prefix.empty() && (!prefix.is_absolute() || prefix != prefix.lexically_normal())))
    return Failure("publication-control-invalid", "Invalid control initialization identity/namespace.");
  auto current = records_.Find(record.install_id);
  if (!current || !SameRuntimeInstallIdentity(*current, record) || current->settings_revision != record.settings_revision ||
      current->state == PreparationState::kRemoving || current->state == PreparationState::kReady ||
      current->state == PreparationState::kRegistering)
    return Failure("publication-control-owner", "The install record does not allow setting up the publication control folder.");
  const bool recovering = current->journal && current->journal->operation == "settings-control";
  if (recovering && (current->journal->operation_id != transaction || current->state != PreparationState::kPreparing ||
      (current->journal->step != "initialize-control" && current->journal->step != "control-ready")))
    return Failure("publication-control-conflict", "A different control initialization intent must be reconciled first.");
  if (!recovering && current->journal && current->journal->operation != "prepare" &&
      !(current->journal->operation == "settings-request" && current->journal->operation_id == transaction &&
        current->journal->step == "capture-settings"))
    return Failure("publication-control-conflict", "An existing OWNER operation cannot be replaced by control initialization.");
  const auto canonical = std::filesystem::path(record.data_root);
  const auto data_path = prefix.empty() ? canonical : prefix / canonical.relative_path();
  auto data = platform::ManagedDirectory::Open(data_path, platform::DirectoryPolicy::kPrivateTraversable);
  if (!data) return Failure("publication-control-data", "Existing owned private bottle data root is unavailable.");
  struct stat info{};
  const bool exists = data->InspectChild(".playden-control", &info);
  if (!exists && errno != ENOENT)
    return Failure("publication-control-collision", "Control path is foreign or unavailable.");
  if (!recovering) {
    // An existing private control folder (an earlier, interrupted setup) is adopted.
    if (!data->Sync())
      return Failure("publication-control-collision", "Control initialization could not sync the data root.");
    // The reserved OWNER intent is durable before mkdir or exclusive lock
    // creation. ManagedDirectory requires coordinated namespace writers; this
    // journal is not a boundary against hostile concurrent ancestor replacement.
    current->state = PreparationState::kPreparing;
    current->journal = PreparationJournal{transaction, "settings-control", "initialize-control"};
    current->last_failure.reset();
    auto saved = Persist(*current); if (saved.state != PackageActionState::kComplete) return saved;
  }
  const bool prepared = current->journal->step == "control-ready";
  auto checkpoint = [&](PublicationControlStep step) {
    return !after_step || after_step(step);
  };
  if (!checkpoint(PublicationControlStep::kOwnerArmed))
    return Failure("publication-control-interrupted", "Control setup interrupted after durable OWNER intent.");
  if (!data->Validate())
    return Failure("publication-control-owner", "The game's data folder changed after the control setup was recorded.");
  // Recheck absence after persistence; a prepared setup never recreates a
  // missing directory or lock, because replacement would change lease identity.
  const bool now_exists = data->InspectChild(".playden-control", &info);
  if ((!now_exists && errno != ENOENT) || (!now_exists && prepared))
    return Failure("publication-control-collision", "Control path changed after admission; retain the OWNER intent.");
  auto control = data->OpenChild(".playden-control", platform::DirectoryPolicy::kPrivate, !prepared);
  if (!control || !control->Sync() || !data->Sync())
    return Failure("publication-control-directory", "Private control creation/sync failed; recover from durable OWNER intent.");
  if (!checkpoint(PublicationControlStep::kDirectorySynced))
    return Failure("publication-control-interrupted", "Control setup interrupted after directory sync.");
  if (!control->Validate())
    return Failure("publication-control-owner", "The control folder changed before the publication lock was created.");
  const bool lock_exists = control->InspectChild("publication.lock", &info);
  if (!lock_exists && (errno != ENOENT || prepared))
    return Failure("publication-control-lock", "Stable publication lock is missing or foreign.");
  int fd = lock_exists ? control->OpenRegularFile("publication.lock", true, 0600) :
      control->CreateExclusiveFile("publication.lock", 0600);
  if (fd < 0) return Failure("publication-control-lock", "Stable publication lock creation/admission failed.");
  const bool lock_valid = fstat(fd, &info) == 0 && S_ISREG(info.st_mode) && info.st_uid == geteuid() &&
      (info.st_mode & 07777) == 0600 && info.st_nlink == 1 && info.st_size == 0 && fsync(fd) == 0;
  const bool closed = close(fd) == 0;
  if (!lock_valid || !closed || !control->Sync())
    return Failure("publication-control-lock", "Stable empty private lock failed durable validation; preserve it.");
  auto lease = platform::ManagedFileLock::Acquire(*control, "publication.lock", platform::LockMode::kPublisher);
  if (!lease || !checkpoint(PublicationControlStep::kLockSynced))
    return Failure("publication-control-lock", "Control setup lock is busy or interrupted after sync.");
  auto observed = records_.Find(record.install_id);
  if (!observed || !SameRuntimeInstallIdentity(*observed, *current) || observed->settings_revision != current->settings_revision ||
      observed->state != PreparationState::kPreparing || !observed->journal ||
      observed->journal->operation != "settings-control" || observed->journal->operation_id != transaction ||
      observed->journal->step != current->journal->step || !lease->Validate(*control, "publication.lock"))
    return Failure("publication-control-owner", "The install record, its journal or the publication lock changed before control setup finished.");
  if (!prepared) {
    current->journal->step = "control-ready";
    auto saved = Persist(*current); if (saved.state != PackageActionState::kComplete) return saved;
  }
  if (!checkpoint(PublicationControlStep::kOwnerPrepared))
    return Failure("publication-control-interrupted", "Control setup interrupted after durable completion intent.");
  return {PackageActionState::kComplete, {}, {}};
}
Result<SettingsPublicationOperation> SettingsPublicationOwner::Prepare(const RuntimeInstall& record,
    const SettingsLaunchPlan& plan, const std::string& kit, const std::string& transaction, std::int64_t time,
    const std::filesystem::path& prefix) {
  auto fail = [](const std::string& reason) { return Result<SettingsPublicationOperation>::Failure(StorageError::kInvalidData, reason); };
  if (!prefix.empty() && (!prefix.is_absolute() || prefix != prefix.lexically_normal())) return fail("Invalid canonical namespace.");
  auto current = records_.Find(record.install_id);
  if (!current || !SameRuntimeInstallIdentity(*current, record) || current->state == PreparationState::kRemoving)
    return fail("The install record does not match this game, or it is being removed.");
  const auto canonical = std::filesystem::path(record.data_root + "/.playden-control");
  const auto control_path = prefix.empty() ? canonical : prefix / canonical.relative_path();
  // A missing control folder or lock is set up; an existing one (also one an
  // interrupted first setup left without its intent) is used as it is.
  struct stat control_info{};
  const bool control_missing = lstat(control_path.c_str(), &control_info) != 0 && errno == ENOENT;
  const bool lock_missing = !control_missing &&
      lstat((control_path / "publication.lock").c_str(), &control_info) != 0 && errno == ENOENT;
  if (control_missing || (lock_missing && current->state != PreparationState::kReady) ||
      (current->journal && current->journal->operation == "settings-control")) {
    auto initialized = InitializeControl(record, transaction, prefix);
    if (initialized.state != PackageActionState::kComplete) return fail(initialized.message);
    current = records_.Find(record.install_id);
    if (!current || !SameRuntimeInstallIdentity(*current, record)) return fail("OWNER changed after control initialization.");
  }
  auto control = platform::ManagedDirectory::Open(control_path, platform::DirectoryPolicy::kPrivate);
  auto lease = control ? platform::ManagedFileLock::Acquire(*control, "publication.lock", platform::LockMode::kPublisher) : std::nullopt;
  if (!lease)
    return fail("The publication lock is unavailable or held (the game may be running).");
  // The loader's reason names the file and the check that refused it: a console start once
  // failed here without the log saying which.
  auto recovered = LoadSettingsPublicationOperation(control_path);
  // A Ready game's last operation that no longer loads (written by another build, damaged) is not
  // needed: the committed receipt and the published metadata prove the previous state, as they do
  // when the journal is absent. It is set aside under its own name, never deleted, and the
  // publication goes on as if it were absent. One merely unavailable (a passing error) is not.
  if (recovered.state == PublicationRecoveryState::kCorrupt && current->state == PreparationState::kReady &&
      !(current->journal && current->journal->operation == "settings-publication")) {
    const auto reason = recovered.message;
    const auto verified = VerifyCommittedPublicationReceipt(*current, prefix);
    if (verified.state != PackageActionState::kComplete)
      return fail("Existing frozen operation is corrupt or unavailable: " + reason + " (kept: " + verified.message + ")");
    if (!lease->Validate(*control, "publication.lock")) return fail("The publication lock changed before the journal was set aside.");
    auto aside = SetAsideSettingsPublicationOperation(control_path, time);
    if (!aside.ok())
      return fail("Existing frozen operation is corrupt or unavailable: " + reason + " (not set aside: " + aside.message() + ")");
    if (log_) log_("settings operation set aside install=" + current->install_id + " as " + aside.value() + ": " + reason);
    recovered = LoadSettingsPublicationOperation(control_path);
  }
  if (recovered.state == PublicationRecoveryState::kCorrupt || recovered.state == PublicationRecoveryState::kUnavailable)
    return fail("Existing frozen operation is corrupt or unavailable: " + recovered.message);
  auto intent = LoadPreparedSettingsPublicationOperation(control_path, transaction);
  if (intent.state == PublicationRecoveryState::kCorrupt || intent.state == PublicationRecoveryState::kUnavailable)
    return fail("Prepared intent is corrupt or unavailable; preserve its evidence: " + intent.message);
  if (intent.operation) {
    if (recovered.operation && recovered.operation->transaction_id == transaction) {
      std::string reason;
      if (EncodeSettingsPublicationOperation(*recovered.operation, &reason) != EncodeSettingsPublicationOperation(*intent.operation, &reason))
        return fail("Active journal and prepared intent disagree.");
    }
    if (current->journal && current->journal->operation == "settings-publication" && current->journal->operation_id != transaction)
      return fail("A different blocked OWNER operation must first be reconciled.");
    if (recovered.operation && recovered.operation->transaction_id != transaction &&
        !ReadyCaptureAllowed(*current, transaction))
      return fail("Previous active operation must first reconcile to durable Ready.");
    recovered = intent;
  }
  if (recovered.operation && recovered.operation->transaction_id == transaction) {
    const auto& frozen = *recovered.operation;
    if (!SameRuntimeInstallIdentity(frozen.install, record) || record.settings_revision != frozen.plan.settings_revision)
      return fail("Recovery OWNER intent changed.");
    auto candidate = frozen; candidate.plan = plan; candidate.kit_id = kit; std::string reason;
    auto checked = FreezeSettingsPublicationOperation(candidate, &reason);
    if (!checked || checked->plan_sha256 != frozen.plan_sha256 || !lease->Validate(*control, "publication.lock"))
      return fail("Recovery settings intent changed; retain the original frozen operation.");
    return Result<SettingsPublicationOperation>::Success(frozen);
  }
  if (current->journal && current->journal->operation == "settings-publication")
    return fail("Unreconciled OWNER intent requires explicit recovery before another operation capture.");
  if (recovered.operation && (!ReadyCaptureAllowed(*current, transaction) ||
      current->settings_revision != recovered.operation->plan.settings_revision))
    return fail("Previous frozen operation must first reconcile to durable Ready.");
  if (!lease->Validate(*control, "publication.lock"))
    return fail("The publication lock changed before capture.");
  // The requested revision and program (RuntimeHost::RepublishSettings may switch it); the
  // rest is the durable record's.
  auto snapshot = *current; snapshot.settings_revision = record.settings_revision;
  CommitLaunch(&snapshot, record);
  const auto moved = GameRootMoveOf(*current);
  auto captured = CaptureSettingsPublicationOperation(snapshot, plan, kit, transaction, time, lease->fd(), prefix,
                                                      moved ? &*moved : nullptr, unowned_game_roots_);
  if (!captured.ok()) return captured;
  if (!lease->Validate(*control, "publication.lock"))
    return fail("The publication lock changed before the capture was saved.");
  auto saved = CreatePreparedSettingsPublicationOperation(control_path, captured.value());
  if (!saved.ok()) return fail("Durable prepared intent failed; reload before any publication effects.");
  if (!lease->Validate(*control, "publication.lock"))
    return fail("The publication lock changed after the capture was saved.");
  return captured;
}
PackageActionResult SettingsPublicationOwner::Resume(const RuntimeInstall& record, const std::filesystem::path& prefix) {
  if (!prefix.empty() && (!prefix.is_absolute() || prefix != prefix.lexically_normal()))
    return Failure("publication-recovery-invalid", "Invalid recovery namespace.");
  auto current = records_.Find(record.install_id);
  if (!current || !SameRuntimeInstallIdentity(*current, record) || current->settings_revision != record.settings_revision ||
      (current->state != PreparationState::kPreparing && current->state != PreparationState::kRegistering) ||
      !current->journal || !record.journal || current->journal->operation != record.journal->operation ||
      current->journal->operation_id != record.journal->operation_id ||
      (current->journal->operation != "settings-control" && current->journal->operation != "settings-publication") ||
      !IsSettingsPublicationTransactionId(current->journal->operation_id))
    return Failure("publication-recovery-owner", "The install record has no matching settings journal to resume.");
  const auto transaction = current->journal->operation_id;
  if (current->journal->operation == "settings-control") {
    auto initialized = InitializeControl(*current, transaction, prefix);
    if (initialized.state != PackageActionState::kComplete) return initialized;
    current = records_.Find(record.install_id);
    if (!current || !SameRuntimeInstallIdentity(*current, record) || current->settings_revision != record.settings_revision ||
        current->state != PreparationState::kPreparing || !current->journal || current->journal->operation != "settings-control" ||
        current->journal->operation_id != transaction || current->journal->step != "control-ready")
      return Failure("publication-recovery-owner", "OWNER changed after setup recovery.");
  }
  auto map = [&](const std::string& path) {
    const auto canonical = std::filesystem::path(path);
    return prefix.empty() ? canonical : prefix / canonical.relative_path();
  };
  std::optional<SettingsPublicationOperation> operation;
  {
    const auto control_path = map(current->data_root + "/.playden-control");
    auto control = platform::ManagedDirectory::Open(control_path, platform::DirectoryPolicy::kPrivate);
    auto lease = control ? platform::ManagedFileLock::Acquire(*control, "publication.lock", platform::LockMode::kPublisher) : std::nullopt;
    if (!lease)
      return Failure("publication-recovery-lock", "The publication lock is unavailable or held (the game may be running).");
    auto active = LoadSettingsPublicationOperation(control_path);
    auto prepared = LoadPreparedSettingsPublicationOperation(control_path, transaction);
    if (active.state == PublicationRecoveryState::kCorrupt || active.state == PublicationRecoveryState::kUnavailable ||
        prepared.state == PublicationRecoveryState::kCorrupt || prepared.state == PublicationRecoveryState::kUnavailable)
      return Failure("publication-recovery-journal", "Recovery journal/intent is corrupt or unavailable; preserve its evidence.");
    if (active.operation && active.operation->transaction_id != transaction)
      return Failure("publication-recovery-conflict", "A different active operation must first be reconciled.");
    std::string reason;
    if (active.operation && prepared.operation &&
        EncodeSettingsPublicationOperation(*active.operation, &reason) != EncodeSettingsPublicationOperation(*prepared.operation, &reason))
      return Failure("publication-recovery-conflict", "Recovery active journal and prepared intent disagree.");
    operation = active.operation ? active.operation : prepared.operation;
    if (!operation)
      return Failure("publication-recovery-intent-missing", "OWNER setup has no valid frozen operation; capture remains unfinished.");
    auto observed = records_.Find(record.install_id);
    if (!SameRuntimeInstallIdentity(operation->install, *current) ||
        (current->settings_revision != operation->previous_revision && current->settings_revision != operation->plan.settings_revision) ||
        !observed || !SameRuntimeInstallIdentity(*observed, *current) || observed->settings_revision != current->settings_revision ||
        observed->state != current->state || !observed->journal || observed->journal->operation != current->journal->operation ||
        observed->journal->operation_id != transaction || observed->journal->step != current->journal->step ||
        !lease->Validate(*control, "publication.lock"))
      return Failure("publication-recovery-owner", "The saved settings operation, the install record or the publication lock changed during recovery.");
  }
  // The coordinator reacquires and revalidates the canonical stable inode and
  // every journal/receipt before effects; the inspection lease grants no future
  // authority after release. No settings/recipe plan is regenerated on recovery.
  const auto profiles = operation->install.data_root + "/library/profiles/";
  const PublicationPaths paths{map(profiles + operation->plan.profile_id + ".profile"), map(profiles + "profiles.lst"),
      map(operation->install.data_root + "/library/input"),
      map("/data/homebrew/" + operation->install.title_id + "/game-launch.json"), map(profiles + ".playden-publication")};
  auto dependencies = Dependencies(prefix, GameRootMoveOf(*current));
  return PublishSettingsOperation(*operation, paths, dependencies);
}
PackageActionResult SettingsPublicationOwner::Persist(const RuntimeInstall& record) {
  auto result = records_.Update(record);
  if (!result.ok) return {PackageActionState::kFailed, result.code.empty() ? "publication-owner-persistence" : result.code,
      result.message.empty() ? "OWNER persistence failed; recover from durable state." : result.message};
  auto observed = records_.Find(record.install_id);
  if (!observed || !SameRuntimeInstallIdentity(*observed, record) || observed->state != record.state ||
      observed->settings_revision != record.settings_revision || observed->last_failure.has_value() != record.last_failure.has_value() ||
      (record.last_failure && (observed->last_failure->code != record.last_failure->code ||
      observed->last_failure->message != record.last_failure->message)) || observed->journal.has_value() != record.journal.has_value() ||
      (record.journal && (observed->journal->operation != record.journal->operation ||
      observed->journal->operation_id != record.journal->operation_id || observed->journal->step != record.journal->step)))
    return Failure("publication-owner-readback", "OWNER persistence readback disagrees with the intended state.");
  return {PackageActionState::kComplete, {}, {}};
}
PackageActionResult SettingsPublicationOwner::Block(const SettingsPublicationOperation& o) {
  std::string reason;
  if (!EncodeSettingsPublicationOperation(o, &reason)) return Failure("publication-invalid", "Frozen OWNER operation is invalid.");
  auto record = records_.Find(o.install.install_id);
  if (!record || !SameRuntimeInstallIdentity(*record, o.install) || record->state == PreparationState::kRemoving ||
      (record->settings_revision != o.previous_revision && record->settings_revision != o.plan.settings_revision))
    return Failure("publication-owner-conflict", "Reserved OWNER identity/revision disagrees with the frozen operation.");
  if (record->journal && record->journal->operation == "settings-request" && record->journal->operation_id != o.transaction_id)
    return Failure("publication-owner-conflict", "A different requested operation must first be reconciled.");
  if (record->journal && record->journal->operation == "settings-publication" && !MatchingJournal(*record, o))
    return Failure("publication-owner-conflict", "A different OWNER publication operation must be reconciled first.");
  // The one package ownership check of an apply: before the journal, receipts or any
  // file change, for a fresh apply and for recovery alike.
  if (!OwnedPackage(package_.InspectOwnership(o.install)))
    return Failure("publication-owner-package", "The title and bottle markers do not name this install.");
  const bool armed = MatchingJournal(*record, o) &&
      (record->journal->step == "register" || record->journal->step == "await-native");
  record->state = armed ? PreparationState::kRegistering : PreparationState::kPreparing;
  if (!armed) record->journal = PreparationJournal{o.transaction_id, "settings-publication", "publish-settings"};
  record->last_failure.reset();
  return Persist(*record);
}
PackageActionResult SettingsPublicationOwner::Reconcile(const SettingsPublicationOperation& o) {
  auto record = records_.Find(o.install.install_id);
  if (!record || !SameRuntimeInstallIdentity(*record, o.install) || !MatchingJournal(*record, o) ||
      (record->settings_revision != o.previous_revision && record->settings_revision != o.plan.settings_revision))
    return Failure("publication-owner-conflict", "Committed operation has no matching blocked OWNER record.");
  // An update follows a Ready title, registered when it was first published: no
  // native query. The first publication decides here whether to register.
  if (!o.previous_receipt_bytes.empty()) {
    record->state = PreparationState::kReady; record->settings_revision = o.plan.settings_revision;
    CommitLaunch(&*record, o.install);
    // Built from the record's install_path: a moved game folder is now the one published.
    record->journal.reset(); record->last_failure.reset(); record->moved_from.clear();
    return Persist(*record);
  }
  auto status = native_.QueryOwnership(o.install);
  auto validate = [&](const NativeStatus& s) {
    return s.available && !s.collision && s.source_available && s.owned_manifest_matches;
  };
  if (!validate(status)) return Failure("publication-native-incoherent", "Native inventory/source ownership is unavailable or incoherent.");
  if (!status.installed) {
    const bool armed = record->journal->step == "register" || record->journal->step == "await-native";
    if (status.scan_pending || armed) {
      if (!armed) {
        record->state = PreparationState::kRegistering; record->journal->step = "await-native";
        auto persisted = Persist(*record); if (persisted.state != PackageActionState::kComplete) return persisted;
      }
      return Failure("publication-registration-pending", "Native registration is pending or uncertain; query before any repeat submission.");
    }
    record->state = PreparationState::kRegistering; record->journal->step = "register";
    auto persisted = Persist(*record); if (persisted.state != PackageActionState::kComplete) return persisted;
    auto submitted = native_.Register(*record);
    if (submitted.state == RegistrationResultState::kFailed || submitted.state == RegistrationResultState::kStaleHook)
      return {PackageActionState::kFailed, submitted.code.empty() ? "publication-registration-failed" : submitted.code,
          submitted.message.empty() ? "Native registration submission failed; its durable arm is retained." : submitted.message};
    record->journal->step = "await-native";
    persisted = Persist(*record); if (persisted.state != PackageActionState::kComplete) return persisted;
    status = native_.QueryOwnership(o.install);
    if (!validate(status)) return Failure("publication-native-incoherent", "Submitted registration has no coherent owned source/inventory.");
    if (!status.installed) return Failure("publication-registration-pending", "Native registration completion is not yet visible.");
  }
  record->state = PreparationState::kReady; record->settings_revision = o.plan.settings_revision;
  CommitLaunch(&*record, o.install);
  record->journal.reset(); record->last_failure.reset(); record->moved_from.clear();
  return Persist(*record);
}
PackageActionResult SettingsPublicationOwner::AuthorizeRetirement(const SettingsPublicationOperation& previous,
    const SettingsPublicationOperation& next) {
  auto record = records_.Find(previous.install.install_id);
  // Retiring the previous journal needs only that it was committed: the record is
  // Ready at its revision with this update's request armed. Ownership is checked
  // once, by Block, before this apply changes anything.
  // The journal of a game folder that moved names the old folder.
  auto earlier = previous.install;
  if (const auto moved = record ? GameRootMoveOf(*record) : std::nullopt;
      moved && platform::WithoutUsbMountNumber(earlier.install_path) == platform::WithoutUsbMountNumber(moved->from))
    earlier.install_path = moved->to;
  // A game published naming no mount: its drive may have changed places since, with nothing published.
  else if (record && platform::WithoutUsbMountNumber(earlier.install_path) == platform::WithoutUsbMountNumber(record->install_path))
    earlier.install_path = record->install_path;
  if (!record || !SameRuntimeInstallIdentity(*record, earlier) || !SameRuntimeInstallIdentity(*record, next.install) ||
      !ReadyCaptureAllowed(*record, next.transaction_id) ||
      record->settings_revision != previous.plan.settings_revision)
    return Failure("publication-retirement-owner", "Previous operation lacks a coherent durable Ready OWNER record.");
  return {PackageActionState::kComplete, {}, {}};
}
PublicationDependencies SettingsPublicationOwner::Dependencies(const std::filesystem::path& prefix,
                                                               std::optional<GameRootMove> moved) {
  PublicationDependencies deps; deps.host_namespace_prefix = prefix; deps.moved_root = moved;
  deps.activity = [this](const RuntimeInstall& r) { return native_.Activity(r.title_id); };
  deps.block_owner = [this](const auto& o) { return Block(o); };
  deps.preflight = [prefix, moved](const auto& o, const auto& paths, int fd) {
    return PreflightSettingsPublication(o, paths, fd, prefix, moved ? &*moved : nullptr);
  };
  deps.apply_files = [prefix](const auto& o, const auto& paths, int fd) { return ApplyPendingPublicationFiles(o, paths, fd, prefix); };
  deps.verify_files = [prefix](const auto& o, const auto& paths, int fd, bool committed) {
    return VerifySettingsPublicationFiles(o, paths, fd, committed, prefix);
  };
  deps.reconcile_committed = [this](const auto& o) { return Reconcile(o); };
  deps.authorize_retirement = [this](const auto& previous, const auto& next) { return AuthorizeRetirement(previous, next); };
  return deps;
}
}  // namespace playden::runtime
