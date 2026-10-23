# =============================================================================
# HYDRA-UMC - tests/test_can_ids.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""One emitter per CAN identifier in the relay table (host reference)."""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

import can_ids  # noqa: E402


class CanIdTableTests(unittest.TestCase):
    def test_no_identifier_has_two_messages(self):
        offsets = [m.offset for m in can_ids.MESSAGES]
        self.assertEqual(len(offsets), len(set(offsets)))

    def test_names_are_unique(self):
        names = [m.name for m in can_ids.MESSAGES]
        self.assertEqual(len(names), len(set(names)))

    def test_every_offset_is_inside_the_slot_window(self):
        for message in can_ids.MESSAGES:
            self.assertTrue(0 <= message.offset < can_ids.SLOT_WINDOW, message.name)

    def test_a_request_and_its_answer_have_different_emitters_and_identifiers(self):
        for request, answer in (("RELAY_REQUEST", "RELAY_RESPONSE"), ("RELAY_REQUEST_EXTRA", "RELAY_RESPONSE_EXTRA")):
            a, b = can_ids.by_name(request), can_ids.by_name(answer)
            self.assertNotEqual(a.offset, b.offset)
            self.assertNotEqual(a.emitter, b.emitter)

    def test_the_host_poll_does_not_use_an_identifier_the_bootloader_transmits(self):
        poll = can_ids.by_name("POLL_STATUS")
        bootloader = {m.offset for m in can_ids.MESSAGES if m.emitter == can_ids.BOOTLOADER}
        self.assertNotIn(poll.offset, bootloader)

    def test_identifier_adds_the_slot_base_and_checks_it(self):
        self.assertEqual(can_ids.identifier(0x100, can_ids.by_name("RELAY_RESPONSE")), 0x116)
        with self.assertRaises(ValueError):
            can_ids.identifier(0x101, can_ids.by_name("RELAY_RESPONSE"))
        with self.assertRaises(ValueError):
            can_ids.identifier(0x800, can_ids.by_name("RELAY_RESPONSE"))
        with self.assertRaises(KeyError):
            can_ids.by_name("NOPE")


if __name__ == "__main__":
    unittest.main()
