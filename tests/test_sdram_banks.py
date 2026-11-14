# =============================================================================
# HYDRA-UMC - tests/test_sdram_banks.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The decision about which SDRAM banks may be used (sdram_banks.h): free of the HAL, the probe of the second bank ships off, it reaches the memory only through the
callbacks of memtest.h, and its host test is registered in both runners with the three build variants. The decision itself is run by tests/test_sdram_banks.c against a RAM
double with faults. Evidence: simulator (text checks)."""
import json
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = (ROOT / "src/mcu_stm32h757/Common/sdram_banks.h").read_text(encoding="utf-8")
PROFILE = (ROOT / "src/mcu_stm32h757/Common/platform_profile.h").read_text(encoding="utf-8")


class SdramBanksTests(unittest.TestCase):
    def test_the_probe_ships_off_and_is_not_a_profile_value(self):
        self.assertIn("#define HYDRA_SDRAM_BANK2_PROBE 0", HEADER)
        self.assertIn("#ifndef HYDRA_SDRAM_BANK2_PROBE", HEADER)
        self.assertNotIn("HYDRA_SDRAM_BANK2_PROBE", PROFILE)

    def test_the_second_bank_still_ships_off(self):
        self.assertIn("#define HYDRA_SDRAM_BANK2_ENABLED 0", PROFILE)

    def test_it_is_free_of_the_hal_and_uses_the_memory_tests(self):
        self.assertNotIn("HAL_", HEADER)
        self.assertNotIn("stm32h7xx", HEADER)
        self.assertIn('#include "memtest.h"', HEADER)
        self.assertIn("Memtest_Run(", HEADER)

    def test_the_probe_window_is_a_power_of_two_inside_the_bank(self):
        words = int(HEADER.split("#define SDRAM_BANKS_PROBE_WORDS ")[1].split("u")[0])
        self.assertEqual(words & (words - 1), 0)
        self.assertLessEqual(words * 4, 134217728)

    def test_the_c_test_is_in_both_lists_with_the_three_variants(self):
        hosts = (ROOT / "tools/host_tests.py").read_text(encoding="utf-8")
        build = (ROOT / "tools/build_firmware.py").read_text(encoding="utf-8")
        self.assertEqual(hosts.count("tests/test_sdram_banks.c"), 3)
        self.assertIn("HYDRA_SDRAM_BANK2_PROBE=1", hosts)
        self.assertIn("test_sdram_banks", build)
        self.assertIn("HYDRA_SDRAM_BANK2_PROBE=1", build)

    def test_it_has_a_coverage_floor_of_100(self):
        floors = json.loads((ROOT / "tools/host_coverage_floor.json").read_text(encoding="utf-8"))
        self.assertEqual(floors["modules"]["src/mcu_stm32h757/Common/sdram_banks.h"], 100)


if __name__ == "__main__":
    unittest.main()
