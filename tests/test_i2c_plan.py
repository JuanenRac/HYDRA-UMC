# =============================================================================
# HYDRA-UMC - tests/test_i2c_plan.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""I2C4 on the CM7 as the bus of the F-RAM: the Python twin (tools/i2c_plan.py) makes the same words as tests/test_i2c_plan.c, an independent check of every word against the I2C
specification, config/i2c4.cfg is the same as the starting values compiled into Common/i2c_plan.h and matches the clock tree, the CM7 bus uses the pins, the address and the memory
access of the F-RAM glue of the CM4, the switch ships off, and the new sources are in both runners with floors. Evidence: simulator (the peripheral double runs in tests/test_fram_i2c4.c)."""
import json
import re
import sys
import tempfile
import unittest
from fractions import Fraction
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import i2c_plan as ip  # noqa: E402

PLAN_H = (ROOT / "src/mcu_stm32h757/Common/i2c_plan.h").read_text(encoding="utf-8")
BUS_C = (ROOT / "src/mcu_stm32h757/CM7/FramI2c4.c").read_text(encoding="utf-8")
BUS_H = (ROOT / "src/mcu_stm32h757/CM7/FramI2c4.h").read_text(encoding="utf-8")
CM4_APP = (ROOT / "src/mcu_stm32h757/CM4/SpiApp.c").read_text(encoding="utf-8")
FRAM_H = (ROOT / "src/mcu_stm32h757/CM4/fram_i2c.h").read_text(encoding="utf-8")
PROFILE = (ROOT / "src/mcu_stm32h757/Common/platform_profile.h").read_text(encoding="utf-8")


def cfg(**changes):
    base = ip.load_config()
    base.update(changes)
    return base


def macro(name):
    return int(re.search(rf"#define {name} (\d+)UL", PLAN_H).group(1))


def violations(c, plan):
    """The I2C specification, taken apart again from the register fields with exact fractions (not the arithmetic of the plan)."""
    clk = Fraction(10**9, c["kernel_hz"])  # ns
    tp = (plan["presc"] + 1) * clk
    spec = ip.SPEC[ip.mode_of(c)]
    af_min = ip.AF_MIN_NS if c["analog_filter"] else 0
    af_max = ip.AF_MAX_NS if c["analog_filter"] else 0
    dnf = c["digital_filter"]
    found = []
    if (plan["scldel"] + 1) * tp < c["rise_ns"] + spec["setup"]:
        found.append("set-up")
    if plan["sdadel"] * tp < c["fall_ns"] - af_min - (dnf + 3) * clk:
        found.append("hold")
    if plan["sdadel"] * tp > spec["valid_max"] - c["rise_ns"] - af_max - (dnf + 4) * clk:
        found.append("valid")
    if (plan["scll"] + 1) * tp < spec["low"] or (plan["sclh"] + 1) * tp < spec["high"]:
        found.append("low/high")
    if plan["actual_hz"] > c["speed_hz"]:
        found.append("too fast")
    word = (plan["presc"] << 28) | (plan["scldel"] << 20) | (plan["sdadel"] << 16) | (plan["sclh"] << 8) | plan["scll"]
    if word != plan["timingr"]:
        found.append("word")
    return found


class TwinCases(unittest.TestCase):
    """The same words as tests/test_i2c_plan.c."""

    def test_the_words(self):
        cases = [
            ({}, 0x40C15664, 99601),
            ({"speed_hz": 400000, "rise_ns": 200, "fall_ns": 50}, 0x10E02246, 398406),
            ({"kernel_hz": 8_000_000}, 0x00502026, None),
            ({"kernel_hz": 48_000_000, "speed_hz": 400000, "rise_ns": 100}, 0x00902143, None),
            ({"kernel_hz": 120_000_000, "speed_hz": 300000, "rise_ns": 150, "digital_filter": 9}, 0x10F03E69, None),
            ({"kernel_hz": 200_000_000, "analog_filter": 0, "digital_filter": 15}, 0x80E15F6F, None),
        ]
        source = (ROOT / "tests/test_i2c_plan.c").read_text(encoding="utf-8")
        for changes, word, hz in cases:
            plan = ip.make_plan(cfg(**changes))
            self.assertEqual(plan["timingr"], word, changes)
            self.assertEqual(violations(cfg(**changes), plan), [], changes)
            self.assertIn("0x%08XUL" % word, source)
            if hz:
                self.assertEqual(plan["actual_hz"], hz)

    def test_no_timing(self):
        for changes in ({"kernel_hz": 1_000_000}, {"kernel_hz": 200_000_000, "speed_hz": 10_000}):
            with self.assertRaises(ValueError, msg=str(changes)):
                ip.make_plan(cfg(**changes))

    def test_every_number_out_of_range_is_refused(self):
        for changes in ({"kernel_hz": 999_999}, {"kernel_hz": 200_000_001}, {"speed_hz": 9_999}, {"speed_hz": 400_001}, {"rise_ns": 0}, {"rise_ns": 1001},
                        {"speed_hz": 100_001, "rise_ns": 301}, {"fall_ns": 0}, {"fall_ns": 301}, {"analog_filter": 2}, {"digital_filter": 16}):
            with self.assertRaises(ValueError, msg=str(changes)):
                ip.make_plan(cfg(**changes))

    def test_every_word_that_is_made_keeps_the_specification(self):
        made = 0
        for kernel in (2_000_000, 4_000_000, 8_000_000, 16_000_000, 25_000_000, 48_000_000, 64_000_000, 80_000_000, 100_000_000, 120_000_000, 200_000_000):
            for speed in (10_000, 50_000, 100_000, 100_001, 250_000, 400_000):
                for rise in (50, 100, 300, 600):
                    for analog in (0, 1):
                        for dnf in (0, 5, 10, 15):
                            c = cfg(kernel_hz=kernel, speed_hz=speed, rise_ns=rise, fall_ns=100, analog_filter=analog, digital_filter=dnf)
                            try:
                                plan = ip.make_plan(c)
                            except ValueError:
                                continue
                            made += 1
                            self.assertEqual(violations(c, plan), [], c)
                            for field, top in (("presc", 15), ("scldel", 15), ("sdadel", 15), ("sclh", 255), ("scll", 255)):
                                self.assertLessEqual(plan[field], top)
        self.assertGreater(made, 500)

    def test_the_c_test_has_the_same_sweep(self):
        source = (ROOT / "tests/test_i2c_plan.c").read_text(encoding="utf-8")
        for needle in ("2000000UL", "200000000UL", "100001UL", "250000UL", "{50u, 100u, 300u, 600u}", "d += 5u"):
            self.assertIn(needle, source)


class ConfigAndSources(unittest.TestCase):
    def test_the_file_is_valid_and_is_the_compiled_default(self):
        self.assertEqual(
            ip.load_config(),
            {
                "kernel_hz": macro("I2CP_DEFAULT_KERNEL_HZ"),
                "speed_hz": macro("I2CP_DEFAULT_SPEED_HZ"),
                "rise_ns": macro("I2CP_DEFAULT_RISE_NS"),
                "fall_ns": macro("I2CP_DEFAULT_FALL_NS"),
                "analog_filter": macro("I2CP_DEFAULT_ANALOG_FILTER"),
                "digital_filter": macro("I2CP_DEFAULT_DIGITAL_FILTER"),
            },
        )

    def test_the_kernel_clock_is_the_one_of_the_clock_tree(self):
        h757 = PROFILE[PROFILE.index("#elif defined(HYDRA_PLATFORM_H757)"):]
        self.assertEqual(int(re.search(r"#define HYDRA_CLOCK_KERNEL_I2C4_HZ (\d+)UL", h757).group(1)), ip.load_config()["kernel_hz"])
        self.assertIn("#define HYDRA_OWNER_I2C4 HYDRA_OWNER_CM7", h757)

    def test_the_c_code_refuses_the_same_limits(self):
        self.assertEqual((macro("I2CP_KERNEL_MIN_HZ"), macro("I2CP_KERNEL_MAX_HZ")), ip.LIMITS["kernel_hz"])
        self.assertEqual((macro("I2CP_SPEED_MIN_HZ"), macro("I2CP_SPEED_MAX_HZ")), ip.LIMITS["speed_hz"])
        self.assertEqual(macro("I2CP_STANDARD_MAX_HZ"), ip.STANDARD_MAX_HZ)
        self.assertEqual(macro("I2CP_DIGITAL_FILTER_MAX"), ip.LIMITS["digital_filter"][1])
        self.assertEqual((macro("I2CP_RISE_MAX_STANDARD_NS"), macro("I2CP_RISE_MAX_FAST_NS")), (ip.RISE_MAX_NS["standard"], ip.RISE_MAX_NS["fast"]))
        self.assertEqual(macro("I2CP_FALL_MAX_NS"), ip.FALL_MAX_NS)
        self.assertEqual((macro("I2CP_AF_MIN_NS"), macro("I2CP_AF_MAX_NS")), (ip.AF_MIN_NS, ip.AF_MAX_NS))

    def test_the_specification_numbers_are_the_same_in_c(self):
        for key, c_name in (("low", "low_min"), ("high", "high_min"), ("setup", "setup"), ("valid_max", "valid_max")):
            self.assertIn("uint64_t %s = fast ? %du : %du;" % (c_name, ip.SPEC["fast"][key], ip.SPEC["standard"][key]), PLAN_H)

    def test_values_out_of_range_in_the_file_are_refused(self):
        base = (ROOT / "config/i2c4.cfg").read_text(encoding="utf-8")
        for good, bad in (("kernel_hz = 100000000", "kernel_hz = 999999"), ("speed_hz = 100000", "speed_hz = 500000"), ("rise_ns = 400", "rise_ns = 1001"), ("analog = 1", "analog = 2"), ("digital = 0", "digital = 16")):
            with tempfile.TemporaryDirectory() as folder:
                path = Path(folder) / "i2c4.cfg"
                path.write_text(base.replace(good, bad), encoding="utf-8")
                with self.assertRaises(ValueError, msg=bad):
                    ip.load_config(path)

    def test_the_switch_ships_off_and_the_whole_unit_is_inside_it(self):
        self.assertIn("#define HYDRA_FRAM_I2C4_CM7 0", BUS_H)
        self.assertTrue(BUS_C.rstrip().endswith("#endif /* HYDRA_FRAM_I2C4_CM7 */"))
        self.assertEqual(BUS_C.count("#if HYDRA_FRAM_I2C4_CM7"), 1)

    def test_the_bus_is_the_one_of_the_cm4_glue(self):
        for needle in ("pins.Pin = GPIO_PIN_12 | GPIO_PIN_13;", "pins.Mode = GPIO_MODE_AF_OD;", "pins.Pull = GPIO_NOPULL;", "pins.Alternate = GPIO_AF4_I2C4;", "HAL_GPIO_Init(GPIOD, &pins);",
                       "I2C_MEMADD_SIZE_16BIT", "(uint16_t)(address7 << 1)", "I2C_ADDRESSINGMODE_7BIT", "I2C_NOSTRETCH_DISABLE"):
            self.assertIn(needle, BUS_C)
            self.assertIn(needle, CM4_APP)
        self.assertIn("#define FRAM_I2C_ADDRESS   0x50u", FRAM_H)

    def test_a_timing_of_zero_is_refused(self):
        self.assertIn("timingr == 0u", BUS_C)

    def test_the_new_sources_are_in_both_runners_and_have_floors(self):
        host = (ROOT / "tools/host_tests.py").read_text(encoding="utf-8")
        build = (ROOT / "tools/build_firmware.py").read_text(encoding="utf-8")
        for test in ("tests/test_i2c_plan.c", "tests/test_fram_i2c4.c"):
            self.assertIn(test, host)
        for token in ("test_i2c_plan", "test_fram_i2c4", "-DHYDRA_FRAM_I2C4_CM7=1", "FramI2c4.c"):
            self.assertIn(token, build)
        floors = json.loads((ROOT / "tools/host_coverage_floor.json").read_text(encoding="utf-8"))["modules"]
        for module in ("Common/i2c_plan.h", "CM7/FramI2c4.c"):
            self.assertEqual(floors["src/mcu_stm32h757/" + module], 100)


if __name__ == "__main__":
    unittest.main()
