# Community recipes

A community recipe tells Playden how to start one Steam game on the PS5. It is a
Playden recipe (the same JSON as `runtime/recipes/*.json`) with a few more fields
that say who tested it, where and how well. Playden reads the recipe part with
`ParseGameRecipe` and the whole file with `ParseCommunityRecipe`
(`src/runtime/game_settings.h`). The stricter rules every shared recipe must pass are
in `CommunityRecipeRefusals` (`src/runtime/community_rules.h`).

Community recipes live in their own repository,
[playden-ps5-recipes](https://github.com/leolobato/playden-ps5-recipes), not in this one. Recipes
contributed there are released under [CC0 1.0](https://creativecommons.org/publicdomain/zero/1.0/): by
sending one, you agree to that. The recipes Playden ships (`runtime/recipes/`) are CC0 1.0 too
([LICENSE](../runtime/recipes/LICENSE)).

## Share one from Playden

Get the game running well from **Game settings**, then choose **Share recipe** (at the top of Game settings once
your changes go beyond display preferences, otherwise under Advanced), or answer "It ran well" when Playden asks
how the game ran. Playden sends the recipe to the Playden API, which opens the pull request on
playden-ps5-recipes for review. No GitHub account is needed.

Who a recipe (or a test report) credits is the **Shared as** row on the Share and feedback sheets, and its default
in Settings › Community › **Share as**:

- **No name**: shown as "from the community".
- **Your name**: a name you type, no account needed.
- **Playden account**: link one (scan the code, sign in with Discord, GitHub, Google or an email link, approve the
  code). Shares then carry the account's name, and the chooser draws it in the account colour.

A recipe revision is **Verified** when the maintainers vouch for it. The chooser shows it beside the recipe's status,
and a star by the name of an author the maintainers trust.

## Make one by hand

1. Get the game to start from **Game settings** (program, arguments, library
   overrides, other rows).
2. Set the game's rating and note under **Compatibility and note**.
3. In **Game settings > Advanced**, choose **Export as recipe**. Playden writes
   `recipe-<app id>.json` next to its logs (`/data/playden/logs` on the console, `logs/`
   in the data folder on a Mac). `tools/collect_logs.py` puts it in the zip for a test report.
4. Fill in what the export leaves for you:
   - a slug, added to `recipe_id`: `community-<app id>` becomes `community-<app id>-<slug>`;
   - `community.title`, the name players see when they choose a recipe (optional);
   - `community.author`, your name (optional);
   - `locked`: empty it if the export listed rows there (see [Rules](#rules-for-community-recipes)).
5. Check it with `community_recipe_check` (built with Playden, see below), then send it as a pull
   request to playden-ps5-recipes as `recipes/<app id>/<slug>.json`.

A game can have several recipes, for example "120 Hz, pointer on left stick" and "Mouse preset": each is
its own file with its own slug.

## Format (format_version 1)

```json
{
  "recipe_id": "community-274520-120hz-left-stick-pointer",
  "recipe_version": 1,
  "app_id": 274520,
  "name": "Darkwood",
  "launch_entries": [
    {"id": "game", "label": "Darkwood.exe", "executable": "Darkwood.exe", "working_directory": "", "arguments": ""}
  ],
  "arguments": "-force-d3d11 {resolution}",
  "resolution_arguments": "-screen-width {w} -screen-height {h}",
  "dll_overrides": "d3d8,d3d9,d3d10core,d3d11,dxgi=n",
  "architecture": "pe64",
  "runtime": "wine-wow64",
  "prefix": "game",
  "graphics": "dxvk",
  "defaults": {
    "pointer_stick": "left_stick",
    "refresh": "120",
    "resolution": "3840x2160"
  },
  "locked": [],
  "steam_appid": 274520,
  "executable": "Darkwood.exe",
  "community": {
    "format_version": 1,
    "title": "120 Hz, pointer on left stick",
    "tested_kit": "0.12",
    "tested_firmware": "13.60",
    "tested_date": "2026-10-22",
    "status": "works",
    "notes": "Turn off the PS5's VRR for unsupported games, or the game stays at 60 Hz.",
    "author": "TodorPenchev",
    "contributors": []
  }
}
```

Recipe fields:

| Field | Meaning |
|---|---|
| `recipe_id` | `community-<app id>-<slug>`. See [Ids and file names](#ids-and-file-names). |
| `recipe_version` | Always `1` in a community recipe. A recipe's revision is the SHA-256 of its file. |
| `launch_entries` | The program that starts, relative to the game folder; its `working_directory` (empty: the program's folder) and its own `arguments`. |
| `arguments` | Arguments the game needs. `{resolution}` is replaced by `resolution_arguments`, with `{w}` and `{h}` from the Resolution setting. |
| `dll_overrides` | The full Wine library override list, like `d3d9,dxgi=n;dinput8=n,b`. Nothing is added to it. |
| `architecture` | `pe32` or `pe64`, from the program's PE header. |
| `graphics` | `auto`, `gdi`, `dxvk` or `opengl`. |
| `defaults` | Game settings by setting id (`SettingsCatalogue()`), only values that differ from Playden's default. |
| `locked` | Setting ids the player must choose "Change anyway" to change. Only for recipes Playden ships: empty in a community recipe. |
| `requirements` | What the runtime kit's template bottle must provide: `{"audio": "ps5"}` (Wine's PS5 audio driver) is the only one so far. On a kit without it, Playden refuses to prepare or start the game and names what is missing. |
| `registry` | Values the bottle gets under `HKEY_CURRENT_USER` when the game's files are prepared, as a Steam install script's would be: `{"Software\\Wine\\AppDefaults\\game.exe\\Direct3D": {"VideoMemorySize": "4096"}}`. Only some keys are allowed; see [Registry values](#registry-values). |
| `ini` | Values the game's own INI files get when the game's files are prepared, written only when the key is missing or empty: `{"BmGame/Config/UserEngine.ini": {"Engine.Engine": {"Language": "int"}}}`. See [INI values](#ini-values). |
| `steam_api_copies` | Shipped recipes only. Paths in the game folder that get a copy of the game's own Steam library while the emulator answers the game: `["Sandfall/Binaries/Win64/steam_api64.dll"]`. See [Steam library copies](#steam-library-copies). |
| `game_files` | Shipped recipes only. Paths in the game folder that get a file Playden ships: `["zombie/DebugFileWatcher.class"]`. See [Game files](#game-files). |

Community fields:

| Field | Meaning |
|---|---|
| `steam_appid`, `executable` | Identity: must match `app_id` and a launch entry's `executable`. |
| `community.title` | Optional. The name players see in the list of recipes: 1–60 printable characters. Without one, Playden shows "Recipe from <author>". Older Playden versions ignore it. |
| `community.tested_kit` | The kit version it was tested with, like `0.12` or a development kit such as `0.12-dev.20261022.1`. Export as recipe fills it in. It is information, not a requirement: testing on 0.12 doesn't prove 0.11 fails. |
| `community.min_kit` | Optional. The oldest kit version the recipe needs, set by hand only when it really needs a newer kit (a capability, a fix). Playden doesn't offer the recipe on an older kit. The export never fills it. |
| `community.tested_firmware`, `tested_date` | PS5 system software and day of the test (`YYYY-MM-DD`). |
| `community.status` | `works`, `playable`, `broken` or `untested`. |
| `community.notes` | Free text, including steps outside Playden (such as a PS5 setting). |
| `community.author` | Optional. Who first shared the recipe. It stays the same when others change it. Without one, the recipe shows as "from the community". |
| `community.contributors` | People whose changes to the recipe were merged since, each named once. |
| `community.base` | Optional. A prospero-win-profiles profile (repository, path, revision) that Playden uses as the base; this file's values apply on top. |

Kit versions are `<generation>.<revision>` and compare as numbers: `0.9` is older than `0.12`. A
development kit counts as the release it leads to, so `0.12-dev.20261022.1` is `0.12`.

Unknown fields are ignored. A file whose `format_version` isn't 1, whose `status` is another word, or whose
`steam_appid` doesn't match `app_id` doesn't load.

## Ids and file names

- `recipe_id` is `community-<app id>-<slug>`.
- The slug uses lowercase ASCII letters, digits and single hyphens, 1–32 characters, without a hyphen
  first or last: `120hz-left-stick-pointer`, `mouse-preset`.
- The whole id fits in 64 bytes (a 10-digit app ID and a 32-character slug make 53).
- A slug is unique for its app ID and is never used again, even after its recipe is removed.
- The file is `recipes/<app id>/<slug>.json` in playden-ps5-recipes.

Export as recipe writes `community-<app id>`, without a slug, because the slug is the author's choice.
Playden reads such a file, but the checker refuses it until it has a slug.

## Rules for community recipes

Passing the parser is not the same as being safe to offer to everyone, so a community recipe has
stricter rules than a recipe on your own console. Pull requests are checked against them, and
Playden checks them again on the console:

- **Title and author:** both are optional. When set, they follow the text rules below.
- **Text:** every text field (`title`, `author`, `contributors`, `notes`, `tested_firmware`, `tested_date`, `status`
  and the recipe's `name`) is valid UTF-8 without control characters, lone surrogates or bidirectional controls
  (U+200E, U+200F, U+202A to U+202E, U+2066 to U+2069). Only `notes` may hold new lines and tabs. Title, author and
  contributors have no space at either end, including a no-break space or another Unicode space.
- **Kit versions:** `tested_kit` and `min_kit`, when set, are real kit versions like `0.12`.
- **No locks:** `locked` is empty. Locks are for the recipes Playden ships; a community recipe can't stop
  the player from changing a row.
- **Program:** every launch entry is a `.exe` inside the game's folder, and starts in a folder inside it:
  no `..`, no absolute path, no drive letter. A folder or file name made only of dots and spaces (`...`, `.. `),
  or ending in a dot or a space, is refused, because Windows reads it as another name. The program may not be a
  Windows or Wine system program (`msiexec`, `explorer`, `rundll32`, `cmd`, `wineboot`, `steam` and the like, with
  or without `.exe`). The program, its folder, arguments and library overrides go in the recipe's own fields
  (`launch_entries`, `arguments`, `dll_overrides`), not in `defaults`, and neither does the player's own arguments
  row (`extra_arguments`). A `launch_option` that names a later program is allowed and flagged.
- **Library overrides:** named libraries only, no wildcards (`*`).
- **Environment:** the `environment` row may set only the variables Playden itself sets or documents:
  `WINE_CPU_TOPOLOGY`, `DXVK_FRAME_RATE`, `MESA_EXTENSION_MAX_YEAR`, `PW_MEDIA_DISABLE` and vkd3d-proton's
  `VKD3D_*` variables (`IsPlaydenEnvironmentVariable`, `src/runtime/prospero_profile.h`), except the two that take
  a path, `VKD3D_LOG_FILE` and `VKD3D_SHADER_CACHE_PATH`. Other variables still work in your own Game settings.
- **Registry:** `Software\Wine\AppDefaults\<program>\…` only for one of the recipe's own programs.
  A publisher key (`Software\<publisher>\…`) is allowed, but I review it before it is merged. Every
  `AppDefaults` key is listed in the review items too.

The program, arguments, library overrides, environment and registry values change how the game starts.
Pull requests that add or change them are labelled for a closer review, and Playden lists them
under "Changes how the game starts" when you choose a recipe.

### Checking a recipe

`community_recipe_check` is built with Playden's host build
(`cmake --build --preset homebrew-host --target community_recipe_check`). It reads each file with
Playden's own parser, applies the rules above, works out the game's settings on a kit with every
capability, and writes the full prospero-win profile, because some refusals only happen there:

```sh
build/homebrew-host/community_recipe_check recipes/274520/120hz-left-stick-pointer.json
```

For each file it prints `ok <path> <revision> <recipe_id>` or a `refused <path>: <reason>` line for
each problem, then `command <path>: <program and arguments>` and a `flag <path>: <item>` line for each
thing that changes how the game starts. It exits with 1 when any file is refused. `--json` prints
one object for the whole run instead, for CI:

```json
{
  "ok": true,
  "files": [
    {"path": "…", "revision": "<sha256 of the file>", "recipe_id": "…", "app_id": 274520, "slug": "…",
     "title": "…", "status": "works", "author": "…", "contributors": [], "tested_kit": "0.12",
     "min_kit": "", "tested_firmware": "13.60", "tested_date": "2026-10-22", "ok": true,
     "refusals": [], "flags": ["program: Darkwood.exe", "…"], "command": "Darkwood.exe -force-d3d11 …"}
  ]
}
```

Applying community recipes from the preset chooser is planned.

## Other stores

A recipe can name a game that is not from Steam, with two more fields:

```json
"store": "local",
"game": "local-<uuid>"
```

Give both or neither; a file with only one is refused, as is an unknown `store`. `local` is the only store so far: it
is a game imported from a folder. Without them, the recipe is for the Steam game `app_id`, as before.

An imported game is keyed by its local ID, which Playden makes when you add it, so a recipe for one only fits the
console that exported it. Community recipes still target Steam games.

## Registry values

Use `registry` only for a value the game reads from the registry and that no
other field or Game settings row sets. Playden writes the values into the
game's bottle whenever it prepares the game or applies its settings. A value the
recipe sets is written again each time, even if the game changed it in the
meantime; values the game writes under other names are left alone.

A recipe may write two kinds of key, both under `HKEY_CURRENT_USER` (write the
key without the `HKEY_CURRENT_USER\` part):

| Key | For | Example |
|---|---|---|
| `Software\Wine\AppDefaults\<program>\<section>` | Wine settings for the game's program only. | `Software\Wine\AppDefaults\ffxiiiimg.exe\Direct3D`, value `VideoMemorySize` |
| `Software\<publisher>\…` | The game's own keys: settings, install paths, accepted EULAs. | `Software\Square Enix\FINAL FANTASY XIII`, value `Language` |

Every other key is refused, and the recipe does not load. Here is where those
needs go instead:

| Refused | Why | Use instead |
|---|---|---|
| `Software\Wine\AppDefaults\<program>\DllOverrides`, `Software\Wine\DllOverrides` | Game settings would not show these overrides. | `dll_overrides` |
| `Version` under `AppDefaults\<program>`, `Control Panel\Desktop` `LogPixels` | Game settings writes them on every start. | The Windows version and text scale rows in `defaults` |
| Other `Software\Wine\…` keys (for example `Drivers`, `Direct3D`) | They change Wine for the whole bottle, including what the kit checks (`Drivers\Audio`). | A section under `AppDefaults\<program>` |
| `Environment` | It would get around the environment allow-list. | The `environment` row in `defaults` |
| `Software\Microsoft`, `Software\Classes`, `Software\Policies`, `Control Panel`, other roots | Windows' own keys, not the game's. | Ask in an issue first. |
| `HKEY_LOCAL_MACHINE` values | The field writes `HKEY_CURRENT_USER` only. | Playden applies the game's Steam install script (`installscript.vdf`), including its `HKEY_LOCAL_MACHINE` values. Ask in an issue if a game needs more. |

Other rules: string values only, at most 16 values, printable ASCII, no `"` in
names or values, no `\` in values, no `[` or `]` in keys, and each value once
(letter case does not make two values different).

## INI values

Use `ini` only for a value that the game's own launcher writes before it starts
the game. The console cannot start a second program, so the launcher cannot
start the game. Batman: Arkham Asylum starts its launcher, and stops, while
`BmGame/Config/UserEngine.ini` has no `Language` under `[Engine.Engine]`.

Playden writes the values into the game folder whenever it prepares the game or
applies its settings:

- A value is written only when its key is missing or empty in the section. A
  value that the player or the game wrote stays.
- A missing key goes after the last line of its section. A missing section goes
  at the end of the file. All other lines stay as they are, with the file's own
  line endings.
- A missing file is not made: the game makes its own file from its defaults.
  Playden also leaves a link or a UTF-16 file alone.
- File, section and key names ignore letter case, as on Windows.
- A game that Playden prepared with the recipe it made itself also gets the
  INI values of a recipe that Playden ships for the game later.

Rules: the file is a `.ini` path in the game folder, with `/` between folders
and no `.` or `..` part. At most 16 values. Printable ASCII only. No `[` or `]`
in a section, no `=`, `[` or `]` in a key, and a key does not start with `;` or
`#`. A value is not empty. Each value once (letter case does not make two
values different).

## Steam library copies

Use `steam_api_copies` only for a game that hashes its Steam library on disk and quits when the file is the emulator,
and only when the game loads the library from a different path than the one it hashes. Clair Obscur: Expedition 33
hashes `Sandfall/Binaries/Win64/steam_api64.dll` first if it exists, while Unreal loads the copy under
`Engine/Binaries/ThirdParty/Steamworks` by its full path.

Playden keeps the emulator where the game loads it. When Steam features = Offline emulation, it copies the `.orig` it
kept to each path when it prepares the game or applies its settings:

- The source is the `.orig` of the one emulated library with the same file name. If there isn't exactly one, or the
  path's folder is missing or is a link, Playden skips the copy and logs `copies_skipped=`.
- The copy is a real file, not a link, because exFAT has no links.
- With Steam features = Game's own files, Playden removes the copy.
- A game that Playden prepared with the recipe it made itself also gets the copies of a recipe that Playden ships for
  the game later.

Rules: the path is a `steam_api.dll` or `steam_api64.dll` in a folder of the game, with `/` between folders and no `.`
or `..` part. At most 4 paths, each once. A community recipe with this field is refused: it places a file that Game
settings doesn't show, and a community recipe is only offered, so the game wouldn't start until the player chose it.

## Game files

Use `game_files` only for a game that can't run on the console without a changed file, and only when nothing else in
this file can do it. Project Zomboid watches its folders for changes by holding one handle on each of its 1704 media
folders. A PS5 title can hold about 250 open files, so the game quits while it starts. The game looks for classes in
its own folder first, so a watcher that watches nothing stands in for the game's. That only stops the game reloading
files that change while it runs.

Playden copies each file from `runtime/recipes/files/<recipe id>/<path>` to `<path>` in the game folder when it
prepares the game or applies its settings:

- A file already at the path stays, whatever it holds. A game version that ships its own keeps it, and a newer file
  from Playden doesn't replace an older one there.
- Missing folders are made. A link or a file in the way of a folder is left alone and the file is skipped.
- Without the file in Playden's package, or with something in the way, the game starts without it. Playden logs
  `recipe files ... skipped=`.
- A game that Playden prepared with the recipe it made itself also gets the files of a recipe that Playden ships for
  the game later.
- Uninstalling the game removes the file with the rest of the folder.

Rules: the path is inside the game folder, with `/` between folders and no `.` or `..` part. At most 4 paths, each
once. A community recipe with this field is refused, for the same reason as `steam_api_copies`. The source of a file
that isn't plain text goes in `tools/recipe-files/<recipe id>`, with what builds it. Write it yourself: no part of
the game goes in Playden's repository.

## Shipped recipes

A recipe Playden ships lives in `runtime/recipes/` and is used for that game
instead of the one Playden makes from Steam's launch options. To add one:

1. Get the game working from Game settings and export it as a recipe (see
   [Make one by hand](#make-one-by-hand)).
2. Name the file and its `recipe_id` `steam-<app id>-<short name>-v<version>`,
   for example `steam-292120-ffxiii-v1`, and leave out the `community` block.
   Change the version only when a change needs the game prepared again.
3. Add the file name to `runtime/recipes/recipes.lst` (the console title
   cannot list folders).
4. Put in `locked` every row whose other values are known to stop the game. The
   player then sees "Required by this game" and must choose "Change anyway" to
   change it. Final Fantasy XIII locks `graphics` (its Direct3D 9 goes to DXVK
   through `dll_overrides` whatever the row says) and `steam_features` (it quits
   without the Steam emulator).
5. Set `steam_features` in `defaults` if the game needs the Steam emulator. A
   shipped recipe without it keeps the game's own Steam files.
6. Add a test in `tests/game_settings_test.cpp` that loads the recipe and
   checks its arguments and settings, and say in the pull request what you ran
   on a console.
