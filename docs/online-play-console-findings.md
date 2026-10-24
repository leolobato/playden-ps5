# Online play on a PS5: console findings (October 2026)

What was tried on a real console to get Steam online features working through the online bridge
(`tools/steam-online`, Game settings › Steam features › Online), what was fixed, and why online services still fail
in the two games tested. Read this before continuing so you do not repeat the same investigation.

Console: PS5 firmware 13.60, kit `hl2-fw1360-dev1` (prospero-win `f729534`), zftpd on port 2120. All console work
went over FTP with the tools in `tools/`.

## Summary

| Game | Engine | Steam sign-in through the bridge | Game's online services | Blocking cause (best evidence) |
| --- | --- | --- | --- | --- |
| RuneScape: Dragonwilds (1374490) | Unreal 5, Redpoint EOS | Works | Stuck on "Attempting to log in" | The game's Redpoint EOS login stops at its Steam credentials step without ever asking Steam for a ticket |
| Yu-Gi-Oh! Master Duel (1449850) | Unity 6 | Works | "There was no response from the game server" | Not Playden: the same error on a Linux PC with real Steam and Proton, same account (traced on the PS5: the server answers, the game treats it as a failure) |

In both games the bridge connects to a Steam CM, logs on (`eresult=1`), shows the account online and playing,
reads the friends list and gets an app ownership ticket. Neither game ever requested a session, Web API or
encrypted app ticket in any run.

## What changed in this branch

1. **Moved dependencies** (commit "Fetch ps5-opengl and the boilerplate from their new homes").
   `blackbearreloaded/ps5-opengl` and `blackbearreloaded/ps5-native-app-boilerplate` are gone (404). The SDK
   archive now downloads from `MyLuxy/overdrive-deps` and the submodule clones from
   `efkann/ps5-native-app-boilerplate`. The archive SHA-256 and the submodule commit are unchanged and were
   verified. CLAUDE.md records this exception.
2. **Null method crash** (commit "Stop online games crashing at Steam sign-in"). `WrapInterface` copied only the
   methods the bridge answers (9 of SteamFriends' ~70, 13 of SteamUtils') into its replacement vtable and left
   the rest null. Dragonwilds called a later slot and crashed with `EXCEPTION_ACCESS_VIOLATION 0x0`. The bridge
   now copies `kVtableEntries` (128) slots of every table.
3. **Flat-API ticket calls**, same commit. `SteamAPI_ISteamUser_GetAuthSessionTicket`, `GetAuthTicketForWebApi`,
   `BeginAuthSession`, `EndAuthSession`, `CancelAuthTicket`, `RequestEncryptedAppTicket` and
   `GetEncryptedAppTicket` are now overridden (listed in `gen.py` `OVERRIDDEN`), because gbe_fork's flat exports
   do not dispatch through the replaced vtable. Each logs its first use.
4. **Crash trail**, same commit. A vectored exception handler writes `CRASH` lines (exception, module+offset,
   access address, modules on the stack) to `playden_online.log`. This is what found the null method crash.
