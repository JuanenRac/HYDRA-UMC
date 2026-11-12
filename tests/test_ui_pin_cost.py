# =============================================================================
# HYDRA-UMC - tests/test_ui_pin_cost.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The estimate of the time of one PIN check on the Cortex-M7 (tools/ui_pin_cost.py): the count of compressions is exact (the C test holds the formula to the count of the C code),
the cycles per compression are an estimate in config/ui.cfg, and a setting that makes a check too slow is caught. Evidence: simulator (arithmetic)."""
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import ui_pin_cost as cost  # noqa: E402


class CostTests(unittest.TestCase):
    def test_the_compressions_follow_the_formula_of_pbkdf2_with_hmac(self):
        self.assertEqual([cost.compressions(n) for n in (1, 2, 1000, 10000)], [4, 6, 2002, 20002])

    def test_the_estimate_is_under_the_limit_and_the_hash_peripheral_is_faster(self):
        n = cost.estimate()
        self.assertTrue(n["fits"])
        self.assertLess(n["hardware_ms"], n["software_ms"])
        self.assertEqual(n["clock_hz"], 400_000_000)
        self.assertAlmostEqual(n["software_ms"], 20002 * 2600 / 400e6 * 1000, places=6)

    def test_guessing_is_bounded_by_the_lockout_and_not_by_the_check(self):
        n = cost.estimate()
        self.assertEqual((n["tries_per_lockout"], n["lockout_s"], n["pin_space"]), (3, 30.0, 10000))
        self.assertAlmostEqual(n["hours_to_try_every_pin"], 10000 / 3 * 30 / 3600, places=6)
        self.assertGreater(n["hours_to_try_every_pin"], 24)

    def test_the_report_says_what_is_exact_and_what_is_an_estimate(self):
        text = " ".join(cost.report(cost.estimate()))
        self.assertIn("(exact)", text)
        self.assertIn("estimates", text)
        self.assertEqual(cost.main(), 0)

    def test_a_slower_core_or_a_heavier_hash_is_caught(self):
        original = cost.ui_pin.load_settings
        try:
            cost.ui_pin.load_settings = lambda *a: dict(original(), pin_cycles_per_block=20000)
            self.assertFalse(cost.estimate()["fits"])
            self.assertEqual(cost.main(), 1)
        finally:
            cost.ui_pin.load_settings = original


if __name__ == "__main__":
    unittest.main()
