# =============================================================================
# HYDRA-UMC - tests/test_estop_contract.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The emergency stop on identifier 0x001: the vectors, the host reference and the contract numbers say the same thing."""
import json
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import can_ids  # noqa: E402
import estop_can  # noqa: E402

VECTORS = json.loads((ROOT / "tests" / "estop_vectors.json").read_text(encoding="utf-8"))
CONTRACT = json.loads((ROOT / "docs" / "bus_contract.json").read_text(encoding="utf-8"))
ESTOP = CONTRACT["emergency_stop"]


class FrameTests(unittest.TestCase):
    def test_the_frame_vectors_encode_and_decode(self):
        for vector in VECTORS["frames"]:
            data = estop_can.encode(vector["kind"], vector["detail"], vector["source"], vector["epoch"], vector["seq"])
            self.assertEqual(data.hex(), vector["data_hex"], vector["name"])
            frame = estop_can.decode(bytes.fromhex(vector["data_hex"]))
            self.assertEqual(frame, estop_can.Frame(vector["kind"], vector["detail"], vector["source"], vector["epoch"], vector["seq"]), vector["name"])

    def test_one_frame_is_written_out_by_hand(self):
        # kind 1, reason 1, source 0, epoch 1, sequence 1 (little endian), then the complements 0xFE 0xFE
        self.assertEqual(estop_can.encode(1, 1, 0, 1, 1), bytes([0x01, 0x01, 0x00, 0x01, 0x01, 0x00, 0xFE, 0xFE]))

    def test_the_malformed_vectors_do_not_decode(self):
        for vector in VECTORS["malformed"]:
            self.assertIsNone(estop_can.decode(bytes.fromhex(vector["data_hex"])), vector["name"])

    def test_every_frame_is_eight_bytes(self):
        for vector in VECTORS["frames"]:
            self.assertEqual(len(bytes.fromhex(vector["data_hex"])), ESTOP["frame"]["dlc"], vector["name"])

    def test_the_sequence_order_is_modular(self):
        self.assertTrue(estop_can.seq_newer(1, None))
        self.assertTrue(estop_can.seq_newer(11, 10))
        self.assertTrue(estop_can.seq_newer(0, 65535))
        self.assertFalse(estop_can.seq_newer(10, 10))
        self.assertFalse(estop_can.seq_newer(10, 11))
        self.assertFalse(estop_can.seq_newer(10 + 0x8000, 10))


class ScenarioTests(unittest.TestCase):
    def test_the_scenarios_hold(self):
        self.assertEqual(VECTORS["alive_timeout_ms"], ESTOP["timing_ms"]["alive_timeout"])
        for scenario in VECTORS["scenarios"]:
            node = estop_can.Node()
            self.assertEqual((node.state, node.drivers_enabled), (estop_can.LATCHED, False), scenario["name"])
            for step in scenario["steps"]:
                label = f"{scenario['name']} at {step['t']} ms"
                accepted = None
                if "frame" in step:
                    node.on_frame(step["t"], bytes.fromhex(step["frame"]))
                elif step.get("do") == "tick":
                    node.tick(step["t"])
                elif step.get("do") == "bus_off":
                    node.on_bus_off()
                elif step.get("do") == "enable":
                    accepted = node.enable_drivers()
                else:
                    self.fail(f"unknown step in {label}")
                expect = step.get("expect")
                if expect is None:
                    continue
                self.assertEqual(node.state, expect["state"], label)
                self.assertEqual(node.drivers_enabled, expect["drivers_enabled"], label)
                if "accepted" in expect:
                    self.assertEqual(accepted, expect["accepted"], label)

    def test_a_trip_always_cuts_the_drivers_whatever_the_state(self):
        for reason in estop_can.REASONS:
            node = estop_can.Node()
            node.on_frame(0, estop_can.encode(estop_can.KIND_ALIVE, estop_can.ALIVE_TRIPPED, 0, 1, 1))
            node.on_frame(1, estop_can.encode(estop_can.KIND_RELEASE, 0, 0, 1, 2))
            self.assertTrue(node.enable_drivers())
            node.on_frame(2, estop_can.encode(estop_can.KIND_TRIP, reason, 1, 2, 3))
            self.assertEqual((node.state, node.drivers_enabled), (estop_can.LATCHED, False))

    def test_a_release_alone_never_enables_the_drivers(self):
        node = estop_can.Node()
        node.on_frame(0, estop_can.encode(estop_can.KIND_ALIVE, estop_can.ALIVE_TRIPPED, 0, 1, 1))
        node.on_frame(1, estop_can.encode(estop_can.KIND_RELEASE, 0, 0, 1, 2))
        self.assertEqual((node.state, node.drivers_enabled), (estop_can.READY, False))


