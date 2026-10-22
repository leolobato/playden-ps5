#!/usr/bin/env python3
"""community_recipe_check on the good, flagged and hostile fixtures, as the recipes repository's CI runs it.

  test_community_recipe_check.py <community_recipe_check> <tests/fixtures/community-recipes>
"""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile

CHECKER = sys.argv[1]
FIXTURES = Path(sys.argv[2])
GOOD = FIXTURES / "recipes/274520/120hz-left-stick-pointer.json"
FLAGGED = FIXTURES / "flagged/publisher-key.json"
failures = []


def check(condition, message):
    if not condition:
        failures.append(message)


def run(*arguments):
    return subprocess.run([CHECKER, *map(str, arguments)], capture_output=True, text=True)


# The good recipe: ok with its revision, the command it starts, and what changes how it starts.
good = run(GOOD)
revision = hashlib.sha256(GOOD.read_bytes()).hexdigest()
check(good.returncode == 0, f"good recipe exits 0: {good.returncode} {good.stdout}{good.stderr}")
check(f"ok {GOOD} {revision} community-274520-120hz-left-stick-pointer\n" in good.stdout, "good recipe line: " + good.stdout)
check(f"command {GOOD}: Darkwood.exe -force-d3d11 -screen-width 3840 -screen-height 2160\n" in good.stdout,
      "good recipe command: " + good.stdout)
check(f"flag {GOOD}: program: Darkwood.exe\n" in good.stdout, "good recipe flags the program")
check("refused" not in good.stdout, "good recipe has no refusal")

# A publisher key passes and is flagged for review.
flagged = run(FLAGGED)
check(flagged.returncode == 0, "flagged recipe exits 0: " + flagged.stdout)
check("publisher registry key, needs review" in flagged.stdout, "publisher key flagged: " + flagged.stdout)

# Each hostile file is refused, for its own reason.
expected = {
    "program-outside-folder.json": "not inside the game folder",
    "program-absolute.json": "not inside the game folder",
    "program-drive-letter.json": "not inside the game folder",
    "working-folder-outside.json": "not inside the game folder",
    "wildcard-override.json": "without a wildcard",
    "unknown-environment.json": "DXVK_CONFIG_FILE is not one Playden sets",
    "registry-other-program.json": "not one of the recipe's programs",
    "id-65-bytes.json": "at most 64 bytes",
    "id-other-app.json": "recipe_id must be community-<app id>-<slug>",
    "locked.json": "locked must be empty",
    "export-no-slug-no-title.json": "has no slug",
    "kit-not-a-version.json": "tested_kit is not a kit version",
    "env-vkd3d-log-file.json": "VKD3D_LOG_FILE is not one Playden sets",
    "env-vkd3d-shader-cache-path.json": "VKD3D_SHADER_CACHE_PATH is not one Playden sets",
    "program-system-msiexec.json": "a Windows or Wine system program",
    "program-dots-and-spaces.json": "only dots and spaces",
    "program-folder-trailing-dot.json": "ending in a dot or space",
    "working-folder-dots.json": "only dots and spaces",
    "text-title-bidi.json": "community.title must be 1-60 printable",
    "text-title-nbsp.json": "community.title must be 1-60 printable",
    "text-title-lone-surrogate.json": "community.title must be 1-60 printable",
    "text-title-bad-utf8.json": "community.title must be 1-60 printable",
    "text-notes-control.json": "community.notes must be valid UTF-8",
    "text-firmware-newline.json": "community.tested_firmware must be valid UTF-8",
    "defaults-extra-arguments.json": "defaults sets extra_arguments",
}
hostile = sorted((FIXTURES / "hostile").glob("*.json"))
check(sorted(path.name for path in hostile) == sorted(expected), "every hostile fixture has an expectation")
for path in hostile:
    result = run(path)
    check(result.returncode == 1, f"{path.name} exits 1: {result.returncode}")
    check(f"refused {path}: " in result.stdout and expected.get(path.name, "?") in result.stdout,
          f"{path.name} refused for its reason: {result.stdout}")
    check(f"ok {path}" not in result.stdout, f"{path.name} is not ok")

# A recipe shared without a name has no author and is accepted.
nameless = run(FIXTURES / "accepted" / "no-author.json")
check(nameless.returncode == 0 and "ok " in nameless.stdout, "a recipe without an author passes: " + nameless.stdout)

# The AppDefaults key of a fake system-program entry is still listed for review.
check("registry: Software\\Wine\\AppDefaults\\msiexec.exe" in run(FIXTURES / "hostile" / "program-system-msiexec.json").stdout,
      "AppDefaults key flagged")
