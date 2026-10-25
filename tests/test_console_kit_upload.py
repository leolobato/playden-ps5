from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))

import console_kit_upload  # noqa: E402


def make_kit(folder):
    folder.mkdir(parents=True)
    (folder / 'manifest.json').write_text('{}')
    return folder


class KitFolderTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def test_takes_the_kit_folder_itself(self):
        kit = make_kit(self.root / 'kit')
        self.assertEqual(console_kit_upload.kit_folder(kit), kit)

    def test_finds_the_kit_inside_the_release_folder(self):
        release = self.root / 'playden-kit-0.12'
        kit = make_kit(release / 'some-kit')
        (release / 'licenses').mkdir()
        self.assertEqual(console_kit_upload.kit_folder(release), kit)

    def test_refuses_a_folder_without_exactly_one_kit(self):
        release = self.root / 'playden-kit-0.12'
        release.mkdir()
        with self.assertRaises(SystemExit):
            console_kit_upload.kit_folder(release)
        make_kit(release / 'a')
        make_kit(release / 'b')
        with self.assertRaises(SystemExit):
            console_kit_upload.kit_folder(release)


if __name__ == '__main__':
    unittest.main()
