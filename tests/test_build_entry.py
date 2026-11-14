# =============================================================================
# HYDRA-UMC - tests/test_build_entry.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The single build entry point and the pinned versions of the build script."""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

import build  # noqa: E402


class BuildEntryTests(unittest.TestCase):
    def test_the_script_pins_the_third_party_versions(self):
        pins = build.read_pins(build.SCRIPT)
        self.assertGreaterEqual(len(pins), 12)
        self.assertEqual(build.pin_problems(pins), [])

    def test_a_repository_without_its_tag_is_reported(self):
        problems = build.pin_problems({"A_TAG": "1", "B_REPO": "2", "B_TAG": "3"})
        self.assertEqual(len(problems), 1)

    def test_no_pins_is_a_problem(self):
        self.assertTrue(build.pin_problems({}))

    def test_command_for_the_target(self):
        self.assertEqual(build.command_for("all", False)[1:], [str(build.SCRIPT)])
        self.assertEqual(build.command_for("g474", True)[-2:], ["--clean", "g474"])


if __name__ == "__main__":
    unittest.main()
