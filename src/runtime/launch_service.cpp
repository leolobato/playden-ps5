#include "runtime/launch_service.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <limits>
#include <string_view>
#include <utility>

namespace playden::runtime {
namespace {

constexpr std::size_t kMaxJournalBytes = 64 * 1024;

bool ValidUuid(const std::string &value) {
  if (value.size() != 36 || value[8] != '-' || value[13] != '-' ||
      value[18] != '-' || value[23] != '-' || value[14] != '4' ||
      std::string("89ab").find(value[19]) == std::string::npos)
    return false;
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23)
      continue;
    if (!std::isdigit(static_cast<unsigned char>(value[i])) &&
        (value[i] < 'a' || value[i] > 'f'))
      return false;
  }
  return true;
}

void AppendHex(std::string *output, const std::string &value) {
  static constexpr char digits[] = "0123456789abcdef";
  for (const unsigned char byte : value) {
    output->push_back(digits[byte >> 4]);
    output->push_back(digits[byte & 15]);
  }
}

std::optional<std::string> Unhex(std::string_view value) {
  if ((value.size() & 1U) != 0 || value.size() > kMaxJournalBytes * 2)
    return std::nullopt;
  const auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
  };
  std::string result;
  result.reserve(value.size() / 2);
  for (std::size_t i = 0; i < value.size(); i += 2) {
    const int high = nibble(value[i]);
    const int low = nibble(value[i + 1]);
    if (high < 0 || low < 0) return std::nullopt;
    result.push_back(static_cast<char>((high << 4) | low));
  }
  return result;
}

template <typename Integer>
bool ParseInteger(std::string_view text, Integer *value) {
  if (text.empty()) return false;
  Integer parsed{};
  const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
  if (result.ec != std::errc() || result.ptr != text.data() + text.size())
    return false;
  *value = parsed;
  return true;
}

template <typename Integer>
bool AppendIntegerLine(std::string *output, Integer value) {
  std::array<char, std::numeric_limits<Integer>::digits10 + 4> buffer{};
  const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
  if (converted.ec != std::errc()) return false;
  output->append(buffer.data(), converted.ptr);
  output->push_back('\n');
  return true;
}

bool SupportedSession(const LaunchSession &session) {
  const auto phase = static_cast<unsigned>(session.phase);
  if (session.schema_version != 1 || !ValidUuid(session.session_id) ||
      session.install_id.empty() || session.title_id.empty() ||
      session.runtime_bundle_id.empty() ||
      phase > static_cast<unsigned>(LaunchSessionPhase::kFailed)) return false;
  const std::string *fields[] = {
      &session.session_id, &session.install_id, &session.title_id,
      &session.runtime_bundle_id, &session.account_token,
      &session.failure_code, &session.failure_message};
  return std::all_of(std::begin(fields), std::end(fields), [](const auto *field) {
    return field->size() <= kMaxJournalBytes / 2;
  });
}

std::optional<std::string> Serialize(const LaunchSession &session) {
  if (!SupportedSession(session)) return std::nullopt;
  std::string output = "PLAYDEN-LAUNCH-V1\n";
  const auto append_text = [&](const std::string &value) {
    AppendHex(&output, value);
    output.push_back('\n');
  };
  append_text(session.session_id);
  append_text(session.install_id);
  append_text(session.title_id);
  append_text(session.runtime_bundle_id);
  if (!AppendIntegerLine(&output, session.settings_revision)) return std::nullopt;
  append_text(session.account_token);
  if (!AppendIntegerLine(&output, static_cast<unsigned>(session.phase)) ||
      !AppendIntegerLine(&output, session.requested_ms) ||
      !AppendIntegerLine(&output, session.handoff_ms) ||
      !AppendIntegerLine(&output, session.has_native_result ? 1u : 0u) ||
      !AppendIntegerLine(&output, session.native_code)) return std::nullopt;
  append_text(session.failure_code);
  append_text(session.failure_message);
  if (output.size() > kMaxJournalBytes) return std::nullopt;
  return output;
}

