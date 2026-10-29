#!/usr/bin/env python3
"""Collect Playden's logs from a PS5 over FTP into a zip for a test report.

Reads only /data/playden/logs (or the app's logs folder on 0.4.x and older) and the active kit's manifest. It never reads
playden-data, which holds the Steam sign-in. The Steam account name and SteamIDs are
replaced in every file unless --no-redact is given.

  python3 tools/collect_logs.py --host 192.168.1.50

It lists the games that have a log, the most recently run first, and asks which one to
collect (the newest when there is no terminal to ask on). --app, --title or --all-titles
pick without asking; --list only prints the games.

With --http it asks Playden's web access (Settings > Web access, port 7031) instead of FTP:
Playden builds the same zip itself, always redacted. Playden must be open on the PS5.

  python3 tools/collect_logs.py --host 192.168.1.50 --http
"""
import argparse
from datetime import datetime, timezone
import ftplib
import io
import json
import os
from pathlib import Path
import re
import sys
import urllib.error
import urllib.parse
import urllib.request
import zipfile

LOGS = "/data/playden/logs"
OLD_LOGS = "/data/homebrew/FAKE77031/logs"  # before 0.6.0
IDENTITY = "/data/homebrew/FAKE77031/playden-package.identity"
KITS = "/data/playden/kits"
KIT_ID = "playden-kit"
TITLE_LOG = re.compile(r"^title-(FAKE\d{5})\.log$")
PROFILE = re.compile(r"profile=playden-steam-(\d+)")
REPORT_TIME = re.compile(r"^PW_REPORT/1 .*?\btime=(\d+)", re.MULTILINE)
ACCOUNT = re.compile(r'L"Steam(?:App)?User" L"([^"]+)"')
STEAM_ID = re.compile(r"\b7656119\d{10}\b")
FIRMWARE = re.compile(r"console_firmware=(\S+)")
SKIP_SUFFIXES = (".ppm",)


def listing(ftp, directory):
    """Name -> (size, mtime text) for the plain files in directory."""
    lines = []
    ftp.retrlines("LIST " + directory, lines.append)
    entries = {}
    for line in lines:
        parts = line.split(None, 8)
        if len(parts) >= 9 and line.startswith("-"):
            entries[parts[8]] = (int(parts[4]), " ".join(parts[5:8]))
    return entries


def pick_logs(ftp):
    """The logs folder to read and its files: the 0.6.0 one, else the app's old one."""
    for directory in (LOGS, OLD_LOGS):
        try:
            entries = listing(ftp, directory)
        except ftplib.Error:
            continue
        if entries:
            return directory, entries
    return LOGS, {}


def folders(ftp, directory):
    lines = []
    ftp.retrlines("LIST " + directory, lines.append)
    return [line.split(None, 8)[8] for line in lines
            if line.startswith("d") and len(line.split(None, 8)) >= 9]


def fetch(ftp, path):
    buffer = io.BytesIO()
    ftp.retrbinary("RETR " + path, buffer.write, 1 << 20)
    return buffer.getvalue()


def title_app(text):
    """The Steam app ID a title log belongs to, from its first report lines."""
    match = PROFILE.search(text[:4096])
    return int(match.group(1)) if match else None


def last_run(text):
    """When the game last started, from its session reports (seconds since 1970), or 0."""
    return max((int(match.group(1)) for match in REPORT_TIME.finditer(text)), default=0)


def games(title_logs, names):
    """The games with a log, the most recently run first."""
    rows = [{"title": title, "app": title_app(text), "name": names.get(title), "last_run": last_run(text)}
            for title, text in title_logs.items()]
    return sorted(rows, key=lambda row: (-row["last_run"], row["title"]))


def pick_title(title_logs, app=None, title=None):
    """The title ID to collect: the one asked for, the one running app, or the most recently run."""
    if title:
        return title if title in title_logs else None
    if app is not None:
        for name, text in title_logs.items():
            if title_app(text) == app:
                return name
        return None
    newest = games(title_logs, {})
    return newest[0]["title"] if newest else None


def game_table(rows):
    lines = []
    for number, row in enumerate(rows, 1):
        when = (datetime.fromtimestamp(row["last_run"]).strftime("%Y-%m-%d %H:%M")
                if row["last_run"] else "never")
        lines.append(f"{number:>3}. {row['name'] or 'unknown game'}  "
                     f"({row['title']}, app {row['app'] or '?'}, last run {when})")
    return "\n".join(lines)


# What Playden's "Store name in PS5 titles" setting adds to a title name; imported games get none.
STORE_SUFFIXES = (" (Steam)", " (GOG)", " (Epic)")


def without_store_suffix(name):
    """The game's own name: a title name without the store Playden added to it."""
    for suffix in STORE_SUFFIXES:
        if name and len(name) > len(suffix) and name.endswith(suffix):
            return name[:-len(suffix)]
    return name


