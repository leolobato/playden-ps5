#pragma once

#include "installs/install_record.h"
#include "library/store.h"
#include "steam/backend/app_info.h"
#include "steam/backend/cdn_client.h"
#include "steam/backend/cm_client.h"
#include "steam/backend/depot_manifest.h"
#include "steam/session_store.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace playden::steam {

enum class InstallError {
  kNone,
  kCanceled,
  kNoSession,
  kRejected,   // Steam refused a depot key or manifest code
  kNetwork,    // retryable transport trouble
  kCorrupt,    // a chunk or manifest failed verification
  kNoSpace,
  kStorage,    // a filesystem operation failed
  kProtocol,
  kGameRunning,  // an update's files can't go in place now (may_apply said no): nothing changed, try later
};
const char* InstallErrorName(InstallError error);

struct InstallTarget {
  // The install record's "app_id": a Steam game's app ID, or the runtime number of a game from
  // another store (as the kept marker holds), which `game` then names.
  std::uint32_t app_id = 0;
  // Written as "store"/"game" in the install record only when not Steam; default: Steam app_id.
  GameId game;
  std::string install_root;          // absolute directory owned by this install
  std::vector<DepotInfo> depots;     // selected depots, in Steam's listing (mounting) order
  // Written to the install record: the branch the depots' manifests come from and the content
  // language they resolved to. Empty: "public" and "english".
  std::string branch;
  std::string language;
  // An update of the finished install at install_root: only files whose content changed are downloaded,
  // into <root>/.playden-update/staging/ with their own journal, while the game stays as it was. Once
  // everything is there they replace the game's files, files the new version dropped are deleted, and
  // the file tree, file list and record are written for the new version (see FinishUpdate).
  bool update = false;
  // Update only, optional: called once every changed file is downloaded, before any of the game's files
  // change, with the folder whose files go in place with the record (a ".playden-steam" there replaces
  // the game's) and the depots the account was refused. False fails the update.
  std::function<bool(const std::string& folder, const std::vector<std::uint32_t>& skipped)> before_apply;
  // Update only, optional: asked right before the game's files start to change (also before an update
  // that stopped while putting them in place carries on). False (the game runs): the update stops with
  // kGameRunning, its downloads kept and the game's files untouched since the last call.
  std::function<bool()> may_apply;
};

// What an install is doing, for the progress reports: an update first checks the installed files (by
// hashing them when the install kept no file tree), then downloads, then applies the changed files.
enum class InstallPhase { kDownloading, kCheckingFiles, kApplying };

struct InstallProgress {
  std::uint64_t download_total = 0;  // compressed bytes of every chunk to write
  std::uint64_t download_done = 0;   // includes chunks already present from a previous run
  std::uint64_t install_total = 0;   // file bytes after merging depots
  std::uint32_t files_total = 0;
  // Bytes of install_total this install already occupies on disk: preallocated files
  // (exFAT allocates on ftruncate) or written chunks (sparse filesystems).
  std::uint64_t held_bytes = 0;
  // Display name of the server chunks come from (CdnServerDisplayName, never a host); empty while
  // the Fastest race runs.
  std::string server_name;
  InstallPhase phase = InstallPhase::kDownloading;
};

struct DownloadTuning {
  int slots = 4;
  int ahead_files = 256;
  std::string preferred_server;  // empty uses Steam's order
  bool fastest = false;          // race the servers at download start and keep checking (preferred_server is unused)
};

// The download server choice that races the servers (DownloadTuning::fastest), as preferences store it.
inline constexpr const char* kFastestDownloadServer = "fastest";

// A missing or malformed file uses defaults; valid slot counts are clamped to 1..8. "server=fastest"
// sets `fastest`.
DownloadTuning ReadDownloadTuning(const std::string& path);

// Manifest and chunk transfers, including failed and corrupt replies.
struct InstallServerStats {
  std::string label;  // fixed provider label, never a host
  std::size_t index = 0;
  std::uint64_t requests = 0, failures = 0, bytes = 0, fetch_ms = 0;
};

// Wall-clock phases and summed stages; worker durations can exceed wall time.
// Bytes exclude resumed journal entries; peak_active counts occupied chunk slots (including waiting).
struct InstallStats : CdnStats {
  std::uint64_t access_ms = 0, servers_ms = 0, manifests_ms = 0, prepare_ms = 0, download_ms = 0;
  std::uint64_t fetch_ms = 0, process_ms = 0, create_ms = 0, write_ms = 0, journal_ms = 0, backoff_ms = 0;
  std::uint64_t chunks_done = 0, chunks_skipped = 0, compressed_bytes = 0, original_bytes = 0;
  std::uint64_t files_created = 0, folders_created = 0, chunk_retries = 0, corrupt_retries = 0;
  std::uint64_t files_total = 0, download_total = 0, chunks_pending = 0, files_pending = 0, bytes_pending = 0;
  std::uint32_t slots = 4, reuse = 1, ahead_files = 256;
  std::uint32_t peak_waiting = 0, peak_active = 0;
  std::vector<InstallServerStats> server_stats;  // first eight servers, then one "other" bucket
};

