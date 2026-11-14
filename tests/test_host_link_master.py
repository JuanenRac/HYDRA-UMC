# =============================================================================
# HYDRA-UMC - tests/test_host_link_master.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The transport of the host link with the board as SPI master: the Python reference, its configuration, the vectors the C twin is held to and the lists that run the C test.
Evidence: simulator (nothing has run on a board)."""
import random
import re
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import gen_host_link_vectors as gen  # noqa: E402
import host_link_master as hl  # noqa: E402
import spi_frame as sf  # noqa: E402

HEADER = (ROOT / "src/mcu_stm32h757/Common/host_link_master.h").read_text(encoding="utf-8")


def make(cfg=None, answers=True):
    """A link with a recording double: returns the link and a dict the test sets the host frame in (it also records what went out and what was dispatched)."""
    state = {"host": hl.SILENCE, "ready": False, "sent": [], "dispatched": [], "fail": False}

    def transfer(wire):
        state["sent"].append(wire)
        return None if state["fail"] else state["host"]

    def dispatch(request):
        state["dispatched"].append(request)
        return bytes([0, 0, 0x60, 1, request[4]]) + bytes(7) if answers else None

    link = hl.HostLinkMaster(dict(cfg or hl.load_config()), transfer, lambda: state["ready"], dispatch)
    return link, state


def damaged(wire: bytes) -> bytes:
    broken = bytearray(wire)
    broken[sf.OFS_CRC + 1] ^= 0xFF
    return bytes(broken)


class ConfigTests(unittest.TestCase):
    def test_the_file_is_valid_and_conservative(self):
        cfg = hl.load_config()
        self.assertEqual(cfg["require_seal"], 1)
        self.assertEqual(cfg["ready_known"], 0)
        self.assertLessEqual(cfg["min_gap_ms"], cfg["idle_interval_ms"])

    def test_values_out_of_range_are_refused(self):
        base = (ROOT / "config/host_link.cfg").read_text(encoding="utf-8")
        changes = (
            ("idle_interval_ms = 50", "idle_interval_ms = 0"),
            ("min_gap_ms = 2", "min_gap_ms = 60"),
            ("max_retries = 3", "max_retries = 11"),
            ("down_after_failures = 5", "down_after_failures = 0"),
            ("require_seal = 1", "require_seal = 2"),
            ("ready_known = 0", "ready_known = 3"),
        )
        for field, bad in changes:
            self.assertIn(field, base)
            with tempfile.TemporaryDirectory() as folder:
                path = Path(folder) / "host_link.cfg"
                path.write_text(base.replace(field, bad), encoding="utf-8")
                with self.assertRaises(ValueError, msg=bad):
                    hl.load_config(path)

    def test_the_c_twin_refuses_the_same_limits(self):
        for fragment in ("idle_interval_ms < 1u", "idle_interval_ms > 10000u", "min_gap_ms > 1000u", "max_retries > 10u", "down_after_failures < 1u", "down_after_failures > 100u"):
            self.assertIn(fragment, HEADER)


class BehaviourTests(unittest.TestCase):
    def test_the_first_poll_sends_a_sealed_idle_frame_and_two_are_not_closer_than_the_gap(self):
        link, state = make()
        self.assertEqual(link.poll(0), hl.POLL_DONE)
        self.assertEqual(sf.check(state["sent"][0]), "ok")
        self.assertEqual(state["sent"][0][2], hl.IDLE_TYPE)
        self.assertEqual(link.poll(1), hl.POLL_NOT_DUE)
        self.assertEqual(link.poll(2), hl.POLL_NOTHING)
        self.assertEqual(link.poll(50), hl.POLL_DONE)
        self.assertEqual([sent[sf.OFS_SEQ] for sent in state["sent"]], [1, 2])

    def test_a_damaged_request_is_never_dispatched(self):
        link, state = make()
        state["host"] = damaged(sf.build(0, 0, 0x30, b"\x01", sequence=1))
        link.poll(0)
        self.assertEqual(state["dispatched"], [])
        self.assertEqual(link.counters.bad_crc, 1)

    def test_a_request_is_dispatched_once_and_its_answer_goes_out_next(self):
        link, state = make()
        state["host"] = sf.build(0, 0, 0x30, b"\x07", sequence=9)
        link.poll(0)
        self.assertEqual(len(state["dispatched"]), 1)
        self.assertIsNotNone(link.staged)
        state["host"] = sf.build(0, 0, hl.IDLE_TYPE, b"", sequence=10)
        link.poll(2)
        self.assertEqual(state["sent"][1][2], 0x60)
        self.assertEqual(state["sent"][1][4], 7)
        self.assertIsNone(link.staged)

    def test_the_same_sequence_twice_runs_the_command_once(self):
        link, state = make(answers=False)
        state["host"] = sf.build(0, 0, 0x30, b"\x01", sequence=4)
        link.poll(0)
        link.poll(50)
        link.poll(100)
        self.assertEqual(len(state["dispatched"]), 1)
        self.assertEqual(link.counters.duplicates, 2)

    def test_silence_is_not_an_error_and_keeps_a_staged_frame(self):
        link, state = make()
        link.stage(bytes([0, 0, 0x40, 0]) + bytes(8))
        link.poll(0)
        self.assertEqual(link.counters.silence, 1)
        self.assertEqual(link.counters.bad_crc + link.counters.unsealed, 0)
        self.assertIsNotNone(link.staged)
        self.assertEqual(link.poll(10), hl.POLL_NOTHING)  # waits for the idle interval instead of clocking a floating line at the full rate
        self.assertEqual(link.poll(50), hl.POLL_DONE)

    def test_a_noisy_link_costs_frames_never_a_wrong_command(self):
        link, state = make()
        link.stage(bytes([0, 0, 0x40, 0]) + bytes(8))
        state["host"] = damaged(sf.build(0, 0, 0x30, b"\x01", sequence=1))
        for step in range(10):
            link.poll(step * 2)
        self.assertEqual(state["dispatched"], [])
        self.assertEqual(link.counters.dropped, 1)
        self.assertEqual(link.counters.retries, link.cfg["max_retries"])

    def test_the_link_goes_down_after_failures_in_a_row_and_up_with_the_first_good_transaction(self):
        link, state = make(dict(hl.load_config(), down_after_failures=2))
        state["fail"] = True
        link.poll(0)
        self.assertFalse(link.down)
        link.poll(50)
        self.assertTrue(link.down)
        state["fail"] = False
        link.poll(100)
        self.assertFalse(link.down)

    def test_ready_is_read_only_when_the_line_is_wired(self):
        calls = []
        link, _ = make(dict(hl.load_config(), ready_known=0))
        link.ready = lambda: calls.append(1) or True
        link.poll(0)
        link.poll(10)
        self.assertEqual(calls, [])
        wired, state2 = make(dict(hl.load_config(), ready_known=1))
        wired.poll(0)
        state2["ready"] = True
        self.assertEqual(wired.poll(10), hl.POLL_DONE)

    def test_the_slot_holds_one_frame(self):
        link, _ = make()
        self.assertTrue(link.stage(bytes(12)))
        self.assertFalse(link.stage(bytes(12)))

    def test_the_counters_agree_with_the_calls_over_a_long_random_run(self):
        rng = random.Random(11)
        link, state = make()
        done = 0
        t = 0
        for _ in range(2000):
            t += rng.choice([0, 1, 2, 5, 50, 120])
            seq = rng.randrange(256)
            good = sf.build(0, 0, 0x30, b"\x01", sequence=seq)
            state["host"] = rng.choice([hl.SILENCE, sf.build(0, 0, hl.IDLE_TYPE, b"", sequence=seq), good, damaged(good), sf.build(0, 0, 0x30, b"\x01")])
            if link.poll(t) == hl.POLL_DONE:
                done += 1
        c = link.counters
        self.assertEqual(c.transactions, done)
        self.assertEqual(c.transactions, c.dispatched + c.duplicates + c.bad_crc + c.unsealed + c.idle_rx + c.silence)
        self.assertEqual(len(state["dispatched"]), c.dispatched)
        for request in state["dispatched"]:
            self.assertEqual(sf.check(request), "ok")  # only a good frame is ever run


class TwinTests(unittest.TestCase):
    def test_the_vectors_are_what_the_reference_says(self):
        self.assertEqual(gen.main(["--check"]), 0)

    def test_every_verdict_and_every_poll_result_appears_in_the_scenarios(self):
        polls, verdicts = set(), set()
        for cfg_name, steps in gen.SCENARIOS.values():
            for row in gen.run(cfg_name, steps):
                polls.add(row[0])
                if row[0] == hl.POLL_DONE:
                    verdicts.add(row[1])
        self.assertEqual(polls, {hl.POLL_NOT_DUE, hl.POLL_NOTHING, hl.POLL_DONE, hl.POLL_XFER_FAIL})
        self.assertEqual(verdicts, {hl.RX_DISPATCH, hl.RX_DUPLICATE, hl.RX_BAD_CRC, hl.RX_UNSEALED, hl.RX_IDLE, hl.RX_SILENCE})

    def test_the_c_test_is_in_both_lists_and_the_header_has_no_hal(self):
        self.assertIn("tests/test_host_link_master.c", (ROOT / "tools/host_tests.py").read_text(encoding="utf-8"))
        self.assertIn("test_host_link_master", (ROOT / "tools" / "build_firmware.py").read_text(encoding="utf-8"))
        self.assertNotIn("HAL_", HEADER)
        self.assertEqual(re.findall(r"#include <(\w+)\.h>", HEADER), ["stdint", "string"])


if __name__ == "__main__":
    unittest.main()
