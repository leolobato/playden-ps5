#pragma once

#include "library/store.h"
#include "steam/backend/app_info.h"
#include "steam/backend/cdn_client.h"
#include "steam/backend/cm_client.h"
#include "steam/backend/depot_manifest.h"
#include "steam/session_store.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
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
  // The folder holds a complete install (or an update that stopped part way): files that changed are
  // built in kUpdateDir, from chunks already on disk where they match and downloads for the rest, and
  // swapped in at the end. Until then the installed version stays as it was.
  bool update = false;
};

struct InstallProgress {
  std::uint64_t download_total = 0;  // compressed bytes of every chunk to write
  std::uint64_t download_done = 0;   // includes chunks already present from a previous run
  std::uint64_t install_total = 0;   // file bytes after merging depots
  std::uint32_t files_total = 0;
  // Bytes of install_total this install already occupies on disk: preallocated files
  // (exFAT allocates on ftruncate) or written chunks (sparse filesystems).
  std::uint64_t held_bytes = 0;
  // An update reading the installed files first: bytes checked of check_total (the other counts are
  // not known yet).
  bool checking = false;
  std::uint64_t check_done = 0;
  std::uint64_t check_total = 0;
};

struct DownloadTuning {
  int slots = 4;
  int ahead_files = 256;
};

// A missing or malformed file uses defaults; valid slot counts are clamped to 1..8.
DownloadTuning ReadDownloadTuning(const std::string& path);

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
};

struct InstallOutcome {
  InstallError error = InstallError::kProtocol;
  InstallProgress progress;
  std::string detail;  // short and identifier-free
  std::vector<std::uint32_t> skipped_depots;  // borrowed depots the account was refused, installed without
};

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
  // Optional: step markers for diagnosing native failures (no identifiers).
  std::function<void(const char* step)> trace;
  // Optional: waits between attempts at a chunk the CDN refused (a busy server answers 503 for a
  // while); returns early when canceled. Without it attempts follow each other at once.
  std::function<void(int milliseconds, const std::atomic<bool>&)> pause;
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
  static constexpr const char* kUpdateDir = ".playden-update";
  static constexpr int kChunkAttempts = 3;

 private:
  InstallerHooks hooks_;
 public:
  void SetStats(std::function<void(const char*, const InstallStats&)> stats) { hooks_.stats = std::move(stats); }
  void SetTrace(std::function<void(const char*)> trace) { hooks_.trace = std::move(trace); }
  void SetTuning(DownloadTuning tuning) { tuning_ = tuning; }
 private:
  DownloadTuning tuning_;
};

// Removes a managed install directory: only paths under install_root, and only when it holds
// Playden's install record, download journal or an imported game's copy journal (.playden-copy).
// The record is deleted last, so a folder that could not be emptied is still recognized next time.
// `failure`, when given, says why it failed.
bool RemoveManagedInstall(const std::string& install_root, std::string* failure = nullptr);

// The depots and manifests an install record names; empty when it is missing or unreadable.
std::vector<std::pair<std::uint32_t, std::uint64_t>> RecordedDepots(const std::string& install_root);

// Deletes an update's staged files. False (and nothing deleted) once its files are being swapped in:
// the next install run finishes that.
bool DiscardUpdate(const std::string& install_root);

}  // namespace playden::steam
