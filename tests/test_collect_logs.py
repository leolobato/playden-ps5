import http.server
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))

import collect_logs  # noqa: E402


def title_log(app, account='Alice', time=1791601941):
    return (f'== session-1.log\nPW_REPORT/1 build=abc profile=playden-steam-{app} cycle=0 pid=5 time={time}\n'
            f'REC seq=561 WINE trace:process:SetEnvironmentVariableW (L"SteamAppUser" L"{account}")\n'
            f'REC seq=562 WINE trace:process:SetEnvironmentVariableW (L"SteamUser" L"{account}")\n')


class CollectLogsTests(unittest.TestCase):
    def test_picks_the_title_of_the_asked_app(self):
        logs = {'FAKE78001': title_log(220), 'FAKE78002': title_log(40800)}
        self.assertEqual(collect_logs.pick_title(logs, app=40800), 'FAKE78002')
        self.assertIsNone(collect_logs.pick_title(logs, app=1))

    def test_picks_the_asked_title_or_the_newest(self):
        logs = {'FAKE78001': title_log(220), 'FAKE78002': title_log(40800)}
        self.assertEqual(collect_logs.pick_title(logs, title='FAKE78001'), 'FAKE78001')
        self.assertIsNone(collect_logs.pick_title(logs, title='FAKE78009'))
        newer = {'FAKE78001': title_log(220, time=100), 'FAKE78002': title_log(40800, time=200)}
        self.assertEqual(collect_logs.pick_title(newer), 'FAKE78002')
        self.assertIsNone(collect_logs.pick_title({}))

    def test_lists_games_by_last_run(self):
        logs = {'FAKE78001': title_log(220, time=100),
                'FAKE78002': title_log(40800, time=300) + 'PW_REPORT/1 build=abc profile=x time=50\n',
                'FAKE78003': 'no report yet\n'}
        rows = collect_logs.games(logs, {'FAKE78001': 'Half-Life 2'})
        self.assertEqual([row['title'] for row in rows], ['FAKE78002', 'FAKE78001', 'FAKE78003'])
        self.assertEqual(rows[0]['last_run'], 300)
        self.assertEqual(rows[1]['name'], 'Half-Life 2')
        self.assertEqual(rows[2]['last_run'], 0)
        table = collect_logs.game_table(rows)
        self.assertIn('1. unknown game  (FAKE78002, app 40800', table)
        self.assertIn('Half-Life 2  (FAKE78001, app 220', table)
        self.assertIn('last run never', table)

    def test_title_names_lose_any_store_suffix(self):
        self.assertEqual(collect_logs.without_store_suffix('Half-Life 2 (Steam)'), 'Half-Life 2')
        self.assertEqual(collect_logs.without_store_suffix('Delores (Epic)'), 'Delores')
        self.assertEqual(collect_logs.without_store_suffix('Gothic (GOG)'), 'Gothic')
        self.assertEqual(collect_logs.without_store_suffix('Imported'), 'Imported')
        self.assertEqual(collect_logs.without_store_suffix('Game (Steam) (GOG)'), 'Game (Steam)')
        self.assertEqual(collect_logs.without_store_suffix(' (Steam)'), ' (Steam)')
        self.assertIsNone(collect_logs.without_store_suffix(None))

    def test_redacts_the_account_name_and_steam_ids(self):
        text = title_log(220) + 'cloud user=alice id=76561198000000001 path=C:/users/Alice/saves\n'
        names = collect_logs.account_names([text])
        self.assertEqual(names, {'Alice'})
        redacted = collect_logs.redact(text, names)
        self.assertNotIn('alice', redacted.lower())
        self.assertNotIn('76561198000000001', redacted)
        self.assertIn('<steam-user>', redacted)
        self.assertIn('<steam-id>', redacted)

    def test_finds_the_account_name_in_any_log(self):
        stopped_early = '== session-1.log\nPW_REPORT/1 profile=playden-steam-220\nopen C:/users/Alice failed\n'
        names = collect_logs.account_names([stopped_early, title_log(40800)])
        self.assertNotIn('alice', collect_logs.redact(stopped_early, names).lower())

    def test_reports_the_latest_firmware(self):
        self.assertEqual(collect_logs.firmware(['console_firmware=13.40 x', 'a\nconsole_firmware=13.60 y']), '13.60')
        self.assertIsNone(collect_logs.firmware(['nothing']))

    def test_reports_the_kit_runtime_not_its_folder_id(self):
        manifests = {'old-kit': {'kit_id': 'old-kit', 'runtime': {'artifact_revision': 'f729534-kit-0.12'},
                                 'capabilities': ['dxvk', 'wine-mono']},
                     'playden-kit': {'kit_id': 'playden-kit', 'runtime': {'artifact_revision': 'ce28942-kit-0.13'},
                                     'capabilities': ['dxvk']}}
        folders, fetch = collect_logs.folders, collect_logs.fetch
        collect_logs.folders = lambda ftp, path: ['old-kit']
        collect_logs.fetch = lambda ftp, path: json.dumps(manifests[path.split('/')[-2]]).encode()
        try:
            self.assertEqual(collect_logs.active_kit(None),
                             {'kit runtime': 'f729534-kit-0.12', 'kit capabilities': 'dxvk, wine-mono'})
            # A kit left under the id it had before is not the one Playden uses.
            collect_logs.folders = lambda ftp, path: ['.previous-x', 'old-kit', 'playden-kit']
            self.assertEqual(collect_logs.active_kit(None), {'kit runtime': 'ce28942-kit-0.13', 'kit capabilities': 'dxvk',
                                                             'other kit folders': 'old-kit'})
            collect_logs.folders = lambda ftp, path: []
            self.assertEqual(collect_logs.active_kit(None), {'kit runtime': None})
        finally:
            collect_logs.folders, collect_logs.fetch = folders, fetch


