# =============================================================================
# HYDRA-UMC - tests/test_verify_release_set.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""tools/verify_release_set.py: what the release workflow accepts from the signed files the maintainer uploads (throw-away keys, public checks only)."""
import contextlib
import io
import json
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tests"))

import release_set_fixture as fx  # noqa: E402
import release_sign  # noqa: E402
import verify_release_set as vrs  # noqa: E402

DOC = fx.trust_doc()


class VerifyReleaseSetTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls._tmp = tempfile.TemporaryDirectory()
        cls.template = Path(cls._tmp.name) / "official"
        fx.make_set(cls.template, "release", DOC)
        cls.bench = Path(cls._tmp.name) / "bench"
        fx.make_set(cls.bench, "dev", DOC, records=False)

    @classmethod
    def tearDownClass(cls):
        cls._tmp.cleanup()

    def setUp(self):
        self._saved = release_sign.PUBLIC_KEY_HEX
        release_sign.PUBLIC_KEY_HEX = fx.RELEASE_PUBLIC
        self.work = tempfile.TemporaryDirectory()
        self.addCleanup(self.work.cleanup)
        self.root = Path(self.work.name) / "HYDRA-UMC"
        self.root.mkdir()
        (self.root / "hydra-umc.project.json").write_text(json.dumps({"name": "HYDRA-UMC", "version": fx.VERSION}), encoding="utf-8")
        self.folder = Path(self.work.name) / "set"
        shutil.copytree(self.template, self.folder)

    def tearDown(self):
        release_sign.PUBLIC_KEY_HEX = self._saved

    def check(self, folder=None, tag="v1.2.3"):
        return vrs.verify_set(folder or self.folder, tag, root=self.root, doc=DOC)

    def test_a_complete_signed_set_passes_and_lists_every_file(self):
        problems, names = self.check()
        self.assertEqual(problems, [])
        self.assertEqual(len(names), 60)
        self.assertEqual(sorted(names), sorted(p.name for p in self.folder.iterdir()))

    def test_the_command_line_writes_the_list_only_for_a_good_set(self):
        listing = Path(self.work.name) / "assets.txt"
        saved = (vrs.ROOT, vrs.publish_release_assets.image_trust.load)
        vrs.ROOT = self.root
        vrs.publish_release_assets.image_trust.load = lambda: DOC
        try:
            with contextlib.redirect_stdout(io.StringIO()) as out:
                # main() takes the real repository root for the version; the throw-away project file stands in for it
                original = vrs.verify_set
                vrs.verify_set = lambda folder, tag: original(folder, tag, root=self.root, doc=DOC)
                try:
                    self.assertEqual(vrs.main([str(self.folder), "--tag", "v1.2.3", "--list-file", str(listing)]), 0)
                    self.assertIn("VERIFY_RELEASE_SET=PASS", out.getvalue())
                    self.assertEqual(len(listing.read_text(encoding="utf-8").split()), 60)
                    (self.folder / "SHA256SUMS").write_bytes(b"x")
                    listing.unlink()
                    self.assertEqual(vrs.main([str(self.folder), "--tag", "v1.2.3", "--list-file", str(listing)]), 1)
                    self.assertFalse(listing.exists())
                finally:
                    vrs.verify_set = original
        finally:
            vrs.ROOT = saved[0]
            vrs.publish_release_assets.image_trust.load = saved[1]

    def test_a_changed_file_is_refused(self):
        path = self.folder / "HYDRA_RCB_APP_v1.2.3.bin"
        path.write_bytes(path.read_bytes() + b"\x00")
        problems, _ = self.check()
        self.assertTrue(problems)

    def test_a_missing_image_signature_is_refused(self):
        (self.folder / "HYDRA_RCB_APP_v1.2.3.sig2").unlink()
        self.assertTrue(any("sig2 is missing" in p for p in self.check()[0]))

    def test_a_missing_release_signature_is_refused(self):
        (self.folder / "HYDRA_RCB_APP_v1.2.3.hex.sig").unlink()
        self.assertTrue(any("HYDRA_RCB_APP_v1.2.3.hex.sig is missing" in p for p in self.check()[0]))

    def test_a_release_signature_by_another_key_is_refused(self):
        release_sign.sign_file(self.folder / "build-manifest.json", bytes(range(2, 34)))
        self.assertTrue(any("signature of build-manifest.json does not verify" in p for p in self.check()[0]))

    def test_a_signature_that_does_not_verify_under_the_pinned_key_is_refused(self):
        release_sign.PUBLIC_KEY_HEX = "00" * 32
        self.assertTrue(self.check()[0])

    def test_the_wrong_tag_is_refused(self):
        self.assertTrue(any("is not v1.2.3" in p for p in self.check(tag="v1.2.4")[0]))

    def test_a_set_built_for_the_development_table_is_never_published_as_official(self):
        problems, _ = self.check(self.bench)
        self.assertTrue(any("release one" in p for p in problems), problems)

    def test_a_file_the_set_does_not_account_for_is_refused(self):
        (self.folder / "notes.txt").write_text("x", encoding="utf-8")
        self.assertTrue(any("notes.txt is in the folder but belongs to no file" in p for p in self.check()[0]))

    def test_a_signed_extra_file_is_still_refused(self):
        (self.folder / "extra.bin").write_bytes(b"x")
        release_sign.sign_file(self.folder / "extra.bin", fx.RELEASE_SEED)
        problems = self.check()[0]
        self.assertTrue(any("extra.bin is in the folder" in p for p in problems), problems)

    def test_a_folder_inside_the_set_is_refused(self):
        (self.folder / "sub").mkdir()
        self.assertTrue(any("sub is in the folder but is not a file" in p for p in self.check()[0]))

    def test_a_missing_folder_is_refused(self):
        self.assertTrue(self.check(Path(self.work.name) / "nope")[0])

    def test_the_pinned_key_is_the_one_written_in_the_signing_page(self):
        text = (ROOT / "docs" / "RELEASE_SIGNING.md").read_text(encoding="utf-8")
        self.assertIn(self._saved, text)


if __name__ == "__main__":
    unittest.main()
