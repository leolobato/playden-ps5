# GOG store (experimental)

GOG is being added as a second store, next to Steam. Until it is finished, it is an experimental feature for testers
and maintainers. This page says what it does now, how to turn it on, and what to know before you test it.

## Turning it on

Add a line `gog-store` to `features.txt` in Playden's data folder (`/data/playden/features.txt` on the console,
writable over FTP; the data folder given with `--data-dir` on the Mac). Playden reads the file once at startup, so
restart Playden after you change it. Dev builds and nightlies also need the line: GOG is off in every build without it.

## What the line turns on now

- **Sign-in.** Settings › Stores lists Steam and GOG. The startup screen offers "Sign in to GOG" after Steam's
  sign-in, so a player can start with GOG only.
- **The library.** The GOG games you own load into the Library with their art, next to your Steam games, in All and in
  a GOG section of the rail. Playden lists Windows games only. The library is saved, so it shows while Playden is
  offline, and while you sign in again after GOG ends your sign-in. If Playden started offline, Refresh library asks
  GOG again and signs you in when GOG answers.
- **The Stores section** in Settings, with Sign in, Refresh library and Sign out for GOG.
- **Installs.** Install on a GOG game's page puts it in the Queue, as for a Steam game. You can pause and resume it,
  and a download that stops when Playden closes continues at the next start. You can install to a USB drive. The page
  shows the install size before you start. Games whose Windows build uses GOG's older download format (gen 1) install
  too.
- **Play.** An installed GOG game becomes its own PS5 title, named "<name> (GOG)" when "Store name in PS5 titles" is
  on. It starts the program that the game's primary play task names. Game settings, recipes (`gog/<product id>`),
  uninstall and Move to… work as for a Steam game.
- **DOSBox and ScummVM games.** Playden installs the DOSBox or ScummVM files that the game needs into its game
  folder. It also runs the install script's `supportData` and `setIni` steps: they copy the game's support files into
  the game folder and set INI keys, such as ScummVM's game path. `setIni` runs again before each Play, so a moved game
  keeps the right paths.
- **Registry values.** The install script's `setRegistry` steps (the DOSBox folder that DOSBox games look up, for
  example) write their values into the game's bottle before each Play, so a moved game keeps the right paths here too.
  Values under `HKEY_LOCAL_MACHINE\Software` go to both registry views, as for a Steam game's install script. A
  recipe's registry value for the same key and name wins. Steps that only make a key, other value types and other
  roots are skipped.
- **Redistributable installers.** Playden downloads a game's `__redist` packages (DirectX, Visual C++, .NET, XNA)
  with the game, into `.playden-gog/redist/` in its game folder, and counts them in the install size. Before the game
  first starts, Playden installs the Windows Installer packages (XNA, for example) and the .NET desktop runtimes into
  the game's bottle, once. It skips the ones Wine provides (Visual C++, DirectX, .NET Framework). Playden can't run
  any other installer (OpenAL's setup program, for example): the log names it with `result=not-run`. "Run launcher
  or setup" does not list the files in `.playden-gog`, and web access's Game files tab hides that folder. To run such
  an installer, upload your own copy of it into the game folder with the Game files tab, outside folders such as
  `redist` or `__installer`, then pick it in "Run launcher or setup".

Not there yet:

- **Game updates.** Playden does not look for new GOG builds of an installed game.
- **Choosing DLC.** Every DLC you own for a game installs with it. You can't add or remove DLC.

With the line off, Playden does not change: there is no Stores section, no GOG sign-in, no GOG routes in web access,
and no `gog` folder in user data.

## Signing in with your phone

GOG's login page needs a browser, so you sign in on your phone, not on the TV:

1. Turn on web access (Settings › Web access). The phone must be on the same network as the console. If web access is
   off, the sign-in screen offers to turn it on.
2. On the TV, select Sign in to GOG. Playden shows a QR code.
3. Scan the QR code with your phone. It opens Playden's GOG page on the console.
4. Select Open GOG sign-in and sign in to GOG. GOG may ask you to show that you are a person. Answer it on the phone.
5. GOG then opens a page whose address starts with `https://embed.gog.com/on_login_success`. Copy the full address
   from the address bar, paste it into Playden's page and select Send.

The TV shows that you are signed in, and the GOG library loads. The QR code is valid for ten minutes and for one
sign-in. If GOG refuses the address, sign in again on the phone and paste the new address into the same page.

"Stay signed in" is one setting for Steam and GOG. When it is off, the GOG sign-in stays in memory only, and Playden
deletes the saved GOG session at the next start. On the console, the saved session is
`/data/playden/user/gog/session`. As with the Steam token, it is not encrypted.

## What Playden logs

GOG's log lines are in `gog.log`; the phone page's posts are in `web.log`. Playden never logs your GOG name or user ID,
a token, the sign-in code, the address you paste, or the QR code's token. A log line gives the step and the result
only, for example `library owned=27 fetched=27 not_modified=0 missing=0 hidden=4 failed=0`. Web access's log bundle
also removes the signed-in GOG name and user ID if they appear in a log.

GOG install lines are in `install.log`, next to Steam's. Each step gives the game's key (`gog-<product id>`) and the
result, for example `gog install game=gog-1351891846 step=plan result=ok language=english`. The download's own lines
(`gog plan …`, `gog chunk …`, `gog range …`) give counts, HTTP statuses and the CDN's name only (`fastly`, `gcore` or
`other`). No line gives your account, a token or a download address.

## Going back to an older build

Before you go back to an older build of Playden with GOG games in your data, read this:

- A build older than the first release that reads GOG records (the release with Phase 1 of the GOG plan) does not
  read the Library organization at all while a GOG game is in it: favorites, hidden games, collections, ratings and
  notes are lost on that build for every store, Steam and imported games included.
- After you install a GOG game, the same older builds (those that can't read GOG records) also break Play for every
  game until the GOG titles are removed: they refuse the whole install records file when one record names GOG. A build
  that reads GOG records but doesn't install GOG games keeps Play working.

## For maintainers

- The wiring is in `src/homebrew/gog_wiring.*`. `host.cpp` and `ps5.cpp` make it only with the line on. When "Stay
  signed in" is off, it deletes a GOG session left from an earlier run even with the line off.
- The client is `src/gog/`, the store is `src/stores/gog*`, and the phone relay is `src/web/gog_relay.*` with the page
  in `src/web/page/`.
