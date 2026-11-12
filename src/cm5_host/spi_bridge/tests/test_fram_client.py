# =============================================================================
# HYDRA-UMC - spi_bridge F-RAM client tests
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see repo root LICENSE
# =============================================================================
"""The host side of the F-RAM erase and self-test against a simulated controller board (evidence: simulator), the constants against fram_node.h and
the identifier table, and the HTTP routes with their switch."""

import json
import re
import sys
import threading
import unittest
from pathlib import Path
from urllib.error import HTTPError
from urllib.request import Request, urlopen

from sim_fram_board import SimulatedFramBoard

from spi_bridge import fram_client as fc
from spi_bridge.http_service import serve
from spi_bridge.protocol import (
    OFS_FRAM_ERASE_REQUEST,
    OFS_FRAM_ERASE_RESPONSE,
    OFS_FRAM_SELFTEST_REQUEST,
    OFS_FRAM_SELFTEST_RESPONSE,
    OFS_STATUS,
    OFS_VERSION_RESPONSE,
    SPI_TARGET_STACKA,
    SpiOtaFrame,
)

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / "tools"))
import can_ids  # noqa: E402

FRAM_NODE = (ROOT / "src" / "mcu_stm32g474" / "boot" / "fram_node.h").read_text(encoding="utf-8")


def first_hex(pattern: str, text: str) -> int:
    found = re.search(pattern, text)
    assert found is not None, pattern
    return int(found.group(1), 16)


def define(name: str) -> int:
    return first_hex(rf"#define {name}\s+(0x[0-9A-Fa-f]+)u?", FRAM_NODE)


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


class ConstantsMatchTheFirmware(unittest.TestCase):
    def test_every_constant_is_the_one_of_fram_node_h(self):
        self.assertEqual(fc.REQUEST_MAGIC, define("FRAM_REQUEST_MAGIC"))
        self.assertEqual(fc.RESPONSE_MAGIC, define("FRAM_RESPONSE_MAGIC"))
        self.assertEqual(fc.OP_ERASE, define("FRAM_OP_ERASE"))
        self.assertEqual(fc.OP_SELFTEST, define("FRAM_OP_SELFTEST"))
        for status in ("OK", "DISABLED", "NO_CHIP", "BAD_REQUEST", "WRONG_SLOT", "BUSY", "CHIP_ERROR", "VERIFY_FAIL"):
            self.assertEqual(getattr(fc, f"STATUS_{status}"), define(f"FRAM_STATUS_{status}"), status)
        self.assertEqual(len(fc.STATUS_NAMES), 8)

    def test_the_offsets_are_the_ones_of_the_identifier_table_and_of_the_bootloaders(self):
        table = {
            "FRAM_ERASE_REQUEST": (OFS_FRAM_ERASE_REQUEST, can_ids.HOST),
            "FRAM_ERASE_RESPONSE": (OFS_FRAM_ERASE_RESPONSE, can_ids.BOOTLOADER),
            "FRAM_SELFTEST_REQUEST": (OFS_FRAM_SELFTEST_REQUEST, can_ids.HOST),
            "FRAM_SELFTEST_RESPONSE": (OFS_FRAM_SELFTEST_RESPONSE, can_ids.BOOTLOADER),
        }
        for name, (offset, emitter) in table.items():
            message = can_ids.by_name(name)
            self.assertEqual((message.offset, message.emitter), (offset, emitter), name)
            for relative in ("src/mcu_stm32g474/boot/bootloader_common.h", "src/mcu_stm32h745/CM4/boot/bootloader_common.h", "src/mcu_stm32h745/CM7/boot/bootloader_common.h"):
                value = first_hex(rf"#define OFS_{name}\s+(0x[0-9A-Fa-f]+)", read(relative))
                self.assertEqual(value, offset, f"{relative}: {name}")

    def test_the_window_is_64_wide_in_every_firmware_and_the_filters_follow_it(self):
        self.assertEqual(can_ids.SLOT_WINDOW, 0x40)
        for relative in ("src/mcu_stm32g474/boot/bootloader_common.h", "src/mcu_stm32h745/CM4/boot/bootloader_common.h"):
            self.assertIn("#define STACKA_SLOT_WINDOW 0x40UL", read(relative), relative)
        for relative in ("src/mcu_stm32g474/boot/bootloader_main.c", "src/mcu_stm32g474/RobotControllerRelay.c"):
            self.assertIn("0x7C0u", read(relative), relative)
            self.assertNotIn("0x7E0u", read(relative), relative)
        for relative in ("src/mcu_stm32h745/CM4/boot/bootloader_main.c", "src/mcu_stm32h745/CM4/KinematicBrainCan.c"):
            self.assertRegex(read(relative), r"STACKA_FILTER_MASK 0x600u")

    def test_the_last_slot_still_fits_in_eleven_bits(self):
        last = 0x600 + 7 * can_ids.SLOT_WINDOW
        for message in can_ids.MESSAGES:
            self.assertLessEqual(can_ids.identifier(last, message), 0x7FF, message.name)

    def test_the_bootloader_answers_both_requests_in_both_receive_loops_and_the_brain_never_sends_the_answers(self):
        boot = read("src/mcu_stm32g474/boot/bootloader_main.c")
        self.assertEqual(boot.count("OFS_FRAM_ERASE_REQUEST"), 2)
        self.assertEqual(boot.count("OFS_FRAM_SELFTEST_REQUEST"), 2)
        brain = read("src/mcu_stm32h745/CM4/boot/bootloader_protocol.c")
        self.assertIn("OFS_FRAM_ERASE_RESPONSE", brain)
        self.assertIn("OFS_FRAM_SELFTEST_RESPONSE", brain)

    def test_the_switch_of_the_firmware_defaults_to_off(self):
        self.assertRegex(FRAM_NODE, r"#ifndef HYDRA_FRAM_ERASE\s+#define HYDRA_FRAM_ERASE 0")


