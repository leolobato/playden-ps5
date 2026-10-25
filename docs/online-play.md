# Online play (experimental)

Steam games get gbe_fork in place of their `steam_api(64).dll` (`runtime/steamworks/README.md`). gbe_fork tells the
game Steam runs and the game is owned, but it never reaches Steam's servers.

## Steam features = Online

Game settings › More settings › Steam features › **Online (experimental)** keeps gbe_fork and puts Playden's online
bridge (`tools/steam-online`) in front of it, 64-bit and 32-bit:

- The game's `steam_api64.dll` / `steam_api.dll` becomes the bridge, with gbe_fork beside it as `steam_api64_gbe.dll` /
  `steam_api_gbe.dll`. Every Steam call the bridge does not answer goes to gbe_fork.
- `steam_settings/playden_online.txt` holds the account name, SteamID and **refresh token** (mode 0600). Anything that
  can read the game's folder can read the token. Switching the game back to Offline emulation or Game's own files, or
  preparing it while signed out, removes the file.
- At start the bridge signs in to a Steam CM over WebSocket (WinHTTP), keeps the session with heartbeats, and shows the
  account online and playing the game.

What it answers through Steam:

| Interface | What the game gets |
| --- | --- |
| ISteamUser | Real session and Web API tickets (game connect token, SteamID, ownership ticket), activated with Steam through the auth list; `CancelAuthTicket`; Steam's encrypted app ticket. `BeginAuthSession`/`EndAuthSession` (hosting) put other players' tickets on the auth list; a Steam refusal reaches the game as `ValidateAuthTicketResponse_t`. |
| ISteamMatchmaking (009) | Lobbies on Steam's matchmaking servers: list with filters, create, join, leave, lobby and member data, chat, game server, owner, type, member limit, linked lobby, invites. |
| ISteamFriends (015-018) | The friends list, names, states, and the game and lobby a friend is in (`GetFriendGamePlayed`). |
| Invites | There is no Steam overlay on the console: an invite to a lobby is accepted at once (`GameLobbyJoinRequested_t`). |
| ISteamUtils, callbacks | The bridge's API calls complete through `IsAPICallCompleted`/`GetAPICallResult`, `CCallResult`, or manual dispatch; its callbacks reach `SteamAPI_RunCallbacks` and manual dispatch. |

When Steam can't be reached in time, tickets fall back to gbe_fork's and lobby calls fail with "no connection".

**On a PS5:** RuneScape: Dragonwilds (Redpoint EOS, Web API ticket for `epiconlineservices`) signs in to its online
services: Steam validates the ticket (`ticket auth complete ... response=0`) and the game lists and creates lobbies.
Co-op through Steam's relays is still untested (see Not done). How it got there, and the dead ends, are in
`online-play-console-findings.md`.

## Tickets

What Steam and a game's servers expect, as SteamKit (`SteamAuthTicket`) and node-steam-user build it:

- The ticket is three length-prefixed sections: the game connect token (20 bytes, sent by Steam ready-made: token,
  SteamID, issue time), the session header (24 bytes: 1, the ticket type, public IP, 0, time, sequence) and the app
  ownership ticket.
- The ticket type is 2 for `GetAuthSessionTicket` and 5 for `GetAuthTicketForWebApi`.
- `ClientAuthList` lists the game's own ticket by its first two sections only (52 bytes) and their CRC. A Web API
  ticket also carries its identity there as `server_secret`: `str:<identity>` and a NUL.
- Games keep `GetCSERIPPort` at slot 7 of ISteamUtils, so the API call methods the bridge answers are at slots 11-13.

## The log

`steam_settings/playden_online.log`, beside the bridge: logon result, presence, ownership ticket, tickets handed out
and Steam's acks, other players' tickets checked, lobbies created, listed, joined and left, invites, friends count, the
interfaces bridged, and a line for every Steam networking interface the game asks for. Never a token or ticket bytes.
The first call to each flat-API ticket function (`SteamAPI_ISteamUser_*`) is noted, and a fatal crash in the game
leaves `CRASH` lines: the exception, the module and offset it happened in, and the modules on its stack.
`tools/collect_logs.py` does not pick it up yet: copy it from the game's folder.

## Not done

- **Steam networking** (ISteamNetworkingSockets/Messages and ISteamNetworking P2P through Steam Datagram Relay) is not
  bridged. It needs Steam's relay protocol and its cryptography (Ed25519 certificates, Curve25519, AES-GCM), none of
  which exist in the bridge. Co-op that sends its traffic through Steam (Palworld invite codes, most UE "Steam sockets"
  games) will find the lobby but not connect. The log names the networking interface the game uses and checks that
  Steam's relay list (`GetSDRConfig`) is reachable.
- **Rich presence** and `InviteUserToGame` (connect-string invites) are not bridged.
- **Kernel anti-cheat** games stay unsupported.

## Protocol

`src/steam/backend/game_tickets.*` holds the ticket messages in Playden's C++ backend, with tests
(`tests/steam_game_tickets_test.cpp`). Lobby and friend message field numbers come from Steam's
`steammessages_clientserver_mms.proto` and `steammessages_clientserver_friends.proto`.
