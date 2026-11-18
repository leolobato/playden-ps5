import ftplib
import hashlib
import json
from pathlib import Path, PurePosixPath
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))

import console_publisher_upload as upload  # noqa: E402


class FakeFtp:
    """An in-memory zftpd: folders, files with modes, LIST, STOR, RETR, RNFR/RNTO."""

    def __init__(self, folders=()):
        self.folders = {'/'} | {str(p) for f in folders for p in [PurePosixPath(f), *PurePosixPath(f).parents]}
        self.files = {}
        self.modes = {}
        self.commands = []

    def mkd(self, path):
        if path in self.folders:
            raise ftplib.error_perm('550 exists')
        self.folders.add(path)

    def sendcmd(self, command):
        self.commands.append(command)
        _, _, mode, path = command.split(' ', 3)
        self.modes[path] = mode

    def storbinary(self, command, stream, blocksize):
        self.files[command.split(' ', 1)[1]] = stream.read()

    def retrbinary(self, command, callback, blocksize):
        callback(self.files[command.split(' ', 1)[1]])

    def retrlines(self, command, callback):
        directory = command.split(' ', 1)[1]
        if directory not in self.folders:
            raise ftplib.error_perm('550 missing')
        for folder in sorted(self.folders):
            if folder != directory and str(PurePosixPath(folder).parent) == directory:
                callback(f'drwxr-xr-x 1 u g 0 Jan 1 00:00 {PurePosixPath(folder).name}')
        for path, data in sorted(self.files.items()):
            if str(PurePosixPath(path).parent) == directory:
                mode = {'644': '-rw-r--r--'}.get(self.modes.get(path), '-rw-------')
                callback(f'{mode} 1 u g {len(data)} Jan 1 00:00 {PurePosixPath(path).name}')

    def delete(self, path):
        del self.files[path]

    def rmd(self, path):
        self.folders.remove(path)

    def rename(self, old, new):
        for folder in sorted(self.folders):
            if folder == old or folder.startswith(old + '/'):
                self.folders.remove(folder)
                self.folders.add(new + folder[len(old):])
        for path in list(self.files):
            if path.startswith(old + '/'):
                self.files[new + path[len(old):]] = self.files.pop(path)
                if path in self.modes:
                    self.modes[new + path[len(old):]] = self.modes.pop(path)


class PublisherUploadTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.folder = Path(self.temp.name)
        self.archives = {'playden-steamhost-1.tzst': b'steam', 'playden-ubisoft-1.tzst': b'ubi'}
        for name, data in self.archives.items():
            (self.folder / name).write_bytes(data)
        self.lock = {'publisher_pack': {'version': '1.test', 'archives': {
            name: {'sha256': hashlib.sha256(data).hexdigest(), 'tree_sha256': 't' + name}
            for name, data in self.archives.items()}}}
        self.write_release('1.test')

    def tearDown(self):
        self.temp.cleanup()

    def write_release(self, version, **changes):
        artifacts = {name: hashlib.sha256(data).hexdigest() for name, data in self.archives.items()}
        artifacts.update(changes)
        (self.folder / 'release.json').write_text(json.dumps(
            {'version': version, 'artifacts': artifacts,
             'tree_sha256': {name: 't' + name for name in self.archives}}))

    def test_accepts_the_pinned_pack(self):
        version, names = upload.check_pack(self.folder, self.lock)
        self.assertEqual(version, '1.test')
        self.assertEqual(names, sorted(self.archives))

    def test_refuses_another_version(self):
        self.write_release('0.old')
        with self.assertRaisesRegex(upload.PackError, 'needs 1.test'):
            upload.check_pack(self.folder, self.lock)

    def test_refuses_a_changed_archive(self):
        (self.folder / 'playden-ubisoft-1.tzst').write_bytes(b'tampered')
        with self.assertRaisesRegex(upload.PackError, 'does not match its pin'):
            upload.check_pack(self.folder, self.lock)

    def test_refuses_a_release_that_lists_other_archives(self):
        self.write_release('1.test', **{'playden-ubisoft-1.tzst': '0' * 64})
        with self.assertRaisesRegex(upload.PackError, 'lists another'):
            upload.check_pack(self.folder, self.lock)

    def test_refuses_a_missing_archive(self):
        (self.folder / 'playden-steamhost-1.tzst').unlink()
        with self.assertRaisesRegex(upload.PackError, 'missing'):
            upload.check_pack(self.folder, self.lock)

    def test_uploads_verifies_and_replaces_other_versions(self):
        ftp = FakeFtp(folders=['/data/playden/publisher/0.old'])
        ftp.files['/data/playden/publisher/0.old/release.json'] = b'{}'
        version, names = upload.check_pack(self.folder, self.lock)
        problems = upload.upload(ftp, self.folder, version, names, '/data/playden/publisher', log=lambda _: None)
        self.assertEqual(problems, [])
        self.assertEqual(ftp.files['/data/playden/publisher/1.test/playden-ubisoft-1.tzst'], b'ubi')
        self.assertIn('/data/playden/publisher/1.test/release.json', ftp.files)
        self.assertNotIn('/data/playden/publisher/0.old', ftp.folders)
        self.assertNotIn('/data/playden/publisher/.1.test.upload', ftp.folders)

    def test_creates_the_folders_on_a_new_console(self):
        ftp = FakeFtp(folders=['/data'])
        version, names = upload.check_pack(self.folder, self.lock)
        self.assertEqual(upload.upload(ftp, self.folder, version, names, '/data/playden/publisher',
                                       log=lambda _: None), [])
        # Playden writes its own data under /data/playden.
        self.assertIn('SITE CHMOD 777 /data/playden', ftp.commands)
        self.assertIn('SITE CHMOD 755 /data/playden/publisher', ftp.commands)

    def test_keeps_the_staging_folder_when_verification_fails(self):
        ftp = FakeFtp(folders=['/data/playden/publisher'])
        original = ftp.storbinary
        ftp.storbinary = lambda command, stream, size: original(command, type('S', (), {'read': lambda s: b'x'})(), size)
        version, names = upload.check_pack(self.folder, self.lock)
        problems = upload.upload(ftp, self.folder, version, names, '/data/playden/publisher', log=lambda _: None)
        self.assertTrue(problems)
        self.assertIn('/data/playden/publisher/.1.test.upload', ftp.folders)
        self.assertNotIn('/data/playden/publisher/1.test', ftp.folders)

    def test_the_lock_names_the_pinned_pack(self):
        lock = json.loads((ROOT / 'dependencies.lock.json').read_text())
        pack = lock['publisher_pack']
        self.assertEqual(pack['version'], '20261120.1')
        self.assertEqual(len(pack['archives']), 4)


if __name__ == '__main__':
    unittest.main()
