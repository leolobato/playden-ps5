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

## Session of 2026-10-25: Dragonwilds reproduced on a Linux PC

Setup: Ubuntu desktop, RTX GPU, Steam snap, Proton Experimental. The game's
`Engine/Binaries/ThirdParty/Steamworks/Steamv157/Win64/` holds the bridge as `steam_api64.dll`, gbe_fork as
`steam_api64_gbe.dll`, Playden's `steam_settings` copied from the PS5, and `playden_online.txt` (the user's
refresh token, 0600, copied by the user; delete it when testing ends). Steam's own DLL is saved beside it as
`steam_api64.dll.steam-orig`; put it back when done. Game log:
`compatdata/1374490/pfx/drive_c/users/steamuser/AppData/Local/RSDragonwilds/Saved/Logs/RSDragonwilds.log`.

| Run | Credentials step (`FGetExternalCredentialsFromLocalPlatformNode`) |
| --- | --- |
| Real Steam | Web API ticket in 2 s, EOS login succeeds |
| gbe_fork only (Playden's Offline emulation) | Fake ticket in 0.3 s, EOS rejects it (`EOS_Auth_ExternalAuthInvalid`), no hang |
| Bridge online, all four interfaces wrapped | **Hangs, as on the PS5**; no ticket call reaches the bridge |
| Bridge online, callback-168 vtable swap off | Still hangs |
| Bridge online, no interface wrapped | No hang; emulator's fake ticket, EOS rejects |
| Bridge online, MSVC RTTI slot copied into the tables | Still hangs (not the cause; the fix is kept in the working tree) |
| Bridge online, **only SteamUser wrapped** | **No hang**: `GetAuthTicketForWebApi` reaches the bridge, callback 168 runs |

Conclusions:

1. **The hang comes from wrapping SteamUtils, SteamFriends or SteamMatchMaking** (not yet narrowed to one).
   Next run: wrap User + one other at a time. Suspects: the Utils hooks for `IsAPICallCompleted`/
   `GetAPICallResult`/`GetAPICallFailureReason` (slots 10-12) and the Friends/MatchMaking method tables.
2. **The bridge cannot make a real ticket**: Steam sends `game connect tokens=0` in every run, so `MakeTicket`
   waits 15 s and gives up (`made=0`) while the emulator's ticket has already reached the game via callback 168.
   Needed: find what makes Steam issue game connect tokens (probably what the real client sends after logon), and
   deliver the patched callback only once the real ticket exists.
3. On the PC the bridge is repeatedly `logged off eresult=6` (LoggedInElsewhere) because the real Steam client on
   that PC uses the same account. Not a PS5 issue.

Uncommitted debug state in `tools/steam-online/online.c` (and the DLLs in `runtime/steamworks/` built from it):
`DEBUG` log lines in the ticket hooks and the callback-168 hooks, the RTTI locator fix (keep), and a
`if (kind != kUser) return object;  /* DEBUG: User only */` line in `WrapInterface` (remove). Do not commit as is;
rebuild the DLLs and hashes from clean code before committing.

## Resolved: the "Attempting to log in" hang (2026-10-25)

Cause: the bridge hooked ISteamUtils' API call methods at slots 10-12, but games built with the Steamworks SDK keep
`GetCSERIPPort` at slot 7, so `IsAPICallCompleted`/`GetAPICallFailureReason`/`GetAPICallResult` are at 11-13.
Dragonwilds' calls proved it (slot 9 `GetAppID` once, slot 16 `SetWarningMessageHook` once, slot 11 with
SteamAPICall_t handles). The game's `IsAPICallCompleted(call, &failed)` landed in the bridge's
`GetAPICallFailureReason`, which passed only the call to the emulator's slot 11, and the emulator wrote its result
through a garbage `failed` pointer: depending on that register, a crash (`steam_api64_gbe.dll+0x14c826 writing
0x7ffffff8`) or an endless wait. That is why single-run bisections earlier gave contradictory answers.
Caution: gbe_fork's own `sdk/steam/isteamutils010.h` lists 010 without `GetCSERIPPort`; trust the game's observed
calls over that header. Fixed in "Stop online games hanging at their Steam sign-in".

