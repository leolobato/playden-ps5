# Online play (experimental)

Game settings › Steam features offers three modes for a Steam game:

| Mode | What the game gets |
| --- | --- |
| Offline emulation | gbe_fork in place of its `steam_api(64).dll` (`runtime/steamworks/README.md`). gbe_fork tells the game that Steam runs and the game is owned, but it never reaches Steam's servers. Steam network says whether the game is told Steam is online; lobbies and servers then reach only other consoles or PCs running a Steam emulator on the same network. |
| Online (experimental) | Valve's own Steam client, running beside the game and signed in as the player (the Steam host). The game keeps its own Steam library. |
| Game's own files | The game's Steam library, unchanged, with no Steam running. |

## Turning it on

Online and Steam network are experimental features. They are off until `features.txt` in Playden's data folder
(`/data/playden/features.txt` on the console, writable over FTP) has a line `online-steam`. Playden reads the file
once at startup, so restart Playden after you change it. Blank lines and lines that start with `#` are skipped.

When the feature is off:

- Steam features does not offer Online, and Steam sign-in and Steam network are not listed.
- Offline emulation tells the game that Steam is offline, and the emulator stays off the network.
- At startup, Playden resets each game that is set to Online, and its Steam sign-in, to the game's default, and
  removes a kept sign-in file. To use Online again, choose it again.

Online also needs a runtime kit whose title can run a second program beside the game (kit capability
`helper-process`). Without one, the game's default applies and the row says why; a game whose title still runs an
older version of the runtime kit keeps Online for its update.

## How Online works

- **Two processes, one Wine server.** The game's PS5 title starts a local process beside the game
  (`sceSystemServiceAddLocalProcess`, as `bdj.elf`), gives it the `/data` grant through the title's helper, and passes it
  a connection to the title's in-process Wine server (prospero-win Wine patches 1011 to 1013). That process runs
  Playden's `steam.exe` (`tools/steam-host`, `runtime/steam-host/README.md`). The profile's `[helper]` section names it,
  the file it writes when the game may start, and how long the title waits.
- **The second process has no GPU.** The system skips its module preload, so it loads libSceNet itself (by its module
  ID, before the grant) and runs Wine headless (`WINE_PS5_HEADLESS`, Wine patch 1014): no Vulkan driver, and the
  display configuration and desktop stay the game's.
- **The host.** `steam.exe` lives with Valve's client files in `/data/playden/steam-client/client`. It loads Valve's
  `steamclient64.dll`, signs in, and writes the registry values a Windows program reads to find a running Steam
  (`HKCU\Software\Valve\Steam\ActiveProcess`). It refuses a client DLL whose SHA-256 is not the one Playden recorded
  when it installed the client (the session file carries it). It calls the client's interfaces by position, as in builds
  1788652215 and 1792618285: a newer build that moved a call shows up as a failed sign-in or a crash of the host, with
  the client's version in the logs and in test reports. It never starts the game. It turns the client's shader
  pre-caching off (`STEAM_ENABLE_SHADER_CACHE_MANAGEMENT=0`): that starts a GPU driver query program, which its process
  can't start.
