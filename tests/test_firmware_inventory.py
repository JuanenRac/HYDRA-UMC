# =============================================================================
# HYDRA-UMC - tests of the check of the firmware inventory
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The repository keeps no firmware image: the check passes on a clean tree and still refuses images that no manifest describes."""
import contextlib
import io
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import verify_firmware_inventory as inventory  # noqa: E402


class FirmwareInventoryTests(unittest.TestCase):
    def run_in(self, folder: Path) -> str:
        out = io.StringIO()
        with mock.patch.object(inventory, "FIRMWARE", folder), mock.patch.object(inventory, "MANIFEST", folder / "firmware_manifest.json"):
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
                try:
                    inventory.main()
                except SystemExit:
                    pass
        return out.getvalue()

    def test_a_tree_without_a_build_passes(self):
        with tempfile.TemporaryDirectory() as folder:
            self.assertIn("FIRMWARE_INVENTORY=PASS", self.run_in(Path(folder) / "firmware"))

    def test_images_without_a_manifest_are_refused(self):
        with tempfile.TemporaryDirectory() as folder:
            (Path(folder) / "HYDRA_RCB_APP_v0.0.1.bin").write_bytes(b"image")
            self.assertIn("FIRMWARE_INVENTORY=FAIL", self.run_in(Path(folder)))

    def test_no_firmware_image_is_tracked_in_git(self):
        try:
            listed = subprocess.run(["git", "ls-files", "firmware"], cwd=ROOT, capture_output=True, text=True, check=True, timeout=60)
        except (OSError, subprocess.SubprocessError):
            self.skipTest("not a git checkout")
        self.assertEqual(listed.stdout.split(), [])


if __name__ == "__main__":
    unittest.main()
