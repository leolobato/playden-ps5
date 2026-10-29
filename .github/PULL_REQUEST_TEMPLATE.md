<!-- Thanks for contributing! Read CONTRIBUTING.md first: one pull request, one change.
     Describe only what is in this pull request, and say what you tested. -->

## Problem

<!-- What was wrong or missing from a player's point of view?
     Link the issue. If there is none, this paragraph is the issue. -->

## What changed

<!-- Describe the resulting behaviour in a few sentences, not the functions you edited.
     For UI changes, include a screenshot or short video when useful. -->

## How it was tested

<!-- Describe what you did and what happened.
     For console tests, include firmware, runtime kit, and game/App ID.
     Attach logs for launch or runtime changes.

     Host tests and PS5 builds are useful, but aren't console testing.
     If you couldn't test on a console, leave that box unchecked
     and say what still needs checking. Mark unrelated checks N/A. -->

- [ ] Host tests pass: `ctest --preset homebrew-host --output-on-failure`
- [ ] PS5 package builds: `scripts/build-homebrew-ps5.sh`
- [ ] Tested on a PS5, with steps and results described above
- [ ] Added or updated tests for changed behaviour, where applicable

## Scope

- [ ] This pull request does one thing. Unrelated fixes and features are in their own pull requests.
- [ ] It targets `develop` and is rebased on it.
- [ ] It does not change the version, the runtime kit pin (`src/runtime/kit_trust.h`, the kit entry in
      `dependencies.lock.json`) or release files.

## Submodule changes

<!-- Delete this section if no submodule pins changed.
     For each changed pin, link the merged pull request on the fork.
     Link an upstream PR when the change is useful upstream. -->

- [ ] Each new pin is a commit already on the fork's `main`
- [ ] Playden's submodule pointers and corresponding revision records
      are updated together, including `dependencies.lock.json`
- [ ] For ps5-homebrew-ui, the revisions in `scripts/build-homebrew.sh`
      and `cmake/HomebrewUI.cmake` are also updated

## AI tools

<!-- Did you use an AI coding tool? Which one, with which model, and roughly
     how much of the change did it write? "All of it" is a fine answer.
     Either way, you've read the result, checked this description against
     the diff, and run it yourself. -->
