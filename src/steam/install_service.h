#pragma once

#include "installs/install_service.h"
#include "steam/session_store.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace playden::steam {

// The install service moved to src/installs/; these names keep Steam code compiling.
using installs::CompletedDownload;
using installs::CompletedDownloads;
using installs::InstallJobKind;
using installs::InstallJobState;
using installs::InstallJobView;
using installs::InstallOptions;
using installs::InstalledGame;
using installs::InstallRequest;
using installs::InstallRunner;
using installs::InstallService;
using installs::InstallSnapshot;
using installs::InstallStep;
using installs::InstallVolume;
using installs::kCompletedDays;
using installs::kMaxCompleted;
using installs::ParseInstallRequests;

// Steam's install runner: it reads the saved Steam session itself and installs by app ID. Without
// a session (Playden may still be restoring it, or nobody signed in) the job waits for the sign-in.
class SessionInstallRunner : public InstallRunner {
 public:
  static constexpr const char* kWaitingForSignIn = "Waiting for the Steam sign-in.";
  static constexpr const char* kWaitingForSteam = "Waiting to reach Steam. It tries again by itself.";

  explicit SessionInstallRunner(SessionRepository& sessions) : sessions_(sessions) {}
  Result Install(const GameId& id, const std::string& root, const InstallOptions& options,
                 const std::atomic<bool>& cancel,
                 const std::function<void(std::uint64_t, std::uint64_t, std::uint64_t)>& progress,
                 const std::function<void(InstallStep)>& step) final {
    return Start(id, root, options, cancel, progress, step, false);
  }
  // An update that is already putting its files in place finishes without a session, once `may_apply` lets it.
  Result Update(const GameId& id, const std::string& root, const InstallOptions& options,
                const std::atomic<bool>& cancel,
                const std::function<void(std::uint64_t, std::uint64_t, std::uint64_t)>& progress,
                const std::function<void(InstallStep)>& step, const std::function<bool()>& may_apply) final {
    if (id.steam_app()) {
      if (const auto state = installs::ReadUpdateState(root);
          state && state->phase != installs::UpdatePhase::kStaging && may_apply && !may_apply()) {
        Result waiting;
        waiting.retry_later = true;
        waiting.waiting = kWaitingForGame;
        return waiting;
      }
      if (auto finished = FinishUpdate(root, cancel, step)) return *finished;
    }
    return Start(id, root, options, cancel, progress, step, true, may_apply);
  }

 protected:
  // No saved session when a job starts: what the runner keeps for an account can go.
  virtual void SignedOut() {}
  virtual Result InstallApp(const StoredSession& session, std::uint32_t app, const std::string& root,
                            const InstallOptions& options, const std::atomic<bool>& cancel,
                            const std::function<void(std::uint64_t, std::uint64_t, std::uint64_t)>& progress,
                            const std::function<void(InstallStep)>& step) = 0;
  // Default: Steam games can't be updated.
  virtual Result UpdateApp(const StoredSession& session, std::uint32_t app, const std::string& root,
                           const InstallOptions& options, const std::atomic<bool>& cancel,
                           const std::function<void(std::uint64_t, std::uint64_t, std::uint64_t)>& progress,
                           const std::function<void(InstallStep)>& step, const std::function<bool()>& may_apply) {
    (void)session;
    return InstallRunner::Update(GameId::Steam(app), root, options, cancel, progress, step, may_apply);
  }
  // Finishes an update at `root` that is putting its files in place; nothing when there is none.
  virtual std::optional<Result> FinishUpdate(const std::string& root, const std::atomic<bool>& cancel,
                                             const std::function<void(InstallStep)>& step) {
    (void)root; (void)cancel; (void)step;
    return std::nullopt;
  }

 private:
  Result Start(const GameId& id, const std::string& root, const InstallOptions& options,
               const std::atomic<bool>& cancel,
               const std::function<void(std::uint64_t, std::uint64_t, std::uint64_t)>& progress,
               const std::function<void(InstallStep)>& step, bool update,
               const std::function<bool()>& may_apply = {}) {
    Result result;
    const auto app = id.steam_app();
    if (!app) {
      result.failure = "This game can't be downloaded.";
      return result;
    }
    const auto session = sessions_.Load();
    if (!session.ok() || !session.value()) {
      SignedOut();
      result.retry_later = true;
      result.waiting = kWaitingForSignIn;
      return result;
    }
    result = update ? UpdateApp(*session.value(), *app, root, options, cancel, progress, step, may_apply)
                    : InstallApp(*session.value(), *app, root, options, cancel, progress, step);
    if (result.retry_later && result.waiting.empty()) result.waiting = kWaitingForSteam;
    return result;
  }

  SessionRepository& sessions_;
};

}  // namespace playden::steam