def title_name(ftp, title):
    """The game's name from its PS5 title, without Playden's store suffix, such as " (Steam)"."""
    try:
        param = json.loads(fetch(ftp, f"/data/homebrew/{title}/sce_sys/param.json"))
    except (ftplib.Error, ValueError):
        return None
    localized = param.get("localizedParameters") or {}
    name = (localized.get(localized.get("defaultLanguage", "en-US")) or {}).get("titleName")
    return without_store_suffix(name)


def ask(rows):
    """The row the player picks, the newest when there is no terminal."""
    if not sys.stdin.isatty():
        return rows[0]
    while True:
        answer = input(f"Which game? [1-{len(rows)}, Enter for 1] ").strip()
        if not answer:
            return rows[0]
        if answer.isdigit() and 1 <= int(answer) <= len(rows):
            return rows[int(answer) - 1]
        print("Type a number from the list.")


def account_names(texts):
    names = set()
    for text in texts:
        names.update(match.group(1) for match in ACCOUNT.finditer(text))
    return names


def redact(text, names):
    text = STEAM_ID.sub("<steam-id>", text)
    for name in sorted(names, key=len, reverse=True):
        text = re.sub(re.escape(name), "<steam-user>", text, flags=re.IGNORECASE)
    return text


def firmware(texts):
    found = None
    for text in texts:
        for match in FIRMWARE.finditer(text):
            found = match.group(1)
    return found


def summary(info):
    lines = ["| Field | Value |", "| --- | --- |"]
    for key, value in info.items():
        lines.append(f"| {key} | {value if value not in (None, '') else 'unknown'} |")
    return "\n".join(lines) + "\n"


def active_kit(ftp):
    # The current kit folder first: a console updated from an earlier Playden may still hold the kit under the id it
    # had before, until Playden retires it, and one gone back to that Playden uses it again. Other folders are listed.
    names = sorted((name for name in folders(ftp, KITS) if not name.startswith(".")), key=lambda name: name != KIT_ID)
    others = {"other kit folders": ", ".join(names[1:])} if len(names) > 1 else {}
    for name in names:
        try:
            manifest = json.loads(fetch(ftp, f"{KITS}/{name}/manifest.json"))
        except (ftplib.Error, ValueError):
            continue
        # No kit_id: kit releases share it (the install folder), so it says nothing.
        return {
            "kit runtime": (manifest.get("runtime") or {}).get("artifact_revision"),
            "kit capabilities": ", ".join(manifest.get("capabilities") or []),
            **others,
        }
    return {"kit runtime": None, **others}


WEB_PORT = 7031


def http_get(base, path, timeout=30):
    with urllib.request.urlopen(base + path, timeout=timeout) as response:
        return response.read()


def http_games(index):
    """The rows game_table prints, from the web page's /api/logs."""
    return [{"title": game["title"], "app": game.get("app"), "name": game.get("name"),
             "last_run": game.get("last_run") or 0} for game in index.get("games", [])]


