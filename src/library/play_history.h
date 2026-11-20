// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "library/model.h"
#include "platform/storage.h"

#include <cstdint>
#include <map>

namespace playden {

// What Playden saw of a game's play on this console (#446): when it last ran (unix seconds) and how long the
// sessions Playden started lasted. Steam hears of neither while Playden emulates it offline, and GOG and imported
// games have no play history of their own. A game started from its Home screen tile counts for `last_played` only:
// its session record says when it ran, not for how long.
struct PlayRecord {
  std::int64_t last_played = 0;
  std::uint64_t played_seconds = 0;
};
bool operator==(const PlayRecord& left, const PlayRecord& right);

using PlayHistory = std::map<GameId, PlayRecord>;

// Notes a play ending at `when` that lasted `seconds` (0: not known). `last_played` only moves forward. True when
// the history changed.
bool RecordPlay(PlayHistory& history, GameId id, std::int64_t when, std::uint64_t seconds);

// `play-history.v1` in the user storage root (which must exist), one file for the device, read and written through
// the managed tree (Storage::OpenTree): "version=1", then one
// "<game key> <last played> <seconds>" line per game. A line it can't read, or one naming a game of a store this
// build doesn't know, is skipped, so a newer Playden's entries or a damaged line never cost the others. Another
// version fails the load (kInvalidData): it is a newer Playden's file, which this one must not overwrite.
class PlayHistoryStore {
 public:
  static constexpr const char* kFile = "play-history.v1";
  explicit PlayHistoryStore(Storage& storage) : storage_(storage) {}
  // No file yet: an empty history.
  Result<PlayHistory> Load() const;
  Result<void> Save(const PlayHistory& history) const;

 private:
  Storage& storage_;
};

}  // namespace playden
