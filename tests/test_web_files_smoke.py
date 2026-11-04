#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Drives the Files tab's API against a real server, as the page does: pair, list, upload, replace, restore, zip, delete."""
import http.client
import io
import json
import os
import subprocess
import sys
import tempfile
import urllib.parse
import zipfile


def main(server_binary):
    with tempfile.TemporaryDirectory(dir=os.path.realpath(tempfile.gettempdir())) as folder:
        server = subprocess.Popen([server_binary, folder], stdout=subprocess.PIPE, text=True)
        try:
            port = int(server.stdout.readline().strip().split("=")[1])
            run(port, server)
        finally:
            server.kill()
            server.wait()
    print("web_files_smoke: ok")


def run(port, server):
    cookie = {}

    def call(method, route, body=None, action=True, **query):
        target = route + ("?" + urllib.parse.urlencode(query) if query else "")
        connection = http.client.HTTPConnection("127.0.0.1", port, timeout=20)
        headers = {}
        if action:
            headers["X-Playden-Action"] = "files"
        if "value" in cookie:
            headers["Cookie"] = cookie["value"]
        connection.request(method, target, body=body, headers=headers)
        response = connection.getresponse()
        data = response.read()
        result = (response.status, data, dict(response.getheaders()))
        connection.close()
        return result

    def ok(result):
        assert 200 <= result[0] < 300, result[:2]
        return json.loads(result[1]) if result[2].get("Content-Type", "").startswith("application/json") else result[1]

    # Unpaired.
    assert call("GET", "/api/files/games")[0] == 401
    assert json.loads(call("GET", "/api/pair/status")[1]) == {"paired": False}

    # Pair with the code the TV shows.
    ok(call("POST", "/api/pair/start"))
    code = server.stdout.readline().strip().split("=")[1]
    result = call("POST", "/api/pair/confirm", code=code)
    ok(result)
    cookie["value"] = result[2]["Set-Cookie"].split(";")[0]
    assert json.loads(call("GET", "/api/pair/status")[1]) == {"paired": True}

    # The games.
    games = ok(call("GET", "/api/files/games"))["games"]
    assert {game["id"] for game in games} == {"steam-12210", "local-3fa9c1", "steam-1091500", "steam-274520"}
    assert next(game for game in games if game["id"] == "steam-1091500")["running"] is True

    # A game playing in place from USB: its redistributable folders, by Play's rules, and what it needs.
    wolverine = ok(call("GET", "/api/files/game", game="local-3fa9c1", redist=1))
    assert wolverine["physx_needed"] == "2.8.1"
    assert [folder["path"] for folder in wolverine["redist"]] == ["Support"]
    # An unplugged drive leaves the saves.
    cyberpunk = ok(call("GET", "/api/files/game", game="steam-1091500"))
    assert "game" not in [root["id"] for root in cyberpunk["roots"]] and "user" in [root["id"] for root in cyberpunk["roots"]]
    # An uninstalled game keeps its saves.
    darkwood = ok(call("GET", "/api/files/list", game="steam-274520", root="user", path="AppData/LocalLow/Acid Wizard Studio/Darkwood"))
    assert [entry["name"] for entry in darkwood["entries"]] == ["save.dat"]

    # The GTA IV save a player asked about: found, and downloaded.
    place = dict(game="steam-12210", root="user")
    save_folder = "Documents/Rockstar Games/GTA IV/Profiles/7A3C91F0"
    listing = ok(call("GET", "/api/files/list", path=save_folder, **place))
    assert [entry["name"] for entry in listing["entries"]] == ["cfg.set", "SGTA400"]
    original = ok(call("GET", "/api/files/download", path=save_folder + "/SGTA400", **place))
    assert len(original) == 512 * 1024

    # Replace it, see the previous version, restore it.
    path = save_folder + "/SGTA400"
    assert call("POST", "/api/files/upload", body=b"new", path=path, **place)[0] == 409
    assert ok(call("POST", "/api/files/upload", body=b"new", path=path, overwrite=1, **place))["replaced"] is True
    assert ok(call("GET", "/api/files/download", path=path, **place)) == b"new"
    entry = next(e for e in ok(call("GET", "/api/files/list", path=save_folder, **place))["entries"] if e["name"] == "SGTA400")
    assert entry["previous"]["size"] == 512 * 1024
    assert ok(call("GET", "/api/files/download", path=path, previous=1, **place)) == original
    ok(call("POST", "/api/files/restore", path=path, **place))
    assert ok(call("GET", "/api/files/download", path=path, **place)) == original

    # A large upload, in one request.
    large = os.urandom(8 * 1024 * 1024)
    ok(call("POST", "/api/files/upload", body=large, path="Documents/large.bin", **place))
    assert ok(call("GET", "/api/files/download", path="Documents/large.bin", **place)) == large
    ok(call("POST", "/api/files/delete", path="Documents/large.bin", **place))
    assert call("GET", "/api/files/download", path="Documents/large.bin", **place)[0] == 404

    # A folder as a zip that opens.
    archive = zipfile.ZipFile(io.BytesIO(ok(call("GET", "/api/files/zip", path=save_folder, **place))))
    assert archive.testzip() is None and "7A3C91F0/cfg.set" in archive.namelist()

    # Read-only and hidden in the game folder.
    game_place = dict(game="steam-12210", root="game")
    names = [entry["name"] for entry in ok(call("GET", "/api/files/list", **game_place))["entries"]]
    assert ".playden-install.json" not in names and "steam_settings" in names
    assert call("POST", "/api/files/upload", body=b"x", path="steam_settings/configs.user.ini", overwrite=1, **game_place)[0] == 403

    # The editor's round trip.
    text = ok(call("GET", "/api/files/text", path="autoexec.cfg", **game_place))
    ok(call("POST", "/api/files/upload", body=b"fps_max 120\n", path="autoexec.cfg", if_sha256=text["sha256"], **game_place))
    assert call("POST", "/api/files/upload", body=b"stale", path="autoexec.cfg", if_sha256=text["sha256"], **game_place)[0] == 409

    # PhysX for a game playing from USB: a new _Redist folder, then the installer; the page now names it.
    wolverine_place = dict(game="local-3fa9c1", root="game")
    ok(call("POST", "/api/files/mkdir", path="_Redist", **wolverine_place))
    # A folder upload: each folder, then its files. Sending it again finds them there.
    for _ in range(2):
        ok(call("POST", "/api/files/mkdir", path="Mods/Big/textures", parents=1, **wolverine_place))
        ok(call("POST", "/api/files/upload", body=b"mod", path="Mods/Big/textures/a.dds", overwrite=1, **wolverine_place))
    tree = ok(call("GET", "/api/files/list", path="Mods/Big/textures", **wolverine_place))
    assert [entry["name"] for entry in tree["entries"]] == ["a.dds"] and "previous" in tree["entries"][0]
    assert call("POST", "/api/files/mkdir", path="Mods/Big/textures/a.dds", parents=1, **wolverine_place)[0] == 409
    ok(call("POST", "/api/files/upload", body=b"msi", path="_Redist/PhysX_9.09.msi", **wolverine_place))
    wolverine = ok(call("GET", "/api/files/game", game="local-3fa9c1", redist=1))
    assert next(folder for folder in wolverine["redist"] if folder["path"] == "_Redist")["physx_installer"] == "_Redist/PhysX_9.09.msi"


if __name__ == "__main__":
    main(sys.argv[1])