struct InstallOutcome {
  InstallError error = InstallError::kProtocol;
  InstallProgress progress;
  std::string detail;  // short and identifier-free
  std::vector<std::uint32_t> skipped_depots;  // borrowed depots the account was refused, installed without
};

// One file an install keeps from its depots: the depot (its index in the merged list) whose entry won.
struct ManifestPlacement {
  std::size_t depot = 0;
  const ManifestFile* file = nullptr;
};

// The depots' files as an install lays them out. Later depots replace the same path from earlier ones;
// identical copies stay with the first. A file and a folder of the same name keep the file, as Steam does
// (Fallout 2's base depot has an empty master.dat folder that its language depot fills with the real
// master.dat), unless something lands inside the folder. A link is not a file here: it never displaces a
// folder.
struct DepotMerge {
  std::map<std::string, ManifestPlacement> placements;  // by lowercase path; points into the manifests
  // Each file that took the place of a folder of the same name: its key, then the folder's path.
  std::vector<std::pair<std::string, std::string>> files_over_folders;
  std::string conflict;  // a path the depots can't share (a link over a folder, a folder with files over a file)
  std::uint64_t install_bytes = 0;   // the merged files' bytes: what the install holds
  std::uint64_t download_bytes = 0;  // their chunks' compressed bytes: what a new install downloads
};
// `log` hears how each clash of a file and a folder settled (the result, then both entries: depot, kind,
// size and path).
DepotMerge MergeDepotManifests(const std::vector<DepotManifest>& manifests,
                               const std::function<void(const char* result, const std::string& detail)>& log = {});

// Seams for tests; the defaults use CmClient, CdnClient and the real filesystem.
struct InstallerHooks {
  struct ChunkFetcher {
    std::function<CdnError(const ContentServer&, std::uint32_t, const std::string&, std::size_t,
                           const std::atomic<bool>&, std::vector<std::uint8_t>*)> fetch;
    std::function<std::string()> last_failure;
    std::function<CdnStats()> stats;
  };
  // Called once per worker; returned hooks are used and destroyed on that worker.
  std::function<ChunkFetcher()> make_fetcher;
  std::function<DepotAccessResult(const StoredSession&, std::uint32_t app_id,
                                  const std::vector<DepotRequest>&, const std::atomic<bool>&)> access;
  std::function<CdnError(std::uint32_t cell_id, const std::atomic<bool>&, std::vector<ContentServer>*)> servers;
  // Returns a decrypted, validated manifest for one granted depot.
  std::function<InstallError(const ContentServer&, const DepotGrant&, const std::atomic<bool>&, DepotManifest*)> manifest;
  std::function<CdnError(const ContentServer&, std::uint32_t depot_id, const std::string& sha_hex,
                         std::size_t max_bytes, const std::atomic<bool>&, std::vector<std::uint8_t>*)> chunk;
  std::function<std::uint64_t(const std::string& path)> free_bytes;
  // Optional: the last CDN failure detail for logs.
  std::function<std::string()> last_failure;
  std::function<CdnStats()> cdn_stats;
  std::function<void(const char* phase, const InstallStats&)> stats;
  std::function<void(const std::vector<ContentServer>&, const char* preference)> server_list;
  // Optional: step markers for diagnosing native failures (no identifiers).
  std::function<void(const char* step)> trace;
  // Optional: how the merge settled a file and a folder of the same name in different depots (the result,
  // then both entries: depot, kind, size and path). At most DepotInstaller::kMergeLogLimit lines, then a count.
  std::function<void(const char* result, const std::string& detail)> merge;
  // Optional: waits between attempts at a chunk the CDN refused (a busy server answers 503 for a
  // while); returns early when canceled. Without it attempts follow each other at once.
  std::function<void(int milliseconds, const std::atomic<bool>&)> pause;
  // Optional, for Fastest: the winner remembered from an earlier download (a host) and where a new
  // winner goes; an empty host forgets it.
  std::function<std::optional<std::string>()> cached_fastest;
  std::function<void(const std::string& host)> remember_fastest;
  // Optional: how a Fastest race ("race") or check ("rerace") ended, with server labels, list
  // indexes and rates; never hosts.
  std::function<void(const char* step, const char* result, const std::string& detail)> race;
  // Optional: the clock for race deadlines and check windows; default steady_clock.
  std::function<std::chrono::steady_clock::time_point()> now;
};

// What installing some depots holds once merged (DepotMerge's sizes), from their manifests.
struct InstallMeasure {
  InstallError error = InstallError::kProtocol;
  std::string detail;  // short and identifier-free
  std::uint64_t install_bytes = 0;
  std::uint64_t download_bytes = 0;
  bool partial = false;  // a depot the account was refused is left out
};