On the PS5 Dragonwilds now asks the bridge for its Web API ticket and Redpoint sends a credential to EOS, which
rejects it (`EOS_Auth_ExternalAuthInvalid`) because the bridge falls back to the emulator's ticket:
`ticket: Steam not ready (... connect token missing)`. **The remaining blocker is game connect tokens**: Steam sends
`ClientGameConnectTokens` with no tokens in every run. Next: find what makes Steam issue them to the bridge's
logon (compare with what the real client sends at logon and after `ClientGamesPlayed`).

Earlier experiments in this file that blamed "wrapping SteamUtils/Friends/MatchMaking" or the Steam launch
environment variables were reading this bug; the environment variables are not needed.

## Game connect tokens fixed; Epic refuses the Web API ticket (2026-10-25)

- Steam does send game connect tokens: 10 right after logon and more later. Each is **20 bytes**, the session
  ticket's whole first section (token, SteamID, issue time; bytes 8-15 are the account's SteamID, checked). The
  bridge only accepted 8-byte tokens and dropped them all, hence `game connect tokens=0`. Fixed in "Keep Steam's game
  connect tokens for online tickets": tokens are kept and copied into tickets unchanged.
  Playden's C++ reference (`src/steam/backend/game_tickets.cpp`, `kGameConnectTokenBytes = 8`) has the same
  assumption and the same bug; its tests encode 8-byte tokens. Fix it the same way when that code is next used.
