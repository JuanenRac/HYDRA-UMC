# =============================================================================
# HYDRA-UMC - tests/test_relay_filter_and_outputs_model.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""Host model of two things the controller board does in silicon: the receive filter of the FDCAN peripheral (a classic filter accepts an
identifier when it equals the filter value on the bits the mask selects) and the forced-off level of the six driver enable pins. The constants
are read from the firmware sources, so a change in the source that breaks the property fails here. Random traffic uses a fixed seed.
Evidence: simulator. The measurement on a board with foreign traffic at full rate stays a bench test."""
import random
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
G4 = ROOT / "src" / "mcu_stm32g474"
SEED = 20260713
STD_ID_MASK = 0x7FF
FOREIGN_FRAMES = 20000


def text(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def define(source: str, name: str) -> int:
    found = re.search(rf"#define\s+{name}\s+\(?(0x[0-9A-Fa-f]+|\d+)", source)
    assert found, f"{name} is not defined"
    return int(found.group(1), 0)


def accepted(identifier: int, filter_id: int, mask: int) -> bool:
    """Classic filter of the FDCAN peripheral: the bits selected by the mask must match."""
    return (identifier & mask) == (filter_id & mask)


RELAY = text(G4 / "RobotControllerRelay.c")
COMMON = text(G4 / "boot" / "bootloader_common.h")
SLOT_BASE = define(COMMON, "CAN_ID_STACKA_BASE")
SLOT_WINDOW = define(COMMON, "STACKA_SLOT_WINDOW")
SLOT_MASK = define(RELAY, "RELAY_SLOT_FILTER_MASK")
ESTOP_ID = define(RELAY, "RELAY_ESTOP_CAN_ID")


class SlotFilterModel(unittest.TestCase):
    def test_the_mask_selects_exactly_the_slot_window(self):
        self.assertEqual(SLOT_MASK, STD_ID_MASK & ~(SLOT_WINDOW - 1), "the mask keeps the bits above the window and ignores the ones inside it")
        self.assertEqual(SLOT_BASE % SLOT_WINDOW, 0, "the base of the slots is aligned to the window")

    def test_the_slots_do_not_overlap_the_emergency_stop(self):
        for slot in range(8):
            base = SLOT_BASE + slot * SLOT_WINDOW
            self.assertFalse(accepted(ESTOP_ID, base, SLOT_MASK), f"slot {slot} window must not swallow the stop identifier")
            self.assertLessEqual(base + SLOT_WINDOW - 1, STD_ID_MASK)

    def test_every_identifier_of_the_own_window_passes_and_no_other_slot_does(self):
        for slot in range(8):
            base = SLOT_BASE + slot * SLOT_WINDOW
            for identifier in range(STD_ID_MASK + 1):
                inside = base <= identifier < base + SLOT_WINDOW
                passes = accepted(identifier, base, SLOT_MASK) or accepted(identifier, ESTOP_ID, STD_ID_MASK)
                self.assertEqual(passes, inside or identifier == ESTOP_ID, f"slot {slot} identifier {identifier:#05x}")

    def test_foreign_traffic_at_random_never_reaches_the_receive_fifo(self):
        rng = random.Random(SEED)
        for slot in (0, 3, 7):
            base = SLOT_BASE + slot * SLOT_WINDOW
            foreign = 0
            for _ in range(FOREIGN_FRAMES):
                identifier = rng.randrange(STD_ID_MASK + 1)
                if base <= identifier < base + SLOT_WINDOW or identifier == ESTOP_ID:
                    continue
                foreign += 1
                self.assertFalse(accepted(identifier, base, SLOT_MASK) or accepted(identifier, ESTOP_ID, STD_ID_MASK))
            self.assertGreater(foreign, FOREIGN_FRAMES * 0.9)

    def test_the_global_filter_rejects_what_matches_no_filter(self):
        self.assertRegex(RELAY, r"HAL_FDCAN_ConfigGlobalFilter\(&hfdcan1_app, FDCAN_REJECT, FDCAN_REJECT, FDCAN_REJECT_REMOTE, FDCAN_REJECT_REMOTE\)")
        self.assertRegex(RELAY, r"sf\.FilterID1 = slot_base;")
        self.assertRegex(RELAY, r"sf\.FilterID2 = RELAY_SLOT_FILTER_MASK;")
        self.assertRegex(RELAY, r"sf\.FilterID1 = RELAY_ESTOP_CAN_ID;\s*\n\s*sf\.FilterID2 = 0x7FF;")


MAIN = text(G4 / "STM32G474RE_main.c")


def force_disabled_mask(axis_mask: int, pins: list[int], level_high: bool, moder: int, bsrr: list[int]) -> int:
    """The same decisions as MotionOutputs_ForceDisabledMask: level first through the set/reset register, then the pins become outputs."""
    pin_mask = 0
    for i in range(6):
        if axis_mask & (1 << i):
            pin_mask |= 1 << pins[i]
    if pin_mask == 0:
        return moder
    bsrr.append(pin_mask if level_high else pin_mask << 16)
    for i in range(6):
        if axis_mask & (1 << i):
            moder = (moder & ~(3 << (pins[i] * 2))) | (1 << (pins[i] * 2))
    return moder


class DriverEnableOutputsModel(unittest.TestCase):
    def setUp(self):
        found = re.search(r"static const uint8_t pins\[6\] = \{([^}]*)\};", MAIN)
        assert found, "the pin table of the forced-off function is missing"
        self.pins = [int(p) for p in found.group(1).split(",")]
        self.pins_mask = re.search(r"#define MOTION_ENABLE_PINS_MASK \((.*)\)\n", MAIN).group(1)

    def test_the_pin_table_matches_the_documented_enable_mask(self):
        declared = sum(1 << int(m) for m in re.findall(r"1u << (\d+)", self.pins_mask))
        self.assertEqual(sum(1 << p for p in self.pins), declared)
        self.assertEqual(len(set(self.pins)), 6)

    def test_forcing_all_axes_covers_the_six_pins_and_the_default_level_is_high(self):
        self.assertRegex(MAIN, r"void MotionOutputs_ForceDisabled\(void\)\s*\{\s*MotionOutputs_ForceDisabledMask\(0x3Fu\);")
        self.assertEqual(define(MAIN, "MOTION_DISABLE_LEVEL_HIGH"), 1, "the shipped level switches the drivers off")
        bsrr: list[int] = []
        moder = force_disabled_mask(0x3F, self.pins, True, 0, bsrr)
        self.assertEqual(bsrr, [sum(1 << p for p in self.pins)])
        for pin in self.pins:
            self.assertEqual((moder >> (pin * 2)) & 3, 1, "pin becomes a general purpose output")

    def test_every_axis_subset_touches_only_its_own_pins_with_the_level_before_the_direction(self):
        rng = random.Random(SEED)
        for axis_mask in range(64):
            for level_high in (True, False):
                start = rng.getrandbits(32)
                bsrr: list[int] = []
                moder = force_disabled_mask(axis_mask, self.pins, level_high, start, bsrr)
                own = sum(1 << self.pins[i] for i in range(6) if axis_mask & (1 << i))
                if own == 0:
                    self.assertEqual((moder, bsrr), (start, []))
                    continue
                self.assertEqual(bsrr, [own if level_high else own << 16])
                touched = 0
                for pin in range(16):
                    if (moder >> (pin * 2)) & 3 != (start >> (pin * 2)) & 3:
                        touched |= 1 << pin
                self.assertEqual(touched & ~own, 0, "pins of other axes keep their mode")

    def test_the_source_writes_the_level_before_the_mode(self):
        body = MAIN[MAIN.index("void MotionOutputs_ForceDisabledMask"):MAIN.index("void MotionOutputs_ForceDisabled(void)")]
        self.assertLess(body.index("GPIOA->BSRR"), body.index("GPIOA->MODER = moder"))
        self.assertIn("if (pin_mask == 0) return;", body)

    def test_every_fault_path_cuts_the_drivers(self):
        self.assertRegex(MAIN, r"void HardFault_Handler\(void\) \{ MotionOutputs_ForceDisabled\(\);")
        self.assertRegex(MAIN, r"static void Error_Handler\(void\)\s*\{\s*MotionOutputs_ForceDisabled\(\);")
        self.assertIn("MotionOutputs_ForceDisabledMask(stop_policy.cut_mask);", RELAY)


if __name__ == "__main__":
    unittest.main()
