# =============================================================================
# HYDRA-UMC - tests/test_zones_frame.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The filter of the brain for the frame of the safety-zones service on identifier 0x002 (tools/zones_frame.py, its C twin src/mcu_stm32h757/CM4/zones_frame.h): what makes the brain
trip with the reason safety-zone, that a replay cannot keep a dead service alive, that the switch ships off, the wiring in the brain and the vectors the C test replays. Evidence: simulator."""
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tests"))

import estop_can  # noqa: E402
import gen_zones_vectors  # noqa: E402
import sim_hil_bench  # noqa: E402
import zones_frame as z  # noqa: E402

CM4 = ROOT / "src" / "mcu_stm32h757" / "CM4"


class FilterTests(unittest.TestCase):
    def test_the_frame_and_the_crc_are_those_the_zones_bridge_of_the_bench_already_uses(self):
        for data in (b"", b"\x01", bytes(range(7)), bytes([1, 1, 5, 0, 3, 0, 1])):
            self.assertEqual(z.crc8(data), sim_hil_bench.crc8(data))
        self.assertEqual(z.frame(z.KIND_STOP, 300, 7, 2), sim_hil_bench.zones_frame(sim_hil_bench.ZONES_KIND_STOP, 300, 7, 2))
        self.assertEqual(z.REASON_SAFETY_ZONE, estop_can.REASON_SAFETY_ZONE if hasattr(estop_can, "REASON_SAFETY_ZONE") else 0x04)
        self.assertEqual(z.ZONES_CAN_ID, 2)
        self.assertNotEqual(z.ZONES_CAN_ID, estop_can.ESTOP_CAN_ID)  # never on 0x001

    def test_a_stop_request_trips_and_only_on_the_identifier_of_the_service(self):
        f = z.ZonesFilter()
        self.assertTrue(f.on_frame(0, 2, z.frame(z.KIND_STOP, 1)))
        self.assertFalse(f.on_frame(1, 3, z.frame(z.KIND_STOP, 1)))
        self.assertFalse(f.on_frame(2, 0x001, z.frame(z.KIND_STOP, 1)))
        self.assertFalse(f.on_frame(3, 0x7A0, z.frame(z.KIND_STOP, 1)))

    def test_every_damaged_frame_trips_because_a_damaged_frame_can_only_cause_a_stop(self):
        good = z.frame(z.KIND_HEARTBEAT, 1)
        for index in range(8):
            damaged = bytearray(good)
            damaged[index] ^= 0x01
            self.assertTrue(z.ZonesFilter().on_frame(0, 2, bytes(damaged)), index)
        for length in (0, 1, 7, 9):
            self.assertTrue(z.ZonesFilter().on_frame(0, 2, (good + good)[:length]), length)
        self.assertTrue(z.ZonesFilter().on_frame(0, 2, z.frame(z.KIND_HEARTBEAT, 1, version=2)))

    def test_silence_after_the_first_heartbeat_trips_once_and_the_next_heartbeat_arms_it_again(self):
        f = z.ZonesFilter()
        self.assertFalse(f.tick(10_000))  # never before the first heartbeat
        f.on_frame(0, 2, z.frame(z.KIND_HEARTBEAT, 1))
        self.assertFalse(f.tick(1000))
        self.assertTrue(f.tick(1001))
        self.assertFalse(f.tick(2000))  # once
        f.on_frame(2500, 2, z.frame(z.KIND_HEARTBEAT, 2))
        self.assertFalse(f.tick(3000))
        self.assertTrue(f.tick(3600))

    def test_a_replayed_or_old_heartbeat_cannot_keep_a_dead_service_alive(self):
        f = z.ZonesFilter()
        f.on_frame(0, 2, z.frame(z.KIND_HEARTBEAT, 10))
        for t, seq in ((300, 10), (600, 9), (900, 10)):
            f.on_frame(t, 2, z.frame(z.KIND_HEARTBEAT, seq))
        self.assertEqual(f.ignored, 3)
        self.assertTrue(f.tick(1100))

    def test_the_sequence_and_the_clock_wrap(self):
        f = z.ZonesFilter()
        f.on_frame(0xFFFFFF00, 2, z.frame(z.KIND_HEARTBEAT, 65535))
        f.on_frame(0xFFFFFF00 + 600, 2, z.frame(z.KIND_HEARTBEAT, 0))  # newer across the wrap
        self.assertEqual(f.ignored, 0)
        self.assertFalse(f.tick(0xFFFFFF00 + 1500))
        self.assertTrue(f.tick(0xFFFFFF00 + 1700))

    def test_an_unknown_valid_kind_is_ignored_and_a_stop_after_it_still_trips(self):
        f = z.ZonesFilter()
        self.assertFalse(f.on_frame(0, 2, z.frame(9, 1)))
        self.assertTrue(f.on_frame(1, 2, z.frame(z.KIND_STOP, 2)))

    def test_with_the_switch_off_nothing_trips(self):
        f = z.ZonesFilter(enabled=False)
        self.assertFalse(f.on_frame(0, 2, z.frame(z.KIND_STOP, 1)))
        self.assertFalse(f.on_frame(0, 2, b"damaged"))
        self.assertFalse(f.tick(10**6))


class SourceTests(unittest.TestCase):
    def test_the_switch_ships_off_and_the_filter_is_programmed_only_when_it_is_on(self):
        header = (CM4 / "zones_frame.h").read_text(encoding="utf-8")
        self.assertRegex(header, r"#define HYDRA_ZONES_CAN_FILTER 0")
        brain = (CM4 / "KinematicBrainCan.c").read_text(encoding="utf-8")
        block = re.search(r"#if HYDRA_ZONES_CAN_FILTER\s+sf\.FilterIndex = 2;(.*?)#endif", brain, re.S)
        self.assertIsNotNone(block)
        self.assertIn("ZONES_CAN_ID", block.group(1))
        self.assertIn("ESTOP_REASON_SAFETY_ZONE", brain)
        self.assertIn("ZonesFilter_Tick", brain)
        self.assertIn("ZonesFilter_OnFrame", brain)

    def test_the_constants_of_the_c_twin_are_those_of_the_reference(self):
        header = (CM4 / "zones_frame.h").read_text(encoding="utf-8")
        for name, value in (("ZONES_CAN_ID", z.ZONES_CAN_ID), ("ZONES_KIND_STOP", z.KIND_STOP), ("ZONES_KIND_HEARTBEAT", z.KIND_HEARTBEAT), ("ZONES_VERSION", z.VERSION)):
            self.assertRegex(header, rf"#define {name} (0x0*{value:X}|{value})u", name)
        self.assertRegex(header, rf"#define ZONES_MAX_AGE_MS {z.MAX_AGE_MS}u")
        self.assertIn("0x07u", header)  # the polynomial of the CRC-8
        brain_reason = (CM4 / "estop_can.h").read_text(encoding="utf-8")
        self.assertRegex(brain_reason, r"ESTOP_REASON_SAFETY_ZONE\s+0x04u")

    def test_the_vectors_the_c_test_replays_are_what_the_reference_says(self):
        self.assertEqual(gen_zones_vectors.main(["--check"]), 0)

    def test_the_docs_name_the_switch_and_the_identifier(self):
        switches = (ROOT / "docs" / "FIRMWARE_SWITCHES.md").read_text(encoding="utf-8")
        self.assertIn("HYDRA_ZONES_CAN_FILTER", switches)
        estop = (ROOT / "docs" / "ESTOP_CAN.md").read_text(encoding="utf-8")
        self.assertIn("0x002", estop)


if __name__ == "__main__":
    unittest.main()
