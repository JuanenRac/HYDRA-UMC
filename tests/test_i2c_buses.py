# =============================================================================
# HYDRA-UMC - tests/test_i2c_buses.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""I2C1 to I2C3 on the CM7: config/i2c_buses.cfg has a valid timing for each bus, the pins and the clock are those of the platform profile, the switch ships off, and the new
source is in both runners with a floor. Evidence: simulator (the peripheral double runs in tests/test_i2c_buses.c)."""
import json
import re
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import i2c_plan as ip  # noqa: E402

BUS_C = (ROOT / "src/mcu_stm32h757/CM7/I2cBuses.c").read_text(encoding="utf-8")
BUS_H = (ROOT / "src/mcu_stm32h757/CM7/I2cBuses.h").read_text(encoding="utf-8")
PROFILE = json.loads((ROOT / "docs/platform_profiles.json").read_text(encoding="utf-8"))


def find(node, key):
    if isinstance(node, dict):
        if key in node:
            return node[key]
        for value in node.values():
            found = find(value, key)
            if found is not None:
                return found
    return None


class Config(unittest.TestCase):
    def test_every_bus_has_a_valid_timing(self):
        buses = ip.load_buses_config()
        self.assertEqual(tuple(buses), ("i2c1", "i2c2", "i2c3"))
        for name, cfg in buses.items():
            self.assertLessEqual(ip.make_plan(cfg)["actual_hz"], cfg["speed_hz"], name)
        self.assertEqual(ip.mode_of(buses["i2c2"]), "fast")
        self.assertEqual(ip.make_plan(buses["i2c2"])["timingr"], 0x10E02246)  # the lines of the fast case of tests/test_i2c_plan.c

    def test_the_kernel_clock_is_the_apb1_clock_of_the_tree(self):
        profile = (ROOT / "src/mcu_stm32h757/Common/platform_profile.h").read_text(encoding="utf-8")
        apb1 = int(re.search(r"#define HYDRA_CLOCK_APB1_HZ (\d+)UL", profile).group(1))
        for name, cfg in ip.load_buses_config().items():
            self.assertEqual(cfg["kernel_hz"], apb1, name)

    def test_a_bad_file_is_refused(self):
        base = (ROOT / "config/i2c_buses.cfg").read_text(encoding="utf-8").replace("\r\n", "\n")
        for good, bad in (("[i2c3]", "[i2c4]"), ("speed_hz = 400000", "speed_hz = 500000"), ("[i2c2]\n# The touch controller of the display: SCL PH4, SDA PH5. Fast mode.\nkernel_hz = 100000000", "[i2c2]\nkernel_hz = 1000000")):
            text = base.replace(good, bad, 1)
            self.assertNotEqual(text, base, bad)
            with tempfile.TemporaryDirectory() as folder:
                path = Path(folder) / "i2c_buses.cfg"
                path.write_text(text, encoding="utf-8")
                with self.assertRaises(ValueError, msg=bad):
                    ip.load_buses_config(path)


class Sources(unittest.TestCase):
    def test_the_pins_are_those_of_the_profile(self):
        pins = {name: find(PROFILE, name)["pins"] for name in ("I2C1", "I2C2", "I2C3")}
        self.assertEqual(pins["I2C1"], {"scl": "PB6", "sda": "PB7"})
        self.assertEqual(pins["I2C2"], {"scl": "PH4", "sda": "PH5"})
        self.assertEqual(pins["I2C3"], {"scl": "PA8", "sda": "PC9"})
        for needle in ("GPIOB, GPIO_PIN_6 | GPIO_PIN_7", "GPIOH, GPIO_PIN_4 | GPIO_PIN_5", "GPIOA, GPIO_PIN_8", "GPIOC, GPIO_PIN_9", "GPIO_MODE_AF_OD", "GPIO_NOPULL"):
            self.assertIn(needle, BUS_C)

    def test_the_switch_ships_off_and_the_whole_unit_is_inside_it(self):
        self.assertIn("#define HYDRA_I2C_BUSES_CM7 0", BUS_H)
        self.assertTrue(BUS_C.rstrip().endswith("#endif /* HYDRA_I2C_BUSES_CM7 */"))
        self.assertEqual(BUS_C.count("#if HYDRA_I2C_BUSES_CM7"), 1)
        self.assertIn("timingr == 0u", BUS_C)

    def test_the_source_is_in_both_runners_and_has_a_floor(self):
        host = (ROOT / "tools/host_tests.py").read_text(encoding="utf-8")
        build = (ROOT / "tools/build_firmware.py").read_text(encoding="utf-8")
        self.assertIn("tests/test_i2c_buses.c", host)
        for token in ("test_i2c_buses", "-DHYDRA_I2C_BUSES_CM7=1", "I2cBuses.c"):
            self.assertIn(token, build)
        floors = json.loads((ROOT / "tools/host_coverage_floor.json").read_text(encoding="utf-8"))["modules"]
        self.assertEqual(floors["src/mcu_stm32h757/CM7/I2cBuses.c"], 100)


if __name__ == "__main__":
    unittest.main()
