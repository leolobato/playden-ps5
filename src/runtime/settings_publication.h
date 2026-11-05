#pragma once

#include "runtime/game_settings.h"
#include "runtime/package_installer.h"
#include "runtime/settings_publication_journal.h"

namespace playden::runtime {

// Fills the settings-owned fields of a profile publication from a launch plan: the profile
// id and bytes, the settings revision and every input preset the profile references. The
// recipe identity comes from the snapshot, so stale plans cannot adopt a newer recipe.
// The owner compares plan.app_id/launch_entry to its install record and keeps the
// remaining identity fields (install, title, runtime bundle) and its
// own parser validation (profile_references_validated).
inline void FillProfilePublication(const SettingsLaunchPlan& plan, ProfilePublication* publication) {
  publication->recipe_id = plan.recipe_id;
  publication->recipe_version = plan.recipe_version;
  publication->profile_id = plan.profile_id;
  publication->profile_bytes = plan.profile_bytes;
  publication->settings_revision = plan.settings_revision;
  publication->required_input_presets.clear();
  publication->input_presets.clear();
  for (const auto& [filename, bytes] : plan.input_presets) {
    publication->required_input_presets.push_back(filename);  // "<name>.input", as the installer checks
    publication->input_presets.push_back({filename, bytes});
  }
}

// A game folder that moved as a whole (the 0.6 layout, RuntimeInstall::moved_from): its
// committed publication names `from`, the install record names `to`. The publication that
// replaces it checks the old one with the game folder at `to`.
struct GameRootMove {
  std::string from, to;
};
// The record's move, while it is marked; nothing otherwise.
std::optional<GameRootMove> GameRootMoveOf(const RuntimeInstall& record);
// The committed publication a moved game folder left, checked like the title checks it, except
// the game folder (descriptor game_root, profile paths, dxvk.path), which names move.from and is
// checked at move.to. Profile, catalog and metadata in the private data root are hash-checked.
// With unowned (the publication replacing it is unowned), the folder may have any owner and mode.
PackageActionResult VerifyMovedPublication(const RuntimeInstall& record, const GameRootMove& move,
    const std::string& receipt_bytes, const std::string& metadata_bytes,
    const std::filesystem::path& host_namespace_prefix = {}, bool unowned = false);
// The committed publication a Ready game plays under: its receipt (publication-state.v1) is
// committed, names this install at the record's revision and binds the published metadata
// (.playden-publication), which names the same install. Read-only; the caller holds the
// publisher lease. What the control folder's operation journal is not needed for.
PackageActionResult VerifyCommittedPublicationReceipt(const RuntimeInstall& record,
    const std::filesystem::path& host_namespace_prefix = {});
// Whether the game's committed publication (its .playden-publication) admits the game folder
// unowned (metadata schema 3); nothing when there is none or it can't be read.
// The folder the game's descriptor names now: /mnt/usb/<folder> for a USB game published naming no mount.
std::optional<std::string> PublishedGameRoot(const RuntimeInstall& record, const std::filesystem::path& prefix);
std::optional<bool> PublishedGameRootUnowned(const RuntimeInstall& record,
    const std::filesystem::path& host_namespace_prefix = {});
// The SHA-256 of the profile the game's committed publication wrote (its .playden-publication);
// nothing when there is none or it can't be read.
std::optional<std::string> PublishedProfileDigest(const RuntimeInstall& record,
    const std::filesystem::path& host_namespace_prefix = {});

enum class PublicationStep {
  kOwnerBlocked, kJournalDurable, kPendingDurable, kFilesApplied,
  kPendingVerified, kCommitDurable, kCommitVerified, kPreviousArchived,
};
using PublicationFileAction = std::function<PackageActionResult(
    const SettingsPublicationOperation&, const PublicationPaths&, int borrowed_publisher_fd)>;
struct PublicationDependencies {
  // Test-only canonical namespace prefix. Production passes an empty path.
  std::filesystem::path host_namespace_prefix;
  std::function<NativeActivity(const RuntimeInstall&)> activity;
  // Validates OWNER identity and package ownership (the one inspection of an
  // apply) and durably blocks new readiness. Called only under publisher
  // ownership, before the operation/receipt/file effects.
  std::function<PackageActionResult(const SettingsPublicationOperation&)> block_owner;
  // Read-only ownership/collision preflight. Never writes directories/settings.
  PublicationFileAction preflight;
  // Creates/writes the complete set, with per-file/parent durability. Called
  // only after the exact frozen pending receipt is durably established.
  PublicationFileAction apply_files;
  // Must use shared native semantics, with exact phase bytes and metadata hash.
  std::function<PackageActionResult(const SettingsPublicationOperation&, const PublicationPaths&,
                                   int borrowed_publisher_fd, bool committed)> verify_files;
  // Reconciles publication with OWNER persistence/registration gates. Initial
  // install still needs registration; no callback may restage the bottle/title.
  std::function<PackageActionResult(const SettingsPublicationOperation&)> reconcile_committed;
  // Read-only check that the different journal being retired was committed (the
  // record is durably Ready at its revision). Missing callback refuses retirement.
  std::function<PackageActionResult(const SettingsPublicationOperation& previous,
                                   const SettingsPublicationOperation& next)> authorize_retirement;
  // Set while the record is marked moved: the journal being retired then names the old game
  // folder and is checked with VerifyMovedPublication instead of verify_files.
  std::optional<GameRootMove> moved_root;
  // Optional diagnostic fault seam after durable effects. False stops the call;
  // caller must reconstruct/reload. Production leaves this empty.
  std::function<bool(PublicationStep)> after_step;
};
// The control directory and stable private lock must already exist. Unknown or
// active native activity never mutates state. Borrowed descriptors must never be
// closed/converted by callbacks. Readiness requires complete committed semantic
// verification and OWNER reconciliation; a failed operation preserves evidence.
PackageActionResult PublishSettingsOperation(const SettingsPublicationOperation& operation,
    const PublicationPaths& paths, PublicationDependencies& dependencies);
// Concrete shared native verification adapter; does not acquire/change the
// borrowed lock or authorize file mutations. Prefix is test-only, empty on PS5.
PackageActionResult VerifySettingsPublicationFiles(const SettingsPublicationOperation& operation,
    const PublicationPaths& paths, int borrowed_publisher_fd, bool committed,
    const std::filesystem::path& host_namespace_prefix = {});
// Registry effect for the production apply adapter. Requires this exact durable
// journal/pending receipt and the caller's exclusive publisher lease. Edits are
// regenerated from the immutable backup and frozen timestamp. Before-or-intended
// bytes only; collision/failure preserves evidence, with no immediate rollback.
PackageActionResult ApplyPendingPublicationRegistry(const SettingsPublicationOperation& operation,
    int borrowed_publisher_fd, const std::filesystem::path& host_namespace_prefix = {});
// Game working-directory DXVK effect under the same pending operation. Preserves
// the captured mode, admits original/intended bytes only, and syncs absence on
// removal. Does not create game directories or roll back earlier effects.
PackageActionResult ApplyPendingPublicationDxvk(const SettingsPublicationOperation& operation,
    int borrowed_publisher_fd, const std::filesystem::path& host_namespace_prefix = {});
// Complete pending file effects. Requires independently approved OWNER/preflight
// and stopped-title status from the coordinator. No bottle/title restaging.
// after_effect is a test-only fault seam; production leaves it empty.
PackageActionResult ApplyPendingPublicationFiles(const SettingsPublicationOperation& operation,
    const PublicationPaths& paths, int borrowed_publisher_fd,
    const std::filesystem::path& host_namespace_prefix = {},
    const std::function<bool(const std::filesystem::path&)>& after_effect = {});
// Read-only preflight under the caller's exclusive lease. Updates verify the exact
// prior committed complete set; live dxvk.conf and user.reg must equal their frozen
// before-snapshots. Leftover files at new destinations are not refused (the apply
// writes over them). Never writes.
// With moved, a previous publication that names moved->from is checked with VerifyMovedPublication;
// so is an owned (schema 2) previous publication that an unowned one replaces, in place.
PackageActionResult PreflightSettingsPublication(const SettingsPublicationOperation& operation,
    const PublicationPaths& paths, int borrowed_publisher_fd,
    const std::filesystem::path& host_namespace_prefix = {}, const GameRootMove* moved = nullptr);
// Builds and freezes one operation from the exact supplied plan and live before
// snapshots. Caller independently approves reserved OWNER/package provenance,
// positively stopped activity and the borrowed exclusive lease. Control/lock
// already exist. Only immutable backup preparation may write; no live edits.
// unowned_game_roots: the kit has the game-root-unowned capability, so every game folder is
// published unowned (metadata schema 3): users may add or change files in it (mods).
// Without it, only a folder on a drive that records no owners (exFAT) is.
Result<SettingsPublicationOperation> CaptureSettingsPublicationOperation(
    const RuntimeInstall& record, const SettingsLaunchPlan& plan, const std::string& kit_id,
    const std::string& transaction_id, std::int64_t registry_unix_time,
    int borrowed_publisher_fd, const std::filesystem::path& host_namespace_prefix = {},
    const GameRootMove* moved = nullptr, bool unowned_game_roots = false);
}  // namespace playden::runtime
