<p align="center"><img src="docs/images/icon.png" alt="Playden icon" width="128"></p>

# Playden for PS5

**Play your Steam library's Windows games on a jailbroken PS5.**

[![Discord](https://img.shields.io/discord/1562443095629373590?label=Discord&logo=discord&logoColor=white)](https://discord.gg/wd8UjExpPA)
[![Reddit](https://img.shields.io/badge/Reddit-r%2FPlayden__PS5-FF4500?logo=reddit&logoColor=white)](https://www.reddit.com/r/Playden_PS5/)

Playden is a launcher that runs on the PS5, adapted from [Playden for macOS](https://github.com/leolobato/playden).
You sign in to Steam with a QR code or your account name and password, browse your library, download games, and play them. The Windows games run through
[prospero-win](https://github.com/mpereiraesaa/prospero-win), a port of Wine to the PS5. Each game gets its own
private Windows environment and its own tile on the PS5 Home screen.

> [!WARNING]
> **Alpha software.** Playden is an early, experimental project. It signs in to **your real Steam account**, downloads
> games with your licenses, and reads and writes your **Steam Cloud saves**. Read [Before you use it](#before-you-use-it).
> If you are not comfortable with these risks, do not use it.

![Playden's library on the PS5: Steam games as cover art, with Installed, All, Favorites and Hidden on the side](docs/images/library.jpg)

## Features

- **Steam sign-in with a QR code** (approve it in the Steam mobile app), **or with your account name and password.**
  Steam Guard email codes, Steam Mobile app codes and approval in the app are supported. The sign-in screen opens on
  the method you used last.
- **Your whole Steam library.** Search, sort, favorites, collections, ratings and notes, with cover art and store
  details. The library works offline from a cache.
- **Downloads from Steam.** Queue games and watch progress. The Queue screen also lists the games being prepared, in
  the order they start; Prepare next moves a waiting game to the front.
- **Storage at a glance.** Settings › Storage shows how much space is used and lists installed games by size or
  last played. Select several games and uninstall them together; saves are kept unless you choose otherwise.
- **Each game is its own PS5 app.** It gets a tile on the Home screen, so you can start it without Playden.
- **A private Windows environment per game** ("bottle"), so games do not share settings or files.
- **Steam Cloud saves.** Playden checks your Cloud saves before a game starts and syncs them after it ends. If the
  console and the Cloud disagree, you choose which one to keep. You can turn the automatic sync off in Settings and
  sync by hand from the Saves tab.
- **Per-game settings.** Graphics, frame rate, resolution, controller mapping, and how the game starts: pick any
  program from the game folder, edit its arguments and library overrides, or try a game Playden flagged anyway. A
  game's settings can be exported as a recipe ([format](docs/community-recipes.md)).
- **Graphics.** Direct3D 8–11 through [DXVK](https://github.com/doitsujin/dxvk), Direct3D 12 through
  [vkd3d-proton](https://github.com/HansKristian-Work/vkd3d-proton) (kit 0.11 and later), OpenGL through Mesa's Zink, and
  Vulkan, all on the PS5 GPU's Vulkan driver.
- **Gamepad support.** XInput, DirectInput, Raw Input, Windows.Gaming.Input and the Windows multimedia joystick API,
  with rumble. Games see the DualSense as an Xbox controller.
- **Mouse and keyboard.** Plug a USB mouse and keyboard into the PS5, or use a controller preset: the left stick as a
  mouse, or the D-pad as the arrow keys.
- **Import Windows games.** Add game folders you already have, from a USB drive, an M.2 SSD or over FTP. No Steam
  account is needed. See [Import Windows games](#import-windows-games).
- **Install to internal or external disks.** Console storage, or an exFAT USB drive formatted on a Mac or PC.

See the [compatibility list](COMPATIBILITY.md) for the games we have tested.

### Planned

- Steam game updates
- Other stores (GOG, Epic)
- Support for game launches (Rockstar, EA, Ubisoft)
- Community recipes in the preset chooser

## Before you use it

Playden is alpha software. It is not affiliated with, endorsed by, or supported by Valve. Know these points before
you sign in:

- **It is an unofficial Steam client.** Playden talks to Steam's servers directly, the way the Steam client does. Valve
  does not support third-party clients. We know of no account problems from this, but the risk is yours to accept.
- **Your Steam session stays on the console.** Playden stores a Steam sign-in token on the PS5, **without encryption**.
  On a jailbroken console, anyone with file access (for example through FTP) can read it. To keep it off the console, turn off "Stay
  signed in", then sign in again each time Playden starts.
- **It writes to your Steam Cloud saves.** Playden uploads saves after you play. A bug can overwrite good saves with
  older or broken ones. Back up saves you care about. When Playden shows a conflict, read it before you choose.
- **It needs a jailbroken console with root-level helpers.** A running game can get elevated rights on the console.
  Run only games you trust.
- **Things will break.** Expect games that fail to start, crashes, and the occasional console restart. Keep your
  console's own data backed up.

## Requirements

- **A jailbroken PS5** (for example with the WebKit autoloader). We test on firmware 13.60. Playden does not block
  other firmware versions, but they are untested. Please give your firmware in a test report.
- **These payloads loaded after each boot:**
  - [kstuff](https://github.com/EchoStretch/kstuff-lite) — required. Without it, every game fails with error
    CE-100008-9.
  - [ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus) **1.7-beta5fix1** — puts each game's tile on the
    Home screen. 1.7beta2 also works.
  - [elfldr](https://github.com/ps5-payload-dev/elfldr), the ELF loader on port 9021 — runs the root helper that
    gives a game access to its files. Most jailbreak loaders start it for you.
  - [nanoDNS](https://github.com/drakmor/nanoDNS), if your console's DNS is set to 127.0.0.1 (as many jailbreak
    guides suggest) — without a local DNS server, Playden can't reach Steam.
  - For installing and updating Playden: an FTP server such as [zftpd](https://github.com/seregonwar/zftpd)
    (port 2120), and optionally [ps5-web-file-manager](https://github.com/owendswang/ps5-web-file-manager)
    (port 8888).
- **Windows games:** a Steam account with games, or game folders you already have (see [Import Windows games](#import-windows-games)).
- **A computer on the same network as the PS5**, to copy Playden to it. Python 3 is needed only for the optional
  install tools.
- **To build it yourself (optional):** a Mac with Apple Silicon. Linux should also work, but we have not tested it
  yet. See [Build from source](#build-from-source).

Tested with kstuff-lite 1.11, ShadowMountPlus v1.7-beta5fix1 and v1.7beta2, elfldr v0.26, zftpd 1.6.0 and ps5-web-file-manager v1.9.
Other versions can work, but these are the ones we know.

## How it works

- **Playden does not run games itself.** It downloads a game, prepares it, and hands it to a separate PS5 app made
  for that game. That app starts Wine with the game's settings. Because each game is its own app, it has its own tile
  on the PS5 Home screen, and it is isolated: its files, settings and crashes stay inside it.
- **The runtime kit is separate from Playden.** The kit holds Wine for PS5, the graphics drivers, a clean Windows
  environment and the per-game app shell. Playden checks that the installed kit is complete before it uses it.
- **Microsoft's MFC library.** Wine doesn't include it. When a game needs it, Playden downloads Microsoft's Visual C++
  redistributable (about 25 MB, once per console) and installs only its MFC files into that game's Windows folder.

## Install

Download `playden-ps5-<version>.zip` from the latest [release](../../releases): the Playden app (`FAKE77031`) and
the console tools used below. Unzip it into a folder of its own, `playden-ps5-<version>` (most unzip tools make it;
with `unzip` on the command line, use `unzip playden-ps5-<version>.zip -d playden-ps5-<version>`). Installing takes one copy over the network, the app folder. Playden
downloads its runtime kit on its first start, and after that it updates itself (see [Update Playden](#update-playden)).

Like other PS5 homebrew, Playden is a folder in `/data/homebrew/`, and
ShadowMountPlus adds its tile to the Home screen.

### Install Playden

1. Copy the `FAKE77031` folder from `playden-ps5-<version>` into `/data/homebrew/` on the console. Use any FTP client
   (FileZilla, Cyberduck, etc.) or the web file manager in a browser. Create `/data/homebrew` first if it isn't there.
   You do not need to `chmod` anything: Playden checks the folders it writes to and fixes their permissions before
   it writes. If it can't fix one, it names that folder. If your FTP client leaves out the empty `logs` and
   `playden-data` folders, Playden makes them again when it starts.

   Or, from a computer with Python 3, run this in the `playden-ps5-<version>` folder. It creates the folders, copies
   only what changed, and keeps a backup of the files it replaces:

   ```sh
   python3 tools/console_title_deploy.py --host <PS5 IP> deploy --install FAKE77031 --backup backups/<name>
   ```

   From 0.6.0, Playden keeps its data outside the app folder and moves it there once, at startup:

   - Your sign-in, settings and other user data: `/data/playden/user/`.
   - Logs: `/data/playden/logs/`. The first startup lines, written before Playden gets its root access,
     stay in `logs/` in the app folder.
   - Downloaded games: `/data/playden-games/`. Each game's Windows folder, with its saves, stays under
     `/data/playden/games/` (see [Saves and game configs over FTP](#saves-and-game-configs-over-ftp)).

   The app folder keeps empty `playden-data` and `logs` folders, and a `MOVED.txt` in each old place says where
   the data went.
2. Wait about 10 seconds. ShadowMountPlus adds the Playden tile to the Home screen.
3. Start Playden. It offers to download the runtime kit (next section); choose Download.
4. Sign in to Steam while the kit downloads.

### Update Playden

Since 0.3.0, Playden updates itself from this repository's GitHub releases:

- **Check:** at startup, and with Settings › Updates › Playden updates › Check now. When a newer release is out,
  Playden offers it once, with its notes, its download size, and a QR code for its release page on GitHub.
- **Download:** Download and install fetches the new app and, when the release has a new one, the runtime kit. You
  can follow the download on the Queue screen and keep using Playden meanwhile.
- **Restart:** when the update is ready, Restart Playden closes Playden, puts the new version and kit in place, and
  opens Playden again. If it doesn't open by itself (the payload loader on port 9021 isn't running), open it from
  the Home screen. On that first start Playden shows what's new in the version; Settings › Updates › Release notes
  shows it again later.

Settings › Updates › **Beta updates** opts into test versions before their stable release. It is off by default;
betas can have new bugs and use the same app and data. Turning it off keeps an installed beta until a newer stable
version is available. Updates never downgrade Playden.

The previous app and kit are kept until the next update, in `/data/playden/self-update/backup/` and
`/data/playden/kits/.previous-*`. Settings › Updates › **Go back to Playden <version>** puts them back: Playden
closes, swaps the previous version and its kit in, and opens again. If the swap does not take and the previous
version opens again, it says so and puts its kit back. If the new version itself fails to start, nothing can report it: reinstall Playden over FTP as
described above (your games, sign-in and settings are kept), and, if the new kit is at fault, rename the previous
kit folder (`.previous-<id>-<time>`) back to `<id>`, moving the installed folder of that name aside.

After the version that added import, going back to an older version keeps your old favorites, but loses the favorites
and collections you changed since. The console's organization data now lives in one file that older versions don't read.

You can also update by hand: copy the new `FAKE77031` folder over the old one, or run the install command without
`--install`. The command copies only the changed files, keeps a backup of what it replaced, and puts those three
folders' modes right if they changed.

### The runtime kit

The runtime kit is what runs Windows games: Wine, Mesa and DXVK. Playden
downloads the kit its version was made for from this repository's releases, on its first start and whenever the
installed kit isn't that one. It installs in the background, shows on the Queue screen, and games can be prepared
as soon as it is in place. If you choose Later, or the download fails, Settings › About › Runtime kit starts
it again. Games pick up a new kit when you press Play.

You can also install the kit by hand, with Python 3: download `playden-kit-<version>.zip` from the release, unzip it
next to `playden-ps5-<version>`, and run this from the `playden-ps5-<version>` folder. A plain FTP copy does not
work for the kit, because Playden checks each kit file's permissions against the kit's manifest.

```sh
python3 tools/console_kit_upload.py ../playden-kit-<version> --host <PS5 IP> --replace
```

The kit is built on a **Linux x86-64 machine or VM** from prospero-win, Wine, Mesa and DXVK. To build it yourself,
see [Build the kit](docs/building.md#build-the-kit). The scripts are a record of how we built it, not a turnkey build
yet, so most people should use the release download.

### Saves and game configs over FTP

Each game's Windows user folder holds its saves and most of its `.ini` configs:

```text
/data/playden/games/steam-<Steam app ID>/library/prefixes/game/drive_c/users/prospero/
```

For Half-Life 2 that is `steam-220`. Look in `AppData/Roaming`, `AppData/Local`, `Documents` and `Saved Games`.

This needs the next runtime kit (it has the `bottle-profile-shared` capability); kit 0.10 keeps the whole folder
private. With it, you can read and write everything in `prospero`. The folders above it can be entered but not
listed, so type the full path in your FTP client instead of browsing to it. New saves a game writes are private
at first. Playden opens them to FTP when it starts, when a game it started ends, and before it starts a game.

## Import Windows games

You can add Windows games you already have, without a Steam account.

**Put the game folders in place:**

- On an exFAT USB drive or M.2 SSD, in `playden/import/<game folder>` at the root of the drive.
- Or over FTP, in `/data/playden-import/<game folder>` on the console.

The drive must be exFAT. The PS5 can't read NTFS, and FAT32 can't hold files over 4 GB.

**Add a game:**

1. Open **Import games**: on the Startup screen, on Home when your library is empty, or in Settings › Library.
2. Choose **Look for games**. Playden lists the folders it finds, with their size.
3. Pick a folder. When it holds one program besides installers and tools, Playden starts the game with it. Otherwise
   pick the program that starts the game; installers and tools are labelled as such. You can change it later in
   Game settings.
4. Choose where the game files live:
   - **Play from the USB drive** (or the M.2 SSD, or the drop folder): nothing is copied. The drive must stay plugged
     in.
   - **Import into Playden:** then choose the storage it goes to. Playden copies the game there; the console copies
     about 11 MB/s, so a large game takes a while. A drive without room for the game, and the drive the game is on,
     can't be chosen.
5. For a game in the drop folder, **Console storage** moves the folder into Playden at once. Another drive gets a
   copy, and once the copy is checked the game's files leave the drop folder, so it isn't stored twice. Until then
   it plays from the drop folder.

You can import a game that plays from its folder later, with **Import into Playden** on its page. The PS5 adds the
game's title to Home when Playden quits. Importing a game later keeps its saves.

**Limits:**

- To play from where it is, the folder's path must be plain ASCII. Otherwise, copy the game.
- Playden refuses a game folder that holds links.
- Removing an imported game from the library leaves its files, and keeps its saves.
- Cloud saves are only for Steam games.

**Steam games copied from a PC** usually run without Steam. If a game needs Steam, Playden uses the game's
`steam_appid.txt`, or the **Steam app ID** in Game settings. Most games run without either.

Games that need their own launcher, or DRM beyond SteamStub (Denuvo, or the Ubisoft, EA or Rockstar launchers), won't
run.

## Build from source

### What you need

- macOS on Apple Silicon. Linux should also work, but we have not tested it yet.
- Xcode command line tools.
- Homebrew packages:

  ```sh
  brew install cmake ninja pkgconf sdl2-compat llvm@18 python
  ```

- About 2 GB of free disk space.
- A checkout path without symbolic links (for example, not under `/tmp`).

### Get the source

```sh
git clone --recursive https://github.com/leolobato/playden-ps5.git
cd playden-ps5
scripts/fetch-deps.sh
```

`fetch-deps.sh` downloads the prebuilt PS5 SDKs into `deps/` and checks each download against the SHA-256 in
`dependencies.lock.json`.

### Build the PS5 package

```sh
scripts/build-lapy-helper.sh
scripts/build-homebrew-ps5.sh
```

The result is the Playden app, `build/homebrew-ps5/dist/FAKE77031`. Install it as in [Install Playden](#install-playden),
with that folder in place of `FAKE77031`:

```sh
python3 tools/console_title_deploy.py --host <PS5 IP> deploy --install build/homebrew-ps5/dist/FAKE77031 \
  --backup build/console-backups/<name>
```

### Try it on your Mac first (optional)

Playden also builds as a desktop app, which is useful to look at the interface without a console:

```sh
scripts/build-homebrew.sh
cmake --build --preset homebrew-host
build/homebrew-host/playden-homebrew --simulation --scenario signed-out
```

## Known issues

- New games can stay at *Waiting for the PS5* after you quit Playden when ShadowMountPlus stops registering titles.
  Reload ShadowMountPlus.
  Until the game is registered, its Cloud saves are not synced before its first launch.
- If every game hangs on its loading spinner, the ELF loader has stopped. Reload it.
- If Playden can't start, its first screen says which step failed. The startup error under it is worth quoting
  in a report:
  - **Playden's app folder is incomplete:** files such as `lapy.elf` or `playden-package.identity` are missing
    from the app folder or can't be read. Copy the whole app folder from the release zip again, or use
    `tools/console_title_deploy.py`.
  - **The ELF loader isn't running:** nothing answers on port 9021, so Playden can't run its root helper. Load
    the ELF loader (most autoloaders start it), then open Playden again.
  - **Root access is missing:** the root helper could not give Playden access to `/data`, or the ELF loader
    stopped while it received the helper. The screen says which. Load kstuff and the autoloader, then open Playden
    again. If it stays, report it with your firmware version.
  - **Playden can't start:** the console's network did not start, or Playden has root access but can't use
    `/data/playden`. The screen says which, and what to try.
- Some controller games show a mouse pointer in a corner of the screen.
- Playden can crash if you open a game page in the first seconds after start-up.
- 32-bit games can run through an x86 translator, which is slower than native. For 32-bit OpenGL games this
  includes Mesa itself, so heavy ones can be slow.
- After you update Playden, it can ask you to sign in to Steam again.
- Steam can turn down a saved sign-in that worked before. Playden tries again the next time it starts; if it keeps
  happening, use Settings › Account › **Sign in again**: it keeps your library, favorites and settings.
- Godot games (Slay the Spire 2) need `--rendering-driver opengl3` in Game settings › Extra arguments. Playden picks
  Godot's Vulkan renderer, which did not work on this kit.
- The 120 Hz choice of the "Refresh rate" game setting needs a newer runtime kit; it is not available with this kit.
- When you close a game from the PS5 menu, it can stay suspended. Pressing Play again resumes it instead of starting
  it again.
- Downloads can be slow (about 1 MB/s) on some networks, far from a fast Steam server. Turn on Settings › Storage ›
  **Downloads for slow networks**: Playden then downloads 8 pieces at the same time instead of 4. On a fast connection
  it can be slower, so turn it off again if downloads get slower.

See the [compatibility list](COMPATIBILITY.md) for what we know about each game.

## Testing games and reporting problems

Every test helps, including games that work. To report one:

1. If the game did not work, start Playden once more, so it copies the game's log.
2. Collect the logs from your computer:

   ```sh
   python3 tools/collect_logs.py --host <PS5 IP>
   ```

   It lists the games that have a log, the most recently run first, and asks which one to collect. The script
   reads only Playden's logs (`/data/playden/logs`), never your Steam sign-in. It hides your Steam account name and
   SteamIDs, and writes a zip with a summary of your firmware and kit versions.
3. [Open a game test report](../../issues/new?template=test-report.yml) and attach the zip. See a
   [sample report](docs/sample-test-report.md) for what to fill in.

For a problem that is not about one game, [open a bug report](../../issues/new?template=bug-report.yml) with the
same zip. A problem installing or updating Playden goes in an
[install or update problem](../../issues/new?template=install-problem.yml), and ideas in a
[feature request](../../issues/new?template=feature-request.yml). Questions and help with setup go to
[Discord](https://discord.gg/wd8UjExpPA) or [r/Playden_PS5](https://www.reddit.com/r/Playden_PS5/), not to issues.

## Built on

Playden would not exist without these projects.

### Projects we carry patches for

I keep a fork of each, with Playden's changes on the fork's `main` branch, and send fixes upstream.

| Project | What it does for Playden |
| --- | --- |
| [prospero-win](https://github.com/mpereiraesaa/prospero-win) | Wine for the PS5. Runs the Windows games: memory management, a 32-bit x86 translator, display, input and audio for the console. Also builds the per-game app shell. |
| [ps5-homebrew-ui](https://github.com/leolobato/ps5-homebrew-ui) | The UI toolkit Playden's screens are drawn with: layout, fonts, controller focus and navigation, on OpenGL. |
| [ps5-opengl](https://github.com/leolobato/ps5-opengl) | OpenGL for PS5 homebrew. Draws Playden's own interface. |
| [ps5-native-app-boilerplate](https://github.com/mpereiraesaa/ps5-native-app-boilerplate) (mpereiraesaa) | Tools to build native PS5 apps and modules, used for the per-game app shell in the kit. Our fork is named `prospero-foundation`. |
| [PS5-Lapy-JB-Daemon](https://github.com/mpereiraesaa/PS5-Lapy-JB-Daemon) | The root helper. Gives a game's app just enough rights to reach its own files in `/data`. |

### Used as they are

| Project | What it does for Playden |
| --- | --- |
| [ps5-native-app-boilerplate](https://github.com/drakmor/ps5-native-app-boilerplate) (drakmor) | Tools to build Playden as a native PS5 app, and its `libc.prx`. |
| [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk) | The SDK for PS5 homebrew. |
| [gbe_fork](https://github.com/Detanup01/gbe_fork) | Steamworks emulator (LGPL-3.0), shipped with Playden in `runtime/steamworks/`, so games run without the Steam client. |

### Inside the runtime kit

- [Wine](https://www.winehq.org/) — runs Windows programs.
- [DXVK](https://github.com/doitsujin/dxvk) — turns Direct3D 8–11 into Vulkan.
- [vkd3d-proton](https://github.com/HansKristian-Work/vkd3d-proton) — turns Direct3D 12 into Vulkan (kit 0.11 and later).
- [PS5_Mesa](https://github.com/mpereiraesaa/PS5_Mesa) and [PS5_Vulkan](https://github.com/mpereiraesaa/PS5_Vulkan) —
  [Mesa](https://mesa3d.org/) for the PS5: RADV, the AMD Vulkan driver, for the PS5 GPU, and Zink with WGL, which
  runs Windows OpenGL games on top of Vulkan.

### Small libraries

[curl](https://curl.se/), [Mbed TLS](https://www.trustedfirmware.org/projects/mbed-tls/),
[SQLite](https://sqlite.org/), [zstd](https://github.com/facebook/zstd), [LZMA SDK](https://www.7-zip.org/sdk.html),
[stb](https://github.com/nothings/stb), [utf8proc](https://github.com/JuliaStrings/utf8proc) and
[QR Code generator](https://github.com/nayuki/QR-Code-generator).

## Contributing

Contributions are welcome. [CONTRIBUTING.md](CONTRIBUTING.md) explains how to report a game test and the rules for
pull requests (one change per pull request). [docs/building.md](docs/building.md) covers the development setup, the
tests, the console workflow, and how to change a forked dependency or a Wine patch.

## License

Playden is licensed under the GNU General Public License v3.0 or later. See [LICENSE](LICENSE). The submodules,
vendored libraries and the runtime kit keep their own licenses: see
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Game recipes in `runtime/recipes/` are CC0 1.0: see
[runtime/recipes/LICENSE](runtime/recipes/LICENSE).

Steam is a trademark of Valve Corporation. PlayStation and PS5 are trademarks of Sony Interactive Entertainment.
This project is not affiliated with either company.
