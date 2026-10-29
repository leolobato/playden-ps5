# SPDX-License-Identifier: GPL-3.0-or-later
import importlib.util
from pathlib import Path
import unittest
from unittest.mock import MagicMock, patch
import ftplib

spec = importlib.util.spec_from_file_location("download_speed_report", Path(__file__).resolve().parents[1] / "tools/download_speed_report.py")
report = importlib.util.module_from_spec(spec)
spec.loader.exec_module(report)


class DownloadSpeedReportTest(unittest.TestCase):
    def test_completed_run_uses_end_and_filters(self):
        text = """2026-10-12T10:00:00Z app=440 step=trace result=access detail=
2026-10-12T10:00:01Z app=440 step=stats result=shape detail=files_total=2 download_total=4000000 chunks_pending=3 files_pending=2 bytes_pending=4000000
2026-10-12T10:00:03Z app=440 step=stats result=end detail=dl_ms=2000 cb=4000000 fc=2 fetch=1000 proc=200 create=400 write=200 jrnl=100 back=0 req=3 conn=3 retry=0 slots=1 reuse=0
2026-10-12T10:00:03Z app=440 step=install result=none detail=
"""
        rows = report.parse_runs(text)
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["outcome"], "none")
        self.assertEqual(rows[0]["files_total"], 2)
        self.assertEqual(rows[0]["mb_s"], 2.0)
        self.assertEqual(rows[0]["fetch_pct"], 50.0)
        self.assertEqual(rows[0]["creation_floor_ms"], 45.0)
        self.assertEqual(report.filter_runs(rows, app=42), [])
        self.assertEqual(report.filter_runs(rows, since="2026-10-12T10:01:00Z"), [])

    def test_force_closed_progress_stays_separate_from_cancellation(self):
        rows = report.parse_runs("""2026-10-12T10:00:00Z app=1 step=trace result=access detail=
2026-10-12T10:05:00Z app=1 step=stats result=progress detail=dl_ms=300000 cb=600000000 fetch=250000 fc=4
2026-10-12T10:06:00Z app=1 step=trace result=access detail=
2026-10-12T10:06:01Z app=1 step=stats result=end detail=dl_ms=1 cb=0 fc=0
2026-10-12T10:06:01Z app=1 step=install result=canceled detail=
""")
        self.assertEqual(len(rows), 2)
        self.assertEqual(rows[0]["outcome"], "interrupted")
        self.assertEqual(rows[0]["download_ms"], 300000)
        self.assertEqual(rows[0]["mb_s"], 2.0)
        self.assertEqual(rows[1]["outcome"], "canceled")

    def test_pre_access_failures_and_interleaved_apps(self):
        rows = report.parse_runs("""2026-10-12T10:00:00Z app=1 step=entitlements result=rejected detail=
2026-10-12T10:00:01Z app=2 step=app_info result=network detail=
2026-10-12T10:00:02Z app=3 step=trace result=access detail=
2026-10-12T10:00:03Z app=4 step=trace result=access detail=
2026-10-12T10:00:04Z app=3 step=install result=network detail=
2026-10-12T10:00:05Z app=4 step=install result=none detail=
""")
        self.assertEqual([r["outcome"] for r in rows], ["entitlements:rejected", "app_info:network", "network", "none"])
        self.assertIsNone(rows[0]["mb_s"])

    def test_no_windows_depots_is_a_failed_prerequisite_run(self):
        rows = report.parse_runs(
            "2026-10-12T10:00:00Z app=440 step=depots result=none detail=no Windows depots\n")
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["app"], 440)
        self.assertEqual(rows[0]["outcome"], "depots:none")
        self.assertIsNone(rows[0]["download_ms"])
        self.assertIsNone(rows[0]["compressed_bytes"])
        self.assertIsNone(rows[0]["mb_s"])
        self.assertEqual(report.parse_runs(
            "2026-10-12T10:00:00Z app=440 step=app_info result=none detail=\n"
            "2026-10-12T10:00:00Z app=440 step=entitlements result=none detail=\n"), [])

    def test_server_labels_and_split_throughput_lines(self):
        rows = report.parse_runs("""2026-10-12T10:00:00Z app=440 step=trace result=access detail=
2026-10-12T10:00:01Z app=440 step=servers result=listed detail=n=3 https=3 pref=chosen first=fastly@0 order=fastly:2,akamai:1,cache:1
2026-10-12T10:00:03Z app=440 step=stats result=end detail=dl_ms=2000 cb=4000000
2026-10-12T10:00:03Z app=440 step=stats result=servers detail=fastly@0:req=4,fail=0,mb=4,ms=2000 akamai@1:req=1,fail=1,mb=0,ms=0
2026-10-12T10:00:03Z app=440 step=stats result=servers detail=cache@2:req=2,fail=0,mb=3,ms=1000
2026-10-12T10:00:03Z app=440 step=install result=none detail=
""")
        self.assertEqual(rows[0]["first_server"], "fastly@0")
        self.assertEqual(rows[0]["server_mb_s"], "fastly@0:2.00 akamai@1:- cache@2:3.00")
        output = report.io.StringIO()
        report.write_csv(rows, output)
        self.assertIn("first_server,server_mb_s", output.getvalue())
        self.assertIn("fastly@0:2.00", output.getvalue())

    def test_missing_rotated_log_is_not_requested(self):
        ftp = MagicMock()
        ftp.__enter__.return_value = ftp
        ftp.retrlines.side_effect = lambda command, callback: callback(
            "-rw-r--r-- 1 root root 10 Oct 12 10:00 install.log")
        ftp.retrbinary.side_effect = lambda command, callback, size: callback(b"a log\n")
        with patch.object(report.ftplib, "FTP", return_value=ftp):
            self.assertEqual(report.fetch_logs("console.invalid", 2120), "a log\n")
        self.assertEqual(ftp.retrbinary.call_count, 1)
        self.assertIn("install.log", ftp.retrbinary.call_args.args[0])
        self.assertNotIn("install.log.1", ftp.retrbinary.call_args.args[0])

    def test_present_log_transfer_failure_is_reported(self):
        ftp = MagicMock()
        ftp.__enter__.return_value = ftp
        ftp.retrlines.side_effect = lambda command, callback: callback(
            "-rw-r--r-- 1 root root 10 Oct 12 10:00 install.log")
        ftp.retrbinary.side_effect = ftplib.error_temp("451 transfer failed")
        with patch.object(report.ftplib, "FTP", return_value=ftp):
            with self.assertRaises(ftplib.error_temp):
                report.fetch_logs("console.invalid", 2120)

    def test_rotation_partial_and_zero_duration_do_not_invent_measurements(self):
        rows = report.parse_runs("""2026-10-12T10:00:00Z app=1 step=stats result=progress detail=dl_ms=0 cb=0 fc=0 req=2 conn=2
2026-10-12T10:00:01Z app=1 step=install result=storage detail=
""")
        self.assertEqual(rows[0]["outcome"], "storage")
        self.assertIsNone(rows[0]["mb_s"])
        self.assertIsNone(rows[0]["files_total"])
        self.assertEqual(rows[0]["sample"], "progress")


if __name__ == "__main__":
    unittest.main()
