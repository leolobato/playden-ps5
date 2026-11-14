// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// GOG's install runner: the install service's runner for Store::kGog. It downloads a Windows build (gen 2 or gen 1)
// with gog::GogInstaller, then, on the same worker and before the game counts as installed:
// 1. reads the base product's goggame-<id>.info into the launch options the record keeps (none when it is missing or
//    unreadable: Prepare then asks for the program);
// 2. runs each installed product's goggame-<id>.script once, copies included (supportData and setIni); a script that
//    fails is logged and the install goes on;
// 3. writes .playden-install.json (FormatGogInstallRecord, with the plan's generation, the builds item's build ID
//    and the meta: the build meta's MD5, or for gen 1 repository.json's, and the DLC turned off) through a synced
//    temporary file, last;
// 4. deletes .playden-download.
//
// The DLC turned off (InstallOptions::excluded_dlc, GogExcludedDlc) are left out of the plan, as if they weren't owned,
// and the record keeps them. A resumed install keeps the products its saved plan has.
//
// An update (Update) brings an installed game to the default branch's newest build, in its work folder
// <root>/.playden-update/ (stores/gog_update.h describes it), while the game stays installed and playable:
// (a) an update.json past staging (also after a restart) is put in place at once, signed out and offline too, once
//     `may_apply` lets it: asked before anything else, false waits with kWaitingForGame;
// (b) otherwise it reads the install record (without its "gog" object the game can't be updated). A work folder staged
//     for another DLC choice than this run's (its sidecar, below) is deleted first (DiscardGogUpdate: "step=plan
//     result=restaged") and staged afresh. An update staged in full already (GogUpdateStaged: the game was running
//     when its files finished) goes on at (d) with no session, so it is put in place signed out and offline too.
//     Otherwise it needs a session as an install does, writes update.json {phase staging}, then the sidecar, deletes
//     .playden-update/record and stages the changed files with GogInstaller::Stage in the installed GOG language,
//     leaving out the DLC GogExcludedDlc names (the record's, so a DLC turned off stays off, or with
//     `options.dlc_choice` the options' alone) (kCheckingFiles while it hashes the game's files for want of an
//     installed plan, then kDownloading);
// (c) once Stage finished, writes the new record to .playden-update/record (synced, with the new build, size, file
//     count, the play tasks of the new info file when the update staged one, else the installed one, and the DLC
//     left out less any product the plan holds) before the phase leaves staging, so an interrupted apply finishes
//     from it;
// (d) checks every folder the new build needs can be made (a file the game wrote in the way fails the update while
//     the game is still playable), then asks `may_apply`: false waits with kWaitingForGame, everything staged;
// (e) sets the phase to replacing, reports kApplying and ApplyGogUpdate puts the files in place, running each
//     installed product's script as an install does (supportData never over an existing file, setIni) before the
//     record goes in place.
// DiscardUpdate deletes a canceled update's work folder (DiscardGogUpdate), refused once replacing has started.
//
// Manage DLC's change (`options.dlc_choice`) is an update: the installed DLC become exactly the owned ones
// `options.excluded_dlc` doesn't name. Stage plans the default branch's newest build as for any update, so a newer
// build comes with the change, and on the same build DiffPlans decides: a DLC turned on is downloaded, and a change
// that only turns DLC off stages nothing (kNone) and the apply deletes their files. The record's products become the
// plan's, its "excluded" the new choice. A change needs the session and the owned list as an update does: signed out
// it waits with kWaitingForSignIn, the list out of reach with kWaitingForGog, planning nothing (no file is deleted
// for want of the list). A DLC turned on that GOG no longer lists as owned, or that the meta lacks, isn't planned;
// the stage line counts them as dlc_missing.
//
// The sidecar <root>/.playden-update/dlc (kGogUpdateDlcName) names the choice a staging was planned with: the
// excluded GOG IDs, one decimal per line, sorted, empty for none. It is written synced right after update.json
// {staging} is created (or when it is missing), before Stage; one that is missing (staging from a build before it)
// stands for the record's "excluded", one that can't be read for another choice. It goes with the work folder
// (ApplyGogUpdate's cleanup, DiscardGogUpdate, RemoveManagedInstall). Past staging an update always finishes as
// recorded, at (a).
//
// A DLC the update leaves out (turned off) loses what the new plan no longer holds: its depot files, its support
// files, its goggame-<id> files and its redist entries; a path it shared with the base game gets the base game's
// version again. What its script did outside its own files stays: supportData copies into the game folder, setIni keys
// and the registry values written at Play. Play runs only the record's products' scripts, so it no longer runs.
//
// No GOG session (signed out, or GOG refused it, also mid-download) makes the job wait with kWaitingForSignIn: it
// keeps its place and its partial download, and the next sign-in's RetryWaiting(Store::kGog) carries it on from the
// journal. GOG or its CDN out of reach (the token's refresh, the builds or meta fetch, a chunk that failed its five
// tries mid-download) makes it wait with kWaitingForGog: it keeps its journal and tries again by itself after the
// service's growing wait, or at once on RetryWaiting(Store::kGog).
//
// install.log gets "gog install game=gog-<id> step=<plan|download|script|record> result=<..>" and "gog update
// game=gog-<id> step=<plan|stage|record|script|apply|finish> result=<..>" lines with counts (the stage line's
// dlc_missing too), "gog update-discard result=<..>", and the installer's own lines: product IDs, counts and endpoint
// names, never the account, a token, a path or an address.

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
#include <vector>