std::optional<LaunchSession> Parse(const Bytes &bytes) {
  if (bytes.empty() || bytes.size() > kMaxJournalBytes) return std::nullopt;
  const std::string_view text(reinterpret_cast<const char *>(bytes.data()), bytes.size());
  std::array<std::string_view, 14> lines{};
  std::size_t start = 0;
  for (std::size_t index = 0; index < lines.size(); ++index) {
    const auto newline = text.find('\n', start);
    if (newline == std::string_view::npos) {
      if (index + 1 != lines.size()) return std::nullopt;
      lines[index] = text.substr(start);
      start = text.size();
    } else {
      lines[index] = text.substr(start, newline - start);
      start = newline + 1;
      if (index + 1 == lines.size() && start != text.size()) return std::nullopt;
    }
  }
  if (lines[0] != "PLAYDEN-LAUNCH-V1") return std::nullopt;
  LaunchSession session;
  const auto session_id = Unhex(lines[1]);
  const auto install_id = Unhex(lines[2]);
  const auto title_id = Unhex(lines[3]);
  const auto bundle_id = Unhex(lines[4]);
  const auto account = Unhex(lines[6]);
  const auto failure_code = Unhex(lines[12]);
  const auto failure_message = Unhex(lines[13]);
  unsigned phase = 0;
  unsigned native_present = 0;
  if (!session_id || !install_id || !title_id || !bundle_id || !account ||
      !failure_code || !failure_message || !ValidUuid(*session_id) ||
      install_id->empty() || title_id->empty() || bundle_id->empty() ||
      !ParseInteger(lines[5], &session.settings_revision) ||
      !ParseInteger(lines[7], &phase) ||
      phase > static_cast<unsigned>(LaunchSessionPhase::kFailed) ||
      !ParseInteger(lines[8], &session.requested_ms) ||
      !ParseInteger(lines[9], &session.handoff_ms) ||
      !ParseInteger(lines[10], &native_present) || native_present > 1 ||
      !ParseInteger(lines[11], &session.native_code))
    return std::nullopt;
  session.session_id = *session_id;
  session.install_id = *install_id;
  session.title_id = *title_id;
  session.runtime_bundle_id = *bundle_id;
  session.account_token = *account;
  session.phase = static_cast<LaunchSessionPhase>(phase);
  session.has_native_result = native_present != 0;
  session.failure_code = *failure_code;
  session.failure_message = *failure_message;
  return session;
}

std::optional<Bytes> Encode(const LaunchSession &session) {
  const auto text = Serialize(session);
  if (!text) return std::nullopt;
  Bytes bytes(text->begin(), text->end());
  if (!Parse(bytes)) return std::nullopt;
  return bytes;
}

LaunchSnapshot FailureSnapshot(const LaunchSession &session, LaunchError error,
                               std::string code, std::string message) {
  LaunchSnapshot snapshot;
  snapshot.state = LaunchState::kFailed;
  snapshot.error = error;
  snapshot.install_id = session.install_id;
  snapshot.session_id = session.session_id;
  snapshot.code = std::move(code);
  snapshot.message = std::move(message);
  if (session.has_native_result) snapshot.native_code = session.native_code;
  return snapshot;
}

LaunchError FailureError(const std::string &code) {
  if (code == "wrapper-evidence-invalid" || code == "evidence-unavailable")
    return LaunchError::kInvalidEvidence;
  if (code == "flush-failed" || code == "flush-unavailable")
    return LaunchError::kFlush;
  if (code == "cloud-account-changed") return LaunchError::kCloudFailed;
  if (code == "install-changed" || code == "preflight-failed" ||
      code == "preflight-unavailable")
    return LaunchError::kPreflight;
  return LaunchError::kNative;
}

