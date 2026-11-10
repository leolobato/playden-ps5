#pragma once

#include "platform/ps5/title_runtime.h"
#include "platform/storage.h"
#include "runtime/install_service.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace playden::runtime {

enum class LaunchState {
  kIdle,
  kWaitingForCloud,
  kReadyToHandoff,
  kLaunching,
  kRunning,
  kExited,
  kFailed,
};

enum class LaunchError {
  kNone,
  kNotFound,
  kNotReady,
  kPreflight,
  kDuplicate,
  kCloudWaiting,
  kCloudConflict,
  kCloudOffline,
  kCloudFailed,
  kCanceled,
  kInvalidSession,
  kPersistence,
  kWrongThread,
  kFlush,
  kNative,
  kInvalidEvidence,
};

struct LaunchResult {
  bool accepted = false;
  LaunchError error = LaunchError::kNone;
  std::string code;
  std::string message;
  std::string session_id;

  static LaunchResult Accepted(std::string session_id = {});
  static LaunchResult Failure(LaunchError error, std::string message,
                              std::string session_id = {});
};

struct LaunchActionResult {
  bool ok = false;
  std::string code;
  std::string message;

  static LaunchActionResult Success();
  static LaunchActionResult Failure(std::string code, std::string message);
};

struct LaunchPreflight {
  bool capability_available = false;
  bool volume_mounted = false;
  bool bottle_present = false;
  bool runtime_present = false;
  bool helper_valid = false;
  bool profile_present = false;
  bool title_owned = false;
  bool inventory_confirmed = false;
  NativeActivity activity = NativeActivity::kUnknown;
  std::uint64_t settings_revision = 0;
};

enum class CloudLaunchState { kReady, kWaiting, kConflict, kOffline, kFailed, kCanceled };

// account_token is an opaque account identity/revision token, never a credential.
struct CloudLaunchCheck {
  CloudLaunchState state = CloudLaunchState::kFailed;
  std::string account_token;
  bool allow_play_without_sync = false;
  std::string code;
  std::string message;
};

enum class LaunchEvidenceState { kNone, kStarting, kRunning, kFailed, kExited };

// The adapter must validate wrapper syntax/signature before setting valid. The
// service then binds every identity field to the pending launch before mapping
// the wrapper phase to a public state.
struct LaunchEvidence {
  bool valid = true;
  LaunchEvidenceState state = LaunchEvidenceState::kNone;
  std::string session_id;
  std::string install_id;
  std::string title_id;
  std::string runtime_bundle_id;
  std::uint64_t settings_revision = 0;
  std::optional<int> exit_code;
  std::string code;
  std::string message;

  static LaunchEvidence Starting(const RuntimeInstall &record,
                                 const std::string &session_id);
  static LaunchEvidence Running(const RuntimeInstall &record,
                                const std::string &session_id);
  static LaunchEvidence Failed(const RuntimeInstall &record,
                               const std::string &session_id,
                               std::string code, std::string message);
  static LaunchEvidence Exited(const RuntimeInstall &record,
                               const std::string &session_id,
                               std::optional<int> exit_code);
};

enum class LaunchSessionPhase { kPrepared, kHandoffBegun, kFailed };

struct LaunchSession {
  std::uint32_t schema_version = 1;
  std::string session_id;
  std::string install_id;
  std::string title_id;
  std::string runtime_bundle_id;
  std::uint64_t settings_revision = 0;
  std::string account_token;
  LaunchSessionPhase phase = LaunchSessionPhase::kPrepared;
  std::int64_t requested_ms = 0;
  std::int64_t handoff_ms = 0;
  bool has_native_result = false;
  std::uint32_t native_code = 0;
  std::string failure_code;
  std::string failure_message;
};

struct LaunchJournalResult {
  bool ok = false;
  std::string code;
  std::string message;
  static LaunchJournalResult Success();
  static LaunchJournalResult Failure(std::string code, std::string message);
};

struct LaunchJournalLoad {
  bool ok = false;
  std::optional<LaunchSession> session;
  std::string code;
  std::string message;
  static LaunchJournalLoad Success(std::optional<LaunchSession> session);
  static LaunchJournalLoad Failure(std::string code, std::string message);
};

class LaunchSessionJournal {
public:
  virtual ~LaunchSessionJournal() = default;
  virtual LaunchJournalLoad LoadPending() = 0;
  virtual LaunchJournalResult CreateHistory(const LaunchSession &session) = 0;
  virtual LaunchJournalResult PersistPending(const LaunchSession &session) = 0;
  virtual LaunchJournalResult ClearPending() = 0;
};

