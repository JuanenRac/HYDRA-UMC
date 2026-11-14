# =============================================================================
# HYDRA-UMC - tests/test_gateway_dispatch.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The dispatch of the gateway core (gateway_dispatch.h) sends a frame of the host to the same three targets as the gateway of the H745 board, with the numbers that are on the wire
today, answers STATUS_ERROR instead of silence when the other core or the stack does not answer, is free of the HAL, and its host test is registered in both runners for both platforms.
Evidence: simulator (text checks); the host test runs the dispatch against a double of the mailbox."""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = (ROOT / "src/mcu_stm32h757/Common/gateway_dispatch.h").read_text(encoding="utf-8")
CM4_COMMON = ROOT / "src/mcu_stm32h757/CM4/boot/bootloader_common.h"
SHARED_IDS = ROOT / "firmware_base/boot/boot_protocol_ids.h"


def cm4_defines():
    text = CM4_COMMON.read_text(encoding="utf-8")
    if SHARED_IDS.is_file():
        text += "\n" + SHARED_IDS.read_text(encoding="utf-8")
    return {m.group(1): m.group(2) for m in re.finditer(r"^#define\s+(\w+)\s+(\S+)", text, re.M)}


def own(name):
    return int(re.search(rf"#define {name} (0x[0-9A-Fa-f]+|\d+)u", HEADER).group(1), 0)


class GatewayDispatchTests(unittest.TestCase):
    def test_the_tiers_are_the_numbers_of_the_gateway_of_the_h745_board_and_of_the_host_driver(self):
        defs = cm4_defines()
        protocol = (ROOT / "src/cm5_host/spi_bridge/spi_bridge/protocol.py").read_text(encoding="utf-8")
        self.assertEqual(own("GW_TARGET_SELF"), int(defs["SPI_TARGET_SELF"]))
        self.assertEqual(own("GW_TARGET_OTHER"), int(defs["SPI_TARGET_CM7"]))   # "the other core" of the H745 gateway
        self.assertEqual(own("GW_TARGET_STACKA"), int(defs["SPI_TARGET_STACKA"]))
        self.assertEqual(own("GW_TARGET_SELF"), int(re.search(r"^SPI_TARGET_SELF = (\d+)", protocol, re.M).group(1)))

    def test_the_status_frame_and_the_error_code_are_the_ones_of_the_bootloaders(self):
        defs = cm4_defines()
        self.assertEqual(own("GW_FRAME_STATUS"), int(defs["OFS_STATUS"], 0))
        self.assertEqual(own("GW_STATUS_ERROR"), int(defs["STATUS_ERROR"], 0))

    def test_the_frame_is_the_twelve_bytes_that_the_link_stages(self):
        link = (ROOT / "src/mcu_stm32h757/Common/host_link_master.h").read_text(encoding="utf-8")
        self.assertEqual(own("GW_FRAME_BYTES"), int(re.search(r"#define HOST_LINK_HEAD_BYTES (\d+)u", link).group(1)))

    def test_the_header_is_free_of_the_hal_and_of_any_address(self):
        self.assertNotIn("HAL_", HEADER)
        self.assertNotIn("0x38000000", HEADER)
        self.assertNotIn("stm32h7xx", HEADER)

    def test_a_target_that_does_not_answer_gets_status_error_never_silence(self):
        other = HEADER.split("if (tier == GW_TARGET_OTHER)", 1)[1].split("if (tier == GW_TARGET_STACKA)", 1)[0]
        self.assertIn("Ipc_PostCommand", other)
        self.assertIn("Ipc_WaitResponse", other)
        self.assertLess(other.index("Ipc_PostCommand"), other.index("Ipc_WaitResponse"))
        self.assertIn("Gateway_PackError", other)
        stack = HEADER.split("if (tier == GW_TARGET_STACKA)", 1)[1].split("g->counters.unknown_tier++", 1)[0]
        self.assertIn("Gateway_PackError", stack)

    def test_an_unknown_tier_is_not_run_and_not_answered(self):
        tail = HEADER.split("g->counters.unknown_tier++", 1)[1].split("}", 1)[0]
        self.assertIn("return 0", tail)

    def test_the_host_test_is_registered_in_both_runners_for_both_platforms(self):
        host = (ROOT / "tools/host_tests.py").read_text(encoding="utf-8")
        script = (ROOT / "tools/build_firmware.py").read_text(encoding="utf-8")
        self.assertEqual(host.count('"tests/test_gateway_dispatch.c"'), 2)
        self.assertIn("test_gateway_dispatch", script)
        floor = (ROOT / "tools/host_coverage_floor.json").read_text(encoding="utf-8")
        self.assertIn("src/mcu_stm32h757/Common/gateway_dispatch.h", floor)


if __name__ == "__main__":
    unittest.main()