LaunchResult PreflightFailure(const LaunchPreflight &preflight,
                              std::uint64_t expected_revision) {
  if (!preflight.capability_available)
    return LaunchResult::Failure(LaunchError::kPreflight,
                                 "native launch capability is unavailable");
  if (!preflight.volume_mounted)
    return LaunchResult::Failure(LaunchError::kPreflight,
                                 "installation volume is unavailable");
  if (!preflight.bottle_present)
    return LaunchResult::Failure(LaunchError::kPreflight,
                                 "game bottle is unavailable");
  if (!preflight.runtime_present)
    return LaunchResult::Failure(LaunchError::kPreflight,
                                 "runtime bundle is unavailable");
  if (!preflight.helper_valid)
    return LaunchResult::Failure(LaunchError::kPreflight,
                                 "runtime helper identity is invalid");
  if (!preflight.profile_present)
    return LaunchResult::Failure(LaunchError::kPreflight,
                                 "effective launch profile is unavailable");
  if (!preflight.title_owned)
    return LaunchResult::Failure(LaunchError::kPreflight,
                                 "native title ownership is invalid");
  if (!preflight.inventory_confirmed)
    return LaunchResult::Failure(LaunchError::kPreflight,
                                 "native title inventory is unavailable");
  if (preflight.activity == NativeActivity::kActive)
    return LaunchResult::Failure(LaunchError::kPreflight,
                                 "native title is already active");
  if (preflight.activity == NativeActivity::kUnknown)
    return LaunchResult::Failure(LaunchError::kPreflight,
                                 "native title activity is unknown");
  if (preflight.settings_revision != expected_revision)
    return LaunchResult::Failure(LaunchError::kPreflight,
                                 "effective settings changed before launch");
  return LaunchResult::Accepted();
}

LaunchEvidence Evidence(LaunchEvidenceState state, const RuntimeInstall &record,
                        const std::string &session_id) {
  LaunchEvidence evidence;
  evidence.state = state;
  evidence.session_id = session_id;
  evidence.install_id = record.install_id;
  evidence.title_id = record.title_id;
  evidence.runtime_bundle_id = record.runtime_bundle_id;
  evidence.settings_revision = record.settings_revision;
  return evidence;
}

} // namespace

LaunchResult LaunchResult::Accepted(std::string session_id) {
  return {true, LaunchError::kNone, {}, {}, std::move(session_id)};
}

LaunchResult LaunchResult::Failure(LaunchError error, std::string message,
                                   std::string session_id) {
  return {false, error, {}, std::move(message), std::move(session_id)};
}

LaunchActionResult LaunchActionResult::Success() { return {true, {}, {}}; }
LaunchActionResult LaunchActionResult::Failure(std::string code,
                                               std::string message) {
  return {false, std::move(code), std::move(message)};
}

LaunchEvidence LaunchEvidence::Starting(const RuntimeInstall &record,
                                        const std::string &session_id) {
  return Evidence(LaunchEvidenceState::kStarting, record, session_id);
}
LaunchEvidence LaunchEvidence::Running(const RuntimeInstall &record,
                                       const std::string &session_id) {
  return Evidence(LaunchEvidenceState::kRunning, record, session_id);
}
LaunchEvidence LaunchEvidence::Failed(const RuntimeInstall &record,
                                      const std::string &session_id,
                                      std::string code, std::string message) {
  auto evidence = Evidence(LaunchEvidenceState::kFailed, record, session_id);
  evidence.code = std::move(code);
  evidence.message = std::move(message);
  return evidence;
}
LaunchEvidence LaunchEvidence::Exited(const RuntimeInstall &record,
                                      const std::string &session_id,
                                      std::optional<int> exit_code) {
  auto evidence = Evidence(LaunchEvidenceState::kExited, record, session_id);
  evidence.exit_code = exit_code;
  return evidence;
}

LaunchJournalResult LaunchJournalResult::Success() { return {true, {}, {}}; }
LaunchJournalResult LaunchJournalResult::Failure(std::string code,
                                                 std::string message) {
  return {false, std::move(code), std::move(message)};
}
LaunchJournalLoad LaunchJournalLoad::Success(
    std::optional<LaunchSession> session) {
  return {true, std::move(session), {}, {}};
}
LaunchJournalLoad LaunchJournalLoad::Failure(std::string code,
                                             std::string message) {
  return {false, std::nullopt, std::move(code), std::move(message)};
}

