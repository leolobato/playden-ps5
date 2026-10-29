# Contributing to Playden for PS5

Thanks for helping! Playden is maintained by one person in spare time, and most testing means sitting in front of
a PS5. The rules below are here so a change can be understood, tested and released quickly. They are not meant to
be gatekeeping; they make the review easier for everyone.

- [Ways to help](#ways-to-help)
- [Report a game test](#report-a-game-test)
- [One pull request, one change](#one-pull-request-one-change)
- [Files that stay out of pull requests](#files-that-stay-out-of-pull-requests)
- [Before you open a pull request](#before-you-open-a-pull-request)
- [Describe the problem and what you tested](#describe-the-problem-and-what-you-tested)
- [Commits](#commits)
- [Using AI tools](#using-ai-tools)

To build Playden, run its tests or put it on a console, see [docs/building.md](docs/building.md).

## Ways to help

- **Test games.** Play a game from your Steam library and [send a test report](../../issues/new?template=test-report.yml),
  whether it worked or not. See [Report a game test](#report-a-game-test).
- **Add a recipe for a game.** A recipe tells Playden how to start one game. See
  [Community recipes](docs/community-recipes.md) for the format, the shipped recipes and the registry values a
  recipe may set.
- **Fix a bug or add a feature.** Look for an open issue. For a large change, open an issue first (see below).
- **Improve the upstream projects.** Many fixes belong in prospero-win, ps5-homebrew-ui or PS5_Mesa. See
  [Dependencies](docs/building.md#dependencies).

## Report a game test

1. Check the [compatibility list](COMPATIBILITY.md). If the game is there, report only what is new.
2. Play the game from Playden or from its tile on the Home screen.
3. If the game did not work, start Playden once more. When it starts, Playden copies the game's newest log to
   `/data/playden/logs/title-<TITLE>.log`.
4. Collect the logs on your computer (Python 3, no packages needed). The PS5 must run an FTP server on port 2120,
   such as zftpd:

   ```sh
   python3 tools/collect_logs.py --host <PS5 IP>
   ```

   The script lists the games that have a log, the most recently run first, and asks which one to collect. It
   writes `playden-logs-<time>.zip` and prints a table with your firmware, Playden package and kit. It reads only
   Playden's `logs/` folder, the games' names and the kit manifest, never your Steam sign-in, and it replaces your
   Steam account name and SteamIDs in the logs. Use `--all-titles` to collect every game.
5. [Open a test report](../../issues/new?template=test-report.yml), paste the table and attach the zip. The
   [sample report](docs/sample-test-report.md) shows what to fill in.

## One pull request, one change

A pull request fixes one bug or adds one feature. If you find yourself writing "also", "additionally" or "while I
was there" in the description, split it.

- A pull request that adds a feature does not also fix unrelated bugs, even small ones. Open a second pull request
  for the fix. Small fixes are easy to review on their own and can ship right away.
- A pull request that fixes a bug does not also refactor, rename or reformat code it did not need to touch.
- If two changes are independent, they are two pull requests. If one needs the other, open the one it needs first
  and mention it in the second.
- A change to a fork in `third_party/` or to the kit is its own pull request on that project, before the Playden
  one (see [Files that stay out of pull requests](#files-that-stay-out-of-pull-requests)).

A good rule of thumb: if a reviewer could reasonably want to merge half of the pull request and reject the other
half, it should have been two. Google's [Small CLs](https://google.github.io/eng-practices/review/developer/small-cls.html)
guide explains why.

**Open an issue first** for large changes: a new feature, anything that touches Steam sign-in or credentials, the
runtime kit, the title shell contract or the install pipeline. Describe the problem and the approach. This keeps you
from spending a weekend on something that conflicts with work in progress or with where the project is going.

Pull requests target `develop`, which holds the next release. Rebase on `develop` rather than merging it in: the history
is linear. `main` is the release line; the maintainer brings fixes there.

## Files that stay out of pull requests

These are changed only at release time. Including them conflicts with every other open pull request or breaks
consoles that update:

- **The version**: `project(PlaydenLauncher VERSION …)` in `CMakeLists.txt` and `PLAYDEN_PRERELEASE`.
- **The runtime kit**: its pin in `src/runtime/kit_trust.h` and its entry in `dependencies.lock.json`. Playden accepts
  only the pinned kit, so a pin to a kit that is not released stops games from starting for every player. A change
  that needs a new kit (a Wine patch, a new library) goes to prospero-win, and the issue says so; kits are built and
  released with Playden releases.
- **Fork pins**: a submodule in `third_party/` may only point at a commit already on that fork's `main`. The pull
  request on the fork comes first; once it is merged, a Playden pull request may move the pin.
- **Prebuilt binaries** (DLLs and other files in `runtime/`): the pull request includes the source and the build
  script. The binaries are rebuilt and their hashes checked before merging.
- **Releases, tags and release notes** ([docs/releasing.md](docs/releasing.md)).

A merged pull request ships in the next release.

## Before you open a pull request

```sh
ctest --preset homebrew-host --output-on-failure   # host tests (macOS)
scripts/build-homebrew-ps5.sh                      # PS5 package, for code that runs on the console
```

[docs/building.md](docs/building.md) explains the setup. New behaviour comes with tests; changed behaviour comes with
changed tests. Many tests have a `*_native_policy` twin that runs the console code paths on the host: keep both
passing.

Match the style of the code around your change. Product behaviour belongs in this repository; reusable UI fixes
belong in ps5-homebrew-ui.

## Describe the problem and what you tested

Fill in the [pull request template](.github/PULL_REQUEST_TEMPLATE.md). The description answers, in this order:

1. **What was wrong**, from a player's point of view. "Games installed on a USB drive lose their saves after an
   update" is a problem. "Refactor the cloud mapping" is not.
2. **Why it happened**, briefly, if you know.
3. **What changed** in behaviour. Not a list of functions edited.
4. **How you tested it**, on a console: firmware, kit, the game and its App ID, what you did and what happened.
   Attach logs for launch or runtime changes. Host tests and a PS5 build are necessary but are not console testing.
   If you could not test something on a console, say so.

Describe only what is in the pull request. Link the issue it addresses; if there is none, the first paragraph is
the issue.

## Commits

- Subject under 60 characters. In the body, `-` bullets that describe the visible behaviour, not the diff.
- A commit that resolves an issue says `Fixes #N` in its body; partial work says `Refs #N`.
- Each commit builds and passes the tests on its own, and the commits tell the story of the change. Fixup and
  "address review" commits are fine during review; they are squashed before merging.

## Using AI tools

**You are welcome to use AI coding tools.** Most of Playden was written with them. Two things to keep in mind:

1. **Say so.** The pull request template asks which tool and model you used and roughly how much of the change it
   wrote. This is not a judgement; it tells the maintainer where to look harder.
2. **You are the author.** You must be able to explain why each change is there, and you must have run the result
   on a PS5.

AI tools are especially prone to the problems this page is about. They bundle unrelated features into one change,
bump versions and pins, fix nearby things nobody asked about, and write long, confident descriptions of changes
that were never verified, or that are not in the diff at all. Before you open the pull request, check that it does
one thing and that every sentence in the description is true.

Coding agents read [CLAUDE.md](CLAUDE.md) (also `AGENTS.md`), which repeats these rules for them.

## Review

Expect a few questions, and possibly a request to split the pull request. Reviews can take a while, because testing
means sitting in front of a console. A pull request that follows this page gets through much faster, because it can
be understood and tested in one sitting. Thanks for your patience, and for contributing!

## License

Playden's source files are marked GPL-3.0-or-later (SPDX headers). The submodules and vendored sources keep their
own licenses. Game recipes (`runtime/recipes/`) are CC0 1.0 ([LICENSE](runtime/recipes/LICENSE)): by contributing a
recipe, you release it under CC0 1.0.
