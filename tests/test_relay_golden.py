# =============================================================================
# HYDRA-UMC - tests/test_relay_golden.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The Python fakes and the CM5 relay code against bytes the firmware made.

tests/relay_golden_vectors.json is what src/mcu_stm32g474/relay_ids.h (compiled for the PC) answers. The CM5 fragmenting code, the fake
Robot Controller Board of the bridge tests and the identifier table must agree with it; a host compiler, when there is one, also checks
that the file is what the firmware makes today.
"""
import json
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BRIDGE = ROOT / "src" / "cm5_host" / "spi_bridge"
for path in (ROOT / "tools", BRIDGE, BRIDGE / "tests"):
    sys.path.insert(0, str(path))

import can_ids  # noqa: E402
import gen_relay_golden  # noqa: E402
import host_tests  # noqa: E402
from spi_bridge.relay_tunnel import RelayFrame, build_relay_send_fragments, parse_relay_fragment  # noqa: E402
from test_relay_tunnel import FakeRelayingTier1Transport  # noqa: E402

GOLDEN = json.loads((ROOT / "tests" / "relay_golden_vectors.json").read_text(encoding="utf-8"))


class RelayGoldenTests(unittest.TestCase):
    def test_the_file_covers_the_cases(self):
        self.assertGreaterEqual(len(GOLDEN["frames"]), 30)
        self.assertEqual(len(GOLDEN["answers"]), 0x20)

    def test_the_cm5_fragmenting_matches_the_firmware_fragments(self):
        for entry in GOLDEN["frames"]:
            total = min(entry["dlc"], 8)
            data = bytes.fromhex(entry["data"])
            self.assertEqual(len(data), total)
            fragments = [fragment.hex() for fragment in build_relay_send_fragments(RelayFrame(entry["can_id"], total, data))]
            self.assertEqual(fragments, entry["fragments"], entry)

    def test_the_fake_controller_board_answers_with_the_firmware_fragments(self):
        for entry in GOLDEN["frames"]:
            total = min(entry["dlc"], 8)
            fake = FakeRelayingTier1Transport()
            fake._queue_response(entry["can_id"], bytes.fromhex(entry["data"])[:total])
            self.assertEqual([fragment.hex() for fragment in fake._pending_recv_fragments], entry["fragments"], entry)

    def test_a_firmware_fragment_is_read_back_by_the_cm5(self):
        for entry in GOLDEN["frames"]:
            total = min(entry["dlc"], 8)
            joined = b""
            for fragment in entry["fragments"]:
                can_id, dlc, piece = parse_relay_fragment(bytes.fromhex(fragment))
                self.assertEqual((can_id, dlc), (entry["can_id"], total))
                joined += piece
            self.assertEqual(joined, bytes.fromhex(entry["data"])[:total])

    def test_the_answer_offsets_are_the_ones_of_the_identifier_table(self):
        expected = {
            can_ids.by_name("RELAY_REQUEST").offset: can_ids.by_name("RELAY_RESPONSE").offset,
            can_ids.by_name("RELAY_REQUEST_EXTRA").offset: can_ids.by_name("RELAY_RESPONSE_EXTRA").offset,
        }
        for offset, answer in enumerate(GOLDEN["answers"]):
            self.assertEqual(answer, expected.get(offset, 0), offset)
        for offset, answer in expected.items():
            self.assertEqual(can_ids.by_name("RELAY_RESPONSE").emitter, can_ids.CONTROLLER_BOARD)
            self.assertNotEqual(offset, answer)

    def test_the_error_counter_bytes_are_transmit_then_receive(self):
        for entry in GOLDEN["counters"]:
            self.assertEqual(entry["bytes"], f"{entry['ecr'] & 0xFF:02x}{(entry['ecr'] >> 8) & 0x7F:02x}")

    @unittest.skipIf(host_tests.find_compiler() is None, "no host C compiler (cc/gcc/clang), or local builds are off (ALLOW_LOCAL_BUILD=1 allows them)")
    def test_the_file_is_what_the_firmware_makes_today(self):
        compiler = host_tests.find_compiler()
        assert compiler is not None
        text = gen_relay_golden.firmware_output(compiler)
        self.assertEqual((ROOT / "tests" / "relay_golden_vectors.json").read_text(encoding="utf-8").replace("\r\n", "\n"), text)


if __name__ == "__main__":
    unittest.main()
