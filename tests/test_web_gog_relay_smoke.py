#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Posts GOG sign-in addresses to a real server as the phone page does, then checks that web.log kept the posts and
their statuses but no token, no pasted address and no code."""
import http.client
import json
import os
import subprocess
import sys
import tempfile
import time

PREFIX = "https://embed.gog.com/on_login_success?origin=client&code="
GOOD_CODE = "FixtureGoodCode"
BAD_CODE = "FixtureRefusedCode"
WRONG_ADDRESS = "https://www.gog.com/en/FixtureWrongPage"
WRONG_TOKEN = "0123456789abcdef0123456789abcdef"


def main(server_binary):
    with tempfile.TemporaryDirectory(dir=os.path.realpath(tempfile.gettempdir())) as folder:
        server = subprocess.Popen([server_binary, folder], stdout=subprocess.PIPE, text=True)
        try:
            port = int(server.stdout.readline().strip().split("=")[1])
            token = server.stdout.readline().strip().split("=")[1]
            run(port, token, os.path.join(folder, "web.log"))
        finally:
            server.kill()
            server.wait()
    print("web_gog_relay_smoke: ok")


def call(port, method, route, body=None, action="gog-sign-in"):
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=20)
    headers = {"Origin": "http://127.0.0.1:%d" % port}
    if action:
        headers["X-Playden-Action"] = action
    if body is not None:
        headers["Content-Type"] = "application/json"
        body = json.dumps(body).encode()
    connection.request(method, route, body=body, headers=headers)
    response = connection.getresponse()
    data = response.read()
    result = (response.status, data, dict(response.getheaders()))
    connection.close()
    return result


def post(port, token, address):
    return call(port, "POST", "/api/gog/sign-in", {"token": token, "address": address})


def run(port, token, web_log):
    assert len(token) == 32

    page = call(port, "GET", "/gog")
    assert page[0] == 200 and page[2].get("Referrer-Policy") == "no-referrer", page[:2]
    assert b"client_id=FIXTURE" in page[1] and token.encode() not in page[1]

    status, data, headers = post(port, WRONG_TOKEN, PREFIX + GOOD_CODE)
    assert status == 403, (status, data)
    status, data, headers = post(port, token, WRONG_ADDRESS)
    assert status == 422 and "token" not in json.loads(data), (status, data)
    status, data, headers = post(port, token, PREFIX + BAD_CODE)
    answer = json.loads(data)
    assert status == 422 and headers.get("Cache-Control") == "no-store", (status, data)
    renewed = answer["token"]
    assert len(renewed) == 32 and renewed != token
    status, data, headers = post(port, token, PREFIX + GOOD_CODE)
    assert status == 403, (status, data)
    status, data, headers = post(port, renewed, PREFIX + GOOD_CODE)
    assert status == 200 and json.loads(data) == {"signed_in": True, "name": "Fixture Player"}, (status, data)

    # The server writes a request's line after it answered it.
    expected = ["403", "422", "422", "403", "200"]
    deadline = time.monotonic() + 5
    while True:
        with open(web_log, encoding="utf-8") as file:
            text = file.read()
        lines = [line for line in text.splitlines() if line.startswith("POST /api/gog/sign-in ")]
        if len(lines) >= len(expected) or time.monotonic() > deadline:
            break
        time.sleep(0.05)
    assert [line.split()[2] for line in lines] == expected, lines
    assert all(line.split()[4] == "127.0.0.1" for line in lines), lines
    assert "GET /gog 200" in text
    relay_lines = [line for line in text.splitlines() if line.startswith("gog relay ")]
    assert [line.split("=")[1] for line in relay_lines] == ["wrong_token", "wrong_shape", "failed", "wrong_token", "signed_in"], relay_lines
    for secret in (token, renewed, WRONG_TOKEN, GOOD_CODE, BAD_CODE, "FixtureWrongPage", "embed.gog.com", "on_login_success"):
        assert secret not in text, secret


if __name__ == "__main__":
    main(sys.argv[1])
