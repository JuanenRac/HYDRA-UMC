# =============================================================================
# HYDRA-UMC - tests/test_fdcan_timing.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""Every place of the firmware that sets up an FDCAN peripheral uses the same bit timing, and that timing gives the bit rate of the stack bus.

The sources set a prescaler and the time segments; the bit rate is only right if the FDCAN kernel clock is the one the timing assumes (bench rows 9
and 10 of docs/BENCH_TESTS.md measure that on the boards). This test cannot see the clock, but it fails when one init site is changed and the
others are not, which would split the bus, and it computes the rate the timing gives for the assumed clock. Evidence: simulator (source check)."""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SITES = (
    "src/mcu_stm32g474/boot/bootloader_main.c",
    "src/mcu_stm32g474/RobotControllerRelay.c",
    "src/mcu_stm32h757/CM4/boot/bootloader_main.c",
    "src/mcu_stm32h757/CM4/KinematicBrainCan.c",
)
FIELDS = ("NominalPrescaler", "NominalSyncJumpWidth", "NominalTimeSeg1", "NominalTimeSeg2",
          "DataPrescaler", "DataSyncJumpWidth", "DataTimeSeg1", "DataTimeSeg2")
ASSUMED_KERNEL_HZ = 16_000_000   # the clock the comments of the bootloaders assume; measured on a board in bench row 10
EXPECTED_BIT_RATE = 1_000_000    # the nominal rate of the stack bus (docs/bus_contract.json, README)


def init_blocks(text):
    """Each FDCAN handle with the values written to its Init structure: {handle: {field: int}}."""
    handles = {}
    for handle, field, value in re.findall(r"\b(\w+)\.Init\.(" + "|".join(FIELDS) + r")\s*=\s*(\d+)\s*;", text):
        handles.setdefault(handle, {})[field] = int(value)
    return handles


def bit_rate(prescaler, seg1, seg2, kernel_hz):
    return kernel_hz / (prescaler * (1 + seg1 + seg2))


def sample_point(seg1, seg2):
    return (1 + seg1) / (1 + seg1 + seg2)


class FdcanTimingTests(unittest.TestCase):
    def setUp(self):
        self.blocks = {}
        for site in SITES:
            for handle, values in init_blocks((ROOT / site).read_text(encoding="utf-8")).items():
                self.blocks[f"{site}:{handle}"] = values

    def test_every_site_was_found_and_sets_every_field(self):
        sites_seen = {key.split(":")[0] for key in self.blocks}
        self.assertEqual(sites_seen, set(SITES))
        self.assertGreaterEqual(len(self.blocks), 5)   # the G474 bootloader, two relay handles, the CM4 bootloader and the brain
        for key, values in self.blocks.items():
            self.assertEqual(set(values), set(FIELDS), key)

    def test_all_sites_use_the_same_timing(self):
        timings = {tuple(values[f] for f in FIELDS) for values in self.blocks.values()}
        self.assertEqual(len(timings), 1, f"the init sites disagree: {sorted(timings)}")

    def test_the_timing_gives_the_stack_bus_rate_at_the_assumed_clock(self):
        values = next(iter(self.blocks.values()))
        for prefix in ("Nominal", "Data"):
            rate = bit_rate(values[prefix + "Prescaler"], values[prefix + "TimeSeg1"], values[prefix + "TimeSeg2"], ASSUMED_KERNEL_HZ)
            self.assertEqual(rate, EXPECTED_BIT_RATE, prefix)

    def test_sample_point_and_jump_width_are_inside_what_the_peripheral_allows(self):
        values = next(iter(self.blocks.values()))
        sample = sample_point(values["NominalTimeSeg1"], values["NominalTimeSeg2"])
        self.assertTrue(0.75 <= sample <= 0.90, sample)
        self.assertLessEqual(values["NominalSyncJumpWidth"], values["NominalTimeSeg2"])
        self.assertLessEqual(values["DataSyncJumpWidth"], values["DataTimeSeg2"])

    def test_a_different_kernel_clock_would_change_the_rate(self):
        """The reason bench row 10 exists: the same timing at another clock is another bit rate."""
        values = next(iter(self.blocks.values()))
        rate = bit_rate(values["NominalPrescaler"], values["NominalTimeSeg1"], values["NominalTimeSeg2"], 64_000_000)
        self.assertNotEqual(rate, EXPECTED_BIT_RATE)


if __name__ == "__main__":
    unittest.main()