class Frames(unittest.TestCase):
    def test_the_request_layout(self):
        body = bytes((0xFA, 1, 5)) + b"ERAS"
        self.assertEqual(fc.build_request(fc.OP_ERASE, 5), body + bytes((fc.check_byte(body),)))
        self.assertEqual(fc.build_request(fc.OP_SELFTEST, 2)[3:7], b"TEST")

    def test_bad_arguments(self):
        with self.assertRaises(ValueError):
            fc.build_request(9, 0)
        with self.assertRaises(ValueError):
            fc.build_request(fc.OP_ERASE, 8)

    def test_a_response_of_another_operation_or_with_a_bad_magic_is_not_taken(self):
        good = bytes((0xFB, fc.OP_ERASE, 0, 3)) + (300).to_bytes(4, "big")
        parsed = fc.parse_response(fc.OP_ERASE, good)
        assert parsed is not None
        self.assertEqual(parsed.value, 300)
        self.assertIsNone(fc.parse_response(fc.OP_SELFTEST, good))
        self.assertIsNone(fc.parse_response(fc.OP_ERASE, b"\xFA" + good[1:]))
        self.assertIsNone(fc.parse_response(fc.OP_ERASE, good[:7]))

    def test_an_unknown_status_is_named_not_hidden(self):
        result = fc.parse_response(fc.OP_ERASE, bytes((0xFB, 1, 0x99, 0, 0, 0, 0, 0)))
        assert result is not None
        self.assertEqual(result.status_name, "unknown-0x99")
        self.assertFalse(result.ok)


class AgainstASimulatedBoard(unittest.TestCase):
    def test_erase_ok_zeros_the_chip(self):
        board = SimulatedFramBoard(3, chip_size=256)
        result = fc.erase_fram(board, 3)
        assert result is not None
        self.assertTrue(result.ok)
        self.assertEqual((result.operation, result.value, result.board_id), ("erase", 256, 3))
        self.assertEqual(bytes(board.cells), bytes(256))
        self.assertEqual(board.requests[0].target_tier, SPI_TARGET_STACKA)

    def test_selftest_ok_keeps_the_chip(self):
        board = SimulatedFramBoard(3)
        before = bytes(board.cells)
        selftest = fc.fram_selftest(board, 3)
        assert selftest is not None
        self.assertTrue(selftest.ok)
        self.assertEqual(bytes(board.cells), before)

    def test_every_refusal_of_the_board_comes_back_by_name_and_touches_nothing(self):
        cases = [
            (SimulatedFramBoard(3, enabled=False), "disabled"),
            (SimulatedFramBoard(4), "wrong-slot"),
            (SimulatedFramBoard(3, update_running=True), "busy"),
            (SimulatedFramBoard(3, chip_size=0), "no-chip"),
        ]
        for board, name in cases:
            before = None if board.cells is None else bytes(board.cells)
            result = fc.erase_fram(board, 3)
            assert result is not None
            self.assertEqual(result.status_name, name)
            self.assertFalse(result.ok)
            if before is not None:
                self.assertEqual(bytes(board.cells), before, name)

    def test_a_cell_that_does_not_take_the_zero_is_a_verify_failure_with_its_address(self):
        result = fc.erase_fram(SimulatedFramBoard(3, stuck_at=250), 3)
        assert result is not None
        self.assertEqual((result.status_name, result.value), ("verify-fail", 250))

    def test_a_slow_erase_is_waited_for_by_polling(self):
        slow = fc.erase_fram(SimulatedFramBoard(3, answer_after_polls=3), 3, poll_interval_seconds=0.001)
        assert slow is not None
        self.assertTrue(slow.ok)

    def test_no_answer_within_the_timeout_is_none_not_a_made_up_result(self):
        board = SimulatedFramBoard(3, answer_after_polls=10_000)
        self.assertIsNone(fc.erase_fram(board, 3, timeout_seconds=0.05, poll_interval_seconds=0.001))

    def test_a_transport_failure_is_none(self):
        class Dead:
            def transceive(self, frame):
                raise OSError("link down")

        self.assertIsNone(fc.erase_fram(Dead(), 3))

    def test_the_simulated_board_ignores_noise(self):
        board = SimulatedFramBoard(3)
        bad = bytearray(fc.build_request(fc.OP_ERASE, 3))
        bad[7] ^= 1
        answer = board.transceive(SpiOtaFrame(2, 3, OFS_FRAM_ERASE_REQUEST, 8, bytes(bad)))
        self.assertNotEqual(answer.frame_type, OFS_FRAM_ERASE_RESPONSE)


