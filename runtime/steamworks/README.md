# gbe_fork Steam API (LGPL-3.0)

`steam_api.dll` and `steam_api64.dll` are [gbe_fork](https://github.com/Detanup01/gbe_fork), a fork of the Goldberg
Steam Emulator by Mr_Goldberg, licensed under the GNU Lesser General Public License v3.0. They are shipped unmodified.
Playden does not link against them: when a game is prepared, they replace the game's own `steam_api(64).dll` in its
folder (the original is kept as `<dll>.orig`) and the game loads them. Source code: https://github.com/Detanup01/gbe_fork.

They are the `release/regular` builds from gbe_fork release
[`release-2026_09_27`](https://github.com/Detanup01/gbe_fork/releases/tag/release-2026_09_27) (`emu-win-release-vs22.7z`),
which answer interfaces up to `SteamClient023`; the earlier build, shared with the macOS Playden app and GameNative,
stopped at `SteamClient021`, and recent games quit with "Missing interface".

| File | SHA-256 |
| --- | --- |
| `steam_api.dll` | `cae4372bceda997d6c73cebc1c14eb27d39b74a7999d8f28384bdfd8d746a79c` |
| `steam_api64.dll` | `70d9826a402d3a78b0c07e9f7ea7c6832468a1113013f31ea570fccaa5445ea9` |

Playden checks both hashes (`src/runtime/steamworks.h`) before every use.

## steam.dll (Playden's own)

`steam.dll` is not gbe_fork's: it is a minimal Steam2-era client library built from `tools/steam-legacy/steam.c` (no imports,
reproducible with `tools/steam-legacy/build.sh`). Games from before Steamworks (GUN) load `steam.dll` from beside their program
and quit with "Failed to find Steam." without it; every call here says Steam runs and the game is owned. When a game has no
`steam_api` DLL, preparing it puts this `steam.dll` beside each 32-bit program that has none of its own; switching Steam features
back to the game's own removes it.

| File | SHA-256 |
| --- | --- |
| `steam.dll` | `f813075716892ab19ee9b0c06000c328bd5f15e713692384a30ddbcfc9065e57` |

## steam_api64_online.dll and steam_api_online.dll (Playden's own)

The online bridge, built from `tools/steam-online/online.c` (no C runtime; imports only kernel32 and winhttp;
reproducible with `tools/steam-online/build.sh`, 64-bit and 32-bit). For a game whose Game settings › Steam features is
**Online**, it becomes the game's `steam_api64.dll` / `steam_api.dll` and gbe_fork's moves beside it as
`steam_api64_gbe.dll` / `steam_api_gbe.dll`. It forwards every call to gbe_fork and loads it by path, so gbe_fork stays
unmodified. It signs in to Steam with the account's refresh token from `steam_settings/playden_online.txt` (mode 0600,
removed when the game leaves Online) and answers tickets, lobbies and friends through Steam. Its log is
`steam_settings/playden_online.log`, without tokens or ticket bytes. See `docs/online-play.md`.

| File | SHA-256 |
| --- | --- |
| `steam_api64_online.dll` | `5638d7b708d2e4627a39a3890f45ac8a0220a097e2ec341bc3cf267e10425603` |
| `steam_api_online.dll` | `44382606a5e5f492b296a83826938e0ef00fc91b7fa586da98d5dd551323639c` |