StorageLaunchSessionJournal::StorageLaunchSessionJournal(
    Storage &storage, std::string pending_path, std::string history_prefix)
    : storage_(storage), pending_path_(std::move(pending_path)),
      history_prefix_(std::move(history_prefix)) {}

LaunchJournalLoad StorageLaunchSessionJournal::LoadPending() {
  const auto read = storage_.ReadDurable(pending_path_, kMaxJournalBytes);
  if (!read.ok()) {
    if (read.error() == StorageError::kNotFound)
      return LaunchJournalLoad::Success(std::nullopt);
    return LaunchJournalLoad::Failure("pending-read-failed",
                                      "pending launch could not be read");
  }
  const auto parsed = Parse(read.value());
  if (!parsed)
    return LaunchJournalLoad::Failure("pending-invalid",
                                      "pending launch is malformed");
  return LaunchJournalLoad::Success(*parsed);
}

LaunchJournalResult
StorageLaunchSessionJournal::CreateHistory(const LaunchSession &session) {
  if (!ValidUuid(session.session_id))
    return LaunchJournalResult::Failure("invalid-session-id",
                                        "launch session UUID is invalid");
  const std::string path = history_prefix_ + session.session_id + ".v1";
  const auto bytes = Encode(session);
  if (!bytes)
    return LaunchJournalResult::Failure("history-invalid",
                                        "launch history state is unsupported or too large");
  const auto written = storage_.WriteDurableExclusive(path, *bytes);
  return written.ok()
             ? LaunchJournalResult::Success()
             : LaunchJournalResult::Failure("history-write-failed",
                                            "launch history could not be written");
}

LaunchJournalResult
StorageLaunchSessionJournal::PersistPending(const LaunchSession &session) {
  if (!ValidUuid(session.session_id))
    return LaunchJournalResult::Failure("invalid-session-id",
                                        "launch session UUID is invalid");
  const auto bytes = Encode(session);
  if (!bytes)
    return LaunchJournalResult::Failure("pending-invalid",
                                        "pending launch state is unsupported or too large");
  const auto written = storage_.WriteDurable(pending_path_, *bytes);
  return written.ok()
             ? LaunchJournalResult::Success()
             : LaunchJournalResult::Failure("pending-write-failed",
                                            "pending launch could not be written");
}

LaunchJournalResult StorageLaunchSessionJournal::ClearPending() {
  const auto removed = storage_.RemoveDurable(pending_path_);
  return removed.ok()
             ? LaunchJournalResult::Success()
             : LaunchJournalResult::Failure("pending-remove-failed",
                                            "pending launch could not be removed");
}

LaunchService::LaunchService(InstallRecordStore &records,
                             LaunchSessionJournal &journal,
                             LaunchDependencies dependencies)
    : records_(records), journal_(journal),
      dependencies_(std::move(dependencies)) {
  const auto pending = journal_.LoadPending();
  if (!pending.ok) {
    snapshot_.state = LaunchState::kFailed;
    snapshot_.error = LaunchError::kPersistence;
    snapshot_.code = pending.code;
    snapshot_.message = pending.message;
  } else if (pending.session) {
    session_ = *pending.session;
    recovered_pending_ = true;
    snapshot_.install_id = session_->install_id;
    snapshot_.session_id = session_->session_id;
    if (session_->phase == LaunchSessionPhase::kPrepared) {
      snapshot_.state = LaunchState::kFailed;
      snapshot_.error = LaunchError::kDuplicate;
      snapshot_.code = "launch-recovery-required";
      snapshot_.message = "pending launch requires reconciliation after restart";
    } else {
      snapshot_.state = LaunchState::kLaunching;
    }
  }
}

LaunchResult LaunchService::Reject(LaunchError error, std::string code,
                                   std::string message) const {
  auto result = LaunchResult::Failure(
      error, std::move(message), session_ ? session_->session_id : std::string{});
  result.code = std::move(code);
  return result;
}

