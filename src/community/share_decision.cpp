// share_decision.cpp - Update or new recipe when sharing (plan Phase 5, item 3).
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "community/share_decision.h"

namespace playden::community {

ShareDecision DecideShare(const ShareInputs& inputs) {
  ShareDecision decision;
  const bool saved = !inputs.saved_submission_id.empty();
  if (saved && inputs.saved_closed_unpublished && !inputs.based_on) {
    decision.kind = ShareKind::kNewRecipe;
    decision.send_submission_id = true;
    return decision;
  }
  if (!inputs.based_on) {
    // A second share of a game not based on a community recipe updates its first one.
    decision.kind = saved ? ShareKind::kUpdateOwn : ShareKind::kNewRecipe;
    decision.send_submission_id = saved;
    return decision;
  }
  // A settings-only base whose program the player kept: the share is settings-only too, and follows the usual rules.
  decision.settings_only = inputs.based_on_settings_only && !inputs.program_changed;
  if (inputs.based_on_settings_only && inputs.program_changed) {
    // Shared with its program, an update would make the recipe one program's: a new recipe that credits it. That holds
    // for another game's too, and the game's own earlier share isn't updated either (it may be settings-only).
    decision.kind = ShareKind::kNewRecipe;
    decision.send_based_on = true;
    decision.send_new = true;
    return decision;
  }
  if (inputs.based_on_other_game) {
    // Another game's recipe: Suggest changes is only for a recipe of the same game, so this is a recipe for this game
    // that credits it, changed or not; this game's own saved share is updated instead.
    decision.kind = saved ? ShareKind::kUpdateOwn : ShareKind::kNewRecipe;
    decision.send_based_on = true;
    decision.send_submission_id = saved;
    return decision;
  }
  if (saved && !inputs.saved_recipe_id.empty() && inputs.saved_recipe_id == inputs.based_on->recipe_id) {
    // This console's own recipe (in review or published): an update, whatever changed.
    decision.kind = ShareKind::kUpdateOwn;
    decision.send_based_on = true;
    decision.send_submission_id = true;
    return decision;
  }
  // Someone else's recipe. A saved submission for another recipe is not sent: it would turn a
  // share based on this recipe into an update of the other one.
  if (!inputs.differs_from_base) {
    decision.kind = ShareKind::kNoChangesOfferVote;
    return decision;
  }
  decision.kind = ShareKind::kAskPlayer;
  decision.preselect = inputs.program_or_graphics_differ ? ShareChoice::kNew : ShareChoice::kUpdate;
  decision.send_based_on = true;
  return decision;
}

ShareChoice ChoiceToSend(const ShareDecision& decision, ShareChoice player_answer) {
  if (decision.kind == ShareKind::kAskPlayer) return player_answer;
  return decision.send_new ? ShareChoice::kNew : ShareChoice::kNone;
}

}  // namespace playden::community
