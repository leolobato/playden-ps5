// uninstall_flow.cpp - The order of an uninstall: Cloud saves, then the PS5 title, then the download.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: MIT
#include "homebrew/uninstall_flow.hpp"

#include <algorithm>

namespace playden::homebrew {

bool UninstallFlow::Begin(GameId id, bool delete_saves, bool cloud_available) {
  if (entries_.count(id)) return false;
  Entry entry;
  entry.delete_saves = delete_saves;
  // Going on without syncing is allowed once, by the uninstall right after the stop that offered it.
  const bool without_sync = without_sync_.erase(id) > 0;
  entry.step = cloud_available && !without_sync ? UninstallStep::kSync : UninstallStep::kRemoveTitle;
  entry.order = next_order_++;
  entries_[id] = entry;
  return true;
}

std::vector<GameId> UninstallFlow::TakeSyncRequests() {
  // One sync at a time: the next waits until the one out settles, so the order holds and only
  // the game being synced says so.
  for (const auto& [id, entry] : entries_)
    if (entry.step == UninstallStep::kSync && entry.sync_issued) return {};
  for (const auto id : Active())
    if (auto& entry = entries_[id]; entry.step == UninstallStep::kSync) {
      entry.sync_issued = true;
      return {id};
    }
  return {};
}

bool UninstallFlow::WaitingForCloud(GameId id) const {
  const auto found = entries_.find(id);
  return found != entries_.end() && found->second.step == UninstallStep::kSync && found->second.sync_issued;
}

void UninstallFlow::SyncDispatched(GameId id) {
  if (const auto found = entries_.find(id); found != entries_.end() && found->second.step == UninstallStep::kSync)
    found->second.sync_dispatched = true;
}

UninstallAction UninstallFlow::Stop(GameId id, std::string reason) {
  entries_.erase(id);
  return {UninstallAction::Kind::kStop, false, std::move(reason)};
}

UninstallAction UninstallFlow::CloudSettled(GameId id, CloudStatus status) {
  if (!WaitingForCloud(id) || !entries_[id].sync_dispatched || status == CloudStatus::Checking ||
      status == CloudStatus::Syncing)
    return {};
  switch (status) {
    case CloudStatus::Synced:
    case CloudStatus::UpToDate:
    case CloudStatus::Unsupported:
      entries_[id].step = UninstallStep::kRemoveTitle;
      return {};
    case CloudStatus::Conflict:
    case CloudStatus::NeedsAccount:
      return Stop(id, "Choose which Cloud saves to keep, then uninstall again.");
    case CloudStatus::Waiting:
      // Not set up (no bottle the game reads): no sync or choice can apply, so none is asked for (#1395). Its
      // folder or an unusable bottle may still hold saves Cloud never got, so the player is told once.
      without_sync_.insert(id);
      return Stop(id, "Its saves can't sync until the game is set up. Uninstall again to go on; Steam Cloud keeps its saves.");
    default:
      without_sync_.insert(id);
      return Stop(id, "Cloud saves could not be synced. Uninstall again to go on without syncing.");
  }
}

UninstallAction UninstallFlow::CloudRefused(GameId id) {
  if (!WaitingForCloud(id)) return {};
  return Stop(id, "The game's PS5 title may still be running. Close it, then uninstall again.");
}

UninstallAction UninstallFlow::SyncRefused(GameId id) {
  if (!WaitingForCloud(id)) return {};
  return Stop(id, "Steam Cloud could not take the sync now. Uninstall again to try again.");
}

void UninstallFlow::CloudSkipped(GameId id) {
  if (const auto found = entries_.find(id); found != entries_.end() && found->second.step == UninstallStep::kSync)
    found->second.step = UninstallStep::kRemoveTitle;
}

UninstallAction UninstallFlow::Advance(GameId id, const UninstallFacts& facts) {
  const auto found = entries_.find(id);
  if (found == entries_.end()) return {};
  auto& entry = found->second;
  // Nothing can be deleted on a drive that is not there: the uninstall stops before it removes the
  // PS5 title (which would leave the files without one), or before it asks for the download.
  static constexpr const char* kDriveAbsent = "Its drive is not connected. Connect it, then uninstall again.";
  if (facts.drive_absent && entry.step != UninstallStep::kRemoveDownload &&
      !(entry.step == UninstallStep::kRemoveTitle && entry.remove_sent))
    return Stop(id, kDriveAbsent);
  if (entry.step == UninstallStep::kRemoveTitle) {
    if (!entry.remove_sent) {
      if (!facts.has_title) {
        entry.step = UninstallStep::kRemoveDownload;
      } else {
        // Saves must not move, and the title must not run, while it is removed.
        if (facts.saves_moving || facts.session_active) return {};
        // The runtime counts removals for all games together, so only one may be in flight to tell whose finished.
        for (const auto& [other_id, other] : entries_)
          if (other_id != id && other.step == UninstallStep::kRemoveTitle && other.remove_sent) return {};
        entry.remove_sent = true;
        entry.removals_before = facts.removals_handled;
        return {UninstallAction::Kind::kRemoveTitle, entry.delete_saves, {}};
      }
    } else {
      if (facts.removing || facts.removals_handled <= entry.removals_before) return {};
      // A refusal before the removal began changed nothing: there is no removal for Retry to continue.
      // A failure stops the uninstall even when the runtime lists no title: its install list can be
      // empty because it could not be read, while the PS5 title still points at the game's files.
      if (facts.has_title || !facts.removal_failure.empty())
        return Stop(id, "The PS5 title could not be removed: " +
                            (facts.removal_failure.empty() ? std::string("unknown reason.") : facts.removal_failure) +
                            (facts.removal_started ? " The game stays installed; Retry on its PS5 title continues."
                                                   : " The game stays installed."));
      entry.step = UninstallStep::kRemoveDownload;
    }
  }
  if (entry.step == UninstallStep::kRemoveDownload) {
    // The uninstall lasts until the files are gone; a failure to delete them comes back as Abandon.
    if (!entry.download_sent) {
      if (facts.drive_absent) return Stop(id, kDriveAbsent);
      entry.download_sent = true;
      return {UninstallAction::Kind::kRemoveDownload, false, {}};
    }
    if (!facts.download_gone) return {};
    entries_.erase(found);
    return {UninstallAction::Kind::kFinished, false, {}};
  }
  return {};
}

void UninstallFlow::Abandon(GameId id) { entries_.erase(id); }

std::optional<UninstallStep> UninstallFlow::Step(GameId id) const {
  const auto found = entries_.find(id);
  return found == entries_.end() ? std::nullopt : std::optional<UninstallStep>(found->second.step);
}

bool UninstallFlow::Started(GameId id, const std::vector<GameId>& deletions) const {
  const auto found = entries_.find(id);
  if (found == entries_.end()) return false;
  const auto& entry = found->second;
  switch (entry.step) {
    case UninstallStep::kSync: return entry.sync_dispatched;
    case UninstallStep::kRemoveTitle: return entry.remove_sent;
    case UninstallStep::kRemoveDownload: break;
  }
  if (!entry.download_sent) return false;
  // Not listed: removed elsewhere (an imported game leaving the library), or just asked for.
  if (std::find(deletions.begin(), deletions.end(), id) == deletions.end()) return true;
  for (const auto other : deletions)
    if (const auto listed = entries_.find(other);
        listed != entries_.end() && listed->second.step == UninstallStep::kRemoveDownload && listed->second.download_sent)
      return other == id;
  return false;
}

std::vector<GameId> UninstallFlow::Active() const {
  std::vector<std::pair<std::uint64_t, GameId>> ordered;
  for (const auto& [id, entry] : entries_) ordered.emplace_back(entry.order, id);
  std::sort(ordered.begin(), ordered.end());
  std::vector<GameId> ids;
  for (const auto& [order, id] : ordered) ids.push_back(id);
  return ids;
}
}  // namespace playden::homebrew
