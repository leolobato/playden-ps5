#pragma once
// share_decision.h - Whether sharing a game's settings makes a new recipe, updates this console's
// own, needs the player to choose, or sends nothing and offers a vote (plan Phase 5, item 3).
// Pure: no files, no network, no GameSettingsRecord.
//
//   | Case                                                        | Result                         |
//   |-------------------------------------------------------------|--------------------------------|
//   | No community recipe in use, no saved submission             | new recipe                     |
//   | No community recipe in use, a saved submission              | update own (submission_id)     |
//   | The saved submission was closed and its recipe never published | new recipe (submission_id)  |
//   | Based on a recipe this console shared (saved recipe id)     | update own (based_on, sub. id) |
//   | Based on someone else's recipe, with changes                | ask the player                 |
//   | Based on someone else's recipe, no changes                  | no share; offer a thumbs up    |
//
// A settings-only recipe (community.format_version 2) as the base:
//
//   | The player's program                                        | Result                         |
//   |-------------------------------------------------------------|--------------------------------|
//   | Not changed (Playden's, or the one the recipe names)        | a settings-only share, by the  |
//   |                                                             | rows above (update or new)     |
//   | Changed (a program row: ShareInputs::program_changed)       | new recipe with its program    |
//   |                                                             | (based_on, "new")              |
//
// A share that names its program is never an update of a settings-only recipe: that would turn a recipe for every
// copy into one for that program only.
//
// When the player is asked, "Suggest changes" is pre-selected when program and graphics are the
// same as the base's, "Share as a new recipe" when either differs.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <optional>
#include <string>

namespace playden::community {

struct RecipeRef {
  std::string recipe_id;
  std::string revision;  // 64 hex
};

struct ShareInputs {
  // The community recipe the game is based on (GameSettingsRecord::community_recipe); nothing for
  // Playden's default or a preset.
  std::optional<RecipeRef> based_on;
  // based_on is another game's recipe (chosen from a search by name): credited, never updated from this game.
  bool based_on_other_game = false;
  // based_on is a settings-only recipe.
  bool based_on_settings_only = false;
  // The player changed a row that chooses the program or how it starts (runtime::IsProgramRow).
  bool program_changed = false;
  // The game's saved share (GameSettingsRecord::shared_recipe); empty when it was never shared.
  std::string saved_submission_id;
  std::string saved_recipe_id;
  // The saved submission's pull request was closed without merging and its recipe isn't published: there is nothing
  // to update, so sharing again makes the recipe anew (the API reuses its name from the submission id).
  bool saved_closed_unpublished = false;
  // Whether the effective settings differ from the base recipe's (any row of the Changes list).
  bool differs_from_base = false;
  // Whether the program (launch entry, arguments) or graphics row differ from the base's.
  bool program_or_graphics_differ = false;
};

enum class ShareKind {
  kNewRecipe,           // the API makes a new file; based_on only credits another game's recipe
  kUpdateOwn,           // send based_on (when set) and the saved submission id, no choice
  kAskPlayer,           // show "Suggest changes" / "Share as a new recipe"
  kNoChangesOfferVote,  // no request; offer a thumbs up for based_on
};

enum class ShareChoice { kNone, kUpdate, kNew };

struct ShareDecision {
  ShareKind kind = ShareKind::kNewRecipe;
  // kAskPlayer only: the answer pre-selected (kUpdate = "Suggest changes to <title>"); kNone otherwise.
  ShareChoice preselect = ShareChoice::kNone;
  // What the request carries (kNoChangesOfferVote: nothing is sent).
  bool send_based_on = false;
  bool send_submission_id = false;
  // kNewRecipe based on a recipe of this game: the request says "new", so the API doesn't take it for an update.
  bool send_new = false;
  // The recipe sent is settings-only (runtime::BuildSettingsOnlyRecipe), not one with its program.
  bool settings_only = false;
};

ShareDecision DecideShare(const ShareInputs& inputs);

// The `choice` field to send: the player's answer for kAskPlayer (kNone if they have not answered,
// which the caller must not send), kNew when send_new, kNone for every other kind: the API decides those.
ShareChoice ChoiceToSend(const ShareDecision& decision, ShareChoice player_answer);

}  // namespace playden::community
