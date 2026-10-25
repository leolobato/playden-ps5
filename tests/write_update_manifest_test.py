#!/usr/bin/env python3
"""Tests for tools/write_update_manifest.py."""
import hashlib
import json
import pathlib
import subprocess
import sys
import tempfile
import zipfile

TOOL = pathlib.Path(__file__).resolve().parent.parent / "tools" / "write_update_manifest.py"


def check(condition, message):
    if not condition:
        print("FAIL:", message)
        sys.exit(1)


def run(*args):
    return subprocess.run([sys.executable, str(TOOL), *args], capture_output=True, text=True)


def main():
    with tempfile.TemporaryDirectory() as folder:
        root = pathlib.Path(folder)
        manifest = json.dumps({"kit_id": "playden-kit", "files": []}).encode()
        sha = hashlib.sha256(manifest).hexdigest()
        (root / "lock.json").write_text(json.dumps({"prospero_kit": {"kit_id": "playden-kit", "manifest_sha256": sha}}))
        app = root / "playden-ps5-0.3.0.zip"
        with zipfile.ZipFile(app, "w") as archive:
            archive.writestr("playden-ps5-0.3.0/tools/x.py", "")
            archive.writestr("playden-ps5-0.3.0/FAKE77031/sce_sys/param.json",
                             json.dumps({"titleId": "FAKE77031", "contentVersion": "00.003.000"}))
        kit = root / "playden-kit-0.13.zip"
        with zipfile.ZipFile(kit, "w") as archive:
            archive.writestr("playden-kit-0.13/LICENSES/a", "")
            archive.writestr("playden-kit-0.13/playden-kit/manifest.json", manifest)
        out, release = root / "playden-update.json", root / "release.json"
        common = ["--app-zip", str(app), "--kit-zip", str(kit), "--kit-revision", "0.13", "--min-from", "v0.2.0",
                  "--lock", str(root / "lock.json"), "--out", str(out)]
        result = run(*common, "--notes", "New things.", "--release-json", str(release))
        check(result.returncode == 0, result.stderr)
        update = json.loads(out.read_text())
        check(update == {"format": 1,
                         "app": {"version": "0.3.0", "title_id": "FAKE77031", "min_from": "0.2.0",
                                 "asset": "playden-ps5-0.3.0.zip", "prefix": "playden-ps5-0.3.0/FAKE77031/"},
                         "kit": {"kit_id": "playden-kit", "revision": "0.13", "manifest_sha256": sha,
                                 "asset": "playden-kit-0.13.zip", "prefix": "playden-kit-0.13/playden-kit/"},
                         "notes": "New things."}, "manifest content")
        # The notes are a short plain-text summary: Markdown and long text are refused.
        check(run(*common, "--notes", "## What's new\n- Faster").returncode != 0, "markdown refused")
        check(run(*common, "--notes", "See [the page](https://x).").returncode != 0, "markdown link refused")
        check(run(*common, "--notes", "word " * 100).returncode != 0, "long notes refused")
        check(run(*common, "--notes", "Faster downloads.\r\n\r\nFewer bugs.  ").returncode == 0, "plain notes")
        check(json.loads(out.read_text())["notes"] == "Faster downloads.\n\nFewer bugs.", "line endings")
        result = run(*common, "--notes", "New things.", "--release-json", str(release))
        listing = json.loads(release.read_text())
        check(listing["tag_name"] == "v0.3.0" and [a["name"] for a in listing["assets"]] ==
              ["playden-update.json", "playden-ps5-0.3.0.zip", "playden-kit-0.13.zip"], "local release")
        check(all(a["digest"].startswith("sha256:") and a["size"] > 0 for a in listing["assets"]), "digests")
        # Full beta identity lives alongside the numeric PS5 version.
        with zipfile.ZipFile(app, "w") as archive:
            prefix = "playden-ps5-0.3.0/FAKE77031/"
            archive.writestr(prefix + "sce_sys/param.json",
                             json.dumps({"titleId": "FAKE77031", "contentVersion": "00.007.001"}))
            archive.writestr(prefix + "playden-version", "0.7.1-beta.2\n")
        result = run(*common, "--min-from", "0.7.1-beta.1", "--tag", "v0.7.1-beta.2",
                     "--release-json", str(release))
        check(result.returncode == 0, result.stderr)
        check(json.loads(out.read_text())["app"]["version"] == "0.7.1-beta.2", "full beta version")
        check(json.loads(out.read_text())["app"]["min_from"] == "0.7.1-beta.1", "beta minimum")
        check(json.loads(release.read_text())["prerelease"], "local beta marked prerelease")
        check(run(*common, "--tag", "v0.7.1").returncode != 0, "tag mismatch refused")
        for bad in ("0.7.1-beta.0", "0.7.1-beta.01", "0.7.1-rc.1", "0.7.1+meta", "0.7.1\n"):
            check(run(*common, "--min-from", bad).returncode != 0, "invalid version refused: " + repr(bad))
        # A kit the lock does not pin is refused unless a test allows it.
        (root / "lock.json").write_text(json.dumps({"prospero_kit": {"kit_id": "playden-kit", "manifest_sha256": "0" * 64}}))
        check(run(*common).returncode != 0, "unpinned kit refused")
        check(run(*common, "--allow-other-kit").returncode == 0, "unpinned kit allowed for tests")
        check(run(*[a if a != "0.13" else "../0.13" for a in common], "--allow-other-kit").returncode != 0, "bad revision")
    print("ok")


if __name__ == "__main__":
    main()
