#include "steam/local_session_repository.h"
#include <cerrno>
#include <atomic>
#include <unistd.h>
#include <sys/stat.h>

namespace playden {
LocalSessionRepository::LocalSessionRepository(std::string root, DiagnosticReport report, SessionVault* vault)
    : root_(std::move(root)),storage_(root_),store_(storage_),report_(std::move(report)),vault_(vault) {}
Result<void> LocalSessionRepository::Prepare() const {
  if (prepared_) return Result<void>::Success();
  if (root_.empty() || root_.find('\0') != std::string::npos)
    return Result<void>::Failure(StorageError::kInvalidPath,"Session storage path is invalid");
  // Open, so the console owner can delete it over FTP (#868); the session file itself stays 0600.
  if (mkdir(root_.c_str(),0777) == 0) (void)chmod(root_.c_str(),0777);
  else if (errno != EEXIST) {
    const int error = errno;
    if (report_) report_("session storage mkdir failed errno=" + std::to_string(error));
    return Result<void>::Failure(StorageError::kOpenFailed,"Session storage is unavailable");
  }
  struct stat info{};
  const int stat_result = stat(root_.c_str(),&info);
  const int stat_error = stat_result ? errno : 0;
  if (stat_result != 0 || !S_ISDIR(info.st_mode)) {
    if (report_) report_("session storage stat failed errno=" + std::to_string(stat_error) +
                        " directory=" + std::to_string(S_ISDIR(info.st_mode)));
    return Result<void>::Failure(StorageError::kOpenFailed,"Session storage is unavailable");
  }
  const Bytes marker{'o','k'};
  // Each service owns a repository for this root. A shared probe (and .tmp)
  // lets one worker rename/remove another worker's file during startup.
  static std::atomic<unsigned long long> next_probe{0};
  const std::string path = "session-storage.probe-" + std::to_string(getpid()) +
                           "-" + std::to_string(next_probe.fetch_add(1, std::memory_order_relaxed));
  const auto written = storage_.WriteAtomicPrivate(path,marker);
  if (!written.ok()) {
    if (report_) report_("session storage private probe failed: " + written.message());
    return Result<void>::Failure(written.error(),"Private session storage is unavailable");
  }
  const auto loaded = storage_.Read(path,marker.size());
  const auto removed = storage_.Remove(path);
  if (!loaded.ok() || loaded.value() != marker || !removed.ok()) {
    if (report_) {
      if (!loaded.ok()) report_("session storage probe read failed: " + loaded.message());
      else if (loaded.value() != marker) report_("session storage probe contents changed: " + path);
      if (!removed.ok()) report_("session storage probe remove failed: " + removed.message());
    }
    return Result<void>::Failure(StorageError::kReadFailed,"Session storage validation failed");
  }
  prepared_ = true;
  return Result<void>::Success();
}
Result<std::optional<StoredSession>> LocalSessionRepository::LoadSaved() const {
  const auto ready = Prepare();
  if (!ready.ok()) return Result<std::optional<StoredSession>>::Failure(ready.error(),ready.message());
  auto result = store_.Load();
  if (!result.ok() && report_) report_("session storage load failed: " + result.message());
  return result;
}
Result<void> LocalSessionRepository::SaveToDisk(const StoredSession& session) const {
  const auto ready = Prepare();
  if (!ready.ok()) return ready;
  return store_.Save(session);
}
Result<void> LocalSessionRepository::ClearSaved() const {
  // Clearing must remain possible even if the private-write probe now fails.
  if (root_.empty() || root_.find('\0') != std::string::npos)
    return Result<void>::Failure(StorageError::kInvalidPath,"Session storage path is invalid");
  const auto cleared = store_.Clear();
  if (!cleared.ok() && report_) report_("session storage clear failed: " + cleared.message());
  return cleared;
}
Result<std::optional<StoredSession>> LocalSessionRepository::Load() const {
  if (!vault_) return LoadSaved();
  std::lock_guard<std::mutex> lock(vault_->mutex_);
  // The session in memory wins: it is the newest sign-in, whichever the setting.
  if (vault_->session_) return Result<std::optional<StoredSession>>::Success(*vault_->session_);
  if (!vault_->persist_) return Result<std::optional<StoredSession>>::Success(std::nullopt);
  return LoadSaved();
}
Result<void> LocalSessionRepository::Save(const StoredSession& session) const {
  if (!vault_) return SaveToDisk(session);
  std::lock_guard<std::mutex> lock(vault_->mutex_);
  if (vault_->persist_) {
    auto saved = SaveToDisk(session);
    if (saved.ok()) vault_->Drop();
    return saved;
  }
  // A session.v1 that can't be deleted fails the sign-in: the player asked for no token on disk.
  vault_->Keep(session);
  auto cleared = ClearSaved();
  if (!cleared.ok()) vault_->Drop();
  return cleared;
}
Result<void> LocalSessionRepository::Clear() const {
  if (!vault_) return ClearSaved();
  std::lock_guard<std::mutex> lock(vault_->mutex_);
  vault_->Drop();
  return ClearSaved();
}
Result<void> LocalSessionRepository::SetPersist(bool persist) const {
  if (!vault_) return Result<void>::Success();
  std::lock_guard<std::mutex> lock(vault_->mutex_);
  if (vault_->persist_ == persist) return Result<void>::Success();
  vault_->persist_ = persist;
  if (persist) {
    if (!vault_->session_) return Result<void>::Success();
    auto saved = SaveToDisk(*vault_->session_);
    // Not saved: the session stays in memory and keeps working until Playden closes.
    if (!saved.ok()) {
      if (report_) report_("session storage save failed: " + saved.message());
      return saved;
    }
    vault_->Drop();
    return saved;
  }
  if (!vault_->session_) {
    auto loaded = LoadSaved();
    if (loaded.ok() && loaded.value()) {
      vault_->Keep(*loaded.value());
      steam::ZeroizeSecret(loaded.value()->refresh_token);
      steam::ZeroizeSecret(loaded.value()->access_token);
    }
  }
  return ClearSaved();
}
}
