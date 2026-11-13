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
