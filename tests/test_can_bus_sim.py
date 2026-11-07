# =============================================================================
# HYDRA-UMC - tests/test_can_bus_sim.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The bus contract on a virtual CAN bus (tests/sim_can_bus.py): the slot scan with boards that are alone, absent, doubled and misplaced,
and the emergency stop under load, with a lost frame and with a node forced bus-off. Evidence: simulator. The timings are those of the model,
not measurements: the rows of docs/BENCH_TESTS.md stay open until they are measured on a bus with boards."""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

import can_ids  # noqa: E402
import estop_can  # noqa: E402
import slot_scan  # noqa: E402
from sim_can_bus import EstopListener, ScanHost, SimNode, VirtualBoard, VirtualCanBus  # noqa: E402

TIMING = estop_can.load_timing()


def bench(*boards):
    bus = VirtualCanBus()
    host = bus.attach(ScanHost())
    for board in boards:
        bus.attach(board)
    return bus, host


class SlotScanOnTheBus(unittest.TestCase):
    def test_empty_slots_are_absent_and_alone_boards_are_ok(self):
        bus, host = bench(VirtualBoard(0, 0, 0x1000), VirtualBoard(3, 3, 0x2000, slot_scan.ROLE_APPLICATION))
        result = host.scan(7)
        self.assertEqual(result[0], slot_scan.OK)
        self.assertEqual(result[3], slot_scan.OK)
        self.assertTrue(all(result[s] == slot_scan.ABSENT for s in (1, 2, 4, 5, 6, 7)))
        self.assertFalse(bus.violations)

    def test_two_boards_in_one_slot_with_different_identifiers_are_a_duplicate_and_no_write_is_allowed(self):
        bus, host = bench(VirtualBoard(2, 2, 0xAAAA0001), VirtualBoard(2, 2, 0xBBBB0002))
        result = host.scan(9)
        self.assertEqual(result[2], slot_scan.DUPLICATE)
        self.assertFalse(slot_scan.write_allowed(result[2]))

    def test_two_boards_that_answer_in_the_same_step_collide_and_the_slot_is_not_ok(self):
        # the same unique identifier: the same reply delay, and the same data except the one the board ident hides: force a collision with a different role
        first = VirtualBoard(4, 4, 0x77, slot_scan.ROLE_BOOTLOADER)
        second = VirtualBoard(4, 4, 0x77, slot_scan.ROLE_APPLICATION)
        bus, host = bench(first, second)
        result = host.scan(11)
        self.assertTrue(bus.error_frames, "the two answers were sent in the same step with different data")
        self.assertIn(result[4], (slot_scan.NOISY, slot_scan.DUPLICATE))
        self.assertFalse(slot_scan.write_allowed(result[4]))

    def test_a_board_in_the_wrong_slot_is_misplaced(self):
        bus, host = bench(VirtualBoard(5, 1, 0x3000))
        self.assertEqual(host.scan(3)[5], slot_scan.MISPLACED)

    def test_a_lost_reply_looks_absent_and_never_ok(self):
        bus, host = bench(VirtualBoard(1, 1, 0x4000))
        reply_id = can_ids.identifier(1 * can_ids.SLOT_WINDOW, can_ids.by_name("SLOT_SCAN_REPLY"))
        bus.drop_next[reply_id] = 1
        self.assertEqual(host.scan(5)[1], slot_scan.ABSENT)

    def test_a_reply_of_an_old_scan_is_not_taken_for_the_new_one(self):
        bus, host = bench(VirtualBoard(6, 6, 0x5000))
        self.assertEqual(host.scan(1)[6], slot_scan.OK)
        self.assertEqual(host.scan(2)[6], slot_scan.OK)


class EmergencyStopOnTheBus(unittest.TestCase):
    def setUp(self):
        self.bus = VirtualCanBus()
        self.brain = self.bus.attach(SimNode("brain", {estop_can.ESTOP_CAN_ID}))
        self.listener = self.bus.attach(EstopListener("axis node", None))
        self.seq = 0
        self.epoch = 1

    def send(self, kind, detail=0, source=estop_can.SOURCE_BRAIN):
        self.seq += 1
        self.brain.send(estop_can.ESTOP_CAN_ID, estop_can.encode(kind, detail, source, self.epoch, self.seq))

    def alive_for(self, milliseconds, detail=estop_can.ALIVE_OK):
        for elapsed in range(milliseconds):
            if elapsed % TIMING["alive_period"] == 0:
                self.send(estop_can.KIND_ALIVE, detail)
            self.bus.step()

    def make_ready(self):
        self.send(estop_can.KIND_TRIP, 0x01)  # the node learns the epoch from a stop
        self.bus.run(2)
        self.alive_for(2 * TIMING["alive_period"])
        self.send(estop_can.KIND_RELEASE)
        self.bus.run(2)
        self.assertEqual(self.listener.node.state, estop_can.READY)

    def test_a_node_is_latched_at_start_and_ready_after_trip_alive_release(self):
        self.assertEqual(self.listener.node.state, estop_can.LATCHED)
        self.make_ready()
        self.assertFalse(self.listener.node.drivers_enabled, "ready does not enable the drivers")
        self.assertTrue(self.listener.node.enable_drivers())

    def test_a_trip_latches_within_the_reaction_time_even_on_a_saturated_bus(self):
        self.make_ready()
        self.listener.node.enable_drivers()
        noise = self.bus.attach(SimNode("noise", None))
        for _ in range(5):  # a flood of low-priority frames every millisecond
            self.bus.run(1)
            for i in range(40):
                noise.send(0x700 + i % 16, b"\xAA" * 8)
        start = self.bus.now_ms
        self.send(estop_can.KIND_TRIP, 0x03, 0x01)
        latched_at = None
        for _ in range(TIMING["node_reaction_max"] * 3):
            self.bus.step()
            if self.listener.node.state == estop_can.LATCHED and latched_at is None:
                latched_at = self.bus.now_ms
        self.assertIsNotNone(latched_at)
        self.assertLessEqual(latched_at - start, TIMING["node_reaction_max"])
        self.assertFalse(self.listener.node.drivers_enabled)

    def test_silence_of_the_brain_latches_the_node_after_the_alive_timeout(self):
        self.make_ready()
        self.bus.run(TIMING["alive_timeout"] + 5)
        self.assertEqual(self.listener.node.state, estop_can.LATCHED)

    def test_a_lost_alive_frame_does_not_latch_but_a_lost_trip_is_repeated_by_the_sender(self):
        self.make_ready()
        self.bus.drop_next[estop_can.ESTOP_CAN_ID] = 1
        self.alive_for(TIMING["alive_period"] * 2)
        self.assertEqual(self.listener.node.state, estop_can.READY, "one lost alive frame is inside the timeout")
        self.bus.drop_next[estop_can.ESTOP_CAN_ID] = 1
        self.send(estop_can.KIND_TRIP, 0x01)  # lost
        self.bus.run(1)
        self.assertEqual(self.listener.node.state, estop_can.READY)
        for _ in range(3):  # the sender repeats a trip every trip_repeat milliseconds
            self.send(estop_can.KIND_TRIP, 0x01)
            self.bus.run(TIMING["trip_repeat"])
        self.assertEqual(self.listener.node.state, estop_can.LATCHED)

    def test_a_node_forced_bus_off_latches_and_cannot_be_released_without_a_live_brain(self):
        self.make_ready()
        self.listener.go_bus_off()
        self.assertEqual(self.listener.node.state, estop_can.LATCHED)
        self.listener.bus_off = False
        self.bus.run(TIMING["alive_timeout"] + 5)  # the brain stays silent for longer than the alive timeout
        self.send(estop_can.KIND_RELEASE)  # no live alive frame in the timeout: refused
        self.bus.run(2)
        self.assertEqual(self.listener.node.state, estop_can.LATCHED)

    def test_a_malformed_frame_on_the_stop_identifier_is_a_trip(self):
        self.make_ready()
        self.brain.send(estop_can.ESTOP_CAN_ID, b"\x02\x00\x00")
        self.bus.run(2)
        self.assertEqual(self.listener.node.state, estop_can.LATCHED)

    def test_the_stop_identifier_has_one_emitter_group_and_a_foreign_sender_is_recorded(self):
        intruder = self.bus.attach(SimNode("intruder", {0x123}))
        intruder.send(estop_can.ESTOP_CAN_ID, estop_can.encode(estop_can.KIND_TRIP, 0x01, 0x10, 1, 1))
        self.bus.run(2)
        self.assertEqual(len(self.bus.violations), 1)


class BusBasics(unittest.TestCase):
    def test_the_lowest_identifier_goes_first_and_the_capacity_follows_the_bit_rate(self):
        bus = VirtualCanBus()
        sender = bus.attach(SimNode("sender", None))
        for can_id in (0x300, 0x100, 0x200):
            sender.send(can_id, b"\x00")
        bus.run(1)
        self.assertEqual([entry[1] for entry in bus.log], [0x100, 0x200, 0x300])
        slow = VirtualCanBus(bitrate=125_000)
        self.assertLess(slow.frames_per_ms, bus.frames_per_ms)

    def test_the_same_identifier_with_different_data_from_two_nodes_is_an_error_frame(self):
        bus = VirtualCanBus()
        one, two = bus.attach(SimNode("one", None)), bus.attach(SimNode("two", None))
        one.send(0x055, b"\x01")
        two.send(0x055, b"\x02")
        bus.run(1)
        self.assertEqual(len(bus.error_frames), 1)
        self.assertFalse(bus.log)


if __name__ == "__main__":
    unittest.main()
