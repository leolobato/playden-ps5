import hashlib
import os
import pathlib
import stat
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


def mode_of(path):
    return stat.S_IMODE(path.stat().st_mode)


class NativePackageIdentityTests(unittest.TestCase):
    def test_package_identity_binds_title_and_signed_binary(self):
        with tempfile.TemporaryDirectory() as name:
            app = pathlib.Path(name)
            image = b"signed-title-fixture"
            (app / "eboot.bin").write_bytes(image)
            result = subprocess.run([
                "cmake", f"-DAPP_DIR={app}", "-DTITLE_ID=FAKE77031", "-P",
                str(ROOT / "cmake/WriteNativePackageIdentity.cmake")],
                capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual((app / "playden-package.identity").read_text(),
                             "playden-native/1\nFAKE77031\n" + hashlib.sha256(image).hexdigest() + "\n")
            self.assertTrue((app / "logs").is_dir())
            self.assertTrue((app / "playden-data").is_dir())

    def test_missing_binary_or_invalid_title_does_not_publish_identity(self):
        with tempfile.TemporaryDirectory() as name:
            app = pathlib.Path(name)
            for title in ("FAKE77031", "../FAKE77031"):
                if title.startswith("../"):
                    (app / "eboot.bin").write_bytes(b"signed-title-fixture")
                result = subprocess.run([
                    "cmake", f"-DAPP_DIR={app}", f"-DTITLE_ID={title}", "-P",
                    str(ROOT / "cmake/WriteNativePackageIdentity.cmake")],
                    capture_output=True, text=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse((app / "playden-package.identity").exists())

    def test_package_step_gives_executables_their_mode(self):
        with tempfile.TemporaryDirectory() as name:
            app = pathlib.Path(name)
            executables = ("eboot.bin", "sce_module/libc.prx", "helper.elf", "runtime/module.prx")
            others = ("sce_sys/param.json", "playden-version")
            for relative in executables + others:
                (app / relative).parent.mkdir(parents=True, exist_ok=True)
                (app / relative).write_bytes(b"fixture")
                (app / relative).chmod(0o644)
            result = subprocess.run([
                "cmake", f"-DAPP_DIR={app}", "-P", str(ROOT / "cmake/SetPackageExecutableModes.cmake")],
                capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            for relative in executables:
                self.assertEqual(mode_of(app / relative), 0o755, relative)
            for relative in others:
                self.assertEqual(mode_of(app / relative), 0o644, relative)

    def test_package_step_refuses_a_missing_binary_or_a_symlink(self):
        script = str(ROOT / "cmake/SetPackageExecutableModes.cmake")
        with tempfile.TemporaryDirectory() as name:
            root = pathlib.Path(name)
            app = root / "app"
            app.mkdir()
            result = subprocess.run(["cmake", f"-DAPP_DIR={app}", "-P", script], capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            outside = root / "outside.prx"
            outside.write_bytes(b"fixture")
            outside.chmod(0o644)
            (app / "eboot.bin").write_bytes(b"fixture")
            (app / "linked.prx").symlink_to(outside)
            result = subprocess.run(["cmake", f"-DAPP_DIR={app}", "-P", script], capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(mode_of(outside), 0o644)

    def test_package_build_sets_modes_after_every_file_is_in_place(self):
        build = (ROOT / "cmake/HomebrewPS5.cmake").read_text()
        modes = build.index('-P "${CMAKE_SOURCE_DIR}/cmake/SetPackageExecutableModes.cmake"')
        self.assertGreater(modes, build.index('--out "${app}/eboot.bin"'))
        self.assertGreater(modes, build.index('"${app}/sce_module/libc.prx"'))
        self.assertGreater(modes, build.index('${native_helper_package_commands}\n'))
        self.assertLess(modes, build.index('-E touch "${stamp}"'))

    # scripts/build-homebrew-ps5.sh sets PLAYDEN_PACKAGE_DIR, so every PS5 build checks its real package.
    # A plain host run has no package to check, and must not read a stale one left by an older build.
    @unittest.skipUnless(os.environ.get("PLAYDEN_PACKAGE_DIR"), "PLAYDEN_PACKAGE_DIR names no built package")
    def test_built_package_keeps_its_execute_bits(self):
        app = pathlib.Path(os.environ["PLAYDEN_PACKAGE_DIR"])
        for relative in ("eboot.bin", "sce_module/libc.prx"):
            self.assertTrue((app / relative).is_file(), relative)
        for path in app.rglob("*"):
            if path.name == "eboot.bin" or path.suffix in (".prx", ".elf"):
                self.assertEqual(mode_of(path), 0o755, path)


if __name__ == "__main__":
    unittest.main()
