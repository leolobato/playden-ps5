# gbe_fork Steam API (LGPL-3.0)

`steam_api.dll` and `steam_api64.dll` are [gbe_fork](https://github.com/Detanup01/gbe_fork), a fork of the Goldberg
Steam Emulator by Mr_Goldberg, licensed under the GNU Lesser General Public License v3.0.
Playden does not link against them: when a game is prepared, they replace the game's own `steam_api(64).dll` in its
folder (the original is kept as `<dll>.orig`) and the game loads them. Source code: https://github.com/Detanup01/gbe_fork.

They are the `release/regular` builds of commit
[`ca45922c`](https://github.com/leolobato/gbe_fork/commit/ca45922c84ded9f404a61a6ae2e8a0c9593b8fa1): gbe_fork's `dev`
(`bd4d990e`, past release `release-2026_09_27`) with two changes:

- [`509fcb90`](https://github.com/leolobato/gbe_fork/commit/509fcb90410077865a39ab5f8d9fda53466ce14f), sent upstream as
  [Detanup01/gbe_fork#648](https://github.com/Detanup01/gbe_fork/pull/648): Steam Input posts its device-connected and
  configuration-loaded callbacks, which some games (No Man's Sky) wait for before they use a controller.
- [`7100368e`](https://github.com/leolobato/gbe_fork/commit/7100368ebade20324adccb4c0b5bfbbb763ab943) and
  [`ca45922c`](https://github.com/leolobato/gbe_fork/commit/ca45922c84ded9f404a61a6ae2e8a0c9593b8fa1): a new
  `steam_settings/owned_app_ids.txt` lists other apps the account owns. `BIsSubscribedApp` reports them as owned, without
  adding them to the DLC list or the installed depots. Playden lists the Source SDK Base apps the account owns there
  (Black Mesa stops with "A Source engine game is required to run mods" without 215).

They come from that commit's `emu-build-all-win-vs22` run
([42802991197](https://github.com/leolobato/gbe_fork/actions/runs/42802991197), artifacts
`emu-win-vs22-api_regular-release-x64-…` and `…-Win32-…`). Once a gbe_fork release has both changes, Playden goes back to
that release's builds. Like `release-2026_09_27`, they answer interfaces up to `SteamClient023`.

Earlier pins (games set up with them are recognised and updated, `RetiredSteamApiSha256()` in `src/runtime/steamworks.cpp`):
the first `owned_app_ids.txt` build `7100368e` (`4635ee7b…` 32-bit, `a87257fa…` 64-bit), the Steam Input build
`509fcb90` (`4007fdef…` 32-bit, `3c46a4b1…` 64-bit), `release-2026_09_27` (`cae4372b…` 32-bit,
`70d9826a…` 64-bit) and the 2026-10-07 build that stopped at `SteamClient021`.

| File | SHA-256 |
| --- | --- |
| `steam_api.dll` | `d778ba3007cc46b7587f23698493c6734752213e34eaec2d5e59a036da11df44` |
| `steam_api64.dll` | `ca4698c8d8db8270735c3570922748300abd10dc9d16c2f3a95a6a564be144d2` |

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
