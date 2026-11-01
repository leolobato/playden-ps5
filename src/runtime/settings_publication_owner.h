#pragma once
#include "runtime/settings_publication.h"
namespace playden::runtime {
// Binds publication callbacks to durable OWNER persistence, immutable package
// ownership and native registration. Use through the coordinator so callbacks
// run under its qualified publisher lease and committed semantic verification.
// Dependencies capture this object; it and its collaborators must outlive them.
enum class PublicationControlStep { kOwnerArmed, kDirectorySynced, kLockSynced, kOwnerPrepared };
class SettingsPublicationOwner {
 public:
  // unowned_game_roots: the admitted kit has game-root-unowned; captures publish every game
  // folder unowned (CaptureSettingsPublicationOperation). host_namespace_prefix: where Request
  // reads the committed publication (a same-revision update only replaces an owned one).
  SettingsPublicationOwner(InstallRecordStore& records, PackageInstaller& package, NativeCoordinator& native,
                           bool unowned_game_roots = false, std::filesystem::path host_namespace_prefix = {});
  // Persist the exact requested plan in the existing
  // private OWNER request directory, then arm OWNER before control/capture.
  // Pre-arm orphans never resume implicitly. Ready updates retain their current
  // revision and gain a capture-update journal that blocks launch until commit.
  PackageActionResult Request(const SettingsPublicationRequest& request,
      const std::filesystem::path& request_directory);
  PackageActionResult ResumeRequested(const RuntimeInstall& record,
      const std::filesystem::path& request_directory,
      const std::filesystem::path& host_namespace_prefix = {});
  // Drops a Ready update's request that never reached its publication: the record's journal is
  // still settings-request/capture-update, so Block never ran and the committed publication (its
  // receipt names another transaction, at the record's revision) is untouched. Under the
  // publisher lease, so a running game (holding it shared) refuses. The record returns to Ready
  // with no journal; the saved request is removed when it can be (a leftover otherwise). A
  // capture's files of this transaction (its registry backup, its prepared intent) are removed
  // too, best effort (logged); no later capture reads them. Other transactions' files stay.
  PackageActionResult DiscardRequested(const RuntimeInstall& record,
      const std::filesystem::path& request_directory,
      const std::filesystem::path& host_namespace_prefix = {});
  // Creates (or adopts) only the private control directory and stable empty lock,
  // after a durable initial OWNER intent. The data root must already exist. A
  // matching interrupted intent resumes; symbolic links and malformed locks refuse.
  // after_step is a test-only crash seam; production leaves it empty.
  PackageActionResult InitializeControl(const RuntimeInstall& record, const std::string& transaction_id,
      const std::filesystem::path& host_namespace_prefix = {},
      const std::function<bool(PublicationControlStep)>& after_step = {});
  // Lines worth the runtime log (an unreadable journal set aside); nothing by default.
  void SetLog(std::function<void(const std::string&)> log) { log_ = std::move(log); }
  // The kit this publication is for. Two installed versions can disagree about unowned game folders.
  void SetUnownedGameRoots(bool unowned) { unowned_game_roots_ = unowned; }
  Result<SettingsPublicationOperation> Prepare(const RuntimeInstall& record, const SettingsLaunchPlan& plan,
      const std::string& kit_id, const std::string& transaction_id, std::int64_t registry_unix_time,
      const std::filesystem::path& host_namespace_prefix = {});
  // Routes a persisted settings-control/settings-publication UUID to its exact
  // frozen active/prepared operation. No plan/recipe regeneration. The complete
  // coordinator rechecks ownership/receipt and reconciles durable Ready.
  PackageActionResult Resume(const RuntimeInstall& record,
      const std::filesystem::path& host_namespace_prefix = {});
  // With moved (the record's GameRootMoveOf), the previous publication may name the old game folder.
  PublicationDependencies Dependencies(const std::filesystem::path& host_namespace_prefix = {},
                                       std::optional<GameRootMove> moved = std::nullopt);
 private:
  PackageActionResult Block(const SettingsPublicationOperation& operation);
  PackageActionResult Reconcile(const SettingsPublicationOperation& operation);
  PackageActionResult AuthorizeRetirement(const SettingsPublicationOperation& previous,
                                         const SettingsPublicationOperation& next);
  PackageActionResult Persist(const RuntimeInstall& record);
  InstallRecordStore& records_;
  PackageInstaller& package_;
  NativeCoordinator& native_;
  bool unowned_game_roots_ = false;
  std::filesystem::path host_namespace_prefix_;
  std::function<void(const std::string&)> log_;
};
}  // namespace playden::runtime
