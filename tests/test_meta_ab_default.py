#!/usr/bin/env python3
# =============================================================================
# HYDRA-UMC - tests/test_meta_ab_default.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The metadata log of the Kinematic Brain bootloaders is kept in two sectors as the images ship, except in the long slot variant."""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class MetaAbDefault(unittest.TestCase):
    def test_two_sectors_by_default_and_one_in_the_long_slot(self):
        for core in ("CM4", "CM7"):
            text = (ROOT / f"src/mcu_stm32h757/{core}/boot/bootloader_common.h").read_text(encoding="utf-8")
            self.assertRegex(text, r"#ifndef HYDRA_META_AB\s+#if defined\(HYDRA_APP_SLOT_768K\) && HYDRA_APP_SLOT_768K\s+#define HYDRA_META_AB 0[^\n]*\s+#else\s+#define HYDRA_META_AB 1", core)
            self.assertEqual(len(re.findall(r"#define HYDRA_META_AB \d", text)), 2, core)


if __name__ == "__main__":
    unittest.main()
