# =============================================================================
# HYDRA-UMC - tests/test_rcc_plan.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The plan of the clock set-up of the CM7 (tools/rcc_plan.py) gives the clock tree of the profile, refuses what the PLLs and the dividers cannot do, and the C header
clock_config.h, its golden file and its host test agree with it. Evidence: simulator (arithmetic and text checks)."""
import copy
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import gen_rcc_golden as golden  # noqa: E402
import platform_profile as pp  # noqa: E402
import rcc_plan as plan  # noqa: E402


class RccPlanTests(unittest.TestCase):
    def setUp(self):
        self.profile = pp.profile("h757_target")

    def test_the_profile_of_the_board_has_no_problem(self):
        self.assertEqual(plan.problems(self.profile), [])

    def test_the_dividers_give_the_clocks_of_the_table(self):
        d = plan.divisors(self.profile)
        c = self.profile["clocks_hz"]
        self.assertEqual(c["sysclk"] // d["cpre"], c["sysclk"])
        self.assertEqual(c["sysclk"] // d["hpre"], c["hclk"])
        for key, clock in (("d1ppre", "apb3"), ("d2ppre1", "apb1"), ("d2ppre2", "apb2"), ("d3ppre", "apb4")):
            self.assertEqual(c["hclk"] // d[key], c[clock], key)

    def test_the_pll_registers_decode_to_the_pll_values_of_the_profile(self):
        p = self.profile["pll"]
        sel = plan.pllckselr(self.profile)
        self.assertEqual(sel & 3, 2)
        self.assertEqual((sel >> 4) & 0x3F, p["pll1"]["divm"])
        self.assertEqual((sel >> 12) & 0x3F, p["pll2"]["divm"])
        self.assertEqual((sel >> 20) & 0x3F, 32)   # PLL3 is left at its reset divider, off
        for number in (1, 2):
            value = plan.plldivr(self.profile, number)
            self.assertEqual((value & 0x1FF) + 1, p[f"pll{number}"]["divn"])
        pll2 = plan.plldivr(self.profile, 2)
        d = pp.PLL_DIVIDERS["pll2"]
        self.assertEqual(((pll2 >> 9) & 0x7F) + 1, d["pll2_p"])
        self.assertEqual(((pll2 >> 16) & 0x7F) + 1, d["pll2_q"])
        self.assertEqual(((pll2 >> 24) & 0x7F) + 1, d["pll2_r"])

    def test_the_ranges_follow_the_reference_and_the_vco(self):
        self.assertEqual([plan.rge(x) for x in (1e6, 2e6, 3e6, 4e6, 5e6, 8e6, 9e6, 16e6)], [0, 0, 1, 1, 2, 2, 3, 3])
        self.assertEqual(plan.vcosel(800e6), 0)
        self.assertEqual(plan.vcosel(400e6), 0)
        self.assertEqual(plan.vcosel(300e6), 0)  # also inside the wide range
        self.assertEqual(plan.vcosel(150e6), 1)
        with self.assertRaises(ValueError):
            plan.rge(25e6)

    def test_pll3_is_not_started(self):
        cfgr = plan.pllcfgr(self.profile)
        self.assertEqual((cfgr >> 22) & 7, 0)
        steps = plan.sequence(self.profile)
        turned_on = [s["value"] for s in steps if s["op"] == "set_bits" and s["reg"] == "CR"][1]
        self.assertEqual(turned_on, plan.PLL1ON | plan.PLL2ON)

    def test_the_core_clock_is_switched_last_and_only_after_the_plls_are_ready(self):
        steps = plan.sequence(self.profile)
        index = {(s["op"], s.get("reg"), s["value"]): i for i, s in enumerate(steps)}
        write = index[("write", "CFGR", plan.SW_PLL1)]
        self.assertEqual(write, len(steps) - 2)
        self.assertEqual(steps[-1]["op"], "wait_switch")
        self.assertLess(index[("wait_bits", "CR", plan.PLL1RDY | plan.PLL2RDY)], write)
        self.assertLess(index[("wait_bits", "CR", plan.HSERDY)], index[("set_bits", "CR", plan.PLL1ON | plan.PLL2ON)])
        self.assertEqual(steps[0]["op"], "flash_latency")   # the wait states before the clock is raised

    def test_what_the_registers_cannot_do_is_refused(self):
        bad = copy.deepcopy(self.profile)
        bad["pll"]["pll1"]["divm"] = 1   # a 25 MHz reference
        self.assertTrue(any("reference" in p for p in plan.problems(bad)))
        bad = copy.deepcopy(self.profile)
        bad["clocks_hz"]["apb1"] = 66_666_666   # not a power-of-two division of the bus clock
        self.assertTrue(plan.problems(bad))
        bad = copy.deepcopy(self.profile)
        bad["clocks_hz"]["cpu2"] = 100_000_000
        self.assertTrue(any("second core" in p for p in plan.problems(bad)))

    def test_the_kernel_clock_of_spi5_is_chosen_from_the_source_the_profile_names(self):
        self.assertEqual(self.profile["kernel_select"], {"spi5": "apb2"})
        value = plan.d2ccip1r(self.profile)
        self.assertEqual((value >> 16) & 7, plan.SPI45SEL["apb2"])
        self.assertEqual((value >> 28) & 3, plan.FDCANSEL_PLL2_Q)
        self.assertEqual(value & ~((3 << 28) | (7 << 16)), 0)  # no other field of the register is written
        for source, code in (("pll2_q", 1), ("pll3_q", 2)):
            other = copy.deepcopy(self.profile)
            other["kernel_select"]["spi5"] = source
            other["clocks_hz"]["kernel_spi5"] = other["clocks_hz"][source]
            self.assertEqual(plan.problems(other), [], source)
            self.assertEqual((plan.d2ccip1r(other) >> 16) & 7, code, source)
        # the register value that the C header writes is the same one
        header = (ROOT / "src/mcu_stm32h757/Common/clock_config.h").read_text(encoding="utf-8")
        self.assertIn("(unsigned long)HYDRA_CLOCK_SPI5_SEL << 16", header)

    def test_a_source_that_does_not_give_the_listed_frequency_or_does_not_exist_is_refused(self):
        wrong = copy.deepcopy(self.profile)
        wrong["kernel_select"]["spi5"] = "pll3_q"   # 75 MHz, while the table lists 100 MHz for the kernel clock of SPI5
        self.assertTrue(any("kernel clock of SPI5" in p and "pll3_q" in p for p in plan.problems(wrong)))
        unknown = copy.deepcopy(self.profile)
        unknown["kernel_select"]["spi5"] = "hse"
        self.assertTrue(any("not a source of SPI45SEL" in p for p in plan.problems(unknown)))
        missing = copy.deepcopy(self.profile)
        del missing["kernel_select"]
        self.assertTrue(any("kernel_select.spi5 is missing" in p for p in plan.problems(missing)))

    def test_the_header_of_the_platform_carries_the_code_of_the_source(self):
        header = (ROOT / "src/mcu_stm32h757/Common/platform_profile.h").read_text(encoding="utf-8")
        self.assertIn("#define HYDRA_CLOCK_SPI5_SEL 0UL", header)
        self.assertEqual(header.count("HYDRA_CLOCK_SPI5_SEL"), 1)  # only the H757 block has it
        self.assertIn("HYDRA_CLOCK_SPI5_SOURCE_HZ", (ROOT / "src/mcu_stm32h757/Common/clock_tree.h").read_text(encoding="utf-8"))

    def test_the_golden_file_is_what_the_model_produces(self):
        self.assertEqual(golden.HEADER_PATH.read_bytes().decode("utf-8").replace("\r\n", "\n"), golden.render())

    def test_the_c_header_is_registered_and_free_of_the_hal(self):
        header = (ROOT / "src/mcu_stm32h757/Common/clock_config.h").read_text(encoding="utf-8")
        self.assertNotIn("HAL_", header)
        self.assertNotIn("RCC->", header)
        self.assertIn("HYDRA_CLOCK_POLL_LIMIT", header)
        self.assertIn("tests/test_clock_config.c", (ROOT / "tools/host_tests.py").read_text(encoding="utf-8"))
        self.assertIn("test_clock_config", (ROOT / "tools" / "build_firmware.py").read_text(encoding="utf-8"))

    def test_the_poll_limit_is_a_setting_of_the_profile(self):
        self.assertGreater(self.profile["rcc_poll_limit"], 0)


if __name__ == "__main__":
    unittest.main()
