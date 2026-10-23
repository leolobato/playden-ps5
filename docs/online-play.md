# Online play (experimental)

Steam games get gbe_fork in place of their `steam_api(64).dll` (`runtime/steamworks/README.md`). gbe_fork tells the
game Steam runs and the game is owned, but it never reaches Steam's servers: no login tickets for the game's servers,
no lobbies, no Steam relays.

## Steam features = Online

Game settings › More settings › Steam features › **Online (experimental)** keeps gbe_fork for everything and adds a
real Steam sign-in inside the game (64-bit games only):

- The game's `steam_api64.dll` becomes Playden's online bridge (`tools/steam-online`), with gbe_fork beside it as
  `steam_api64_gbe.dll`. The bridge forwards every Steam call to gbe_fork.
- `steam_settings/playden_online.txt` holds the account name, SteamID and **refresh token** (mode 0600). Anything that
  can read the game's folder can read the token. Switching the game back to Offline emulation or Game's own files, or
  preparing it while signed out, removes the file.
- At start the bridge logs on to a Steam CM over WebSocket (WinHTTP) and keeps the session with heartbeats. It asks
  Steam for the game's app ownership ticket and keeps the game connect tokens Steam sends.
- `ISteamUser::GetAuthSessionTicket` and `GetAuthTicketForWebApi` return real session tickets (game connect token,
  SteamID, ownership ticket), activated with Steam through the auth list, so a game's servers can check them with Steam.
  `CancelAuthTicket` takes them off the list. `RequestEncryptedAppTicket` / `GetEncryptedAppTicket` return Steam's
  encrypted app ticket.
- The log is `steam_settings/playden_online.log` beside the DLL: logon result, tickets handed out and Steam's
  answers, never a token or ticket bytes.

When Steam can't be reached within 15 seconds, the game gets gbe_fork's ticket and plays as before.

This targets games that sign in to their own backend or join dedicated servers with a Steam ticket (for example a
Jagex account in RuneScape: Dragonwilds, Palworld community servers). It has not been tried on a console yet.

## Not done

- **Lobbies, friends, rich presence and invites** (ISteamMatchmaking, ISteamFriends) are still gbe_fork's, so joining a
  friend through Steam does not work.
- **Steam networking** (ISteamNetworkingSockets/Messages through Steam Datagram Relay), which co-op hosted by a player
  needs (Palworld invite codes), is not implemented.
- **Hosting**: `BeginAuthSession` (a listen server checking other players' tickets) is still gbe_fork's.
- 32-bit games keep plain emulation.
- Kernel anti-cheat games stay unsupported.

## Protocol

`src/steam/backend/game_tickets.*` holds the same messages in Playden's C++ backend, with tests
(`tests/steam_game_tickets_test.cpp`), as the reference for the bridge's C.
