#pragma once
// The Local store: Windows games the player added from a drive folder or the drop folder
// (imports.json), and the folders on the attached drives that could be added (candidates). A
// worker owns every file access: imports.json, the import folder scans and the drive checks; the
// presentation thread reads snapshots.
#include "installs/installed_location.h"
#include "stores/library_source.h"
#include "stores/local/drop_folder_access.h"
#include "stores/local/import_store.h"
#include "stores/local/scanner.h"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace playden::stores::local {

// Where games to import are looked for on one install volume: `folder` under `mount`.
struct ImportVolume {
  std::string volume;  // install volume id ("internal", "usb0", ...)
  std::string mount;   // console path of the drive ("/mnt/usb0"; the drop folder's is "/data")
  std::string folder;  // the import folder under it ("playden/import", "playden-import")
  // An external drive: there only while something is mounted on `mount` (the console keeps
  // empty /mnt/usb* folders without a drive).
  bool require_mount_point = false;
};
// ImportRoots() as volumes: the drop folder, then the external drives.
std::vector<ImportVolume> ConsoleImportVolumes();

// A folder in an import folder that could be added.
struct ImportCandidate {
  std::string volume;
  std::string folder;  // relative to the volume's mount, as imports.json names it
  std::string path;    // console path
  std::string title;
  std::vector<Program> programs;  // ranked: the likely game first; helpers flagged
  std::uint64_t total_bytes = 0;  // estimate (scanner.h)
  std::uint64_t file_count = 0;
  bool ascii_ok = false;  // printable-ASCII path: can play in place
  bool partial = false;   // the scan stopped at a limit or a depth
  ScanStatus status = ScanStatus::kOk;
  std::string imported_id;  // already in the library as this game; empty when not
};

// The outcome of an Add or Remove.
struct ImportOutcome {
  std::uint64_t request = 0;
  bool ok = false;
  std::string id;       // the game's import ID
  std::string message;  // a sentence for the player or the log when it failed
};

struct LocalSnapshot {
  std::uint64_t revision = 0;        // anything below changed
  std::uint64_t games_revision = 0;  // the imported games changed: the library reloads them
  bool loaded = false;
  std::string load_error;  // imports.json could not be read
  std::vector<ImportEntry> games;
  std::set<std::string> present_volumes;
  std::vector<ImportCandidate> candidates;
  bool scanning = false;  // a look for games is queued or running
  // Where each game playing in place is, for the controller (external locations).
  std::vector<installs::InstalledLocation> locations;
  std::vector<ImportOutcome> outcomes;  // the latest ones, oldest first (at most kMaxOutcomes)
};

struct LocalLibraryConfig {
  std::vector<ImportVolume> volumes = ConsoleImportVolumes();
  // Prepended to every path the worker opens (a host test's namespace); never to the paths it reports.
  std::string host_prefix;
  // Whether a volume's drive is there now; empty: its mount is a folder (and a mount point when required).
  std::function<bool(const ImportVolume&)> present;
  std::function<void(const std::string&)> log;
  // Opens the files of a drop-folder game (drop_folder_access.h) with the console path of its
  // folder, at Add and before each Play; the empty string or a sentence naming the folder. Empty:
  // drop-folder games play as they are (host builds, whose files are the user's own).
  DropFolderOpener open_drop_folder;
  // Queues "Copy into Playden" for an in_place entry from `source` (its folder, as this process
  // opens it) to an install volume (empty: the default); LocalCopyRunner::Enqueue. The empty
  // string, or a sentence for the player. Empty: games can't be copied.
  std::function<std::string(const ImportEntry& entry, const std::string& source, const std::string& volume_id,
                            std::uint32_t runtime_number)>
      copy_into_playden;
  // "Move into Playden" of a drop-folder entry from `source` (its folder, as this process opens it);
  // LocalCopyRunner::Move. Blocks the worker for the rename and its PS5 title's relocation. The
  // empty string, or a sentence for the player. Empty: games can't be moved.
  std::function<std::string(const ImportEntry& entry, const std::string& source, std::uint32_t runtime_number)>
      move_into_playden;
  // Deletes a copy's folder (<games_root>/local-<id>, holding a copy plan) that no copy owns any
  // more; LocalCopyRunner::Remove, which only deletes what Playden recognizes as its own. Reconcile
  // calls it. Empty: such folders stay.
  std::function<bool(const std::string& root)> remove_copy_folder;
  // The program that starts the game in place of `program` (relative to `folder`, as this process opens it), or
  // empty to keep it: an Unreal root launcher only starts the real program as a child process, which the console
  // cannot do (runtime::UnrealGameProgram). Called on the worker at Add. Empty: the chosen program is kept.
  std::function<std::string(const std::string& folder, const std::string& program)> game_program;
};

// The words for a game that can't play from its folder because of its name (the descriptor rule).
inline constexpr const char* kNotAsciiReason =
    "The game's folder name has characters the PS5 can't play from. Copy the game into Playden instead.";

class LocalLibrarySource final : public LibrarySource {
 public:
  static constexpr std::size_t kMaxOutcomes = 16;

  // `store` outlives the source. The worker starts at once: it loads imports.json, checks the
  // drives and looks for games.
  LocalLibrarySource(ImportStore& store, LocalLibraryConfig config = {});
  ~LocalLibrarySource() override;
  LocalLibrarySource(const LocalLibrarySource&) = delete;
  LocalLibrarySource& operator=(const LocalLibrarySource&) = delete;

