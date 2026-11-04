# =============================================================================
# HYDRA-UMC - tests/test_slot_scan.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The slot scan: the vectors, the host reference, the identifier table and the contract numbers say the same thing."""
import json
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import can_ids  # noqa: E402
import slot_scan  # noqa: E402

VECTORS = json.loads((ROOT / "tests" / "slot_scan_vectors.json").read_text(encoding="utf-8"))
CONTRACT = json.loads((ROOT / "docs" / "bus_contract.json").read_text(encoding="utf-8"))
SCAN = CONTRACT["slot_scan"]


class FrameTests(unittest.TestCase):
    def test_the_probe_vectors_encode_and_decode(self):
        for vector in VECTORS["probes"]:
            self.assertEqual(slot_scan.encode_probe(vector["scan_id"]).hex(), vector["data_hex"], vector["name"])
            self.assertEqual(slot_scan.decode_probe(bytes.fromhex(vector["data_hex"])), vector["scan_id"], vector["name"])

    def test_the_reply_vectors_encode_and_decode(self):
        for vector in VECTORS["replies"]:
            data = slot_scan.encode_reply(vector["board_id"], vector["role"], vector["scan_id"], vector["uid32"])
            self.assertEqual(data.hex(), vector["data_hex"], vector["name"])
            self.assertEqual(slot_scan.decode_reply(data), slot_scan.Reply(vector["board_id"], vector["role"], vector["scan_id"], vector["uid32"]))

    def test_one_reply_is_written_out_by_hand(self):
        # magic 0x5D, board 3 and role 1 (0x13), scan 7, uid little endian, XOR of the seven bytes
        body = [0x5D, 0x13, 0x07, 0xEF, 0xBE, 0xAD, 0xDE]
        check = 0
        for byte in body:
            check ^= byte
        self.assertEqual(slot_scan.encode_reply(3, 1, 7, 0xDEADBEEF), bytes(body + [check]))

    def test_malformed_replies_do_not_decode(self):
        for vector in VECTORS["malformed"]:
            self.assertIsNone(slot_scan.decode_reply(bytes.fromhex(vector["data_hex"])), vector["name"])

    def test_a_probe_with_wrong_complements_does_not_decode(self):
        data = bytearray(slot_scan.encode_probe(4))
        data[7] ^= 1
        self.assertIsNone(slot_scan.decode_probe(bytes(data)))
        self.assertIsNone(slot_scan.decode_probe(slot_scan.encode_probe(4)[:7]))


class ScanTests(unittest.TestCase):
    def test_the_scenarios(self):
        for scenario in VECTORS["scenarios"]:
            heard = {int(slot): slot_scan.Heard(tuple(bytes.fromhex(f) for f in h["frames"]), h["bus_errors"]) for slot, h in scenario["heard"].items()}
            result = slot_scan.scan(heard, scenario["scan_id"])
            self.assertEqual({str(k): v for k, v in result.items()}, scenario["expect"], scenario["name"])

    def test_only_a_single_board_in_its_own_slot_allows_a_write(self):
        names = (slot_scan.OK, slot_scan.ABSENT, slot_scan.DUPLICATE, slot_scan.MISPLACED, slot_scan.NOISY)
        self.assertEqual([name for name in names if slot_scan.write_allowed(name)], [slot_scan.OK])


class DelayTests(unittest.TestCase):
    def test_the_delay_vectors(self):
        for vector in VECTORS["delays"]:
            self.assertEqual(slot_scan.reply_delay_ms(vector["uid32"]), vector["delay_ms"])

    def test_the_longest_delay_fits_in_the_scan_window_with_room_for_the_frames(self):
        timing = SCAN["timing_ms"]
        longest = (timing["reply_steps"] - 1) * timing["reply_step"]
        frame_ms = 135 * 1000 / CONTRACT["buses"]["stack_a"]["nominal_bit_rate_bps"]
        self.assertLess(longest + 8 * frame_ms, timing["scan_window"])

    def test_the_delay_is_spread_over_the_steps_for_boards_numbered_in_a_row(self):
        delays = {slot_scan.reply_delay_ms(uid) for uid in range(1000, 1064)}
        self.assertGreater(len(delays), SCAN["timing_ms"]["reply_steps"] // 2)

    def test_the_step_is_longer_than_two_frames(self):
        frame_ms = 135 * 1000 / CONTRACT["buses"]["stack_a"]["nominal_bit_rate_bps"]
        self.assertGreater(SCAN["timing_ms"]["reply_step"], frame_ms * 2)


class ContractTests(unittest.TestCase):
    def test_the_identifiers_are_in_the_table_with_one_emitter_each(self):
        probe, reply = can_ids.by_name("SLOT_SCAN_PROBE"), can_ids.by_name("SLOT_SCAN_REPLY")
        self.assertEqual((probe.offset, reply.offset), (int(SCAN["probe_offset"], 16), int(SCAN["reply_offset"], 16)))
        self.assertEqual((probe.emitter, reply.emitter), (can_ids.HOST, can_ids.BOOTLOADER))

    def test_the_magic_numbers_differ(self):
        self.assertNotEqual(SCAN["probe_magic"], SCAN["reply_magic"])

    def test_the_page_names_the_numbers(self):
        text = (ROOT / "docs" / "SLOT_SCAN.md").read_text(encoding="utf-8")
        for needle in ("0x1E", "0x1F", "slot_scan.py", "slot_scan_vectors.json", "bus_contract.json", "DUPLICATE", "Pending"):
            self.assertIn(needle, text)
        for name, value in SCAN["timing_ms"].items():
            self.assertIn(f"`{value}`", text, name)


if __name__ == "__main__":
    unittest.main()
