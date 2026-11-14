# =============================================================================
# HYDRA-UMC - tests/test_clock_tree.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The clock-tree macros of the H757 platform header are what the profile says, the C header that checks them is registered in the host tests, and the
numbers of the profile give the 100 MHz FDCAN kernel clock and the 1 Mbit/s of the stack bus. Evidence: simulator (header text and the Python model)."""
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import platform_profile as pp  # noqa: E402

HEADER = (ROOT / "src/mcu_stm32h757/Common/platform_profile.h").read_text(encoding="utf-8")


class ClockTreeTests(unittest.TestCase):
    def setUp(self):
        self.profile = pp.profile("h757_target")
        self.h757 = HEADER.split("#elif defined(HYDRA_PLATFORM_H757)", 1)[1]

    def macro(self, name):
        return int(re.search(rf"#define {name} (\d+)U?L?\n", self.h757).group(1))

    def test_the_header_carries_the_crystal_and_the_pll_values_of_the_profile(self):
        self.assertEqual(self.macro("HYDRA_CLOCK_HSE_HZ"), 25_000_000)
        for pll_name, pll in self.profile["pll"].items():
            number = pll_name[-1]
            self.assertEqual(self.macro(f"HYDRA_CLOCK_PLL{number}_DIVM"), pll["divm"])
            self.assertEqual(self.macro(f"HYDRA_CLOCK_PLL{number}_DIVN"), pll["divn"])

    def test_the_outputs_in_the_header_are_the_outputs_of_the_pll_values(self):
        for pll_name, pll in self.profile["pll"].items():
            for output, hz in pp.pll_outputs(self.profile["hse_hz"], pll, pp.PLL_DIVIDERS[pll_name]).items():
                self.assertAlmostEqual(self.macro(f"HYDRA_CLOCK_{output.upper()}_HZ"), hz, delta=1, msg=output)
                self.assertEqual(self.macro(f"HYDRA_CLOCK_{output.upper()}_DIV"), pp.PLL_DIVIDERS[pll_name][output], output)

    def test_the_fdcan_kernel_clock_is_pll2_q_and_gives_one_megabit(self):
        self.assertEqual(self.macro("HYDRA_CLOCK_KERNEL_FDCAN_HZ"), self.macro("HYDRA_CLOCK_PLL2_Q_HZ"))
        self.assertEqual(self.macro("HYDRA_CLOCK_KERNEL_FDCAN_HZ"), 100_000_000)
        quanta = 1 + self.macro("HYDRA_STACK_BUS_TIME_SEG1") + self.macro("HYDRA_STACK_BUS_TIME_SEG2")
        self.assertEqual(100_000_000 // (self.macro("HYDRA_STACK_BUS_PRESCALER") * quanta), 1_000_000)

    def test_the_h745_platform_defines_no_clock_tree(self):
        h745 = HEADER.split("#if defined(HYDRA_PLATFORM_H745)", 1)[1].split("#elif defined(HYDRA_PLATFORM_H757)", 1)[0]
        self.assertNotIn("HYDRA_CLOCK_", h745)

    def test_the_c_header_asserts_the_tree_and_the_host_test_is_registered_in_both_runners(self):
        c_header = (ROOT / "src/mcu_stm32h757/Common/clock_tree.h").read_text(encoding="utf-8")
        for needle in ("HYDRA_CLOCK_KERNEL_FDCAN_HZ == HYDRA_CLOCK_PLL2_Q_HZ", "HYDRA_STACK_BUS_BIT_RATE_BPS", "HYDRA_PLL1_VCO_HZ >= 192000000UL"):
            self.assertIn(needle, c_header)
        self.assertIn("HYDRA_PLATFORM_H757", (ROOT / "tools/host_tests.py").read_text(encoding="utf-8"))
        self.assertIn("test_clock_tree", (ROOT / "tools" / "build_firmware.py").read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
