# =============================================================================
# HYDRA-UMC - tests/test_build_entry.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The single build entry point and the pinned versions of the two platform scripts."""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

import build  # noqa: E402


class BuildEntryTests(unittest.TestCase):
    def test_both_scripts_pin_the_same_third_party_versions(self):
        sh = build.read_pins(build.SH, build._PIN_SH)
        bat = build.read_pins(build.BAT, build._PIN_BAT)
        self.assertGreaterEqual(len(sh), 12)
        self.assertEqual(build.pin_drift(sh, bat), [])

    def test_drift_is_reported(self):
        problems = build.pin_drift({"A_TAG": "1", "B_TAG": "2"}, {"A_TAG": "9", "C_TAG": "3"})
        self.assertEqual(len(problems), 3)

    def test_no_pins_is_a_problem(self):
        self.assertTrue(build.pin_drift({}, {}))

    def test_command_per_platform(self):
        self.assertEqual(build.command_for("all", False, False)[0], "bash")
        self.assertEqual(build.command_for("g474", True, False)[-2:], ["--clean", "g474"])
        self.assertEqual(build.command_for("all", False, True)[:2], ["cmd", "/c"])


if __name__ == "__main__":
    unittest.main()