class StubPlayden(http.server.BaseHTTPRequestHandler):
    """Playden's web access as the --http path sees it: the log index and a report zip."""
    index = {'games': [{'title': 'FAKE78002', 'app': 40800, 'name': None, 'last_run': 300, 'size': 10, 'files': []},
                       {'title': 'FAKE78001', 'app': 220, 'name': 'Half-Life 2', 'last_run': 100, 'size': 10, 'files': []}],
             'playden': [{'name': 'runtime.log', 'changed': 1, 'size': 5}]}
    requests = []

    def do_GET(self):
        StubPlayden.requests.append(self.path)
        if self.path == '/api/logs':
            body, kind = json.dumps(self.index).encode(), 'application/json'
        elif self.path.startswith('/api/report.zip'):
            buffer = io.BytesIO()
            with zipfile.ZipFile(buffer, 'w', zipfile.ZIP_STORED) as archive:
                archive.writestr('summary.md', '| Field | Value |\n| --- | --- |\n| asked | ' + self.path + ' |\n')
                archive.writestr('logs/runtime.log', 'console_firmware=4.03\n')
            body, kind = buffer.getvalue(), 'application/zip'
        else:
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header('Content-Type', kind)
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


class CollectLogsHttpTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), StubPlayden)
        threading.Thread(target=cls.server.serve_forever, daemon=True).start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()

    def run_tool(self, *extra):
        StubPlayden.requests = []
        return subprocess.run([sys.executable, '-I', str(ROOT / 'tools' / 'collect_logs.py'), '--http', '--host', '127.0.0.1',
                               '--port', str(self.server.server_address[1]), *extra],
                              capture_output=True, text=True, stdin=subprocess.DEVNULL, timeout=60)

    def report(self, *extra):
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory) / 'report.zip'
            result = self.run_tool('--out', str(out), *extra)
            self.assertEqual(result.returncode, 0, result.stderr)
            with zipfile.ZipFile(out) as archive:
                self.assertEqual(archive.namelist(), ['summary.md', 'logs/runtime.log'])
            return result, StubPlayden.requests

    def test_lists_the_games_from_the_index(self):
        result = self.run_tool('--list')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('1. unknown game  (FAKE78002, app 40800', result.stdout)
        self.assertIn('Half-Life 2  (FAKE78001, app 220', result.stdout)
        self.assertEqual(StubPlayden.requests, ['/api/logs'])

    def test_downloads_the_newest_game_without_a_terminal(self):
        result, requests = self.report()
        self.assertEqual(requests, ['/api/logs', '/api/report.zip?title=FAKE78002'])
        self.assertIn('| asked | /api/report.zip?title=FAKE78002 |', result.stdout)
        self.assertIn('Wrote ', result.stdout)

    def test_picks_by_title_app_or_all(self):
        self.assertEqual(self.report('--title', 'FAKE78001')[1][-1], '/api/report.zip?title=FAKE78001')
        self.assertEqual(self.report('--app', '220')[1][-1], '/api/report.zip?title=FAKE78001')
        self.assertEqual(self.report('--all-titles')[1][-1], '/api/report.zip?title=all')
        result, requests = self.report('--app', '1')
        self.assertEqual(requests[-1], '/api/report.zip?title=none')
        self.assertIn('no game log found for app 1', result.stderr)

    def test_refuses_no_redact_and_says_when_playden_is_closed(self):
        result = self.run_tool('--no-redact')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('always redacted', result.stderr)
        closed = subprocess.run([sys.executable, '-I', str(ROOT / 'tools' / 'collect_logs.py'), '--http', '--host',
                                 '127.0.0.1', '--port', '9', '--list'], capture_output=True, text=True, timeout=60)
        self.assertNotEqual(closed.returncode, 0)
        self.assertIn('Is Playden open on the PS5', closed.stderr)


if __name__ == '__main__':
    unittest.main()
