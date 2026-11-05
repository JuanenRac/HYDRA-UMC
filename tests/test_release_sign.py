# =============================================================================
# HYDRA-UMC - tests/test_release_sign.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""tools/release_sign.py: the release signature of an asset, checked with the public key only (throw-away keys)."""
import base64
import contextlib
import hashlib
import io
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import ed25519_ref as ed25519  # noqa: E402
import release_sign  # noqa: E402

SEED = hashlib.sha256(b"test key of test_release_sign").digest()
PUBLIC = ed25519.public_key(SEED).hex()


class ReleaseSignTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.asset = Path(self._tmp.name) / "HYDRA_RCB_APP_v1.2.3.bin"
        self.asset.write_bytes(bytes(range(200)))
        release_sign.sign_file(self.asset, SEED)

    def test_the_pinned_key_is_a_32_byte_public_key(self):
        self.assertEqual(len(bytes.fromhex(release_sign.PUBLIC_KEY_HEX)), 32)
        self.assertTrue(release_sign.PUBLIC_KEY_HEX.startswith("1228393c"))

    def test_a_signed_file_verifies_under_its_key_and_not_under_another(self):
        self.assertIsNone(release_sign.problem_with(self.asset, PUBLIC))
        self.assertIsNotNone(release_sign.problem_with(self.asset))  # the pinned key of the releases is not the throw-away one
        self.assertIsNotNone(release_sign.problem_with(self.asset, ed25519.public_key(bytes(32)).hex()))

    def test_the_sidecar_is_in_the_sha256sum_format_and_the_signature_is_base64(self):
        self.assertEqual(Path(str(self.asset) + ".sha256").read_text(encoding="ascii"), f"{hashlib.sha256(self.asset.read_bytes()).hexdigest()}  {self.asset.name}\n")
        self.assertEqual(len(base64.b64decode(Path(str(self.asset) + ".sig").read_text(encoding="ascii"))), 64)

    def test_a_changed_file_is_refused(self):
        self.asset.write_bytes(self.asset.read_bytes() + b"\x01")
        self.assertIn("does not describe", release_sign.problem_with(self.asset, PUBLIC))

    def test_a_signature_over_other_contents_is_refused_even_with_a_matching_digest_file(self):
        other = Path(self._tmp.name) / "other.bin"
        other.write_bytes(b"other contents")
        release_sign.sign_file(other, SEED)
        shutil.copyfile(str(other) + ".sig", str(self.asset) + ".sig")
        self.assertIn("does not verify", release_sign.problem_with(self.asset, PUBLIC))

    def test_a_digest_file_for_another_name_is_refused(self):
        sidecar = Path(str(self.asset) + ".sha256")
        sidecar.write_text(sidecar.read_text(encoding="ascii").replace(self.asset.name, "HYDRA_KB_CM7_APP_v1.2.3.bin"), encoding="ascii")
        self.assertIn("does not describe", release_sign.problem_with(self.asset, PUBLIC))

    def test_missing_and_malformed_files_are_refused(self):
        Path(str(self.asset) + ".sig").write_text("not base64 !!", encoding="ascii")
        self.assertIn("not a base64", release_sign.problem_with(self.asset, PUBLIC))
        Path(str(self.asset) + ".sig").write_text(base64.b64encode(b"short").decode("ascii"), encoding="ascii")
        self.assertIn("not a base64", release_sign.problem_with(self.asset, PUBLIC))
        Path(str(self.asset) + ".sig").unlink()
        self.assertIn(".sig is missing", release_sign.problem_with(self.asset, PUBLIC))
        Path(str(self.asset) + ".sha256").unlink()
        self.assertIn(".sha256 is missing", release_sign.problem_with(self.asset, PUBLIC))
        self.assertIsNotNone(release_sign.problem_with(Path(self._tmp.name) / "absent.bin", PUBLIC))

    def test_a_public_key_that_is_not_a_key_is_refused(self):
        self.assertIsNotNone(release_sign.problem_with(self.asset, "zz"))
        self.assertIsNotNone(release_sign.problem_with(self.asset, "00" * 16))

    def test_the_command_line(self):
        saved = release_sign.PUBLIC_KEY_HEX
        release_sign.PUBLIC_KEY_HEX = PUBLIC
        try:
            with contextlib.redirect_stdout(io.StringIO()) as out:
                self.assertEqual(release_sign.main(["verify", str(self.asset)]), 0)
                self.assertEqual(release_sign.main(["public"]), 0)
            self.assertIn("RELEASE_SIGN=PASS", out.getvalue())
            Path(str(self.asset) + ".sig").unlink()
            with contextlib.redirect_stdout(io.StringIO()) as out:
                self.assertEqual(release_sign.main(["verify", str(self.asset)]), 1)
            self.assertIn("RELEASE_SIGN=FAIL", out.getvalue())
            with contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(release_sign.main([]), 2)
        finally:
            release_sign.PUBLIC_KEY_HEX = saved


if __name__ == "__main__":
    unittest.main()
