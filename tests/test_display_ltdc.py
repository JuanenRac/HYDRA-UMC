# =============================================================================
# HYDRA-UMC - tests/test_display_ltdc.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""Source guards of the LTDC and DMA2D driver of the H757 board and its Python twin: the plan of the starting panel is the one the C test checks word by word, the settings come from
config/display.cfg with the same limits in Python and C, the switch ships off and holds the whole unit, and the driver is built and tested in both runners. Evidence: simulator
(the register double runs in tests/test_display_ltdc.c)."""
import json
import re
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import display_plan as dp  # noqa: E402

DRIVER = (ROOT / "src/mcu_stm32h757/CM7/DisplayLtdc.c").read_text(encoding="utf-8")
HEADER = (ROOT / "src/mcu_stm32h757/CM7/DisplayLtdc.h").read_text(encoding="utf-8")
PLAN_H = (ROOT / "src/mcu_stm32h757/Common/display_plan.h").read_text(encoding="utf-8")
C_TEST = (ROOT / "tests/test_display_ltdc.c").read_text(encoding="utf-8")

FB = 0xC0000000
WINDOW = 0x08000000


class DriverGuards(unittest.TestCase):
    def test_the_switch_ships_off_and_the_whole_unit_is_inside_it(self):
        self.assertIn("#define HYDRA_DISPLAY_LTDC 0", HEADER)
        self.assertTrue(DRIVER.rstrip().endswith("#endif /* HYDRA_DISPLAY_LTDC */"))
        self.assertEqual(DRIVER.count("#if HYDRA_DISPLAY_LTDC"), 1)

    def test_the_controller_is_enabled_last(self):
        body = DRIVER[DRIVER.index("int DisplayLtdc_Init"): DRIVER.index("static DisplayLtdcStatus DisplayLtdc_Run")]
        self.assertLess(body.index("LTDC->GCR &= ~LTDC_GCR_LTDCEN"), body.index("LTDC->SSCR"))
        self.assertLess(body.index("LTDC_Layer1->CR |= LTDC_LxCR_LEN"), body.index("LTDC->SRCR"))
        self.assertLess(body.index("LTDC->SRCR"), body.index("LTDC->GCR |= LTDC_GCR_LTDCEN"))

    def test_the_driver_never_waits_without_a_limit(self):
        self.assertIn("polls < g_cfg.poll_limit", DRIVER)
        self.assertNotIn("while (", DRIVER)

    def test_the_driver_is_in_both_runners_and_has_floors(self):
        self.assertIn("tests/test_display_ltdc.c", (ROOT / "tools/host_tests.py").read_text(encoding="utf-8"))
        build = (ROOT / "tools/build_firmware.py").read_text(encoding="utf-8")
        self.assertIn("-DHYDRA_DISPLAY_LTDC=1", build)
        self.assertIn("DisplayLtdc.c", build)
        floors = json.loads((ROOT / "tools/host_coverage_floor.json").read_text(encoding="utf-8"))
        self.assertEqual(floors["modules"]["src/mcu_stm32h757/CM7/DisplayLtdc.c"], 100)
        self.assertEqual(floors["modules"]["src/mcu_stm32h757/Common/display_plan.h"], 100)


class ConfigTests(unittest.TestCase):
    def test_the_file_is_valid(self):
        cfg = dp.load_config()
        self.assertEqual((cfg["width"], cfg["height"], cfg["format"]), (1280, 720, "rgb565"))
        self.assertEqual(cfg["poll_limit"], 100000)

    def test_values_out_of_range_are_refused(self):
        base = (ROOT / "config/display.cfg").read_text(encoding="utf-8")
        bad_values = (
            ("width = 1280", "width = 0"), ("width = 1280", "width = 2049"), ("height = 720", "height = 0"), ("height = 720", "height = 2049"),
            ("hsync = 4", "hsync = 0"), ("hbp = 60", "hbp = 1025"), ("hfp = 60", "hfp = 0"), ("vsync = 4", "vsync = 1025"), ("vbp = 16", "vbp = 0"), ("vfp = 16", "vfp = 1025"),
            ("format = rgb565", "format = rgb888"), ("poll_limit = 100000", "poll_limit = 0"), ("poll_limit = 100000", "poll_limit = 1000001"),
        )
        for field, bad in bad_values:
            self.assertIn(field, base)
            with tempfile.TemporaryDirectory() as folder:
                path = Path(folder) / "display.cfg"
                path.write_text(base.replace(field, bad), encoding="utf-8")
                with self.assertRaises(ValueError, msg=bad):
                    dp.load_config(path)

    def test_the_c_code_refuses_the_same_limits(self):
        self.assertIn("#define DISPLAY_MAX_WIDTH %du" % dp.MAX_SIZE, PLAN_H)
        self.assertIn("#define DISPLAY_MAX_HEIGHT %du" % dp.MAX_SIZE, PLAN_H)
        self.assertIn("#define DISPLAY_MAX_SYNC %du" % dp.MAX_SYNC, PLAN_H)
        self.assertIn("#define DISPLAY_POLL_LIMIT_MAX %du" % dp.MAX_POLL, DRIVER)
        self.assertIn("cfg->poll_limit < 1u", DRIVER)


class PlanTwin(unittest.TestCase):
    def test_the_starting_panel_gives_the_words_the_c_test_checks(self):
        plan = dp.plan(dp.load_config(), FB, WINDOW)
        expected = {"sscr": 0x00030003, "bpcr": 0x003F0013, "awcr": 0x053F02E3, "twcr": 0x057B02F3, "whpcr": 0x053F0040, "wvpcr": 0x02E30014, "cfblr": 0x0A000A03}
        for name, value in expected.items():
            self.assertEqual(plan[name], value, name)
            self.assertIn("0x%08XUL" % value, C_TEST, name)
        self.assertEqual((plan["pitch_bytes"], plan["frame_bytes"]), (2560, 1843200))

    def test_argb8888_doubles_the_frame(self):
        cfg = dict(dp.load_config(), format="argb8888")
        plan = dp.plan(cfg, FB, WINDOW)
        self.assertEqual((plan["pitch_bytes"], plan["frame_bytes"], plan["pfcr"]), (5120, 3686400, 0))

    def test_a_frame_that_does_not_fit_is_refused(self):
        cfg = dp.load_config()
        with self.assertRaises(ValueError):
            dp.plan(cfg, FB, 1843199)
        with self.assertRaises(ValueError):
            dp.plan(cfg, FB + 2, WINDOW)
        with self.assertRaises(ValueError):
            dp.plan(cfg, 0xFFF00000, WINDOW)

    def test_the_c_plan_uses_the_same_formulas(self):
        for fragment in ("(panel->hsync - 1u) << 16", "(h_acc - 1u) << 16", "(h_acc + panel->width - 1u) << 16", "(h_acc + panel->width + panel->hfp - 1u) << 16", "pitch + 3u"):
            self.assertIn(fragment, PLAN_H)
        self.assertTrue(re.search(r"DISPLAY_LTDC_PF_RGB565 2u", PLAN_H))


if __name__ == "__main__":
    unittest.main()
