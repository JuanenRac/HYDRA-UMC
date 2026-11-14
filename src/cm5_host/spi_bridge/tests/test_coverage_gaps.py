# =============================================================================
# HYDRA-UMC - spi_bridge tests of the branches the other tests do not reach
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see repo root LICENSE
# =============================================================================
"""The real SPI/GPIO opening path with doubles for the two hardware libraries, the command line entry point, the busy answers (409) and the
dead link of the local service, and the refusals of the relay framing. Nothing here touches a bus: the link is a double."""

import contextlib
import io
import json
import sys
import tempfile
import threading
import types
import unittest
from pathlib import Path
from unittest import mock
from urllib.error import HTTPError
from urllib.request import Request, urlopen

from fake_transport import FakeBootloaderTransport

from spi_bridge import __main__ as entry
from spi_bridge.bootloader_client import SpiOtaFlasher, poll_frame
from spi_bridge.http_service import serve
from spi_bridge.protocol import (
    OFS_RELAY_RECV_RESPONSE,
    OFS_STATUS,
    SPI_FRAME_SIZE,
    STATUS_ERROR,
    SpiOtaFrame,
)
from spi_bridge.relay_tunnel import RelayedTransport, RelayFrame, build_relay_send_fragments, parse_relay_fragment, urtc_can_id_to_ofs
from spi_bridge.system_client import poll_probe
from spi_bridge.transport import RealSpiOtaTransport, _parse_spidev_path, open_spi_transport, seal_frame

SIG_Q = "&signature=" + "ab" * 64


def _status_frame_bytes(*, sealed: bool = True, damaged: bool = False) -> bytes:
    raw = SpiOtaFrame(2, 0, OFS_STATUS, 1, b"\x01").to_bytes()
    if sealed:
        raw = seal_frame(raw, 7)
    if damaged:
        raw = bytes([raw[0] ^ 0x01]) + raw[1:]  # inside the sealed span: the CRC no longer matches
    return raw


class FakeSpi:
    def __init__(self, answer: bytes):
        self.answer = answer
        self.sent: list[bytes] = []
        self.closed = False

    def xfer2(self, data):
        self.sent.append(bytes(data))
        return list(self.answer)

    def close(self):
        self.closed = True


class FakeReadyLine:
    def __init__(self, ready: bool):
        self.ready = ready
        self.released = False

    def event_wait(self, timeout):
        return self.ready

    def release(self):
        self.released = True


class RealTransportTests(unittest.TestCase):
    def test_a_sealed_answer_is_read_and_every_request_is_sealed_with_a_new_sequence(self):
        spi = FakeSpi(_status_frame_bytes())
        link = RealSpiOtaTransport(spi, FakeReadyLine(True))
        answer = link.transceive(poll_frame(2, 0, OFS_STATUS))
        link.transceive(poll_frame(2, 0, OFS_STATUS))
        self.assertEqual((answer.frame_type, answer.dlc, answer.payload[0]), (OFS_STATUS, 1, 1))
        self.assertEqual([sent[13] for sent in spi.sent], [1, 2])
        self.assertTrue(link.wait_data_ready(0.1))
        link.close()
        self.assertTrue(spi.closed)

    def test_an_unsealed_request_goes_out_as_built_when_sealing_is_off(self):
        spi = FakeSpi(_status_frame_bytes(sealed=False))
        link = RealSpiOtaTransport(spi, FakeReadyLine(False), seal_requests=False)
        link.transceive(poll_frame(2, 0, OFS_STATUS))
        self.assertEqual(spi.sent[0], poll_frame(2, 0, OFS_STATUS).to_bytes())
        self.assertFalse(link.wait_data_ready(0.1))

    def test_an_answer_of_the_wrong_length_a_damaged_one_and_a_corrupt_one_are_transfer_errors(self):
        short = RealSpiOtaTransport(FakeSpi(b"\x00" * 5), FakeReadyLine(True))
        with self.assertRaisesRegex(OSError, "returned 5 bytes"):
            short.transceive(poll_frame(2, 0, OFS_STATUS))
        damaged = RealSpiOtaTransport(FakeSpi(_status_frame_bytes(damaged=True)), FakeReadyLine(True))
        with self.assertRaisesRegex(OSError, "CRC"):
            damaged.transceive(poll_frame(2, 0, OFS_STATUS))
        floating = RealSpiOtaTransport(FakeSpi(b"\xff" * SPI_FRAME_SIZE), FakeReadyLine(True))
        with self.assertRaisesRegex(OSError, "corrupt frame"):
            floating.transceive(poll_frame(2, 0, OFS_STATUS))

    def test_a_frame_of_the_wrong_size_is_not_sealed(self):
        with self.assertRaises(ValueError):
            seal_frame(b"\x00" * 10, 1)

    def test_the_spidev_path_names_the_bus_and_the_device(self):
        self.assertEqual(_parse_spidev_path("/dev/spidev0.0"), (0, 0))
        self.assertEqual(_parse_spidev_path("/dev/spidev3.1"), (3, 1))
        for bad in ("/dev/null", "/dev/spidev", "/dev/spidevX.Y", "/dev/spidev0", "spidev1.", "/dev/spidev.2"):
            with self.assertRaises(ValueError, msg=bad):
                _parse_spidev_path(bad)