namespace playden::stores {

// The DLC a GOG run leaves out, mirroring Steam's ExcludedDlcFor. An install: the options' GOG DLC. An update: those
// and what the install `record` left out, so a DLC turned off stays off; one that changes the game's DLC
// (`options.dlc_choice`, Manage DLC): the options' alone, which can turn DLC on again. Only GOG IDs, never `base`,
// sorted, without repeats and at most installs::kMaxGogRecordProducts.
std::vector<std::uint64_t> GogExcludedDlc(const installs::InstallOptions& options, bool update,
                                          const std::optional<installs::InstalledRecord>& record, std::uint64_t base);

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
  // The update described at the top. It keeps the installed language; `options` choose only the DLC left out
  // (GogExcludedDlc), and with `dlc_choice` change the game's DLC.
  Result Update(const GameId& id, const std::string& root, const installs::InstallOptions& options,
                const std::atomic<bool>& cancel,
                const std::function<void(std::uint64_t done, std::uint64_t total, std::uint64_t held)>& progress,
                const std::function<void(installs::InstallStep)>& step, const std::function<bool()>& may_apply) override;
  // DiscardGogUpdate: empty, or why not.
  std::string DiscardUpdate(const std::string& root) override;
  // steam::RemoveManagedInstall: the game's files, .playden-gog and the support files go, and both versions' files
  // during an update.
  std::string Remove(const std::string& root) override;
  // "GOG", or "GOG · fastly" once a chunk came from a CDN endpoint GOG names.
  std::string DownloadServerName() const override;

 private:
  Result Run(const GameId& id, std::uint64_t product, const std::string& root, const installs::InstallOptions& options,
             const std::atomic<bool>& cancel,
             const std::function<void(std::uint64_t, std::uint64_t, std::uint64_t)>& progress);
  // After a complete download: launches, scripts, the record (with `excluded`, less the plan's products), then
  // .playden-download goes.
  Result Finish(const GameId& id, std::uint64_t product, const std::string& root, const std::string& steam_language,
                const gog::GogInstaller::Outcome& outcome, const std::vector<std::uint64_t>& excluded);
  Result RunUpdate(const GameId& id, std::uint64_t product, const std::string& root,
                   const installs::InstallOptions& options, const std::atomic<bool>& cancel,
                   const std::function<void(std::uint64_t, std::uint64_t, std::uint64_t)>& progress,
                   const std::function<void(installs::InstallStep)>& step, const std::function<bool()>& may_apply);
  // ApplyGogUpdate with the install scripts, its outcome as a Result.
  Result Apply(const GameId& id, const std::string& root, const std::atomic<bool>& cancel);
  // A token now: false with `result` saying why the job stops or waits. `game` starts the log line.
  bool SessionReady(const std::atomic<bool>& cancel, const std::string& game, Result* result);
  // Whether a download's error ends the run, with `result` saying how; kNone goes on.
  bool Stopped(gog::GogInstaller::Outcome::Error error, const std::atomic<bool>& cancel, Result* result);
  gog::GogInstaller MakeInstaller();
  // Each of `products`' goggame-<id>.script at the top of `root`, once with its copies.
  void RunScripts(const std::string& game, const std::string& root, const std::vector<std::uint64_t>& products,
                  const std::string& gog_language);
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
