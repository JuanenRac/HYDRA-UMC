# =============================================================================
# HYDRA-UMC - spi_bridge system_client tests: stop, release, telemetry and probe over the SPI link
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see repo root LICENSE
# =============================================================================
"""The bridge talks to the host twin of the service of the application of the CM4 (tools/spi_app.py), which takes the decisions of spi_app.h frame by
frame, through a transport that seals every request and returns what the firmware staged before it, as the real spidev transport does. Evidence:
simulator; nothing here runs on a board."""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[4] / "tools"))

import spi_app  # noqa: E402
import spi_system  # noqa: E402

from spi_bridge import system_client  # noqa: E402
from spi_bridge.protocol import SpiOtaFrame  # noqa: E402
from spi_bridge.transport import seal_frame  # noqa: E402


class TwinTransport:
    """A SpiOtaTransport whose far end is the twin of the firmware. `pump` plays the STACK A task between the transactions."""

    def __init__(self, app: spi_app.SpiApp, pump=None):
        self.app = app
        self.pump = pump
        self.sequence = 0
        self.sent: list[SpiOtaFrame] = []
        self.fail: OSError | None = None

    def transceive(self, frame: SpiOtaFrame) -> SpiOtaFrame:
        if self.fail:
            raise self.fail
        self.sent.append(frame)
        self.sequence = (self.sequence + 1) & 0xFF
        staged = self.app.tx_wire
        self.app.on_receive(seal_frame(frame.to_bytes(), self.sequence))
        self.app.service()
        if self.pump:
            self.pump()
        return SpiOtaFrame.from_bytes(staged)

    def wait_data_ready(self, timeout_seconds: float) -> bool:
        return True

    def close(self) -> None:
        pass


class Brain:
    """What the STACK A task does with the requests of the SPI task (the same lines as SpiApp_Pump)."""

    def __init__(self, app: spi_app.SpiApp, scan_after_pumps: int = 1):
        self.app = app
        self.now = 0
        self.tripped = True
        self.trips: list[int] = []
        self.releases = 0
        self.scan_after = scan_after_pumps
        self._scan_wait = 0
        self._scan_id: int | None = None

    def pump(self) -> None:
        self.now += 1
        if self.app.zones_tick(self.now, 1000):
            self.tripped = True
            self.trips.append(0x04)
        reason = self.app.take_trip()
        if reason is not None:
            self.tripped = True
            self.trips.append(reason)
        if self.app.take_release():
            self.releases += 1
            self.tripped = False
        scan_id = self.app.take_scan()
        if scan_id is not None:
            self._scan_id, self._scan_wait = scan_id, self.scan_after
        if self._scan_id is not None:
            self._scan_wait -= 1
            if self._scan_wait <= 0:
                self.app.scan_finished(self._scan_id, [1, 1, 2, 0, 4, 1, 1, 3])
                self._scan_id = None
        self.app.publish_brain_state(self.tripped, 0, 0)


def make(telemetry=True, stop=True, scan_after_pumps=1):
    app = spi_app.SpiApp(spi_system.Config(telemetry, stop))
    brain = Brain(app, scan_after_pumps)
    return app, brain, TwinTransport(app, brain.pump)


class TelemetryTests(unittest.TestCase):
    def test_the_counters_and_flags_come_back(self):
        _app, _brain, transport = make()
        info = system_client.read_telemetry(transport)
        self.assertIsNotNone(info)
        self.assertTrue(info.in_application)
        self.assertTrue(info.link_ok and info.stop_enabled and info.estop_latched)
        self.assertEqual((info.crc_errors, info.retries, info.seq_gaps), (0, 0, 0))

    def test_a_firmware_with_the_switches_off_does_not_answer_and_nothing_is_invented(self):
        _app, _brain, transport = make(telemetry=False, stop=False)
        self.assertIsNone(system_client.read_telemetry(transport))

    def test_a_link_failure_is_none_not_a_crash(self):
        _app, _brain, transport = make()
        transport.fail = OSError("no device")
        self.assertIsNone(system_client.read_telemetry(transport))
        self.assertFalse(system_client.request_stop(transport, 3))
        self.assertFalse(system_client.request_release(transport))
        self.assertIsNone(system_client.probe_slots(transport, 1))

    def test_read_only_firmware_reports_the_stop_commands_as_off(self):
        _app, _brain, transport = make(stop=False)
        info = system_client.read_telemetry(transport)
        self.assertFalse(info.stop_enabled)

    def test_the_decoder_refuses_what_is_not_a_telemetry_answer(self):
        self.assertIsNone(system_client.decode_telemetry(SpiOtaFrame(0, 0, 0x23, 7, bytes(7))))
        self.assertIsNone(system_client.decode_telemetry(SpiOtaFrame(0, 0, 0x23, 8, bytes([1]) + bytes(7))))  # a selector other than 0
        self.assertIsNone(system_client.decode_telemetry(SpiOtaFrame(0, 0, 0x24, 8, bytes(8))))