class FakeSpiDev:
    instances: list["FakeSpiDev"] = []

    def __init__(self):
        self.opened = None
        self.mode = None
        self.max_speed_hz = None
        self.bits_per_word = None
        self.closed = False
        FakeSpiDev.instances.append(self)

    def open(self, bus, device):
        self.opened = (bus, device)

    def xfer2(self, data):
        return list(_status_frame_bytes())

    def close(self):
        self.closed = True


class FakeRequest:
    def __init__(self, edges: bool):
        self.edges = edges
        self.read = 0
        self.released = False

    def wait_edge_events(self, timeout):
        return self.edges

    def read_edge_events(self):
        self.read += 1

    def release(self):
        self.released = True


def _fake_libraries(edges: bool):
    spidev = types.ModuleType("spidev")
    vars(spidev)["SpiDev"] = FakeSpiDev
    gpiod = types.ModuleType("gpiod")
    requests: list[FakeRequest] = []
    configs: list[dict] = []

    def request_lines(path, consumer, config):
        configs.append({"path": path, "consumer": consumer, "config": config})
        requests.append(FakeRequest(edges))
        return requests[-1]

    vars(gpiod)["request_lines"] = request_lines
    vars(gpiod)["LineSettings"] = lambda **kwargs: kwargs
    line = types.ModuleType("gpiod.line")
    vars(line)["Direction"] = types.SimpleNamespace(INPUT="input")
    vars(line)["Edge"] = types.SimpleNamespace(RISING="rising")
    vars(gpiod)["line"] = line
    return {"spidev": spidev, "gpiod": gpiod, "gpiod.line": line}, requests, configs


class OpenTransportTests(unittest.TestCase):
    def test_the_link_is_opened_with_the_documented_parameters_and_the_edge_is_drained(self):
        FakeSpiDev.instances.clear()
        modules, requests, configs = _fake_libraries(edges=True)
        with mock.patch.dict(sys.modules, modules):
            link = open_spi_transport("/dev/spidev1.2", "/dev/gpiochip4", 17, spi_speed_hz=5_000_000)
        device = FakeSpiDev.instances[0]
        self.assertEqual((device.opened, device.mode, device.max_speed_hz, device.bits_per_word), ((1, 2), 0, 5_000_000, 8))
        self.assertEqual((configs[0]["path"], configs[0]["consumer"]), ("/dev/gpiochip4", "hydra-umc-spi-bridge"))
        self.assertEqual(configs[0]["config"][17], {"direction": "input", "edge_detection": "rising"})
        self.assertTrue(link.wait_data_ready(0.1))
        self.assertEqual(requests[0].read, 1, "the pending edge is read, or every later wait would return at once")
        self.assertEqual(link.transceive(poll_frame(2, 0, OFS_STATUS)).frame_type, OFS_STATUS)
        link.close()
        self.assertTrue(device.closed and requests[0].released)

    def test_no_edge_means_not_ready_and_nothing_is_read(self):
        modules, requests, _configs = _fake_libraries(edges=False)
        with mock.patch.dict(sys.modules, modules):
            link = open_spi_transport("/dev/spidev0.0", "/dev/gpiochip0", 3)
        self.assertFalse(link.wait_data_ready(0.1))
        self.assertEqual(requests[0].read, 0)

    def test_a_missing_library_is_one_clear_error(self):
        with mock.patch.dict(sys.modules, {"spidev": None, "gpiod": None}):
            with self.assertRaisesRegex(RuntimeError, "not installed"):
                open_spi_transport("/dev/spidev0.0", "/dev/gpiochip0", 3)

    def test_a_bad_device_path_stops_before_anything_is_requested(self):
        modules, requests, _configs = _fake_libraries(edges=True)
        with mock.patch.dict(sys.modules, modules):
            with self.assertRaises(ValueError):
                open_spi_transport("/dev/ttyS0", "/dev/gpiochip0", 3)
        self.assertEqual(requests, [])


