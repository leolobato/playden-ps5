#!/usr/bin/env python3
import http.server
import os
import pathlib
import ssl
import subprocess
import sys
import threading
import time

# Streamed bodies: byte i is (i * 31 + 7) & 0xff, so the pattern repeats every 256 bytes.
PATTERN = bytes((index * 31 + 7) & 0xFF for index in range(256))
STREAM_BLOCK = 64 * 1024


def pattern(length):
    return (PATTERN * (length // len(PATTERN) + 1))[:length]


class Server(http.server.ThreadingHTTPServer):
    accepted_connections = 0

    def get_request(self):
        connection, address = super().get_request()
        self.accepted_connections += 1
        connection.fixture_id = self.accepted_connections
        return connection, address


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, _format, *_args):
        pass

    def _reply(self, status, body, keep=False, marker=None, slow=False):
        self.send_response(status)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "keep-alive" if keep else "close")
        self.send_header("X-Fixture-Connection", str(self.connection.fixture_id))
        self.send_header("X-Fixture-Accepted", str(self.server.accepted_connections))
        if marker:
            self.send_header("X-Fixture-Marker", marker)
        self.end_headers()
        try:
            if slow:
                self.wfile.write(body[:1])
                self.wfile.flush()
                time.sleep(2.0)
                body = body[1:]
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError, ssl.SSLEOFError):
            pass

    def _stream(self, status, length, blocks):
        """Sends a Content-Length of `length`, then each (bytes, pause) in `blocks`, flushed one by one."""
        self.send_response(status)
        self.send_header("Content-Length", str(length))
        self.send_header("Connection", "close")
        self.end_headers()
        try:
            for block, pause in blocks:
                self.wfile.write(block)
                self.wfile.flush()
                if pause:
                    time.sleep(pause)
        except (BrokenPipeError, ConnectionResetError, ssl.SSLEOFError):
            pass

    def do_GET(self):
        if self.path.startswith("/stream/"):
            body = pattern(int(self.path[len("/stream/"):]))
            self._stream(200, len(body), ((body[start:start + STREAM_BLOCK], 0)
                                          for start in range(0, len(body), STREAM_BLOCK)))
        elif self.path == "/stream-slow":
            body = pattern(2 * STREAM_BLOCK)
            self._stream(200, len(body), ((body[:STREAM_BLOCK], 3.0), (body[STREAM_BLOCK:], 0)))
        elif self.path == "/stall":
            body = pattern(2 * STREAM_BLOCK)
            self._stream(200, len(body), ((body[:STREAM_BLOCK], 3.0), (body[STREAM_BLOCK:], 0)))
        elif self.path == "/trickle":
            body = pattern(25 * 4096)
            self._stream(200, len(body), ((body[start:start + 4096], 0.1) for start in range(0, len(body), 4096)))
        elif self.path == "/stream-404":
            body = b'{"error":"not_found"}'
            self.send_response(404)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(body)
        elif self.path.startswith("/stream-403/"):
            body = pattern(int(self.path[len("/stream-403/"):]))
            self._stream(403, len(body), ((body[start:start + STREAM_BLOCK], 0)
                                          for start in range(0, len(body), STREAM_BLOCK)))
        elif self.path.startswith("/keep/"):
            if self.path == "/keep/slow":
                self._reply(200, b"late", keep=True, marker="slow", slow=True)
            elif self.path == "/keep/large":
                self._reply(200, b"x" * 4096, keep=True, marker="large")
            elif self.path == "/keep/close":
                self._reply(200, b"fixture-ok", keep=True)
                self.close_connection = True
            else:
                self._reply(200, b"fixture-ok", keep=True,
                            marker="first" if self.path == "/keep/marked" else None)
        elif self.path == "/stats/connections":
            self._reply(200, str(self.server.accepted_connections).encode("ascii"), keep=True)
        elif self.path == "/ok":
            body = b"fixture-ok"
            self.send_response(200)
            self.send_header("X-ErEsUlT", "42")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(body)
        elif self.path == "/slow":
            time.sleep(2.0)
            self._reply(200, b"late")
        elif self.path == "/large":
            self._reply(200, b"x" * 4096)
        elif self.path == "/large-headers":
            self.send_response(200)
            for index in range(65):
                self.send_header(f"X-Fixture-{index}", "bounded")
            self.send_header("Content-Length", "0")
            self.end_headers()
        elif self.path == "/redirect":
            self.send_response(302)
            self.send_header("Location", "/ok")
            self.send_header("Content-Length", "0")
            self.end_headers()
        else:
            self._reply(404, b"missing")

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length)
        marker = self.headers.get("X-Playden-Test", "").encode("ascii")
        self._reply(201, marker + b":" + body, keep=self.path.startswith("/keep/"))

    do_PUT = do_POST


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: http_fixture_server.py TEST_BINARY FIXTURE_DIR")
    fixture_dir = pathlib.Path(sys.argv[2]).resolve()
    server = Server(("127.0.0.1", 0), Handler)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(fixture_dir / "server.pem", fixture_dir / "server-key.pem")
    server.socket = context.wrap_socket(server.socket, server_side=True)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    # A TLS 1.2-only server, like the Alibaba Cloud storage hosts some Steam Cloud saves come from (#155). With an
    # RSA key, OpenSSL (1.1.1 and later) signs the ServerKeyExchange with rsa_pss_rsae_* when the client offers it,
    # which a client that offers TLS 1.3 always does. LibreSSL picks rsa_pkcs1 instead and would not test this.
    if not ssl.OPENSSL_VERSION.startswith("OpenSSL") or ssl.OPENSSL_VERSION_INFO < (1, 1, 1):
        raise SystemExit(f"http_fixture_server.py needs a Python built with OpenSSL 1.1.1 or later, not "
                         f"{ssl.OPENSSL_VERSION} ({sys.executable}); configure with -DPython3_EXECUTABLE=...")
    tls12 = Server(("127.0.0.1", 0), Handler)
    tls12_context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    tls12_context.maximum_version = ssl.TLSVersion.TLSv1_2
    tls12_context.load_cert_chain(fixture_dir / "server.pem", fixture_dir / "server-key.pem")
    tls12.socket = tls12_context.wrap_socket(tls12.socket, server_side=True)
    tls12_thread = threading.Thread(target=tls12.serve_forever, daemon=True)
    tls12_thread.start()
    plain = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    plain_thread = threading.Thread(target=plain.serve_forever, daemon=True)
    plain_thread.start()
    try:
        base_url = f"https://localhost:{server.server_port}"
        plain_url = f"http://localhost:{plain.server_port}"
        tls12_url = f"https://localhost:{tls12.server_port}"
        environment = os.environ.copy()
        environment["HTTPS_PROXY"] = "http://127.0.0.1:1"
        environment["ALL_PROXY"] = "http://127.0.0.1:1"
        environment["NO_PROXY"] = ""
        result = subprocess.run(
            [sys.argv[1], base_url, str(fixture_dir / "ca.pem"),
             str(fixture_dir / "wrong-ca.pem"), plain_url, tls12_url],
            env=environment,
            check=False,
        )
        return result.returncode
    finally:
        server.shutdown()
        server.server_close()
        thread.join()
        plain.shutdown()
        plain.server_close()
        plain_thread.join()
        tls12.shutdown()
        tls12.server_close()
        tls12_thread.join()


if __name__ == "__main__":
    raise SystemExit(main())
