# steam.exe (Playden's own)

Playden's Steam host, built from `tools/steam-host` (no C runtime; imports only kernel32 and advapi32; reproducible with
`tools/steam-host/build.sh`, 64-bit). It runs beside a game set to Steam features = **Online**, in a second process on the game's
Wine server, from the folder that holds Valve's Steam client files. It loads Valve's unmodified `steamclient64.dll`, signs in
with the session Playden hands over at Play, and writes the registry values that tell the game's own Steam library where Steam
is. While the game runs it asks Valve's client to pass lobby joins to the game (Playden's Join, a friend's invite) and
invites the friends picked before Play. It also tells Steam the account is online, as the Steam window's Friends list does
on a PC, so the game sees which friends are online. Before the client loads it hosts Wine's nsiproxy driver through the
runtime kit's hosted mode, so the client can list the console's network adapters for peer-to-peer (not for a game set to
Network adapters = Off). See `docs/online-play.md`.

Playden installs Valve's current client (manifest `steam_client_win64`; its version is in runtime.log, the host's log, the
client folder's marker and test reports of Online games) and records the SHA-256 of its `steamclient64.dll` in its own
private state. The host uses the DLL only when it has that hash, which the session file carries: the client folder is
open to everyone. The host calls the client's interfaces by position, as in builds 1788652215 and 1792618285; a build
that moved a call shows up as a failed sign-in or a crash of the host. Playden downloads Valve's files from Valve, checks
the package against the manifest's SHA-256, leaves out Valve's crash reporter (`crashhandler*.dll`,
`steamerrorreporter*.exe`) and never ships any of them.

| File | SHA-256 |
| --- | --- |
| `steam.exe` | `54ba650685764d24baf6035d35e1928e85e417420775447d659f6710d40426f0` |
