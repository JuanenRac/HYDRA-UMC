# =============================================================================
# HYDRA-UMC - tests/test_gateway_stack_a.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""Source guards of the stack tier of the gateway of the H757 board (Common/gateway_stack_a.h and the FDCAN1 binding CM7/GatewayFdcan1.c): the id scheme and the types that the boards
transmit are the ones of the CM4 bootloader, the time-out comes from config/gateway.cfg with the same limits in Python and C, the switch ships off and the sources are in both runners.
Evidence: simulator (the bus double runs in tests/test_gateway_stack_a.c and tests/test_gateway_fdcan1.c)."""
import json
import re
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import gateway_cfg as gc  # noqa: E402

STACK = (ROOT / "src/mcu_stm32h757/Common/gateway_stack_a.h").read_text(encoding="utf-8")
BINDING = (ROOT / "src/mcu_stm32h757/CM7/GatewayFdcan1.c").read_text(encoding="utf-8")
HEADER = (ROOT / "src/mcu_stm32h757/CM7/GatewayFdcan1.h").read_text(encoding="utf-8")
CM4_COMMON = (ROOT / "src/mcu_stm32h757/CM4/boot/bootloader_common.h").read_text(encoding="utf-8")
CM4_PROTOCOL = (ROOT / "src/mcu_stm32h757/CM4/boot/bootloader_protocol.c").read_text(encoding="utf-8")
BOOT_IDS = (ROOT / "firmware_base/boot/boot_protocol_ids.h").read_text(encoding="utf-8")


def offsets() -> dict[str, int]:
    found = {}
    for text in (CM4_COMMON, BOOT_IDS):
        for name, value in re.findall(r"#define\s+(OFS_[A-Z_0-9]+)\s+(0x[0-9A-Fa-f]+)", text):
            found[name] = int(value, 16)
    return found


class StackTierGuards(unittest.TestCase):
    def test_the_id_scheme_is_the_one_of_the_cm4(self):
        self.assertIn("#define CAN_ID_STACKA_BASE 0x600UL", CM4_COMMON)
        self.assertIn("#define STACKA_SLOT_WINDOW 0x40UL", CM4_COMMON)
        self.assertIn("#define GSA_BASE_ID 0x600UL", STACK)
        self.assertIn("#define GSA_SLOT_WINDOW 0x40UL", STACK)
        self.assertIn("#define GSA_SLOT_MAX 7u", STACK)
        self.assertIn("slot > 7", CM4_PROTOCOL)

    def test_the_types_the_boards_transmit_are_the_ones_of_the_cm4(self):
        expression = CM4_PROTOCOL[CM4_PROTOCOL.index("uint8_t transmitted_by_the_board"): CM4_PROTOCOL.index("if (transmitted_by_the_board)")]
        names = re.findall(r"OFS_[A-Z_0-9]+", expression)
        values = offsets()
        expected = sorted({values[name] for name in names})
        self.assertEqual(len(expected), 12)
        start = STACK.index("#define GSA_TX_BY_BOARD_MASK")
        mask = STACK[start: STACK.index("typedef struct", start)]
        in_mask = sorted(int(bit, 16) for bit in re.findall(r"1ULL << (0x[0-9A-Fa-f]+)", mask))
        self.assertEqual(in_mask, expected)

    def test_the_poll_type_is_the_one_of_the_cm4(self):
        self.assertEqual(offsets()["OFS_POLL_STATUS"], 0x17)
        self.assertIn("#define GSA_TYPE_POLL_STATUS 0x17u", STACK)

    def test_the_wait_is_bounded_by_the_clock(self):
        self.assertIn("< s->timeout_ms", STACK)
        self.assertIn("now_ms", STACK)


class BindingGuards(unittest.TestCase):
    def test_the_switch_ships_off_and_the_whole_unit_is_inside_it(self):
        self.assertIn("#define HYDRA_GATEWAY_FDCAN1 0", HEADER)
        self.assertTrue(BINDING.rstrip().endswith("#endif /* HYDRA_GATEWAY_FDCAN1 */"))
        self.assertEqual(BINDING.count("#if HYDRA_GATEWAY_FDCAN1"), 1)

    def test_it_never_starts_the_peripheral(self):
        for call in ("HAL_FDCAN_Init", "HAL_FDCAN_Start", "HAL_FDCAN_ConfigFilter", "HAL_FDCAN_ConfigGlobalFilter"):
            self.assertNotIn(call, BINDING)

    def test_both_sources_are_in_both_runners_and_have_floors(self):
        host = (ROOT / "tools/host_tests.py").read_text(encoding="utf-8")
        build = (ROOT / "tools/build_firmware.py").read_text(encoding="utf-8")
        for test in ("tests/test_gateway_stack_a.c", "tests/test_gateway_fdcan1.c"):
            self.assertIn(test, host)
        self.assertIn("test_gateway_stack_a", build)
        self.assertIn("-DHYDRA_GATEWAY_FDCAN1=1", build)
        self.assertIn("GatewayFdcan1.c", build)
        floors = json.loads((ROOT / "tools/host_coverage_floor.json").read_text(encoding="utf-8"))["modules"]
        self.assertEqual(floors["src/mcu_stm32h757/Common/gateway_stack_a.h"], 100)
        self.assertEqual(floors["src/mcu_stm32h757/CM7/GatewayFdcan1.c"], 100)


class ConfigTests(unittest.TestCase):
    def test_the_file_is_valid(self):
        self.assertEqual(gc.load_config(), {"timeout_ms": 200})

    def test_values_out_of_range_are_refused(self):
        base = (ROOT / "config/gateway.cfg").read_text(encoding="utf-8")
        for bad in ("timeout_ms = 0", "timeout_ms = 1001"):
            with tempfile.TemporaryDirectory() as folder:
                path = Path(folder) / "gateway.cfg"
                path.write_text(base.replace("timeout_ms = 200", bad), encoding="utf-8")
                with self.assertRaises(ValueError, msg=bad):
                    gc.load_config(path)

    def test_the_c_code_refuses_the_same_limits(self):
        self.assertIn("#define GSA_TIMEOUT_MIN_MS %du" % gc.TIMEOUT_MIN_MS, STACK)
        self.assertIn("#define GSA_TIMEOUT_MAX_MS %du" % gc.TIMEOUT_MAX_MS, STACK)


if __name__ == "__main__":
    unittest.main()
