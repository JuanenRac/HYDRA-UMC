# =============================================================================
# HYDRA-UMC - tests/test_spi_app.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The host twin of the SPI slave service of the application of the CM4 (tools/spi_app.py), the bytes it shares with tests/test_spi_app.c, and the
guards over the sources of the service (the switch ships off, the driver is in both build lists and in the CM4 application). Evidence: simulator."""
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import spi_app  # noqa: E402
import spi_frame  # noqa: E402
import spi_system  # noqa: E402

CM4 = ROOT / "src" / "mcu_stm32h745" / "CM4"


def text(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def vectors_in_c() -> dict[str, bytes]:
    """Every `VECTOR name: aa bb ...` comment of the C test, as bytes."""
    found = {}
    for name, hexes in re.findall(r"VECTOR ([a-z ]+): ((?:[0-9a-f]{2} ?)+)", text(ROOT / "tests" / "test_spi_app.c")):
        found[name.strip()] = bytes.fromhex(hexes.replace(" ", ""))
    return found


def query_frame(sequence: int) -> bytes:
    return spi_frame.build(0, 0, spi_system.TELEMETRY_QUERY, b"\x00", sequence)


class TwinFollowsTheContract(unittest.TestCase):
    def full(self) -> spi_app.SpiApp:
        return spi_app.SpiApp(spi_system.Config(True, True))

    def test_the_bytes_the_c_test_asserts_are_the_ones_the_twin_builds(self):
        vectors = vectors_in_c()
        self.assertEqual(set(vectors), {"idle frame", "telemetry query", "telemetry reply", "probe request", "probe reply"})
        app = self.full()
        self.assertEqual(app.tx_wire[:16], vectors["idle frame"])
        self.assertEqual(query_frame(1)[:16], vectors["telemetry query"])
        app.on_receive(query_frame(1))
        self.assertEqual(app.tx_wire[:16], vectors["telemetry reply"])
        self.assertEqual(spi_frame.build(0, 0, spi_system.PROBE_REQUEST, b"\x2a", 2)[:16], vectors["probe request"])
        probe = spi_app.SpiApp(spi_system.Config(True, False))
        probe.on_receive(spi_frame.build(0, 0, spi_system.PROBE_REQUEST, b"\x2a", 1))
        self.assertEqual(probe.take_scan(), 0x2A)
        probe.scan_finished(0x2A, [1, 1, 2, 0, 4, 1, 1, 3])
        probe.service()
        self.assertEqual(probe.tx_wire[:16], vectors["probe reply"])

    def test_every_frame_the_twin_sends_is_sealed_and_the_sequence_moves_by_one(self):
        app = self.full()
        last = None
        for index in range(300):
            app.on_receive(query_frame(index + 1))
            self.assertEqual(spi_frame.check(app.tx_wire), "ok")
            sequence = app.tx_wire[spi_frame.OFS_SEQ]
            if last is not None:
                self.assertEqual((sequence - last) & 0xFF, 1)
            last = sequence
        self.assertEqual((app.seq_gaps, app.crc_errors, app.commands_run), (0, 0, 300))

    def test_a_damaged_frame_a_retry_and_a_gap_are_counted_apart(self):
        app = self.full()
        stop = bytearray(spi_frame.build(0, 0, spi_system.STOP_REQUEST, spi_system.stop_payload(9), 1))
        stop[5] ^= 1
        self.assertFalse(app.on_receive(bytes(stop)))
        self.assertEqual((app.crc_errors, app.take_trip()), (1, None))
        good = spi_frame.build(0, 0, spi_system.STOP_REQUEST, spi_system.stop_payload(9), 2)
        self.assertTrue(app.on_receive(good))
        self.assertFalse(app.on_receive(good))
        self.assertEqual(app.retries, 1)
        self.assertEqual(app.take_trip(), 9)
        self.assertIsNone(app.take_trip(), "a request is taken once")
        app.on_receive(query_frame(7))
        self.assertEqual(app.seq_gaps, 1)

    def test_what_is_not_a_command_of_the_tier_self_is_ignored(self):
        app = self.full()
        before = app.tx_wire
        frames = [
            spi_frame.build(2, 0, spi_system.TELEMETRY_QUERY, b"\x00", 1),  # for the stack
            spi_frame.build(1, 0, spi_system.STOP_REQUEST, spi_system.stop_payload(1), 2),  # for the CM7
            spi_frame.build(0, 0, 0x03, b"\x00", 3),  # an update frame
            spi_frame.build(0, 0, spi_system.TELEMETRY_RESPONSE, b"\x00", 4),
            spi_frame.build(0, 0, 0x26, b"\x00", 5),
            spi_frame.build(0, 0, spi_system.STOP_REQUEST, b"STOP", 6),  # too short
            spi_frame.build(0, 0, spi_app.IDLE_TYPE, b"", 7),
        ]
        for wire in frames:
            self.assertFalse(app.on_receive(wire))
        self.assertEqual((app.tx_wire, app.frames_ignored, app.commands_run), (before, 7, 0))
        self.assertIsNone(app.take_trip())

    def test_the_seal_can_be_required_or_not(self):
        lenient, strict = self.full(), spi_app.SpiApp(spi_system.Config(True, True), require_crc=True)
        unsealed = spi_frame.build(0, 0, spi_system.TELEMETRY_QUERY, b"\x00")
        self.assertTrue(lenient.on_receive(unsealed))
        self.assertFalse(strict.on_receive(unsealed))
        self.assertEqual((lenient.unsealed, strict.unsealed), (1, 1))

    def test_with_every_switch_off_nothing_is_answered_or_requested(self):
        app = spi_app.SpiApp(spi_system.Config(False, False))
        before = app.tx_wire
        link = spi_app.Cm5Link(app)
        self.assertEqual(link.exchange(spi_system.TELEMETRY_QUERY, b"\x00"), (0, b""))
        link.transaction(spi_system.STOP_REQUEST, spi_system.stop_payload(1))
        link.transaction(spi_system.RELEASE_REQUEST, b"RELS")
        link.transaction(spi_system.PROBE_REQUEST, b"\x01")
        self.assertEqual((app.take_trip(), app.take_release(), app.take_scan()), (None, False, None))
        self.assertEqual(app.tx_wire, before)

    def test_read_only_answers_telemetry_and_probe_but_not_stop_and_release(self):
        app = spi_app.SpiApp(spi_system.Config(True, False))
        link = spi_app.Cm5Link(app)
        kind, data = link.exchange(spi_system.TELEMETRY_QUERY, b"\x00")
        self.assertEqual(kind, spi_system.TELEMETRY_RESPONSE)
        self.assertEqual(spi_system.decode_telemetry(data).stop_enabled, False)
        link.transaction(spi_system.STOP_REQUEST, spi_system.stop_payload(1))
        link.transaction(spi_system.RELEASE_REQUEST, b"RELS")
        self.assertEqual((app.take_trip(), app.take_release()), (None, False))
        link.transaction(spi_system.PROBE_REQUEST, b"\x09")
        self.assertEqual(app.take_scan(), 9)

    def test_the_client_of_the_reference_reads_the_telemetry_through_the_link(self):
        app = self.full()
        app.publish_brain_state(False, 12, 3)
        client = spi_system.Client(spi_app.Cm5Link(app).exchange)
        info = client.read_telemetry()
        self.assertEqual((info.estop_latched, info.can_tx_errors, info.can_rx_errors, info.state), (False, 12, 3, spi_system.STATE_APPLICATION))

    def test_a_scan_answer_is_staged_once_and_only_when_done(self):
        app = self.full()
        link = spi_app.Cm5Link(app)
        link.transaction(spi_system.PROBE_REQUEST, b"\x2a")
        staged = app.tx_wire
        app.service()
        self.assertEqual(app.tx_wire, staged, "nothing is staged while the scan runs")
        self.assertEqual(app.take_scan(), 0x2A)
        app.scan_finished(0x2A, [0, 1, 0, 1, 0, 1, 0, 1])
        app.service()
        self.assertEqual(app.tx_wire[2], spi_system.PROBE_RESPONSE)
        again = app.tx_wire
        app.service()
        self.assertEqual(app.tx_wire, again)
        scan_id, verdicts = spi_system.decode_probe_response(bytes(app.tx_wire[4:9]))
        self.assertEqual((scan_id, verdicts[:4]), (0x2A, ["ABSENT", "OK", "ABSENT", "OK"]))


class SourcesOfTheService(unittest.TestCase):
    def test_the_switch_ships_off_and_the_header_has_no_hal(self):
        header = text(CM4 / "spi_app.h")
        self.assertRegex(header, r"#ifndef HYDRA_SPI_APP\s*\n#define HYDRA_SPI_APP\s+0")
        self.assertRegex(header, r"#ifndef HYDRA_SPI_REQUIRE_CRC\s*\n#define HYDRA_SPI_REQUIRE_CRC\s+0")
        self.assertNotIn("stm32h7xx_hal", header)
        self.assertNotIn("FreeRTOS", header)

    def test_the_driver_is_an_empty_unit_unless_the_switch_is_on(self):
        driver = text(CM4 / "SpiApp.c")
        self.assertIn("#if HYDRA_SPI_APP", driver)
        self.assertLess(driver.index("#if HYDRA_SPI_APP"), driver.index("HAL_SPI_Init"))
        self.assertIn("#else", driver)
        header = text(CM4 / "SpiApp.h")
        self.assertEqual(header.count("#if HYDRA_SPI_APP"), 1)

    def test_the_application_starts_the_service_and_pumps_it_only_behind_the_switch(self):
        main = text(CM4 / "STM32H745ZI_CM4_main.c")
        self.assertEqual(main.count("#if HYDRA_SPI_APP"), 2)
        self.assertIn("SpiApp_Start();", main)
        self.assertIn("SpiApp_Pump();", main)
        self.assertLess(main.index("KinematicBrainCan_Init();"), main.index("SpiApp_Start();"), "the bus is up before the service starts")

    def test_the_spi_task_never_calls_the_emergency_stop(self):
        driver = text(CM4 / "SpiApp.c")
        task = driver[driver.index("static void SpiApp_Task"): driver.index("void SpiApp_Start")]
        self.assertNotIn("Estop", task)
        self.assertNotIn("KinematicBrainCan_", task)
        header = text(CM4 / "spi_app.h")
        self.assertNotIn('#include "KinematicBrainCan', header)

    def test_the_geometry_of_the_frame_is_the_one_of_the_bootloader(self):
        common = text(CM4 / "boot" / "bootloader_common.h")
        self.assertIn("#define SPI_FRAME_SIZE       128", common)
        header = text(CM4 / "spi_app.h")
        self.assertIn("#define SPI_APP_FRAME_SIZE 128u", header)
        self.assertEqual(spi_frame.FRAME_SIZE, 128)
        self.assertIn("#define SPI_TARGET_SELF      0", common)
        self.assertIn("#define SPI_APP_TARGET_SELF 0u", header)

    def test_the_pins_are_the_ones_of_the_bootloader(self):
        boot = text(CM4 / "boot" / "bootloader_main.c")
        driver = text(CM4 / "SpiApp.c")
        self.assertIn("GPIO_PIN_4 | GPIO_PIN_5 | GPIO_PIN_6 | GPIO_PIN_7", boot)
        self.assertIn("GPIO_PIN_4 | GPIO_PIN_5 | GPIO_PIN_6 | GPIO_PIN_7", driver)
        self.assertIn("#define HYDRA_DATA_READY_PIN  GPIO_PIN_3", boot)
        self.assertIn("#define SPI_APP_DATA_READY_PIN  GPIO_PIN_3", driver)
        for mode in ("SPI_MODE_SLAVE", "SPI_NSS_HARD_INPUT", "SPI_POLARITY_LOW", "SPI_PHASE_1EDGE", "SPI_FIRSTBIT_MSB"):
            self.assertIn(mode, boot)
            self.assertIn(mode, driver)

    def test_the_host_tests_and_the_build_know_the_service(self):
        table = text(ROOT / "tools" / "host_tests.py")
        script = text(ROOT / "build_firmware.sh")
        self.assertIn("tests/test_spi_app.c", table)
        self.assertIn("tests/test_spi_app.c", script)
        self.assertIn("SPI_APP_TEST_SWITCHES=0", table)
        self.assertIn("SPI_APP_TEST_SWITCHES=1", table)
        self.assertIn('"$SRC/SpiApp.c"', script)

    def test_the_documents_name_the_switch(self):
        self.assertIn("HYDRA_SPI_APP", text(ROOT / "docs" / "FIRMWARE_SWITCHES.md"))
        self.assertIn("HYDRA_SPI_APP", text(ROOT / "docs" / "SPI_SYSTEM.md"))


if __name__ == "__main__":
    unittest.main()
