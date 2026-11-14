// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "gog/content.h"
#include "gog/http.h"
#include "gog/session.h"
#include "installs/install_record.h"
#include "stores/details_source.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <pthread.h>

namespace playden::stores {

// GOG's game details on their own worker: what installing a GOG game takes on this console, and whether an installed
// one has an update. Per game, the default-branch Windows build (gog::ChooseBuild), then for a gen 2 build its meta and
// the depots an install takes (gog::SelectDepots, in Settings › Game language), for a gen 1 build its repository.json
// (unsigned, plain JSON) and gog::SelectDepotsV1, as the install runner would choose them:
// - download_bytes: the depots' compressed size (gen 1 files are stored whole: their size), plus the dependencies'
//   from the public dependency store, game-folder ones (DOSBox, ScummVM, language_setup) and __redist installers
//   (MSVC, DirectX…) alike; install_bytes: their sizes.
// - The owned DLC are counted (the owned list, read once per GOG sign-in); with no usable token, the base game only.
// - A game with no Windows build is answered as not installable here, with the reason.
// - launches stay empty (an installed game's come from its install record), and so do languages, versions and DLC.
// - An installed game (SetInstalled) whose record has `gog`: update.newer when the default build's ID or generation
//   differs from the record's (GOG builds never change once published, so the build decides), with update.target
//   gog::UpdateTarget of the new build. new_dlc stays false: an update, like an install, takes every owned DLC. No
//   update when the game is not installed, its record has no `gog`, or it has no installable Windows build now.
// - No answer from GOG sets `failed`: asking again may work.
//
// Requests are deduplicated and cached for the current generation, as Steam's are; Invalidate drops what is queued
// and any answer still to come.
//
// CheckInstalled asks again for installed games, behind every page's request, and CheckProgress counts them as
// steam::AppDetailsService's check does: asked when the worker takes one, answered when Pump hands its answer out,
// failed when that answer is. kMaximumEvents bounds the pages' answers waiting for Pump (the oldest goes); the check's
// are never dropped: the worker takes no more of the check while kMaximumEvents of them wait.
//
// Measure sizes an installed game's update (SizeRequest::update), behind the check: the default build planned into
// gog::MeasureLayout (<root>/.playden-gog/measure/) through GogInstaller::Plan, seeded from the installed file list,
// in the record's language and with the owned DLC, then gog::DiffPlans against the installed plan and the files on
// disk. The measure folder is deleted afterwards whatever happened; the install's own plan, file list and redist
// list are never touched. A game with an update under way (.playden-update/update.json) is not measured. Answers are
// kept on the worker by the folder, the record's build and the new one (the default build GOG last answered for the
// game), so measuring again asks GOG nothing.
//
// gog.log gets "details result=<kind>", "details update result=newer|current|none" (installed games) and
// "details size result=measured|skipped|failed changed=N removed=N" lines with counts only: never an ID, a URL or a
// token.
class GogDetailsSource final : public DetailsSource {
 public:
  using LogSink = std::function<void(const std::string&)>;
  using InstalledLookup = std::function<std::optional<installs::InstalledRecord>(const GameId&)>;
  using RootLookup = std::function<std::optional<std::string>(const GameId&)>;

  static constexpr const char* kNoWindowsReason = "GOG has no Windows version of this game to install.";
  static constexpr std::size_t kMaximumPending = 64;
  static constexpr std::size_t kMaximumEvents = 64;
  // The worker parses build metas and makes TLS connections: an explicit stack, as the installer's planner has.
  static constexpr std::size_t kWorkerStackBytes = 1024 * 1024;

  // make_transport is called on the worker, before its first request (the transport is the worker's own), and once
  // more for each measure that plans: the planner runs on its own thread.
  // content_language is Settings › Game language as a Steam code, read for each game; empty: English.
  GogDetailsSource(gog::Session& session, std::function<gog::Transport()> make_transport,
                   std::function<std::string()> content_language, LogSink log = {});
  ~GogDetailsSource() override;
  GogDetailsSource(const GogDetailsSource&) = delete;
  GogDetailsSource& operator=(const GogDetailsSource&) = delete;

  // Set before the first request; called on the worker. The services are made before the install service, so these
  // are bound late, as steam::CmAppDetailsSource's are. An installed game's record (none: not installed), for its
  // update; an installed game's folder, for its update's size.
  void SetInstalled(InstalledLookup installed);
  void SetInstallRoot(RootLookup root);

