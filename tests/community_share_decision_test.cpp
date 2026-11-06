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

  if (failures) std::fprintf(stderr, "%d check(s) failed\n", failures);
  return failures ? 1 : 0;
}
