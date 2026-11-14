// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "gog/content.h"
#include "gog/http.h"
#include "gog/session.h"
#include "stores/details_source.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <pthread.h>

namespace playden::stores {

// GOG's game details on their own worker: what installing a GOG game takes on this console. Per game, the
// default-branch Windows build (gog::ChooseBuild), then for a gen 2 build its meta and the depots an install takes
// (gog::SelectDepots, in Settings › Game language), as the install runner would choose them:
// - download_bytes: the depots' compressed size, plus the game-folder dependencies' (DOSBox, ScummVM) from the public
//   dependency store; install_bytes: their sizes.
// - The owned DLC are counted (the owned list, read once per GOG sign-in); with no usable token, the base game only.
// - A gen 1 only game, or one with no Windows build, is answered as not installable here, with the reason.
// - launches stay empty (an installed game's come from its install record), and so do languages, versions and DLC.
// - No answer from GOG sets `failed`: asking again may work.
//
// Requests are deduplicated and cached for the current generation, as Steam's are; Invalidate drops what is queued
// and any answer still to come. gog.log gets "details result=<kind>" lines with counts only: never an ID, a URL or
// a token.
class GogDetailsSource final : public DetailsSource {
 public:
  using LogSink = std::function<void(const std::string&)>;

  static constexpr const char* kGen1Reason =
      "Playden can't install this GOG game yet: it uses an older GOG download format.";
  static constexpr const char* kNoWindowsReason = "GOG has no Windows version of this game to install.";
  static constexpr std::size_t kMaximumPending = 64;
  static constexpr std::size_t kMaximumEvents = 64;
  // The worker parses build metas and makes TLS connections: an explicit stack, as the installer's planner has.
  static constexpr std::size_t kWorkerStackBytes = 1024 * 1024;

  // make_transport is called once, on the worker, before its first request: the transport is the worker's own.
  // content_language is Settings › Game language as a Steam code, read for each game; empty: English.
  GogDetailsSource(gog::Session& session, std::function<gog::Transport()> make_transport,
                   std::function<std::string()> content_language, LogSink log = {});
  ~GogDetailsSource() override;
  GogDetailsSource(const GogDetailsSource&) = delete;
  GogDetailsSource& operator=(const GogDetailsSource&) = delete;

  Store store() const override { return Store::kGog; }
  void Request(const GameId& id, std::uint64_t generation) override;
  void RequestFirst(const GameId& id, std::uint64_t generation) override;
  void Refresh(const GameId& id, std::uint64_t generation, bool first = false) override;
  void Invalidate(std::uint64_t generation) override;
  std::vector<GameDetails> Pump(std::size_t maximum) override;
  void Shutdown() override;

 private:
  void Queue(const GameId& id, std::uint64_t generation, bool first, bool again);
  void ResetLocked(std::uint64_t generation);
  void Run();
  GameDetails Fetch(const GameId& id, std::uint64_t generation, const std::atomic<bool>& cancel);
  // The owned products for the session's sign-in; nothing when no token or list could be had.
  std::optional<std::set<std::uint64_t>> Owned(const std::atomic<bool>& cancel);
  // The dependency store's entries, read once per process; nothing when it could not be read.
  enum class Fetched { kOk, kNetwork, kProtocol, kCanceled };
  Fetched Dependencies(const std::vector<std::string>& bases, const std::atomic<bool>& cancel);
  void Log(const std::string& line) const;

  gog::Session& session_;
  std::function<gog::Transport()> make_transport_;
  std::function<std::string()> content_language_;
  LogSink log_;

  mutable std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<GameId> pending_;
  std::set<GameId> requested_;  // queued, in flight or answered for this generation
  std::deque<GameDetails> events_;
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
};

}  // namespace playden::stores