class ContractNumbersTests(unittest.TestCase):
    def test_the_identifier_is_the_highest_priority_and_outside_every_slot(self):
        self.assertEqual(int(ESTOP["can_id"], 16), estop_can.ESTOP_CAN_ID)
        self.assertEqual(can_ids.ESTOP_CAN_ID, estop_can.ESTOP_CAN_ID)
        base = int(CONTRACT["buses"]["stack_a"]["slot_base"], 16)
        self.assertLess(estop_can.ESTOP_CAN_ID, base)
        self.assertGreater(estop_can.ESTOP_CAN_ID, 0, "0x000 is left unused on purpose")
        for bus in ESTOP["buses"]:
            self.assertIn(bus, CONTRACT["buses"])

    def test_the_kinds_match_the_reference(self):
        self.assertEqual({k: int(v, 16) for k, v in ESTOP["kinds"].items()}, {"TRIP": estop_can.KIND_TRIP, "RELEASE": estop_can.KIND_RELEASE, "ALIVE": estop_can.KIND_ALIVE})

    def test_the_timing_is_coherent(self):
        t = ESTOP["timing_ms"]
        self.assertGreaterEqual(t["alive_timeout"], 3 * t["alive_period"], "a single lost frame must not latch the stack")
        self.assertLessEqual(t["trip_repeat"], t["alive_period"])
        self.assertGreater(t["release_ack_timeout"], t["release_repeat"])
        self.assertEqual(estop_can.load_timing(), t)

    def test_the_stop_reaches_the_nodes_in_a_fraction_of_the_reaction_budget(self):
        stack = CONTRACT["buses"]["stack_a"]
        frame_us = estop_can.worst_case_frame_bits() * 1_000_000 / stack["nominal_bit_rate_bps"]
        self.assertEqual(estop_can.worst_case_frame_bits(), 135)
        # at most one frame already on the wire is waited for, then the stop itself: two frame times, whatever the load
        self.assertLess(2 * frame_us / 1000, ESTOP["timing_ms"]["node_reaction_max"] / 10)

    def test_the_alive_traffic_is_a_small_share_of_the_bus(self):
        stack = CONTRACT["buses"]["stack_a"]
        bits_per_second = estop_can.worst_case_frame_bits() * 1000 / ESTOP["timing_ms"]["alive_period"]
        self.assertLess(bits_per_second / stack["nominal_bit_rate_bps"], 0.01)

    def test_the_state_message_has_its_own_identifier_in_the_slot_window(self):
        message = can_ids.by_name("ESTOP_STATE")
        self.assertEqual(message.emitter, can_ids.CONTROLLER_BOARD)
        self.assertNotEqual(can_ids.identifier(int(CONTRACT["buses"]["stack_a"]["slot_base"], 16), message), estop_can.ESTOP_CAN_ID)

    def test_the_contract_page_names_the_numbers(self):
        text = (ROOT / "docs" / "ESTOP_CAN.md").read_text(encoding="utf-8")
        for needle in ("0x001", "TRIP", "RELEASE", "ALIVE", "bus_contract.json", "estop_vectors.json", "Pending"):
            self.assertIn(needle, text)
        for name, value in ESTOP["timing_ms"].items():
            self.assertIn(f"`{value}`", text, name)


if __name__ == "__main__":
    unittest.main()