class StopAndReleaseTests(unittest.TestCase):
    def test_a_stop_reaches_the_brain_with_its_reason(self):
        _app, brain, transport = make()
        brain.tripped = False
        self.assertTrue(system_client.request_stop(transport, 7))
        self.assertEqual(brain.trips, [7])
        self.assertTrue(brain.tripped)
        info = system_client.read_telemetry(transport)
        self.assertTrue(info.estop_latched)

    def test_a_release_reaches_the_brain_and_the_telemetry_shows_it(self):
        _app, brain, transport = make()
        self.assertTrue(system_client.request_release(transport))
        self.assertEqual(brain.releases, 1)
        self.assertFalse(system_client.read_telemetry(transport).estop_latched)

    def test_with_the_stop_commands_off_the_brain_is_not_touched_although_the_frame_was_sent(self):
        _app, brain, transport = make(stop=False)
        brain.tripped = False
        self.assertTrue(system_client.request_stop(transport, 7))
        self.assertEqual(brain.trips, [])
        self.assertFalse(brain.tripped)

    def test_the_frames_are_exactly_the_ones_of_the_contract(self):
        _app, _brain, transport = make()
        system_client.request_stop(transport, 0x1FF)
        system_client.request_release(transport)
        self.assertEqual((transport.sent[0].frame_type, transport.sent[0].dlc, transport.sent[0].payload), (0x20, 5, b"STOP\xff"))
        self.assertEqual((transport.sent[1].frame_type, transport.sent[1].dlc, transport.sent[1].payload), (0x21, 4, b"RELS"))
        self.assertTrue(all(frame.target_tier == 0 for frame in transport.sent))


class ZonesHeartbeatTests(unittest.TestCase):
    def test_the_heartbeat_frame_is_the_one_of_the_contract_and_is_not_answered(self):
        _app, _brain, transport = make()
        self.assertTrue(system_client.send_zones_heartbeat(transport))
        sent = transport.sent[0]
        self.assertEqual((sent.target_tier, sent.frame_type, sent.dlc, sent.payload), (0, 0x26, 4, b"ZONE"))

    def test_the_brain_stops_the_stack_when_the_beats_stop_after_having_come(self):
        _app, brain, transport = make()
        brain.tripped = False
        for _ in range(5):
            self.assertTrue(system_client.send_zones_heartbeat(transport))
        self.assertEqual(brain.trips, [])
        brain.now += 1500  # the chain went silent
        brain.pump()
        self.assertEqual(brain.trips, [0x04])
        self.assertTrue(brain.tripped)

    def test_without_any_heartbeat_the_brain_is_never_stopped_by_silence(self):
        _app, brain, _transport = make()
        brain.tripped = False
        brain.now += 100000
        brain.pump()
        self.assertEqual(brain.trips, [])

    def test_a_link_failure_is_reported(self):
        _app, _brain, transport = make()
        transport.fail = OSError("no device")
        self.assertFalse(system_client.send_zones_heartbeat(transport))


class ProbeTests(unittest.TestCase):
    def test_the_verdicts_of_the_eight_slots(self):
        _app, _brain, transport = make()
        result = system_client.probe_slots(transport, 0x2A)
        self.assertIsNotNone(result)
        self.assertEqual(result.scan_id, 0x2A)
        self.assertEqual(result.verdicts, ("ok", "ok", "duplicate", "absent", "noisy", "ok", "ok", "misplaced"))

    def test_a_scan_that_takes_longer_is_waited_for_by_polling(self):
        _app, _brain, transport = make(scan_after_pumps=3)
        self.assertEqual(system_client.probe_slots(transport, 5).scan_id, 5)

    def test_a_scan_that_never_finishes_gives_up(self):
        _app, _brain, transport = make(scan_after_pumps=1000)
        self.assertIsNone(system_client.probe_slots(transport, 5, attempts=4))

    def test_the_answer_of_an_older_scan_is_not_taken_for_ours(self):
        _app, _brain, transport = make()
        system_client.probe_slots(transport, 1)
        self.assertIsNone(system_client.poll_probe(transport, 2, attempts=2))

    def test_the_probe_is_off_with_the_telemetry_switch_off(self):
        _app, _brain, transport = make(telemetry=False, stop=False)
        self.assertIsNone(system_client.probe_slots(transport, 1, attempts=2))

    def test_the_decoder_names_a_nibble_outside_the_table_unknown(self):
        result = system_client.decode_probe(SpiOtaFrame(0, 0, 0x25, 5, bytes([9, 0xF1, 0, 0, 0])))
        self.assertEqual(result.verdicts[:2], ("ok", "unknown"))
        self.assertIsNone(system_client.decode_probe(SpiOtaFrame(0, 0, 0x25, 4, bytes(4))))


if __name__ == "__main__":
    unittest.main()
