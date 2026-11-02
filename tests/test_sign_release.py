# =============================================================================
# HYDRA-UMC - tests/test_sign_release.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The release signing step of the build and the table of the C host tests."""
import contextlib
import io
import os
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import ed25519_ref as ed25519  # noqa: E402
import host_tests  # noqa: E402
import image_header as ih  # noqa: E402
import sign_release  # noqa: E402

SEED = bytes(range(0x70, 0x90))  # made up for the test; never the key of a real build
PUBLIC = ed25519.public_key(SEED)


def image(tag: int) -> bytes:
    return bytes((0x00, 0x00, 0x01, 0x20, 0x41, 0x90, 0x00, 0x08)) + bytes((i * tag) & 0xFF for i in range(120))


def run(*args: str) -> tuple[int, str]:
    out = io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
        code = sign_release.main(list(args))
    return code, out.getvalue()


class SignReleaseTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.folder = Path(self._tmp.name)
        (self.folder / "HYDRA_RCB_APP_v0.1.9.bin").write_bytes(image(3))
        (self.folder / "HYDRA_KB_CM4_APP_v0.1.6.bin").write_bytes(image(5))
        (self.folder / "HYDRA_RCB_BOOTLOADER_v0.1.8.bin").write_bytes(image(7))
        self.seed_file = self.folder / "seed.txt"
        self.seed_file.write_text(SEED.hex() + "\n", encoding="ascii")
        os.environ.pop("HYDRA_UMC_RELEASE", None)

    def test_only_application_images_are_signed_and_they_verify(self):
        code, _ = run("sign", "--dir", str(self.folder), "--seed-file", str(self.seed_file))
        self.assertEqual(code, 0)
        self.assertEqual(sorted(p.name for p in self.folder.glob("*.sig")), ["HYDRA_KB_CM4_APP_v0.1.6.sig", "HYDRA_RCB_APP_v0.1.9.sig"])
        good, bad, unsigned = sign_release.verify_folder(self.folder, PUBLIC)
        self.assertEqual((len(good), len(bad), len(unsigned)), (2, 0, 0))

    def test_the_signature_is_the_one_the_reference_makes_and_the_bootloader_rule_accepts(self):
        sign_release.sign_folder(self.folder, SEED)
        data = (self.folder / "HYDRA_RCB_APP_v0.1.9.bin").read_bytes()
        header = sign_release.header_for(data, 0x47343031, (0, 1, 9))
        blob = header.pack() + data + (self.folder / "HYDRA_RCB_APP_v0.1.9.sig").read_bytes()
        ih.verify_image(blob, PUBLIC, hw_id=0x47343031, link_address=0x08009000, installed_version=(0, 1, 8))
        with self.assertRaises(ih.ImageError):
            ih.verify_image(blob, PUBLIC, hw_id=0x47343031, link_address=0x08009000, installed_version=(0, 1, 9))

    def test_a_changed_image_or_another_key_is_reported(self):
        sign_release.sign_folder(self.folder, SEED)
        (self.folder / "HYDRA_RCB_APP_v0.1.9.bin").write_bytes(image(4))
        _, bad, _ = sign_release.verify_folder(self.folder, PUBLIC)
        self.assertEqual([p.name for p in bad], ["HYDRA_RCB_APP_v0.1.9.sig"])
        _, bad, _ = sign_release.verify_folder(self.folder, ed25519.public_key(bytes(32)))
        self.assertEqual(len(bad), 2)

    def test_without_a_seed_nothing_is_signed_and_a_release_refuses(self):
        os.environ.pop("HYDRA_FW_SIGNING_SEED_FILE", None)
        code, text = run("sign", "--dir", str(self.folder), "--seed-file", "")
        self.assertEqual(code, 0)
        self.assertIn("SKIPPED", text)
        self.assertEqual(list(self.folder.glob("*.sig")), [])
        self.assertEqual(run("sign", "--dir", str(self.folder), "--seed-file", "", "--require")[0], 1)

    def test_the_seed_is_never_printed(self):
        _, text = run("sign", "--dir", str(self.folder), "--seed-file", str(self.seed_file))
        self.assertNotIn(SEED.hex(), text)

    def test_verify_with_a_wrong_key_fails_and_unsigned_images_fail_only_when_required(self):
        code, _ = run("verify", "--dir", str(self.folder), "--public-key-hex", PUBLIC.hex())
        self.assertEqual(code, 0)
        self.assertEqual(run("verify", "--dir", str(self.folder), "--public-key-hex", PUBLIC.hex(), "--require")[0], 1)
        sign_release.sign_folder(self.folder, SEED)
        self.assertEqual(run("verify", "--dir", str(self.folder), "--public-key-hex", ed25519.public_key(bytes(32)).hex())[0], 1)


class HostTestsTableTests(unittest.TestCase):
    def test_every_c_test_of_the_tests_folder_is_in_the_table(self):
        listed = {Path(test.source).name for test in host_tests.table()}
        for path in (ROOT / "tests").glob("test_*.c"):
            self.assertIn(path.name, listed)

    def test_every_source_of_the_table_exists(self):
        for test in host_tests.table():
            self.assertTrue((ROOT / test.source).is_file(), test.source)
            for include in test.includes:
                self.assertTrue((ROOT / include).is_dir(), include)

    def test_the_table_is_the_list_build_firmware_runs(self):
        script = (ROOT / "build_firmware.sh").read_text(encoding="utf-8")
        for test in host_tests.table():
            self.assertIn(Path(test.source).name.replace(".c", ""), script.replace(".c", ""))


if __name__ == "__main__":
    unittest.main()
