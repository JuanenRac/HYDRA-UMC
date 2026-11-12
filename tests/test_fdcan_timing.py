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
ASSUMED_KERNEL_HZ = 16_000_000   # the clock of the earlier bench that the default platform (H745) still assumes; measured on a board in bench row 10
H757_KERNEL_HZ = 100_000_000     # the clock of the H757 board: 25 MHz crystal, PLL2 at 100 MHz (docs/platform_profiles.json, profile h757_target)
HEADER = ROOT / "src/mcu_stm32h757/Common/platform_profile.h"
MACROS = {"Prescaler": "PRESCALER", "SyncJumpWidth": "SYNC_JUMP_WIDTH", "TimeSeg1": "TIME_SEG1", "TimeSeg2": "TIME_SEG2"}
EXPECTED_BIT_RATE = 1_000_000    # the nominal rate of the stack bus (docs/bus_contract.json, README)


def init_blocks(text):
    """Each FDCAN handle with the values written to its Init structure: {handle: {field: int}}."""
    handles = {}
    for handle, field, value in re.findall(r"\b(\w+)\.Init\.(" + "|".join(FIELDS) + r")\s*=\s*(\w+)\s*;", text):
        handles.setdefault(handle, {})[field] = value
    return handles


def platform_values(platform):
    """The HYDRA_STACK_BUS_* numbers the header gives for one platform (HYDRA_PLATFORM_H745 or HYDRA_PLATFORM_H757)."""
    text = HEADER.read_text(encoding="utf-8").replace("\r\n", "\n")
    start = re.search(rf"#(?:if|elif) defined\({platform}\)\n", text).end()
    end = re.search(r"\n#(?:elif defined|endif\n\n)", text[start:]).start()
    return {name: int(number.rstrip("UL")) for name, number in re.findall(r"#define (HYDRA_STACK_BUS_\w+) (\w+)", text[start : start + end])}


def resolved(values, platform):
    """The numbers of one init block, with the header macros of one platform substituted."""
    bus = platform_values(platform)
    return {field: int(token) if token.isdigit() else bus[token] for field, token in values.items()}


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

    def test_the_sites_that_belong_to_the_firmware_of_the_h757_take_the_timing_from_the_profile(self):
        for key, values in self.blocks.items():
            if "stm32h757" in key:
                self.assertEqual(set(values.values()), {f"HYDRA_STACK_BUS_{MACROS[next(k for k in MACROS if f.endswith(k))]}" for f in values}, key)
                self.assertIn('#include "platform_profile.h"', (ROOT / key.split(":")[0]).read_text(encoding="utf-8"))

    def test_all_sites_use_the_same_timing_on_each_platform(self):
        for platform in ("HYDRA_PLATFORM_H745", "HYDRA_PLATFORM_H757"):
            timings = {tuple(resolved(values, platform)[f] for f in FIELDS) for values in self.blocks.values()}
            if platform == "HYDRA_PLATFORM_H745":
                self.assertEqual(len(timings), 1, f"the init sites disagree: {sorted(timings)}")
            else:   # the G474 sites keep their own numbers (another chip, another clock); the H757 sites agree with each other
                h757 = {tuple(resolved(v, platform)[f] for f in FIELDS) for k, v in self.blocks.items() if "stm32h757" in k}
                self.assertEqual(len(h757), 1, sorted(h757))

    def test_the_timing_gives_the_stack_bus_rate_at_the_assumed_clock(self):
        values = resolved(next(iter(self.blocks.values())), "HYDRA_PLATFORM_H745")
        for prefix in ("Nominal", "Data"):
            rate = bit_rate(values[prefix + "Prescaler"], values[prefix + "TimeSeg1"], values[prefix + "TimeSeg2"], ASSUMED_KERNEL_HZ)
            self.assertEqual(rate, EXPECTED_BIT_RATE, prefix)

    def test_the_h757_timing_gives_one_megabit_at_100_mhz_with_prescaler_5(self):
        bus = platform_values("HYDRA_PLATFORM_H757")
        self.assertEqual(bus["HYDRA_STACK_BUS_KERNEL_CLOCK_HZ"], H757_KERNEL_HZ)
        self.assertEqual(bus["HYDRA_STACK_BUS_PRESCALER"], 5)
        for key, block in self.blocks.items():
            if "stm32h757" in key:
                values = resolved(block, "HYDRA_PLATFORM_H757")
                for prefix in ("Nominal", "Data"):
                    self.assertEqual(bit_rate(values[prefix + "Prescaler"], values[prefix + "TimeSeg1"], values[prefix + "TimeSeg2"], H757_KERNEL_HZ), EXPECTED_BIT_RATE, key)
                self.assertTrue(0.75 <= sample_point(values["NominalTimeSeg1"], values["NominalTimeSeg2"]) <= 0.90, key)

    def test_sample_point_and_jump_width_are_inside_what_the_peripheral_allows(self):
        values = resolved(next(iter(self.blocks.values())), "HYDRA_PLATFORM_H745")
        sample = sample_point(values["NominalTimeSeg1"], values["NominalTimeSeg2"])
        self.assertTrue(0.75 <= sample <= 0.90, sample)
        self.assertLessEqual(values["NominalSyncJumpWidth"], values["NominalTimeSeg2"])
        self.assertLessEqual(values["DataSyncJumpWidth"], values["DataTimeSeg2"])

    def test_a_different_kernel_clock_would_change_the_rate(self):
        """The reason bench row 10 exists: the same timing at another clock is another bit rate."""
        values = resolved(next(iter(self.blocks.values())), "HYDRA_PLATFORM_H745")
        rate = bit_rate(values["NominalPrescaler"], values["NominalTimeSeg1"], values["NominalTimeSeg2"], 64_000_000)
        self.assertNotEqual(rate, EXPECTED_BIT_RATE)


if __name__ == "__main__":
    unittest.main()
