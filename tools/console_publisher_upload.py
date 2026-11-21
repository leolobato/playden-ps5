#!/usr/bin/env python3
"""Upload the publisher pack to the console over FTP, then verify it.

The publisher pack holds the closed Rockstar Games helpers that Rockstar's Steam games need.
It is not part of Playden: each Playden build accepts exactly one pack
version, listed under "publisher_pack" in dependencies.lock.json. This tool refuses a pack of
another version or with any archive that doesn't match its pin. The folder may hold only some of the archives:
Playden checks each publisher's archive on its own.

The folder must hold release.json (playden-helper-tools format: version, artifacts, tree_sha256)
and the archives. They are written to a hidden staging folder, read back and hashed, and only
then renamed to /data/playden/publisher/<version>. Other versions there are removed, since this
Playden can't use them.
"""
import argparse
import ftplib
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(Path(__file__).resolve().parent))

from console_kit_upload import listing, remove_tree, remote_sha256  # noqa: E402


class PackError(Exception):
    pass


def sha256_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def check_pack(folder, lock):
    """Returns (version, [archive names]) for a local pack matching the lock, or raises PackError."""
    pinned = lock["publisher_pack"]
    version = pinned["version"]
    release_path = folder / "release.json"
    if not release_path.is_file():
        raise PackError(f"{release_path} is missing")
    release = json.loads(release_path.read_text())
    if release.get("version") != version:
        raise PackError(f"the pack is version {release.get('version')}; this Playden needs {version}")
    artifacts = release.get("artifacts", {})
    trees = release.get("tree_sha256", {})
    # A pack may hold only some publishers' archives (the builds page hosts the Rockstar one alone).
    names = sorted(name for name in pinned["archives"] if (folder / name).is_file())
    if not names:
        raise PackError(f"the folder holds none of the pinned archives: {', '.join(sorted(pinned['archives']))}")
    for name in names:
        pin = pinned["archives"][name]
        path = folder / name
        if artifacts.get(name) != pin["sha256"]:
            raise PackError(f"release.json lists another {name}")
        if name in trees and trees[name] != pin["tree_sha256"]:
            raise PackError(f"release.json lists another file tree for {name}")
        if sha256_file(path) != pin["sha256"]:
            raise PackError(f"{name} does not match its pin")
    return version, names


def ensure_parents(ftp, remote_root, log):
    """Creates the folders above and at remote_root; /data/playden must stay writable for Playden."""
    path = ""
    playden_root = str(PurePosixPath(remote_root).parent)
    for part in PurePosixPath(remote_root).parts[1:]:
        path += "/" + part
        try:
            ftp.mkd(path)
        except ftplib.error_perm:
            continue  # already there
        mode = "777" if path == playden_root else "755"
        ftp.sendcmd(f"SITE CHMOD {mode} {path}")
        log(f"created {path}/ {mode}")


def upload(ftp, folder, version, names, remote_root, log=print):
    """Uploads and verifies the pack; returns a list of problems (empty on success)."""
    ensure_parents(ftp, remote_root, log)
    present = listing(ftp, remote_root)
    staging_name = f".{version}.upload"
    staging = f"{remote_root}/{staging_name}"
    if staging_name in present:
        remove_tree(ftp, staging)
        log(f"removed an unfinished upload {staging}")
    ftp.mkd(staging)
    ftp.sendcmd(f"SITE CHMOD 755 {staging}")
    files = ["release.json"] + names
    for name in files:
        with (folder / name).open("rb") as stream:
            ftp.storbinary(f"STOR {staging}/{name}", stream, 1 << 20)
        ftp.sendcmd(f"SITE CHMOD 644 {staging}/{name}")
        log(f"uploaded {name}")
    problems = []
    entries = listing(ftp, staging)
    for name in files:
        local = folder / name
        observed = entries.get(name)
        if not observed or observed[0] != "-rw-r--r--" or observed[2] != local.stat().st_size:
            problems.append(f"{name}: listed {observed}")
        elif remote_sha256(ftp, f"{staging}/{name}") != sha256_file(local):
            problems.append(f"{name}: sha256 mismatch")
    if problems:
        return problems
    for name, (_, is_dir, _) in listing(ftp, remote_root).items():
        if is_dir and name != staging_name:
            remove_tree(ftp, f"{remote_root}/{name}")
            log(f"removed {remote_root}/{name}")
    ftp.rename(staging, f"{remote_root}/{version}")
    log(f"verified {len(files)} files; installed {remote_root}/{version}")
    return []


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("pack", type=Path, help="folder with release.json and the archives")
    parser.add_argument("--host", default=os.environ.get("PLAYDEN_PS5_HOST"),
                        help="the PS5's IP address (or set PLAYDEN_PS5_HOST)")
    parser.add_argument("--port", type=int, default=2120)
    parser.add_argument("--remote-root", default="/data/playden/publisher")
    parser.add_argument("--lock", type=Path, default=ROOT / "dependencies.lock.json")
    args = parser.parse_args()
    if not args.host:
        parser.error("give the PS5's address with --host or PLAYDEN_PS5_HOST")
    try:
        version, names = check_pack(args.pack, json.loads(args.lock.read_text()))
    except PackError as error:
        print(f"refusing: {error}", file=sys.stderr)
        return 1
    ftp = ftplib.FTP()
    ftp.connect(args.host, args.port, timeout=60)
    ftp.login()
    problems = upload(ftp, args.pack, version, names, args.remote_root)
    if problems:
        print("verification failed; the staging folder is kept for inspection:", file=sys.stderr)
        for problem in problems:
            print("  " + problem, file=sys.stderr)
        return 2
    ftp.quit()
    print("Restart Playden to use the publisher pack.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
