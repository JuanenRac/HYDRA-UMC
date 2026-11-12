# =============================================================================
# HYDRA-UMC - tests/test_ui_sched_sim.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The model of the task priorities of the interface (tools/ui_sched_sim.py): the time from a touch of STOP to the stop frame does not depend on the drawing or on the PIN check when the
priorities are those of the design, and the model shows the argument fails when the interface task is above the stack task. The numbers are estimates of the design; the real latency is
measured on the board. Evidence: simulator (a model, not FreeRTOS)."""
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import ui_sched_sim as sim  # noqa: E402


class SchedTests(unittest.TestCase):
    def setUp(self):
        self.s = sim.load()

    def test_the_latency_is_bounded_by_the_period_of_the_stack_task_whatever_the_drawing_does(self):
        bound = self.s["touch_isr_us"] + self.s["stack_task_period_us"] + self.s["stack_task_wcet_us"] + 2 * sim.STEP_US
        for ui_ms in (1, 25, 100, 300, 800):
            self.assertLessEqual(sim.worst_latency_us(self.s, ui_wcet_ms=ui_ms), bound, ui_ms)
        self.assertLessEqual(sim.worst_latency_us(self.s, with_pin_check=True), bound)

    def test_it_does_not_grow_with_the_drawing(self):
        light = sim.worst_latency_us(self.s, ui_wcet_ms=1, with_pin_check=False)
        heavy = sim.worst_latency_us(self.s, ui_wcet_ms=500)
        self.assertLess(abs(heavy - light), 2 * self.s["stack_task_period_us"])
        self.assertLessEqual(heavy, self.s["stop_max_latency_ms"] * 1000)

    def test_with_the_interface_above_the_stack_task_the_stop_is_starved(self):
        wrong = sim.worst_latency_us(self.s, ui_priority=4, ui_wcet_ms=300)
        self.assertGreater(wrong, 100 * self.s["stop_max_latency_ms"] * 1000)  # the argument needs the priorities of the design

    def test_a_lower_priority_stack_task_than_the_link_still_meets_the_limit_only_while_it_runs_often(self):
        slower = dict(self.s, stack_task_period_us=20_000)
        self.assertGreater(sim.worst_latency_us(slower), self.s["stop_max_latency_ms"] * 1000)

    def test_the_estimate_and_the_command_line_pass(self):
        n = sim.estimate()
        self.assertTrue(n["fits"])
        self.assertGreater(n["ui_above_stack_us"], n["limit_us"])
        self.assertEqual(sim.main(), 0)

    def test_the_settings_are_in_the_configuration_file(self):
        self.assertEqual(set(self.s), set(sim.FIELDS))
        self.assertGreaterEqual(self.s["stop_max_latency_ms"], 1)


if __name__ == "__main__":
    unittest.main()
