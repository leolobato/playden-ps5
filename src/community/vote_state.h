#pragma once
// vote_state.h - What this console remembers about votes and feedback (plans 2026-10-22 Phase 6 and
// 2026-11-06 Phase B): the votes it cast, the recipe revisions it already asked about, whether the
// player turned voting on or off, and which games it asked "How did it run?" about. Kept in
// <community store root>/votes-cast.json (0600, durable write through platform::Storage):
//   {"format_version":1,"voting":"unset"|"on"|"off",
//    "votes":[{"recipe_id":"...","revision":"<64 hex>","vote":"up"|"down","reason":"slow"}],
//    "asked":[{"recipe_id":"...","revision":"<64 hex>"}],
//    "feedback":[{"game":"steam-440","kit":"0.19","settings":"<16 hex>","prompts":1,"answered":false}]}
// "feedback" came later in the same format_version: older Playden versions skip it.
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
// A session this long (seconds) asks "How did it run?"; a shorter one "Did it start?".
inline constexpr std::int64_t kFeedbackMinPlayedSeconds = 180;
// Prompts per game, runtime kit version and settings before Playden stops asking (Not now counts).
inline constexpr int kFeedbackMaxPrompts = 2;
// The file keeps at most this many votes and asked revisions each; the oldest go first.
inline constexpr std::size_t kVoteStateMaxEntries = 1000;

enum class VotingPreference { kUnset, kOn, kOff };

struct CastVote {
  std::string recipe_id;
  std::string revision;  // 64 hex
  std::string vote;      // "up" or "down"
  std::string reason;    // down only, optional: no-start, crash, slow, controls, other
};

// The feedback prompts for one game on one runtime kit version, with the settings last asked about.
struct FeedbackSeen {
  std::string game;      // ToInstallId: [a-z0-9-], at most 64 bytes
  std::string kit;       // the runtime kit version; "unknown" when Playden can't tell
  std::string settings;  // FeedbackSettingsFingerprint of the settings asked about: 16 hex
  int prompts = 0;       // prompts shown for those settings
  bool answered = false; // a report was sent for those settings
};

enum class FeedbackQuestion { kNone, kHowDidItRun, kDidItStart };

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

  std::vector<FeedbackSeen> feedback;  // oldest first, one per game and kit
  const FeedbackSeen* FindFeedback(std::string_view game, std::string_view kit) const;
  // A feedback prompt was shown, or a report sent (answered), for these settings. Other settings
  // than the ones last asked about start the count again.
  void NoteFeedback(std::string_view game, std::string_view kit, std::string_view settings, bool answered);
};

// "up" or "down", and a reason that fits it ("" or one of the five, down only).
bool IsValidVote(std::string_view vote, std::string_view reason);

// What to ask after the player quits a game Playden started: nothing when the player turned asking off, nothing once a
// report was sent for these settings or after kFeedbackMaxPrompts prompts; otherwise "How did it
// run?" after kFeedbackMinPlayedSeconds, "Did it start?" after a shorter session. New settings (or a
// new runtime kit version) ask again. Recipe votes don't matter here: with them off the answer is a report only.
FeedbackQuestion NextFeedbackQuestion(const VoteState& state, std::string_view game, std::string_view kit,
                                      std::string_view settings, std::int64_t played_seconds, bool ask);
// 16 lowercase hex characters (FNV-1a 64) of a text that describes a game's settings.
std::string FeedbackSettingsFingerprint(std::string_view settings_text);

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
