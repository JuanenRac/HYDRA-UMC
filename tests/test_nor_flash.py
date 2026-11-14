# =============================================================================
# HYDRA-UMC - tests/test_nor_flash.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The driver of the SPI NOR flash (nor_flash.h) takes its geometry and identifier from the profile, is free of the HAL, checks ranges before it touches the bus, and its
host test is registered in both runners. The driver is run by tests/test_nor_flash.c against a double of the chip. Evidence: simulator (text checks)."""
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import platform_profile as pp  # noqa: E402

DRIVER = (ROOT / "src/mcu_stm32h757/Common/nor_flash.h").read_text(encoding="utf-8")
HEADER = (ROOT / "src/mcu_stm32h757/Common/platform_profile.h").read_text(encoding="utf-8")
H757 = HEADER.split("#elif defined(HYDRA_PLATFORM_H757)", 1)[1]


def macro(name):
    return int(re.search(rf"#define {name} (0x[0-9A-Fa-f]+|\d+)U?L?\b", H757).group(1), 0)


class NorFlashTests(unittest.TestCase):
    def setUp(self):
        self.nor = pp.profile("h757_target")["new_peripherals"]["SPI5_NOR_FLASH"]

    def test_the_profile_describes_the_chip_of_the_board(self):
        self.assertEqual(self.nor["device"], "GD25Q128E")
        self.assertEqual(self.nor["capacity_bytes"], 16 * 1024 * 1024)
        self.assertEqual(self.nor["capacity_bytes"] % self.nor["sector_bytes"], 0)
        self.assertEqual(self.nor["sector_bytes"] % self.nor["page_bytes"], 0)
        self.assertLessEqual(self.nor["capacity_bytes"], 1 << 24)   # three address bytes
        self.assertIn("to_confirm", self.nor["status"])   # the command set is that of the family, not read from the datasheet of the chip
        self.assertEqual(len(self.nor["jedec_id"]), 6)

    def test_the_header_of_the_platform_carries_the_profile(self):
        self.assertEqual(macro("HYDRA_NOR_CAPACITY_BYTES"), self.nor["capacity_bytes"])
        self.assertEqual(macro("HYDRA_NOR_PAGE_BYTES"), self.nor["page_bytes"])
        self.assertEqual(macro("HYDRA_NOR_SECTOR_BYTES"), self.nor["sector_bytes"])
        self.assertEqual(macro("HYDRA_NOR_JEDEC_ID"), int(self.nor["jedec_id"], 16))
        self.assertEqual(macro("HYDRA_NOR_SPI_CLOCK_HZ"), self.nor["clock_hz"])
        self.assertNotIn("HYDRA_NOR_", HEADER.split("#elif defined(HYDRA_PLATFORM_H757)", 1)[0])   # the H745 platform has no NOR flash

    def test_the_driver_is_free_of_the_hal_and_reaches_the_chip_only_through_callbacks(self):
        self.assertNotIn("HAL_", DRIVER)
        self.assertNotIn("SPI5->", DRIVER)
        self.assertIn("NorOps", DRIVER)

    def test_every_operation_checks_the_range_before_the_bus_is_used(self):
        for name in ("Nor_Read", "Nor_EraseSector", "Nor_Write"):
            body = DRIVER.split(f"static inline NorStatus {name}(", 1)[1].split("\nstatic inline", 1)[0]
            self.assertIn("Nor_InRange", body, name)
            self.assertLess(body.index("Nor_InRange"), min(i for i in (body.find("Nor_Command"), body.find("Nor_WriteEnable")) if i >= 0), name)

    def test_the_write_is_cut_at_the_pages_and_the_status_wait_has_a_limit(self):
        write = DRIVER.split("static inline NorStatus Nor_Write(", 1)[1]
        self.assertIn("HYDRA_NOR_PAGE_BYTES - (here % HYDRA_NOR_PAGE_BYTES)", write)
        self.assertIn("HYDRA_NOR_BUSY_POLL_LIMIT", DRIVER.split("static inline NorStatus Nor_WaitIdle(", 1)[1])

    def test_the_host_test_is_registered_in_both_runners(self):
        self.assertIn("tests/test_nor_flash.c", (ROOT / "tools/host_tests.py").read_text(encoding="utf-8"))
        self.assertIn("test_nor_flash", (ROOT / "tools" / "build_firmware.py").read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
