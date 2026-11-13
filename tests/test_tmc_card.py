# =============================================================================
# HYDRA-UMC - tests/test_tmc_card.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The datagrams of the TMC5160 cards and the choice of the card on SPI6: the Python reference with the same golden datagrams as the C test, and the pins it relies on.
Evidence: simulator (nothing has run on a board)."""
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import platform_profile as pp  # noqa: E402
import tmc_card as tc  # noqa: E402

HEADER = (ROOT / "src/mcu_stm32h757/Common/tmc_card.h").read_text(encoding="utf-8")


class DatagramTests(unittest.TestCase):
    def test_golden_datagrams(self):
        self.assertEqual(tc.encode_write(0x00, 4), bytes([0x80, 0, 0, 0, 4]))
        self.assertEqual(tc.encode_write(0x10, 0x00071F0A), bytes([0x90, 0x00, 0x07, 0x1F, 0x0A]))
        self.assertEqual(tc.encode_write(0xFF, 0xDEADBEEF), bytes([0xFF, 0xDE, 0xAD, 0xBE, 0xEF]))
        self.assertEqual(tc.encode_read(0x04), bytes([0x04, 0, 0, 0, 0]))
        self.assertEqual(tc.encode_read(0xFF)[0], 0x7F)
        self.assertEqual(tc.decode_reply(bytes([0x09, 0x12, 0x34, 0x56, 0x78])), (0x09, 0x12345678))

    def test_the_c_twin_has_the_same_golden_datagrams(self):
        source = (ROOT / "tests/test_tmc_card.c").read_text(encoding="utf-8")
        for fragment in ("0x90 && tx[1] == 0x00 && tx[2] == 0x07 && tx[3] == 0x1F && tx[4] == 0x0A", "0x80 && tx[1] == 0 && tx[2] == 0 && tx[3] == 0 && tx[4] == 0x04", "0x12345678UL"):
            self.assertIn(fragment, source)
        self.assertNotIn("HAL_", HEADER)


class CardTests(unittest.TestCase):
    def make(self, answers=None, fail=False):
        queue = list(answers or [bytes(5), bytes(5)])
        return tc.Card(lambda tx: None if fail else queue.pop(0))

    def test_the_select_lines_change_only_while_the_strobe_is_high(self):
        card = self.make([bytes([1, 0, 0, 0, 0])])
        self.assertEqual(card.write(2, 0b111, 0, 4)[0], tc.OK)
        self.assertEqual([entry[0] for entry in card.log], ["cs", "select", "cs", "transfer", "cs", "select"])
        self.assertEqual(card.log[1], ("select", 2))
        self.assertEqual(card.log[-1], ("select", 0))
        self.assertEqual((card.cs, card.select), (1, 0))

    def test_a_read_is_two_datagrams_and_returns_the_second(self):
        card = self.make([bytes([8, 0, 0, 0, 0]), bytes([8, 0xCA, 0, 0, 0xFE])])
        status, reply = card.read(3, 0b100, 4)
        self.assertEqual((status, reply), (tc.OK, (8, 0xCA0000FE)))
        self.assertEqual(sum(1 for e in card.log if e[0] == "transfer"), 2)

    def test_a_slot_that_does_not_exist_or_is_not_ready_moves_nothing(self):
        card = self.make()
        for slot, mask, expected in ((0, 7, tc.ERR_SLOT), (4, 7, tc.ERR_SLOT), (1, 0b110, tc.ERR_NOT_READY), (3, 0, tc.ERR_NOT_READY)):
            self.assertEqual(card.write(slot, mask, 0, 0)[0], expected)
        self.assertEqual(card.read(2, 0, 0)[0], tc.ERR_NOT_READY)
        self.assertEqual(card.log, [])

    def test_a_failing_peripheral_still_releases_the_card(self):
        card = self.make(fail=True)
        self.assertEqual(card.write(1, 1, 0, 0)[0], tc.ERR_TRANSFER)
        self.assertEqual((card.cs, card.select), (1, 0))
        self.assertEqual(card.read(1, 1, 0)[0], tc.ERR_TRANSFER)
        self.assertEqual(sum(1 for e in card.log if e[0] == "transfer"), 2)  # one for the write, only the first datagram of the read


class ProfileTests(unittest.TestCase):
    def test_the_select_lines_and_the_strobe_are_pins_of_the_profile(self):
        profile = pp.load()["profiles"]["h757_target"]
        self.assertEqual(profile["expansion"]["lines"]["gpio1"], "PE5")
        self.assertEqual(profile["expansion"]["lines"]["gpio2"], "PE6")
        self.assertEqual(profile["new_peripherals"]["SPI6"]["pins"]["cs"], "PG3")
        self.assertEqual(tc.SLOTS, len(profile["expansion"]["slots"]))

    def test_the_c_test_is_in_both_lists(self):
        self.assertIn("tests/test_tmc_card.c", (ROOT / "tools/host_tests.py").read_text(encoding="utf-8"))
        self.assertIn("test_tmc_card.c", (ROOT / "build_firmware.sh").read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
