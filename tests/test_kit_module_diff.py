import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / 'tools/kit_module_diff.py'


class KitModuleDiffTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        base = Path(self.directory.name)
        self.work, self.kit = base / 'work', base / 'kit'
        # A fake strip, like MinGW's: drops everything after the first "|" (debug sections) and stamps the file
        # with SOURCE_DATE_EPOCH ("@<time>" at the end replaces any earlier one). Without SOURCE_DATE_EPOCH MinGW's
        # strip stamps the current time; this one fails, so a comparison that leaves it out can't pass by luck.
        self.strip = base / 'strip'
        self.strip.write_text('#!/bin/sh\n[ "$1" = -s ] || exit 9\n[ -n "$SOURCE_DATE_EPOCH" ] || exit 8\n'
                              'body=$(tr "|" "\\n" < "$2" | head -n 1 | sed "s/@.*//")\n'
                              'printf "%s@%s" "$body" "$SOURCE_DATE_EPOCH" > "$2"\n')
        self.strip.chmod(0o755)

    def tearDown(self):
        self.directory.cleanup()

    def write(self, path, data):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)

    def run_tool(self, *extra):
        result = subprocess.run([sys.executable, '-I', str(TOOL), str(self.work), str(self.kit), '--json',
                                 '--strip-i386', str(self.strip), '--strip-x86_64', str(self.strip), *extra],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        return json.loads(result.stdout)

    def test_compares_prx_bytes_and_stripped_pe(self):
        self.write(self.work / 'prx/sce_module/ntdll.prx', b'ntdll-new')
        self.write(self.kit / 'x86_64-unix/ntdll.prx', b'ntdll-old')
        self.write(self.work / 'prx/sce_module/win32u.prx', b'win32u')
        self.write(self.kit / 'x86_64-unix/win32u.prx', b'win32u')
        # Same code, debug info only in the build, the kit's copy stripped at another time: the same once both are
        # stripped with one SOURCE_DATE_EPOCH. Kit 0.27 kept every PE module because this always said "changed" (#894).
        self.write(self.work / 'pe/i386-windows/xinput1_3.dll', b'MZcode|debug /build/path')
        self.write(self.kit / 'i386-windows/xinput1_3.dll', b'MZcode@1762000000')
        self.write(self.work / 'pe/x86_64-windows/ntdll.dll', b'MZnew|debug')
        self.write(self.kit / 'x86_64-windows/ntdll.dll', b'MZold@1762000000')
        self.write(self.work / 'pe/x86_64-windows/wow64.dll', b'MZwow')
        result = self.run_tool()
        self.assertEqual(result['changed'], ['x86_64-unix/ntdll.prx', 'x86_64-windows/ntdll.dll'])
        self.assertEqual(result['same'], ['x86_64-unix/win32u.prx', 'i386-windows/xinput1_3.dll'])
        self.assertEqual(result['new'], ['x86_64-windows/wow64.dll'])
        # The build's and the kit's own files stay untouched.
        self.assertEqual((self.work / 'pe/i386-windows/xinput1_3.dll').read_bytes(), b'MZcode|debug /build/path')
        self.assertEqual((self.kit / 'i386-windows/xinput1_3.dll').read_bytes(), b'MZcode@1762000000')

    def test_refuses_a_folder_without_modules(self):
        self.kit.mkdir()
        result = subprocess.run([sys.executable, '-I', str(TOOL), str(self.work), str(self.kit)],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 2)


if __name__ == '__main__':
    unittest.main()