# A launch option naming a later program passes and is flagged.
second = run(FIXTURES / "flagged" / "launch-option-second.json")
check(second.returncode == 0 and "starts the program config, not the first one" in second.stdout, "launch_option flagged: " + second.stdout)

# A recipe filed under the wrong name in a recipes folder is refused.
with tempfile.TemporaryDirectory() as directory:
    wrong = Path(directory) / "recipes" / "274520" / "darkwood.json"
    wrong.parent.mkdir(parents=True)
    wrong.write_bytes(GOOD.read_bytes())
    result = run(wrong)
    check(result.returncode == 1 and "must be recipes/274520/120hz-left-stick-pointer.json" in result.stdout,
          "misplaced file refused: " + result.stdout)
    flat = Path(directory) / "recipes" / "274520-darkwood.json"
    flat.write_bytes(GOOD.read_bytes())
    result = run(flat)
    check(result.returncode == 1 and "must be recipes/274520/" in result.stdout, "old flat layout refused: " + result.stdout)
    duplicate = Path(directory) / "copy.json"
    duplicate.write_bytes(GOOD.read_bytes())
    result = run(GOOD, duplicate)
    check(result.returncode == 1 and "is also in" in result.stdout, "the same id twice refused: " + result.stdout)
    large = Path(directory) / "large.json"
    large.write_text(" " * 17000 + GOOD.read_text())
    result = run(large)
    check(result.returncode == 1 and "larger than 16 KB" in result.stdout, "a large file refused: " + result.stdout)

# --json: one object for the run, every field CI reads.
result = run("--json", GOOD, FIXTURES / "hostile" / "wildcard-override.json")
check(result.returncode == 1, "json run with a refused file exits 1")
try:
    report = json.loads(result.stdout)
except ValueError as error:
    report = {}
    failures.append(f"--json output is not JSON: {error}: {result.stdout}")
if report:
    check(report["ok"] is False and len(report["files"]) == 2, "json lists both files")
    first, second = report["files"]
    check(first["ok"] is True and first["revision"] == revision and first["app_id"] == 274520 and
          first["slug"] == "120hz-left-stick-pointer" and first["title"] == "120 Hz, pointer on left stick" and
          first["status"] == "works" and first["author"] == "TodorPenchev" and first["contributors"] == [] and
          first["tested_kit"] == "0.12" and first["min_kit"] == "" and first["tested_firmware"] == "13.60" and
          first["tested_date"] == "2026-10-22" and first["refusals"] == [] and
          "program: Darkwood.exe" in first["flags"] and
          first["command"] == "Darkwood.exe -force-d3d11 -screen-width 3840 -screen-height 2160", f"json fields: {first}")
    check(second["ok"] is False and any("wildcard" in item for item in second["refusals"]), f"json refusal: {second}")

# --json is valid UTF-8 JSON even for a file with invalid bytes or control characters.
hostile_json = subprocess.run([CHECKER, "--json", *map(str, sorted((FIXTURES / "hostile").glob("text-*.json")))],
                              capture_output=True)
try:
    decoded = json.loads(hostile_json.stdout.decode("utf-8"))
    check(len(decoded["files"]) == 6 and decoded["ok"] is False, "json of hostile text files")
except (ValueError, UnicodeDecodeError) as error:
    failures.append(f"--json of hostile text is not valid UTF-8 JSON: {error}")
raw = subprocess.run([CHECKER, "--json", str(FIXTURES / "hostile" / "text-title-bad-utf8.json")], capture_output=True)
try:
    text = json.loads(raw.stdout.decode("utf-8"))["files"][0]["title"]
    check(text == "a\ufffdb", f"invalid byte becomes U+FFFD: {text!r}")
except (ValueError, UnicodeDecodeError, KeyError) as error:
    failures.append(f"--json of invalid bytes: {error}")
# Log lines can't be forged with a new line in a field.
with tempfile.TemporaryDirectory() as directory:
    forged = Path(directory) / "x\nok.json"
    forged.write_text("{")
    out = run(forged).stdout
    check(out.count("\n") == 1 and out.startswith("refused "), "a new line in a path is escaped: " + repr(out))

# Usage errors.
check(run().returncode == 2, "no files is a usage error")
check(run("--bogus", GOOD).returncode == 2, "an unknown option is a usage error")

if failures:
    for failure in failures:
        print("FAIL:", failure, file=sys.stderr)
    sys.exit(1)
print("community_recipe_check tests passed")
