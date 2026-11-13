# =============================================================================
# HYDRA-UMC - tests/test_enabling_device.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The logic of the three-position enabling device (tools/enabling_device.py, its C twin src/mcu_stm32h757/CM4/enabling_device.h): motion only from the middle position reached from
the released one, a stop at once when it is left, the way back from panic through the middle never enables, a fault on a long disagreement or a code that is not a position, and a
fuzz of fixed seed that holds those rules. Evidence: simulator."""
import random
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import enabling_device as e  # noqa: E402
import gen_enabling_vectors  # noqa: E402

R, M, P = e.RELEASED, e.MIDDLE, e.PANIC


def device():
    cfg = e.load_config()
    return e.EnablingDevice(cfg["debounce_ms"], cfg["discrepancy_ms"])


class LogicTests(unittest.TestCase):
    def feed(self, d, t0, steps):
        t = t0
        out = []
        for dt, a, b in steps:
            t += dt
            out.append(d.update(t, a, b))
        return t, out

    def test_released_then_middle_enables_only_after_the_debounce(self):
        d = device()
        t, out = self.feed(d, 0, [(0, R, R), (30, R, R), (5, M, M), (10, M, M)])
        self.assertEqual(out, [False, False, False, False])
        t, out = self.feed(d, t, [(15, M, M)])
        self.assertEqual(out, [True])

    def test_held_in_the_middle_at_power_up_never_enables(self):
        d = device()
        _, out = self.feed(d, 0, [(0, M, M), (30, M, M), (1000, M, M)])
        self.assertEqual(out, [False, False, False])

    def test_leaving_the_middle_asks_for_the_stop_at_once_whatever_the_debounce(self):
        d = device()
        t, _ = self.feed(d, 0, [(0, R, R), (30, R, R), (5, M, M), (30, M, M)])
        self.assertTrue(d.enabled())
        for position in (R, P):
            probe = device()
            t2, _ = self.feed(probe, 0, [(0, R, R), (30, R, R), (5, M, M), (30, M, M)])
            before = probe.stops
            self.assertFalse(probe.update(t2 + 1, position, position))  # one millisecond: no debounce
            self.assertEqual(probe.stops, before + 1)

    def test_the_way_back_from_panic_through_the_middle_does_not_enable(self):
        d = device()
        t, _ = self.feed(d, 0, [(0, R, R), (30, R, R), (5, M, M), (30, M, M), (5, P, P)])
        self.assertEqual(d.state, e.ST_LOCKED_OUT)
        _, out = self.feed(d, t, [(5, M, M), (100, M, M), (100, M, M)])
        self.assertEqual(out, [False, False, False])
        t, _ = self.feed(d, t + 205, [(5, R, R), (30, R, R)])
        self.assertEqual(d.state, e.ST_ARMED)
        _, out = self.feed(d, t, [(5, M, M), (30, M, M)])
        self.assertEqual(out[-1], True)

    def test_a_short_disagreement_is_tolerated_while_armed_but_stops_a_device_that_was_enabled(self):
        d = device()
        t, _ = self.feed(d, 0, [(0, R, R), (30, R, R)])
        t, out = self.feed(d, t, [(5, R, M), (20, R, M), (5, M, M), (30, M, M)])
        self.assertEqual(out[-1], True)  # moving up from 1 to 2 with the contacts a few ms apart
        before = d.stops
        self.assertFalse(d.update(t + 2, M, R))
        self.assertEqual(d.stops, before + 1)
        self.assertFalse(d.update(t + 4, M, M))  # back together, but it has to be released first
        self.assertEqual(d.state, e.ST_STOPPED)

    def test_a_long_disagreement_is_a_fault_and_a_clean_release_clears_it(self):
        d = device()
        t, _ = self.feed(d, 0, [(0, R, R), (30, R, R), (0, R, M)])
        t, _ = self.feed(d, t, [(60, R, M), (60, R, M)])
        self.assertEqual(d.state, e.ST_FAULT)
        t, out = self.feed(d, t, [(5, M, M), (200, M, M)])
        self.assertEqual(out, [False, False])
        t, _ = self.feed(d, t, [(5, R, R), (30, R, R)])
        self.assertEqual(d.state, e.ST_ARMED)

    def test_a_code_that_is_not_a_position_is_never_trusted(self):
        for bad in ((0, 2), (4, 4), (2, 255), (-1, 1)):
            d = device()
            t, _ = self.feed(d, 0, [(0, R, R), (30, R, R), (5, M, M), (30, M, M)])
            self.assertTrue(d.enabled())
            self.assertFalse(d.update(t + 1, *bad))
            self.assertEqual(d.state, e.ST_FAULT, bad)

    def test_the_clock_wraps(self):
        d = device()
        out = [d.update(t & 0xFFFFFFFF, a, b) for t, a, b in ((0xFFFFFFF0, R, R), (0x10, R, R), (0x20, M, M), (0x30, M, M), (0x40, M, M))]
        self.assertEqual(out, [False, False, False, False, True])

    def test_the_fuzz_never_enables_against_the_rules(self):
        rng = random.Random(20260412)
        cfg = e.load_config()
        d = e.EnablingDevice(cfg["debounce_ms"], cfg["discrepancy_ms"])
        t, last_good_release = 0, None
        for step in range(60_000):
            t += rng.choice((1, 1, 2, 5, 10, 25, 60))
            a = rng.choice((1, 1, 2, 2, 2, 3, 0, 4))
            b = a if rng.random() < 0.8 else rng.choice((1, 2, 3))
            was = d.enabled()
            now = d.update(t, a, b)
            if now:
                self.assertTrue(a == b == M, f"enabled with channels {a}, {b} (step {step})")
                self.assertTrue(was or last_good_release is not None, "enabled without a release before")
            if a == b == R:
                last_good_release = t
            if a == b == P or a != b or a not in (1, 2, 3):
                self.assertFalse(now, f"enabled on panic or a disagreement or a bad code (step {step})")
            if was and not now:
                self.assertGreaterEqual(d.stops, 1)
            if a == b == P:
                last_good_release = None  # after panic only a release arms again


class SourceTests(unittest.TestCase):
    def test_the_c_defaults_are_the_times_of_the_configuration_file(self):
        header = (ROOT / "src" / "mcu_stm32h757" / "CM4" / "enabling_device.h").read_text(encoding="utf-8")
        cfg = e.load_config()
        self.assertRegex(header, rf"#define ENABLING_DEBOUNCE_MS {cfg['debounce_ms']}u")
        self.assertRegex(header, rf"#define ENABLING_DISCREPANCY_MS {cfg['discrepancy_ms']}u")
        for name, value in (("RELEASED", 1), ("MIDDLE", 2), ("PANIC", 3)):
            self.assertRegex(header, rf"#define ENABLING_{name} {value}u")

    def test_the_configuration_refuses_values_out_of_range(self):
        import tempfile

        base = (ROOT / "config" / "enabling.cfg").read_text(encoding="utf-8")
        with tempfile.TemporaryDirectory() as folder:
            for name, text in (("zero", base.replace("debounce_ms = 20", "debounce_ms = 0")), ("tight", base.replace("discrepancy_ms = 100", "discrepancy_ms = 5")),
                               ("huge", base.replace("discrepancy_ms = 100", "discrepancy_ms = 9000"))):
                path = Path(folder) / f"{name}.cfg"
                path.write_text(text, encoding="utf-8")
                with self.assertRaises(ValueError, msg=name):
                    e.load_config(path)

    def test_the_vectors_the_c_test_replays_are_what_the_reference_says(self):
        self.assertEqual(gen_enabling_vectors.main(["--check"]), 0)

    def test_the_docs_name_the_logic(self):
        doc = (ROOT / "docs" / "ENABLING_DEVICE.md").read_text(encoding="utf-8")
        self.assertTrue(re.search(r"three-position", doc))
        self.assertIn("config/enabling.cfg", doc)


if __name__ == "__main__":
    unittest.main()
