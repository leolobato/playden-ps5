# GOG fixtures

Answers of GOG's token endpoint (`auth.gog.com/token`) for the `gog_auth` test. Every file is public or synthetic: none
holds a real user ID, account name, email address or token.

- `token-code-invalid-grant.json`: GOG's real answer (400) to a made-up sign-in code, recorded without signing in.
- `token-refresh-invalid-grant.json`: the 400 answer to a refresh token GOG doesn't know, written by hand from the
  real shape (the real one also names the public Galaxy client ID).
- `token-success.json`, `token-success-rotated.json` (another refresh token), `token-success-no-expiry.json` (no
  `expires_in`): synthetic 200 answers in the shape GOG_PROTOCOL.md §1 lists, with made-up tokens, a zero session ID
  and the user ID `1000000000000000001`.

Answers of `embed.gog.com/userData.json` for the `gog_session` and `gog_account` tests. All synthetic: the spike had no
token, so the real shape is not verified. Playden reads only `username`.

- `user-data.json`: the smallest answer, a made-up username and the user ID `1000000000000000001`.
- `user-data-extra-fields.json`: the same with many other fields (objects, arrays, nulls) a fuller answer may hold,
  all made up.
- `user-data-no-username.json`: no `username`; Playden refuses it.

Answers of gamesdb (`gamesdb.gog.com/platforms/gog/external_releases/<id>`) and the owned list for the `gog_api`
test. gamesdb is public: these were recorded without signing in and name no account.

- `gamesdb-game-1351891846.json` (Jazz Jackrabbit 2: The Secret Files, a Windows game) and
  `gamesdb-spam-1440166342.json` (an entry typed `spam`): GOG's real answers, unchanged.
- `gamesdb-game-with-dlcs-1441974651.json` (Prison Architect, 12 DLC, a developer): GOG's real answer with
  `game.releases` cut to its `gog` and `generic` entries. The full list names releases on other stores, some with
  account-like `@` suffixes.
- `gamesdb-dlc.json` (type `dlc`), `gamesdb-no-windows.json` (Linux and macOS only) and `gamesdb-invisible.json`
  (`visible_in_library` false): synthetic, made from the 1351891846 answer with made-up product IDs 1900000101-103.
- `owned.json`: a synthetic `embed.gog.com/user/data/games` answer naming the products above. The real shape is from
  GOG_PROTOCOL.md §2; the spike had no token to record one. Large lists are made by the test.

GOG's content system for the `gog_manifest_v2` test: builds, build metas, depot manifests and the dependency store. All
are public answers (no sign-in), recorded on 2026-11-13. They are file lists and settings, not game content, and name
no account.

- Builds (`content-system.gog.com/products/<id>/os/windows/builds?generation=2`, unchanged):
  - `builds-v2-branches-first-1351891846.json`: Jazz Jackrabbit 2 with `_version=2`. The first five items are
    `JJ2+ Mod` branch builds; the default one is the sixth (`50747586883966799`).
  - `builds-link-form-1351891846.json`: the same without `_version=2`, so items carry `link` instead of `urls`.
  - `builds-v2-1440148836.json`: Fallout, one gen 2 item on the page (`count` 1 of 3).
  - `builds-v2-gen1-only-1425039730.json`: Monkey Island 2 Special Edition, whose default build is gen 1 only.
- Build metas (`gog-cdn-fastly.gog.com/content-system/v2/meta/…`, zlib JSON). Each game's `clientId` is replaced with
  `50000000000000001` and its `clientSecret` with zeros, then the JSON is compressed again:
  - `meta-v2-1351891846.zlib`: Jazz Jackrabbit 2, depots in the deprecated language code `en`.
  - `meta-v2-dlc-osbitness-1441974651.zlib`: Prison Architect, `osBitness` `["64"]`, 11 DLC products.
  - `meta-v2-scummvm-1207658695.zlib`: Beneath a Steel Sky, 21 one-file gog depots across 7 languages, needs ScummVM.
  - `meta-v2-dosbox-1435828982.zlib`: The Elder Scrolls: Arena, needs `DOSBox074_2CS`.
- Depot manifests (`…/content-system/v2/meta/…`, zlib JSON, unchanged):
  - `depot-v2-1351891846.zlib`: Jazz Jackrabbit 2's main depot, 250 files with `/` paths, sfcRef and a small-files
    container after `items`.
  - `depot-v2-1351891846-gogdepot.zlib`: its gog depot, `goggame-1351891846.hashdb` and `.info`.
  - `depot-v2-1435828982.zlib`: Arena's main depot, 323 items with `\` paths, a file with no chunks, a four-chunk file.
  - `depot-v2-1435828982-gogdepot.zlib`: its `goggame-1435828982.hashdb` depot.
  Support files (`app\…`, `save\…`) are in other depots of these games; the test makes its own.
