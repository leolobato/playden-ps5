# Online play (work in progress)

Steam games get gbe_fork in place of their `steam_api(64).dll` (`runtime/steamworks/README.md`). gbe_fork tells the
game Steam runs and the game is owned, but it never reaches Steam's servers, so online play does not work: no login
tickets for the game's servers, no lobbies, no Steam relays.

Online play needs a signed-in Steam client inside the game's process. The plan, in order:

1. **Tickets** (protocol done: `src/steam/backend/game_tickets.*`). Game connect tokens, app ownership tickets, the
   session ticket layout, the auth list that activates tickets, and encrypted app tickets. These sign the game in to
   its developer's servers (Jagex accounts, dedicated servers such as Palworld community servers).
2. **In-game Steam backend.** A steam_api backend (a gbe_fork fork in `third_party/`, pinned like the other forks)
   that logs on to a CM from inside the game and answers `GetAuthSessionTicket`, `GetAuthTicketForWebApi`,
   `RequestEncryptedAppTicket` and `BeginAuthSession` with real tickets. Playden only hands the game a token: it does
   not run while the game does, and Steam cancels a ticket when the session that activated it logs off.
3. **Lobbies, friends, rich presence and invites** (ISteamMatchmaking, ISteamFriends) over the same CM session.
4. **Steam networking** (ISteamNetworkingSockets/Messages through Steam Datagram Relay), which co-op hosted by a
   player needs. Largest part.

Not planned: kernel anti-cheat games stay unsupported whatever Steam can do.