// Downloads and assembles an app's depots into install_root. Safe to run again after any
// interruption: finished chunks recorded in the journal are not fetched twice. Runs
// synchronously on the caller's worker thread.
class DepotInstaller {
 public:
  DepotInstaller(CmClient& cm, CdnClient& cdn);
  explicit DepotInstaller(InstallerHooks hooks);

  InstallOutcome Run(const StoredSession& session, const InstallTarget& target,
                     const std::atomic<bool>& cancel,
                     const std::function<void(const InstallProgress&)>& on_progress = {});

  static constexpr const char* kJournalDir = ".playden-download";
  static constexpr const char* kInstallRecord = ".playden-install.json";
  static constexpr const char* kFileList = ".playden-files";
  // The merged tree a finished install holds, for updates: a "v1" line, then "<sha1 hex>\t<size>\t<path>"
  // per file (the depot entry that won the merge; folders and links left out), path as installed.
  // Written before the install record.
  static constexpr const char* kFileTree = ".playden-tree";
  // An update's work folder (installs::kUpdateDirName). Beside update.json and the journal it holds
  // "staging/<path>" (the changed files), "staged" (their paths, a line each), "old" (the file list the
  // game had), "files", "tree" and "record" (what the game gets when the update finishes) and "paths"
  // (every path either version has, in the file list format, for an uninstall in between). All of it is
  // written before the first staged chunk.
  static constexpr const char* kUpdateDir = installs::kUpdateDirName;

  // The size an install of `depots` (Steam's listing order, from `branch`; empty: public) would have, from
  // the manifests Run would fetch: depot access, the server list and each manifest, merged. Nothing is
  // written. A depot the account is refused is left out, as an install leaves it.
  InstallMeasure Measure(const StoredSession& session, std::uint32_t app_id, const std::vector<DepotInfo>& depots,
                         const std::string& branch, const std::atomic<bool>& cancel);

  // Finishes an update whose files are all staged (phase replacing or deleting): renames them over the
  // game's, deletes the files the new version dropped and the folders that leaves empty (never anything
  // else), writes the new tree, file list and record and removes the work folder. Safe to run again
  // after any interruption; needs no network. Stops between files when `cancel` is set (the update stays
  // where it got to). kProtocol when there is no such update.
  static InstallOutcome FinishUpdate(const std::string& install_root, const std::atomic<bool>& cancel,
                                     const std::function<void(const char* step)>& trace = {});
  static constexpr int kChunkAttempts = 3;
  static constexpr std::size_t kMergeLogLimit = 16;
  // Fastest. Only requests of large chunks over a reused connection are scored, by transfer time.
  static constexpr std::uint64_t kRaceChunkBytes = 256 * 1024;
  static constexpr std::uint64_t kRaceMinPendingBytes = 200ull * 1000 * 1000;
  static constexpr std::size_t kRaceSamples = 4;       // scored requests per server that end a race
  static constexpr std::size_t kReraceSamples = 3;
  static constexpr int kRaceCapSeconds = 15;
  static constexpr int kReraceCapSeconds = 5;
  static constexpr int kCheckWindowSeconds = 30;
  static constexpr std::size_t kCheckMinSamples = 8;   // a window with fewer large requests is not judged
  static constexpr int kReraceLimit = 2;

 private:
  InstallerHooks hooks_;
 public:
  void SetStats(std::function<void(const char*, const InstallStats&)> stats) { hooks_.stats = std::move(stats); }
  void SetServerList(std::function<void(const std::vector<ContentServer>&, const char*)> listed) {
    hooks_.server_list = std::move(listed);
  }
  void SetTrace(std::function<void(const char*)> trace) { hooks_.trace = std::move(trace); }
  void SetMerge(std::function<void(const char*, const std::string&)> merge) { hooks_.merge = std::move(merge); }
  void SetRace(std::function<void(const char*, const char*, const std::string&)> race) { hooks_.race = std::move(race); }
  void SetFastestCache(std::function<std::optional<std::string>()> cached, std::function<void(const std::string&)> remember) {
    hooks_.cached_fastest = std::move(cached);
    hooks_.remember_fastest = std::move(remember);
  }
  void SetTuning(DownloadTuning tuning) { tuning_ = tuning; }
 private:
  DownloadTuning tuning_;
};

// Removes a managed install directory: only paths under install_root, and only when it holds
// Playden's install record, download journal or an imported game's copy journal (.playden-copy).
// The record is deleted last, so a folder that could not be emptied is still recognized next time.
// `failure`, when given, says why it failed.
bool RemoveManagedInstall(const std::string& install_root, std::string* failure = nullptr);

// Deletes an update's work folder, leaving the installed game as it was; refuses once the update is
// putting files in place (phase replacing or deleting), which must finish instead.
bool DiscardManagedUpdate(const std::string& install_root, std::string* failure = nullptr);

}  // namespace playden::steam
