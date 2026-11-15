# =============================================================================
# HYDRA-UMC - tests/test_touch_gt911.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The touch input of the interface: the Python twin (tools/touch_gt911.py) maps a raw position and decodes a frame of the GT911 the same way as tests/test_touch_gt911.c, config/lvgl.cfg
holds values the checks accept and the picture is the one of config/display.cfg, the register numbers are the same in C and Python, the switch ships off and nothing calls the header yet,
and it is in both runners with a floor. Evidence: simulator."""
import json
import re
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import display_plan  # noqa: E402
import touch_gt911 as tg  # noqa: E402

TOUCH_H = (ROOT / "src/mcu_stm32h757/Common/touch_gt911.h").read_text(encoding="utf-8")


def cfg(**changes):
    base = {"address7": 0x5D, "raw_width": 1280, "raw_height": 720, "width": 1280, "height": 720, "swap_xy": 0, "mirror_x": 0, "mirror_y": 0}
    base.update(changes)
    return base


class Mapping(unittest.TestCase):
    def test_the_same_numbers_as_the_c_test(self):
        self.assertEqual(tg.map_point(cfg(), 0, 0), (0, 0))
        self.assertEqual(tg.map_point(cfg(), 1279, 719), (1279, 719))
        self.assertEqual(tg.map_point(cfg(), 640, 360), (640, 360))
        self.assertEqual(tg.map_point(cfg(mirror_x=1), 0, 0), (1279, 0))
        self.assertEqual(tg.map_point(cfg(mirror_y=1), 0, 0), (0, 719))
        self.assertEqual(tg.map_point(cfg(raw_width=720, raw_height=1280, swap_xy=1), 0, 1279), (1279, 0))
        self.assertEqual(tg.map_point(cfg(raw_width=720, raw_height=1280, swap_xy=1), 719, 0), (0, 719))
        self.assertEqual(tg.map_point(cfg(raw_width=640, raw_height=360), 639, 359), (1278, 718))

    def test_a_point_outside_the_raw_size_is_refused(self):
        for raw in ((1280, 0), (0, 720), (-1, 0)):
            with self.assertRaises(ValueError, msg=str(raw)):
                tg.map_point(cfg(), *raw)

    def test_every_pixel_maps_inside_the_picture_whatever_the_orientation(self):
        for swap in (0, 1):
            for mx in (0, 1):
                for my in (0, 1):
                    c = cfg(raw_width=720 if swap else 1280, raw_height=1280 if swap else 720, swap_xy=swap, mirror_x=mx, mirror_y=my)
                    for rx in range(0, c["raw_width"], 97):
                        for ry in range(0, c["raw_height"], 89):
                            x, y = tg.map_point(c, rx, ry)
                            self.assertTrue(0 <= x < 1280 and 0 <= y < 720, (c, rx, ry))

    def test_the_checks(self):
        for bad in (cfg(address7=0x40), cfg(raw_width=0), cfg(raw_height=2049), cfg(width=0), cfg(height=2049), cfg(swap_xy=2), cfg(mirror_x=-1), cfg(poll_ms=4), cfg(poll_ms=101)):
            with self.assertRaises(ValueError, msg=str(bad)):
                tg.check(bad)
        tg.check(cfg(address7=0x14))


class Frames(unittest.TestCase):
    def test_a_frame(self):
        self.assertIsNone(tg.decode_frame(0x01, b""))
        self.assertEqual(tg.decode_frame(0x80, b""), {"points": 0, "x": None, "y": None})
        point = bytes([0, 0x05, 0x01, 0x02, 0x01, 0, 0, 0])
        self.assertEqual(tg.decode_frame(0x82, point), {"points": 2, "x": 0x0105, "y": 0x0102})
        with self.assertRaises(ValueError):
            tg.decode_frame(0x86, point)

    def test_the_register_numbers_are_the_same_in_c(self):
        macro = lambda name: int(re.search(rf"#define {name} (0x[0-9A-F]+|\d+)U", TOUCH_H).group(1), 0)  # noqa: E731
        self.assertEqual((macro("GT911_ADDR_PRIMARY"), macro("GT911_ADDR_ALTERNATE")), tg.ADDRESSES)
        self.assertEqual((macro("GT911_MAX_POINTS"), macro("GT911_POINT_BYTES"), macro("GT911_SIZE_MAX")), (tg.MAX_POINTS, tg.POINT_BYTES, tg.SIZE_MAX))
        self.assertEqual((macro("GT911_REG_STATUS"), macro("GT911_REG_POINT0"), macro("GT911_REG_ID")), (0x814E, 0x8150, 0x8140))


class File(unittest.TestCase):
    def test_the_file_is_accepted_and_the_picture_is_the_display(self):
        loaded = tg.load_config()
        display = display_plan.load_config()
        self.assertEqual((loaded["width"], loaded["height"]), (display["width"], display["height"]))
        self.assertEqual((loaded["raw_width"], loaded["raw_height"]), (1280, 720))
        self.assertEqual(loaded["address7"], 0x5D)

    def test_a_bad_file_is_refused(self):
        base = (ROOT / "config/lvgl.cfg").read_text(encoding="utf-8").replace("\r\n", "\n")
        for good, bad in (("address = 0x5D", "address = 0x40"), ("raw_width = 1280", "raw_width = 0"), ("swap_xy = 0", "swap_xy = 2"), ("poll_ms = 10", "poll_ms = 1"), ("controller = gt911", "controller = ft5x06")):
            with tempfile.TemporaryDirectory() as folder:
                path = Path(folder) / "lvgl.cfg"
                path.write_text(base.replace(good, bad, 1), encoding="utf-8")
                with self.assertRaises(ValueError, msg=bad):
                    tg.load_config(path)

    def test_the_switch_ships_off_and_nothing_calls_the_header(self):
        self.assertIn("HYDRA_TOUCH_GT911 0", TOUCH_H)
        users = [
            p
            for p in (ROOT / "src").rglob("*")
            if p.is_file() and p.suffix in (".c", ".h") and p.name != "touch_gt911.h" and '#include "touch_gt911.h"' in p.read_text(encoding="utf-8", errors="ignore")
        ]
        self.assertEqual(users, [])

    def test_the_header_is_in_both_runners_and_has_a_floor(self):
        host = (ROOT / "tools/host_tests.py").read_text(encoding="utf-8")
        build = (ROOT / "tools/build_firmware.py").read_text(encoding="utf-8")
        self.assertIn("tests/test_touch_gt911.c", host)
        self.assertIn("test_touch_gt911", build)
        floors = json.loads((ROOT / "tools/host_coverage_floor.json").read_text(encoding="utf-8"))["modules"]
        self.assertEqual(floors["src/mcu_stm32h757/Common/touch_gt911.h"], 100)


if __name__ == "__main__":
    unittest.main()
