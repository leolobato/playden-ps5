#!/usr/bin/env python3
"""Write playden-update.json, the release asset Playden's self-update reads.

Run it on the app zip (the FAKE77031/ app folder at the top, tools and documents next to it) and the kit zip before publishing a GitHub release, and attach its output to the
release next to them:

    python3 tools/write_update_manifest.py --app-zip FAKE77031.zip \\
        --kit-zip playden-kit-0.10.zip --kit-revision 0.10 --min-from 0.2.0 \\
        --notes "Faster downloads and simpler game settings." --out playden-update.json

It reads the app's version and title id from the zip's sce_sys/param.json and the kit's id and
manifest from its manifest.json, and refuses a kit that is not the one dependencies.lock.json pins
(the kit this Playden was tested with). --notes is a short plain-text summary for the console's
update dialog, which links to the release page (as a QR code) for the full notes. A release whose kit did not change may leave the kit zip
out of the release; pass the zip anyway so the manifest names the kit.

--release-json writes a release.json like GitHub's API answer as well, for a console test with a
dev/update-local folder (see src/update/update_service.h).
"""
import argparse
import hashlib
import json
import pathlib
import re
import sys
import zipfile

VERSION = re.compile(r"[vV]?([0-9]{1,6})\.([0-9]{1,6})\.([0-9]{1,6})(?:-beta\.([1-9][0-9]{0,5}))?")


def version(text):
    match = VERSION.fullmatch(text)
    if not match:
        raise SystemExit(f"not a version: {text!r}")
    return ".".join(str(int(part)) for part in match.groups()[:3]) + (f"-beta.{match[4]}" if match[4] else "")


def app_prefix(archive):
    """The "<title id>/" app folder at the top of the app zip, the layout ProsperoStore installs. Tools and
    documents may sit next to it."""
    found = sorted({name[: -len("sce_sys/param.json")] for name in archive.namelist()
                    if name.endswith("/sce_sys/param.json") and name.count("/") == 2})
    if len(found) != 1 or found[0] in ("./", "../"):
        raise SystemExit(f"{archive.filename}: needs exactly one <title id>/sce_sys/param.json at the top, found {found}")
    return found[0]


def inner_prefix(archive, marker):
    """The "<top>/<inner>/" folder holding marker, as Playden's FindInnerPrefix finds it."""
    for name in archive.namelist():
        if name.endswith("/" + marker):
            prefix = name[: -len(marker)]
            if prefix.count("/") == 2 and ".." not in prefix.split("/"):
                return prefix
    raise SystemExit(f"{archive.filename}: no <top>/<folder>/{marker}")


MAX_NOTES = 400
MARKDOWN = re.compile(r"^\s*(#|>|\||[-*+] |\d+\. |```)|\]\(|\*\*|`|<[a-z!/]", re.MULTILINE | re.IGNORECASE)