5. **ISteamClient getters for Unity games** (commit "Bridge Steam interfaces that Unity games get from
   ISteamClient"). Steamworks.NET gets every interface through `SteamAPI_ISteamClient_GetISteamUser`,
   `GetISteamFriends`, `GetISteamUtils`, `GetISteamMatchmaking` and `GetISteamGenericInterface`. These went
   straight to gbe_fork, so the bridge saw no interface in Master Duel. They now go through `WrapInterface`; the
   log shows `bridged SteamUser021`, `SteamFriends017`, `SteamUtils010`, `SteamMatchMaking009`.

Every bridge change rebuilds both DLLs (`tools/steam-online/build.sh`, needs `PATH=$(brew --prefix
llvm@18)/bin:$PATH`), and updates `kSteamOnlineBridge64Sha256`/`kSteamOnlineBridge32Sha256` in
`src/runtime/steamworks.h` and the table in `runtime/steamworks/README.md`. The build is reproducible (checked).
Host tests: 148/148 pass (`http_client` once timed out under full parallel load and passes alone).

## Tested on the console

- Package built with `scripts/build-lapy-helper.sh` and `scripts/build-homebrew-ps5.sh`, deployed with
  `tools/console_title_deploy.py` (hash-checked read-back) after each change.
- Dragonwilds in Offline emulation: the game's login fails at once with "No auth service is available on this
  platform" (Redpoint gives the STEAM service score 0).
- Dragonwilds in Online, before the fixes: crash at start-up (null slot). After the fixes: no crash, Redpoint
  picks STEAM (score 1000), then waits forever at "Attempting to log in"; the game's own watchdog eventually shows
  "Application Hang Detected".
- Master Duel in Online, before and after the ISteamClient fix: "There was no response from the game server".

## Dragonwilds: why it still fails

The game's log (`RSDragonwilds.log`, included in `title-FAKE78001.log`) ends the login at:

```
Authentication graph entering node: FGetExternalCredentialsFromLocalPlatformNode
```

and nothing follows for minutes. Temporary diagnostics (since removed, see below) established:

- **Not a deadlock or spin in the bridge.** Thread samples at 40 s and 70 s showed render threads running, task
  threads waiting on events, the bridge's own threads idle in WinHTTP. The ~1 core of CPU use is rendering the
  spinner.
- **Steam callbacks are pumped.** `SteamAPI_RunCallbacks` is called every frame (~3400 times in 70 s). 22 callbacks
  are registered, including 168 (`GetTicketForWebApiResponse_t`).
- **No ticket is ever requested.** Per-export and per-vtable-slot call counts over every interface the game opened
  (User023, Friends017, Utils010, MatchMaking009, Apps008, UserStats012, RemoteStorage016, MatchMakingServers002,
  Networking006, NetworkingSockets012, NetworkingUtils004) showed, during the hang, only `ISteamUser::GetSteamID`
  (slot 2) and `ISteamFriends::GetPersonaName` (slot 0) being polled. `BLoggedOn` (slot 1) was called twice early.
  Slots 13 (GetAuthSessionTicket), 14 (GetAuthTicketForWebApi) and 21 (RequestEncryptedAppTicket) were never
  called, nor their flat-API forms.
- Only one `steam_api64.dll` exists in the game (`Engine/Binaries/ThirdParty/Steamworks/Steamv157/Win64/`), and it
  is the bridge, so no call bypasses it through another copy.

Conclusion: some check inside the game's `OnlineSubsystemRedpointSteam` fails silently before the ticket request,
and the authentication graph (`Until_Forever`) never times out. Finding it needs disassembly of
`RSDragonwilds-Win64-Shipping.exe` around Redpoint's Steam external-credentials code. Not done.

Launching through `RSDragonwilds.exe` is not an option: it only starts the Shipping exe, and process creation is
not supported on the PS5 (`wrapper-start-failed`). The Shipping exe initialises Steam itself ("Steam User is
subscribed 1"), so starting it directly is not the cause.

Even if the login is fixed, the game opens `SteamNetworkingSockets012`, which the bridge does not bridge, so co-op
through Steam relays will likely still fail (see `online-play.md` › Not done).

## Master Duel: why it still fails

The error appears before the game calls any Steam ticket function (the bridge logged none, even with every
interface bridged). **Confirmed not Steam:** with Steam features = Offline emulation (no bridge at all) the game
shows the same "There was no response from the game server".

**The network itself works (traced).** With Game settings › Logging = Network (added in this branch), the last
17 seconds of a session with several RETRY presses show one game thread (Unity's own HTTP client: it calls
`WinHttpGetIEProxyConfigForCurrentUser` before each request and does its own TLS, not Wine's schannel) keeping one
socket open, sending a request about every 0.5 s (14 sends) and reading the replies (28 successful reads). No
connection error, no failed name lookup; the `0xc00000a3` reads are the normal would-block of a non-blocking
socket. So the server answers and the game treats the answer as a failure: an **application-level** refusal, not
a Wine networking, DNS or TLS fault. The payload is encrypted by the game itself, so the traces cannot show what
the server says. Kept here for the record, the hypotheses below are now ruled out as the main cause.

What the logs do and do not show:

- `err:secur32:start_samss` and `err:secur32:load_auth_packages ... 80090304` appear, but they are start-up noise:
  the same lines appear in Dragonwilds, whose HTTPS requests to Epic succeed. Do not treat them as the cause.
- The kit's schannel is backed by GnuTLS, and prospero-win patch `0877-secur32-log-gnutls-errors-and-the-clock-on-
  handshake-failure` logs every failed handshake with the GnuTLS error and the clock. **No such line appears** in
  Master Duel's log, so a TLS handshake failure is not shown (the log does leave out the middle of each session, so
  it is not fully ruled out either). Patch `0878` supplies root certificates beside the runtime.
- `err:dnsapi:DllMain No libresolv support, expect problems` appears: name lookup is a candidate.
- Master Duel is reported to work under Proton on Linux and the Steam Deck, so the cause is likely specific to Wine
  on the PS5 (name lookup, sockets, TLS or certificates), not Wine in general.

**Same error on a Linux PC with real Steam (decisive).** On an Ubuntu desktop (RTX GPU, Steam snap, Proton
Experimental, the same Steam account, no emulator, no bridge, no PS5) Master Duel shows the same "There was no
response from the game server" on the same day. So the PS5 failure is not caused by Playden, the bridge, gbe_fork
or the kit. The cause is between this account/network and Konami's servers (an outage, an account or region
restriction, or Konami refusing Proton at the time). **Do not spend more time on Master Duel in Playden** until it
connects on a PC with the same account. Its `Player.log` on the PC stops at the same start-up lines as on the PS5.

No kit patch is indicated. Next ideas, none tried:

- The first requests of a session are lost: the kit's session log keeps only about the last 1 MB (it rotates into
  `session-N.previous.log`), and Network logging fills that in ~17 s. To see the first request, trace fewer
  channels (`+winsock` only) or read the logs sooner.
- What the server dislikes is unknown. Candidates: the game identifying the platform or hardware (Konami's
  anti-tamper or a device check), or a Steam identity check done server-side with data the emulator supplies
  (the game never asked the bridge for a ticket). Comparing with a Proton run on a PC (where the game works) with the
  same Steam features mode would show whether the emulator alone breaks it.

How the full session logs were read: they are in `/data/playden/games/steam-<app>/library/logs/session-N.log` and
`session-N.previous.log`, inside 0700 folders. Over FTP, `SITE CHMOD 711` on `steam-<app>`, `library` and `logs`
(711 lets FTP pass through but not list), `SITE CHMOD 644` on the file, `RETR` it, then put every mode back (600 on
the file, 700 on the folders) before the next launch: Playden checks those modes.

## Practical notes for the next attempt

- **Bridge log**: `steam_settings/playden_online.log` beside the game's `steam_api(64).dll` (Dragonwilds:
  `/data/playden-games/1374490/Engine/Binaries/ThirdParty/Steamworks/Steamv157/Win64/`, Master Duel on the
  external drive: `/mnt/ext1/playden/games/1449850/masterduel_Data/Plugins/x86_64/`). It is 0600 and owned by the
  title; FTP can read it after `SITE CHMOD 644 <path>` (it holds no tokens; `playden_online.txt` does, leave it).
- **Game logs** arrive in `/data/playden/logs/title-<title id>.log` only after Playden starts again. Close the game,
  open Playden, then collect. The file keeps the head and tail of each session and leaves out the middle, so the
  Wine-level end of a crashed session is often missing; the game's own log is appended at the end.
- **The game's private folders** (`/data/playden/games/steam-*`, `Saved/`, game settings) are 0700 and not
  readable over FTP. Game settings can only be changed on the console.
- **Deploy permissions**: after Playden has run, the package's subfolders reject FTP writes. Deploys that change
  `runtime/steamworks/` need `SITE CHMOD 777` on that folder first and `755` after. A timed-out deploy leaves
  `.<file>.stage`; the tool refuses to continue until it is removed (check it is the tool's own partial upload).
- **zftpd** can stop while a game is hung; restart it on the console (delete a stale `/data/zftpd/instance.pid`).
- **`game connect tokens=0`**: in every run Steam sent no game connect tokens, so `MakeTicket` would wait 15 s and
  log "ticket: Steam not ready" if a game did ask for a session ticket. Untested because neither game asked. This
  will need attention for the first game that does (check what makes Steam issue tokens after
  `ClientGamesPlayed`).
- **Diagnostics that were built and removed** (rebuild them from this description if needed, not from scratch
  guesses): a sampler thread logging each thread's RIP/stack modules (Toolhelp32 snapshot, Suspend/
  GetThreadContext, ReadProcessMemory), per-export call counters (`lock incl` in the `gen.py` stubs), per-slot
  vtable thunks counting every interface method (64-bit, generated in `gen.py`), and a list of registered
  callback IDs. They answered the Dragonwilds questions above.

## Suggested next steps

1. Try a Steam game that authenticates with Steam tickets directly (no EOS, no own TLS-dependent backend) to
   confirm the ticket path end to end, including the connect-token issue above.
2. For Master Duel: nothing to do in Playden; it fails the same way on a PC with real Steam. Retest only once it connects on a PC with the same account.
3. For Dragonwilds: reverse-engineer Redpoint's Steam credentials node only if it is worth it, given that Steam
   networking is also unbridged.
