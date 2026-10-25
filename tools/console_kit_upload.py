#!/usr/bin/env python3
"""Upload a packaged kit to the console over FTP with exact modes, then verify.

The kit is written into a temporary sibling directory, every file is read back
and hashed, file and directory modes are compared with manifest.json, and only
then is the directory renamed to its final name. An existing final directory is
never replaced; the previous kit is moved aside, and older copies moved aside
earlier are deleted so the console storage doesn't fill up. Development tooling;
it does not install or register anything.
"""
import argparse
import ftplib
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import sys


def listing(ftp, directory):
    """name -> (mode string, is_dir, size) for one remote directory."""
    entries = {}
    lines = []
    ftp.retrlines("LIST " + directory, lines.append)
    for line in lines:
        parts = line.split(None, 8)
        if len(parts) < 9 or parts[8] in (".", ".."):
            continue
        entries[parts[8]] = (parts[0], parts[0].startswith("d"), int(parts[4]))
    return entries


def remove_tree(ftp, path):
    for name, (_, is_dir, _) in listing(ftp, path).items():
        child = f"{path}/{name}"
        if is_dir:
            remove_tree(ftp, child)
        else:
            ftp.delete(child)
    ftp.rmd(path)


def mode_string(mode, directory):
    bits = "".join(flag if mode & (1 << (8 - index)) else "-"
                   for index, flag in enumerate("rwxrwxrwx"))
    return ("d" if directory else "-") + bits


def remote_sha256(ftp, path):
    digest = hashlib.sha256()
    ftp.retrbinary("RETR " + path, digest.update, 1 << 20)
    return digest.hexdigest()


