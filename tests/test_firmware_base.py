# =============================================================================
# HYDRA-UMC - tests/test_firmware_base.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The shared part of the three STM32 bootloaders (tools/sync_firmware_base.py, docs/FIRMWARE_BASE.md): the copies in the chip directories are the
base byte for byte, the security code (the hash, the signature check, the image header, the boot decision, the trust tables) is in it, and the tool
tells a drifted copy. Evidence: source check."""
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import sync_firmware_base as base  # noqa: E402

MUST_BE_SHARED = (
    "bootloader_crypto.c", "bootloader_crypto.h", "bootloader_trust.c", "bootloader_trust.h", "boot_decision.h", "update_signature.c", "update_signature.h",
    "update_verify.h", "image_header.h", "flash_layer.h", "tweetnacl.c", "tweetnacl.h", "hydra_trust_dev.h", "hydra_trust_release.h", "boot_update_glue.h",
)


class SharedBootloaderCode(unittest.TestCase):
    def test_every_copy_is_the_base(self):
        self.assertEqual(base.problems(), [])

    def test_the_security_code_is_shared(self):
        shared = set(base.read_list())
        for name in MUST_BE_SHARED:
            self.assertIn(name, shared, name)

    def test_nothing_that_differs_between_the_chips_is_listed_as_shared(self):
        for name in ("bootloader_common.h", "bootloader_flash.c", "bootloader_protocol.c", "bootloader_main.c"):
            self.assertNotIn(name, base.read_list(), name)

    def test_the_base_holds_no_key_and_no_hmac(self):
        for name in ("bootloader_crypto.c", "bootloader_crypto.h"):
            text = (base.BASE / name).read_text(encoding="utf-8")
            for forbidden in ("HMAC_KEY", "UPDATE_PUBLIC_KEY", "hmac_sha256"):
                self.assertNotIn(forbidden, text, f"{name}: {forbidden}")

    def test_the_crypto_file_takes_the_hal_through_the_common_header(self):
        text = (base.BASE / "bootloader_crypto.c").read_text(encoding="utf-8")
        self.assertIn('#include "bootloader_common.h"', text)
        self.assertNotIn("stm32g4xx_hal.h", text)
        self.assertNotIn("stm32h7xx_hal.h", text)
        for directory in base.CHIPS.values():
            self.assertRegex((directory / "bootloader_common.h").read_text(encoding="utf-8"), r'#include "stm32(g4|h7)xx_hal\.h"')

    def test_the_tool_finds_a_drifted_copy_a_missing_copy_and_an_unlisted_base_file(self):
        with tempfile.TemporaryDirectory() as scratch:
            work = Path(scratch)
            for key, directory in base.CHIPS.items():
                shutil.copytree(directory, work / key)
            shutil.copytree(base.BASE, work / "base")
            saved = (dict(base.CHIPS), base.BASE, base.LIST)
            try:
                base.CHIPS.update({key: work / key for key in base.CHIPS})
                base.BASE = work / "base"
                self.assertEqual(base.problems(), [])
                drifted = work / "g474" / "boot_decision.h"
                drifted.write_bytes(drifted.read_bytes() + b"// edited\n")
                self.assertTrue(any("boot_decision.h" in line and "g474" in line and "not the base" in line for line in base.problems()))
                (work / "h745_cm7" / "update_verify.h").unlink()
                self.assertTrue(any("update_verify.h: missing from h745_cm7" == line for line in base.problems()))
                (work / "base" / "extra.h").write_text("x", encoding="utf-8")
                self.assertTrue(any("extra.h" in line and "not in shared.json" in line for line in base.problems()))
            finally:
                base.CHIPS.update(saved[0])
                base.BASE, base.LIST = saved[1], saved[2]

    def test_the_difference_between_two_files_counts_lines_not_line_endings(self):
        self.assertEqual(base.line_difference(b"a\r\nb\r\n", b"a\nb\n"), 0)
        self.assertEqual(base.line_difference(b"a\nb\n", b"a\nc\n"), 2)

    def test_sync_restores_a_drifted_copy(self):
        with tempfile.TemporaryDirectory() as scratch:
            work = Path(scratch)
            for key, directory in base.CHIPS.items():
                shutil.copytree(directory, work / key)
            saved = dict(base.CHIPS)
            try:
                base.CHIPS.update({key: work / key for key in base.CHIPS})
                (work / "h745_cm4" / "image_header.h").write_bytes(b"drifted")
                self.assertNotEqual(base.problems(), [])
                self.assertEqual(base.cmd_sync(), 0)
                self.assertEqual(base.problems(), [])
            finally:
                base.CHIPS.update(saved)


if __name__ == "__main__":
    unittest.main()