- **The game** keeps its own `steam_api(64).dll`, which finds the running client as on a PC; a 32-bit game loads
  Valve's 32-bit `steamclient.dll`. Playden writes `steam_appid.txt` beside the library and in each program's folder
  (the working folder the library reads it from; an Unreal game's library is elsewhere) when there is none, so the
  library finds its app and does not ask Steam to start the game again. Every Steam feature, Steam's relays included, comes from Valve's
  code.
- **The game waits for the host.** The title starts the host from Wine's main thread, after the server runs and before
  the game loads, and holds the game until the host writes `C:\playden\steam-host.ready` ("ok", or "failed <reason>")
  or 60 seconds pass. The host writes "ok" once the client is signed in and knows the account owns the game (after a
  sign-in with the token and no saved one, the client fetches the account's licenses for about 2.5 s more; a game
  started before that connects to Steam half-way, and Team Fortress 2 then crashed). It waits at most 10 s for that and
  says so in its log: an account that doesn't own the game, or only borrows it through family sharing, may never get
  that answer. The sign-in itself ends within 45 s of the host's start, inside the title's wait: the client's saved
  sign-in (a kept game only) gets the first 20 s, then the token. If the host can't start or sign in, the game starts
  anyway, without Steam, and the title says so in a notification ("Steam didn't start: <reason>. The game runs without
  it; close the game and try again"). A crash of the host's process would end the game with it, so it leaves cleanly on
  every failure.
- **Debugging a start.** With the game's Logging set to Verbose, Playden puts an empty
  `C:\playden\steam-host.ready.notify` beside the ready file. The title then shows each step of the start as a
  notification: its own (helper started, Wine running) and each line the host adds (client loaded, signing in, signed
  in, game owned, ready).
- **Valve's client files** come from Valve (`client-update.akamai.steamstatic.com`, over HTTPS only): at a Play of an
  Online game Playden reads Valve's client manifest (`steam_client_win64`) and, when its version is newer than the one
  installed (or none is), downloads its `bins_win64` package (about 64 MB), checks it against the manifest's SHA-256,
  unpacks it beside its folder and renames it into place; a failed download keeps the installed client. Valve's crash
  reporter (`crashhandler*.dll`, which uploads minidumps of the process that holds the session, and
  `steamerrorreporter*.exe`) is left out: the client does without it. Playden keeps its own record of the installed
  version and the client DLL's SHA-256 in its private state, and never ships Valve's files. Their folders are 0777: the
  client writes its own state there, and the title runs as another user.
- **The session.** At Play, Playden writes the account's session into the game's private bottle:
  `C:\playden\online-session.txt` (`drive_c/playden`, folder 0700, file 0600, internal storage), with the account name,
  SteamID and refresh token. The host reads it once as it starts and deletes it. A Play that does not start the game, a
  game that is not set to Online, and Playden's next start each remove a file left behind. The token never goes into
  the game's folder (which may be on an exFAT drive, where every file is readable).
- **Finish setup.** A new game's first start is from the PS5 Home screen: the PS5 adds the game only once Playden quits.
  When Finish setup (or Play in the "ready to play" dialog) quits Playden, the game gets the same file as at Play, if
  the settings its title has published are Online (`RuntimeHost::HomeHandoff`). Settings saved after the title was
  published reach it only at Playden's next start, so the game gets nothing then. Playden's next start removes the file
  if the game never took it.
- **Steam sign-in = Kept for the Home screen** writes `keep=1` into the file and the host leaves it, so the game also
  signs in when it starts from the PS5 Home screen. The token then stays in the bottle while that setting is on, and the
  client's own saved sign-in stays: anything that can read the console's internal storage can use the account. With At
  each Play, Playden removes the client's saved sign-in before each start: its account list (`config/config.vdf`,
  `config/loginusers.vdf`) and the token it saves in the bottle (`C:\users\prospero\AppData\Local\Steam\local.vdf`).
  The host signs in from a saved one only for a kept game. The client saves them again while it runs; the token goes
  when the game ends, the account list at the next start.
- The client's token leaves the bottle with Playden's handoff: when the game is no longer Online, at a sign-out, at
  Playden's next start without a stored session, and when the bottle is kept after an uninstall. The account list goes
  at a sign-out, and at Playden's start when no game keeps its sign-in. The token's folder stays 0700 (its file 0600)
  when the rest of the user profile is opened to everyone, and the web file service does not serve it.
- **Joins and invites.** `C:\playden\online-launch.txt` is for one start only: a friend's lobby to join and friends to
  invite, from the Join and Invite sheets. The host reads it and deletes it at every start, and ignores one written more
  than two minutes before (a leftover from a start that failed early). While the game runs, the host:
  - asks Valve's client to pass the lobby to the game, through the client's private
    `IClientFriends::NotifyLobbyJoinRequested`, which by its name and arguments is what makes Steam post
    `GameLobbyJoinRequested_t` to a running game on a PC (not yet seen working): 20 s after "ready", unless the game is
    in that lobby already. The client doesn't tell the host when the game's Steam library is up: it doesn't report the
    account's own game to the host's lookup;
  - invites the picked friends to the first lobby the game is in (`ISteamMatchmaking::InviteUserToLobby`), once. The
    game's own `LobbyCreated_t` and `LobbyEnter_t` go to the game's connection, not the host's. So the host makes its
    connection one for the game's app as well (`IClientUtils::SetAppIDForCurrentPipe`, without tracking its process),
    and the client then sends it `LobbyDataUpdate_t` when the game makes or joins a lobby. A lobby counts only when the
    account is in it (it has members for the host): the game may also look at lobbies it doesn't join;
  - answers a friend's invite to this game (`LobbyInvite_t`) as Game invites says. Ask shows "<name> invited you to
    their game. Press L3 + R3 within 15 seconds to join." and joins when the player does; Only show shows the invite;
    Join at once joins and says "Joining <name>'s game.". The invites sent at Play show as "Invited 2 friends to your
    game.".

  The title shows these while the game runs through files beside the ready file (prospero-win's
  `src/pw_helper_channel.h`): it makes `C:\playden\steam-host.ready.say` (messages, at most 4 in any 10 s) and, once it
  has the pad, `steam-host.ready.answer`; the host asks by writing `steam-host.ready.ask` and reads the answer. A title
  without them (a runtime kit without `title-notify`) gets nothing: Ask and Only show leave the invite, as the Game
  invites row shows (Off, with the reason). Friends' names go to the screen only, never to a log.

  The friends and lobby calls are Valve's public interfaces (`SteamClient0xx`, `SteamFriends017`,
  `SteamMatchMaking009`), whose layout doesn't change, and the host opens them only for a Play that asked for a join or
  invites; the join and the connection's app are private ones, at the same positions in builds 1788652215 and
  1792618285.
