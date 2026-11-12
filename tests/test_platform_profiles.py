# =============================================================================
# HYDRA-UMC - tests/test_platform_profiles.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The platform profiles (docs/platform_profiles.json): the clock tree recomputes from the crystal, the stack bus gives 1 Mbit/s on every platform, and the
profile of today is the one the firmware sources are built for. Evidence: simulator (arithmetic on the numbers); nothing ran on a board."""
import copy
import json
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import platform_profile as pp  # noqa: E402


class PlatformProfileTests(unittest.TestCase):
    def setUp(self):
        self.document = pp.load()
        self.contract = json.loads((ROOT / "docs" / "bus_contract.json").read_text(encoding="utf-8"))

    def test_the_file_is_consistent_and_agrees_with_the_bus_contract(self):
        for name, p in self.document["profiles"].items():
            self.assertEqual(pp.problems(name, p), [], name)
        self.assertEqual(pp.contract_problems(self.document, self.contract), [])

    def test_the_default_profile_exists_and_is_the_one_of_today(self):
        self.assertEqual(self.document["default_profile"], "h745_current")
        self.assertIn(self.document["default_profile"], self.document["profiles"])
        self.assertIn("mcu_stm32h745", pp.profile()["source"])
        self.assertTrue((ROOT / pp.profile()["source"]).is_dir())

    def test_every_platform_gives_one_megabit_on_the_stack_bus(self):
        for name, p in self.document["profiles"].items():
            self.assertAlmostEqual(pp.bit_rate(p["stack_bus"]), 1_000_000, delta=1, msg=name)

    def test_the_sample_point_is_in_the_range_every_node_can_live_with(self):
        for name, p in self.document["profiles"].items():
            self.assertTrue(0.75 <= pp.sample_point(p["stack_bus"]) <= 0.90, name)

    def test_the_timing_of_today_is_the_one_of_the_sources(self):
        text = (ROOT / "src/mcu_stm32h745/CM4/KinematicBrainCan.c").read_text(encoding="utf-8")
        bus = pp.profile("h745_current")["stack_bus"]
        for field, key in (("NominalPrescaler", "prescaler"), ("NominalSyncJumpWidth", "sync_jump_width"), ("NominalTimeSeg1", "time_segment_1"), ("NominalTimeSeg2", "time_segment_2")):
            found = {int(v) for v in re.findall(rf"\.Init\.{field}\s*=\s*(\d+)\s*;", text)}
            self.assertEqual(found, {bus[key]}, field)

    def test_the_cubemx_value_is_kept_and_is_the_half_rate_the_owner_corrected(self):
        p = self.document["profiles"]["h757_target"]
        self.assertAlmostEqual(pp.bit_rate(p["cubemx_stack_bus"]), 500_000, delta=1)
        self.assertEqual(p["cubemx_stack_bus"]["prescaler"], 2 * p["stack_bus"]["prescaler"])

    def test_the_clock_tree_recomputes_from_the_crystal(self):
        p = self.document["profiles"]["h757_target"]
        self.assertEqual(p["hse_hz"], 25_000_000)
        for pll_name, pll in p["pll"].items():
            for output, value in pp.pll_outputs(p["hse_hz"], pll, pp.PLL_DIVIDERS[pll_name]).items():
                self.assertAlmostEqual(value, p["clocks_hz"][output], delta=1, msg=output)
        clocks = p["clocks_hz"]
        self.assertEqual(clocks["hclk"] * 2, clocks["sysclk"])
        self.assertEqual(clocks["cpu2"], clocks["hclk"])
        self.assertEqual(clocks["kernel_fdcan"], clocks["pll2_q"])

    def test_the_peripheral_owners_follow_the_decision_of_the_owner(self):
        p = self.document["profiles"]["h757_target"]
        self.assertEqual(p["peripheral_owner"], {"SPI1": "CM7", "FDCAN1": "CM7", "I2C4": "CM7"})
        self.assertEqual(p["host"]["spi_role"], "master")
        self.assertEqual(pp.profile("h745_current")["host"]["spi_role"], "slave")

    def test_the_fram_timing_is_a_candidate_with_no_default(self):
        self.assertIsNone(pp.profile("h745_current")["fram_i2c"]["timing"])
        fram = self.document["profiles"]["h757_target"]["fram_i2c"]
        self.assertEqual(fram["timing"], "0x009034B6")
        self.assertIn("NOT confirmed", fram["timing_status"])
        self.assertEqual(fram["address"], "0x50")

    def test_a_wrong_profile_is_caught(self):
        def broken(mutate):
            p = copy.deepcopy(self.document["profiles"]["h757_target"])
            mutate(p)
            return pp.problems("h757_target", p)

        self.assertTrue(broken(lambda p: p["peripheral_owner"].update(SPI1="CM9")))
        self.assertTrue(broken(lambda p: p["host"].update(spi_role="both")))
        self.assertTrue(broken(lambda p: p["stack_bus"].update(kernel_clock_hz=16_000_000)))
        self.assertTrue(broken(lambda p: p["clocks_hz"].update(pll2_q=90_000_000, kernel_fdcan=90_000_000) or p["stack_bus"].update(kernel_clock_hz=90_000_000)))
        self.assertTrue(broken(lambda p: p["stack_bus"].update(sync_jump_width=9)))
        self.assertTrue(broken(lambda p: p["stack_bus"].update(prescaler=1000)))
        document = copy.deepcopy(self.document)
        document["profiles"]["h757_target"]["stack_bus"]["prescaler"] = 10
        self.assertTrue(pp.contract_problems(document, self.contract))

    def test_the_controller_board_runs_both_of_its_buses_at_one_megabit(self):
        self.assertEqual(pp.board_problems(self.document, self.contract), [])
        board = self.document["controller_board"]
        for name in ("stack_bus", "tool_head_downlink"):
            self.assertEqual(pp.bit_rate(board[name]), 1_000_000, name)
            self.assertGreaterEqual(pp.sample_point(board[name]), 0.75, name)

    def test_the_controller_board_timing_is_what_the_sources_of_both_controllers_set(self):
        text = (ROOT / "src/mcu_stm32g474/RobotControllerRelay.c").read_text(encoding="utf-8")
        board = self.document["controller_board"]
        for handle, bus in (("hfdcan1_app", "stack_bus"), ("hfdcan2_app", "tool_head_downlink")):
            for field, key in (("NominalPrescaler", "prescaler"), ("NominalSyncJumpWidth", "sync_jump_width"), ("NominalTimeSeg1", "time_segment_1"), ("NominalTimeSeg2", "time_segment_2")):
                found = {int(v) for v in re.findall(rf"\b{handle}\.Init\.{field}\s*=\s*(\d+)\s*;", text)}
                self.assertEqual(found, {board[bus][key]}, f"{handle}.{field}")

    def test_a_wrong_controller_board_timing_is_caught(self):
        for key, value in (("prescaler", 2), ("time_segment_2", 6), ("kernel_clock_hz", 8_000_000)):
            document = copy.deepcopy(self.document)
            document["controller_board"]["tool_head_downlink"][key] = value
            self.assertTrue(pp.board_problems(document, self.contract), key)

    def test_the_tool_head_rate_of_the_can_contract_is_the_downlink_rate(self):
        import can_ids

        self.assertEqual(can_ids.TOOL_HEAD_BIT_RATE_BPS, pp.bit_rate(self.document["controller_board"]["tool_head_downlink"]))

    def test_the_command_line_prints_the_table_and_checks(self):
        self.assertEqual(pp.main(["--check"]), 0)
        self.assertEqual(pp.main([]), 0)


if __name__ == "__main__":
    unittest.main()