def collect_over_http(args):
    """The report from Playden's web access: the same zip, made and redacted by Playden."""
    base = f"http://{args.host}:{args.port or WEB_PORT}"
    try:
        index = json.loads(http_get(base, "/api/logs"))
    except (OSError, ValueError) as error:
        sys.exit(f"could not reach Playden at {base}: {error}\n"
                 "Is Playden open on the PS5, with Settings > Web access on?")
    rows = http_games(index)
    if args.list or not (args.all_titles or args.app is not None or args.title):
        if not rows:
            print("No game logs yet. Start Playden once after running a game: it copies the game's log when it "
                  "starts.", file=sys.stderr)
        else:
            print(game_table(rows))
        if args.list:
            return
        if rows:
            args.title = ask(rows)["title"]
    if args.all_titles:
        title = "all"
    else:
        title = None
        if args.title:
            title = args.title if any(row["title"] == args.title for row in rows) else None
        elif args.app is not None:
            title = next((row["title"] for row in rows if row["app"] == args.app), None)
        elif rows:
            title = rows[0]["title"]
        if title is None and (args.app is not None or args.title):
            asked = args.title or f"app {args.app}"
            print(f"warning: no game log found for {asked}; collecting Playden's logs only.\n"
                  "Start Playden once after the failed run: it copies the game's log when it starts.",
                  file=sys.stderr)
            title = "none"  # names no game: Playden's logs only
    query = "?" + urllib.parse.urlencode({"title": title}) if title else ""
    try:
        report = http_get(base, "/api/report.zip" + query, timeout=120)
    except OSError as error:
        sys.exit(f"could not download the report from {base}: {error}")
    out = args.out or Path(f"playden-logs-{datetime.now().strftime('%Y%m%d-%H%M%S')}.zip")
    out.write_bytes(report)
    try:
        with zipfile.ZipFile(out) as archive:
            names = [name for name in archive.namelist() if name.startswith("logs/")]
            print(archive.read("summary.md").decode("utf-8", "replace"))
    except (zipfile.BadZipFile, KeyError) as error:
        sys.exit(f"Playden sent a report that isn't a valid zip ({error}); it is in {out}")
    print(f"Wrote {out} ({len(names)} logs). Attach it to your test report.")
    print("Logs can still name your games and file paths. Look through them before you share.")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default=os.environ.get("PLAYDEN_PS5_HOST"),
                        help="the PS5's IP address (or set PLAYDEN_PS5_HOST)")
    parser.add_argument("--port", type=int, help="FTP port (default 2120, zftpd), or with --http the web port (7031)")
    parser.add_argument("--http", action="store_true",
                        help="ask Playden's web access instead of FTP (Playden must be open; always redacted)")
    which = parser.add_mutually_exclusive_group()
    which.add_argument("--app", type=int, help="Steam app ID of the game (from its store page URL)")
    which.add_argument("--title", help="PS5 title ID of the game, for example FAKE78003")
    which.add_argument("--all-titles", action="store_true", help="include every game's log")
    which.add_argument("--list", action="store_true", help="only list the games that have a log")
    parser.add_argument("--out", type=Path, help="zip to write (default: playden-logs-<time>.zip here)")
    parser.add_argument("--no-redact", action="store_true", help="keep the Steam account name and SteamIDs")
    args = parser.parse_args()
    if not args.host:
        parser.error("give the PS5's address with --host or PLAYDEN_PS5_HOST")
    if args.http:
        if args.no_redact:
            parser.error("--http reports are always redacted; use FTP (without --http) for --no-redact")
        collect_over_http(args)
        return
    args.port = args.port or 2120

    ftp = ftplib.FTP()
    try:
        ftp.connect(args.host, args.port, timeout=15)
        ftp.login()
    except (OSError, ftplib.Error) as error:
        sys.exit(f"could not connect to FTP at {args.host}:{args.port}: {error}\n"
                 "Is an FTP server (zftpd) running on the PS5?")

    logs_dir, entries = pick_logs(ftp)
    title_logs, playden_logs = {}, {}
    for name in entries:
        if name.endswith(SKIP_SUFFIXES):
            continue
        match = TITLE_LOG.match(name)
        if match:
            title_logs[match.group(1)] = fetch(ftp, f"{logs_dir}/{name}").decode("utf-8", "replace")
        elif name.startswith("title-"):
            continue  # per-title extras such as registry dumps; taken with their title below
        else:
            playden_logs[name] = fetch(ftp, f"{logs_dir}/{name}").decode("utf-8", "replace")

    if args.list or not (args.all_titles or args.app is not None or args.title):
        rows = games(title_logs, {title: title_name(ftp, title) for title in title_logs})
        if not rows:
            print("No game logs yet. Start Playden once after running a game: it copies the game's log when it "
                  "starts.", file=sys.stderr)
        elif args.list:
            print(game_table(rows))
            ftp.quit()
            return
        else:
            print(game_table(rows))
            args.title = ask(rows)["title"]
    if args.all_titles:
        chosen = sorted(title_logs)
    else:
        picked = pick_title(title_logs, args.app, args.title)
        if picked is None and (args.app is not None or args.title):
            asked = args.title or f"app {args.app}"
            print(f"warning: no game log found for {asked}; collecting Playden's logs only.\n"
                  "Start Playden once after the failed run: it copies the game's log when it starts.",
                  file=sys.stderr)
        chosen = [picked] if picked else []
    files = dict(playden_logs)
    for title in chosen:
        files[f"title-{title}.log"] = title_logs[title]
        for name in entries:
            if name.startswith(f"title-{title}.") and name != f"title-{title}.log":
                files[name] = fetch(ftp, f"{logs_dir}/{name}").decode("utf-8", "replace")

    info = {"collected": datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M UTC")}
    try:
        info["Playden package"] = fetch(ftp, IDENTITY).decode("utf-8", "replace").split()[-1][:12]
    except ftplib.Error:
        info["Playden package"] = None
    try:
        info["Playden version"] = fetch(ftp, "/data/homebrew/FAKE77031/playden-version").decode("ascii").strip()
    except (ftplib.Error, UnicodeError):
        info["Playden version"] = None
    info["firmware"] = firmware([playden_logs.get("runtime.log.1", ""), playden_logs.get("runtime.log", "")])
    info.update(active_kit(ftp))
    info["game titles"] = ", ".join(
        f"{title} (app {title_app(title_logs[title]) or '?'})" for title in chosen) or "none"
    ftp.quit()

    # Names come from every log, so a game that stopped before Wine named the user is still covered.
    names = set() if args.no_redact else account_names([*title_logs.values(), *playden_logs.values()])
    if not args.no_redact:
        files = {name: redact(text, names) for name, text in files.items()}
    info["redacted"] = "no" if args.no_redact else "Steam account name and SteamIDs"
    report = summary(info)

    out = args.out or Path(f"playden-logs-{datetime.now().strftime('%Y%m%d-%H%M%S')}.zip")
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as archive:
        archive.writestr("summary.md", report)
        for name, text in sorted(files.items()):
            archive.writestr(f"logs/{name}", text)
    print(report)
    print(f"Wrote {out} ({len(files)} logs). Attach it to your test report.")
    if not args.no_redact:
        print("Logs can still name your games and file paths. Look through them before you share.")


if __name__ == "__main__":
    main()