LaunchResult LaunchService::Request(const std::string &install_id) {
  if (session_)
    return Reject(LaunchError::kDuplicate, "launch-pending",
                  "another launch session is still pending");
  if (cloud_record_) {
    if (cloud_record_->install_id != install_id)
      return Reject(LaunchError::kDuplicate, "cloud-pending",
                    "another launch request is waiting for Cloud");
    return ContinueCloud(false);
  }

  const auto disk_pending = journal_.LoadPending();
  if (!disk_pending.ok)
    return Reject(LaunchError::kPersistence, disk_pending.code,
                  disk_pending.message);
  if (disk_pending.session) {
    session_ = *disk_pending.session;
    recovered_pending_ = true;
    stopped_polls_ = 0;
    return Reject(LaunchError::kDuplicate, "launch-pending",
                  "another launch session is still pending");
  }

  const auto record = records_.Find(install_id);
  if (!record)
    return Reject(LaunchError::kNotFound, "install-not-found",
                  "runtime installation was not found");
  if (record->state != PreparationState::kReady || record->journal)
    return Reject(LaunchError::kNotReady, "install-not-ready",
                  "runtime installation is not ready");
  if (!dependencies_.preflight)
    return Reject(LaunchError::kPreflight, "preflight-unavailable",
                  "launch preflight is unavailable");
  const auto checked = PreflightFailure(dependencies_.preflight(*record),
                                        record->settings_revision);
  if (!checked.accepted) return checked;

  cloud_record_ = *record;
  return ContinueCloud(false);
}

LaunchResult LaunchService::ContinueCloud(bool play_without_sync) {
  if (!cloud_record_)
    return Reject(LaunchError::kCloudFailed, "cloud-request-missing",
                  "no Cloud-gated launch is pending");
  if (!dependencies_.cloud) {
    const auto record = *cloud_record_;
    cloud_record_.reset();
    return Begin(record, dependencies_.current_account_token
                             ? dependencies_.current_account_token()
                             : std::string{});
  }
  const std::string original_account = cloud_check_.account_token;
  const auto check = dependencies_.cloud(*cloud_record_, play_without_sync);
  cloud_check_ = check;
  const std::string current_account = dependencies_.current_account_token
                                          ? dependencies_.current_account_token()
                                          : std::string{};
  if ((!check.account_token.empty() && check.account_token != current_account) ||
      (!original_account.empty() && check.account_token != original_account)) {
    cloud_record_.reset();
    snapshot_.state = LaunchState::kFailed;
    snapshot_.error = LaunchError::kCloudFailed;
    snapshot_.code = "cloud-account-changed";
    snapshot_.message = "Cloud account changed before launch";
    return LaunchResult::Failure(LaunchError::kCloudFailed, snapshot_.message);
  }

  if (check.state == CloudLaunchState::kReady) {
    if (play_without_sync && !check.allow_play_without_sync)
      return Reject(LaunchError::kCloudOffline, "cloud-opt-in-rejected",
                    "Cloud did not permit Play without syncing");
    const auto record = *cloud_record_;
    cloud_record_.reset();
    return Begin(record, check.account_token);
  }
  if (check.state == CloudLaunchState::kFailed ||
      check.state == CloudLaunchState::kCanceled) {
    cloud_record_.reset();
    snapshot_.state = LaunchState::kFailed;
    snapshot_.error = check.state == CloudLaunchState::kCanceled
                          ? LaunchError::kCanceled
                          : LaunchError::kCloudFailed;
    snapshot_.code = check.code;
    snapshot_.message = check.message;
    return LaunchResult::Failure(snapshot_.error, snapshot_.message);
  }
  snapshot_.state = LaunchState::kWaitingForCloud;
  snapshot_.error = check.state == CloudLaunchState::kConflict
                        ? LaunchError::kCloudConflict
                        : check.state == CloudLaunchState::kOffline
                              ? LaunchError::kCloudOffline
                              : LaunchError::kCloudWaiting;
  snapshot_.install_id = cloud_record_->install_id;
  snapshot_.code = check.code;
  snapshot_.message = check.message;
  return LaunchResult::Accepted();
}