// History creation is immutable: an existing UUID is rejected, never replaced.
// Pending state is a separate private file so wrapper session evidence can use
// its own per-session files without exposing the opaque account token.
class StorageLaunchSessionJournal final : public LaunchSessionJournal {
public:
  explicit StorageLaunchSessionJournal(
      Storage &storage, std::string pending_path = "launch-pending.v1",
      std::string history_prefix = "launch-history-");
  LaunchJournalLoad LoadPending() override;
  LaunchJournalResult CreateHistory(const LaunchSession &session) override;
  LaunchJournalResult PersistPending(const LaunchSession &session) override;
  LaunchJournalResult ClearPending() override;

private:
  Storage &storage_;
  std::string pending_path_;
  std::string history_prefix_;
};

struct LaunchSnapshot {
  LaunchState state = LaunchState::kIdle;
  LaunchError error = LaunchError::kNone;
  std::string install_id;
  std::string session_id;
  std::string code;
  std::string message;
  std::optional<std::uint32_t> native_code;
  std::optional<int> exit_code;
};

using LaunchPreflightQuery = std::function<LaunchPreflight(const RuntimeInstall &)>;
using LaunchCloudHook =
    std::function<CloudLaunchCheck(const RuntimeInstall &, bool play_without_sync)>;
using LaunchEvidenceQuery =
    std::function<LaunchEvidence(const RuntimeInstall &, const std::string &session_id)>;
using LaunchActivityQuery = std::function<NativeActivity(const RuntimeInstall &)>;
using LaunchBackend =
    std::function<ps5::TitleResult(const RuntimeInstall &, const std::string &session_id)>;

// The production adapter for LaunchBackend calls
// TitleBackend::Launch(record.title_id, {"session=" + session_id}). Keeping
// that native boundary injected lets host tests prove no invalid request can
// reach it.

struct LaunchDependencies {
  LaunchPreflightQuery preflight;
  LaunchCloudHook cloud;
  LaunchEvidenceQuery evidence;
  // Whether the title is running, for following a session after handoff. Empty
  // uses the full preflight's activity.
  LaunchActivityQuery activity;
  LaunchBackend backend;
  std::function<LaunchActionResult()> flush;
  std::function<std::string()> uuid;
  std::function<std::string()> current_account_token;
  std::function<std::int64_t()> now_ms;
  std::function<bool()> is_application_thread;
  std::int64_t evidence_wait_ms = 10000;
};

class LaunchService {
public:
  // A record that still says starting or running is ended after this many
  // consecutive stopped observations (the title was closed or crashed).
  static constexpr int kStoppedPollsToEnd = 3;
  LaunchService(InstallRecordStore &records, LaunchSessionJournal &journal,
                LaunchDependencies dependencies);

  LaunchResult Request(const std::string &install_id);
  LaunchResult PlayWithoutSync();
  LaunchResult Cancel();

  // This is the only method that can invoke the native backend. The injected
  // thread predicate is enforced immediately before all handoff effects.
  LaunchResult Handoff();
  LaunchSnapshot Snapshot();
  // Ends the pending session once its outcome is final: the wrapper reported
  // exit or failure, the handoff failed, or a session recovered after restart
  // never began its handoff. The pending record is removed so a new launch can
  // start; the immutable history stays. An undecided session is refused.
  LaunchResult Finish();

private:
  LaunchResult Begin(const RuntimeInstall &record,
                     const std::string &account_token);
  LaunchResult ContinueCloud(bool play_without_sync);
  LaunchResult Reject(LaunchError error, std::string code,
                      std::string message) const;
  bool PersistFailure(LaunchError error, const std::string &code,
                      const std::string &message);

  InstallRecordStore &records_;
  LaunchSessionJournal &journal_;
  LaunchDependencies dependencies_;
  std::optional<LaunchSession> session_;
  std::optional<RuntimeInstall> cloud_record_;
  CloudLaunchCheck cloud_check_;
  LaunchSnapshot snapshot_;
  bool recovered_pending_ = false;
  // Consecutive observations of a stopped title while its record still says
  // starting or running: the title ended without writing its exit. Each
  // session starts its own count.
  int stopped_polls_ = 0;
  // A durable operation can fail after its rename/remove became visible. Freeze
  // this instance until a fresh journal load reconciles the on-disk state.
  bool persistence_uncertain_ = false;
};

} // namespace playden::runtime
