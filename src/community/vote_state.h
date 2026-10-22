#pragma once
// vote_state.h - What this console remembers about votes (plan Phase 6): the votes it cast, the
// recipe revisions it already asked about, and whether the player turned voting on or off. Kept
// in <community store root>/votes-cast.json (0600, durable write through platform::Storage):
//   {"format_version":1,"voting":"unset"|"on"|"off",
//    "votes":[{"recipe_id":"...","revision":"<64 hex>","vote":"up"|"down","reason":"slow"}],
//    "asked":[{"recipe_id":"...","revision":"<64 hex>"}]}
// Not thread-safe: one owner loads, changes and saves it.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace playden::community {

inline constexpr const char* kVoteStateFileName = "votes-cast.json";
// The post-play prompt waits for this much play (seconds) in one session.
inline constexpr std::int64_t kVotePromptMinPlayedSeconds = 300;
// The file keeps at most this many votes and asked revisions each; the oldest go first.
inline constexpr std::size_t kVoteStateMaxEntries = 1000;

enum class VotingPreference { kUnset, kOn, kOff };

struct CastVote {
  std::string recipe_id;
  std::string revision;  // 64 hex
  std::string vote;      // "up" or "down"
  std::string reason;    // down only, optional: no-start, crash, slow, controls, other
};

struct VoteState {
  VotingPreference voting = VotingPreference::kUnset;
  std::vector<CastVote> votes;  // oldest first, one per recipe revision
  std::vector<std::pair<std::string, std::string>> asked;  // (recipe id, revision), oldest first

  const CastVote* Find(std::string_view recipe_id, std::string_view revision) const;
  bool WasAsked(std::string_view recipe_id, std::string_view revision) const;
  // Records (or replaces) the vote for its revision, and counts the revision as asked.
  void Record(const CastVote& vote);
  // The prompt was shown (answered or dismissed).
  void MarkAsked(std::string_view recipe_id, std::string_view revision);
};

// "up" or "down", and a reason that fits it ("" or one of the five, down only).
bool IsValidVote(std::string_view vote, std::string_view reason);

// Whether to ask after the player quits a game that used this recipe revision: voting not off,
// at least kVotePromptMinPlayedSeconds played, the revision not asked about before and no vote cast
// for it (Game settings may have cast one). kUnset asks: the first prompt says what is sent.
bool ShouldAskForVote(const VoteState& state, std::string_view recipe_id, std::string_view revision,
                      std::int64_t played_seconds, VotingPreference voting);

std::string SerializeVoteState(const VoteState& state);
// Lenient about entries (a bad one is skipped), strict about the shape; nothing for a file that is
// not a format_version 1 object.
std::optional<VoteState> ParseVoteState(std::string_view json);

// The state in store_root/votes-cast.json; an empty state when the file is missing or unreadable as
// JSON (*note says so, safe to log). Nothing (and *error) when the folder or file can't be used.
// store_root: an absolute normalized folder whose parent exists; created private when missing.
std::optional<VoteState> LoadVoteState(const std::string& store_root, std::string* error,
                                       std::string* note = nullptr);
bool SaveVoteState(const std::string& store_root, const VoteState& state, std::string* error);

}  // namespace playden::community
