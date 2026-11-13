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

Installs come later. A GOG game's page shows Install, but you can't select it.

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

## Going back to an older build

Before you go back to an older build of Playden with GOG games in your data, read this:

- A build older than the first release that reads GOG records (the release with Phase 1 of the GOG plan) does not
  read the Library organization at all while a GOG game is in it: favorites, hidden games, collections, ratings and
  notes are lost on that build for every store, Steam and imported games included.
- From the build that installs GOG games, going back to a build older than that release also breaks Play for every
  game until the GOG titles are removed. The older build refuses the whole install records file when one record names
  GOG.

## For maintainers

- The wiring is in `src/homebrew/gog_wiring.*`. `host.cpp` and `ps5.cpp` make it only with the line on. When "Stay
  signed in" is off, it deletes a GOG session left from an earlier run even with the line off.
- The client is `src/gog/`, the store is `src/stores/gog*`, and the phone relay is `src/web/gog_relay.*` with the page
  in `src/web/page/`.
