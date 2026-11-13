# =============================================================================
# HYDRA-UMC - tests/test_board_pin_map.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The pins and buses of the H757 board in the platform profile are the ones of the netlist of the board. Evidence: simulator (the table is checked against the numbers read from the netlist; nothing has run on a board)."""
import copy
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import platform_profile as pp  # noqa: E402


class BoardPinMapTests(unittest.TestCase):
    def setUp(self):
        self.document = pp.load()
        self.p = self.document["profiles"]["h757_target"]

    def test_the_host_link_is_spi3_on_the_pins_the_netlist_gives(self):
        spi = self.p["host_spi"]
        self.assertEqual(spi["peripheral"], "SPI3")
        self.assertEqual(spi["pins"], {"sck": "PB3", "miso": "PB4", "mosi": "PB5", "nss": "PA15"})
        self.assertEqual(spi["role"], "master")
        self.assertEqual(spi["clock_hz"], self.p["clocks_hz"][spi["kernel_clock_key"]] // spi["prescaler"])

    def test_spi1_spi2_and_spi4_do_not_exist_on_the_board(self):
        text = str(self.p)
        for name in ("SPI1", "SPI2", "SPI4"):
            self.assertNotIn(f"'{name}'", text, name)
        self.assertFalse([k for k in self.p["clocks_hz"] if k in ("kernel_spi1", "kernel_spi2", "kernel_spi4")])

    def test_the_stepper_drivers_are_on_the_expansion_cards_not_on_the_base_board(self):
        spi6 = self.p["new_peripherals"]["SPI6"]
        self.assertEqual(spi6["pins"], {"sck": "PG13", "miso": "PG12", "mosi": "PG14", "cs": "PG3"})
        self.assertIn("expansion cards", spi6["use"])
        self.assertEqual(self.p["new_peripherals"]["I2C3"]["pins"], {"scl": "PA8", "sda": "PC9"})

    def test_each_slot_has_its_own_five_lines_and_they_are_all_different(self):
        slots = self.p["expansion"]["slots"]
        self.assertEqual(sorted(slots), ["1", "2", "3"])
        self.assertEqual(slots["1"], {"enable": "PJ3", "fault_n": "PJ2", "irq": "PJ1", "present_n": "PJ0", "reset_n": "PJ4"})
        self.assertEqual(slots["3"]["reset_n"], "PJ14")
        pins = [pin for slot in slots.values() for pin in slot.values()]
        self.assertEqual(len(pins), 15)
        self.assertEqual(len(set(pins)), 15)

    def test_the_sdram_has_two_banks_of_two_chips_each_with_its_own_select(self):
        sdram = self.p["new_peripherals"]["FMC"]["sdram"]
        self.assertEqual(sdram["bank_pins"]["bank1"]["cs"], "PC2")
        self.assertEqual(sdram["bank_pins"]["bank2"]["cke"], "PH7")
        self.assertEqual(sdram["byte_lanes"], {"nbl0": "PE0", "nbl1": "PE1", "nbl2": "PI4", "nbl3": "PI5"})
        self.assertEqual(sdram["bus_width_bits"], 32)

    def test_no_pin_is_claimed_twice_and_the_table_is_large_enough_to_mean_something(self):
        self.assertEqual(pp.pin_problems("h757_target", self.p), [])
        self.assertGreaterEqual(len(pp.pin_assignments(self.p)), 100)

    def test_the_analog_pads_are_separate_from_the_digital_pin_of_the_same_port_pin(self):
        io = self.p["board_io"]
        self.assertEqual(io["adc_voltage"]["v24"], "PA0_C")
        self.assertEqual(io["adc_voltage"]["usb5v"], "PA0")
        self.assertEqual(pp.pin_problems("h757_target", self.p), [])

    def test_a_clash_between_a_slot_line_and_the_host_link_is_reported(self):
        broken = copy.deepcopy(self.p)
        broken["expansion"]["slots"]["2"]["enable"] = "PB3"
        problems = pp.pin_problems("h757_target", broken)
        self.assertEqual(len(problems), 1)
        self.assertIn("PB3", problems[0])

    def test_the_ready_line_is_empty_until_the_schematic_draws_it(self):
        ready = self.p["host_control"]["ready"]
        self.assertIsNone(ready["pin"])
        self.assertIn("PENDING-DECISION", ready["status"])
        self.assertEqual(self.p["host_control"]["pins"]["power_en"], "PI11")

    def test_the_bench_board_profile_lists_no_pins(self):
        self.assertEqual(pp.pin_assignments(self.document["profiles"]["h745_current"]), [])


if __name__ == "__main__":
    unittest.main()
