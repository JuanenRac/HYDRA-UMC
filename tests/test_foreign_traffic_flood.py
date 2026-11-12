# =============================================================================
# HYDRA-UMC - tests/test_foreign_traffic_flood.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""Row 28 of docs/BENCH_TESTS.md on the virtual bus: frames of the other slots, of random identifiers and of the emergency stop sent at the maximum rate
to a controller board. The board is modelled with the two hardware filters of the firmware (the window of its own slot with mask 0x7C0, and the exact
emergency stop identifier 0x001) and a receive FIFO of the depth of tests/timing_models.json. Evidence: simulator; the row stays open until it is run on a
board."""
import json
import random
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

import can_ids  # noqa: E402
from sim_can_bus import Frame, SimNode, VirtualCanBus, slot_base  # noqa: E402

SLOT_FILTER_MASK = 0x7C0
FIFO_DEPTH = json.loads((Path(__file__).resolve().parent / "timing_models.json").read_text(encoding="utf-8"))["g474_start"]["rx_fifo_depth"]


class FilteredBoard(SimNode):
    """The receive side of one controller board: filters 0 and 1 of the firmware, a FIFO of FIFO_DEPTH frames emptied by the software each millisecond."""

    def __init__(self, slot: int, *, filtered: bool = True, drain_per_ms: int = 1):
        super().__init__(f"controller slot {slot}", set())
        self.slot, self.filtered, self.drain_per_ms = slot, filtered, drain_per_ms
        self.fifo: list[int] = []
        self.accepted: list[int] = []
        self.lost = 0
        self.rejected = 0

    def passes_filter(self, can_id: int) -> bool:
        if not self.filtered:
            return True
        return (can_id & SLOT_FILTER_MASK) == (slot_base(self.slot) & SLOT_FILTER_MASK) or can_id == can_ids.ESTOP_CAN_ID

    def on_frame(self, now_ms: int, frame: Frame) -> None:
        if not self.passes_filter(frame.can_id):
            self.rejected += 1
            return
        if len(self.fifo) >= FIFO_DEPTH:
            self.lost += 1
            return
        self.fifo.append(frame.can_id)

    def tick(self, now_ms: int) -> None:
        for _ in range(min(self.drain_per_ms, len(self.fifo))):
            self.accepted.append(self.fifo.pop(0))


class Flooder(SimNode):
    """Another node that queues far more frames than the bus can carry, so the bus is always full."""

    def __init__(self, identifiers):
        super().__init__("flooder", None)
        self.identifiers = identifiers
        self.sent = 0

    def tick(self, now_ms: int) -> None:
        for _ in range(self.bus.frames_per_ms + 2):
            self.send(self.identifiers[self.sent % len(self.identifiers)], bytes([self.sent & 0xFF]))
            self.sent += 1


def run_flood(identifiers, *, filtered=True, slot=3, milliseconds=60, drain_per_ms=1):
    bus = VirtualCanBus()
    board = bus.attach(FilteredBoard(slot, filtered=filtered, drain_per_ms=drain_per_ms))
    flooder = bus.attach(Flooder(identifiers))
    bus.run(milliseconds)
    return bus, board, flooder


def foreign_identifiers(slot):
    return [i for s in range(0x800 // can_ids.SLOT_WINDOW) if s != slot for i in (slot_base(s), slot_base(s) + can_ids.SLOT_WINDOW - 1)]


class ForeignTrafficAtMaximumRate(unittest.TestCase):
    def test_the_other_slots_at_the_maximum_rate_never_reach_the_fifo_and_never_overflow_it(self):
        _, board, flooder = run_flood(foreign_identifiers(3))
        self.assertGreater(flooder.sent, 300)
        self.assertGreater(board.rejected, 100)
        self.assertEqual(board.accepted, [])
        self.assertEqual(board.lost, 0)

    def test_random_identifiers_with_a_fixed_seed_only_let_the_own_window_and_the_stop_through(self):
        rng = random.Random(28)
        identifiers = [rng.randrange(0x800) for _ in range(512)]
        _, board, _ = run_flood(identifiers, slot=5)
        own = {slot_base(5) + i for i in range(can_ids.SLOT_WINDOW)} | {can_ids.ESTOP_CAN_ID}
        self.assertTrue(set(board.accepted) <= own)
        self.assertEqual(board.lost, 0)
        self.assertGreater(board.rejected, 0)

    def test_the_emergency_stop_in_the_middle_of_foreign_traffic_always_arrives(self):
        identifiers = foreign_identifiers(3) + [can_ids.ESTOP_CAN_ID]
        _, board, _ = run_flood(identifiers)
        self.assertIn(can_ids.ESTOP_CAN_ID, board.accepted)
        self.assertEqual(set(board.accepted), {can_ids.ESTOP_CAN_ID})
        self.assertEqual(board.lost, 0)

    def test_the_whole_own_window_is_accepted_and_the_neighbour_windows_are_not(self):
        board = FilteredBoard(7)
        for offset in range(can_ids.SLOT_WINDOW):
            self.assertTrue(board.passes_filter(slot_base(7) + offset))
            self.assertFalse(board.passes_filter(slot_base(6) + offset))
            self.assertFalse(board.passes_filter(slot_base(8) + offset))

    def test_without_the_filter_the_same_flood_overflows_the_fifo(self):
        _, board, _ = run_flood(foreign_identifiers(3), filtered=False)
        self.assertGreater(board.lost, 0)

    def test_the_flood_is_repeatable(self):
        first = run_flood(foreign_identifiers(2), slot=2)[1]
        second = run_flood(foreign_identifiers(2), slot=2)[1]
        self.assertEqual((first.rejected, first.lost, first.accepted), (second.rejected, second.lost, second.accepted))


if __name__ == "__main__":
    unittest.main()
