# Building and running Playden

How to build Playden, run its tests, put it on a console, and change one of its dependencies. For the rules on
pull requests, see [CONTRIBUTING.md](../CONTRIBUTING.md).

- [Requirements](#requirements)
- [Get the source](#get-the-source)
- [Build and test on macOS](#build-and-test-on-macos)
- [Build the PS5 package](#build-the-ps5-package)
- [Build with Docker](#build-with-docker)
- [Run on a console](#run-on-a-console)
- [Build the kit](#build-the-kit)
- [Dependencies](#dependencies)

## Requirements

- macOS on Apple Silicon (the only host the build supports today).
- Xcode command line tools.
- Homebrew packages: `brew install cmake ninja pkgconf sdl2-compat llvm@18 python`.
  LLVM 18 builds the PS5 code. Apple clang builds the macOS host.
- About 2 GB of free space for `deps/` and `build/`.
- A checkout path without symbolic links. Playden refuses symlinked paths when it reads its data, so two tests fail
  in a link such as macOS `/tmp` (use `/private/tmp` or a folder in your home directory).
- For console work: a jailbroken PS5, preferably on firmware 13.60, the version we test on (see
  [Run on a console](#run-on-a-console)).

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

Each test is its own executable. To run one, pass its name: `ctest --preset homebrew-host -R '^runtime_launch_service$'`.
A new `tests/<name>_test.cpp` is registered with `add_test` in the matching `cmake/*.cmake`.

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

## Build with Docker

If you do not have a Mac, a Linux container builds the PS5 package. It needs Docker with Compose and nothing else
(no Homebrew, LLVM or SDK on the machine), and works on Windows, Linux and macOS.

```sh
scripts/build-in-docker.sh          # Windows: scriptsuild-in-docker.ps1
```

The script builds the image from the [Dockerfile](../Dockerfile) (Ubuntu 24.04 with LLVM 18, CMake and Ninja), then
runs `scripts/fetch-deps.sh`, `scripts/build-lapy-helper.sh` and `scripts/build-homebrew-ps5.sh` in it, the same
steps as above. The package is copied to `out/FAKE77031`; put it on the console as in
[Run on a console](#run-on-a-console). The first run downloads the SDKs and builds curl and Mbed TLS, so it takes
a while.

- Run `git submodule update --init --recursive` on the host first: the container uses your checkout.
- Downloads and build products live in Docker volumes (`build`, `deps`, `network-deps`, `lapy-build`), not in your
  checkout, so they do not clash with a macOS build in the same folder. `docker compose down -v` deletes them for
  a clean build.
- The pinned revisions and hashes are checked as on macOS. A mismatch means the input changed: do not edit the hash.
- **Known issue: the build stops at the archive check.** The PS5 archives built in the container (libcurl.a,
  Mbed TLS) do not yet match the hashes pinned in `dependencies.lock.json`, even with the same LLVM version (18.1.8),
  so CMake reports `Pinned ps5 archive hash mismatch`. Do not edit the hashes. Until this is resolved, the
  container build does not produce a package you can trust.
- **This does not run the tests.** The host build and `ctest --preset homebrew-host` support macOS only
  (`cmake/HomebrewUI.cmake`), so tests still need a Mac. Neither build is console testing.

## Run on a console

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

### Install the runtime kit

Games run with a runtime kit: the Wine runtime, a prepared bottle template and the per-game title files. The
launcher accepts only the kit whose manifest is pinned in `dependencies.lock.json` and `src/runtime/kit_trust.h`.

Most people install the kit from a release download. To install a kit on the console:

```sh
python3 tools/console_kit_upload.py <kit folder> --host <PS5 IP> --replace
```

The upload verifies every file by read-back before it switches to the new kit.

### Logs

Playden writes its logs to `/data/playden/logs/` (`runtime.log`, `launch.log`, `cloud.log` and others). The first
startup lines (`native-startup.log`), written before the root grant, stay in the app's own `logs/` folder. User data
is in `/data/playden/user/` and games are in `/data/playden-games/`.

Game bottles are private to their titles. When Playden starts, it copies the newest game session report to
`logs/title-<TITLE>.log`. `python3 tools/collect_logs.py --host <PS5 IP>` collects them into a zip.

## Build the kit

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

[`tools/kit-h/`](../tools/kit-h/README.md) has the scripts that built kit H and its revisions. They are a record, not
a turnkey build: they reuse inputs from earlier kits that are not in this repository (a template bottle prepared
from Half-Life 2, an earlier kit's runtime file list) and carry the maintainer's local paths. A build from a clean machine is not
documented end to end yet.

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

2. Open a pull request on the fork (for prospero-win, [leolobato/prospero-win](https://github.com/leolobato/prospero-win)).
   Playden can only pin a commit that is on the fork's `main`.
3. Once it is merged, move the pin in Playden together with everything that records the revision:
   `dependencies.lock.json`, and for ps5-homebrew-ui the revision in `scripts/build-homebrew.sh` and
   `cmake/HomebrewUI.cmake`.
4. Send the change upstream when it is useful there. When upstream merges it, the fork's `main` is rebased onto
   upstream and the commit drops out.

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

A Wine patch reaches players only in a new kit, and kits are built and released with Playden releases (see
[CONTRIBUTING.md › Files that stay out of pull requests](../CONTRIBUTING.md#files-that-stay-out-of-pull-requests)).

When prospero-win moves to a newer Wine, merge it into our branch and refresh only our patch files if they no
longer apply. Rebuilding the runtime and the kit follows prospero-win's `docs/WINE_PS5_BUILD.md` (Linux host).
