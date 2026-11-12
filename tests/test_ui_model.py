# =============================================================================
# HYDRA-UMC - tests/test_ui_model.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The view-model of the interface of the STM32H757 (tools/ui_model.py): the failure scenarios say what the panel does when the Jetson hangs, the SPI link is cut, the stack
bus fills with errors, STOP is touched while moving and listen-only is on; and a fuzz with a fixed seed holds the rules that never change. Evidence: simulator (the model
runs on the PC); nothing ran on a display or a board."""
import json
import random
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import ui_model as m  # noqa: E402

SCENARIOS = {s["name"]: s for s in json.loads((ROOT / "src" / "ui_h757" / "spec" / "scenarios.json").read_text(encoding="utf-8"))["scenarios"]}


def fields(line: str) -> dict[str, str]:
    return dict(part.split("=", 1) for part in line.split(" "))


def run(name: str) -> list[dict[str, str]]:
    return [fields(line) for line in m.run_scenario(SCENARIOS[name], m.load_config())]


def at(rows: list[dict[str, str]], t: int, event: str | None = None) -> dict[str, str]:
    found = [r for r in rows if r["t"] == str(t) and (event is None or r["ev"] == event)]
    assert found, (t, event)
    return found[0]


def effects(row: dict[str, str]) -> list[str]:
    return [] if row["fx"] == "-" else row["fx"].split(",")


class ConfigTests(unittest.TestCase):
    def test_the_file_is_valid_and_ships_the_conservative_defaults(self):
        cfg = m.load_config()
        self.assertEqual(cfg["degraded_allow_release"], 0)
        self.assertEqual(cfg["degraded_allow_motion"], 0)
        self.assertEqual(set(cfg), set(m.CONFIG_FIELDS))

    def test_a_value_out_of_range_or_an_unknown_key_is_refused(self):
        base = (ROOT / "config" / "ui.cfg").read_text(encoding="utf-8")
        scratch = Path(tempfile.mkdtemp(prefix="ui_cfg_test_"))
        self.addCleanup(shutil.rmtree, scratch, True)
        for name, text in (("range", base.replace("jetson_timeout_ms = 1500", "jetson_timeout_ms = 5")), ("unknown", base.replace("degraded_allow_motion = 0", "degraded_allow_motion = 0\nsurprise = 1")),
                           ("word", base.replace("jog_arm_ms = 10000", "jog_arm_ms = soon")), ("section", "[other]\na = 1\n")):
            path = scratch / f"{name}.cfg"
            path.write_text(text, encoding="utf-8")
            with self.assertRaises(m.ConfigError, msg=name):
                m.load_config(path)


class ScenarioTests(unittest.TestCase):
    def test_every_scenario_runs_and_the_trace_is_the_same_twice(self):
        for name, scenario in SCENARIOS.items():
            first = m.run_scenario(scenario, m.load_config())
            self.assertEqual(first, m.run_scenario(scenario, m.load_config()), name)
            self.assertEqual(len(first), len(scenario["events"]), name)

    def test_without_a_jetson_the_panel_starts_then_degrades_and_stop_always_works(self):
        rows = run("boot_without_jetson_goes_degraded")
        self.assertEqual(at(rows, 1000)["mode"], "boot")
        self.assertIn("refused:jog_arm:starting", effects(at(rows, 1000)))
        self.assertIn("stop", effects(at(rows, 2000)))
        self.assertEqual(at(rows, 5000)["link"], "silent")
        self.assertIn("alarm+:jetson_lost", effects(at(rows, 5000)))
        self.assertIn("refused:release:degraded", effects(at(rows, 6000)))  # the stop is not released without the Jetson by default

    def test_the_jetson_hang_disarms_the_jog_refuses_motion_and_keeps_stop(self):
        rows = run("jetson_hangs_then_recovers")
        self.assertEqual(at(rows, 1900)["mode"], "normal")  # inside the timeout
        late = at(rows, 2000, "DOWN")
        self.assertEqual((late["mode"], late["link"], late["arm"]), ("degraded", "silent", "0"))
        self.assertIn("refused:jog_y_plus:degraded", effects(late))
        self.assertNotIn("jog:y_plus", effects(late))
        self.assertIn("stop", effects(at(rows, 2200)))
        self.assertEqual(at(rows, 7100)["mode"], "normal")  # the heartbeat is back
        self.assertIn("jog_armed", effects(at(rows, 7200)))

    def test_a_cut_spi_link_is_told_apart_from_a_silent_jetson(self):
        rows = run("spi_link_is_cut")
        self.assertEqual((at(rows, 1700)["link"], at(rows, 1700)["ban"]), ("silent", "degraded"))
        self.assertEqual((at(rows, 1800)["link"], at(rows, 1800)["ban"]), ("spi_fault", "spi_fault"))
        self.assertEqual(at(rows, 2700)["mode"], "normal")

    def test_bus_errors_warn_then_the_bus_off_blocks_commands_and_stop_still_goes_out(self):
        rows = run("can_errors_rise_to_bus_off")
        self.assertEqual(at(rows, 200)["bus"], "warn")
        self.assertIn("jog_armed", effects(at(rows, 300)))  # a warning alone does not block
        off = at(rows, 400)
        self.assertEqual((off["bus"], off["mode"], off["arm"]), ("off", "alarm", "0"))
        self.assertIn("refused:jog_arm:bus_fault", effects(at(rows, 500)))
        self.assertIn("stop", effects(at(rows, 600)))
        self.assertEqual(at(rows, 1200)["ban"], "stop_unconfirmed")
        self.assertEqual(at(rows, 1400)["ban"], "stopped")  # the brain confirmed

    def test_stop_while_moving_raises_an_alarm_when_the_axis_keeps_moving(self):
        rows = run("stop_while_moving")
        self.assertEqual(at(rows, 400)["mode"], "stopped")
        self.assertEqual(at(rows, 400)["arm"], "0")
        self.assertEqual(at(rows, 1000)["al"], "000")
        self.assertIn("alarm+:motion_after_stop", effects(at(rows, 1500)))
        self.assertIn("alarm-:motion_after_stop", effects(at(rows, 1600)))

    def test_an_unconfirmed_stop_never_reads_as_good(self):
        rows = run("stop_is_never_confirmed")
        self.assertEqual(at(rows, 700)["ban"], "stop_unconfirmed")
        self.assertEqual(at(rows, 700)["mode"], "stopped")
        self.assertEqual(at(rows, 2100)["ban"], "stopped")

    def test_listen_only_refuses_every_command_but_stop_and_leaving_needs_a_long_press(self):
        rows = run("listen_only_refuses_everything_but_stop")
        self.assertIn("refused:jog_arm:listen_only", effects(at(rows, 300)))
        self.assertIn("stop", effects(at(rows, 400)))
        self.assertIn("refused:release:listen_only", effects(at(rows, 500)))
        self.assertIn("hold_cancel", effects(at(rows, 900)))
        self.assertEqual(at(rows, 1000)["lo"], "1")
        self.assertIn("listen:0", effects(at(rows, 2700)))
        self.assertEqual(at(rows, 2700)["lo"], "0")
        self.assertIn("release", effects(at(rows, 4400)))

    def test_a_short_touch_on_release_does_nothing(self):
        rows = run("release_needs_a_long_press")
        self.assertNotIn("release", sum((effects(r) for r in rows[:5]), []))
        self.assertIn("release", effects(at(rows, 2300)))

    def test_the_two_degraded_switches_are_off_by_default_and_work_when_on(self):
        self.assertIn("refused:release:degraded", effects(at(run("degraded_release_refused_by_default"), 2600)))
        on = run("degraded_release_switch_on")
        self.assertIn("release", effects(at(on, 4200)))
        self.assertIn("refused:jog_arm:degraded", effects(at(on, 4500)))  # releasing does not allow motion
        motion = run("degraded_motion_switch_on")
        self.assertIn("jog:x_plus", effects(at(motion, 2200)))

    def test_safety_verdicts_of_the_jetson_become_alarms_and_block_release_and_motion(self):
        rows = run("zone_verdicts_of_the_jetson")
        self.assertEqual(at(rows, 100)["mode"], "normal")  # a warning alone
        self.assertEqual(at(rows, 200)["mode"], "alarm")
        self.assertIn("refused:jog_arm:alarm", effects(at(rows, 300)))
        self.assertEqual((at(rows, 400)["al"], at(rows, 400)["un"]), ("080", "000"))  # acknowledged, not cleared
        self.assertIn("refused:release:alarm", effects(at(rows, 900)))
        self.assertIn("release", effects(at(rows, 2700)))

    def test_the_armed_jog_expires(self):
        rows = run("jog_arming_expires")
        self.assertIn("jog:x_plus", effects(at(rows, 5100)))
        self.assertIn("refused:jog_x_plus:not_armed", effects(at(rows, 10300)))

    def test_everything_failing_at_once_still_sends_stop_and_says_not_to_rely_on_it(self):
        rows = run("worst_case_everything_fails")
        self.assertIn("stop", effects(at(rows, 2000, "DOWN")))
        self.assertEqual(at(rows, 2600)["ban"], "stop_unconfirmed")
        self.assertNotIn("release", sum((effects(r) for r in rows), []))

    def test_hostile_input_changes_nothing_it_should_not(self):
        rows = run("settings_and_navigation")
        self.assertEqual(at(rows, 300, "DOWN")["fx"], "lang:2")
        self.assertEqual(at(rows, 400)["scr"], "7")  # a screen out of range is ignored
        self.assertEqual(at(rows, 500)["fx"], "-")  # an unknown action is ignored
        self.assertEqual(at(rows, 600)["al"], "000")  # a level out of range reads as unknown: no zone alarm
        self.assertEqual(rows[-1]["scr"], "6")

    def test_the_operator_pin_locks_every_command_but_stop_and_is_off_by_default(self):
        self.assertEqual(m.load_config()["operator_pin_enabled"], 0)
        rows = run("pin_locks_commands_but_never_stop")
        self.assertEqual((at(rows, 100)["lk"], at(rows, 100)["ban"]), ("1", "locked"))
        self.assertIn("refused:jog_arm:locked", effects(at(rows, 100)))
        self.assertIn("stop", effects(at(rows, 200)))  # STOP never asks for the PIN
        self.assertIn("refused:release:locked", effects(at(rows, 300)))
        self.assertIn("pin_wrong", effects(at(rows, 500)))
        self.assertIn("unlocked", effects(at(rows, 600)))
        self.assertIn("release", effects(at(rows, 2300)))  # the same hold releases once unlocked
        self.assertIn("jog_armed", effects(at(rows, 2600)))
        off = run("pin_is_off_by_default")
        self.assertEqual(effects(at(off, 100)), [])  # the PIN events change nothing
        self.assertEqual(at(off, 300)["lk"], "0")
        self.assertIn("jog_armed", effects(at(off, 300)))

    def test_wrong_pins_lock_the_pad_out_and_the_panel_locks_again_when_idle(self):
        rows = run("pin_lockout_and_idle_relock")
        self.assertIn("pin_lockout", effects(at(rows, 300)))
        self.assertEqual(at(rows, 400)["lk"], "1")  # a right PIN is ignored during the lock-out
        self.assertIn("unlocked", effects(at(rows, 3400)))
        late = at(rows, 8500)
        self.assertEqual((late["lk"], late["arm"]), ("1", "0"))
        self.assertIn("locked", effects(late))
        self.assertIn("refused:jog_x_plus:locked", effects(at(rows, 8600)))

    def test_turning_listen_only_on_needs_no_pin_but_leaving_it_does(self):
        rows = run("leaving_listen_only_needs_the_pin")
        self.assertIn("listen:1", effects(at(rows, 200)))
        self.assertIn("refused:toggle_listen_only:locked", effects(at(rows, 5500)))
        self.assertIn("listen:0", effects(at(rows, 7300)))

    def test_time_never_goes_back(self):
        state = m.State(m.load_config())
        state.event(1000, "TICK")
        state.event(500, "TICK")
        self.assertEqual(state.now, 1000)

    def test_an_unknown_event_is_an_error_and_a_bad_line_does_not_parse(self):
        with self.assertRaises(ValueError):
            m.State(m.load_config()).event(0, "WHAT")
        with self.assertRaises(ValueError):
            m.parse_event("x TICK")


class FuzzTests(unittest.TestCase):
    """The rules that never change, over a long random run with a fixed seed (the C twin runs the same kind of fuzz)."""

    def run_fuzz(self, allow_release: int, allow_motion: int, pin: int = 0) -> None:
        cfg = m.load_config()
        cfg["degraded_allow_release"], cfg["degraded_allow_motion"] = allow_release, allow_motion
        cfg["operator_pin_enabled"], cfg["pin_idle_ms"] = pin, 20000
        state = m.State(cfg)
        state._derive()
        rng = random.Random(20260001 + allow_release * 2 + allow_motion + 7 * pin)
        now = 0
        for step in range(30000):
            kind = rng.choice(("TICK", "HB", "SPI_ERR", "SPI_OK", "CAN", "ESTOP", "MOTION", "NAV", "DOWN", "UP", "PIN_OK", "PIN_BAD"))
            now += rng.randrange(700)
            args: tuple[int, ...] = ()
            if kind == "HB":
                args = (rng.randrange(7),)
            elif kind == "CAN":
                args = (int(rng.randrange(4) == 0), rng.randrange(200), rng.randrange(200))
            elif kind in ("ESTOP", "MOTION"):
                args = (rng.randrange(2),)
            elif kind == "NAV":
                args = (rng.randrange(11),)
            elif kind in ("DOWN", "UP"):
                args = (rng.randrange(14) - 1,)
            latched_before = state.latched
            listen_before = state.listen_only
            locked_before = state.locked()
            fx = state.event(now, kind, *args)
            where = f"step {step} {kind} {args}"
            if kind == "DOWN" and args == (m.A_STOP,):
                self.assertIn("stop", fx, where)
                self.assertEqual(m.MODES[state.mode], "stopped", where)
            for effect in fx:
                if effect.startswith("jog"):
                    degraded = state.link in (2, 3)
                    self.assertFalse(state.listen_only or listen_before or state.locked() or locked_before or state.mode in (0, 3, 4) or state.bus == 2 or (degraded and not allow_motion), where)
                if effect == "release":
                    self.assertTrue(latched_before and not state.listen_only and not state.locked() and not locked_before and state.bus != 2 and (state.link == 1 or allow_release), where)
            self.assertTrue(state.caps & 1, where)  # STOP is always allowed
            if state.listen_only:
                self.assertEqual(state.caps & ((1 << m.A_RELEASE) | (1 << m.A_JOG_ARM) | (1 << m.A_JOG_FIRST)), 0, where)
            if state.locked():
                self.assertEqual(state.caps & ((1 << m.A_RELEASE) | (1 << m.A_JOG_ARM) | (1 << m.A_JOG_FIRST)), 0, where)
            if state.latched:
                self.assertEqual(state.mode, 4, where)
            if state.link == 1:
                self.assertNotEqual(state.mode, 2, where)
            self.assertLessEqual(len(fx), 24, where)

    def test_the_rules_hold_with_the_defaults(self):
        self.run_fuzz(0, 0)

    def test_the_rules_hold_with_each_degraded_switch_on(self):
        self.run_fuzz(1, 0)
        self.run_fuzz(0, 1)
        self.run_fuzz(1, 1)

    def test_the_rules_hold_with_the_operator_pin_on(self):
        self.run_fuzz(0, 0, 1)
        self.run_fuzz(1, 1, 1)


if __name__ == "__main__":
    unittest.main()
