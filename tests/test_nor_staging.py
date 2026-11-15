# =============================================================================
# HYDRA-UMC - tests/test_nor_staging.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The SPI5 driver of the NOR flash, the glue that puts the copy of the 768 KB slot on it, the staging of an update there and the switch of the size of the slot: the numbers agree with
the platform profile, everything ships off, the default size of the slot is still 384 KB, and the new sources are in both runners with floors. Evidence: simulator (the doubles run in
tests/test_nor_spi5.c, tests/test_nor_backup_glue.c and tests/test_nor_backup_plan.c)."""
from __future__ import annotations

import json
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import platform_profile as pp  # noqa: E402

CM7 = ROOT / "src/mcu_stm32h757/CM7"
COMMON = ROOT / "src/mcu_stm32h757/Common"
BOOT_COMMON = (CM7 / "boot/bootloader_common.h").read_text(encoding="utf-8")


class Spi5Driver(unittest.TestCase):
    def test_the_prescaler_gives_the_clock_of_the_profile(self):
        profile = pp.profile("h757_target")
        kernel = profile["clocks_hz"]["kernel_spi5"]
        self.assertEqual(kernel // 16, profile["new_peripherals"]["SPI5_NOR_FLASH"]["clock_hz"])
        self.assertIn("#define NOR_SPI5_PRESCALER 16UL", (CM7 / "NorSpi5.c").read_text(encoding="utf-8"))

    def test_it_ships_off_and_takes_its_pins_from_the_board_set_up(self):
        header = (CM7 / "NorSpi5.h").read_text(encoding="utf-8")
        self.assertIn("#define HYDRA_NOR_SPI5 0", header)
        for field in ("cs_port", "cs_pin", "bus_port", "bus_pins", "alternate"):
            self.assertIn(field, header)
        # the pins are not in the profile and are not invented in the driver
        source = (CM7 / "NorSpi5.c").read_text(encoding="utf-8")
        self.assertIsNone(re.search(r"GPIO_PIN_\d+|GPIO[A-K]\b", source))

    def test_nothing_in_the_firmware_includes_the_driver_or_the_glue_yet(self):
        users = [
            p.name
            for p in (ROOT / "src").rglob("*")
            if p.is_file() and p.suffix in (".c", ".h") and p.name not in {"NorSpi5.c", "NorSpi5.h", "nor_backup_glue.h"}
            and re.search(r'#include "(NorSpi5|nor_backup_glue)\.h"', p.read_text(encoding="utf-8", errors="ignore"))
        ]
        self.assertEqual(users, [])


class SlotSize(unittest.TestCase):
    def test_the_default_size_of_the_slot_is_384k_and_the_long_one_needs_the_nor_copy(self):
        self.assertIn("#define APP_MAX_SIZE         (384UL * 1024UL) // 3 sectors", BOOT_COMMON)
        self.assertIn("#define APP_MAX_SIZE         (768UL * 1024UL)", BOOT_COMMON)
        self.assertRegex(BOOT_COMMON, r"#if defined\(HYDRA_APP_SLOT_768K\) && HYDRA_APP_SLOT_768K\s+#if !defined\(HYDRA_NOR_BACKUP\) \|\| !HYDRA_NOR_BACKUP\s+#error")

    def test_the_long_size_is_the_length_of_the_slot_of_the_h757_profile(self):
        regions = {r["name"]: r for r in pp.profile("h757_target")["memory_map"]["regions"]}
        self.assertEqual(regions["FLASH_APP_CM7"]["length"], 768 * 1024)

    def test_the_build_does_not_define_the_switches(self):
        build = (ROOT / "tools/build_firmware.py").read_text(encoding="utf-8")
        for name in ("HYDRA_APP_SLOT_768K", "HYDRA_NOR_BACKUP"):
            self.assertNotIn(name, build)


class Registered(unittest.TestCase):
    def test_the_new_sources_are_in_both_runners_and_have_floors(self):
        host = (ROOT / "tools/host_tests.py").read_text(encoding="utf-8")
        build = (ROOT / "tools/build_firmware.py").read_text(encoding="utf-8")
        for test in ("tests/test_nor_spi5.c", "tests/test_nor_backup_glue.c"):
            self.assertIn(test, host)
        for token in ("test_nor_spi5", "test_nor_backup_glue", "-DHYDRA_NOR_SPI5=1", "NorSpi5.c"):
            self.assertIn(token, build)
        floors = json.loads((ROOT / "tools/host_coverage_floor.json").read_text(encoding="utf-8"))["modules"]
        for module in ("CM7/NorSpi5.c", "Common/nor_backup_glue.h", "Common/nor_backup_plan.h"):
            self.assertEqual(floors["src/mcu_stm32h757/" + module], 100)


if __name__ == "__main__":
    unittest.main()
