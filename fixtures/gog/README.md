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