  Store store() const override { return Store::kGog; }
  void Request(const GameId& id, std::uint64_t generation) override;
  void RequestFirst(const GameId& id, std::uint64_t generation) override;
  void Refresh(const GameId& id, std::uint64_t generation, bool first = false) override;
  void Invalidate(std::uint64_t generation) override;
  std::vector<GameDetails> Pump(std::size_t maximum) override;
  // GOG ids only, each once. False when stopping, the ids hold no GOG game, or a check of this generation runs.
  // Invalidate and a newer generation drop it (CheckProgress total 0).
  bool CheckInstalled(const std::vector<GameId>& ids, std::uint64_t generation) override;
  steam::InstalledCheck CheckProgress() override;
  // An update's size only (request.update, a GOG id); false for anything else, when stopping, or when
  // kMaximumPending wait. A request already queued is not asked twice.
  bool Measure(const steam::SizeRequest& request, std::uint64_t generation) override;
  std::vector<steam::MeasuredSize> PumpMeasured() override;
  void Shutdown() override;

 private:
  // An answer and the check that asked for it (its serial), 0 for a page's.
  struct Event {
    GameDetails details;
    std::uint64_t check = 0;
  };
  void Queue(const GameId& id, std::uint64_t generation, bool first, bool again);
  void ResetLocked(std::uint64_t generation);
  bool CheckRoomLocked() const;
  void PushLocked(GameDetails details, std::uint64_t check);
  void Run();
  GameDetails Fetch(const GameId& id, std::uint64_t generation, const std::atomic<bool>& cancel);
  steam::MeasuredSize MeasureUpdate(const steam::SizeRequest& request, std::uint64_t generation,
                                    const std::atomic<bool>& cancel);
  // The owned products for the session's sign-in; nothing when no token or list could be had.
  std::optional<std::set<std::uint64_t>> Owned(const std::atomic<bool>& cancel);
  // The dependency store's entries, read once per process; nothing when it could not be read.
  enum class Fetched { kOk, kNetwork, kProtocol, kCanceled };
  Fetched Dependencies(const std::vector<std::string>& bases, const std::atomic<bool>& cancel);
  // The dependencies among `wanted` (by ID, each once) that the store lists, game-folder and __redist alike, added to
  // the sizes; their count.
  std::size_t AddDependencies(const std::vector<std::string>& wanted, std::uint64_t* install, std::uint64_t* download) const;
  void Log(const std::string& line) const;

  gog::Session& session_;
  std::function<gog::Transport()> make_transport_;
  std::function<std::string()> content_language_;
  LogSink log_;
  InstalledLookup installed_;
  RootLookup install_root_;

  mutable std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<GameId> pending_;
  std::set<GameId> requested_;  // queued, in flight or answered for this generation
  std::deque<Event> events_;
  std::deque<GameId> check_pending_;  // the check's games not yet asked
  std::set<GameId> check_waiting_;    // asked by the check, not yet handed out by Pump
  steam::InstalledCheck check_;
  std::uint64_t check_serial_ = 0;
  std::deque<steam::SizeRequest> measure_pending_;
  std::set<steam::SizeRequest> measure_requested_;  // queued or being measured
  std::deque<steam::MeasuredSize> measured_;
  std::shared_ptr<std::atomic<bool>> active_cancel_;
  std::uint64_t generation_ = 0;
  bool stopping_ = false;
  bool started_ = false;
  bool joined_ = false;
  pthread_t worker_{};

  // The worker's own.
  gog::Transport transport_;
  bool transport_made_ = false;
  std::optional<std::vector<gog::Dependency>> dependencies_;
  std::uint64_t owned_generation_ = 0;  // the session generation `owned_` was read for; 0: none
  std::set<std::uint64_t> owned_;
  // The default build GOG last answered for each product ("<generation>:<build ID>"), by a fetch or a measure's plan.
  std::map<std::uint64_t, std::string> latest_builds_;
  // Measured updates, by the folder, the record's build and the new one; for measured_generation_ only.
  struct Measured {
    std::uint64_t download_bytes = 0;
    std::uint64_t staged_bytes = 0;
    std::uint64_t install_bytes = 0;
    std::uint64_t target = 0;
    std::uint64_t changed = 0;
    std::uint64_t removed = 0;
  };
  std::map<std::string, Measured> measured_updates_;
  std::uint64_t measured_generation_ = 0;
};

}  // namespace playden::stores
