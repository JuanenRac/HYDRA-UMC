# =============================================================================
# HYDRA-UMC - tests/test_hy25_sources.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""Guards over the sources of the switches that are off by default (explicit FDCAN clock, ENTER_BOOTLOADER in the application, health frame,
telemetry and stop commands of the SPI link, quiet tick before the scheduler, read-back gate, clock handshake of the H745) and the references in Python
that the C host tests are compared with. Evidence: simulator (source check). Each switch changes the binary only when a build sets it."""
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import can_ids  # noqa: E402
import health_frame  # noqa: E402
import slot_scan  # noqa: E402
import spi_system  # noqa: E402

G4 = ROOT / "src" / "mcu_stm32g474"
H7 = ROOT / "src" / "mcu_stm32h745"


def text(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def default_of(path: Path, name: str) -> int:
    found = re.search(rf"#ifndef {name}\s*\n#define {name}\s+(\d+)", text(path))
    assert found, f"{name} has no default in {path.name}"
    return int(found.group(1))


class SwitchesShipOff(unittest.TestCase):
    def test_every_switch_defaults_to_zero(self):
        cases = (
            (G4 / "app_boot_request.h", "HYDRA_APP_ENTER_BOOTLOADER"),
            (G4 / "health_frame.h", "HYDRA_HEALTH_FRAME"),
            (G4 / "boot_report.h", "HYDRA_BOOT_REPORT"),
            (G4 / "init_fault.h", "HYDRA_INIT_FAULT_LATCH"),
            (G4 / "boot" / "bootloader_flash.c", "HYDRA_FLASH_LAYER"),
            (H7 / "CM4" / "boot" / "bootloader_flash.c", "HYDRA_FLASH_LAYER"),
            (H7 / "CM7" / "boot" / "bootloader_flash.c", "HYDRA_FLASH_LAYER"),
            (G4 / "boot" / "adoption_guard.h", "HYDRA_READBACK_GATE"),
            (G4 / "STM32G474RE_main.c", "HYDRA_QUIET_TICK_BEFORE_SCHEDULER"),
            (H7 / "CM4" / "spi_system.h", "HYDRA_SPI_SYSTEM"),
            (H7 / "CM4" / "spi_system.h", "HYDRA_SPI_STOP_COMMAND"),
            (H7 / "CM4" / "rcc_owner.h", "HYDRA_RCC_HSEM_HANDSHAKE"),
        )
        for path, name in cases:
            self.assertEqual(default_of(path, name), 0, f"{name} must ship off")

    def test_the_fdcan_clock_switch_ships_on_by_decision_of_the_maintainer(self):
        self.assertEqual(default_of(G4 / "fdcan_clock.h", "HYDRA_FDCAN_KERNEL_CLOCK_EXPLICIT"), 1)
        self.assertIn("not been validated on a board", text(G4 / "fdcan_clock.h"))

    def test_the_sources_use_the_switches_and_the_headers(self):
        boot = text(G4 / "boot" / "bootloader_main.c")
        relay = text(G4 / "RobotControllerRelay.c")
        self.assertIn("#if HYDRA_FDCAN_KERNEL_CLOCK_EXPLICIT", boot)
        self.assertIn("RCC_FDCANCLKSOURCE_PCLK1", boot)
        self.assertIn("#if HYDRA_FDCAN_KERNEL_CLOCK_EXPLICIT", relay)
        self.assertIn("RCC_FDCANCLKSOURCE_PCLK1", relay)
        self.assertIn("#if HYDRA_APP_ENTER_BOOTLOADER", boot)
        self.assertIn("AppBoot_TakeRequest", boot)
        self.assertIn("AppBoot_OnEnterBootloader", relay)
        self.assertIn("#if HYDRA_HEALTH_FRAME", relay)
        self.assertIn("HealthFrame_Due", relay)
        protocol = text(G4 / "boot" / "bootloader_protocol.c")
        self.assertIn("#if HYDRA_READBACK_GATE", protocol)
        self.assertIn("Adopt_ReadbackAllowed", protocol)
        self.assertIn("Adopt_RecordMatches", protocol)
        self.assertIn("HYDRA_SPI_SYSTEM", text(H7 / "CM4" / "boot" / "bootloader_main.c"))
        self.assertIn("RccOwner_Decide", text(H7 / "CM4" / "STM32H745ZI_CM4_main.c"))

    def test_the_boot_report_travels_from_the_bootloader_to_the_health_flags(self):
        boot = text(G4 / "boot" / "bootloader_main.c")
        relay = text(G4 / "RobotControllerRelay.c")
        self.assertIn("#if HYDRA_BOOT_REPORT", boot)
        self.assertIn("BootReport_Write(&TAMP->BKP2R", boot)
        self.assertLess(boot.index("BootReport_Write("), boot.index("JumpToApplication();"))
        self.assertIn("#if HYDRA_BOOT_REPORT", relay)
        self.assertIn("BootReport_TakeHealthFlags(&TAMP->BKP2R)", relay)
        self.assertLess(relay.index("HealthFrame_Reset(&health);"), relay.index("BootReport_TakeHealthFlags"))

    def test_the_g474_flash_routines_go_through_the_common_layer_only_where_the_switch_says(self):
        flash = text(G4 / "boot" / "bootloader_flash.c")
        self.assertIn('#include "flash_layer.h"', flash)
        self.assertEqual(flash.count("#if HYDRA_FLASH_LAYER"), 3)
        for call in ("FlashLayer_Erase(", "FlashLayer_Write(", "FlashLayer_GeometryG474()"):
            self.assertIn(call, flash)
        # the legacy path stays in the #else branch: a build without the switch is the code that shipped
        legacy = flash.split("#else", 1)[1]
        self.assertIn("HAL_FLASHEx_Erase(&eraseInit", flash)
        self.assertIn("FLASH_TYPEPROGRAM_DOUBLEWORD", legacy)

    def test_the_h745_flash_routines_stay_inside_their_own_bank_under_the_layer(self):
        for core, base in (("CM4", "BANK2_BASE"), ("CM7", "BANK1_BASE")):
            flash = text(H7 / core / "boot" / "bootloader_flash.c")
            self.assertEqual(flash.count("#if HYDRA_FLASH_LAYER"), 2, core)
            self.assertIn("FlashLayer_InRange(this_bank_geometry()", flash, core)
            self.assertIn(f"{{{base}, 0x00100000UL, FLASH_SECTOR_SIZE, 32u}}", flash, core)
            self.assertIn("FlashLayer_Write(h7_layer_ops()", flash, core)

    def test_a_failed_setup_of_the_application_resets_once_then_keeps_the_watchdog_in_the_safe_state(self):
        relay = text(G4 / "RobotControllerRelay.c")
        main = text(G4 / "STM32G474RE_main.c")
        self.assertIn("InitFault_OnFailure(&TAMP->BKP4R", relay)
        self.assertIn("InitFault_Begin(&TAMP->BKP4R)", relay)
        self.assertIn("InitFault_Clear(&TAMP->BKP4R)", relay)
        self.assertIn("AppWatchdog_Refresh();", relay)
        self.assertIn("void AppWatchdog_Refresh(void)", main)
        self.assertLess(relay.index("MotionOutputs_ForceDisabled();"), relay.index("InitFault_OnFailure("), "the drivers go off before anything else")
        self.assertLess(relay.index("HealthFrame_Reset(&health);"), relay.index("InitFault_HealthFlags("))
        self.assertLess(relay.index("AxisStatusGpio_Init();"), relay.index("InitFault_Clear("), "the counter is cleared only after the whole set-up")

    def test_the_init_fault_registers_do_not_overlap_the_other_reports(self):
        self.assertIn("BKP4R and BKP5R", text(G4 / "init_fault.h"))
        for other in ("BKP0R", "BKP1R", "BKP2R", "BKP3R"):
            self.assertNotIn(other, text(G4 / "init_fault.h").split("BKP4R and BKP5R")[1], other)

    def test_the_boot_report_registers_do_not_overlap_the_enter_bootloader_request(self):
        self.assertIn("&TAMP->BKP0R", text(G4 / "RobotControllerRelay.c"))
        self.assertNotIn("BKP2R", text(G4 / "app_boot_request.h"))

    def test_the_fdcan_clock_selection_comes_before_the_peripheral_clock_is_enabled(self):
        for path in (G4 / "boot" / "bootloader_main.c", G4 / "RobotControllerRelay.c"):
            body = text(path)
            self.assertLess(body.index("RCC_FDCANCLKSOURCE_PCLK1"), body.index("__HAL_RCC_FDCAN_CLK_ENABLE()"), path.name)

    def test_the_relay_polls_and_enables_no_fdcan_interrupt_before_the_scheduler(self):
        """The timing model of bench row 12 assumes it: a receive interrupt that called the RTOS before the scheduler runs would be a fault."""
        for path in G4.rglob("*.[ch]"):
            body = text(path)
            self.assertNotIn("HAL_FDCAN_ActivateNotification", body, path.name)
            self.assertNotIn("HAL_NVIC_EnableIRQ(FDCAN", body, path.name)

    def test_the_tick_is_quieted_only_where_the_switch_says(self):
        main = text(G4 / "STM32G474RE_main.c")
        self.assertEqual(len(re.findall(r"SysTick->CTRL &= ~SysTick_CTRL_TICKINT_Msk", main)), 2)
        self.assertIn("#if HYDRA_QUIET_TICK_BEFORE_SCHEDULER == 1", main)
        self.assertIn("#if HYDRA_QUIET_TICK_BEFORE_SCHEDULER == 2", main)
        self.assertLess(main.index("#if HYDRA_QUIET_TICK_BEFORE_SCHEDULER == 2"), main.index("RobotControllerRelay_Init();"))
        self.assertGreater(main.index("#if HYDRA_QUIET_TICK_BEFORE_SCHEDULER == 1"), main.index("RobotControllerRelay_Init();"))
        self.assertLess(main.index("#if HYDRA_QUIET_TICK_BEFORE_SCHEDULER == 1"), main.index("vTaskStartScheduler();"))

    def test_the_flash_layer_is_the_same_file_in_the_three_boot_directories(self):
        copies = [text(G4 / "boot" / "flash_layer.h"), text(H7 / "CM4" / "boot" / "flash_layer.h"), text(H7 / "CM7" / "boot" / "flash_layer.h")]
        self.assertEqual(copies[0], copies[1])
        self.assertEqual(copies[0], copies[2])

    def test_the_new_host_tests_are_in_both_lists(self):
        table = text(ROOT / "tools" / "host_tests.py")
        script = text(ROOT / "build_firmware.sh")
        for source in ("test_app_support.c", "test_spi_system.c", "test_rcc_owner.c", "test_flash_layer.c"):
            self.assertIn(source, table, source)
            self.assertIn(source, script, source)
        self.assertEqual(table.count("tests/test_flash_layer.c"), 3)

    def test_the_cm4_stages_the_reply_through_one_function(self):
        self.assertIn("void Protocol_StageSelfReply", text(H7 / "CM4" / "boot" / "bootloader_protocol.h"))
        self.assertIn("StashResponse(frame_type, data, dlc);", text(H7 / "CM4" / "boot" / "bootloader_protocol.c"))


class HealthFrameReference(unittest.TestCase):
    def test_the_message_is_in_the_identifier_table_with_the_board_as_only_emitter(self):
        message = can_ids.by_name("HEALTH_FRAME")
        self.assertEqual((message.offset, message.emitter), (health_frame.OFFSET, can_ids.CONTROLLER_BOARD))
        header = text(G4 / "health_frame.h")
        self.assertEqual(int(re.search(r"#define HEALTH_OFFSET\s+(0x[0-9A-Fa-f]+)u", header).group(1), 16), message.offset)
        self.assertEqual(int(re.search(r"#define HEALTH_MARKER\s+(0x[0-9A-Fa-f]+)u", header).group(1), 16), health_frame.MARKER)
        self.assertEqual(int(re.search(r"#define HEALTH_PERIOD_MS\s+(\d+)u", header).group(1)), health_frame.PERIOD_MS)

    def test_the_flag_bits_of_the_header_are_the_ones_of_the_reference(self):
        header = text(G4 / "health_frame.h")
        for name, value in (("ESTOP", health_frame.FLAG_ESTOP), ("BUSOFF", health_frame.FLAG_BUSOFF), ("WDG_RST", health_frame.FLAG_WATCHDOG_RESET),
                            ("INIT_BAD", health_frame.FLAG_INIT_FAILED), ("CONFIRMED", health_frame.FLAG_CONFIRMED), ("ENABLED", health_frame.FLAG_DRIVERS_ENABLED), ("INIT_SAFE", health_frame.FLAG_INIT_SAFE_STATE)):
            self.assertEqual(int(re.search(rf"#define HEALTH_FLAG_{name}\s+(0x[0-9A-Fa-f]+)u", header).group(1), 16), value, name)

    def test_the_vector_of_the_c_test_is_what_the_reference_builds(self):
        vector = re.search(r"/\* VECTOR: ([0-9A-Fa-f ]+) \*/", text(ROOT / "tests" / "test_app_support.c")).group(1)
        expected = bytes(int(part, 16) for part in vector.split())
        built = health_frame.build(0, health_frame.FLAG_ESTOP | health_frame.FLAG_BUSOFF, 3, 7, 1, 100)
        self.assertEqual(built, expected)
        self.assertEqual(health_frame.decode(built).names(), ["estop_latched", "bus_off_seen"])

    def test_decode_rejects_anything_that_is_not_a_health_frame(self):
        good = health_frame.build(5, 1, 0, 0, 0, 0)
        self.assertIsNotNone(health_frame.decode(good))
        self.assertIsNone(health_frame.decode(good[:7]))
        self.assertIsNone(health_frame.decode(bytes((0x49,)) + good[1:]))
        for index in range(8):
            damaged = bytearray(good)
            damaged[index] ^= 0x10
            self.assertIsNone(health_frame.decode(bytes(damaged)), index)

    def test_the_sender_follows_the_period_wraps_and_reports_bus_off_once(self):
        sender = health_frame.Sender()
        sender.flags = health_frame.FLAG_BUSOFF
        first = sender.due(100)
        self.assertIsNotNone(first)
        self.assertTrue(health_frame.decode(first).flags & health_frame.FLAG_BUSOFF)
        self.assertIsNone(sender.due(600))
        second = sender.due(1100)
        self.assertFalse(health_frame.decode(second).flags & health_frame.FLAG_BUSOFF)
        self.assertEqual(health_frame.decode(second).sequence, 1)
        wrapping = health_frame.Sender()
        self.assertIsNotNone(wrapping.due(0xFFFFFF00))
        self.assertIsNone(wrapping.due(0x100))
        self.assertIsNotNone(wrapping.due(0x3F0))

    def test_the_uptime_saturates(self):
        self.assertEqual(health_frame.decode(health_frame.build(0, 0, 0, 0, 0, 16000 * 300)).uptime_s, 255 * 16)


class SpiSystemReference(unittest.TestCase):
    def snapshot(self):
        return spi_system.Snapshot(crc_errors=3, retries=300, seq_gaps=0, can_tx_errors=5, can_rx_errors=9, state=spi_system.STATE_APPLICATION, link_ok=True,
                                   estop_latched=True, scan_verdicts=[1, 1, 2, 0, 4, 1, 1, 3])

    def c_vectors(self):
        body = text(ROOT / "tests" / "test_spi_system.c")
        return {m.group(1): bytes(int(x, 16) for x in m.group(2).split()) for m in re.finditer(r"/\* VECTOR ([a-z ()]+): ([0-9a-f ]+) \*/", body)}

    def test_the_vectors_of_the_c_test_are_what_the_reference_builds(self):
        vectors = self.c_vectors()
        self.assertEqual(vectors["telemetry (stop disabled)"], spi_system.telemetry(spi_system.Config(True, False), self.snapshot()))
        self.assertEqual(vectors["telemetry (stop enabled)"], spi_system.telemetry(spi_system.Config(True, True), self.snapshot()))
        self.assertEqual(vectors["probe response"], spi_system.probe_response(self.snapshot(), 0x2A))

    def test_the_types_and_flags_of_the_header_are_the_ones_of_the_reference(self):
        header = text(H7 / "CM4" / "spi_system.h")
        for name in ("STOP_REQUEST", "RELEASE_REQUEST", "TELEMETRY_QUERY", "TELEMETRY_RESPONSE", "PROBE_REQUEST", "PROBE_RESPONSE"):
            value = int(re.search(rf"#define SPI_SYS_{name}\s+(0x[0-9A-Fa-f]+)u", header).group(1), 16)
            self.assertEqual(value, getattr(spi_system, name), name)
            self.assertGreaterEqual(value, 0x20, f"{name} must stay above the first 32 offsets of a slot, which the update protocol owns")
        for name, value in (("LINK_OK", spi_system.FLAG_LINK_OK), ("ESTOP_LATCHED", spi_system.FLAG_ESTOP_LATCHED), ("STOP_ENABLED", spi_system.FLAG_STOP_ENABLED)):
            self.assertEqual(int(re.search(rf"#define SPI_SYS_FLAG_{name}\s+(0x[0-9A-Fa-f]+)u", header).group(1), 16), value, name)

    def test_the_tier_is_decided_before_the_type_so_a_self_type_never_reaches_the_bus(self):
        main = text(H7 / "CM4" / "boot" / "bootloader_main.c")
        dispatch = main[main.index("static uint8_t DispatchFrame"):]
        self.assertLess(dispatch.index("SPI_TARGET_STACKA"), dispatch.index("SpiSys_IsCommand"))
        self.assertLess(dispatch.index("Relay_ToStackA"), dispatch.index("SpiSys_IsCommand"))

    def test_the_verdict_codes_follow_the_slot_scan_names(self):
        self.assertEqual(set(spi_system.VERDICTS), {slot_scan.ABSENT, slot_scan.OK, slot_scan.DUPLICATE, slot_scan.MISPLACED, slot_scan.NOISY})

    def test_switches_off_means_nothing_is_done(self):
        for frame_type, payload in ((spi_system.TELEMETRY_QUERY, b"\x00"), (spi_system.STOP_REQUEST, b"STOP\x01"), (spi_system.RELEASE_REQUEST, b"RELS"),
                                    (spi_system.PROBE_REQUEST, b"\x01")):
            self.assertEqual(spi_system.handle(spi_system.Config(), self.snapshot(), frame_type, payload), ("none", b""))
        self.assertEqual(spi_system.handle(spi_system.Config(True, False), self.snapshot(), spi_system.STOP_REQUEST, b"STOP\x01"), ("none", b""))

    def test_probe_response_decodes_back_to_the_verdict_names(self):
        scan_id, names = spi_system.decode_probe_response(spi_system.probe_response(self.snapshot(), 0x2A))
        self.assertEqual(scan_id, 0x2A)
        self.assertEqual(names, ["OK", "OK", "DUPLICATE", "ABSENT", "NOISY", "OK", "OK", "MISPLACED"])


if __name__ == "__main__":
    unittest.main()