- `client_os_type` (0, "unknown", in the bridge's logon) does not stop Steam sending tokens.
- On the PS5, Dragonwilds now gets real tickets: `encrypted app ticket eresult=1`, `auth list ack sequence=N`,
  `web api ticket handle=N`. Epic still refuses the credential: OAuth `TokenGrantv2` 400
  `errors.com.epicgames.account.ext_auth.invalid_external_auth_token` and EOSAuth `TokenGrant` 401
  `errors.com.epicgames.eos.auth.external.invalid_token`.
- Leading hypothesis: Epic validates a **Web API** ticket with Steam's `ISteamUserAuth/AuthenticateUserTicket`
  quoting the identity the game asked for (`epiconlineservices`). The bridge registers the ticket in
  `ClientAuthList` as a plain session ticket: it never sets `CMsgAuthTicket.ticket_type` (field 9) and passes no
  identity. How the real client registers a Web API ticket and its identity is not in the repo's protos; it needs
  research (SteamKit/SteamDB protobuf dumps of newer `steammessages_*.proto`, or the real client's behaviour).
- Token hygiene: never use the PS5's `playden_online.txt` on another machine. On 2026-10-25 the token was used from
  the Linux PC with logon fields Steam rejected (`eresult=15`) and afterwards Steam refused it everywhere
  (`eresult=5`) until Playden had a new one.

## Working: Dragonwilds signs in on the PS5 (2026-10-25)

After the connect-token fix, three more differences from the real ticket format (found in SteamKit's
`SteamAuthTicket.cs` and node-steam-user's `appauth.js`) stood between the bridge and a valid ticket:

1. `ClientAuthList` must list the game's own ticket by its token section and session header only (52 bytes) and their
   CRC, not the whole ticket.
2. A Web API ticket has type 5 (not 2) in its session header, and its auth-list entry carries the identity as
   `server_secret` = `str:epiconlineservices` plus a NUL. After this fix Epic's error became specific:
   `Steam Session Ticket rejected. Expected Steam App ID(s) [1374490, 1714720], got [-826450924, 4]`.
3. The ownership ticket needs its own 4-byte length prefix; without it everything after was read 4 bytes off (the
   `4` was the ownership ticket's version).

With all three, the game asks for one Web API ticket, Steam validates it (`ticket auth complete estate=2
response=0`), and the game lists and creates lobbies. Untested: joining another player, co-op traffic (Steam
networking is not bridged), Master Duel's own server problem (not Steam).

Still to do: Playden's C++ reference (`src/steam/backend/game_tickets.cpp`) has the 8-byte token, missing ownership
length and type-2-only assumptions; bring it in line with the bridge. The log line for Steam's validation of the
game's own ticket says "(another player's)"; cosmetic.

## Joining a friend's world (2026-10-25)

Symptoms: joining a friend's Friends world hangs at "Joining game"; a friend joining ours saw "You are not able to
join this world with this character type".

1. **Steam lobbies failed** (`FOnlineAsyncTaskSteamCreateLobby ... k_EResultFail`, Redpoint "Unable to advertise
   synthetic session"). The bridge sent ClientMMS* messages without `routing_appid` (CMsgProtoBufHeader field 3) and
   created lobbies with cell 0 and no public IP. Fixed in "Route Steam lobby messages to the game": lobbies are
   created (`lobby created lobby=1097752422247... value=1`) and a friend's game joins the lobby. The "character
   type" message has not been retested since.
2. **The join itself uses EOS P2P** (`RedpointEOSNetDriver`, ports 7777-7876, TURN relay), not Steam networking.
   Signaling through Epic works (offer, answer, candidates), then `Failed to establish connection with peer after 45
   seconds`. Network trace: the `EOSP2PNetwork` thread calls `GetAdaptersAddresses` 22 times and each time
   `CreateFileW "\\.\Nsi"` fails with `c0000034`: Wine's nsiproxy device does not exist on the console, so the game
   sees **no network adapters**, opens no UDP socket and sends no P2P traffic.
   Fix needed in the kit (prospero-win Wine patch): host `nsiproxy.sys` in the game as patch 0952 does for the HID
   drivers, or have `iphlpapi`/`nsi` report the console's adapter (address from libSceNet) when `\\.\Nsi` is missing.
   Builds only on Linux x86-64.

How to trace: Game settings › Logging › Network (now `err+all,+winsock,+dnsapi,+nsi,+iphlpapi,+winhttp`; the TLS and
certificate channels filled the kept part of the session log within seconds). Exit the game soon after the failure:
the kit keeps only about the last 2 MB of a session log. Full logs: see "How the full session logs were read" above.

## Working: joining a friend's world (2026-10-25)

Fixed in the kit, not in Playden: prospero-win patch `0990-iphlpapi-ps5-report-the-routed-address-without-nsi`.
When the NSI interface table cannot be read (no `\\.\Nsi` on the console), `GetAdaptersAddresses` reports one up
Ethernet adapter with the routed IPv4 address (a UDP socket connected to 8.8.8.8, which sends nothing, then
`getsockname`). With it, EOS P2P gathers an address and joining a friend's Friends world works on the PS5.

How the kit was made (no full kit rebuild; only a PE module changed):

1. Host Wine at the pinned revision (`490f6d5`) with the series, built by `tools/build_host_wine.sh` inside an
   Ubuntu 24.04 Docker container on the Linux server (its apt was broken; the container needs build-essential, git,
   bison, flex, pkg-config, gcc/g++-mingw-w64 and the X11/FreeType/fontconfig/GnuTLS dev packages).
2. Patch 0990 applied to the built tree, `make install`, then `strip -s` in place (as the script does). The DLLs
   come out with 4 KiB file alignment where kit 0.13's had 512 bytes; same sections, both valid PE.
3. Kit 0.13 with the two `lib/wine/{x86_64,i386}-windows/iphlpapi.dll` replaced (the template's copies are Wine's
   placeholders and stay), their hashes updated in `runtime-inventory.json`, the manifest entries and
   `files_sha256` recomputed (canonical `path\tsize\tsha256\tmode\n` lines), JSON written with indent 2, sorted keys.
4. Pinned: `src/runtime/kit_trust.h` and `dependencies.lock.json` (manifest `bc85f197...`, files `f3632f2f...`),
   and `prospero_win.revision` moved to the submodule commit with the patch.

The kit, the DLLs and the container scripts are kept locally in `build/dev-kits/playden-kit-0.13-iphlpapi-nsi/`
(ignored by git). This is a development kit: it is not published, and the prospero-win commit exists only in the
local submodule until it is pushed and released.
