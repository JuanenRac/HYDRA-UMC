# =============================================================================
# HYDRA-UMC - tests/test_ui_tick_plan.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The tick and the pace of the interface library: the Python twin (tools/ui_tick_plan.py) gives the same numbers as tests/test_ui_tick_plan.c, config/lvgl.cfg holds values the checks
accept, the limits are the same in C and Python, the switch ships off and nothing calls the header yet, and it is in both runners with a floor. Evidence: simulator."""
import json
import re
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import ui_tick_plan as ut  # noqa: E402

PLAN_H = (ROOT / "src/mcu_stm32h757/Common/ui_tick_plan.h").read_text(encoding="utf-8")
CFG = {"catchup_max_ms": 100, "handler_min_ms": 5, "handler_max_ms": 33}


class Twin(unittest.TestCase):
    def test_the_same_numbers_as_the_c_test(self):
        self.assertEqual(ut.tick(CFG, 1000, 1000), (0, False))
        self.assertEqual(ut.tick(CFG, 1000, 1005), (5, False))
        self.assertEqual(ut.tick(CFG, 1005, 1105), (100, False))
        self.assertEqual(ut.tick(CFG, 1105, 1206), (100, True))
        self.assertEqual(ut.tick(CFG, 0xFFFFFFF0, 4), (20, False))
        self.assertEqual([ut.sleep_ms(CFG, v) for v in (0, 4, 5, 20, 33, 34, ut.NO_TIMER_READY)], [5, 5, 5, 20, 33, 33, 33])

    def test_the_amounts_fed_add_up(self):
        for start in (0, 0xFFFFFF00):
            for step in (1, 7, 50):
                now, last, total = start, start, 0
                for _ in range(40):
                    now = (now + step) & ut.WORD
                    fed, clamped = ut.tick(CFG, last, now)
                    last = now
                    total += fed
                    self.assertFalse(clamped)
                self.assertEqual(total, 40 * step)

    def test_the_checks(self):
        for bad in (
            dict(CFG, handler_min_ms=0),
            dict(CFG, handler_max_ms=1001),
            dict(CFG, handler_min_ms=34),
            dict(CFG, catchup_max_ms=0),
            dict(CFG, catchup_max_ms=10001),
        ):
            with self.assertRaises(ValueError, msg=str(bad)):
                ut.check(bad)
        ut.check(dict(CFG, handler_min_ms=33, handler_max_ms=33))
        ut.check(dict(CFG, catchup_max_ms=10000))

    def test_the_limits_are_the_same_in_c(self):
        macro = lambda name: int(re.search(rf"#define {name} (\d+)UL", PLAN_H).group(1))  # noqa: E731
        self.assertEqual((macro("UTP_BOUND_MAX_MS"), macro("UTP_CATCHUP_MAX_MS")), (ut.BOUND_MAX_MS, ut.CATCHUP_MAX_MS))
        self.assertEqual(int(re.search(r"UTP_NO_TIMER_READY (0x[0-9A-F]+)UL", PLAN_H).group(1), 16), ut.NO_TIMER_READY)


class File(unittest.TestCase):
    def test_the_file_is_accepted_and_the_refresh_period_is_the_30_hz_of_the_budget(self):
        cfg = ut.load_config()
        self.assertEqual(cfg, CFG)
        self.assertLessEqual(cfg["handler_max_ms"], 1000 // 30)

    def test_a_bad_file_is_refused(self):
        base = (ROOT / "config/lvgl.cfg").read_text(encoding="utf-8").replace("\r\n", "\n")
        for good, bad in (("handler_min_ms = 5", "handler_min_ms = 0"), ("handler_max_ms = 33", "handler_max_ms = 4"), ("catchup_max_ms = 100", "catchup_max_ms = 0")):
            with tempfile.TemporaryDirectory() as folder:
                path = Path(folder) / "lvgl.cfg"
                path.write_text(base.replace(good, bad, 1), encoding="utf-8")
                with self.assertRaises(ValueError, msg=bad):
                    ut.load_config(path)

    def test_the_switch_ships_off_and_nothing_calls_the_header(self):
        self.assertIn("HYDRA_UI_TICK 0", PLAN_H)
        users = [
            p
            for p in (ROOT / "src").rglob("*")
            if p.is_file() and p.suffix in (".c", ".h") and p.name != "ui_tick_plan.h" and '#include "ui_tick_plan.h"' in p.read_text(encoding="utf-8", errors="ignore")
        ]
        self.assertEqual(users, [])

    def test_the_header_is_in_both_runners_and_has_a_floor(self):
        host = (ROOT / "tools/host_tests.py").read_text(encoding="utf-8")
        build = (ROOT / "tools/build_firmware.py").read_text(encoding="utf-8")
        self.assertIn("tests/test_ui_tick_plan.c", host)
        self.assertIn("test_ui_tick_plan", build)
        floors = json.loads((ROOT / "tools/host_coverage_floor.json").read_text(encoding="utf-8"))["modules"]
        self.assertEqual(floors["src/mcu_stm32h757/Common/ui_tick_plan.h"], 100)


if __name__ == "__main__":
    unittest.main()
