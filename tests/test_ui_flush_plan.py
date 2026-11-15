# =============================================================================
# HYDRA-UMC - tests/test_ui_flush_plan.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The flush plan of the interface library: the Python twin (tools/ui_flush_plan.py) gives the same numbers as tests/test_ui_flush_plan.c, config/ui_flush.cfg is a tenth of the
frame of config/display.cfg and fits the interface budget of docs/UI_STACK_H757.md, the limits are the same in C and Python, and the new header is in both runners with a floor.
Evidence: simulator."""
import json
import re
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import ui_flush_plan as uf  # noqa: E402

PLAN_H = (ROOT / "src/mcu_stm32h757/Common/ui_flush_plan.h").read_text(encoding="utf-8")


def panel(width=1280, height=720, fmt="rgb565"):
    return {"width": width, "height": height, "format": fmt}


class Plans(unittest.TestCase):
    """The same numbers as tests/test_ui_flush_plan.c."""

    def test_the_plans(self):
        cases = [
            (panel(), {"buffer_kb": 180, "buffers": 1}, (72, 72 * 2560, 10)),
            (panel(), {"buffer_kb": 2048, "buffers": 1}, (720, 720 * 2560, 1)),
            (panel(), {"buffer_kb": 4, "buffers": 2}, (1, 2560, 720)),
            (panel(fmt="argb8888"), {"buffer_kb": 180, "buffers": 1}, (36, 36 * 5120, 20)),
        ]
        for pnl, cfg, (lines, buffer_bytes, count) in cases:
            plan = uf.make_plan(pnl, cfg)
            self.assertEqual((plan["lines"], plan["buffer_bytes"], plan["strips"]), (lines, buffer_bytes, count), cfg)
            self.assertEqual(plan["total_bytes"], buffer_bytes * cfg["buffers"])
        with self.assertRaises(ValueError):
            uf.make_plan(panel(2048, 2048, "argb8888"), {"buffer_kb": 4, "buffers": 1})
        for bad in ({"buffer_kb": 3, "buffers": 1}, {"buffer_kb": 2049, "buffers": 1}, {"buffer_kb": 180, "buffers": 0}, {"buffer_kb": 180, "buffers": 3}):
            with self.assertRaises(ValueError, msg=str(bad)):
                uf.make_plan(panel(), bad)

    def test_the_strips_tile_the_picture_exactly(self):
        for height in (1, 7, 100, 480, 719, 720, 1000):
            for size in (4, 13, 64, 180, 1024):
                pnl = panel(320, height)
                try:
                    plan = uf.make_plan(pnl, {"buffer_kb": size, "buffers": 1})
                except ValueError:
                    continue
                bands = uf.strips(pnl, plan)
                self.assertEqual(sum(h for _, h in bands), height)
                self.assertEqual([y for y, _ in bands], [sum(h for _, h in bands[:i]) for i in range(len(bands))])

    def test_the_last_strip_can_be_short(self):
        plan = uf.make_plan(panel(height=719), {"buffer_kb": 180, "buffers": 1})
        self.assertEqual(uf.strips(panel(height=719), plan)[-1], (648, 71))

    def test_the_areas(self):
        self.assertEqual(uf.area_to_rect(panel(), 10, 20, 109, 59), (10, 20, 100, 40))
        self.assertEqual(uf.area_to_rect(panel(), 0, 0, 1279, 719), (0, 0, 1280, 720))
        self.assertEqual(uf.area_to_rect(panel(), 5, 5, 5, 5), (5, 5, 1, 1))
        for bad in ((-1, 0, 5, 5), (0, -1, 5, 5), (6, 0, 5, 5), (0, 6, 5, 5), (0, 0, 1280, 5), (0, 0, 5, 720)):
            with self.assertRaises(ValueError, msg=str(bad)):
                uf.area_to_rect(panel(), *bad)
        source = (ROOT / "tests/test_ui_flush_plan.c").read_text(encoding="utf-8")
        for needle in ("0xC0000000UL + 20u * 2560u + 10u * 2u", "100u << 16", "1280u - 100u"):
            self.assertIn(needle, source)


class ConfigAndSources(unittest.TestCase):
    def test_the_file_is_a_tenth_of_the_frame_and_fits_the_interface_budget(self):
        cfg = uf.load_config()
        self.assertEqual(cfg, {"buffer_kb": 180, "buffers": 1})
        picture = uf.display_plan.load_config()
        frame = picture["width"] * picture["height"] * uf.display_plan.BYTES_PER_PIXEL[picture["format"]]
        plan = uf.make_plan(picture, cfg)
        self.assertEqual(plan["buffer_bytes"] * 10, frame)
        budget = (ROOT / "docs" / "UI_STACK_H757.md").read_text(encoding="utf-8")
        self.assertIn("render buffer (1/10 of the frame)", budget)
        self.assertLess(plan["total_bytes"] // 1024, 298)

    def test_the_limits_are_the_same_in_c(self):
        macro = lambda name: int(re.search(rf"#define {name} (\d+)UL", PLAN_H).group(1))  # noqa: E731
        self.assertEqual((macro("UFP_BUFFER_KB_MIN"), macro("UFP_BUFFER_KB_MAX")), uf.BUFFER_KB)
        self.assertEqual(macro("UFP_BUFFERS_MAX"), uf.BUFFERS_MAX)

    def test_a_bad_file_is_refused(self):
        base = (ROOT / "config/ui_flush.cfg").read_text(encoding="utf-8").replace("\r\n", "\n")
        for good, bad in (("size_kb = 180", "size_kb = 3"), ("size_kb = 180", "size_kb = 4096"), ("count = 1", "count = 0"), ("count = 1", "count = 3")):
            text = base.replace(good, bad, 1)
            with tempfile.TemporaryDirectory() as folder:
                path = Path(folder) / "ui_flush.cfg"
                path.write_text(text, encoding="utf-8")
                with self.assertRaises(ValueError, msg=bad):
                    uf.load_config(path)

    def test_the_header_is_in_both_runners_and_has_a_floor(self):
        host = (ROOT / "tools/host_tests.py").read_text(encoding="utf-8")
        build = (ROOT / "tools/build_firmware.py").read_text(encoding="utf-8")
        self.assertIn("tests/test_ui_flush_plan.c", host)
        self.assertIn("test_ui_flush_plan", build)
        floors = json.loads((ROOT / "tools/host_coverage_floor.json").read_text(encoding="utf-8"))["modules"]
        self.assertEqual(floors["src/mcu_stm32h757/Common/ui_flush_plan.h"], 100)


if __name__ == "__main__":
    unittest.main()
