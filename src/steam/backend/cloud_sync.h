#pragma once

#include "steam/backend/cloud_mapping.h"
#include "steam/backend/cloud_messages.h"
#include "steam/backend/cm_client.h"
#include "steam/backend/http_client.h"
#include "steam/session_store.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace playden::steam::cloud {

// Seams for tests; the defaults use CmClient and HttpClient.
struct CloudHooks {
  std::function<CmError(const StoredSession&, std::uint32_t deadline_ms, const std::atomic<bool>&,
                        const std::function<CmError(const CmClient::ServiceCall&, std::uint64_t steam_id)>&)> session;
  // The app's Steam Cloud declaration from PICS; nothing when Steam does not describe the app.
  std::function<std::optional<CloudSaveInfo>(const StoredSession&, std::uint32_t app_id, const std::atomic<bool>&,
                                             CmError* error)> app_info;
  std::function<HttpResponse(const HttpRequest&, const std::atomic<bool>&)> http;
  // Unix time for baselines and backup folder names.
  std::function<std::int64_t()> now;
  // Optional step markers for diagnosing failures on the console (no names or identifiers).
  std::function<void(const char* step)> trace;
  // Optional: where the last session open or app info request stopped (CmClient::last_failure), so a
  // failure that never reached Steam says why and its log line names the stage.
  std::function<CmFailure()> failure;
};

// Where one game's saves live on this device.
struct GameSaveLocation {
  std::uint32_t app_id = 0;
  std::string game_root;    // the installed game
  std::string prefix_root;  // folder holding drive_c for the game's Windows user folders
  std::string state_root;   // the sync baseline and backups; outlives the install
};

struct FileFact {
  bool present = false;
  std::vector<std::uint8_t> sha1;
  std::uint64_t size = 0;
  std::uint64_t time = 0;
};

enum class Action { kUnchanged, kDownload, kUpload, kDeleteLocal, kDeleteRemote, kConflict, kUnavailable };

struct Decision {
  std::string remote_name;
  LocalSavePath local;
  FileFact local_fact;
  FileFact remote_fact;
  Action action = Action::kUnchanged;
};

enum class PlanStatus {
  kUpToDate,       // nothing to transfer
  kReady,          // transfers can run without a choice
  kConflict,       // the player picks local or Cloud
  kNeedsAccount,   // remote changes wait for the player to confirm this Steam account
  kUnsupported,    // no Cloud saves, or locations Playden cannot map
  kFailed,
};

struct CloudPlan {
  PlanStatus status = PlanStatus::kFailed;
  std::string message;  // short, user-facing
  std::string detail;   // for the log only: the step and what failed (no address, token or account)
  bool canceled = false;  // failed because the caller canceled it, not because of Steam or this device
  std::uint64_t change_number = 0;
  std::string account_key;
  bool requires_account_confirmation = false;
  std::vector<Decision> decisions;
  std::size_t downloads = 0, uploads = 0, local_deletes = 0, remote_deletes = 0, conflicts = 0;
  // For the conflict dialog: what each side holds.
  std::size_t local_files = 0, remote_files = 0;
  std::uint64_t local_bytes = 0, remote_bytes = 0, local_latest = 0, remote_latest = 0;
  // This console would replace Cloud saves before it ever uploaded any for this game: asked first.
  bool first_overwrite = false;
};

enum class Resolution { kAutomatic, kUseLocal, kUseCloud };

// What the quick check found for one game since its last full check: which side changed, or
// kUnknown when there is nothing to compare with (no index yet, another account, Steam unreachable).
enum class Drift { kUnknown, kUnchanged, kLocal, kRemote, kBoth };

// Bytes moved so far out of the bytes this Apply moves.
using ApplyProgress = std::function<void(std::uint64_t done, std::uint64_t total)>;

struct CloudOutcome {
  bool ok = false;
  std::string message;  // short, user-facing
  std::string detail;   // for the log only: the file and what failed
  bool canceled = false;  // failed because the caller canceled a request, not because of Steam or this device
  std::size_t downloaded = 0, uploaded = 0, removed_local = 0, removed_remote = 0;
  // Both sides once the transfers are done (when ok).
  std::size_t local_files = 0, remote_files = 0;
  std::uint64_t local_bytes = 0, remote_bytes = 0, local_latest = 0, remote_latest = 0;
};

inline constexpr std::size_t kMaxLocalSaveFiles = 10'000;
inline constexpr std::uint64_t kMaxLocalSaveBytes = 512ULL * 1024 * 1024;
inline constexpr const char* kMachineName = "Playden";

std::string AccountKey(std::uint64_t steam_id);

// Steam Cloud save sync for one game, run synchronously on a worker thread. Timestamps never pick
// a winner: a file changed on both sides since the last sync is a conflict for the player. Local
// files are backed up under state_root before they are replaced or removed, and so are the Cloud
// copies an upload replaces or removes (under cloud/). The first upload from this console that would
// replace Cloud saves is asked first (CloudPlan::first_overwrite).
class CloudSync {
 public:
  CloudSync(CmClient& cm, HttpClient& http);
  explicit CloudSync(CloudHooks hooks);
  void SetTrace(std::function<void(const char*)> trace) { hooks_.trace = std::move(trace); }

  CloudPlan Check(const StoredSession& session, const GameSaveLocation& location, const std::atomic<bool>& cancel);
  // Carries out a plan from Check. `resolution` settles conflicts; remote changes on a plan that
  // requires it run only with `confirm_account`.
  CloudOutcome Apply(const StoredSession& session, const GameSaveLocation& location, const CloudPlan& plan,
                     Resolution resolution, bool confirm_account, const std::atomic<bool>& cancel,
                     const ApplyProgress& progress = {});
  // Cheap check of several games against the index each full check leaves in state_root: local
  // save sizes and times, and one change-number request per game in a single Steam session. Only
  // reads; a game that drifted needs a full Check.
  std::vector<Drift> QuickCheck(const StoredSession& session, const std::vector<GameSaveLocation>& locations,
                                const std::atomic<bool>& cancel);
  // Removes the save files the last check found on this device, backed up under state_root first.
  // Cloud is not touched; the next Check offers the Cloud copies again.
  bool RemoveLocal(const GameSaveLocation& location, std::string* message);

 private:
  CloudHooks hooks_;
};

}  // namespace playden::steam::cloud
