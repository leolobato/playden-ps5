# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Playden is a controller-first launcher that runs as a native PS5 app (title `FAKE77031`, reserved for Playden) on a
jailbroken console. It signs in to Steam, downloads games and registers each one as its own PS5 title. The same C++
code also builds as a macOS desktop app (`playden-homebrew`) for development and tests.

## Commands

Host (macOS, Apple Silicon):

```sh
git submodule update --init --recursive
scripts/fetch-deps.sh                      # prebuilt PS5 SDKs into deps/, hash-checked against dependencies.lock.json
scripts/build-homebrew.sh                  # checks the UI pin, bakes fonts, builds curl/Mbed TLS once, configures + builds
cmake --build --preset homebrew-host
ctest --preset homebrew-host --output-on-failure
ctest --preset homebrew-host -R '^runtime_launch_service$'   # one test (names match add_test in cmake/*.cmake)
build/homebrew-host/playden-homebrew --simulation --scenario signed-out   # UI with synthetic data
```

PS5 package (no console needed):

```sh
scripts/build-lapy-helper.sh               # root helper payload
scripts/build-homebrew-ps5.sh              # -> build/homebrew-ps5/dist/FAKE77031
scripts/with-env.sh <cmd>                  # run a command with the PS5 toolchain env (LLVM 18, PS5_PAYLOAD_SDK)
```

Console tools in `tools/` (Python 3, stdlib only) take `--host <PS5 IP>`: `console_title_deploy.py`,
`console_kit_upload.py`, `collect_logs.py`.

- The checkout path must not contain symlinks (e.g. macOS `/tmp`): Playden refuses symlinked data paths.
- Each test is its own executable. Register a new `tests/<name>_test.cpp` with `add_test` in the matching
  `cmake/*.cmake`. Python tests in `tests/` also run through ctest.
- Many tests have a `*_native_policy` twin: the same source built on the host with `PLAYDEN_PLATFORM_PS5=1` to run
  the console code paths. Keep both passing.
- Builds check pinned revisions and hashes. On a mismatch, find out why the input changed; never just update the hash.
  ps5-homebrew-ui must be the pinned commit with no local changes, or configure fails.

## Architecture

Playden never runs a game. The **runtime kit** (Wine for PS5 from prospero-win, Mesa/RADV, DXVK, vkd3d-proton, a
template bottle, the per-game title shell) is installed separately. For each game, Playden copies a private bottle,
writes a prospero-win profile from the game settings, publishes the title files, registers the title, and hands off.
The title shell then starts Wine.

- `src/homebrew/` is the UI. `simulation.*` is the renderer-neutral model (synthetic scenarios or Live data),
  `view.cpp` draws it, and `controller.*` bridges it to the services on the main thread. IO stays on service workers,
  and the UI never receives credential tokens. `host.cpp` and `ps5.cpp` wire everything for each platform.
- Lower layers (`stores/`, `steam/`, `library/`, `runtime/`, `platform/`, `update/`) do not know about the UI.
  `steam/backend/` is a from-scratch Steam client; no Steam client or SDK is involved.
- Only the kit pinned in `src/runtime/kit_trust.*` and `dependencies.lock.json` is accepted.
- The profile, descriptor and receipt formats are a contract with the title shell in prospero-win. They are written
  transactionally (`runtime/settings_publication*`, `publication_*`, `package_installer`) and checked with the same
  strict parser the title uses. prospero-win profiles are strict INI; `profiles.lst` holds at most 16 entries.
- All writes under Playden's data roots go through `platform::ManagedDirectory`, which enforces ownership and mode
  policies and rejects symlinks in every path component.

On the console (0.6+): user data `/data/playden/user/`, logs `/data/playden/logs/`, bottles `/data/playden/games/`,
game files `/data/playden-games/`, kits `/data/playden/kits/`, the app `/data/homebrew/FAKE77031`.

## Console constraints

Code in a PS5 title has limits the host does not. Use the helpers in `platform/` and copy how nearby code handles
the same call:

- `openat`/`mkdirat` take only `AT_FDCWD` (a real dirfd gives `EINVAL`). `fchdir` and `getcwd` crash the title.
  Walks go by absolute path (`platform/directory_walk.h`).
- The console libc lacks the locale support iostreams need. Use stdio/POSIX calls, not streams.
- `/data` drops the sticky bit, and root has no DAC bypass there. exFAT shows every entry as 0777, owned by the mount
  owner (`DirectoryPolicy::kUnownedTree`).
- Worker stacks are small and processes can't be created. `/data` needs the root grant first.
- A payload runs with different rights than a title, so a probe from a payload can mislead.

## Before reporting done

- Run the full host test suite (`ctest --preset homebrew-host --output-on-failure`). For PS5-only code, also run
  `scripts/build-homebrew-ps5.sh`.
- Host tests and a PS5 build are not console acceptance. Say plainly which console behaviour was not tested.

## Dependencies

- Forks in `third_party/` carry Playden's patches on their `main` branch. Change them in the submodule, then move the
  pin with every place that records it: `dependencies.lock.json`, and for ps5-homebrew-ui also
  `scripts/build-homebrew.sh` and `cmake/HomebrewUI.cmake`.
- Wine is not forked. Wine changes are new patch files in prospero-win's `wine/patches/`.
- The kit builds only on Linux x86-64. Changes meant for upstream must not name Playden, its paths, pins or issues.
- See docs/building.md › Dependencies for the full workflow.

## Conventions

- Match surrounding style. Source files carry GPL-3.0-or-later SPDX headers.
- Reusable UI fixes belong in ps5-homebrew-ui, product behaviour here.
- Commit format: see CONTRIBUTING.md › Commits. A commit that resolves an issue says `Fixes #N` (`Refs #N`
  for partial work).
- History is linear: rebase and fast-forward, no merge commits.

## Pull requests

These rules are for every contributor's agent (full text in CONTRIBUTING.md):

- One pull request, one change. Do not bundle a second feature or an unrelated fix; tell the user to open another
  pull request for it.
- Target `develop`, rebased on it. `main` is the release line.
- Never change the version (`CMakeLists.txt`, `PLAYDEN_PRERELEASE`), the runtime kit pin (`src/runtime/kit_trust.h`,
  the kit entry in `dependencies.lock.json`) or release files. Only the maintainer releases kits.
- Move a submodule pin only to a commit already on that fork's `main`.
- Fill in `.github/PULL_REQUEST_TEMPLATE.md`. Describe only what is in the diff and only tests that were actually run;
  say plainly what was not tested on a console.
