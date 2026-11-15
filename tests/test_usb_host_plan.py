# =============================================================================
# HYDRA-UMC - tests/test_usb_host_plan.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The USB host of the CM7: config/usb_host.cfg is valid and is what the C test uses as its starting values, the limits and the FIFO words are the same in C and Python, the pins are
those of the platform profile, the switch ships off, and the new sources are in both runners with floors. Evidence: simulator (the controller double runs in tests/test_usb_host_fs.c)."""
import json
import re
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import usb_host_plan as up  # noqa: E402

PLAN_H = (ROOT / "src/mcu_stm32h757/Common/usb_host_plan.h").read_text(encoding="utf-8")
HOST_C = (ROOT / "src/mcu_stm32h757/CM7/UsbHostFs.c").read_text(encoding="utf-8")
HOST_H = (ROOT / "src/mcu_stm32h757/CM7/UsbHostFs.h").read_text(encoding="utf-8")


def macro(name):
    return int(re.search(rf"#define {name} (\d+)", PLAN_H).group(1))


class Plan(unittest.TestCase):
    def test_the_file_is_valid_and_is_the_set_of_the_c_test(self):
        cfg = up.load_config()
        self.assertEqual(cfg, {"host_channels": 8, "vbus_sensing": 1, "sof_output": 0, "irq_priority": 5})
        source = (ROOT / "tests/test_usb_host_plan.c").read_text(encoding="utf-8")
        self.assertIn("{8u, 1u, 0u, 5u}", source)

    def test_the_limits_are_the_same_in_c(self):
        self.assertEqual((macro("USBP_CHANNELS_MIN"), macro("USBP_CHANNELS_MAX")), up.CHANNELS)
        self.assertEqual(macro("USBP_IRQ_PRIORITY_MAX"), up.IRQ_PRIORITY_MAX)
        self.assertEqual(macro("USBP_FIFO_TOTAL_WORDS"), up.FIFO_TOTAL_WORDS)
        self.assertEqual((macro("USBP_FIFO_RX_WORDS"), macro("USBP_FIFO_NPTX_WORDS"), macro("USBP_FIFO_PTX_WORDS")), (up.FIFO_WORDS["rx"], up.FIFO_WORDS["nptx"], up.FIFO_WORDS["ptx"]))
        self.assertEqual(macro("USBP_RX_PACKET_WORDS"), up.RX_PACKET_WORDS)

    def test_the_fifos_fit_the_controller(self):
        self.assertLessEqual(up.fifo_words_used(), up.FIFO_TOTAL_WORDS)
        self.assertGreaterEqual(up.FIFO_WORDS["rx"], up.RX_PACKET_WORDS + up.CHANNELS[1])

    def test_a_bad_file_is_refused(self):
        base = (ROOT / "config/usb_host.cfg").read_text(encoding="utf-8").replace("\r\n", "\n")
        for good, bad in (("host_channels = 8", "host_channels = 13"), ("host_channels = 8", "host_channels = 0"), ("vbus_sensing = 1", "vbus_sensing = 2"), ("sof_output = 0", "sof_output = 2"), ("priority = 5", "priority = 16")):
            text = base.replace(good, bad, 1)
            with tempfile.TemporaryDirectory() as folder:
                path = Path(folder) / "usb_host.cfg"
                path.write_text(text, encoding="utf-8")
                with self.assertRaises(ValueError, msg=bad):
                    up.load_config(path)


class Sources(unittest.TestCase):
    def test_the_pins_are_those_of_the_profile(self):
        profile = json.loads((ROOT / "docs/platform_profiles.json").read_text(encoding="utf-8"))
        text = json.dumps(profile)
        for needle in ('"dm": "PA11"', '"dp": "PA12"', '"vbus": "PA9"', '"mode": "host"'):
            self.assertIn(needle, text)
        for needle in ("GPIO_PIN_11 | GPIO_PIN_12", "GPIO_PIN_9", "GPIO_AF10_OTG1_FS", "HCD_SPEED_FULL", "HCD_PHY_EMBEDDED"):
            self.assertIn(needle, HOST_C)

    def test_the_switch_ships_off_and_the_whole_unit_is_inside_it(self):
        self.assertIn("#define HYDRA_USB_HOST_CM7 0", HOST_H)
        self.assertTrue(HOST_C.rstrip().endswith("#endif /* HYDRA_USB_HOST_CM7 */"))
        self.assertEqual(HOST_C.count("#if HYDRA_USB_HOST_CM7"), 1)

    def test_the_new_sources_are_in_both_runners_and_have_floors(self):
        host = (ROOT / "tools/host_tests.py").read_text(encoding="utf-8")
        build = (ROOT / "tools/build_firmware.py").read_text(encoding="utf-8")
        for test in ("tests/test_usb_host_plan.c", "tests/test_usb_host_fs.c"):
            self.assertIn(test, host)
        for token in ("test_usb_host_plan", "test_usb_host_fs", "-DHYDRA_USB_HOST_CM7=1", "UsbHostFs.c"):
            self.assertIn(token, build)
        floors = json.loads((ROOT / "tools/host_coverage_floor.json").read_text(encoding="utf-8"))["modules"]
        for module in ("Common/usb_host_plan.h", "CM7/UsbHostFs.c"):
            self.assertEqual(floors["src/mcu_stm32h757/" + module], 100)


if __name__ == "__main__":
    unittest.main()