LaunchResult LaunchService::PlayWithoutSync() {
  if (!cloud_record_ || cloud_check_.state != CloudLaunchState::kOffline ||
      !cloud_check_.allow_play_without_sync)
    return Reject(LaunchError::kCloudOffline, "cloud-opt-in-unavailable",
                  "Play without syncing is not available");
  return ContinueCloud(true);
}

LaunchResult LaunchService::Cancel() {
  if (!cloud_record_)
    return Reject(LaunchError::kCanceled, "launch-not-cancelable",
                  "no Cloud-gated launch is pending");
  cloud_record_.reset();
  snapshot_ = {};
  snapshot_.state = LaunchState::kFailed;
  snapshot_.error = LaunchError::kCanceled;
  snapshot_.code = "launch-canceled";
  snapshot_.message = "launch was canceled";
  return LaunchResult::Accepted();
}

LaunchResult LaunchService::Begin(const RuntimeInstall &record,
                                  const std::string &account_token) {
  if (!dependencies_.uuid || !dependencies_.now_ms)
    return Reject(LaunchError::kInvalidSession, "session-provider-unavailable",
                  "launch session provider is unavailable");
  const std::string session_id = dependencies_.uuid();
  if (!ValidUuid(session_id))
    return Reject(LaunchError::kInvalidSession, "invalid-session-id",
                  "launch session UUID is invalid");
  LaunchSession session;
  session.session_id = session_id;
  session.install_id = record.install_id;
  session.title_id = record.title_id;
  session.runtime_bundle_id = record.runtime_bundle_id;
  session.settings_revision = record.settings_revision;
  session.account_token = account_token;
  session.requested_ms = dependencies_.now_ms();
  const auto history = journal_.CreateHistory(session);
  if (!history.ok)
    return Reject(LaunchError::kPersistence, history.code, history.message);
  const auto pending = journal_.PersistPending(session);
  if (!pending.ok)
    return Reject(LaunchError::kPersistence, pending.code, pending.message);
  session_ = session;
  stopped_polls_ = 0;
  snapshot_ = {};
  snapshot_.state = LaunchState::kReadyToHandoff;
  snapshot_.install_id = session.install_id;
  snapshot_.session_id = session.session_id;
  return LaunchResult::Accepted(session.session_id);
}

bool LaunchService::PersistFailure(LaunchError error, const std::string &code,
                                   const std::string &message) {
  if (!session_) return false;
  session_->phase = LaunchSessionPhase::kFailed;
  session_->failure_code = code;
  session_->failure_message = message;
  const auto persisted = journal_.PersistPending(*session_);
  snapshot_ = FailureSnapshot(*session_, error, code, message);
  if (!persisted.ok) persistence_uncertain_ = true;
  return persisted.ok;
}

