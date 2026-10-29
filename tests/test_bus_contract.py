# =============================================================================
# HYDRA-UMC - tests/test_bus_contract.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""docs/bus_contract.json says the same as the firmware sources and the identifier table."""
import json
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import can_ids  # noqa: E402

CONTRACT = json.loads((ROOT / "docs" / "bus_contract.json").read_text(encoding="utf-8"))
BUSES = CONTRACT["buses"]
TIMING_FIELDS = {
    "prescaler": "NominalPrescaler",
    "sync_jump_width": "NominalSyncJumpWidth",
    "time_segment_1": "NominalTimeSeg1",
    "time_segment_2": "NominalTimeSeg2",
}


def can_inits(path: Path) -> dict[str, dict[str, str]]:
    """For each FDCAN handle initialised in a source, the Init fields it sets (as written)."""
    handles: dict[str, dict[str, str]] = {}
    for handle, field, value in re.findall(r"\b(hfdcan\d\w*)\.Init\.(\w+)\s*=\s*([^;]+);", path.read_text(encoding="utf-8")):
        handles.setdefault(handle, {})[field] = value.strip()
    return handles


class BusContractTests(unittest.TestCase):
    def test_the_bit_rate_follows_from_the_timing_and_the_assumed_clock(self):
        for name, bus in BUSES.items():
            if bus["kind"] != "can":
                continue
            timing = bus["nominal_bit_timing"]
            quanta = 1 + timing["time_segment_1"] + timing["time_segment_2"]
            self.assertEqual(quanta, bus["time_quanta_per_bit"], name)
            self.assertEqual(bus["assumed_kernel_clock_hz"] // timing["prescaler"] // quanta, bus["nominal_bit_rate_bps"], name)

    def test_every_can_source_sets_the_timing_and_format_of_the_contract(self):
        for name, bus in BUSES.items():
            if bus["kind"] != "can":
                continue
            checked = 0
            for relative in bus["sources"]:
                for handle, fields in can_inits(ROOT / relative).items():
                    # the downlink is the FDCAN2 handle of the controller board, every other handle is on STACK A
                    on_downlink = "fdcan2" in handle
                    if on_downlink != (name == "tool_head_downlink"):
                        continue
                    checked += 1
                    expected_format = "FDCAN_FRAME_CLASSIC" if bus["frame_format"] == "classic" else "FDCAN_FRAME_FD_BRS"
                    self.assertEqual(fields.get("FrameFormat"), expected_format, f"{relative} {handle}")
                    for key, field in TIMING_FIELDS.items():
                        self.assertEqual(fields.get(field), str(bus["nominal_bit_timing"][key]), f"{relative} {handle} {field}")
            self.assertGreater(checked, 0, f"no FDCAN initialisation found for {name}")

    def test_the_classic_format_carries_at_most_eight_bytes_and_no_rate_switching(self):
        for name, bus in BUSES.items():
            if bus["kind"] == "can" and bus["frame_format"] == "classic":
                self.assertEqual(bus["max_data_bytes"], 8, name)
                self.assertFalse(bus["bit_rate_switching"], name)

    def test_the_slot_window_and_base_match_the_identifier_table_and_the_bootloaders(self):
        stack = BUSES["stack_a"]
        self.assertEqual(stack["slot_window"], can_ids.SLOT_WINDOW)
        base = int(stack["slot_base"], 16)
        for relative in ("src/mcu_stm32h745/CM4/boot/bootloader_common.h", "src/mcu_stm32g474/boot/bootloader_common.h"):
            found = re.search(r"#define\s+CAN_ID_STACKA_BASE\s+(0x[0-9A-Fa-f]+)", (ROOT / relative).read_text(encoding="utf-8"))
            self.assertIsNotNone(found, relative)
            self.assertEqual(int(found.group(1), 16), base, relative)
        # the last slot still fits in the 11 identifier bits for every message of the table
        last_base = base + (stack["slots"] - 1) * stack["slot_window"]
        for message in can_ids.MESSAGES:
            self.assertLessEqual(can_ids.identifier(last_base, message), 0x7FF, message.name)

    def test_the_spi_clock_default_and_maximum_match_the_host_driver(self):
        spi = BUSES["cm5_spi"]
        source = (ROOT / "src/cm5_host/ipc_driver/src/ipc_driver.c").read_text(encoding="utf-8")
        self.assertIn(f"{spi['default_clock_hz']}", source.replace("_", ""))
        header = (ROOT / "src/cm5_host/ipc_driver/src/ipc_driver.h").read_text(encoding="utf-8")
        self.assertIn(f"{spi['maximum_clock_hz']:,}", header)

    def test_every_source_named_in_the_contract_exists(self):
        for name, bus in BUSES.items():
            for relative in bus["sources"]:
                self.assertTrue((ROOT / relative).is_file(), f"{name}: {relative}")


if __name__ == "__main__":
    unittest.main()