class Online:
    def transceive(self, frame):
        return SpiOtaFrame(frame.target_tier, frame.target_slot, OFS_VERSION_RESPONSE, 8, bytes(8))


class Silent:
    def transceive(self, frame):
        return SpiOtaFrame(frame.target_tier, frame.target_slot, OFS_STATUS, 0)


class HttpRoutes(unittest.TestCase):
    def start(self, board, **options):
        server = serve(board, hmac_key=b"\x00" * 32, port=0, **options)
        threading.Thread(target=server.serve_forever, daemon=True).start()

        def stop():
            server.shutdown()
            server.server_close()

        self.addCleanup(stop)
        return f"http://127.0.0.1:{server.server_port}"

    def post(self, base, path):
        try:
            with urlopen(Request(base + path, method="POST", data=b"")) as response:
                return response.status, json.loads(response.read())
        except HTTPError as error:
            with error:
                return error.code, json.loads(error.read())

    def test_off_by_default_every_route_answers_403_and_the_board_is_not_touched(self):
        board = SimulatedFramBoard(3)
        base = self.start(board)
        for path in ("/fram/erase?slot=3&confirm=ERASE", "/fram/selftest?slot=3"):
            status, body = self.post(base, path)
            self.assertEqual(status, 403, path)
            self.assertIn("switched off", body["error"])
        self.assertEqual(board.requests, [])

    def test_the_capability_is_announced_only_when_the_service_has_it(self):
        for flag in (False, True):
            base = self.start(Online(), fram_erase=flag)
            with urlopen(base + "/version?tier=2&slot=3") as response:
                self.assertEqual(json.loads(response.read())["capabilities"], {"fram_erase": flag})

    def test_with_the_switch_on_an_erase_needs_the_confirmation_and_a_valid_slot(self):
        board = SimulatedFramBoard(3)
        base = self.start(board, fram_erase=True)
        for path in ("/fram/erase?slot=3", "/fram/erase?slot=3&confirm=erase", "/fram/erase?slot=9&confirm=ERASE", "/fram/erase?confirm=ERASE", "/fram/erase?slot=x&confirm=ERASE"):
            self.assertEqual(self.post(base, path)[0], 400, path)
        self.assertEqual(board.requests, [])

    def test_with_the_switch_on_both_operations_run(self):
        base = self.start(SimulatedFramBoard(3), fram_erase=True)
        status, body = self.post(base, "/fram/erase?slot=3&confirm=ERASE")
        self.assertEqual((status, body["status"], body["ok"], body["value"]), (200, "ok", True, 256))
        status, body = self.post(base, "/fram/selftest?slot=3")
        self.assertEqual((status, body["operation"], body["ok"]), (200, "selftest", True))

    def test_a_board_that_says_no_chip_is_reported_as_it_is(self):
        base = self.start(SimulatedFramBoard(3, chip_size=0), fram_erase=True)
        status, body = self.post(base, "/fram/erase?slot=3&confirm=ERASE")
        self.assertEqual((status, body["status"], body["ok"]), (200, "no-chip", False))

    def test_a_silent_board_is_a_504(self):
        defaults = fc.run_operation.__kwdefaults__
        original = defaults["timeout_seconds"]
        defaults["timeout_seconds"] = 0.05
        self.addCleanup(defaults.__setitem__, "timeout_seconds", original)
        base = self.start(Silent(), fram_erase=True)
        self.assertEqual(self.post(base, "/fram/selftest?slot=1")[0], 504)


if __name__ == "__main__":
    unittest.main()