- **Online for friends.** Steam sends friends' online states only to a connection that has said it is online
  (`ClientChangeStatus`, 716), and Valve's client doesn't send that on its own: on a PC the Steam window's Friends list
  does, through the client's shared connection (`IClientSharedConnection`). The host sends the same message after the
  sign-in, and again whenever the client connects to Steam anew. Before it did, the game's friends list showed nobody
  online (Portal 2's "No friends currently available") and friends saw the account offline. The shared connection is a
  private interface too (engine slot 0x228, `AllocateSharedConnection` and `SendMessage`), at the same positions in both
  builds.

  The Friends tab's Triangle switches between Appear online and Appear invisible (the preference `steam_invisible`).
  Invisible sends persona state 7 instead of 1, on Playden's own Steam session at once and in the handoff
  (`persona=invisible`) for the host of the next Online game: friends see the account offline. Steam should still
  send friends' states to an invisible connection; that is not confirmed on a console yet. A handoff kept for the Home
  screen keeps the choice it was written with until it is written again (the game's next Play, a settings change or
  the next sign-in).

  Settings › Online play › Steam sign-in notice (the preference `steam_online_notice`, on by default) puts `notice=1`
  in the handoff. Once the game may start, the host writes "Online on Steam as <name>" (or "Signed in to Steam as
  <name>, invisible to friends") to the title's messages file, which the title shows as a notification once the game
  runs. The name is the one Valve's client has for the account (`ISteamFriends::GetPersonaName`, the friends interface
  opened for this alone when Game invites is Off), else the session's account name; it never goes into the host's log.
  A title without the messages file shows nothing.
- **Peer-to-peer through the client.** A game's `ISteamNetworking` peer-to-peer (Portal 2's co-op: it accepts with
  `AcceptP2PSessionWithUser` on its user's interface) runs in Valve's client, not in the game: on a Mac a process that
  uses only that interface opens no UDP socket while it connects. Valve's client needs the console's network adapters
  for it, and iphlpapi lists them through nsiproxy's `\\.\Nsi` device, which a 64-bit game hosts (prospero-win patch
  1003) and a 32-bit game and the second process don't. Without it every session timed out (`P2PSessionConnectFail`,
  error 4) and Portal 2's co-op map stayed black. So the host hosts nsiproxy itself, through ntoskrnl's hosted mode
  (`WINE_NTOSKRNL_HOSTED=nsiproxy` while `ntoskrnl.exe` loads, then `wine_ntoskrnl_host_start`), before Valve's client
  loads. A game set to Network adapters = Off, or with `PW_NSIPROXY_DISABLE=1` in Environment variables, gets
  `adapters=off` in the handoff and the host leaves the adapters unlisted. On the console (2026-11-11, a 32-bit test
  program in Portal 2's bottle against a Mac, Spacewar), sessions timed out before and connected within 4 s after, on
  the local network. `ISteamNetworkingMessages`, which runs in the game itself, already connected through Valve's relays
  before.
- Valve Anti-Cheat: when the Steam store lists a game with "Valve Anti-Cheat enabled" (category 8), Steam features says
  on the Online choice, and on the row while it is chosen, that the game uses Valve Anti-Cheat (VAC) and that playing it
  online could get the account banned from this game (a VAC ban is for that game, not the Steam account). Playden learns
  this from the store details it fetches when the game's page opens.
- What remains: the host holds the account's Steam session while the game runs, in the game's own bottle and sandbox.
  Code there (a mod, for example) can read it. That is why Online is chosen per game and marked experimental.

## Not done

- **Asking and showing invites while the game runs** needs a runtime kit whose title shell reads the host's files
  (`title-notify`); none is released yet.
- **A game acting on a join has not been seen.** On the console (Spacewar, two accounts), a friend's invite reached
  the host, Ask asked the player, and the host's invites at Play reached the friend. The client accepted the join it
  was passed, but Spacewar doesn't listen for `GameLobbyJoinRequested_t`, so no game has been seen joining.
- **The second process's threads can't be interrupted by the server**, which signals threads through a table in the
  title's process. Asynchronous calls queued for them run at their next wait on the server. Before Wine patch 1015 the
  server dropped such a call instead; when the host's client later closed that socket, the title's Wine server asserted
  (`release_object` from `free_async_queue`) and the game ended (Deep Rock Galactic on its way into the Space Rig, Team
  Fortress 2 at its menu within 90 seconds). Online needs a runtime kit built with patch 1015.
- **Peer-to-peer across the internet** (NAT traversal, Valve's relays) has not run on a console since the host lists
  the adapters; only a connection on the local network has.
- **`GetAdaptersAddresses` still fails** with ERROR_NOT_SUPPORTED wherever nsiproxy is hosted: Wine patch 1004 makes
  `DnsQueryConfig` fail on the console, and iphlpapi returns that error instead of leaving the DNS servers out. Valve's
  client connects anyway; the in-game ICE of `ISteamNetworkingSockets` can't list addresses and uses the relays.
- **Valve's codecs package** (`bins_codecs_win64`, used for streaming) is not downloaded.
- **Kernel anti-cheat** games stay unsupported. **CEG** stays out of scope.
- **Epic Online Services P2P** joins need the console's network adapter in `GetAdaptersAddresses` (Wine's `nsiproxy`
  hosted in the game).

## The logs

- `C:\playden\steam-host.log` in the game's bottle: whether the session was handed over and the file deleted (or kept),
  the client's check, the sign-in, what a Play asked for, and while the game runs: each callback number the first time it
  reaches the host (`callback: first id=`, with its time), the first lobby, the
  invites sent, friends' invites and the joins passed to the game. Never a token, a Steam ID or a lobby ID.
- The title's log (`PW_WINE64 helper …` lines): the local process, the grant, the server connection and how long the
  game waited.
- `helper-wine.log` in the title's library folder: Wine's messages from the host's process.
- At its next start, Playden copies both into the game's title log in its logs folder
  (`/data/playden/logs/title-<title ID>.log`), after the session report, because the bottle can't be read over FTP:
  the end of the host's log, and the start and end of the Wine log (the host's libraries load at its start).
- A file named `C:\playden\steam-host.no-sign-in` in the bottle is a test switch: the host loads Valve's client and
  lets the game start, but nobody signs in.
- Playden's `runtime.log`: `steamworks app=<n> mode=steam-host` and the client download (`steam client app=<n> …`).

## Earlier: the online bridge

Before the Steam host, nightlies put Playden's online bridge in the game's DLL slot for Online: a `steam_api` of
Playden's own that signed in to Steam itself and forwarded the rest to gbe_fork. It is gone. A game that still holds a
bridge DLL (it carries the text `playden-online-bridge`) gets its own DLL back from its `.orig` at its next Play, and the
emulator copy the bridge kept beside it is removed.
