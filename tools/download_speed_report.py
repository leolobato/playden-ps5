#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Report Steam install measurements from the PS5's two rotating install logs.

Install bytes count verified chunks written this run; HTTP bytes include retries and
preparation requests. DNS/TCP/TLS/TTFB cover all CDN requests in the run. TTFB is
cumulative curl start-transfer time, so it overlaps TCP/TLS and is not a separate
write-loop stage. File creation floor is the requested comparison (22.5 ms/file),
not a measured duration. Interrupted runs use their last progress snapshot.
Per-server MB/s uses the logged whole MB divided by summed fetch time, so parallel
requests do not make it a wall-clock download rate.

Example: download_speed_report.py --host 192.168.1.50 --app 439490 \
    --since 2026-10-12T10:00:00Z --csv results/baseline.csv
"""
import argparse
import csv
from datetime import datetime, timezone
import ftplib
import io
import os
from pathlib import Path
import re
import sys

LOGS = "/data/playden/logs"
OLD_LOGS = "/data/homebrew/FAKE77031/logs"  # before 0.6.0
LINE = re.compile(r"^(\S+) app=(\d+) step=(\S+) result=(\S+) detail=(.*)$")
METRIC = re.compile(r"\b([a-z_]+)=(\d+)\b")
SERVER_LABEL = r"(?:akamai|fastly|google|alibaba|edgenext|valve|cache|other)"
FIRST_SERVER = re.compile(r"\bfirst=(" + SERVER_LABEL + r"@[0-8])(?: |$)")
SERVER_METRIC = re.compile(r"(" + SERVER_LABEL + r"@[0-8]):req=\d+,fail=\d+,mb=(\d+),ms=(\d+)(?: |$)")
STAGES = ("fetch", "proc", "create", "write", "jrnl", "back")
COLUMNS = ("app", "start", "outcome", "sample", "files_total", "compressed_bytes", "download_ms", "mb_s",
           "first_server", "server_mb_s",
           *(stage + "_pct" for stage in STAGES), "req", "conn", "retry", "bad", "slots", "reuse",
           "creation_floor_ms", "res", "tcp", "tls", "ttfb", "http_ms", "http_b", "fail",
           "access", "servers", "manifest", "prep", "ch", "sk", "fc", "dc", "original_bytes",
           "download_total", "chunks_pending", "files_pending", "bytes_pending")


def timestamp(value):
    """An ISO timestamp (UTC when unspecified), or Unix seconds."""
    try:
        return datetime.fromtimestamp(float(value), timezone.utc)
    except ValueError:
        parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
        return parsed.replace(tzinfo=timezone.utc) if parsed.tzinfo is None else parsed


def parse_runs(text):
    active, runs = {}, []

    def begin(app, start):
        run = {"app": app, "start": start, "outcome": "interrupted", "sample": "missing", "metrics": {},
               "first_server": None, "server_rates": {}}
        active[app] = run
        runs.append(run)
        return run

    for line in text.splitlines():
        match = LINE.match(line)
        if not match:
            continue
        start, app, step, result, detail = match.groups()
        app = int(app)
        if step == "trace" and result == "access":
            begin(app, start)
        elif step == "stats" and result in ("shape", "progress", "end"):
            run = active.get(app) or begin(app, start)
            run["metrics"].update({key: int(value) for key, value in METRIC.findall(detail)})
            if result != "shape":
                run["sample"] = result
        elif step == "servers" and result == "listed":
            run = active.get(app) or begin(app, start)
            first = FIRST_SERVER.search(detail)
            run["first_server"] = first.group(1) if first else None
        elif step == "stats" and result == "servers":
            run = active.get(app) or begin(app, start)
            for server, mb, milliseconds in SERVER_METRIC.findall(detail):
                run["server_rates"][server] = int(mb) * 1000 / int(milliseconds) if int(milliseconds) else None
        elif step == "install":
            run = active.pop(app, None) or begin(app, start)
            run["outcome"] = result
            active.pop(app, None)
        elif step == "depots" or (step in ("app_info", "entitlements") and result != "none"):
            # Depots logs "none" for missing Windows content; it is a prerequisite failure.
            # These errors occur before access; preserve failed precondition runs too.
            run = begin(app, start)
            run["outcome"] = step + ":" + result
            active.pop(app, None)

    rows = []
    for run in runs:
        metrics = run.pop("metrics")
        rates = run.pop("server_rates")
        run["server_mb_s"] = " ".join(server + ":" + (f"{rate:.2f}" if rate is not None else "-")
                                      for server, rate in rates.items()) or None
        row = {column: None for column in COLUMNS}
        row.update(run)
        row.update({key: value for key, value in metrics.items() if key in COLUMNS})
        row["compressed_bytes"] = metrics.get("cb")
        row["original_bytes"] = metrics.get("ob")
        row["download_ms"] = metrics.get("dl_ms")
        duration = row["download_ms"]
        row["mb_s"] = metrics["cb"] / duration / 1000 if duration and "cb" in metrics else None
        for stage in STAGES:
            row[stage + "_pct"] = 100 * metrics[stage] / duration if duration and stage in metrics else None
        row["creation_floor_ms"] = metrics["fc"] * 22.5 if "fc" in metrics else None
        rows.append(row)
    return rows


def filter_runs(rows, since=None, app=None):
    return [row for row in rows if (app is None or row["app"] == app)
            and (since is None or timestamp(row["start"]) >= timestamp(since))]


def fetch_logs(host, port):
    parts = []
    with ftplib.FTP() as ftp:
        ftp.connect(host, port, timeout=15)
        ftp.login()
        # List first: zftpd answers 451, rather than 550, for an absent rotated log.
        listing = []
        logs = LOGS
        for logs in (LOGS, OLD_LOGS):
            listing = []
            try:
                ftp.retrlines("LIST " + logs, listing.append)
            except ftplib.Error:
                continue
            if any(line.startswith("-") for line in listing):
                break
        names = {line.split(None, 8)[8] for line in listing
                 if line.startswith("-") and len(line.split(None, 8)) == 9}
        for name in ("install.log.1", "install.log"):
            if name not in names:
                continue
            buffer = io.BytesIO()
            try:
                ftp.retrbinary("RETR " + logs + "/" + name, buffer.write, 1 << 20)
            except ftplib.error_perm as error:
                if str(error).startswith("550"):
                    continue
                raise
            parts.append(buffer.getvalue().decode("utf-8", errors="replace"))
    if not parts:
        raise ValueError("No install logs available")
    return "\n".join(parts)


def write_csv(rows, output):
    writer = csv.DictWriter(output, fieldnames=COLUMNS)
    writer.writeheader()
    writer.writerows(rows)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default=os.environ.get("PLAYDEN_PS5_HOST"))
    parser.add_argument("--port", type=int, default=2120)
    parser.add_argument("--since", help="ISO UTC timestamp or Unix seconds")
    parser.add_argument("--app", type=int)
    parser.add_argument("--csv", nargs="?", const="-", help="CSV file, or stdout when no path is given")
    parser.add_argument("--log", action="append", type=Path, help="Local log instead of FTP, oldest first")
    parser.add_argument("--raw", type=Path, help="Save the fetched raw log lines")
    args = parser.parse_args(argv)
    if not args.log and not args.host:
        parser.error("give --host / PLAYDEN_PS5_HOST, or --log")
    try:
        text = "\n".join(path.read_text() for path in args.log) if args.log else fetch_logs(args.host, args.port)
        rows = filter_runs(parse_runs(text), args.since, args.app)
        if args.raw:
            args.raw.write_text(text)
        if args.csv:
            if args.csv == "-":
                write_csv(rows, sys.stdout)
            else:
                with Path(args.csv).open("w", newline="") as output:
                    write_csv(rows, output)
        else:
            columns = COLUMNS[:23]
            print("\t".join(columns))
            for row in rows:
                print("\t".join("-" if row[column] is None else
                                f"{row[column]:.2f}" if isinstance(row[column], float) else str(row[column])
                                for column in columns))
        print("Stage shares use download_ms. CDN timings include preparation; TTFB overlaps TCP/TLS. "
              "Creation floor is fc × 22.5 ms, a comparison estimate.", file=sys.stderr)
    except (OSError, ValueError, ftplib.Error) as error:
        print(f"download speed report failed: {type(error).__name__}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
