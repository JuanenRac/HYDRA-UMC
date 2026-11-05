# =============================================================================
# HYDRA-UMC - tests/test_publish_release_assets.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""tools/publish_release_assets.py: what counts as a complete, consistent, official firmware set (made with throw-away keys)."""
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

import image_trust  # noqa: E402
import publish_release_assets as pra  # noqa: E402
import release_set_fixture as fx  # noqa: E402
import release_sign  # noqa: E402

DOC = fx.trust_doc()


class PublishReleaseAssetsTests(unittest.TestCase):
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

    def plan(self, folder=None, **options):
        return pra.build_plan(self.root, folder or self.folder, "o/r", doc=DOC, **options)

    def resign(self, name):
        for suffix in (".sha256", ".sig"):
            target = self.folder / (name + suffix)
            if target.exists():
                target.unlink()
        release_sign.sign_file(self.folder / name, fx.RELEASE_SEED)

    def test_a_good_official_set_is_accepted_and_lists_every_file_with_its_signature(self):
        plan = self.plan()
        self.assertEqual(plan["problems"], [])
        self.assertFalse(plan["prerelease"])
        self.assertEqual(plan["tag"], "v" + fx.VERSION)
        names = {Path(p).name for p in plan["files"]}
        # 6 components x (bin, hex) + SHA256SUMS + build-manifest + 3 x (hdr2, sig2) = 20 files, each with .sha256 and .sig
        self.assertEqual(len(names), 60)
        for expected in ("HYDRA_RCB_APP_v1.2.3.hdr2", "HYDRA_RCB_APP_v1.2.3.sig2", "HYDRA_RCB_APP_v1.2.3.hdr2.sig", "SHA256SUMS.sha256", "build-manifest.json.sig"):
            self.assertIn(expected, names)

    def test_the_older_signature_file_is_accepted_when_it_is_there_and_never_required(self):
        legacy = Path(self.work.name) / "legacy"
        fx.make_set(legacy, "release", DOC, legacy=True)
        plan = self.plan(legacy)
        self.assertEqual(plan["problems"], [])
        self.assertIn("HYDRA_RCB_APP_v1.2.3.sig", {Path(p).name for p in plan["files"]})

    def test_a_missing_image_signature_is_refused(self):
        (self.folder / "HYDRA_KB_CM7_APP_v0.1.4.sig2").unlink()
        problems = self.plan()["problems"]
        self.assertTrue(any("HYDRA_KB_CM7_APP_v0.1.4.sig2 is missing" in p for p in problems), problems)

    def test_a_missing_header_is_refused(self):
        (self.folder / "HYDRA_RCB_APP_v1.2.3.hdr2").unlink()
        self.assertTrue(any("HYDRA_RCB_APP_v1.2.3.hdr2 is missing" in p for p in self.plan()["problems"]))

    def test_an_image_signature_by_another_key_is_refused(self):
        other = bytes(range(32))
        image = (self.folder / "HYDRA_RCB_APP_v1.2.3.bin").read_bytes()
        header = fx.h2.make_header2(other, image, 0x47343031, (1, 2, 3), 0x08000000, 5)
        (self.folder / "HYDRA_RCB_APP_v1.2.3.hdr2").write_bytes(header.pack())
        (self.folder / "HYDRA_RCB_APP_v1.2.3.sig2").write_bytes(fx.h2.sign_image2(other, header, image))
        self.resign("HYDRA_RCB_APP_v1.2.3.hdr2")
        self.resign("HYDRA_RCB_APP_v1.2.3.sig2")
        problems = self.plan()["problems"]
        self.assertTrue(any("do not verify as a bootloader" in p and "unknown_key" in p for p in problems), problems)

    def test_a_damaged_image_signature_is_refused(self):
        path = self.folder / "HYDRA_RCB_APP_v1.2.3.sig2"
        data = bytearray(path.read_bytes())
        data[10] ^= 1
        path.write_bytes(bytes(data))
        self.resign(path.name)
        self.assertTrue(any("bad_signature" in p for p in self.plan()["problems"]))

    def test_a_set_built_with_the_development_table_is_not_official(self):
        plan = self.plan(self.bench)
        self.assertTrue(any("release one" in p and "--allow-dev-key" in p for p in plan["problems"]), plan["problems"])

    def test_a_development_set_is_accepted_only_as_a_bench_pre_release(self):
        plan = self.plan(self.bench, allow_dev_key=True)
        self.assertEqual(plan["problems"], [])
        self.assertTrue(plan["prerelease"])
        self.assertIn("--prerelease", pra.gh_command(plan, True))

    def test_a_manifest_that_claims_release_over_development_bootloaders_is_refused(self):
        claimed = Path(self.work.name) / "claimed"
        shutil.copytree(self.bench, claimed)
        manifest = json.loads((claimed / "build-manifest.json").read_text(encoding="utf-8"))
        manifest["signing_key_kind"] = "release"
        (claimed / "build-manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
        problems = self.plan(claimed)["problems"]
        self.assertTrue(any("must hold the release table" in p for p in problems), problems)

    def test_a_bootloader_with_another_table_is_refused(self):
        name = "HYDRA_KB_CM4_BOOTLOADER_v0.1.1.bin"
        (self.folder / name).write_bytes(fx.bootloader(DOC, "KB_CM4", "dev"))
        self.assertTrue(any(name in p and "must hold the release table" in p for p in self.plan()["problems"]))

    def test_a_changed_image_is_refused(self):
        path = self.folder / "HYDRA_RCB_BOOTLOADER_v0.1.8.hex"
        path.write_bytes(path.read_bytes() + b"x")
        problems = self.plan()["problems"]
        self.assertTrue(any("does not match the digest" in p for p in problems), problems)
        self.assertTrue(any("HYDRA_RCB_BOOTLOADER_v0.1.8.hex.sha256 does not describe" in p for p in problems), problems)

    def test_a_file_without_its_release_signature_is_refused(self):
        (self.folder / "HYDRA_RCB_APP_v1.2.3.bin.sig").unlink()
        self.assertTrue(any("HYDRA_RCB_APP_v1.2.3.bin.sig is missing" in p for p in self.plan()["problems"]))

    def test_a_release_signature_by_another_key_is_refused(self):
        release_sign.sign_file(self.folder / "SHA256SUMS", bytes(range(1, 33)))
        self.assertTrue(any("signature of SHA256SUMS does not verify" in p for p in self.plan()["problems"]))

    def test_the_controller_application_must_carry_the_project_version(self):
        (self.root / "hydra-umc.project.json").write_text(json.dumps({"name": "HYDRA-UMC", "version": "1.2.4"}), encoding="utf-8")
        problems = self.plan()["problems"]
        self.assertTrue(any("not version 1.2.4" in p for p in problems), problems)

    def test_a_missing_component_is_refused(self):
        (self.folder / "HYDRA_KB_CM4_APP_v0.1.6.hex").unlink()
        problems = self.plan()["problems"]
        self.assertTrue(any("HYDRA_KB_CM4_APP_v0.1.6.hex is missing" in p for p in problems), problems)

    def test_a_missing_folder_is_a_problem(self):
        self.assertTrue(self.plan(Path(self.work.name) / "nope")["problems"])

    def test_the_command_is_a_draft_and_the_list_and_tag_files_are_written_only_when_all_is_well(self):
        plan = self.plan()
        command = pra.gh_command(plan, True)
        self.assertIn("--draft", command)
        self.assertNotIn("--prerelease", command)
        self.assertIn("gh release create v1.2.3 --repo o/r", command)
        self.assertNotIn("--draft", pra.gh_command(plan, False))
        listing, tag = Path(self.work.name) / "list.txt", Path(self.work.name) / "tag.txt"
        saved = (pra.ROOT, image_trust.load)
        pra.ROOT, image_trust.load = self.root, lambda: DOC
        try:
            with contextlib.redirect_stdout(io.StringIO()) as out:
                code = pra.main(["--firmware-dir", str(self.folder), "--repo", "o/r", "--draft", "--list-file", str(listing), "--tag-file", str(tag)])
            self.assertEqual(code, 0)
            self.assertIn("RELEASE_ASSETS=PASS", out.getvalue())
            self.assertEqual(len(listing.read_text(encoding="utf-8").split()), 60)
            self.assertEqual(tag.read_text(encoding="utf-8").split(), ["v1.2.3", "official"])
            (self.folder / "SHA256SUMS.sig").unlink()
            listing.unlink()
            tag.unlink()
            with contextlib.redirect_stdout(io.StringIO()):
                code = pra.main(["--firmware-dir", str(self.folder), "--repo", "o/r", "--list-file", str(listing), "--tag-file", str(tag)])
        finally:
            pra.ROOT, image_trust.load = saved
        self.assertEqual(code, 1)
        self.assertFalse(listing.exists())
        self.assertFalse(tag.exists())


if __name__ == "__main__":
    unittest.main()
