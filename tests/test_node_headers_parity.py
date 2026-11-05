# =============================================================================
# HYDRA-UMC - tests/test_node_headers_parity.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The firmware headers of the emergency stop and of the slot scan, and the vectors they are tested against, say the same as the references.

Each target builds from its own folder, so estop_can.h is kept as two identical copies (controller board and kinematic brain). The C tests read
the vectors through headers that tools/gen_node_vectors.py writes from the JSON files; this test fails when they are out of date. The offsets of
the slot window are compared with tools/can_ids.py and the numbers of the contract with the constants of the headers."""
import json
import re
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import can_ids  # noqa: E402
import estop_can  # noqa: E402
import slot_scan  # noqa: E402

CONTRACT = json.loads((ROOT / "docs" / "bus_contract.json").read_text(encoding="utf-8"))


def read(path: str) -> str:
    return (ROOT / path).read_bytes().decode("utf-8").replace("\r\n", "\n")


def define(text: str, name: str) -> int:
    match = re.search(rf"^#define {name}\s+\(?(0x[0-9A-Fa-f]+|\d+)u?\)?", text, re.M)
    if not match:
        raise AssertionError(f"{name} not found")
    return int(match.group(1), 0)


class HeaderParityTests(unittest.TestCase):
    def test_estop_can_h_is_the_same_in_the_controller_board_and_the_brain(self):
        self.assertEqual(read("src/mcu_stm32g474/estop_can.h"), read("src/mcu_stm32h745/CM4/estop_can.h"))

    def test_the_generated_vector_headers_are_up_to_date(self):
        result = subprocess.run([sys.executable, str(ROOT / "tools" / "gen_node_vectors.py"), "--check"], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


class EstopHeaderTests(unittest.TestCase):
    def test_the_constants_follow_the_reference_and_the_contract(self):
        text = read("src/mcu_stm32g474/estop_can.h")
        self.assertEqual(define(text, "ESTOP_CAN_ID"), estop_can.ESTOP_CAN_ID)
        self.assertEqual(define(text, "ESTOP_KIND_TRIP"), estop_can.KIND_TRIP)
        self.assertEqual(define(text, "ESTOP_KIND_RELEASE"), estop_can.KIND_RELEASE)
        self.assertEqual(define(text, "ESTOP_KIND_ALIVE"), estop_can.KIND_ALIVE)
        self.assertEqual(define(text, "ESTOP_SOURCE_CM5"), estop_can.SOURCE_CM5)
        for code, name in {0x01: "OPERATOR", 0x02: "STOP_ALL", 0x03: "HARDWARE", 0x04: "SAFETY_ZONE", 0x05: "HOST_LINK", 0x06: "CONTROLLER", 0x07: "BUS_FAULT", 0x08: "POWER_LOSS", 0xFF: "UNSPECIFIED"}.items():
            self.assertEqual(define(text, f"ESTOP_REASON_{name}"), code)
            self.assertIn(code, estop_can.REASONS)
        timing = CONTRACT["emergency_stop"]["timing_ms"]
        self.assertEqual(define(text, "ESTOP_ALIVE_TIMEOUT_MS"), timing["alive_timeout"])
        brain = read("src/mcu_stm32h745/CM4/estop_brain.h")
        self.assertEqual(define(brain, "ESTOP_TRIP_REPEAT_MS"), timing["trip_repeat"])
        self.assertEqual(define(brain, "ESTOP_ALIVE_PERIOD_MS"), timing["alive_period"])
        self.assertEqual(define(brain, "ESTOP_RELEASE_REPEAT_MS"), timing["release_repeat"])
        self.assertEqual(define(brain, "ESTOP_RELEASE_ACK_TIMEOUT_MS"), timing["release_ack_timeout"])

    def test_the_state_answer_has_the_offset_of_the_table(self):
        offset = can_ids.by_name("ESTOP_STATE").offset
        for path in ("src/mcu_stm32g474/boot/bootloader_common.h", "src/mcu_stm32h745/CM4/boot/bootloader_common.h", "src/mcu_stm32h745/CM7/boot/bootloader_common.h"):
            self.assertEqual(define(read(path), "OFS_ESTOP_STATE"), offset, path)

    def test_the_application_of_the_controller_board_acts_on_the_stop(self):
        relay = read("src/mcu_stm32g474/RobotControllerRelay.c")
        self.assertIn("EstopNode_OnFrame", relay)
        self.assertIn("MotionOutputs_ForceDisabled", relay)
        self.assertIn("OFS_ESTOP_STATE", relay)
        # the stop is forwarded unchanged to the tool head on the downlink, not through the relay tunnel
        self.assertRegex(relay, r"SendClassicFrame\(&hfdcan2_app, RELAY_ESTOP_CAN_ID")


class SlotScanHeaderTests(unittest.TestCase):
    def test_the_constants_follow_the_reference_and_the_contract(self):
        text = read("src/mcu_stm32g474/boot/slot_scan_node.h")
        contract = CONTRACT["slot_scan"]
        self.assertEqual(define(text, "SLOT_SCAN_PROBE_MAGIC"), int(contract["probe_magic"], 16))
        self.assertEqual(define(text, "SLOT_SCAN_REPLY_MAGIC"), int(contract["reply_magic"], 16))
        self.assertEqual(define(text, "SLOT_SCAN_REPLY_STEP_MS"), contract["timing_ms"]["reply_step"])
        self.assertEqual(define(text, "SLOT_SCAN_REPLY_STEPS"), contract["timing_ms"]["reply_steps"])
        self.assertEqual(define(text, "SLOT_SCAN_ROLE_BOOTLOADER"), slot_scan.ROLE_BOOTLOADER)
        self.assertEqual(define(text, "SLOT_SCAN_ROLE_APPLICATION"), slot_scan.ROLE_APPLICATION)

    def test_the_offsets_follow_the_table_in_every_bootloader(self):
        for path in ("src/mcu_stm32g474/boot/bootloader_common.h", "src/mcu_stm32h745/CM4/boot/bootloader_common.h", "src/mcu_stm32h745/CM7/boot/bootloader_common.h"):
            text = read(path)
            self.assertEqual(define(text, "OFS_SLOT_SCAN_PROBE"), can_ids.by_name("SLOT_SCAN_PROBE").offset, path)
            self.assertEqual(define(text, "OFS_SLOT_SCAN_REPLY"), can_ids.by_name("SLOT_SCAN_REPLY").offset, path)

    def test_the_bootloader_and_the_application_both_answer(self):
        boot = read("src/mcu_stm32g474/boot/bootloader_main.c")
        self.assertEqual(boot.count("OFS_SLOT_SCAN_PROBE"), 2, "both receive loops of the bootloader")
        self.assertIn("SLOT_SCAN_ROLE_BOOTLOADER", boot)
        relay = read("src/mcu_stm32g474/RobotControllerRelay.c")
        self.assertIn("OFS_SLOT_SCAN_PROBE", relay)
        self.assertIn("SLOT_SCAN_ROLE_APPLICATION", relay)
        self.assertIn("HAL_GetUIDw0", relay)
        self.assertIn("HAL_GetUIDw0", boot)

    def test_the_delay_in_the_header_is_the_one_of_the_reference(self):
        text = read("src/mcu_stm32g474/boot/slot_scan_node.h")
        self.assertIn("2654435761u", text)
        self.assertEqual(slot_scan.reply_delay_ms(1), 17)


if __name__ == "__main__":
    unittest.main()
