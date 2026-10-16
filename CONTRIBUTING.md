# Contributing to Playden for PS5

Playden is a controller-first launcher for Windows games on a jailbroken PS5. It signs in to Steam, downloads
games, prepares a private Wine bottle for each one with [prospero-win](https://github.com/mpereiraesaa/prospero-win),
and registers each game as its own PS5 title.

This guide covers setup, builds, tests, running on a console, and how to change a dependency.

## Ways to help

- **Test games.** Play a game from your Steam library and [send a test report](../../issues/new?template=test-report.yml),
  whether it worked or not. See [Report a game test](#report-a-game-test).
- **Add a recipe for a game.** A recipe tells Playden how to start one game. See
  [Community recipes](docs/community-recipes.md) for the format, the shipped recipes and the registry values a
  recipe may set.
- **Fix a bug or add a feature.** Look for an open issue, or open one to discuss a bigger change first.
- **Improve the upstream projects.** Many fixes belong in prospero-win, ps5-homebrew-ui or PS5_Mesa. See
  [Dependencies](#dependencies).

## Report a game test

1. Check the [compatibility list](COMPATIBILITY.md). If the game is there, report only what is new.
2. Play the game from Playden or from its tile on the Home screen.
3. If the game did not work, start Playden once more. When it starts, Playden copies the game's newest log to
   `logs/title-<TITLE>.log`.
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

## Requirements

- macOS on Apple Silicon for the host app and the tests. The PS5 package also builds on Ubuntu Linux, including
  Ubuntu under WSL2 on Windows (see [Build on Ubuntu](#build-on-ubuntu)).
- On macOS: Xcode command line tools and the Homebrew packages
  `brew install cmake ninja pkgconf sdl2-compat llvm@18 python`.
  LLVM 18 builds the PS5 code. Apple clang builds the macOS host.
- On Ubuntu: the apt packages `cmake ninja-build pkgconf libsdl2-dev llvm-18 llvm-18-dev clang-18 lld-18 python3
  python3-dev python3-pip python3-venv unzip zstd binutils git curl perl xz-utils`, plus [Homebrew](https://brew.sh)
  and `brew install llvm@18`. Ubuntu's clang builds the host-side tools, Homebrew's LLVM builds the PS5 code.
- About 2 GB of free space for `deps/` and `build/`.
- A checkout path without symbolic links. Playden refuses symlinked paths when it reads its data, so two tests fail
  in a link such as macOS `/tmp` (use `/private/tmp` or a folder in your home directory).
- For console work: a jailbroken PS5, preferably on firmware 13.60, the version we test on (see [Running on a console](#running-on-a-console)).

## Get the source

```sh
git clone --recursive https://github.com/leolobato/playden-ps5.git
cd playden-ps5
scripts/fetch-deps.sh
```

`--recursive` checks out the submodules in `third_party/`. Most of them are forks that carry our patches on a
`main` branch (see [Dependencies](#dependencies)).

`scripts/fetch-deps.sh` downloads the prebuilt PS5 toolchain pieces into `deps/`: two PS5 payload SDK releases, the
ps5-opengl SDK and the LLVM compiler-rt builtins. It checks every download against the SHA-256 in
`dependencies.lock.json`. You can run it again at any time; it skips what is already correct.

## Build and test on macOS

```sh
scripts/build-homebrew.sh
cmake --build --preset homebrew-host
ctest --preset homebrew-host --output-on-failure
build/homebrew-host/playden-homebrew
```

The first run also builds curl and Mbed TLS from pinned source archives. The host app signs in to Steam like the
console app does and stores its data under `build/homebrew-host/playden-data`.

You do not need a console or a Steam account for the tests. To see the screens with synthetic data, use the
simulation mode:

```sh
build/homebrew-host/playden-homebrew --simulation --scenario signed-out
```

## Build the PS5 package

```sh
scripts/build-lapy-helper.sh
scripts/build-homebrew-ps5.sh
```

The first script builds the root helper that gives the title access to `/data`. The second builds the launcher
title `FAKE77031` into `build/homebrew-ps5/dist/FAKE77031`, with the two small payloads it sends to the payload
loader (the title remover and the self-update's relaunch payload). Neither script contacts a console.

The build checks the pinned revisions and hashes. If it stops with a mismatch, do not edit the hash: find out why
the input changed. The PS5 build is reproducible, so the same sources give the same `eboot.bin` in any checkout.

`FAKE77031` is allocated to Playden. Do not use it for another application.

## Build on Ubuntu

The PS5 package builds on Ubuntu Linux with the same scripts. This has been tested on Ubuntu under WSL2 on
Windows; a native Ubuntu install should behave the same. The host app and the tests have not been built on
Linux yet because the ps5-homebrew-ui library only supports macOS at this time.

Install the apt packages from [Requirements](#requirements), then Homebrew and its LLVM 18:

```sh
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
eval "$(/home/linuxbrew/.linuxbrew/bin/brew shellenv)"
brew install llvm@18
"$(brew --prefix llvm@18)/bin/clang" --version   # the first line must start with "Homebrew clang version 18.1.8"
```

Homebrew's LLVM is required because every object clang emits carries the compiler's vendor string in its
`.comment` section. The hashes in `dependencies.lock.json` for the network libraries, the title remover and the
root helper were recorded with Homebrew's LLVM, so Ubuntu's `clang-18` package builds the same code but fails
the hash check. `scripts/with-env.sh` prefers Homebrew's `llvm@18` whenever a `brew` command is on `PATH`, so
nothing else needs configuring.

Then build as on macOS:

```sh
scripts/fetch-deps.sh
scripts/build-lapy-helper.sh
scripts/build-homebrew-ps5.sh
```

If you built anything with Ubuntu's clang before installing Homebrew you will need to rebuild the pinned pieces,
since the build scripts only rebuild them when they are missing:

```sh
scripts/network-deps/prepare-ps5-deps.sh
scripts/build-title-remover.sh
scripts/build-lapy-helper.sh
scripts/build-homebrew-ps5.sh
```

## Running on a console

You need:

- A jailbroken PS5 (for example with the WebKit autoloader). We test on firmware 13.60; other versions are untested.
- These payloads loaded after each boot. The versions are the ones we test with:

  | Payload | Version | What it is for |
  | --- | --- | --- |
  | [kstuff](https://github.com/EchoStretch/kstuff-lite) | kstuff-lite 1.11 | Required: without it every title fails with CE-100008-9. |
  | [ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus) | v1.7-beta5fix1 (v1.7beta2 also works) | Registers folder titles under `/data/homebrew`. |
  | [elfldr](https://github.com/ps5-payload-dev/elfldr) | v0.26 | ELF loader on port 9021; runs the root helper. |
  | [zftpd](https://github.com/seregonwar/zftpd) | 1.6.0 | FTP on port 2120; the console tools use it. |
  | [ps5-web-file-manager](https://github.com/owendswang/ps5-web-file-manager) | v1.9 | Port 8888; useful for recovery. |
- The Mac and the PS5 on the same network. Pass the PS5's address to the console tools with `--host <PS5 IP>`.

If zftpd does not start after a reboot, delete the stale `/data/zftpd/instance.pid` and start it again.

### Install or update the launcher

The first time, copy `build/homebrew-ps5/dist/FAKE77031` to `/data/homebrew/FAKE77031` on the console.
ShadowMountPlus registers it in about 10 seconds. After that, update only the changed files:

```sh
python3 tools/console_title_deploy.py --host <PS5 IP> deploy build/homebrew-ps5/dist/FAKE77031 \
  --backup build/console-backups/<name>
```

The tool backs up each console file it replaces. `restore` puts a backup back. It never touches game downloads,
logs or user data.

### The compatibility kit

Games run with a compatibility kit: the Wine runtime, a prepared bottle template and the per-game title files. The
launcher accepts only the kit whose manifest is pinned in `dependencies.lock.json` and `src/runtime/kit_trust.cpp`.

Most people install the kit from a release download. To install a kit on the console:

```sh
python3 tools/console_kit_upload.py <kit folder> --host <PS5 IP> --replace
```

The upload verifies every file by read-back before it switches to the new kit.

### Build the kit

The kit builds on a **Linux x86-64 machine or VM**: Wine, Mesa and the PS5 libraries do not build on macOS. The
maintainer uses an Ubuntu VM and packages and uploads the kit from the Mac.

The build is prospero-win's tools, run in this order:

1. `tools/build_host_wine.sh`: Wine for the host (PE modules, winebuild) with the patch series.
2. `tools/build_wine_ps5.sh`: Wine's Unix side for the PS5, linked to RADV from
   [PS5_Vulkan](https://github.com/mpereiraesaa/PS5_Vulkan) / [PS5_Mesa](https://github.com/mpereiraesaa/PS5_Mesa).
3. `tools/build_tls_ps5.sh`: schannel through GnuTLS.
4. `tools/build_wowprospero.sh` and `tools/build_wow64native.sh`: 32-bit support.
5. `tools/build_mesa_zink.sh`: Mesa WGL/Zink for OpenGL games.
6. `tools/build_native.sh` (on the Mac): the per-game title shell, with the Lapy root helper.
7. `tools/package_playden_kit.py`: package and validate the kit.

[`tools/kit-h/`](tools/kit-h/README.md) has the scripts that built kit H and its revisions up to r6, the kit this release uses. They are a record, not a turnkey build:
they reuse inputs from earlier kits that are not in this repository (a template bottle prepared from Half-Life 2, an
earlier kit's runtime file list) and carry the maintainer's local paths. A build from a clean machine is not
documented end to end yet.

### Logs

Playden writes its logs to `/data/homebrew/FAKE77031/logs/` (`runtime.log`, `launch.log`, `cloud.log` and others).
Game bottles are private to their titles. When Playden starts, it copies the newest game session report to
`logs/title-<TITLE>.log`.

## Dependencies

| Location | What | How it is pinned |
| --- | --- | --- |
| `third_party/<fork>` | Patched projects: `prospero-win`, `ps5-homebrew-ui`, `ps5-opengl`, `prospero-foundation`, `PS5-Lapy-JB-Daemon` | Submodule commit on the fork's `main` branch |
| `third_party/ps5-native-app-boilerplate` | Native title tooling | Submodule commit (upstream) |
| `third_party/{stb,sqlite3,zstd,lzma-sdk,…}` | Small vendored sources | File hashes in `dependencies.lock.json` |
| `deps/` | Prebuilt SDKs and builtins | Download hashes in `dependencies.lock.json` |

To see our commits in a fork, run `git log --oneline upstream/main..main` in its submodule (after
`git remote add upstream <upstream URL>` and a fetch).

### Change a forked dependency

1. Work in the submodule on its `main` branch:

   ```sh
   cd third_party/prospero-win
   git switch main
   # edit, test, commit
   ```

2. Update the pin in Playden and anything that records the revision (`dependencies.lock.json`, and for
   ps5-homebrew-ui the revision in `scripts/build-homebrew.sh` and `cmake/HomebrewUI.cmake`). Commit the submodule
   change with it.
3. Push the fork branch before you push Playden, so the pin is fetchable. For prospero-win, the fork is
   [leolobato/prospero-win](https://github.com/leolobato/prospero-win).
4. Send the change upstream when it is useful there. When upstream merges it, rebase the fork's `main` onto
   upstream and drop the commit from our branch.

Commits on a fork's `main` are temporary patches that wait for upstream. Larger work can sit on its own
fork branch based on the fork's `main` (for example `feat/74-media-backend`) until it is ready to merge there.

To send a change upstream, branch from `upstream/main` and replay only that change. Leave out fork-only commits
and anything that names Playden: our console paths (`/data/playden-*`), profile `[environment]` support, kit
pins and links to our issues or notes. Follow upstream's `CONTRIBUTING.md` and pull request template, and run its
CI checks (`make all`, `make sanitize`) on Linux.

To take a new upstream version, merge or rebase upstream into the fork's `main`, test, and move the pin.

### Patch a downloaded dependency

The downloads in `deps/` are used unmodified. If one needs a patch, turn it into a fork: add its source as a
submodule on a `main` branch, add a build step that writes the result into `deps/`, and pin the result
in `dependencies.lock.json`. You can attach the built result to the fork as a release, so others download it
instead of building it.

### Change Wine

Wine is not forked. prospero-win pins an upstream Wine commit and applies the patch series in
`wine/patches/` on top of it. Our Wine changes are patch files in that series, on our prospero-win fork.

To write or change a Wine patch:

1. Make a scratch Wine branch: check out the pinned Wine commit and apply the series as commits.
2. Make your change there with normal git tools.
3. Export it with `git format-patch` into prospero-win's `wine/patches/`, with the next free number.
4. Check the series: `tools/build_wine_ps5.sh --check-patches` in prospero-win.
5. Commit only the patch file on the fork's `main`.

When prospero-win moves to a newer Wine, merge it into our branch and refresh only our patch files if they no
longer apply. Rebuilding the runtime and the kit follows prospero-win's `docs/WINE_PS5_BUILD.md` (Linux host).

## Code and commits

- Match the style of the code around your change. Keep product behaviour in this repository and reusable UI fixes
  in ps5-homebrew-ui.
- Add or update tests with each change. Run the host tests before you send a change.
- A build or upload is not console acceptance. Say what you ran on a console, and keep the logs.
- Commit subjects under 60 characters. In the body, use `-` bullets that describe the visible behaviour.

## Publish a release

Playden updates itself from the latest GitHub release (`releases/latest`, which leaves out drafts and
pre-releases), so every release needs its update manifest:

1. Raise the version in `CMakeLists.txt` (`project(PlaydenLauncher VERSION …)`) and build the PS5 package. The
   version becomes `param.json`'s `contentVersion` and the version Playden compares.
2. Make the two zips as before: `playden-ps5-<version>.zip` (with the `FAKE77031` folder inside
   `playden-ps5-<version>/`) and `playden-kit-h-<revision>.zip` (with the kit folder inside
   `playden-kit-h-<revision>/`).
3. Write the manifest:

   ```sh
   python3 tools/write_update_manifest.py --app-zip playden-ps5-<version>.zip \
       --kit-zip playden-kit-h-<revision>.zip --kit-revision <revision> --min-from <oldest version> \
       --notes "A short summary of the release." --out playden-update.json
   ```

   `--notes` is the summary the update dialog shows: plain text (no Markdown) of at most 400 characters, such as the
   first line of the release notes. The dialog links to the release page, as a QR code, for the full notes. The tool
   refuses a kit that `dependencies.lock.json` does not pin. `--min-from` is the oldest version that can update
   to this one; older versions are told to install by hand.
4. Attach `playden-update.json` and both zips to the release, then publish it. GitHub records each file's SHA-256,
   and Playden checks the downloads against it. If the kit did not change, the kit zip may be left out: consoles
   that already have that kit download only the app.

To try an update on a console without publishing, put `release.json` (from `--release-json`, with
`--allow-other-kit` for a test kit), `playden-update.json` and the zips in `/data/homebrew/FAKE77031/dev/update-local/`.
Playden then reads the release from there instead of GitHub. Delete the folder afterwards.

## Maintainer notes

The maintainer's status notes, plans and console evidence are kept in a separate repository. Comments in the code
that name a `results/<run>` folder refer to it.

## License

Playden's source files are marked GPL-3.0-or-later (SPDX headers). The submodules and vendored sources keep their
own licenses.
