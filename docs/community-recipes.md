# Community recipes

A community recipe tells Playden how to start one game on the PS5: a Steam game, or a game imported from a folder
(GOG and Epic games once Playden has those stores). It is a
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

## Turn them on

Community recipes are an experimental feature. Nightly builds have them on. Other builds need a line
`community-recipes` in `features.txt` in Playden's data folder (`/data/playden/features.txt` on the console, which FTP
and the file manager can write; one feature name per line, `#` starts a comment). Playden reads the file when it
starts. My presets don't need it.

Without it, Playden doesn't download recipes, ask how a game ran, send votes or test reports, or link an account, and
the screens for them are hidden. A game that already uses a community recipe keeps it: Playden reads it from the copy
it saved, and the game can still go back to a preset. A nightly build can't turn community recipes off.

## Share one from Playden

Get the game running well from **Game settings**, then choose **Share recipe** (near the end of Game settings, just
above Reset all to preset), or answer "It ran well" when Playden asks
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
   `recipe-<game>.json` (`recipe-274520.json`, `recipe-local-<uuid>.json`) next to its logs (`/data/playden/logs` on the console, `logs/`
   in the data folder on a Mac). `tools/collect_logs.py` puts it in the zip for a test report.
4. Fill in what the export leaves for you:
   - a slug, added to `recipe_id`: `community-<store>-<key>` becomes `community-<store>-<key>-<slug>`;
   - `community.title`, the name players see when they choose a recipe (optional);
   - `community.author`, your name (optional);
   - `locked`: empty it if the export listed rows there (see [Rules](#rules-for-community-recipes)).
5. Check it with `community_recipe_check` (built with Playden, see below), then send it as a pull
   request to playden-ps5-recipes as `recipes/<store>/<key>/<slug>.json`.

A game can have several recipes, for example "120 Hz, pointer on left stick" and "Mouse preset": each is
its own file with its own slug.

## Format (format_version 1)

```json
{
  "recipe_id": "community-steam-274520-120hz-left-stick-pointer",
  "recipe_version": 1,
  "game": {"store": "steam", "key": "274520", "name": "Darkwood"},
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
| `recipe_id` | `community-<store>-<key>-<slug>`. See [Ids and file names](#ids-and-file-names). |
| `game` | The game it is for: `{"store", "key", "name"}`. See [Games](#games). |
| `made_on` | Optional. The store of the copy it was made with, when that isn't `game.store`: `"imported"` for an imported copy shared under its Steam app ID. |
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
| `executable` | Identity: must match a launch entry's `executable`. |
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

Unknown fields are ignored. A file whose `format_version` isn't 1 or 2, whose `status` is another word, whose `game`
isn't a store and its key, or whose `recipe_id` isn't made from its `game` doesn't load. The `app_id` and `steam_appid`
of earlier files are refused: the `game` object replaced them.

## Settings-only recipes (format_version 2)

A settings-only recipe changes Game settings and leaves the program to Playden. Use one when the settings a game needs
are known (the Mouse preset, OpenGL, vkd3d-proton, a resolution) but not the program file: a compatibility report
often says which settings worked without naming the file. A recipe exported from Playden always has its program, so it
is format 1. A settings-only recipe is written by hand, or shared from a game based on one (see below).

```json
{
  "recipe_id": "community-steam-274520-mouse-preset",
  "recipe_version": 1,
  "game": {"store": "steam", "key": "274520", "name": "Darkwood"},
  "name": "Darkwood",
  "graphics": "opengl",
  "defaults": {
    "controller_preset": "mouse",
    "d3d12": "vkd3d-proton"
  },
  "community": {
    "format_version": 2,
    "title": "Mouse preset",
    "tested_kit": "0.19",
    "tested_date": "2026-11-08",
    "status": "works",
    "notes": "Report: https://github.com/leolobato/playden-ps5/issues/…",
    "author": "",
    "contributors": []
  }
}
```

Playden fills in the program from its own recipe for the player's copy: the one it ships for the game, else the one it
makes from Steam's launch options and the game's folder (`FillSettingsOnlyRecipe`, `src/runtime/recipe_choice.h`).
Each recipe field is one of three kinds:

| From Playden's recipe for the copy | From the file | Refused |
|---|---|---|
| `launch_entries`, `architecture`, `runtime`, `prefix`, `detected`, `dll_overrides`, `arguments`, `resolution_arguments` | `defaults`, `locked` (empty, as in every community recipe), `requirements`, `name`, the `community` block | the fields in the first column, `executable`, `registry`, `ini`, `game_files`, `steam_api_copies` |

`graphics` may be in the file: when it is, it wins over Playden's; without it, Playden's applies. OpenGL is
`"graphics": "opengl"`, not a default, because the Graphics row always comes from the recipe's field.
vkd3d-proton is the `d3d12` default. `registry` and `ini` values belong to a program, so only a recipe with its own
program (format 1) may carry them.

The program rows may still be set in `defaults`, where format 1 refuses them:

- `program`: a program file, relative to the game folder. When the player's copy has it, it starts, built as the copy's
  file says (32- or 64-bit). When it doesn't, Playden's program starts and the program rows below are dropped, as for
  [another game's recipe](#search-by-name-and-linked-games); the chooser says "Bin/Game.exe isn't in this copy, so
  Playden's program is used."
- `launch_option`: one of Playden's launch options for the game, by its id. One Playden doesn't have is dropped with
  the program rows. A `program` default wins over it.
- `program_arguments` and `program_folder`: the arguments and starting folder of the program that starts.

`arguments`, `extra_arguments`, `dll_overrides`, `architecture` and `graphics` defaults are refused.

When Playden has no program for the copy (it couldn't make a recipe for it), the recipe doesn't apply, and Game
settings says "Playden found no program to start in this copy." The chooser's side panel says "Uses the program
Playden finds" for a recipe that names no program, and lists only the program rows the file sets under "Changes how
the game starts". A settings-only recipe is matched to a game by its `game` alone, like every community recipe, and to
an installed game by its id and revision.

Sharing a game whose recipe is a settings-only one depends on whether the player changed a program row
(`IsProgramRow`: `launch_option`, `program`, `program_folder`, `program_arguments`, `architecture`, `arguments` or
`dll_overrides` in Game settings):

- **Program kept** (Playden's program, or the one the recipe names): the share is a settings-only recipe too
  (`BuildSettingsOnlyRecipe`). It has the base's defaults, every other row the player changed (not Extra arguments,
  the player's own row), and `graphics` when that changed, and the base's own program rows as they are. Picking the
  same program the recipe names, however its path is written, is no change. The usual rules apply, so Suggest changes is offered
  (and pre-selected unless graphics changed), and the recipe stays settings-only when the change is merged. Without
  changes, Playden offers a thumbs up.
- **Program changed:** the share is a recipe with its program (format 1), always as a new recipe that credits the
  settings-only one as `based_on` (`"choice": "new"`), for another game's settings-only recipe too, and never as an
  update of the game's own earlier share. An update would turn a recipe for every copy into one for that
  program only, and a format 1 recipe for the game's own copy doesn't fall back to Playden's program when the file is
  missing.

Export as recipe always writes format 1.

### Format versions

`community.format_version` says which Playden can read a file. Each Playden reads a set of versions and refuses a
file of any other version alone, with a log line in `community.log`; the rest of the release still loads. This
Playden reads 1 and 2. Earlier builds read only 1, so they skip settings-only files. Release builds read community
recipes only when they are turned on (see [Turn them on](#turn-them-on)), so the builds that skip them are mostly
older nightlies.

- The version goes up when an older Playden would read a file wrongly or can't apply it. A new optional field that
  older builds may ignore safely doesn't need a new version.
- `index.json` in a release keeps its own `format_version` 2. Changing it would make every older Playden refuse the
  whole release.
- There is no minimum Playden version field: the parser knows what it can read. `min_kit` stays for what the runtime
  kit must provide.

## Games

A recipe belongs to one store's game, its `store` and `key`:

| `store` | `key` |
|---|---|
| `steam` | The Steam app ID: digits, no leading zero, at most 4294967295. |
| `gog` | The GOG product ID: 1–20 digits, no leading zero. |
| `epic` | The Epic catalog item ID: 32 lowercase hex digits. |
| `imported` | A name key made from the game's name: 1–48 of `a-z` and `0-9`, single hyphens, no hyphen at either end. |

Playden makes a name key (`NameKey`, `src/library/game_ref.h`) the same way as the recipes repository's checker and the
Playden API: the name in Unicode NFKD with every mark (category M) dropped, lowercased, every run of other characters
than `a-z` and `0-9` made one `-`, `-` trimmed at both ends, cut to 48 bytes and trimmed again, and `game` when
nothing is left. "TMNT: Mutant Melee" is `tmnt-mutant-melee`, "Crème Brûlée & Clockwork — déjà vu" is
`creme-brulee-clockwork-deja-vu`, "ＦＵＬＬ　ＷＩＤＴＨ" is `full-width`. A maintainer may rename an imported game's
key in review until it has a published recipe.

`game.name` is the game's name as the sharer's store or folder shows it: 1–120 characters with at least one visible
character, and no control, format, private-use or unassigned characters and no line breaks. Spaces of any width are
fine.

Which recipes a game gets, matched only by ID:

- A Steam game: those of `steam/<its app ID>`.
- An imported game with a Steam app ID (its `steam_appid.txt`, or the **Steam app ID** in Game settings): those of
  `steam/<that ID>`. Without one, none: an imported game is never matched by its name.

Games are never mapped between stores. A player who wants another game's recipe (the same game from another store, or
an imported copy under another name) searches for it by name, below.

What a share or an export is for: the same `steam/<ID>`, with `"made_on": "imported"` for an imported copy, or for an
imported game without a Steam app ID `imported/<name key of its title>`.

A share saved before shares recorded their game (Playden builds before store and key) counts as made for
`steam/<app ID>` on a Steam game, the only kind that could share then, so its next share updates it. On any other game
it counts as unknown, and the next share is a new one.

### Search by name and linked games

The chooser (Game settings › Preset, and the Install sheet's Settings row) has **Search recipes by game name…**. It
opens the keyboard with the game's name, and lists the games of the release that have community recipes and whose
name matches, each with its store (Steam, Epic, GOG or Imported) and how many recipes it has. A game without recipes
is never found, and the searching game's own recipes are already in the chooser, so they aren't listed again. When a
game has no recipes of its own (an imported game without a Steam app ID, for example), the chooser's "None yet" row
points at the search.

The search compares words (`SearchWords` and `NameMatchesSearch`, `src/library/game_ref.h`). The name and the query
are normalised the way `NameKey` does it (Unicode NFKD, every mark dropped, case folded), but letters and digits of
every script are kept; each run of letters and digits is a word. A name matches when every word of the query begins
one of its words, in any order: "creme bru" and "clock" find "Crème Brûlée & Clockwork — déjà vu", "utant" doesn't
find "TMNT: Mutant Melee". A query without words lists every game with recipes. Results are sorted by name, then
store.

Choosing a result lists that game's recipes, with the same rows and side panel as the game's own; Circle goes back to
the results, and again to the game's own recipes. Choosing one of them:

- records the game it is for with the choice (`community_recipe.game` in the game's settings, `"<store>/<key>"`): the
  runtime takes a community recipe for a game only when it is for the game's own `GameRef` or for the game its choice
  records, so an unrelated recipe is still refused. An installed game's record is checked the same way before its
  recipe is bound to the game (the linked game counts too, for a choice changed since): a record naming another
  game's recipe that neither the game nor its settings name resolves to nothing, and nothing is published from it;
- links that game (`linked_game` in the game's settings): the chooser lists its recipes after the game's own, under
  "Linked game's recipes", and a newer revision of the chosen one shows as **Updated recipe available**, as for the
  game's own recipes. **Unlink …** at the end of that list goes back to the game's own recipes; the current choice
  stays until another is chosen (it keeps working, without updates).

Records written before linking have neither field: the choice is one of the game's own recipes, and nothing is linked.

Another game's recipe is offered even when its program isn't in this copy. Its program is the one it selects
(`SelectedProgram`: its `program` default, else the launch entry its `launch_option` default names, else its first
entry); when that file isn't in the game's folder (any case), the game starts Playden's own program instead: Playden's
launch entries and architecture, and none of the recipe's defaults or locks for the launch option, program, program
folder, program arguments or architecture (`UseOwnProgram`, `src/runtime/recipe_choice.h`). Everything else is the
recipe's. When the file is there but built for the other architecture, this copy's architecture is used. The Preset
row and the side panel say so: "Made for Night Sky · GOG: NightSky64.exe isn't in this copy, so Playden's program is
used.", and the side panel's program line reads "program: Game.exe, Playden's (NightSky64.exe isn't in this copy)".
Sharing a game based on another game's recipe credits it as `based_on` but never offers Suggest changes: the share is a
new recipe for this game (or updates this game's own earlier share). The chooser only knows the game's own launch entries and the folder's programs once scanned (it asks
for a scan); the runtime checks the folder itself when it prepares or publishes the game. Your changes that don't
apply to the new recipe are listed when you switch, as for any recipe switch.

## Ids and file names

- `recipe_id` is `community-<store>-<key>-<slug>`: `community-steam-274520-120hz-left-stick-pointer`,
  `community-imported-tmnt-mutant-melee-arrow-keys`.
- The slug uses lowercase ASCII letters, digits and single hyphens, 1–32 characters, without a hyphen
  first or last: `120hz-left-stick-pointer`, `mouse-preset`.
- The whole id fits in 128 bytes.
- An id is only ever made from the game and the slug and compared; nothing splits it to find its game (a name key has
  hyphens too). The store and key come from the file's path or its `game`.
- A slug is unique for its game and is never used again, even after its recipe is removed.
- The file is `recipes/<store>/<key>/<slug>.json` in playden-ps5-recipes; its `recipe_id` and `game` must agree with
  the path.

Export as recipe writes `community-<store>-<key>`, without a slug, because the slug is the author's choice.
Playden reads such a file, but the checker refuses it until it has a slug.

An installed game's record and title hold its recipe id in 64 bytes (the runtime kit's title files and Playden's
publication journal). Every Steam game's id fits (at most 59 bytes); an imported game's can be longer (up to 100
bytes). Playden publishes a longer id as its alias, `community-h-<the first 24 lowercase hex of the SHA-256 of the
full id>` (`InstalledRecipeId`, `src/runtime/game_settings.h`), and maps it back from its own records: the recipe's
object, found by the revision the record holds, and the game's settings name the full id.

## Rules for community recipes

Passing the parser is not the same as being safe to offer to everyone, so a community recipe has
stricter rules than a recipe on your own console. Pull requests are checked against them, and
Playden checks them again on the console:

- **Title and author:** both are optional. When set, they follow the text rules below.
- **Game:** `game.name` follows the game name rule in [Games](#games); `made_on` is left out when it is `game.store`.
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
- **Settings-only program rows:** in a settings-only recipe the `program` and `program_folder` defaults follow the
  same rules as a launch entry's program and folder. Its `arguments`, `extra_arguments`, `dll_overrides`,
  `architecture` and `graphics` defaults are refused (see [Settings-only recipes](#settings-only-recipes-format_version-2)).
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
build/homebrew-host/community_recipe_check recipes/steam/274520/120hz-left-stick-pointer.json
```

For each file it prints `ok <path> <revision> <recipe_id>` or a `refused <path>: <reason>` line for
each problem, then `command <path>: <program and arguments>` and a `flag <path>: <item>` line for each
thing that changes how the game starts. A settings-only recipe is filled from a stand-in for Playden's own recipe
(one 64-bit program, DXVK, and the copy has every program the file names), and its command names `<Playden's program>`
for that program. It exits with 1 when any file is refused. `--json` prints one object for the whole run instead, for
CI, with each file's `format_version` (0 when it doesn't parse):

```json
{
  "ok": true,
  "files": [
    {"path": "…", "revision": "<sha256 of the file>", "recipe_id": "…", "store": "steam", "key": "274520", "slug": "…",
     "format_version": 1,
     "title": "…", "status": "works", "author": "…", "contributors": [], "tested_kit": "0.12",
     "min_kit": "", "tested_firmware": "13.60", "tested_date": "2026-10-22", "ok": true,
     "refusals": [], "flags": ["program: Darkwood.exe", "…"], "command": "Darkwood.exe -force-d3d11 …"}
  ]
}
```

Applying community recipes from the preset chooser is planned.

## Recipes for imported games on this console

A recipe Playden makes for one console's imported game (not a community recipe) names it with two more fields:

```json
"store": "local",
"game": "local-<uuid>"
```

Give both or neither; a file with only one is refused, as is an unknown `store`. `local` is the only store so far.
Without them, the recipe is for the Steam game `app_id`. An imported game is keyed by its local ID, which Playden makes
when you add it, so such a recipe only fits that console. A community recipe has neither field nor `app_id`: its
`game` object names its game on every console.

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