def summary(text):
    """The console shows plain text only: no Markdown, at most MAX_NOTES characters."""
    text = "\n".join(line.rstrip() for line in text.replace("\r\n", "\n").replace("\r", "\n").split("\n")).strip()
    if len(text) > MAX_NOTES:
        raise SystemExit(f"--notes is {len(text)} characters; write a summary of at most {MAX_NOTES} "
                         "(the release page has the full notes)")
    if MARKDOWN.search(text):
        raise SystemExit("--notes must be plain text: the console does not format Markdown "
                         "(headings, quotes, lists, links, code)")
    return text


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as file:
        for block in iter(lambda: file.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--app-zip", required=True, type=pathlib.Path)
    parser.add_argument("--kit-zip", required=True, type=pathlib.Path)
    parser.add_argument("--kit-revision", required=True, help='the kit version, e.g. "0.10"')
    parser.add_argument("--tag", help="release tag; must match the packaged full version")
    parser.add_argument("--min-from", required=True, help="oldest Playden version that can update to this one")
    parser.add_argument("--notes", default="", help=f"a plain-text summary for the update dialog, at most {MAX_NOTES} characters")
    parser.add_argument("--lock", type=pathlib.Path,
                        default=pathlib.Path(__file__).resolve().parent.parent / "dependencies.lock.json")
    parser.add_argument("--out", required=True, type=pathlib.Path)
    parser.add_argument("--release-json", type=pathlib.Path, help="also write a local release.json for console tests")
    parser.add_argument("--allow-other-kit", action="store_true",
                        help="accept a kit that dependencies.lock.json does not pin (console tests only)")
    args = parser.parse_args(argv)

    with zipfile.ZipFile(args.app_zip) as app:
        prefix = app_prefix(app)
        param = json.loads(app.read(prefix + "sce_sys/param.json"))
        numeric_version = version(param["contentVersion"])
        try:
            full_text = app.read(prefix + "playden-version").decode("ascii")
            app_version = version(full_text.removesuffix("\n"))
        except KeyError:
            app_version = numeric_version  # Packages predating full version metadata.
    if app_version.split("-", 1)[0] != numeric_version:
        raise SystemExit("packaged version does not match param.json contentVersion")
    if args.tag and version(args.tag) != app_version:
        raise SystemExit("release tag does not match the packaged version")
    title_id = param["titleId"]
    if not re.fullmatch(r"[A-Z]{4}\d{5}", title_id):
        raise SystemExit(f"unexpected title id {title_id!r}")
    if prefix != title_id + "/":
        raise SystemExit(f"app folder {prefix!r} is not named after the title id {title_id!r}")

    with zipfile.ZipFile(args.kit_zip) as kit:
        kit_prefix = inner_prefix(kit, "manifest.json")
        manifest_bytes = kit.read(kit_prefix + "manifest.json")
    kit_manifest = json.loads(manifest_bytes)
    kit_id = kit_manifest["kit_id"]
    if kit_prefix.rstrip("/").split("/")[-1] != kit_id:
        raise SystemExit(f"kit folder {kit_prefix!r} is not named after kit_id {kit_id!r}")
    kit_sha = hashlib.sha256(manifest_bytes).hexdigest()
    pinned = json.loads(args.lock.read_text())["prospero_kit"]
    if not args.allow_other_kit and (pinned["kit_id"] != kit_id or pinned["manifest_sha256"] != kit_sha):
        raise SystemExit(f"the kit zip ({kit_id} {kit_sha[:12]}) is not the kit dependencies.lock.json pins "
                         f"({pinned['kit_id']} {pinned['manifest_sha256'][:12]})")
    if not re.fullmatch(r"[A-Za-z0-9._-]{1,128}", args.kit_revision) or args.kit_revision.startswith("."):
        raise SystemExit(f"bad kit revision {args.kit_revision!r}")

    notes = summary(args.notes)
    update = {
        "format": 1,
        "app": {"version": app_version, "title_id": title_id, "min_from": version(args.min_from),
                "asset": args.app_zip.name, "prefix": prefix},
        "kit": {"kit_id": kit_id, "revision": args.kit_revision, "manifest_sha256": kit_sha,
                "asset": args.kit_zip.name, "prefix": kit_prefix},
        "notes": notes,
    }
    text = json.dumps(update, indent=2) + "\n"
    args.out.write_text(text)
    print(f"{args.out}: Playden {app_version} ({title_id}), kit {kit_id} {args.kit_revision} {kit_sha[:12]}")

    if args.release_json:
        assets = []
        for path in (args.out, args.app_zip, args.kit_zip):
            assets.append({"name": path.name, "size": path.stat().st_size,
                           "browser_download_url": f"https://github.com/local/release/{path.name}",
                           "digest": "sha256:" + sha256_file(path)})
        release = {"tag_name": "v" + app_version, "draft": False, "prerelease": "-beta." in app_version, "assets": assets}
        args.release_json.write_text(json.dumps(release, indent=2) + "\n")
        print(f"{args.release_json}: local release v{app_version}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
