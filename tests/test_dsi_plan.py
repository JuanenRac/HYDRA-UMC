# =============================================================================
# HYDRA-UMC - tests/test_dsi_plan.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The DSI link and the panel start-up: the Python twin (tools/dsi_plan.py) gives the same plans as tests/test_dsi_plan.c, the C table of the panel is the one of config/panel.cfg,
the limits are the same in C and Python, the picture is the one of config/display.cfg, and the new headers are in both runners with floors. Evidence: simulator."""
import json
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import dsi_plan as dp  # noqa: E402

PLAN_H = (ROOT / "src/mcu_stm32h757/Common/dsi_plan.h").read_text(encoding="utf-8")
START_H = (ROOT / "src/mcu_stm32h757/Common/panel_start.h").read_text(encoding="utf-8")


def macro(text, name):
    return int(re.search(rf"#define {name} (\d+)", text).group(1))


def link(**changes):
    base = dp.load_config()["link"]
    base.update(changes)
    return base


class Plans(unittest.TestCase):
    """The same numbers as tests/test_dsi_plan.c."""

    def test_the_plans(self):
        cases = [
            ({}, (1, 24, 1, 1_200_000, 600_000, 75_000, 63_685, 5, 71, 1653, 1_171_804)),
            ({"lane_kbps": 900_000}, (1, 36, 1, 1_800_000, 900_000, 112_500, 63_685, 7, 106, 2480, 1_171_804)),
            ({"hse_hz": 24_000_000}, (1, 25, 1, 1_200_000, 600_000, 75_000, 63_685, 5, 71, 1653, 1_171_804)),
            ({"lane_kbps": 613_000}, (2, 49, 1, 1_225_000, 612_500, 76_562, 63_685, 5, 72, 1688, 1_171_804)),
            ({"lanes": 1, "lane_kbps": 1_000_000, "width": 800, "height": 480, "hsync": 4, "hbp": 20, "hfp": 20, "vsync": 2, "vbp": 10, "vfp": 10, "fps": 30}, (1, 40, 1, 2_000_000, 1_000_000, 125_000, 12_711, 39, 197, 8300, 233_882)),
        ]
        keys = ("idf", "ndiv", "odf", "vco_khz", "lane_kbps", "byte_clock_khz", "pixel_khz", "hsa", "hbp", "hline", "need_kbps")
        for changes, expected in cases:
            plan = dp.make_plan(link(**changes))
            self.assertEqual(tuple(plan[k] for k in keys), expected, changes)
        small = {"lane_kbps": 62_500, "width": 64, "height": 64, "hsync": 1, "hbp": 1, "hfp": 1, "vsync": 1, "vbp": 1, "vfp": 1, "fps": 10}
        plan = dp.make_plan(link(**small))
        self.assertEqual((plan["idf"], plan["ndiv"], plan["odf"], plan["hline"]), (1, 20, 8, 11631))
        source = (ROOT / "tests/test_dsi_plan.c").read_text(encoding="utf-8")
        for needle in ("1171804UL", "2480u", "1688u", "8300u", "11631u", "233882UL"):
            self.assertIn(needle, source)

    def test_refusals(self):
        bad = [
            {"lane_kbps": 613_333, "tolerance_permille": 0}, {"bpp": 24}, {"hse_hz": 3_999_999}, {"hse_hz": 50_000_001}, {"lanes": 0}, {"lanes": 3},
            {"lane_kbps": 62_499}, {"lane_kbps": 1_000_001}, {"tolerance_permille": 51}, {"bpp": 8}, {"fps": 9}, {"fps": 121}, {"width": 0}, {"width": 2049},
            {"height": 0}, {"height": 2049}, {"hsync": 0}, {"hbp": 1025}, {"hfp": 0}, {"vsync": 1025}, {"vbp": 0}, {"vfp": 1025},
            {"lane_kbps": 62_500, "width": 64, "height": 64, "hsync": 1024, "hbp": 1, "hfp": 1, "vsync": 1, "vbp": 1, "vfp": 1, "fps": 10},
        ]
        for changes in bad:
            with self.assertRaises(ValueError, msg=str(changes)):
                dp.make_plan(link(**changes))

    def test_every_swept_plan_keeps_the_limits(self):
        made = 0
        for rate in range(62_500, 1_000_001, 12_500):
            try:
                plan = dp.make_plan(link(lane_kbps=rate, tolerance_permille=50, lanes=2, width=320, height=240, fps=30))
            except ValueError:
                continue
            made += 1
            self.assertTrue(dp.VCO_KHZ[0] <= plan["vco_khz"] <= dp.VCO_KHZ[1])
            self.assertEqual(plan["lane_kbps"], plan["vco_khz"] // (2 * plan["odf"]))
            self.assertLessEqual(abs(plan["lane_kbps"] - rate) * 1000, rate * 50)
        self.assertGreater(made, 40)


class Config(unittest.TestCase):
    def test_the_file_is_valid_and_has_the_picture_of_the_display_file(self):
        data = dp.load_config()
        self.assertEqual(data["reset"], {"low_ms": 10, "high_ms": 120})
        self.assertEqual(data["steps"], [(0x11, [], 120), (0x29, [], 20)])
        picture = dp.display_plan.load_config()
        for key in ("width", "height", "hsync", "hbp", "hfp", "vsync", "vbp", "vfp"):
            self.assertEqual(data["link"][key], picture[key])

    def test_the_c_table_is_the_one_of_the_file(self):
        result = subprocess.run([sys.executable, str(ROOT / "tools" / "dsi_plan.py"), "--check"], capture_output=True, text=True, cwd=ROOT, check=False)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("PANEL_INIT_TABLE=PASS", result.stdout)

    def test_the_table_text_has_the_parameters(self):
        steps = [(0xFF, [0x98, 0x81, 0x03], 0), (0x11, [], 120), (0x29, [], 20)]
        text = dp.render_table({"reset": {"low_ms": 5, "high_ms": 50}, "steps": steps})
        self.assertIn("static const uint8_t g_panel_params_0[] = {0x98, 0x81, 0x03};", text)
        self.assertIn("{0xFF, 3, g_panel_params_0, 0},", text)
        self.assertIn("{0x11, 0, NULL, 120},", text)
        self.assertIn("#define PANEL_INIT_STEPS 3u", text)
        self.assertIn("#define PANEL_INIT_RESET_LOW_MS 5u", text)

    def test_bad_steps_are_refused(self):
        good = [(0x11, [], 120), (0x29, [], 0)]
        dp.check_steps(good)
        for steps in ([], [(0x29, [], 0)], [(0x11, [], 120)], [(0x29, [], 0), (0x11, [], 120)], [(0x11, [], 119), (0x29, [], 0)], [(0x11, [], 120), (0x29, [], 1001)],
                      [(0xFF, [0] * 65, 0), (0x11, [], 120), (0x29, [], 0)], [(0x11, [], 120)] * 129):
            with self.assertRaises(ValueError, msg=str(steps)):
                dp.check_steps(steps)

    def test_a_bad_file_is_refused(self):
        base = (ROOT / "config/panel.cfg").read_text(encoding="utf-8").replace("\r\n", "\n")
        for good, bad in (("step01 = 11 ; 120", "step01 = 11"), ("step01 = 11 ; 120", "step01 = ZZ ; 120"), ("step01 = 11 ; 120", "step01 = 11 ; 100"), ("low_ms = 10", "low_ms = 0"), ("high_ms = 120", "high_ms = 1001")):
            text = base.replace(good, bad, 1)
            self.assertNotEqual(text, base, bad)
            with tempfile.TemporaryDirectory() as folder:
                path = Path(folder) / "panel.cfg"
                path.write_text(text, encoding="utf-8")
                with self.assertRaises(ValueError, msg=bad):
                    dp.load_config(path)


class Sources(unittest.TestCase):
    def test_the_limits_are_the_same_in_c(self):
        self.assertEqual((macro(PLAN_H, "DSIP_HSE_MIN_HZ"), macro(PLAN_H, "DSIP_HSE_MAX_HZ")), dp.HSE_HZ)
        self.assertEqual((macro(PLAN_H, "DSIP_PLL_IN_MIN_KHZ"), macro(PLAN_H, "DSIP_PLL_IN_MAX_KHZ")), dp.PLL_IN_KHZ)
        self.assertEqual((macro(PLAN_H, "DSIP_VCO_MIN_KHZ"), macro(PLAN_H, "DSIP_VCO_MAX_KHZ")), dp.VCO_KHZ)
        self.assertEqual((macro(PLAN_H, "DSIP_NDIV_MIN"), macro(PLAN_H, "DSIP_NDIV_MAX"), macro(PLAN_H, "DSIP_IDF_MAX")), (dp.NDIV[0], dp.NDIV[1], dp.IDF_MAX))
        self.assertEqual((macro(PLAN_H, "DSIP_LANE_MIN_KBPS"), macro(PLAN_H, "DSIP_LANE_MAX_KBPS")), dp.LANE_KBPS)
        self.assertEqual((macro(PLAN_H, "DSIP_TOLERANCE_MAX_PERMILLE"), macro(PLAN_H, "DSIP_LANES_MAX")), (dp.TOLERANCE_MAX, dp.LANES_MAX))
        self.assertEqual((macro(PLAN_H, "DSIP_FPS_MIN"), macro(PLAN_H, "DSIP_FPS_MAX")), dp.FPS)
        self.assertEqual((macro(PLAN_H, "DSIP_HSA_HBP_MAX"), macro(PLAN_H, "DSIP_HLINE_MAX"), macro(PLAN_H, "DSIP_PACKET_OVERHEAD_PERMILLE")), (dp.HSA_HBP_MAX, dp.HLINE_MAX, dp.OVERHEAD_PERMILLE))
        self.assertIn("{1u, 2u, 4u, 8u}", PLAN_H)

    def test_the_panel_limits_are_the_same_in_c(self):
        self.assertEqual((macro(START_H, "PANEL_STEPS_MAX"), macro(START_H, "PANEL_PARAMS_MAX"), macro(START_H, "PANEL_DELAY_MAX_MS")), (dp.STEPS_MAX, dp.PARAMS_MAX, dp.DELAY_MAX_MS))
        self.assertEqual((macro(START_H, "PANEL_RESET_MIN_MS"), macro(START_H, "PANEL_RESET_MAX_MS")), dp.RESET_MS)
        self.assertEqual(macro(START_H, "PANEL_SLEEP_OUT_MIN_DELAY_MS"), dp.SLEEP_OUT_MIN_DELAY_MS)
        self.assertIn("#define PANEL_SLEEP_OUT 0x11u", START_H)
        self.assertIn("#define PANEL_DISPLAY_ON 0x29u", START_H)

    def test_the_new_headers_are_in_both_runners_and_have_floors(self):
        host = (ROOT / "tools/host_tests.py").read_text(encoding="utf-8")
        build = (ROOT / "tools/build_firmware.py").read_text(encoding="utf-8")
        for test in ("tests/test_dsi_plan.c", "tests/test_panel_start.c"):
            self.assertIn(test, host)
        for token in ("test_dsi_plan", "test_panel_start"):
            self.assertIn(token, build)
        floors = json.loads((ROOT / "tools/host_coverage_floor.json").read_text(encoding="utf-8"))["modules"]
        for module in ("Common/dsi_plan.h", "Common/panel_start.h"):
            self.assertEqual(floors["src/mcu_stm32h757/" + module], 100)


if __name__ == "__main__":
    unittest.main()