LaunchResult LaunchService::Handoff() {
  if (persistence_uncertain_)
    return Reject(LaunchError::kPersistence, "launch-recovery-required",
                  "launch journal requires reconciliation");
  if (!session_ || session_->phase != LaunchSessionPhase::kPrepared)
    return Reject(LaunchError::kDuplicate, "handoff-unavailable",
                  "launch handoff is not available");
  if (recovered_pending_)
    return Reject(LaunchError::kDuplicate, "launch-recovery-required",
                  "recovered launch must be reconciled before another handoff");
  if (!dependencies_.is_application_thread ||
      !dependencies_.is_application_thread())
    return Reject(LaunchError::kWrongThread, "wrong-launch-thread",
                  "native launch must run on the application thread");
  const auto record = records_.Find(session_->install_id);
  if (!record || record->state != PreparationState::kReady || record->journal ||
      record->title_id != session_->title_id ||
      record->runtime_bundle_id != session_->runtime_bundle_id ||
      record->settings_revision != session_->settings_revision) {
    PersistFailure(LaunchError::kPreflight, "install-changed",
                   "runtime installation changed before handoff");
    return Reject(LaunchError::kPreflight, "install-changed", snapshot_.message);
  }
  if (!dependencies_.preflight) {
    PersistFailure(LaunchError::kPreflight, "preflight-unavailable",
                   "launch preflight is unavailable");
    return Reject(LaunchError::kPreflight, "preflight-unavailable",
                  snapshot_.message);
  }
  const auto checked = PreflightFailure(dependencies_.preflight(*record),
                                        session_->settings_revision);
  if (!checked.accepted) {
    PersistFailure(checked.error, "preflight-failed", checked.message);
    return Reject(checked.error, "preflight-failed", checked.message);
  }
  if (!session_->account_token.empty() &&
      (!dependencies_.current_account_token ||
       dependencies_.current_account_token() != session_->account_token)) {
    PersistFailure(LaunchError::kCloudFailed, "cloud-account-changed",
                   "Cloud account changed before handoff");
    return Reject(LaunchError::kCloudFailed, "cloud-account-changed",
                  snapshot_.message);
  }
  if (!dependencies_.now_ms) {
    PersistFailure(LaunchError::kPersistence, "clock-unavailable",
                   "launch clock is unavailable");
    return Reject(LaunchError::kPersistence, "clock-unavailable",
                  snapshot_.message);
  }
  session_->phase = LaunchSessionPhase::kHandoffBegun;
  session_->handoff_ms = dependencies_.now_ms();
  const auto committed = journal_.PersistPending(*session_);
  if (!committed.ok) {
    persistence_uncertain_ = true;
    snapshot_ = FailureSnapshot(*session_, LaunchError::kPersistence,
                                committed.code, committed.message);
    return Reject(LaunchError::kPersistence, committed.code, committed.message);
  }
  if (!dependencies_.flush) {
    PersistFailure(LaunchError::kFlush, "flush-unavailable",
                   "application state flush is unavailable");
    return Reject(LaunchError::kFlush, "flush-unavailable", snapshot_.message);
  }
  const auto flushed = dependencies_.flush();
  if (!flushed.ok) {
    PersistFailure(LaunchError::kFlush, flushed.code, flushed.message);
    return Reject(LaunchError::kFlush, flushed.code, flushed.message);
  }
  if (!dependencies_.backend) {
    PersistFailure(LaunchError::kNative, "native-backend-unavailable",
                   "native launch backend is unavailable");
    return Reject(LaunchError::kNative, "native-backend-unavailable",
                  snapshot_.message);
  }

  const auto result = dependencies_.backend(*record, session_->session_id);
  session_->has_native_result = true;
  session_->native_code = result.code;
  const auto persisted = journal_.PersistPending(*session_);
  snapshot_ = {};
  snapshot_.state = LaunchState::kLaunching;
  snapshot_.install_id = session_->install_id;
  snapshot_.session_id = session_->session_id;
  snapshot_.native_code = result.code;
  if (!persisted.ok) {
    persistence_uncertain_ = true;
    return Reject(LaunchError::kPersistence, persisted.code, persisted.message);
  }
  return LaunchResult::Accepted(session_->session_id);
}

LaunchResult LaunchService::Finish() {
  if (persistence_uncertain_)
    return Reject(LaunchError::kPersistence, "launch-recovery-required",
                  "launch journal requires reconciliation");
  if (!session_)
    return Reject(LaunchError::kInvalidSession, "launch-not-pending",
                  "no launch session is pending");
  const bool never_handed_off =
      session_->phase == LaunchSessionPhase::kPrepared && recovered_pending_;
  const auto current = never_handed_off ? snapshot_ : Snapshot();
  if (persistence_uncertain_)
    return Reject(LaunchError::kPersistence, "launch-recovery-required",
                  "launch journal requires reconciliation");
  if (!never_handed_off && current.state != LaunchState::kExited &&
      current.state != LaunchState::kFailed)
    return Reject(LaunchError::kDuplicate, "launch-undecided",
                  "the launch session has not ended");
  const auto cleared = journal_.ClearPending();
  if (!cleared.ok) {
    persistence_uncertain_ = true;
    return Reject(LaunchError::kPersistence, cleared.code, cleared.message);
  }
  const std::string session_id = session_->session_id;
  session_.reset();
  recovered_pending_ = false;
  stopped_polls_ = 0;
  snapshot_ = current;
  return LaunchResult::Accepted(session_id);
}

