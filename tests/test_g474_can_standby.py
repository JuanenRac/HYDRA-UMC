# =============================================================================
# HYDRA-UMC - tests/test_g474_can_standby.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The CAN standby pins of the G474 robot controller: one header, used by both firmwares, in agreement with the pinout document.

The chip package has no PG7 and no PG8; the standby lines are PC5 and PC8. Evidence: simulator (sources and document compared as text; no
transceiver was woken on a board).
"""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
G4 = ROOT / "src" / "mcu_stm32g474"
HEADER = G4 / "can_standby.h"
SOURCES = (G4 / "boot" / "bootloader_main.c", G4 / "RobotControllerRelay.c")
DOCUMENT = ROOT / "docs" / "PINOUT_STM32G474_ROBOT_CONTROLLER.md"


def text(path):
    return path.read_text(encoding="utf-8")


def defined(name):
    match = re.search(rf"#define\s+{name}\s+(\S+)", text(HEADER))
    return match.group(1) if match else None


class CanStandbyTests(unittest.TestCase):
    def test_the_header_names_pc5_and_pc8_on_port_c(self):
        self.assertEqual(defined("HYDRA_CAN1_STBY_PIN"), "GPIO_PIN_5")
        self.assertEqual(defined("HYDRA_CAN2_STBY_PIN"), "GPIO_PIN_8")
        self.assertEqual(defined("HYDRA_CAN1_STBY_PORT"), "GPIOC")
        self.assertEqual(defined("HYDRA_CAN2_STBY_PORT"), "GPIOC")
        self.assertIn("__HAL_RCC_GPIOC_CLK_ENABLE", text(HEADER))

    def test_no_firmware_source_drives_the_pins_that_do_not_exist(self):
        for path in SOURCES:
            source = text(path)
            self.assertNotIn("GPIOG", source, path.name)
            self.assertNotRegex(source, r"\bPG[78]\b", path.name)

    def test_both_firmwares_take_their_pins_from_the_header(self):
        for path in SOURCES:
            self.assertIn('#include "can_standby.h"', text(path), path.name)
        boot, relay = (text(p) for p in SOURCES)
        self.assertIn("HYDRA_CAN1_STBY_PIN", boot)
        self.assertNotIn("HYDRA_CAN2_STBY_PIN", boot)  # the bootloader only uses FDCAN1
        self.assertIn("HYDRA_CAN1_STBY_PIN", relay)
        self.assertIn("HYDRA_CAN2_STBY_PIN", relay)

    def test_each_standby_line_is_driven_low_to_wake_the_transceiver(self):
        for path in SOURCES:
            for write in re.findall(r"HAL_GPIO_WritePin\(HYDRA_CAN\d_STBY_PORT,[^;]*;", text(path)):
                self.assertIn("GPIO_PIN_RESET", write, write)

    def test_every_write_is_on_the_pin_of_the_same_line_as_its_port(self):
        for path in SOURCES:
            for port, pin in re.findall(r"HAL_GPIO_WritePin\(HYDRA_(CAN\d)_STBY_PORT,\s*HYDRA_(CAN\d)_STBY_PIN", text(path)):
                self.assertEqual(port, pin)

    def test_the_pinout_document_agrees(self):
        doc = text(DOCUMENT)
        self.assertRegex(doc, r"PC5\s+\|\s+CAN1_STBY")
        self.assertRegex(doc, r"PC8\s+\|\s+CAN2_STBY")
        self.assertNotRegex(doc, r"PG7\s+\|\s+CAN1_STBY")
        self.assertNotRegex(doc, r"PG8\s+\|\s+CAN2_STBY")
        self.assertIn("can_standby.h", doc)

    def test_the_standby_pins_are_not_used_by_anything_else_in_the_firmware(self):
        for path in list(G4.glob("*.c")) + list((G4 / "boot").glob("*.c")):
            source = text(path)
            for pin in ("GPIO_PIN_5", "GPIO_PIN_8"):
                for line in source.splitlines():
                    if "GPIOC" in line and pin in line and "STBY" not in line:
                        self.fail(f"{path.name} uses PC{pin[-1]}: {line.strip()}")


if __name__ == "__main__":
    unittest.main()
