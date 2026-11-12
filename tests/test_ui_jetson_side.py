# =============================================================================
# HYDRA-UMC - tests/test_ui_jetson_side.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The Jetson side of the exchange as a protocol-level service (tools/ui_jetson_side.py): it answers the status and the camera picture from sources, ignores what it cannot trust,
never says a value the panel cannot read, and gives the master reference the same heartbeats and pictures as the Jetson double. Evidence: simulator; nothing ran on a Jetson."""
import struct
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import spi_boot_master as boot  # noqa: E402
import ui_jetson_side as side  # noqa: E402
import ui_link as link  # noqa: E402


def make(zone=0, cpu=23, temperature=52.0):
    cameras = side.CameraSource()
    service = side.JetsonSide(lambda now: side.StatusReading(zone, cpu, temperature), cameras)
    return service, cameras


def status_request(sequence=1):
    return boot.encode(link.T_STATUS_REQ, sequence)


def snapshot_request(sequence=1, camera=0, last=0, offset=0):
    return boot.encode(link.T_SNAPSHOT_REQ, sequence, struct.pack(link.SNAPSHOT_REQ_FORMAT, camera, last, offset))


class StatusTests(unittest.TestCase):
    def test_the_status_carries_the_verdict_the_load_the_temperature_and_the_uptime(self):
        service, cameras = make(zone=2, cpu=40, temperature=61.5)
        service.boot_ms = 1000
        status = link.decode_status(service.answer(status_request(5), 4000), 5)
        self.assertEqual((status.zone_level, status.cpu_pct, status.temperature_c, status.uptime_s, status.picture_available), (2, 40, 61.5, 3, False))
        cameras.publish(0, link.make_jpeg(500))
        self.assertTrue(link.decode_status(service.answer(status_request(6), 4000), 6).picture_available)

    def test_a_value_the_panel_cannot_read_is_never_said(self):
        service, _ = make(zone=9, cpu=250)
        status = link.decode_status(service.answer(status_request(), 100), 1)
        self.assertEqual((status.zone_level, status.cpu_pct), (5, 100))  # an unknown verdict, a load capped at 100
        service, _ = make(zone=-3, cpu=-5)
        status = link.decode_status(service.answer(status_request(), 100), 1)
        self.assertEqual((status.zone_level, status.cpu_pct), (5, 0))

    def test_what_cannot_be_trusted_is_not_answered(self):
        service, _ = make()
        request = status_request(3)
        for index in range(len(request)):
            damaged = bytearray(request)
            damaged[index] ^= 0x01
            self.assertIsNone(service.answer(bytes(damaged), 0), index)
        self.assertIsNone(service.answer(boot.encode(link.T_STATUS_REQ, 1, b"x"), 0))  # a status request has no payload
        self.assertIsNone(service.answer(boot.encode(link.T_SNAPSHOT_REQ, 1, b"\x00\x00"), 0))  # a short snapshot request
        self.assertIsNone(service.answer(boot.encode(0x01, 1), 0))  # a type of the boot exchange
        self.assertIsNone(service.answer(b"", 0))
        self.assertGreaterEqual(service.ignored, 5)


class PictureTests(unittest.TestCase):
    def test_a_camera_that_is_not_there_or_is_busy_or_has_nothing_newer_says_why(self):
        service, cameras = make()
        reason = lambda frame: boot.decode(frame, link.HOST_TYPES)[2][0]  # noqa: E731
        self.assertEqual(reason(service.answer(snapshot_request(camera=3), 0)), side.NO_CAMERA)
        number = cameras.publish(0, link.make_jpeg(500))
        self.assertEqual(reason(service.answer(snapshot_request(last=number), 0)), side.NO_NEWER)
        self.assertEqual(reason(service.answer(snapshot_request(last=number, offset=9999), 0)), side.NO_NEWER)
        cameras.busy.add(0)
        self.assertEqual(reason(service.answer(snapshot_request(), 0)), side.BUSY)
        self.assertFalse(link.decode_status(service.answer(status_request(), 0), 1).picture_available)  # a busy camera is not offered

    def test_a_chunk_comes_from_any_offset_and_a_lost_one_is_simply_asked_again(self):
        service, cameras = make()
        picture = link.make_jpeg(1000)
        cameras.publish(0, picture)
        kind, _, payload = boot.decode(service.answer(snapshot_request(offset=240), 0), link.HOST_TYPES)
        picture_id, total, offset = struct.unpack_from(link.SNAPSHOT_HEADER, payload)
        self.assertEqual((kind, picture_id, total, offset), (link.T_SNAPSHOT, 1, 1000, 240))
        self.assertEqual(payload[struct.calcsize(link.SNAPSHOT_HEADER) :], picture[240:480])
        again = service.answer(snapshot_request(offset=240), 0)
        self.assertEqual(boot.decode(again, link.HOST_TYPES)[2], payload)  # no state: the same answer

    def test_the_picture_number_wraps_in_sixteen_bits(self):
        _, cameras = make()
        cameras.pictures[0] = (65535, b"x")
        self.assertEqual(cameras.publish(0, b"y"), 0)


class WithTheMasterTests(unittest.TestCase):
    def test_the_master_gets_the_same_heartbeats_and_pictures_as_from_the_double(self):
        picture = link.make_jpeg(3000, 4)
        service, cameras = make(zone=1)
        cameras.publish(0, picture)
        double = link.FakeJetson(picture, zone_level=1)
        results = []
        for host in (service, double):
            master = link.LinkMaster(host, 2 * 24576)
            events = []
            for now in range(0, 1500, 50):
                events += [(now, name, args) for name, args in master.step(now)]
            results.append((events, master.pictures))
        self.assertEqual(results[0], results[1])
        self.assertEqual(results[0][1], [picture])

    def test_a_newer_picture_is_fetched_and_a_busy_camera_stops_the_offers(self):
        service, cameras = make()
        cameras.publish(0, link.make_jpeg(1200, 1))
        master = link.LinkMaster(service, 2 * 24576)
        for now in range(0, 1500, 50):
            master.step(now)
        self.assertEqual(len(master.pictures), 1)
        cameras.publish(0, link.make_jpeg(1200, 2))
        for now in range(1500, 3000, 50):
            master.step(now)
        self.assertEqual(len(master.pictures), 2)
        cameras.busy.add(0)
        cameras.publish(0, link.make_jpeg(1200, 3))
        for now in range(3000, 4500, 50):
            master.step(now)
        self.assertEqual(len(master.pictures), 2)


if __name__ == "__main__":
    unittest.main()