- Dependency store (`content-system.gog.com/dependencies/repository?generation=2` and the CDN, unchanged):
  - `dependencies-repository.json`: the repository answer.
  - `dependencies-manifest.zlib`: its manifest, 69 dependencies (the `signature` fields are GOG's own).
  - `dependency-depot-DOSBox074_2CS.json`, `dependency-depot-ScummVM.json`: two dependency depot manifests, inflated.
  - `dependency-depot-XNA.json`: the `__redist` depot manifest of XNA 3.1, inflated and unchanged (recorded on
    2026-11-14): one file, `__redist/XNA/xnafx31_redist.msi`, the program the repository manifest names. For the
    `gog_dependencies` test.

GOG's gen 1 content system for the `gog_manifest_v1` test (and the gen 1 builds item for `gog_manifest_v2`). All are
public answers (no sign-in), recorded on 2026-11-13 and unchanged. They are file lists and settings, not game content,
and name no account.

- `builds-gen1-1440148836.json`: a builds answer with Fallout's gen 1 build item, `legacy_build_id` 46964530. fastly
  supports generations 1 and 2, gcore only 2.
- `repository-v1-1425039730.json`: Monkey Island 2 Special Edition's gen 1 repository
  (`gog-cdn-fastly.gog.com/content-system/v1/manifests/1425039730/windows/32971092/repository.json`, plain JSON): a
  Neutral and an English depot with sizes as strings, four redist depots (`language_setup` has no executable) and one
  support command.
- `depot-v1-1425039730-1bf89566-2d44-40e7-81b1-06b0c8e716e4.json`: its Neutral depot manifest, one support file
  (`1425039730/galaxy_monkey_island2_se_2.0.0.10.exe`) at offset 0 of `main.bin`.
- `depot-v1-1425039730-44877313-0ade-439d-88ae-0538513fb165.json`: its English depot manifest, 40 files with offsets
  into `1425039730/main.bin`, one of them the support file `1425039730/app/lang.ini`.

Answers for the `gog_download` test (the downloader against a stub CDN). The test makes its own game (random files,
chunks compressed with miniz, manifests, meta and builds); these files are the answers it can't make:

- `dependencies-open-link.json`: GOG's real `content-system.gog.com/open_link?generation=2&_version=2&path=/dependencies/store/`
  answer, unchanged: the public dependency store's unsigned chunk bases (fastly, then gcore as `fallback_only`).
- `dependency-chunk-ede30538132b194f6a477a932441df95.zlib`: one real chunk from that public store (DOSBox's
  `AUTHORS.txt`, 213 bytes compressed), with `dependency-chunk.expected.json` holding its `compressedMd5`,
  `compressedSize`, `md5` and `size` (264). No purchased game content.
- `secure-link-401.json`: GOG's real answer to a `secure_link` request without a good token.
- `secure-link-200.json`: synthetic. It has the two endpoint shapes a real `secure_link` answer has (recorded on
  2026-11-13 and redacted, never copied): fastly's `url_format` `{base_url}/token=nva={expires_at}~dirs={dirs}~token={token}{path}`
  with a numeric `expires_at` (1900000000) and `path` `/content-system/v2/store/<product>`, and gcore's
  `{base_url}/{path}?wsSecret={token}&wsTime={time}&prefix={prefix}` with no expiry and `path` without its leading `/`.
  The tokens are made up and `base_url` points at stub hosts (`https://cdn.example.invalid`,
  `https://gcore.example.invalid`). The product is the made-up `1900000001`; the stub puts another product's ID in its
  place.
- `secure-link-gen1-200.json`: synthetic, for `gog_api` and the `gog_download` byte-range cases. It has the shape of
  GOG's real answer to `secure_link?_version=2&type=depot&path=/windows/<timestamp>/` for a gen 1 build (recorded on
  2026-11-14 and redacted, never copied): fastly with `dirs` 6 and `path` `/content-system/v1/depots/<product>/windows/<timestamp>`
  (no trailing `/`; `main.bin` is that path plus `/main.bin`), and gcore, which lists only generation 2, with `path`
  and `prefix` the same folder (`path` without its leading `/`). The tokens are made up, `base_url` points at the stub
  hosts, the product is `1900000001` and the timestamp the made-up `31000007`; the stub puts the requested product and
  timestamp in their place.

Play tasks and install scripts for the `gog_info_file` and `gog_install_script` tests. All synthetic: the real files
come from purchased builds and stay out of the repository.

- `goggame-1900000001.info` and `goggame-1900000001.script`: the exact shape of a locally installed game's files (a
  `launcher` primary task, a task `icon`, a hidden `game` task, a `tool`, a `document` FileTask and a `document`
  URLTask; a `setIni` action with `conditions` and `keyType`), with made-up IDs, names and paths.
- `goggame-1900000002.info`: the shape of Jazz Jackrabbit 2's info file as read on 2026-11-13 (a primary `game`
  FileTask named `jazz2.exe` in lower case while the build's file is `Jazz2.exe`, with `compatibilityFlags`; a `tool`
  FileTask `jcs.exe`; a `document` URLTask; no `workingDir`). The IDs, names and link are made up.
- `goggame-1900000003.script`: `setRegistry` actions in the shape of the 25 in 10 owned games' scripts read on
  2026-11-14 (the DOSBox games' `DOSBOXFOLDER` under `Software\GOG.com\Games\{productID}` with `valueType`
  `String`; `string` in lower case, `saveGameData`, a `SOFTWARE\` key in capitals and a `{userappdata}` value as in
  Monkey Island SE; a value named after a program path under `AppCompatFlags\Layers`; a key with no value, as Jazz
  Jackrabbit 2's). The
  `dword`, `binary`, HKEY_CLASSES_ROOT, other-language, control-character and `{publisher}` actions are made up to
  test what is skipped: no owned game's script had a `dword`. Keys, names and values are made up.