def kit_folder(path):
    """The kit folder: the given one, or the one kit inside it (an unzipped playden-kit-<version> folder)."""
    if (path / "manifest.json").is_file():
        return path
    kits = [child for child in sorted(path.iterdir()) if (child / "manifest.json").is_file()]
    if len(kits) != 1:
        raise SystemExit(f"{path}: no kit found (a folder with manifest.json, or one that holds exactly one)")
    return kits[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kit", type=Path, help="the kit folder, or the unzipped playden-kit-<version> folder")
    parser.add_argument("--host", default=os.environ.get("PLAYDEN_PS5_HOST"),
                        help="the PS5's IP address (or set PLAYDEN_PS5_HOST)")
    parser.add_argument("--port", type=int, default=2120)
    parser.add_argument("--remote-parent", default="/data/playden/kits")
    parser.add_argument("--replace", action="store_true",
                        help="after verification, move an existing kit aside instead of refusing")
    args = parser.parse_args()
    if not args.host:
        parser.error("give the PS5's address with --host or PLAYDEN_PS5_HOST")
    args.kit = kit_folder(args.kit)
    manifest = json.loads((args.kit / "manifest.json").read_text())
    kit_id = manifest["kit_id"]
    final = f"{args.remote_parent}/{kit_id}"
    staging = f"{args.remote_parent}/.{kit_id}.upload"
    ftp = ftplib.FTP()
    ftp.connect(args.host, args.port, timeout=60)
    ftp.login()
    try:
        parent = listing(ftp, args.remote_parent)
    except ftplib.all_errors:
        # A console without Playden data yet (first install, or after a system reinstall).
        path = ""
        for part in PurePosixPath(args.remote_parent).parts[1:]:
            path += "/" + part
            try:
                ftp.mkd(path)
            except ftplib.error_perm:
                continue  # already there
            # Playden itself must be able to write to its data root (/data/playden) as the app's user; it would
            # create that folder on first start, so one made here has to be writable by others.
            mode = "777" if path == str(PurePosixPath(args.remote_parent).parent) else "755"
            ftp.sendcmd(f"SITE CHMOD {mode} {path}")
            print(f"created {path}/ {mode}")
        parent = listing(ftp, args.remote_parent)
    if kit_id in parent and not args.replace:
        print(f"refusing: {final} already exists (use --replace)", file=sys.stderr)
        return 1
    if f".{kit_id}.upload" in parent:
        print(f"refusing: stale {staging} exists; inspect it first", file=sys.stderr)
        return 1
    files = {entry["path"]: entry for entry in manifest["files"]}
    files["manifest.json"] = {"path": "manifest.json", "mode": "0644",
                              "size": (args.kit / "manifest.json").stat().st_size,
                              "sha256": hashlib.sha256((args.kit / "manifest.json").read_bytes()).hexdigest()}
    directories = sorted({str(parent) for path in files for parent in PurePosixPath(path).parents
                          if str(parent) != "."}, key=lambda value: value.count("/"))
    ftp.mkd(staging)
    ftp.sendcmd(f"SITE CHMOD 755 {staging}")
    for directory in directories:
        ftp.mkd(f"{staging}/{directory}")
        ftp.sendcmd(f"SITE CHMOD 755 {staging}/{directory}")
    for index, (path, entry) in enumerate(sorted(files.items()), 1):
        with (args.kit / path).open("rb") as stream:
            ftp.storbinary(f"STOR {staging}/{path}", stream, 1 << 20)
        ftp.sendcmd(f"SITE CHMOD {entry['mode'][-3:]} {staging}/{path}")
        if index % 250 == 0:
            print(f"uploaded {index}/{len(files)}", flush=True)
    # Independent read-back: bytes, sizes and modes as the console reports them.
    problems = []
    by_directory = {}
    for path in files:
        by_directory.setdefault(str(PurePosixPath(path).parent), []).append(path)
    for directory, paths in by_directory.items():
        remote_directory = staging if directory == "." else f"{staging}/{directory}"
        entries = listing(ftp, remote_directory)
        for path in paths:
            name = PurePosixPath(path).name
            entry = files[path]
            observed = entries.get(name)
            expected_mode = mode_string(int(entry["mode"], 8), False)
            if not observed or observed[0] != expected_mode or observed[2] != entry["size"]:
                problems.append(f"{path}: listed {observed} expected {expected_mode} size {entry['size']}")
            elif remote_sha256(ftp, f"{remote_directory}/{name}") != entry["sha256"]:
                problems.append(f"{path}: sha256 mismatch")
    for directory in directories:
        head, _, name = (f"{staging}/{directory}").rpartition("/")
        observed = listing(ftp, head).get(name)
        if not observed or observed[0] != "drwxr-xr-x":
            problems.append(f"{directory}/: listed {observed}")
    if problems:
        print("verification failed; staging kept for inspection:", file=sys.stderr)
        for problem in problems[:50]:
            print("  " + problem, file=sys.stderr)
        return 2
    if kit_id in parent:
        # Named after the kit being moved aside, so the name says which kit it holds.
        try:
            previous = io.BytesIO()
            ftp.retrbinary(f"RETR {final}/manifest.json", previous.write)
            tag = json.loads(previous.getvalue())["files_sha256"][:12]
        except (ftplib.Error, ValueError, KeyError, TypeError):
            tag = "unknown"
        retired = f"{args.remote_parent}/.{kit_id}.retired-{tag}"
        if f".{kit_id}.retired-{tag}" in listing(ftp, args.remote_parent):
            remove_tree(ftp, retired)  # an earlier copy of this same kit
        ftp.rename(final, retired)
        print(f"moved previous kit aside to {retired}")
        for name in listing(ftp, args.remote_parent):
            if name.startswith(f".{kit_id}.retired-") and f"{args.remote_parent}/{name}" != retired:
                remove_tree(ftp, f"{args.remote_parent}/{name}")
                print(f"deleted older copy {name}")
    ftp.rename(staging, final)
    print(f"verified {len(files)} files and {len(directories)} directories; installed {final}")
    # A kit installed under another id (renamed since): Playden versions that use this id retire it at
    # their next start, as a previous kit the next update removes; versions that use the other id keep it.
    for name, (_, is_dir, _) in sorted(parent.items()):
        if is_dir and not name.startswith(".") and name != kit_id:
            print(f"note: {args.remote_parent}/{name} holds a kit under another id; it is left as it is")
    ftp.quit()
    return 0


if __name__ == "__main__":
    sys.exit(main())
