# =============================================================================
# HYDRA-UMC - tests/test_rdp_plan.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The read-out protection plan: levels of the option byte, the refusal of level 2, the command and the device identity."""
import hashlib
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import rdp_plan  # noqa: E402


class RdpPlanTests(unittest.TestCase):
    def test_the_levels_of_the_option_byte(self):
        self.assertEqual(rdp_plan.decode(0xAA), 0)
        self.assertEqual(rdp_plan.decode(0xCC), 2)
        self.assertEqual(rdp_plan.decode(0xBB), 1)
        self.assertEqual(rdp_plan.decode(0x00), 1)  # anything else is level 1
        with self.assertRaises(ValueError):
            rdp_plan.decode(0x100)

    def test_level_two_is_never_offered(self):
        with self.assertRaises(ValueError):
            rdp_plan.option_byte(2)
        self.assertNotEqual(rdp_plan.decode(rdp_plan.option_byte(1)), 2)
        self.assertEqual(rdp_plan.main(["command", "--level", "2"]), 1)

    def test_the_command(self):
        self.assertEqual(rdp_plan.command(1), "STM32_Programmer_CLI -c port=SWD mode=UR -ob RDP=0xBB")
        self.assertEqual(rdp_plan.command(0, "prog"), "prog -c port=SWD mode=UR -ob RDP=0xAA")

    def test_the_device_identity_is_a_domain_separated_hash_of_the_unique_id(self):
        uid = bytes(range(12))
        self.assertEqual(rdp_plan.device_id(uid), hashlib.sha256(b"hydra-umc-device-id/1" + uid).digest())
        self.assertNotEqual(rdp_plan.device_id(uid), rdp_plan.device_id(bytes(12)))
        with self.assertRaises(ValueError):
            rdp_plan.device_id(b"short")


if __name__ == "__main__":
    unittest.main()
