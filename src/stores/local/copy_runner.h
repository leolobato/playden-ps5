#pragma once
// "Copy into Playden": the install service's runner for imported games (Store::kLocal). An
// imported game is copied from its drive folder into <games_root>/local-<id> on the chosen volume
// and becomes a managed install there; the drive folder is never touched. A drop-folder game's
// folder (/data/playden-import/<name>) is deleted once its copy is committed, so the game isn't
// stored twice (files changed since they were copied stay); until then it plays from the drop folder.
//
// Flow: Enqueue (on the Local source's worker) walks the folder into a copy plan, saves it in the
// destination (copy_plan.h), sets the entry copy_queued and queues the job with the plan's total
// (the service's free-space check). Install (on the service's worker) sets it copying, checks free
// space again, copies the files in plan order (.playden-copy/<index>.part, fsync, rename; a resume
// skips files already at their planned size), verifies every file and the program, has the title
// follow the copy (files changed in the drive folder meanwhile are copied again), writes the
// install record and sets the entry copied. Remove (cancel or uninstall) deletes the folder and moves the entry
// back to in_place (a copy) or to the removed list (a copied game).
//
// A game already prepared to play from its folder keeps its PS5 title, runtime number and bottle:
// once the copy is verified, and before the install record makes it a managed install, its title
// is relocated to the copy (LocalCopyOptions::relocate). While its title runs the job waits and
// tries again; the drive folder stays the active one until the relocation commits.
//
// "Move into Playden" (Move) is for a drop-folder game, on the same file system as Playden's games
// folder: one rename(2) as root of /data/playden-import/<name> to <games_root>/local-<id>, done by
// the relocation under the title's publication lease (a game without a title at once), then the
// install record and the file list, the install service lists it (InstallService::Adopt) and the
// entry becomes copied. The entry says copying meanwhile, so a restart finishes it (Move again).
//
// Failures that a Resume can't fix (the source changed, the program is missing, the plan is
// gone) delete the partial copy and set the entry back to in_place with the failure; the job then
// waits in Downloads as failed until cancelled. Others (no space, a read or write error) keep the
// partial copy and the entry copying, so Resume carries on.
#include "installs/install_service.h"
#include "steam/install_volumes.h"
#include "stores/local/copy_plan.h"
#include "stores/local/import_store.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>

namespace playden::stores::local {

inline constexpr const char* kCopyChangedFailure = "The game's files changed while copying. Copy it again.";
inline constexpr const char* kCopyStoppedFailure = "The copy stopped. Copy it again.";

inline constexpr const char* kMoveStoppedFailure = "The move stopped. Move it again.";

// What LocalCopyOptions::relocate is asked: the game's folder in Playden becomes the one it plays from.
struct LocalRelocation {
  GameId game;
  std::uint32_t runtime_number = 0;  // 0: none yet (no PS5 title)
  std::string root;                  // the folder in Playden, as this process opens it
  std::string volume_id;             // the install volume of `root`; empty: the game's copy job's
  // Run once the game's title is stopped: the move's rename, or for a copy, copying again the files
  // that changed since they were copied. Empty, or why not. Safe to run again after it succeeded.
  std::function<std::string()> move_files;
};
struct LocalRelocationResult {
  enum class Outcome { kDone, kBusy, kFailed };
  Outcome outcome = Outcome::kDone;
  std::string message;
};

struct LocalCopyOptions {
  std::function<void(const std::string&)> log;
  // Free space of the drive holding a folder; empty: steam::QueryVolumeSpace (it leaves out the
  // console storage reserve).
  std::function<std::optional<steam::VolumeSpace>(const std::string& path)> space;
  std::size_t buffer_bytes = 1024 * 1024;
  // Moves the game's PS5 title to its folder in Playden (RuntimeHost::Relocate): after a copy is
  // verified, and for a move; both run move_files. Empty: no game has a title (move_files runs at
  // once).
  std::function<LocalRelocationResult(const LocalRelocation&)> relocate;
  // Runs work with root raised (ps5::WithRoot): the move's rename of a root-owned drop folder.
  // Empty: as the app user.
  std::function<bool(const std::function<bool()>& work)> as_root;
  // Tests: called once the copy is a complete managed install (record written), before
  // imports.json says `copied`. False stops there, as a crash would: the entry stays `copying` and
  // the next start's reconcile (LocalLibrarySource::Reconcile) finishes it.
  std::function<bool(const GameId& game, const std::string& root)> before_finish;
  // Tests: after each file reaches its final name.
  std::function<void(const std::string& path)> after_file;
};

class LocalCopyRunner final : public installs::InstallRunner {
 public:
  explicit LocalCopyRunner(ImportStore& store, LocalCopyOptions options = {});

  // Called after the runner changed imports.json, so the Local source shows it
  // (LocalLibrarySource::Reload). May be set while the service runs.
  void SetChanged(std::function<void()> changed);

  // Queues the copy of `entry` (in_place) from `source` (its folder, as this process opens it) to
  // `volume_id` (empty: the service's default volume). `runtime_number` goes into the install
  // record. Empty, or a sentence for the player; on failure nothing is left behind and the entry
  // stays in_place (a free-space refusal records its failure on the entry).
  std::string Enqueue(installs::InstallService& service, const ImportEntry& entry, const std::string& source,
                      const std::string& volume_id, std::uint32_t runtime_number);

  // "Move into Playden" of a drop-folder entry (in_place, or copying after an interrupted move)
  // from `source` (its folder, as this process opens it) into the primary volume's games folder.
  // `runtime_number` goes into the install record. Runs on the caller's thread (the Local source's
  // worker) and blocks for the relocation. Empty, or a sentence for the player; a refusal before
  // the rename leaves the entry in_place, one after it leaves it copying for a retry.
  std::string Move(installs::InstallService& service, const ImportEntry& entry, const std::string& source,
                   std::uint32_t runtime_number);

  // Copies ignore the install options.
  Result Install(const GameId& id, const std::string& root, const installs::InstallOptions& options,
                 const std::atomic<bool>& cancel,
                 const std::function<void(std::uint64_t done, std::uint64_t total, std::uint64_t held)>& progress,
                 const std::function<void(installs::InstallStep)>& step) override;
  std::string Remove(const std::string& root) override;
  void Forget(const GameId& id) override;

 private:
  // Reads the imports if nobody has yet (removals run from the install worker, which can start
  // first). False while they can't be read.
  bool StoreLoaded();
  LocalRelocationResult Relocate(const LocalRelocation& relocation);
  // Before the title follows the copy (LocalRelocation::move_files): every planned file the source
  // has changed since it was copied is planned again as it is now (the plan saved) and copied again.
  // A file gone from the source stays as copied. Empty, or why not.
  std::string CopyChangedFiles(const std::string& id, const std::string& root, CopyPlan& plan);
  // After a drop-folder game's copy (in `root`) committed: the copy's folders are synced, then its
  // drop-folder files (those the copy took, unchanged since) are deleted. Empty, or the sentence
  // kept on the entry when some stay.
  std::string RemoveDropFolderSource(const ImportEntry& entry, const CopyPlan& plan, const std::string& root);
  Result Abandon(const ImportEntry& entry, const std::string& root, const std::string& failure);
  void Changed();
  void Log(const std::string& line) const;

  ImportStore& store_;
  LocalCopyOptions options_;
  std::mutex changed_mutex_;
  std::function<void()> changed_;
};

}  // namespace playden::stores::local
