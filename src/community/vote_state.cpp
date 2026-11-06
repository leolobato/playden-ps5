// vote_state.cpp - What this console remembers about votes.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "community/vote_state.h"

#include <algorithm>

#include "community/install_secret.h"
#include "platform/storage.h"
#include "steam/backend/json.h"

namespace playden::community {
namespace {

bool IsRevisionText(std::string_view text) {
  if (text.size() != 64) return false;
  for (char c : text)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  return true;
}

// A recipe id as the file keeps it: printable ASCII without quotes or backslashes, at most 64 bytes.
bool IsRecipeIdText(std::string_view text) {
  if (text.empty() || text.size() > 64) return false;
  for (char c : text)
    if (static_cast<unsigned char>(c) < 0x21 || static_cast<unsigned char>(c) > 0x7e || c == '"' || c == '\\')
      return false;
  return true;
}

// ToInstallId's alphabet: [a-z0-9-], at most 64 bytes.
bool IsFeedbackKey(std::string_view text) {
  if (text.empty() || text.size() > 64) return false;
  for (char c : text)
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
  return true;
}

// A runtime kit version ("0.19", "0.10-dev.20261016.1") or "unknown".
bool IsKitText(std::string_view text) {
  if (text.empty() || text.size() > 40) return false;
  for (char c : text)
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-')) return false;
  return true;
}

bool IsFingerprintText(std::string_view text) {
  if (text.size() != 16) return false;
  for (char c : text)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  return true;
}

const char* VotingName(VotingPreference voting) {
  switch (voting) {
    case VotingPreference::kOn: return "on";
    case VotingPreference::kOff: return "off";
    case VotingPreference::kUnset: break;
  }
  return "unset";
}

template <typename T>
void KeepNewest(std::vector<T>* items) {
  if (items->size() > kVoteStateMaxEntries)
    items->erase(items->begin(), items->begin() + static_cast<std::ptrdiff_t>(items->size() - kVoteStateMaxEntries));
}

}  // namespace

const CastVote* VoteState::Find(std::string_view recipe_id, std::string_view revision) const {
  for (const auto& vote : votes)
    if (vote.recipe_id == recipe_id && vote.revision == revision) return &vote;
  return nullptr;
}

bool VoteState::WasAsked(std::string_view recipe_id, std::string_view revision) const {
  for (const auto& [id, rev] : asked)
    if (id == recipe_id && rev == revision) return true;
  return false;
}

void VoteState::Record(const CastVote& vote) {
  for (auto it = votes.begin(); it != votes.end(); ++it)
    if (it->recipe_id == vote.recipe_id && it->revision == vote.revision) {
      votes.erase(it);
      break;
    }
  votes.push_back(vote);
  KeepNewest(&votes);
  MarkAsked(vote.recipe_id, vote.revision);
}

void VoteState::MarkAsked(std::string_view recipe_id, std::string_view revision) {
  if (WasAsked(recipe_id, revision)) return;
  asked.emplace_back(std::string(recipe_id), std::string(revision));
  KeepNewest(&asked);
}

bool IsValidVote(std::string_view vote, std::string_view reason) {
  if (vote == "up") return reason.empty();
  if (vote != "down") return false;
  return reason.empty() || reason == "no-start" || reason == "crash" || reason == "slow" || reason == "controls" ||
         reason == "other";
}

const FeedbackSeen* VoteState::FindFeedback(std::string_view game, std::string_view kit) const {
  for (const auto& seen : feedback)
    if (seen.game == game && seen.kit == kit) return &seen;
  return nullptr;
}

void VoteState::NoteFeedback(std::string_view game, std::string_view kit, std::string_view settings, bool answered) {
  FeedbackSeen seen{std::string(game), std::string(kit), std::string(settings), 0, false};
  for (auto it = feedback.begin(); it != feedback.end(); ++it)
    if (it->game == game && it->kit == kit) {
      if (it->settings == settings) seen = *it;
      feedback.erase(it);
      break;
    }
  if (answered) seen.answered = true;
  else ++seen.prompts;
  feedback.push_back(std::move(seen));
  KeepNewest(&feedback);
}

FeedbackQuestion NextFeedbackQuestion(const VoteState& state, std::string_view game, std::string_view kit,
                                      std::string_view settings, std::int64_t played_seconds, VotingPreference voting) {
  if (voting == VotingPreference::kOff || game.empty()) return FeedbackQuestion::kNone;
  if (const auto* seen = state.FindFeedback(game, kit); seen && seen->settings == settings &&
                                                        (seen->answered || seen->prompts >= kFeedbackMaxPrompts))
    return FeedbackQuestion::kNone;
  return played_seconds >= kFeedbackMinPlayedSeconds ? FeedbackQuestion::kHowDidItRun : FeedbackQuestion::kDidItStart;
}

std::string FeedbackSettingsFingerprint(std::string_view settings_text) {
  std::uint64_t hash = 14695981039346656037ull;
  for (const char c : settings_text) {
    hash ^= static_cast<unsigned char>(c);
    hash *= 1099511628211ull;
  }
  static const char* digits = "0123456789abcdef";
  std::string text(16, '0');
  for (int i = 15; i >= 0; --i, hash >>= 4) text[static_cast<std::size_t>(i)] = digits[hash & 0xF];
  return text;
}

std::string SerializeVoteState(const VoteState& state) {
  // Every string is checked on the way in, so nothing here needs escaping; anything else is left out.
  std::string text = std::string("{\"format_version\":1,\"voting\":\"") + VotingName(state.voting) + "\",\"votes\":[";
  bool first = true;
  for (const auto& vote : state.votes) {
    if (!IsRecipeIdText(vote.recipe_id) || !IsRevisionText(vote.revision) || !IsValidVote(vote.vote, vote.reason))
      continue;
    text += std::string(first ? "" : ",") + "{\"recipe_id\":\"" + vote.recipe_id + "\",\"revision\":\"" +
            vote.revision + "\",\"vote\":\"" + vote.vote + "\"" +
            (vote.reason.empty() ? std::string() : ",\"reason\":\"" + vote.reason + "\"") + "}";
    first = false;
  }
  text += "],\"asked\":[";
  first = true;
  for (const auto& [id, revision] : state.asked) {
    if (!IsRecipeIdText(id) || !IsRevisionText(revision)) continue;
    text += std::string(first ? "" : ",") + "{\"recipe_id\":\"" + id + "\",\"revision\":\"" + revision + "\"}";
    first = false;
  }
  text += "],\"feedback\":[";
  first = true;
  for (const auto& seen : state.feedback) {
    if (!IsFeedbackKey(seen.game) || !IsKitText(seen.kit) || !IsFingerprintText(seen.settings) || seen.prompts < 0) continue;
    text += std::string(first ? "" : ",") + "{\"game\":\"" + seen.game + "\",\"kit\":\"" + seen.kit + "\",\"settings\":\"" +
            seen.settings + "\",\"prompts\":" + std::to_string(std::min(seen.prompts, 99)) +
            ",\"answered\":" + (seen.answered ? "true" : "false") + "}";
    first = false;
  }
  return text + "]}\n";
}

std::optional<VoteState> ParseVoteState(std::string_view json) {
  steam::json::Limits limits;
  limits.max_values = 16 * kVoteStateMaxEntries;
  const auto doc = steam::json::Parse(json, limits);
  if (!doc || !doc->is_object()) return std::nullopt;
  const auto* format = doc->Find("format_version");
  if (!format || !format->is_number() || format->AsUint64() != 1u) return std::nullopt;
  VoteState state;
  if (const auto* voting = doc->Find("voting"); voting && voting->is_string()) {
    if (voting->text() == "on") state.voting = VotingPreference::kOn;
    else if (voting->text() == "off") state.voting = VotingPreference::kOff;
  }
  const auto text = [](const steam::json::Value& object, const char* key) -> std::string {
    const auto* value = object.Find(key);
    return value && value->is_string() ? value->text() : std::string();
  };
  if (const auto* votes = doc->Find("votes"); votes && votes->is_array())
    for (const auto& item : votes->array()) {
      if (!item.is_object()) continue;
      CastVote vote{text(item, "recipe_id"), text(item, "revision"), text(item, "vote"), text(item, "reason")};
      if (!IsRecipeIdText(vote.recipe_id) || !IsRevisionText(vote.revision) || !IsValidVote(vote.vote, vote.reason))
        continue;
      state.Record(vote);
    }
  if (const auto* asked = doc->Find("asked"); asked && asked->is_array())
    for (const auto& item : asked->array()) {
      if (!item.is_object()) continue;
      const std::string id = text(item, "recipe_id"), revision = text(item, "revision");
      if (IsRecipeIdText(id) && IsRevisionText(revision)) state.MarkAsked(id, revision);
    }
  if (const auto* feedback = doc->Find("feedback"); feedback && feedback->is_array())
    for (const auto& item : feedback->array()) {
      if (!item.is_object()) continue;
      FeedbackSeen seen{text(item, "game"), text(item, "kit"), text(item, "settings"), 0, false};
      if (const auto* prompts = item.Find("prompts"); prompts && prompts->is_number())
        seen.prompts = static_cast<int>(std::min<std::uint64_t>(prompts->AsUint64().value_or(0), 99));
      if (const auto* answered = item.Find("answered"); answered && answered->is_bool()) seen.answered = answered->boolean();
      if (!IsFeedbackKey(seen.game) || !IsKitText(seen.kit) || !IsFingerprintText(seen.settings)) continue;
      state.feedback.push_back(std::move(seen));
      KeepNewest(&state.feedback);
    }
  return state;
}

std::optional<VoteState> LoadVoteState(const std::string& store_root, std::string* error, std::string* note) {
  if (note) note->clear();
  if (!PrepareCommunityRoot(store_root, error)) return std::nullopt;
  const auto read = Storage(store_root).ReadDurable(kVoteStateFileName, 1 << 20);
  if (!read.ok()) {
    if (read.error() == StorageError::kNotFound) return VoteState{};
    if (error) *error = std::string("cannot read the vote memory: ") + read.message();
    return std::nullopt;
  }
  const std::string text(read.value().begin(), read.value().end());
  if (auto state = ParseVoteState(text)) return state;
  if (note) *note = "community: vote memory was malformed; starting empty";
  return VoteState{};
}

bool SaveVoteState(const std::string& store_root, const VoteState& state, std::string* error) {
  if (!PrepareCommunityRoot(store_root, error)) return false;
  const std::string text = SerializeVoteState(state);
  const auto result = Storage(store_root).WriteDurable(kVoteStateFileName, Bytes(text.begin(), text.end()));
  if (!result.ok()) {
    if (error) *error = std::string("cannot write the vote memory: ") + result.message();
    return false;
  }
  return true;
}

}  // namespace playden::community
