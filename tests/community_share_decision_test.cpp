// community_share_decision_test.cpp - Update or new recipe when sharing (plan Phase 5, item 3):
// every row of the decision table, the pre-selection rule and the choice sent.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "community/share_decision.h"

#include <cstdio>
#include <string>

using namespace playden::community;

namespace {

int failures = 0;
#define CHECK(cond)                                                                 \
  do {                                                                              \
    if (!(cond)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                                   \
    }                                                                               \
  } while (0)

const RecipeRef kTheirs{"community-steam-274520-120hz-left-stick-pointer", std::string(64, 'a')};
const RecipeRef kMine{"community-steam-274520-mouse-preset", std::string(64, 'b')};

}  // namespace

int main() {
  // No community recipe in use, never shared: a new recipe, whatever changed.
  for (bool differs : {false, true}) {
    ShareInputs inputs;
    inputs.differs_from_base = differs;
    inputs.program_or_graphics_differ = differs;
    const auto decision = DecideShare(inputs);
    CHECK(decision.kind == ShareKind::kNewRecipe);
    CHECK(decision.preselect == ShareChoice::kNone);
    CHECK(!decision.send_based_on && !decision.send_submission_id);
    CHECK(ChoiceToSend(decision, ShareChoice::kUpdate) == ShareChoice::kNone);
  }

  // No community recipe in use, shared before: the second share updates the saved submission.
  {
    ShareInputs inputs;
    inputs.saved_submission_id = "s1";
    inputs.saved_recipe_id = "community-steam-274520-darkwood";
    inputs.differs_from_base = true;
    const auto decision = DecideShare(inputs);
    CHECK(decision.kind == ShareKind::kUpdateOwn);
    CHECK(decision.send_submission_id && !decision.send_based_on);
    CHECK(decision.preselect == ShareChoice::kNone);
    CHECK(ChoiceToSend(decision, ShareChoice::kNew) == ShareChoice::kNone);
  }

  // Shared before, but that pull request was closed unmerged and the recipe never published: nothing to update, so a
  // new recipe that still sends the submission id (the API reuses its name).
  {
    ShareInputs inputs;
    inputs.saved_submission_id = "s1";
    inputs.saved_recipe_id = "community-steam-274520-darkwood";
    inputs.saved_closed_unpublished = true;
    inputs.differs_from_base = true;
    const auto decision = DecideShare(inputs);
    CHECK(decision.kind == ShareKind::kNewRecipe);
    CHECK(decision.send_submission_id && !decision.send_based_on);
    CHECK(ChoiceToSend(decision, ShareChoice::kUpdate) == ShareChoice::kNone);
  }

  // Based on a recipe this console shared: an update, with or without changes, no question.
  for (bool differs : {false, true})
    for (bool program : {false, true}) {
      ShareInputs inputs;
      inputs.based_on = kMine;
      inputs.saved_submission_id = "s2";
      inputs.saved_recipe_id = kMine.recipe_id;
      inputs.differs_from_base = differs;
      inputs.program_or_graphics_differ = program;
      const auto decision = DecideShare(inputs);
      CHECK(decision.kind == ShareKind::kUpdateOwn);
      CHECK(decision.send_based_on && decision.send_submission_id);
      CHECK(decision.preselect == ShareChoice::kNone);
      CHECK(ChoiceToSend(decision, ShareChoice::kNew) == ShareChoice::kNone);
    }

  // Based on someone else's recipe, with changes: the player chooses. Suggest changes is
  // pre-selected when program and graphics are the same, a new recipe when either differs.
  {
    ShareInputs inputs;
    inputs.based_on = kTheirs;
    inputs.differs_from_base = true;
    inputs.program_or_graphics_differ = false;
    auto decision = DecideShare(inputs);
    CHECK(decision.kind == ShareKind::kAskPlayer);
    CHECK(decision.preselect == ShareChoice::kUpdate);
    CHECK(decision.send_based_on && !decision.send_submission_id);
    CHECK(ChoiceToSend(decision, ShareChoice::kUpdate) == ShareChoice::kUpdate);
    CHECK(ChoiceToSend(decision, ShareChoice::kNew) == ShareChoice::kNew);
    inputs.program_or_graphics_differ = true;
    decision = DecideShare(inputs);
    CHECK(decision.kind == ShareKind::kAskPlayer);
    CHECK(decision.preselect == ShareChoice::kNew);
  }

  // Based on someone else's recipe, no changes: nothing to send, offer a thumbs up.
  {
    ShareInputs inputs;
    inputs.based_on = kTheirs;
    const auto decision = DecideShare(inputs);
    CHECK(decision.kind == ShareKind::kNoChangesOfferVote);
    CHECK(!decision.send_based_on && !decision.send_submission_id);
    CHECK(decision.preselect == ShareChoice::kNone);
  }

  // Edge: the game was shared as one recipe, then switched to someone else's. The saved submission
  // belongs to the other recipe, so it is not sent: the share is treated as based on theirs.
  {
    ShareInputs inputs;
    inputs.based_on = kTheirs;
    inputs.saved_submission_id = "s3";
    inputs.saved_recipe_id = kMine.recipe_id;
    inputs.differs_from_base = true;
    auto decision = DecideShare(inputs);
    CHECK(decision.kind == ShareKind::kAskPlayer);
    CHECK(!decision.send_submission_id);
    inputs.differs_from_base = false;
    decision = DecideShare(inputs);
    CHECK(decision.kind == ShareKind::kNoChangesOfferVote);
  }

  // A saved submission without a recipe id never matches a base.
  {
    ShareInputs inputs;
    inputs.based_on = kTheirs;
    inputs.saved_submission_id = "s4";
    inputs.differs_from_base = true;
    CHECK(DecideShare(inputs).kind == ShareKind::kAskPlayer);
  }

  // Based on another game's recipe (chosen from a search): never Suggest changes, which the API refuses across games.
  // A new recipe for this game that credits it, changed or not; this game's own saved share is updated instead.
  for (bool differs : {false, true})
    for (bool program : {false, true}) {
      ShareInputs inputs;
      inputs.based_on = kTheirs;
      inputs.based_on_other_game = true;
      inputs.differs_from_base = differs;
      inputs.program_or_graphics_differ = program;
      auto decision = DecideShare(inputs);
      CHECK(decision.kind == ShareKind::kNewRecipe);
      CHECK(decision.send_based_on && !decision.send_submission_id);
      CHECK(decision.preselect == ShareChoice::kNone);
      CHECK(ChoiceToSend(decision, ShareChoice::kUpdate) == ShareChoice::kNone);
      inputs.saved_submission_id = "s5";
      inputs.saved_recipe_id = kMine.recipe_id;
      decision = DecideShare(inputs);
      CHECK(decision.kind == ShareKind::kUpdateOwn);
      CHECK(decision.send_based_on && decision.send_submission_id);
      CHECK(ChoiceToSend(decision, ShareChoice::kUpdate) == ShareChoice::kNone);
    }

  // Based on a settings-only recipe, the player's program kept: a settings-only share, by the usual rules (Suggest
  // changes pre-selected unless graphics changed; this console's own recipe updated; nothing changed, a thumbs up).
  for (bool graphics : {false, true}) {
    ShareInputs inputs;
    inputs.based_on = kTheirs;
    inputs.based_on_settings_only = true;
    inputs.differs_from_base = true;
    inputs.program_or_graphics_differ = graphics;
    const auto decision = DecideShare(inputs);
    CHECK(decision.settings_only && decision.kind == ShareKind::kAskPlayer && decision.send_based_on && !decision.send_new);
    CHECK(decision.preselect == (graphics ? ShareChoice::kNew : ShareChoice::kUpdate));
    CHECK(ChoiceToSend(decision, ShareChoice::kUpdate) == ShareChoice::kUpdate);
  }
  {
    ShareInputs inputs;
    inputs.based_on = kMine;
    inputs.based_on_settings_only = true;
    inputs.saved_submission_id = "s2";
    inputs.saved_recipe_id = kMine.recipe_id;
    inputs.differs_from_base = true;
    auto decision = DecideShare(inputs);
    CHECK(decision.settings_only && decision.kind == ShareKind::kUpdateOwn && decision.send_submission_id);
    inputs.based_on = kTheirs;
    inputs.saved_recipe_id.clear();
    inputs.saved_submission_id.clear();
    inputs.differs_from_base = false;
    decision = DecideShare(inputs);
    CHECK(decision.kind == ShareKind::kNoChangesOfferVote);
  }
  // The player changed the program: a recipe with its program, always new (an update would make the settings-only
  // recipe one program's), whoever shared the base, saying "new" so the API doesn't ask.
  for (bool mine : {false, true}) {
    ShareInputs inputs;
    inputs.based_on = mine ? kMine : kTheirs;
    inputs.based_on_settings_only = true;
    if (mine) inputs.saved_submission_id = "s2", inputs.saved_recipe_id = kMine.recipe_id;
    inputs.differs_from_base = inputs.program_or_graphics_differ = inputs.program_changed = true;
    const auto decision = DecideShare(inputs);
    CHECK(!decision.settings_only && decision.kind == ShareKind::kNewRecipe && decision.send_based_on && decision.send_new);
    CHECK(!decision.send_submission_id && ChoiceToSend(decision, ShareChoice::kUpdate) == ShareChoice::kNew);
  }
  // Another game's settings-only recipe: credited as another game's, updating this game's own earlier share while the
  // program is kept (settings-only); with the program changed, always new, never that share (it may be settings-only).
  for (bool program : {false, true})
    for (bool saved : {false, true}) {
      ShareInputs inputs;
      inputs.based_on = kTheirs;
      inputs.based_on_other_game = inputs.based_on_settings_only = true;
      inputs.program_changed = program;
      if (saved) inputs.saved_submission_id = "s3", inputs.saved_recipe_id = "community-steam-274520-mine";
      const auto decision = DecideShare(inputs);
      CHECK(decision.send_based_on && decision.settings_only == !program);
      if (program)
        CHECK(decision.kind == ShareKind::kNewRecipe && !decision.send_submission_id &&
              ChoiceToSend(decision, ShareChoice::kNone) == ShareChoice::kNew);
      else
        CHECK(decision.kind == (saved ? ShareKind::kUpdateOwn : ShareKind::kNewRecipe) && decision.send_submission_id == saved &&
              ChoiceToSend(decision, ShareChoice::kNone) == ShareChoice::kNone);
    }
  // A program change on a format 1 base changes nothing in the decision.
  {
    ShareInputs inputs;
    inputs.based_on = kTheirs;
    inputs.differs_from_base = inputs.program_or_graphics_differ = inputs.program_changed = true;
    const auto decision = DecideShare(inputs);
    CHECK(!decision.settings_only && decision.kind == ShareKind::kAskPlayer && decision.preselect == ShareChoice::kNew);
  }

  if (failures) std::fprintf(stderr, "%d check(s) failed\n", failures);
  return failures ? 1 : 0;
}
