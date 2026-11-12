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


class CanIdContractTests(unittest.TestCase):
    """The table is the contract of the SDK (CanIds): the copy in docs/ is the one this module reads."""

    SDK_COPY = Path(__file__).resolve().parents[2] / "HYDRA-UMC-SDK" / "contracts" / "can_ids.json"

    def test_the_numbers_come_from_the_contract_file(self):
        import json

        document = json.loads((Path(__file__).resolve().parents[1] / "docs" / "can_ids.json").read_text(encoding="utf-8"))
        self.assertEqual(document["schema_version"], "1.0")
        self.assertEqual(can_ids.SLOT_WINDOW, document["slot_window"])
        self.assertEqual(can_ids.SLOT_BASE, document["slot_base"])
        self.assertEqual(can_ids.ESTOP_CAN_ID, document["estop_id"])
        self.assertEqual([(m.name, m.offset, m.emitter) for m in can_ids.MESSAGES], [(m["name"], m["offset"], m["emitter"]) for m in document["messages"]])

    def test_the_contract_agrees_with_the_bus_contract_of_this_repository(self):
        import json

        docs = Path(__file__).resolve().parents[1] / "docs"
        bus = json.loads((docs / "bus_contract.json").read_text(encoding="utf-8"))["buses"]["stack_a"]
        self.assertEqual(int(bus["slot_base"], 16), can_ids.SLOT_BASE)
        self.assertEqual(bus["slot_window"], can_ids.SLOT_WINDOW)
        self.assertEqual(bus["slots"], json.loads((docs / "can_ids.json").read_text(encoding="utf-8"))["slots"])
        self.assertEqual(int(json.loads((docs / "bus_contract.json").read_text(encoding="utf-8"))["emergency_stop"]["can_id"], 16), can_ids.ESTOP_CAN_ID)

    def test_every_identifier_of_every_slot_fits_in_eleven_bits_and_the_stop_is_below_them(self):
        top = can_ids.SLOT_BASE + 8 * can_ids.SLOT_WINDOW - 1
        self.assertLessEqual(top, 0x7FF)
        self.assertLess(can_ids.ESTOP_CAN_ID, can_ids.SLOT_BASE)
        for message in can_ids.MESSAGES:
            for slot in range(8):
                self.assertLessEqual(can_ids.identifier(can_ids.SLOT_BASE + slot * can_ids.SLOT_WINDOW, message), 0x7FF)

    @unittest.skipUnless(SDK_COPY.is_file(), "HYDRA-UMC-SDK is not next to this repository")
    def test_the_copy_is_the_contract_of_the_sdk(self):
        ours = (Path(__file__).resolve().parents[1] / "docs" / "can_ids.json").read_bytes().replace(b"\r\n", b"\n")
        theirs = self.SDK_COPY.read_bytes().replace(b"\r\n", b"\n")
        self.assertEqual(ours, theirs, "docs/can_ids.json differs from contracts/can_ids.json of the SDK: copy it again")


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

    def test_the_signed_update_frames_match_the_bootloaders(self):
        import re

        root = Path(__file__).resolve().parents[1]
        headers = [root / "src/mcu_stm32g474/boot/bootloader_common.h", root / "src/mcu_stm32h745/CM4/boot/bootloader_common.h", root / "src/mcu_stm32h745/CM7/boot/bootloader_common.h"]
        names = {
            "BOOT_IMAGE_HEADER": ("OFS_IMAGE_HEADER", can_ids.HOST),
            "BOOT_DOWNGRADE_NONCE_REQUEST": ("OFS_DOWNGRADE_NONCE_REQUEST", can_ids.HOST),
            "BOOT_DOWNGRADE_TOKEN": ("OFS_DOWNGRADE_TOKEN", can_ids.HOST),
            "BOOT_DOWNGRADE_NONCE_RESPONSE": ("OFS_DOWNGRADE_NONCE_RESPONSE", can_ids.BOOTLOADER),
        }
        for header in headers:
            text = header.read_text(encoding="utf-8")
            for table_name, (define, emitter) in names.items():
                message = can_ids.by_name(table_name)
                self.assertEqual(message.emitter, emitter, table_name)
                value = int(re.search(rf"^#define {define}\s+(0x[0-9A-Fa-f]+)", text, re.M).group(1), 16)
                self.assertEqual(value, message.offset, f"{header.parent.parent.name}: {define}")

    def test_the_relay_answers_use_their_own_identifiers_in_the_firmware_and_the_host(self):
        import re

        root = Path(__file__).resolve().parents[1]
        expected = {
            "RELAY_RESPONSE": ("OFS_AXIS_STATUS_RESPONSE", can_ids.CONTROLLER_BOARD),
            "POLL_STATUS": ("OFS_POLL_STATUS", can_ids.HOST),
            "RELAY_RESPONSE_EXTRA": ("OFS_RELAY_RECV_RESPONSE", can_ids.CONTROLLER_BOARD),
        }
        headers = [root / "src/mcu_stm32g474/boot/bootloader_common.h", root / "src/mcu_stm32h745/CM4/boot/bootloader_common.h"]
        protocol = (root / "src/cm5_host/spi_bridge/spi_bridge/protocol.py").read_text(encoding="utf-8")
        for table_name, (define, emitter) in expected.items():
            message = can_ids.by_name(table_name)
            self.assertEqual(message.emitter, emitter, table_name)
            for header in headers:
                value = int(re.search(rf"^#define {define}\s+(0x[0-9A-Fa-f]+)", header.read_text(encoding="utf-8"), re.M).group(1), 16)
                self.assertEqual(value, message.offset, f"{header.parent.parent.name}: {define}")
            python_value = int(re.search(rf"^{define} = (0x[0-9A-Fa-f]+)", protocol, re.M).group(1), 16)
            self.assertEqual(python_value, message.offset, f"spi_bridge: {define}")
        for request, answer in (("RELAY_REQUEST", "RELAY_RESPONSE"), ("RELAY_REQUEST_EXTRA", "RELAY_RESPONSE_EXTRA")):
            self.assertEqual(can_ids.by_name(request).offset, int(re.search(rf"#define RELAY_REQUEST_{'AXIS_STATUS' if request == 'RELAY_REQUEST' else 'RECV'}\s+(0x[0-9A-Fa-f]+)u", (root / "src/mcu_stm32g474/relay_ids.h").read_text(encoding="utf-8")).group(1), 16))
            self.assertNotEqual(can_ids.by_name(request).offset, can_ids.by_name(answer).offset)

    def test_the_controller_board_never_transmits_on_the_identifier_of_a_request(self):
        import re

        root = Path(__file__).resolve().parents[1]
        text = (root / "src/mcu_stm32g474/RobotControllerRelay.c").read_text(encoding="utf-8")
        for sent in re.findall(r"SendClassicFrame\(&hfdcan1_app, ([^,]+),", text):
            # the answers to the requests of the brain have their own identifiers; the error counters keep the one of the bootloader (the application
            # and the bootloader never run at the same time, and the host asks both with the same query)
            # the stop frame of the physical input goes out on the stop identifier 0x001 (any node may send a TRIP, docs/ESTOP_CAN.md)
            # the state of the emergency stop and the reply to the slot scan are messages of the board itself (one emitter each, see tools/can_ids.py)
            self.assertTrue("RelayAnswerOffset" in sent or any(name in sent for name in ("OFS_ERROR_COUNTERS_RESPONSE", "OFS_ESTOP_STATE", "OFS_SLOT_SCAN_REPLY", "RELAY_ESTOP_CAN_ID", "HEALTH_OFFSET")), sent)
        brain = (root / "src/mcu_stm32h745/CM4/KinematicBrainCan.c").read_text(encoding="utf-8")
        self.assertNotIn("WaitForResponse(base + OFS_AXIS_STATUS,", brain)
        self.assertNotIn("WaitForResponse(relay_recv_id,", brain)
        self.assertIn("OFS_AXIS_STATUS_RESPONSE", brain)
        self.assertIn("OFS_RELAY_RECV_RESPONSE", brain)

    def test_the_gateway_does_not_transmit_what_a_board_transmits(self):
        root = Path(__file__).resolve().parents[1]
        text = (root / "src/mcu_stm32h745/CM4/boot/bootloader_protocol.c").read_text(encoding="utf-8")
        start = text.index("void Relay_ToStackA")
        body = text[start : text.index("Relay_ToCM7", start)]
        self.assertIn("listen_only", body)
        for name in ("OFS_AXIS_STATUS_RESPONSE", "OFS_RELAY_RECV_RESPONSE", "OFS_PAGE_ACK", "OFS_STATUS", "OFS_HEARTBEAT"):
            self.assertIn(name, body, name)

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
