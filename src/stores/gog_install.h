// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// GOG's install runner: the install service's runner for Store::kGog. It downloads a Windows build (gen 2 or gen 1)
// with gog::GogInstaller, then, on the same worker and before the game counts as installed:
// 1. reads the base product's goggame-<id>.info into the launch options the record keeps (none when it is missing or
//    unreadable: Prepare then asks for the program);
// 2. runs each installed product's goggame-<id>.script once, copies included (supportData and setIni); a script that
//    fails is logged and the install goes on;
// 3. writes .playden-install.json (FormatGogInstallRecord, with the plan's generation, the builds item's build ID
//    and the meta: the build meta's MD5, or for gen 1 repository.json's) through a synced temporary file, last;
// 4. deletes .playden-download.
//
// No GOG session (signed out, or GOG refused it, also mid-download) makes the job wait with kWaitingForSignIn: it
// keeps its place and its partial download, and the next sign-in's RetryWaiting(Store::kGog) carries it on from the
// journal. GOG or its CDN out of reach (the token's refresh, the builds or meta fetch, a chunk that failed its five
// tries mid-download) makes it wait with kWaitingForGog: it keeps its journal and tries again by itself after the
// service's growing wait, or at once on RetryWaiting(Store::kGog).
//
// install.log gets "gog install game=gog-<id> step=<plan|download|script|record> result=<..>" lines with counts, and
// the installer's own lines: product IDs, counts and endpoint names, never the account, a token or an address.

#include "gog/http.h"
#include "gog/installer.h"
#include "gog/session.h"
#include "installs/install_service.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>

namespace playden::stores {

class GogInstallRunner final : public installs::InstallRunner {
 public:
  static constexpr const char* kWaitingForSignIn = "Downloads paused · Sign in to GOG";
  static constexpr const char* kWaitingForGog = "Waiting to reach GOG. It tries again by itself.";

  // Free bytes of the drive holding a folder; nothing when it can't be read.
  using FreeBytes = std::function<std::optional<std::uint64_t>(const std::string& path)>;

  // Tests: the installer's parallel downloads, its retry pause and its wall clock (empty: the defaults).
  struct Tuning {
    int parallel = gog::kDefaultParallel;
    std::function<void(std::uint32_t ms, const std::atomic<bool>& cancel)> pause;
    std::function<std::int64_t()> now_seconds;
  };

  // `make_transport` makes one transport per thread that downloads (the planner and each worker), only while an
  // install runs. `content_language` is Settings › Game language as a Steam code, read when a download starts.
  // `log` takes install.log's lines, from any thread. `free_bytes` empty: steam::QueryVolumeSpace.
  GogInstallRunner(gog::Session& session, std::function<gog::Transport()> make_transport,
                   std::function<std::string()> content_language, std::function<void(const std::string&)> log,
                   FreeBytes free_bytes = {});

  // Before the first install.
  void SetTuning(Tuning tuning) { tuning_ = std::move(tuning); }

  // `options.language` (a Steam code) chooses the content language, else Settings', else English. A resumed download
  // keeps the language its saved plan has.
  Result Install(const GameId& id, const std::string& root, const installs::InstallOptions& options,
                 const std::atomic<bool>& cancel,
                 const std::function<void(std::uint64_t done, std::uint64_t total, std::uint64_t held)>& progress,
                 const std::function<void(installs::InstallStep)>& step) override;
  // steam::RemoveManagedInstall: the game's files, .playden-gog and the support files go.
  std::string Remove(const std::string& root) override;
  // "GOG", or "GOG · fastly" once a chunk came from a CDN endpoint GOG names.
  std::string DownloadServerName() const override;

 private:
  Result Run(const GameId& id, std::uint64_t product, const std::string& root, const installs::InstallOptions& options,
             const std::atomic<bool>& cancel,
             const std::function<void(std::uint64_t, std::uint64_t, std::uint64_t)>& progress);
  // After a complete download: launches, scripts, the record, then .playden-download goes.
  Result Finish(const GameId& id, std::uint64_t product, const std::string& root, const std::string& steam_language,
                const gog::GogInstaller::Outcome& outcome);
  std::uint64_t Free(const std::string& path) const;
  void Log(const std::string& line) const;

  gog::Session& session_;
  std::function<gog::Transport()> make_transport_;
  std::function<std::string()> content_language_;
  std::function<void(const std::string&)> log_;
  FreeBytes free_bytes_;
  Tuning tuning_;

  mutable std::mutex endpoint_mutex_;
  std::string endpoint_;  // the last chunk's endpoint name; empty before one
};

}  // namespace playden::stores