LaunchSnapshot LaunchService::Snapshot() {
  if (cloud_record_ || !session_) return snapshot_;
  if (persistence_uncertain_) return snapshot_;
  snapshot_.install_id = session_->install_id;
  snapshot_.session_id = session_->session_id;
  if (session_->has_native_result) snapshot_.native_code = session_->native_code;
  if (session_->phase == LaunchSessionPhase::kPrepared) {
    if (recovered_pending_) return snapshot_;
    snapshot_.state = LaunchState::kReadyToHandoff;
    return snapshot_;
  }
  if (session_->phase == LaunchSessionPhase::kFailed &&
      session_->failure_code != "launch-no-evidence") {
    return FailureSnapshot(*session_, FailureError(session_->failure_code),
                           session_->failure_code, session_->failure_message);
  }

  const auto record = records_.Find(session_->install_id);
  if (!record || !dependencies_.evidence) {
    PersistFailure(LaunchError::kInvalidEvidence, "evidence-unavailable",
                   "wrapper launch evidence is unavailable");
    return snapshot_;
  }
  const auto evidence = dependencies_.evidence(*record, session_->session_id);
  if (evidence.state != LaunchEvidenceState::kNone &&
      (!evidence.valid || evidence.session_id != session_->session_id ||
       evidence.install_id != session_->install_id ||
       evidence.title_id != session_->title_id ||
       evidence.runtime_bundle_id != session_->runtime_bundle_id ||
       evidence.settings_revision != session_->settings_revision)) {
    PersistFailure(LaunchError::kInvalidEvidence, "wrapper-evidence-invalid",
                   "wrapper launch evidence identity is invalid");
    return snapshot_;
  }
  snapshot_.error = LaunchError::kNone;
  snapshot_.code.clear();
  snapshot_.message.clear();
  snapshot_.exit_code.reset();
  const auto activity = [&] {
    if (dependencies_.activity) return dependencies_.activity(*record);
    return dependencies_.preflight ? dependencies_.preflight(*record).activity
                                   : NativeActivity::kUnknown;
  };
  switch (evidence.state) {
    case LaunchEvidenceState::kStarting:
    case LaunchEvidenceState::kRunning:
      // A title closed from the system menu, or one that crashed, never
      // writes its exit. Once it is seen stopped repeatedly, the session ended.
      if (activity() == NativeActivity::kStopped) ++stopped_polls_;
      else stopped_polls_ = 0;
      if (stopped_polls_ >= kStoppedPollsToEnd) {
        snapshot_.state = LaunchState::kExited;
        snapshot_.code = "title-ended-without-record";
        snapshot_.message = "the game ended without reporting its exit";
        return snapshot_;
      }
      snapshot_.state = evidence.state == LaunchEvidenceState::kRunning
                            ? LaunchState::kRunning
                            : LaunchState::kLaunching;
      return snapshot_;
    case LaunchEvidenceState::kFailed:
      PersistFailure(LaunchError::kNative,
                     evidence.code.empty() ? "wrapper-start-failed" : evidence.code,
                     evidence.message.empty() ? "wrapper reported startup failure"
                                              : evidence.message);
      return snapshot_;
    case LaunchEvidenceState::kExited:
      snapshot_.state = LaunchState::kExited;
      snapshot_.exit_code = evidence.exit_code;
      return snapshot_;
    case LaunchEvidenceState::kNone:
      break;
  }

  const std::int64_t now = dependencies_.now_ms ? dependencies_.now_ms() : 0;
  const std::int64_t elapsed =
      now >= session_->handoff_ms ? now - session_->handoff_ms : 0;
  if (activity() == NativeActivity::kStopped &&
      elapsed >= std::max<std::int64_t>(0, dependencies_.evidence_wait_ms)) {
    PersistFailure(LaunchError::kNative, "launch-no-evidence",
                   "native launch returned without wrapper startup evidence");
    return snapshot_;
  }
  snapshot_.state = LaunchState::kLaunching;
  return snapshot_;
}

} // namespace playden::runtime
