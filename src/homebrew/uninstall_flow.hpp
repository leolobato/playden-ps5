#pragma once
// uninstall_flow.hpp - The order of an uninstall: Cloud saves, then the PS5 title, then the download.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: MIT
#include "homebrew/simulation.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace playden::homebrew {
// Uninstall keeps saves safe and never leaves a PS5 title pointing at deleted files:
// Steam Cloud syncs first (a conflict or a failure stops and asks the player), then
// the game's PS5 title is removed (journaled by the runtime host), and only then the
// Steam download. A failure stops the uninstall with the reason; nothing is skipped.
// Uninstalls go first in, first out: syncs and title removals are handed out one at a
// time, in the order the uninstalls were asked for.
enum class UninstallStep { kSync, kRemoveTitle, kRemoveDownload };

struct UninstallAction {
  // kFinished: the download is gone and the uninstall is over.
  enum class Kind { kNone, kRemoveTitle, kRemoveDownload, kStop, kFinished } kind = Kind::kNone;
  bool delete_saves = false;
  std::string reason;  // kStop: what the player is told
};

// What the runtime reports about one game right now.
struct UninstallFacts {
  bool has_title = false;            // an install record exists (Ready, failed or being removed)
  bool removing = false;             // the runtime is removing it now
  bool removal_started = false;      // its record holds a removal journal that Retry continues
  std::uint64_t removals_handled = 0;
  std::string removal_failure;       // the runtime's latest removal refusal or failure
  bool saves_moving = false;         // a Cloud transfer runs for the game
  bool session_active = false;       // the game's title may be running
  bool download_gone = false;        // the game's files are no longer installed
  bool drive_absent = false;         // the game's files are on a drive that is not connected
};

class UninstallFlow {
 public:
  // Starts an uninstall; false when one already runs for the game. With Cloud the
  // saves sync first, unless this is the uninstall right after a stop that offered going on without syncing.
  bool Begin(GameId id, bool delete_saves, bool cloud_available);
  // The game whose Cloud sync should be issued now (each once), the first waiting one in
  // order, while no other sync is out.
  std::vector<GameId> TakeSyncRequests();
  bool WaitingForCloud(GameId id) const;
  // The sync went to the Cloud service: from now on its results settle the uninstall
  // (an older status seen before that is not this sync's result).
  void SyncDispatched(GameId id);
  // The Cloud check finished: continue, or stop and ask.
  UninstallAction CloudSettled(GameId id, CloudStatus status);
  // The Cloud check could not run because the title may be running.
  UninstallAction CloudRefused(GameId id);
  // The game has nothing Cloud could sync (no folder): continue.
  void CloudSkipped(GameId id);
  // Cloud did not take the sync (stopping, or too many waiting): stop, so later uninstalls go on.
  UninstallAction SyncRefused(GameId id);
  // What to do next for a game, given what the runtime reports now.
  UninstallAction Advance(GameId id, const UninstallFacts& facts);
  // The download removal failed outside the flow: the uninstall is over.
  void Abandon(GameId id);
  std::optional<UninstallStep> Step(GameId id) const;
  // Whether the uninstall works now: its sync was sent, or its PS5 title or download is being
  // removed. One that waits its turn does not. `deletions` lists the games whose files the install
  // service deletes, in its order: it deletes one at a time, so of the games listed only the first
  // of these uninstalls is at work.
  bool Started(GameId id, const std::vector<GameId>& deletions = {}) const;
  // Uninstalls in the order they were asked for.
  std::vector<GameId> Active() const;

 private:
  struct Entry {
    UninstallStep step = UninstallStep::kSync;
    bool delete_saves = false;
    bool sync_issued = false;
    bool sync_dispatched = false;
    bool remove_sent = false;
    bool download_sent = false;
    std::uint64_t removals_before = 0;
    std::uint64_t order = 0;  // when it was asked for
  };
  UninstallAction Stop(GameId id, std::string reason);
  std::map<GameId, Entry> entries_;
  std::set<GameId> without_sync_;
  std::uint64_t next_order_ = 0;
};
}  // namespace playden::homebrew