  Store store() const override { return Store::kLocal; }
  // The imported games (a game on an absent drive stays listed).
  std::uint64_t Refresh(std::uint64_t account, std::uint64_t generation) override;
  std::uint64_t LoadCached(std::uint64_t account, std::uint64_t generation) override { return Refresh(account, generation); }
  // Imported games belong to the console, not to an account: nothing is deleted.
  std::uint64_t Clear(std::uint64_t account, std::uint64_t generation) override;
  void Invalidate(std::uint64_t) override {}
  void Shutdown() override;
  std::vector<LibraryEvent> Pump(std::size_t max_events) override;

  // "Look for games": the drives are checked and every import folder is scanned again.
  std::uint64_t LookForGames();
  // The drives are checked; when one came or went, the import folders are scanned again. A check
  // already waiting makes this a no-op.
  void CheckVolumes();
  // Adds a folder to the library, playing in place with `program` (relative to the folder). `folder`
  // is relative to the volume's mount, or a bare name in the volume's import folder ("Delores" on
  // usb0 is playden/import/Delores). The title comes from the folder's scan.
  std::uint64_t Add(const std::string& volume, const std::string& folder, const std::string& program);
  // Takes the game out of the library; its files stay where they are.
  std::uint64_t Remove(const std::string& id);
  // Before a Play or Prepare: a drop-folder game's files are opened again (FTP may have added
  // root-owned ones, or an earlier walk stopped). The outcome says whether the game may go on;
  // a game anywhere else is ok at once.
  std::uint64_t OpenForPlay(const std::string& id);
  // "Copy into Playden": the game's folder is planned and its copy queued on `volume_id` (empty:
  // the install service's default). `runtime_number` (the runtime's number for the game, 0 when
  // unknown) goes into the copy's install record; a game already set up to play from its folder
  // keeps its PS5 title, which follows the copy once it is verified. The outcome says whether it
  // was queued.
  std::uint64_t CopyIntoPlayden(const std::string& id, const std::string& volume_id = {},
                                std::uint32_t runtime_number = 0);
  // "Move into Playden": a drop-folder game's folder is renamed into Playden's games folder (its
  // PS5 title, if any, follows). The outcome says whether it moved; a move a restart or a running
  // title interrupted after the rename is finished by asking again.
  std::uint64_t MoveIntoPlayden(const std::string& id, std::uint32_t runtime_number = 0);
  // imports.json changed outside the source (the copy runner): the games are read again.
  void Reload();
  // After a restart, against the install service (`installs` and `cancel_job` are called on the
  // worker, in order with copies being queued): a copy that finished as a managed install but whose
  // entry still says copy_queued/copying becomes copied; one the service has neither as a job nor
  // as an install goes back to in_place, unless it is a move whose rename happened (its drop
  // folder is gone): that one is finished (move_into_playden, with `runtime_number`'s number); and
  // a copy job whose game isn't being copied (back in place, or gone) is cancelled, which removes
  // its partial folder. Last, a copy plan folder in an available games folder that no copy owns (a
  // stop between saving the plan and queuing its job; a game back in place, or gone) is removed
  // (LocalLibraryConfig::remove_copy_folder).
  std::uint64_t Reconcile(std::function<installs::InstallSnapshot()> installs,
                          std::function<void(const GameId&)> cancel_job,
                          std::function<std::uint32_t(const GameId&)> runtime_number = {});

  // The runtime's number for a game, which may wait for the runtime worker: called on this store's
  // worker for every game it adds and for every game at Reconcile, so the number is known (and the
  // runtime's cache warm) before the main thread, which must never wait, asks for it.
  void SetRuntimeNumber(std::function<std::uint32_t(const GameId&)> runtime_number);

  std::shared_ptr<const LocalSnapshot> Snapshot() const;
  // Cheap: a copy of the last projection.
  std::vector<installs::InstalledLocation> Locations() const;

 private:
  enum class Command { kList, kClear, kScan, kCheckVolumes, kAdd, kRemove, kOpen, kCopy, kMove, kReload, kReconcile };
  struct Job {
    Command command = Command::kList;
    std::uint64_t request = 0;
    std::uint64_t account = 0;
    std::uint64_t generation = 0;
    std::string volume, folder, program, id;
    std::uint32_t runtime_number = 0;
    std::function<installs::InstallSnapshot()> installs;  // kReconcile
    std::function<void(const GameId&)> cancel_job;
    std::function<std::uint32_t(const GameId&)> number_for;
  };
  std::uint64_t Submit(Job job);
  void Run();
  void Execute(const Job& job);
  void WarmRuntimeNumber(const std::function<std::uint32_t(const GameId&)>& given, const std::string& id);
  void EnsureLoaded();
  bool CheckPresence();  // true when the present volumes changed
  void Scan();
  void Project();
  void Outcome(const Job& job, bool ok, std::string id, std::string message);
  const ImportVolume* Volume(const std::string& id) const;
  std::string HostPath(const std::string& console_path) const;
  // The empty string, or why the drop-folder game at `path` (a console path) can't be played.
  std::string OpenDropFolder(const ImportVolume& volume, const std::string& path);
  void Log(const std::string& line) const;

  ImportStore& store_;
  LocalLibraryConfig config_;
  // Worker-only.
  bool load_tried_ = false;
  std::string load_error_;
  std::set<std::string> present_;
  std::vector<ImportCandidate> candidates_;
  std::vector<ImportOutcome> outcomes_;
  std::uint64_t games_revision_ = 0;

  std::function<std::uint32_t(const GameId&)> runtime_number_;  // guarded by mutex_
  mutable std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<Job> pending_;
  std::deque<LibraryEvent> events_;
  std::shared_ptr<const LocalSnapshot> snapshot_;
  std::uint64_t request_ = 0;
  bool scan_waiting_ = false;
  bool check_waiting_ = false;
  bool stopping_ = false;
  std::thread worker_;
};

}  // namespace playden::stores::local
