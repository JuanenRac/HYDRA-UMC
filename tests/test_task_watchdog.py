# =============================================================================
# HYDRA-UMC - tests/test_task_watchdog.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The supervision of liveness by task (tools/task_watchdog.py, its C twin src/mcu_stm32h757/CM4/task_watchdog.h): a hung mandatory task stops the refresh for good, a task that is not
mandatory only degrades, the worst time to the cut fits the time the safety file requires, and a simulation of a freeze measures it. Evidence: simulator."""
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import gen_taskwd_vectors  # noqa: E402
import task_watchdog as w  # noqa: E402

CONTROL, BUS, UI = 0, 1, 2


def started():
    dog = w.from_config(w.load_config())
    dog.start(0)
    return dog


class LogicTests(unittest.TestCase):
    def test_a_healthy_loop_is_refreshed_and_never_trips(self):
        dog = started()
        for t in range(0, 2000, 5):
            for task in (CONTROL, BUS, UI):
                dog.check_in(task, t)
            self.assertTrue(dog.update(t))
        self.assertEqual((dog.state, dog.trips), (w.ST_RUN, 0))

    def test_a_mandatory_task_that_hangs_stops_the_refresh_and_trips_once(self):
        dog = started()
        answers = []
        for t in range(0, 300, 5):
            dog.check_in(BUS, t)
            dog.check_in(UI, t)
            if t < 100:
                dog.check_in(CONTROL, t)
            answers.append(dog.update(t))
        self.assertFalse(answers[-1])
        self.assertEqual((dog.state, dog.trips, dog.late_mask), (w.ST_TRIPPED, 1, 1 << CONTROL))

    def test_the_trip_is_latched_even_if_the_hung_task_wakes_up(self):
        dog = started()
        self.assertFalse(dog.update(100))
        dog.check_in(CONTROL, 101)
        dog.check_in(BUS, 101)
        self.assertFalse(dog.update(102))
        self.assertEqual(dog.trips, 1)

    def test_a_task_that_is_not_mandatory_only_degrades_and_recovers(self):
        dog = started()
        for t in range(0, 700, 5):
            dog.check_in(CONTROL, t)
            dog.check_in(BUS, t)
            self.assertTrue(dog.update(t))
        self.assertEqual(dog.state, w.ST_DEGRADED)
        dog.check_in(UI, 700)
        self.assertTrue(dog.update(700))
        self.assertEqual((dog.state, dog.trips), (w.ST_RUN, 0))

    def test_a_number_that_is_no_task_is_never_a_sign_of_life(self):
        dog = started()
        for t in range(0, 100, 5):
            dog.check_in(99, t)
            dog.check_in(-1, t)
        self.assertFalse(dog.update(100))

    def test_the_freeze_is_cut_within_the_bound_at_every_phase(self):
        """The control task (period 10 ms) freezes at every millisecond of a stretch; the time from its last check-in to the cut (the supervisor stops refreshing, then the hardware
        watchdog bites after its timeout) never exceeds the bound."""
        cfg = w.load_config()
        worst = 0
        for freeze in range(1000, 1040):
            dog = w.from_config(cfg)
            dog.start(0)
            cut = None
            for t in range(0, 1400):
                if t < freeze and t % 10 == 0:
                    dog.check_in(CONTROL, t)
                if t % 5 == 0:
                    dog.check_in(BUS, t)
                    dog.check_in(UI, t)
                    if not dog.update(t) and cut is None:
                        cut = t
            self.assertIsNotNone(cut)
            last_check_in = max(t for t in range(0, freeze) if t % 10 == 0)
            worst = max(worst, cut + cfg["iwdg_timeout_ms"] - last_check_in)
        self.assertLessEqual(worst, cfg["bound_ms"])
        self.assertLessEqual(cfg["bound_ms"], cfg["required_cut_ms"])


class ConfigTests(unittest.TestCase):
    def test_the_bound_is_the_longest_mandatory_deadline_plus_a_period_plus_the_timeout(self):
        cfg = w.load_config()
        self.assertEqual(cfg["bound_ms"], 50 + cfg["update_period_ms"] + cfg["iwdg_timeout_ms"])

    def test_the_configuration_refuses_what_cannot_work(self):
        base = (ROOT / "config" / "task_watchdog.cfg").read_text(encoding="utf-8")
        cases = {
            "too_slow": base.replace("required_cut_ms = 150", "required_cut_ms = 100"),
            "no_mandatory": base.replace("mandatory = 1", "mandatory = 0"),
            "zero_deadline": base.replace("deadline_ms = 20", "deadline_ms = 0"),
            "huge_timeout": base.replace("iwdg_timeout_ms = 50", "iwdg_timeout_ms = 9000"),
        }
        with tempfile.TemporaryDirectory() as folder:
            for name, text in cases.items():
                path = Path(folder) / f"{name}.cfg"
                path.write_text(text, encoding="utf-8")
                with self.assertRaises(ValueError, msg=name):
                    w.load_config(path)

    def test_the_vectors_the_c_test_replays_are_what_the_reference_says(self):
        self.assertEqual(gen_taskwd_vectors.main(["--check"]), 0)

    def test_the_doc_names_the_bound_and_the_configuration(self):
        doc = (ROOT / "docs" / "TASK_WATCHDOG.md").read_text(encoding="utf-8")
        self.assertIn("config/task_watchdog.cfg", doc)
        self.assertIn(f"{w.load_config()['bound_ms']} ms", doc)


if __name__ == "__main__":
    unittest.main()
