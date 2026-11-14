# =============================================================================
# HYDRA-UMC - tests/test_sdram_plan.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The plan of the SDRAM start-up (tools/sdram_plan.py) computes the FMC registers and the JEDEC sequence from the profile, refuses settings the geometry cannot hold, and the
C header sdram_init.h, its golden file and its host test agree with it. Evidence: simulator (arithmetic and text checks)."""
import copy
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import gen_sdram_golden as golden  # noqa: E402
import platform_profile as pp  # noqa: E402
import sdram_plan as plan  # noqa: E402

HEADER = (ROOT / "src/mcu_stm32h757/Common/platform_profile.h").read_text(encoding="utf-8")
H757 = HEADER.split("#elif defined(HYDRA_PLATFORM_H757)", 1)[1]


def macro(name):
    return int(re.search(rf"#define {name} (\d+)U?L?\b", H757).group(1))


class SdramPlanTests(unittest.TestCase):
    def setUp(self):
        self.profile = pp.profile("h757_target")
        self.cfg = plan.sdram(self.profile)

    def mutate(self, **changes):
        profile = copy.deepcopy(self.profile)
        profile["new_peripherals"]["FMC"]["sdram"].update(changes)
        return profile

    def test_the_profile_of_the_board_has_no_problem(self):
        self.assertEqual(plan.problems(self.profile), [])

    def test_the_geometry_of_the_board_holds_128_mib_per_bank(self):
        self.assertEqual(plan.capacity_bytes(self.cfg), 128 * 1024 * 1024)
        for bank in self.cfg["banks"]:
            region = next(r for r in self.profile["memory_map"]["regions"] if r["name"] == bank["region"])
            self.assertEqual(region["length"], bank["size_bytes"])

    def test_the_registers_decode_to_the_settings(self):
        cr = plan.sdcr(self.profile)
        self.assertEqual(cr & 3, 10 - 8)
        self.assertEqual((cr >> 2) & 3, 13 - 11)
        self.assertEqual((cr >> 4) & 3, 2)
        self.assertEqual((cr >> 6) & 1, 1)
        self.assertEqual((cr >> 7) & 3, 3)
        self.assertEqual((cr >> 10) & 3, 2)
        self.assertEqual((cr >> 12) & 1, 1)
        tr = plan.sdtr(self.profile)
        for name, shift in plan.TIMING_SHIFT.items():
            self.assertEqual((tr >> shift) & 0xF, self.cfg["timing_sdclk_cycles"][name] - 1, name)

    def test_the_mode_register_is_burst_1_sequential_cas_3_single_write(self):
        self.assertEqual(plan.mode_register(self.profile), 0x230)

    def test_the_refresh_count_covers_every_row_in_the_period(self):
        count = plan.refresh_count(self.profile)
        self.assertEqual(count, 761)
        row_time = (count + self.cfg["refresh_margin_cycles"]) / plan.sdclk_hz(self.profile)
        self.assertLessEqual(row_time * (1 << self.cfg["row_bits"]), self.cfg["refresh_period_ms"] * 1e-3 + 1e-12)

    def test_the_sequence_is_the_jedec_one_and_the_refresh_rate_is_last(self):
        steps = plan.sequence(self.profile)
        commands = [(s["value"] & 7) for s in steps if s.get("reg") == "SDCMR"]
        self.assertEqual(commands, [plan.MODE_CLOCK_ENABLE, plan.MODE_PALL, plan.MODE_AUTO_REFRESH, plan.MODE_LOAD_MODE])
        waits = [i for i, s in enumerate(steps) if s["op"] == "wait_us"]
        self.assertEqual(len(waits), 1)
        self.assertGreaterEqual(steps[waits[0]]["value"], 100)
        self.assertEqual(steps[waits[0] - 1]["reg"], "SDCMR")   # the wait follows the clock enable
        self.assertEqual(steps[-1]["reg"], "SDRTR")

    def test_a_bank_switched_off_leaves_the_commands_and_a_smaller_window_has_to_follow_the_region(self):
        profile = copy.deepcopy(self.profile)
        sdram = profile["new_peripherals"]["FMC"]["sdram"]
        sdram["banks"][1]["enabled"] = False
        self.assertEqual(plan.target_bits(profile), plan.BANK_TARGET_BITS[1])
        self.assertEqual(plan.problems(profile), [])
        sdram["banks"][0]["size_bytes"] = 64 * 1024 * 1024   # the window used is smaller than what the chips hold
        self.assertTrue(any("region says" in p for p in plan.problems(profile)))   # the region of the memory map has to follow

    def test_settings_the_geometry_cannot_hold_are_refused(self):
        too_big = copy.deepcopy(self.profile)
        too_big["new_peripherals"]["FMC"]["sdram"]["banks"][0]["size_bytes"] = 256 * 1024 * 1024
        self.assertTrue(any("do not fit" in p for p in plan.problems(too_big)))
        for field, value in (("row_bits", 14), ("column_bits", 12), ("cas_latency", 4), ("bus_width_bits", 24), ("startup_delay_us", 50), ("auto_refresh_cycles", 1), ("sdclk_period_hclk", 1)):
            self.assertTrue(plan.problems(self.mutate(**{field: value})), field)
        none = copy.deepcopy(self.profile)
        for bank in none["new_peripherals"]["FMC"]["sdram"]["banks"]:
            bank["enabled"] = False
        self.assertTrue(any("no SDRAM bank" in p for p in plan.problems(none)))

    def test_a_refresh_period_too_long_for_the_counter_is_refused(self):
        self.assertTrue(any("13 bits" in p for p in plan.problems(self.mutate(refresh_period_ms=8000, refresh_margin_cycles=0))))

    def test_the_header_of_the_platform_carries_the_settings_of_the_profile(self):
        self.assertEqual(macro("HYDRA_SDRAM_ROW_BITS"), self.cfg["row_bits"])
        self.assertEqual(macro("HYDRA_SDRAM_COLUMN_BITS"), self.cfg["column_bits"])
        self.assertEqual(macro("HYDRA_SDRAM_CAS_LATENCY"), self.cfg["cas_latency"])
        self.assertEqual(macro("HYDRA_SDRAM_REFRESH_PERIOD_MS"), self.cfg["refresh_period_ms"])
        for name, cycles in self.cfg["timing_sdclk_cycles"].items():
            self.assertEqual(macro(f"HYDRA_SDRAM_{name.upper()}"), cycles, name)
        for bank in self.cfg["banks"]:
            self.assertEqual(macro(f"HYDRA_SDRAM_BANK{bank['fmc_bank']}_SIZE_BYTES"), bank["size_bytes"])
        # the H745 platform has no SDRAM
        self.assertNotIn("HYDRA_SDRAM_", HEADER.split("#elif defined(HYDRA_PLATFORM_H757)", 1)[0])

    def test_the_golden_file_is_what_the_model_produces(self):
        self.assertEqual(golden.HEADER_PATH.read_bytes().decode("utf-8").replace("\r\n", "\n"), golden.render())

    def test_the_c_header_is_registered_and_free_of_the_hal(self):
        c_header = (ROOT / "src/mcu_stm32h757/Common/sdram_init.h").read_text(encoding="utf-8")
        self.assertNotIn("HAL_", c_header)
        self.assertNotIn("FMC_Bank", c_header)
        for needle in ("SDRAM_MODE_CLOCK_ENABLE", "Sdram_Start", "wait_not_busy", "HYDRA_SDRAM_STARTUP_DELAY_US"):
            self.assertIn(needle, c_header)
        self.assertIn("tests/test_sdram_init.c", (ROOT / "tools/host_tests.py").read_text(encoding="utf-8"))
        self.assertIn("test_sdram_init", (ROOT / "tools" / "build_firmware.py").read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
