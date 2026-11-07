# =============================================================================
# HYDRA-UMC - tests/test_hil_bench.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The virtual bench (tests/sim_hil_bench.py) running the scripts of docs/BENCH_TESTS.md that need several boards: the batch update with duplicates, silent
boards and a cut during the flash; the health frame; the commands of the SPI link; the safety-zones frame on its way to the emergency stop; and the time
models of tests/sim_timing_models.py. Evidence: simulator. A row of the bench stays open until it is run on the boards."""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

import can_ids  # noqa: E402
import estop_can  # noqa: E402
import health_frame  # noqa: E402
import slot_scan  # noqa: E402
import spi_system  # noqa: E402
import sim_hil_bench as hil  # noqa: E402
import sim_timing_models as models  # noqa: E402
from sim_can_bus import EstopListener, VirtualCanBus  # noqa: E402


def bench_with(*boards):
    bench = hil.make_bench()
    for board in boards:
        bench.add(board)
    return bench


def on(**kwargs):
    return hil.BoardOptions(**kwargs)


class BatchUpdate(unittest.TestCase):
    def test_the_firmware_as_shipped_cannot_be_updated_over_the_bus_and_the_batch_says_so(self):
        """Bench row 14: with the switch off the application ignores ENTER_BOOTLOADER, and the board that does not answer is reported."""
        bench = bench_with(hil.ControllerBoardDouble(0, 0xA0), hil.ControllerBoardDouble(1, 0xA1))
        results = hil.flash_batch(bench, {0: "0.2.0", 1: "0.2.0"})
        self.assertEqual([r.status for r in results], ["no_answer", "no_answer"])
        self.assertTrue(all(b.version == "0.1.0" and b.flash_writes == 0 for b in bench.boards))
        self.assertFalse(bench.bus.violations)

    def test_three_boards_one_after_the_other_each_report_their_new_version(self):
        boards = [hil.ControllerBoardDouble(slot, 0x100 + slot, on(enter_bootloader=True)) for slot in (0, 1, 2)]
        bench = bench_with(*boards)
        results = hil.flash_batch(bench, {0: "0.2.0", 1: "0.2.0", 2: "0.2.0"})
        self.assertEqual([r.status for r in results], ["updated"] * 3)
        self.assertEqual([r.version_after for r in results], ["0.2.0"] * 3)
        self.assertTrue(all(b.role == slot_scan.ROLE_APPLICATION and b.resets == 2 for b in boards), "into the bootloader and back")
        self.assertFalse(bench.bus.violations)
        self.assertEqual([bench.scan()[slot] for slot in (0, 1, 2)], [slot_scan.OK] * 3)

    def test_a_duplicate_is_reported_before_any_write_and_the_others_finish(self):
        """Bench row 15: two boards share a slot; nothing is sent to that slot and nothing written."""
        twin_a = hil.ControllerBoardDouble(2, 0x2A, on(enter_bootloader=True))
        twin_b = hil.ControllerBoardDouble(2, 0x2B, on(enter_bootloader=True))
        good = hil.ControllerBoardDouble(1, 0x1A, on(enter_bootloader=True))
        bench = bench_with(good, twin_a, twin_b)
        results = {r.slot: r for r in hil.flash_batch(bench, {1: "0.2.0", 2: "0.2.0"})}
        self.assertEqual(results[2].status, "skipped")
        self.assertIn(slot_scan.DUPLICATE, results[2].reason)
        self.assertEqual(results[1].status, "updated")
        self.assertEqual((twin_a.resets, twin_b.resets, twin_a.flash_writes, twin_b.flash_writes), (0, 0, 0, 0))

    def test_an_absent_slot_is_skipped_and_a_silent_board_is_reported_without_stopping_the_batch(self):
        deaf = hil.ControllerBoardDouble(3, 0x3A, on(enter_bootloader=True, deaf=True))
        good = hil.ControllerBoardDouble(4, 0x4A, on(enter_bootloader=True))
        bench = bench_with(deaf, good)
        results = {r.slot: r for r in hil.flash_batch(bench, {3: "0.2.0", 4: "0.2.0", 5: "0.2.0"})}
        self.assertEqual(results[3].status, "skipped")   # a deaf board does not answer the scan either, so it is absent
        self.assertEqual(results[4].status, "updated")
        self.assertEqual(results[5].status, "skipped")

    def test_a_board_that_scans_fine_but_does_not_leave_its_application_is_reported(self):
        stubborn = hil.ControllerBoardDouble(0, 0x10, on(enter_bootloader=False))
        good = hil.ControllerBoardDouble(1, 0x11, on(enter_bootloader=True))
        bench = bench_with(stubborn, good)
        results = {r.slot: r for r in hil.flash_batch(bench, {0: "0.2.0", 1: "0.2.0"})}
        self.assertEqual(results[0].status, "no_answer")
        self.assertEqual(results[1].status, "updated")

    def test_a_board_whose_drivers_are_enabled_never_resets_for_a_frame(self):
        moving = hil.ControllerBoardDouble(0, 0x20, on(enter_bootloader=True))
        moving.node.state = estop_can.READY
        moving.node.drivers_enabled = True
        bench = bench_with(moving)
        results = hil.flash_batch(bench, {0: "0.2.0"})
        self.assertEqual(results[0].status, "no_answer")
        self.assertEqual(moving.resets, 0)

    def test_a_cut_during_the_flash_keeps_the_old_application_and_the_board_can_be_updated_again(self):
        """Bench rows 7 and 24 in the model: the append-only metadata keeps the old record until the new one is complete."""
        board = hil.ControllerBoardDouble(0, 0x30, on(enter_bootloader=True))
        bench = bench_with(board)
        first = hil.flash_batch(bench, {0: "0.2.0"}, cuts={0: 0.5})[0]
        self.assertEqual(first.status, "flash_failed")
        self.assertEqual((board.version, board.role), ("0.1.0", slot_scan.ROLE_APPLICATION))
        second = hil.flash_batch(bench, {0: "0.2.0"})[0]
        self.assertEqual((second.status, board.version), ("updated", "0.2.0"))

    def test_the_bootloader_refuses_a_flash_when_the_board_is_not_in_it(self):
        board = hil.ControllerBoardDouble(0, 0x40)
        self.assertEqual(board.flash("0.2.0"), "not_in_bootloader")


class HealthFrameOnTheBus(unittest.TestCase):
    def test_a_board_with_the_switch_on_sends_one_frame_a_second_with_a_continuous_sequence(self):
        board = hil.ControllerBoardDouble(1, 0x50, on(health_frame=True))
        bench = bench_with(board)
        bench.bus.run(3600)
        heard = bench.host.health_heard.get(1, [])
        self.assertGreaterEqual(len(heard), 3)
        self.assertEqual([h.sequence for h in heard], list(range(len(heard))))
        self.assertTrue(heard[0].flags & health_frame.FLAG_ESTOP, "a board starts latched and says so")
        self.assertFalse(bench.bus.violations)

    def test_a_board_with_the_switch_off_sends_nothing_new(self):
        bench = bench_with(hil.ControllerBoardDouble(1, 0x51))
        bench.bus.run(3600)
        self.assertEqual(bench.host.health_heard, {})
        health_id = can_ids.identifier(hil.slot_base(1), can_ids.by_name("HEALTH_FRAME"))
        self.assertFalse([entry for entry in bench.bus.log if entry[1] == health_id])

    def test_a_lost_frame_shows_as_a_gap_in_the_sequence(self):
        board = hil.ControllerBoardDouble(2, 0x52, on(health_frame=True))
        bench = bench_with(board)
        bench.bus.drop_next[can_ids.identifier(hil.slot_base(2), can_ids.by_name("HEALTH_FRAME"))] = 1
        bench.bus.run(3600)
        sequences = [h.sequence for h in bench.host.health_heard[2]]
        self.assertEqual(sequences[0], 1, "the first frame was lost on the bus")

    def test_the_health_frame_does_not_disturb_the_slot_scan(self):
        board = hil.ControllerBoardDouble(3, 0x53, on(health_frame=True))
        bench = bench_with(board)
        bench.bus.run(1200)
        self.assertEqual(bench.scan()[3], slot_scan.OK)

    def test_the_whole_bus_keeps_the_one_emitter_per_identifier_rule(self):
        boards = [hil.ControllerBoardDouble(slot, 0x60 + slot, on(health_frame=True, enter_bootloader=True)) for slot in range(4)]
        bench = bench_with(*boards)
        hil.flash_batch(bench, {slot: "0.2.0" for slot in range(4)})
        bench.bus.run(2500)
        self.assertEqual(bench.bus.violations, [])
        self.assertEqual(bench.bus.error_frames, [])


class SpiCommands(unittest.TestCase):
    def setUp(self):
        self.boards = [hil.ControllerBoardDouble(0, 0x70), hil.ControllerBoardDouble(2, 0x72, on(enter_bootloader=True))]
        self.bench = bench_with(*self.boards)

    def double(self, telemetry=True, stop=True):
        return hil.SpiBrainDouble(self.bench, spi_system.Config(telemetry, stop))

    def test_the_probe_runs_the_slot_scan_and_returns_the_verdicts(self):
        client = spi_system.Client(self.double().exchange)
        scan_id, names = client.scan(9)
        self.assertEqual(scan_id, 9)
        self.assertEqual(names[0], slot_scan.OK)
        self.assertEqual(names[1], slot_scan.ABSENT)
        self.assertEqual(names[2], slot_scan.OK)

    def test_the_telemetry_reports_the_counters_and_the_stop_switch(self):
        double = self.double(stop=False)
        double.snapshot.crc_errors, double.snapshot.can_rx_errors = 4, 2
        telemetry = spi_system.Client(double.exchange).read_telemetry()
        self.assertEqual((telemetry.crc_errors, telemetry.can_rx_errors, telemetry.stop_enabled, telemetry.link_ok), (4, 2, False, True))

    def test_stop_and_release_reach_the_brain_only_with_both_switches_on(self):
        read_only = self.double(stop=False)
        spi_system.Client(read_only.exchange).stop(7)
        spi_system.Client(read_only.exchange).release()
        self.assertEqual((read_only.trips, read_only.releases), ([], 0))
        full = self.double()
        spi_system.Client(full.exchange).stop(7)
        spi_system.Client(full.exchange).release()
        self.assertEqual((full.trips, full.releases), ([7], 1))

    def test_with_everything_off_the_cm5_gets_no_answer_at_all(self):
        double = self.double(telemetry=False, stop=False)
        client = spi_system.Client(double.exchange)
        self.assertIsNone(client.read_telemetry())
        self.assertIsNone(client.scan(1))
        self.assertEqual(double.scans, 0)


class SafetyZonesFrame(unittest.TestCase):
    """HSAF-IDEA-005: the frame of the safety-zones service against the emulated bus."""

    def setUp(self):
        self.bus = VirtualCanBus()
        self.bridge = self.bus.attach(hil.ZonesBridge())
        self.node = self.bus.attach(EstopListener("axis node", None))
        self.service = self.bus.attach(hil.SimNode("safety zones service", {0x7A0, 0x7A1, 0x001, 0x002}))
        self.node.node.state = estop_can.READY

    def test_the_native_stop_frame_on_the_stop_identifier_is_malformed_for_the_boards_and_stops_them(self):
        """The default identifier of the service is 0x001: the layout there is not the one of docs/ESTOP_CAN.md, a node reads it as malformed and latches."""
        native = hil.zones_frame(hil.ZONES_KIND_STOP, 1, 0xBEEF, 2)
        self.assertIsNone(estop_can.decode(native))
        self.service.send(0x001, native)
        self.bus.run(3)
        self.assertEqual(self.node.node.state, estop_can.LATCHED)

    def test_the_native_heartbeat_identifier_is_not_in_the_filter_of_any_board(self):
        for slot in range(8):
            base = 0x600 + slot * 0x20
            self.assertNotEqual(0x002 & 0x7E0, base)
        self.assertNotEqual(0x002, estop_can.ESTOP_CAN_ID)

    def test_through_the_bridge_a_stop_becomes_a_trip_of_reason_safety_zone_from_the_cm5(self):
        self.service.send(0x7A0, hil.zones_frame(hil.ZONES_KIND_STOP, 1, 0x1234, 1))
        self.bus.run(30)
        trips = [estop_can.decode(data) for _, can_id, data in self.bus.log if can_id == estop_can.ESTOP_CAN_ID]
        self.assertEqual(len(trips), 3, "a stop is repeated, a bus may drop a frame")
        self.assertTrue(all(t.kind == estop_can.KIND_TRIP and t.detail == 0x04 and t.source == estop_can.SOURCE_CM5 for t in trips))
        self.assertEqual(self.node.node.state, estop_can.LATCHED)
        self.assertFalse(self.bus.violations)

    def test_a_damaged_frame_is_a_stop_too(self):
        damaged = bytearray(hil.zones_frame(hil.ZONES_KIND_STOP, 1))
        damaged[3] ^= 0x40
        self.service.send(0x7A0, bytes(damaged))
        self.bus.run(10)
        self.assertEqual(self.node.node.state, estop_can.LATCHED)

    def test_a_steady_heartbeat_stops_nothing_and_its_silence_does(self):
        for step in range(0, 2000, 100):
            self.service.send(0x7A1, hil.zones_frame(hil.ZONES_KIND_HEARTBEAT, step // 100))
            self.bus.run(100)
        self.node.node.last_alive_ms = None
        self.assertIsNone(self.bridge.tripped_reason)
        self.bus.run(1200)   # the service goes silent
        self.assertEqual(self.bridge.tripped_reason, "no heartbeat")
        self.assertEqual(self.node.node.state, estop_can.LATCHED)

    def test_silence_before_the_first_heartbeat_is_not_a_stop(self):
        self.bus.run(3000)
        self.assertIsNone(self.bridge.tripped_reason)


class TimingModels(unittest.TestCase):
    def test_the_start_model_loses_the_frames_the_fifo_cannot_hold_and_has_no_receive_interrupt(self):
        for level in (0, 1, 2):
            result = models.g474_start(level)
            self.assertEqual(result["frames_kept_in_the_fifo"], models.MODELS["g474_start"]["rx_fifo_depth"])
            self.assertEqual(result["frames_lost"], result["frames_arrived"] - result["frames_kept_in_the_fifo"])
            self.assertEqual(result["receive_interrupts_before_the_scheduler"], 0)
        ticks = [models.g474_start(level)["ticks_before_the_scheduler"] for level in (0, 1, 2)]
        self.assertGreater(ticks[0], ticks[1])
        self.assertGreater(ticks[1], ticks[2])
        self.assertEqual(ticks[2], 0)

    def test_the_spi_link_recovers_after_every_damaged_frame_and_runs_no_frame_twice(self):
        result = models.spi_link()
        self.assertEqual(result["frames_dispatched"], models.MODELS["spi_link"]["frames_to_send"])
        self.assertGreater(result["dropped_bad_crc"], 0)
        self.assertEqual(result["recoveries"], result["retries"], "every damaged frame was followed by a frame that was run")
        self.assertGreater(result["dropped_duplicates"], 0)

    def test_a_flipped_marker_is_accepted_unchecked_unless_the_crc_is_required(self):
        loose = models.spi_link()
        self.assertGreater(loose["accepted_unsealed_unchecked"], 0, "the finding that HYDRA_SPI_REQUIRE_CRC exists for")
        strict_params = dict(models.MODELS["spi_link"], require_crc=True)
        strict = models.spi_link(strict_params)
        self.assertEqual(strict["accepted_unsealed_unchecked"], 0)
        self.assertGreater(strict["dropped_unsealed"], 0)

    def test_the_spi_model_is_repeatable(self):
        self.assertEqual(models.spi_link(), models.spi_link())

    def test_the_relay_model_orders_the_latencies(self):
        result = models.stack_a_relay()
        self.assertLessEqual(result["read_best_ms"], result["read_average_ms"])
        self.assertLessEqual(result["read_average_ms"], result["read_worst_ms"])
        self.assertLess(result["read_worst_during_scan_ms"], result["read_worst_ms"])
        self.assertGreater(result["refresh_rate_hz_all_boards_worst_case"], 10, "eight boards are read more than ten times a second even in the worst case")


if __name__ == "__main__":
    unittest.main()