class FakeServer:
    def __init__(self, interrupted: bool = True):
        self.interrupted = interrupted
        self.closed = False
        self.options = None

    def serve_forever(self):
        if self.interrupted:
            raise KeyboardInterrupt

    def server_close(self):
        self.closed = True


class FakeLifecycle:
    def __init__(self):
        self.stop = threading.Event()

    def start(self):
        return None, self.stop


class FakeLink:
    def __init__(self):
        self.closed = False

    def close(self):
        self.closed = True


class MainTests(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.addCleanup(self.folder.cleanup)
        self.key = Path(self.folder.name) / "hmac.key"
        self.key.write_bytes(bytes(range(32)))
        self.base = ["serve", "--gpio-chip", "/dev/gpiochip4", "--gpio-line", "17", "--hmac-key-file", str(self.key)]

    def run_main(self, extra=(), *, publisher=None, heartbeat=None):
        link, server = FakeLink(), FakeServer()
        seen = {}

        def fake_serve(transport, key, **options):
            seen.update(options, key=key)
            return server

        out, err = io.StringIO(), io.StringIO()
        with (
            mock.patch.object(entry, "open_spi_transport", return_value=link),
            mock.patch.object(entry, "serve", fake_serve),
            mock.patch.object(entry, "build_publisher", return_value=publisher),
            mock.patch.object(entry, "build_zones_heartbeat", return_value=heartbeat),
            contextlib.redirect_stdout(out),
            contextlib.redirect_stderr(err),
        ):
            code = entry.main([*self.base, *extra])
        return code, out.getvalue(), err.getvalue(), link, server, seen

    def test_a_plain_start_serves_until_interrupted_and_closes_everything(self):
        code, out, _err, link, server, seen = self.run_main()
        self.assertEqual(code, 0)
        self.assertIn("HTTP service on 127.0.0.1:8765", out)
        self.assertTrue(link.closed and server.closed)
        self.assertEqual(seen["key"], bytes(range(32)))
        self.assertEqual((seen["fram_erase"], seen["node_health"]), (False, False))

    def test_the_switches_reach_the_service(self):
        _code, _out, _err, _link, _server, seen = self.run_main(["--enable-fram-erase", "--enable-node-health", "--port", "9000"])
        self.assertEqual((seen["fram_erase"], seen["node_health"], seen["port"]), (True, True, 9000))

    def test_the_publisher_and_the_heartbeat_are_started_and_stopped(self):
        publisher, heartbeat = FakeLifecycle(), FakeLifecycle()
        extra = ["--collector-url", "http://127.0.0.1:8092", "--counter-target", "2:0", "--zones-state-url", "http://127.0.0.1:8300"]
        code, out, _err, _link, _server, _seen = self.run_main(extra, publisher=publisher, heartbeat=heartbeat)
        self.assertEqual(code, 0)
        self.assertIn("CAN error counters of 1 board(s)", out)
        self.assertIn("heartbeat of the safety zones", out)
        self.assertTrue(publisher.stop.is_set() and heartbeat.stop.is_set())

    def test_a_missing_key_file_is_reported_and_nothing_is_opened(self):
        self.key.unlink()
        err = io.StringIO()
        with mock.patch.object(entry, "open_spi_transport") as opener, contextlib.redirect_stderr(err):
            code = entry.main(self.base)
        self.assertEqual(code, 2)
        self.assertIn("[spi-bridge]", err.getvalue())
        opener.assert_not_called()

    def test_a_link_that_cannot_be_opened_is_reported(self):
        err = io.StringIO()
        with mock.patch.object(entry, "open_spi_transport", side_effect=RuntimeError("spidev/gpiod are not installed")), contextlib.redirect_stderr(err):
            code = entry.main(self.base)
        self.assertEqual(code, 2)
        self.assertIn("not installed", err.getvalue())

    def test_a_counter_target_without_a_collector_is_refused(self):
        err = io.StringIO()
        with mock.patch.object(entry, "open_spi_transport", return_value=FakeLink()), contextlib.redirect_stderr(err):
            code = entry.main([*self.base, "--counter-target", "2:0"])
        self.assertEqual(code, 2)
        self.assertIn("--counter-target needs --collector-url", err.getvalue())


class KeyAndTokenTests(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.addCleanup(self.folder.cleanup)

    def file(self, name, data: bytes) -> str:
        path = Path(self.folder.name) / name
        path.write_bytes(data)
        return str(path)

    def test_the_key_is_32_raw_bytes_or_64_hexadecimal_characters(self):
        raw = bytes(range(32))
        self.assertEqual(entry.read_hmac_key(self.file("raw", raw)), raw)
        self.assertEqual(entry.read_hmac_key(self.file("hex", raw.hex().encode() + b"\n")), raw)

    def test_a_key_of_any_other_shape_is_refused(self):
        for name, data in (("short", b"abc"), ("sixty_four_not_hex", b"zz" * 32), ("empty", b"")):
            with self.assertRaisesRegex(ValueError, "32 bytes or 64 hexadecimal", msg=name):
                entry.read_hmac_key(self.file(name, data))

    def test_the_zones_heartbeat_takes_its_token_from_a_file_or_the_environment(self):
        args = entry.make_parser().parse_args([*["serve", "--gpio-chip", "c", "--gpio-line", "1", "--hmac-key-file", "k"], "--zones-state-url", "http://127.0.0.1:1"])
        link, lock = FakeLink(), threading.Lock()
        self.assertIsNotNone(entry.build_zones_heartbeat(args, link, lock, {entry.ZONES_TOKEN_ENV: " secret-from-env "}))
        args.zones_token_file = self.file("token", b" from-file \n")
        self.assertIsNotNone(entry.build_zones_heartbeat(args, link, lock, {}))
        args.zones_state_url = None
        self.assertIsNone(entry.build_zones_heartbeat(args, link, lock, {}))

    def test_the_collector_token_comes_from_a_file_the_environment_or_nowhere(self):
        self.assertEqual(entry.read_token(self.file("t", b" abc \n"), {}), "abc")
        self.assertEqual(entry.read_token(None, {entry.COLLECTOR_TOKEN_ENV: " xyz "}), "xyz")
        self.assertIsNone(entry.read_token(None, {}))
        self.assertIsNone(entry.read_token(self.file("empty", b"\n"), {}))


class BusyAndFailedTests(unittest.TestCase):
    def setUp(self):
        self.transport = FakeBootloaderTransport(hardware_id=0x48374334)
        self.lock = threading.Lock()
        self.server = serve(self.transport, hmac_key=b"\x00" * 32, port=0, bus_lock=self.lock, fram_erase=True, node_health=True)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.base_url = f"http://127.0.0.1:{self.server.server_port}"

    def tearDown(self):
        self.server.shutdown()
        self.thread.join()
        self.server.server_close()

    def call(self, path, *, data=None, method="GET"):
        try:
            with urlopen(Request(self.base_url + path, data=data, method=method)) as response:
                return response.status, json.loads(response.read().decode("utf-8").splitlines()[0])
        except HTTPError as error:
            with error:
                return error.code, json.loads(error.read())

    def test_every_route_that_uses_the_link_answers_409_while_another_operation_holds_it(self):
        flash = "/flash?tier=2&slot=0&hardware_id=0x48374334&version_major=0&version_minor=1" + SIG_Q
        routes = (
            ("/version?tier=2&slot=0", None, "GET"),
            ("/counters?tier=2&slot=0", None, "GET"),
            ("/node-health?slot=1", None, "GET"),
            ("/estop", None, "GET"),
            (flash, b"\x00" * 64, "POST"),
            ("/fram/erase?confirm=ERASE", b"", "POST"),
            ("/fram/selftest", b"", "POST"),
        )
        with self.lock:
            for path, data, method in routes:
                status, body = self.call(path, data=data, method=method)
                self.assertEqual(status, 409, path)
                self.assertIn("busy", body["error"], path)

    def test_a_link_that_is_down_reads_as_a_board_that_is_offline_not_as_a_server_error(self):
        self.transport.raise_on_transceive = OSError("link down")
        status, body = self.call("/version?tier=2&slot=0")
        self.assertEqual((status, body["online"]), (200, False))
        status, body = self.call("/counters?tier=2&slot=0")
        self.assertEqual((status, body["online"]), (200, False))
        self.assertEqual(self.lock.locked(), False, "the link is let go after a failed read")

    def test_the_node_health_refuses_a_slot_that_is_not_a_number_or_out_of_range(self):
        self.assertEqual(self.call("/node-health?slot=abc")[0], 400)
        self.assertEqual(self.call("/node-health?slot=8")[0], 400)
        self.assertEqual(self.call("/node-health")[0], 400)

    def test_an_erase_needs_the_word_erase(self):
        status, body = self.call("/fram/erase?confirm=yes", data=b"", method="POST")
        self.assertEqual(status, 400)
        self.assertIn("ERASE", body["error"])

    def test_an_unknown_route_is_a_404_for_get_and_post(self):
        self.assertEqual(self.call("/nothing")[0], 404)
        self.assertEqual(self.call("/nothing", data=b"", method="POST")[0], 404)


class RelayFramingTests(unittest.TestCase):
    def test_a_frame_that_does_not_fit_a_can_frame_is_refused(self):
        with self.assertRaisesRegex(ValueError, "uint16_t"):
            build_relay_send_fragments(RelayFrame(0x10000, 0, b""))
        with self.assertRaisesRegex(ValueError, "0-8"):
            build_relay_send_fragments(RelayFrame(0x7F0, 9, b"\x00" * 9))
        with self.assertRaisesRegex(ValueError, "match dlc"):
            build_relay_send_fragments(RelayFrame(0x7F0, 4, b"\x00" * 3))

    def test_a_fragment_shorter_than_its_header_is_refused(self):
        for size in (0, 1, 2):
            with self.assertRaises(ValueError):
                parse_relay_fragment(b"\x00" * size)

    def test_a_long_frame_splits_in_two_and_the_pieces_join_back(self):
        data = bytes(range(1, 9))
        first, second = build_relay_send_fragments(RelayFrame(0x7F5, 8, data))
        _id1, dlc1, part1 = parse_relay_fragment(first)
        _id2, dlc2, part2 = parse_relay_fragment(second)
        self.assertEqual((dlc1, dlc2, part1 + part2), (8, 8, data))


class ScriptedTier1:
    """A Tier 1 link that answers each RELAY_RECV poll with the next scripted fragment."""

    def __init__(self, fragments):
        self.fragments = list(fragments)
        self.ready_asked = []
        self.closed = False

    def transceive(self, frame):
        if not self.fragments:
            return SpiOtaFrame(frame.target_tier, frame.target_slot, 0xFF, 0)
        fragment = self.fragments.pop(0)
        return SpiOtaFrame(frame.target_tier, frame.target_slot, OFS_RELAY_RECV_RESPONSE, len(fragment), fragment)

    def wait_data_ready(self, timeout_seconds):
        self.ready_asked.append(timeout_seconds)
        return True

    def close(self):
        self.closed = True


class RelayedDrainTests(unittest.TestCase):
    def test_the_fragments_of_one_frame_are_joined_and_those_of_another_are_skipped(self):
        data = bytes(range(1, 9))
        first, second = build_relay_send_fragments(RelayFrame(0x7F5, 8, data))
        stranger = build_relay_send_fragments(RelayFrame(0x7F6, 3, b"abc"))[0]
        tier1 = ScriptedTier1([first, stranger, second])
        relayed = RelayedTransport(tier1, 1, 0)
        answer = relayed.transceive(poll_frame(2, 0, OFS_STATUS))
        self.assertEqual(answer.frame_type, urtc_can_id_to_ofs(0x7F5))
        self.assertEqual((answer.dlc, answer.payload[:8]), (8, data))

    def test_waiting_and_closing_go_to_the_link_that_owns_the_connection(self):
        tier1 = ScriptedTier1([])
        relayed = RelayedTransport(tier1, 1, 0)
        self.assertTrue(relayed.wait_data_ready(0.5))
        self.assertEqual(tier1.ready_asked, [0.5])
        relayed.close()
        self.assertFalse(tier1.closed, "the relayed view does not close the real link")


class SmallBranchTests(unittest.TestCase):
    def test_a_probe_poll_whose_transfer_fails_gives_nothing(self):
        failing = FakeBootloaderTransport()
        failing.raise_on_transceive = OSError("link down")
        self.assertIsNone(poll_probe(failing, 1))

    def test_a_status_wait_that_never_sees_a_final_status_ends_as_an_error(self):
        flasher = SpiOtaFlasher(ScriptedTier1([]), b"\x00" * 32)  # answers every poll with a frame of no type
        self.assertEqual(flasher._wait_for_status(2, 0, 0.05), STATUS_ERROR)


if __name__ == "__main__":
    unittest.main()
