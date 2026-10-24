# =============================================================================
# HYDRA-UMC - tests for the CAN error counters: query, HTTP route and publisher
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see repo root LICENSE
# =============================================================================
import json
import threading
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.request import urlopen

from spi_bridge.bootloader_client import query_error_counters
from spi_bridge.http_service import serve
from spi_bridge.protocol import (
    OFS_ERROR_COUNTERS_RESPONSE,
    OFS_QUERY_ERROR_COUNTERS,
    SPI_TARGET_STACKA,
    SpiOtaFrame,
)
from spi_bridge.relay_tunnel import RelayedTransport
from spi_bridge.telemetry_publisher import CounterPublisher, Target, parse_target

from test_relay_tunnel import FakeRelayingTier1Transport


class CountersTransport:
    """Answers the error counter query like a board; `slots` maps a slot to (tec, rec), a slot that is not there does not answer."""

    def __init__(self, slots):
        self.slots = slots
        self.sent = []

    def transceive(self, frame):
        self.sent.append(frame)
        if frame.frame_type == OFS_QUERY_ERROR_COUNTERS and frame.target_slot in self.slots:
            tec, rec = self.slots[frame.target_slot]
            return SpiOtaFrame(frame.target_tier, frame.target_slot, OFS_ERROR_COUNTERS_RESPONSE, 2, bytes((tec, rec)))
        return SpiOtaFrame(frame.target_tier, frame.target_slot, 0xFF, 0)

    def wait_data_ready(self, timeout_seconds):
        return True

    def close(self):
        pass


class FakeCollector:
    def __init__(self, status=202):
        self.received = []
        self.headers = []
        collector = self

        class Handler(BaseHTTPRequestHandler):
            def do_POST(self):  # noqa: N802
                body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
                collector.received.append((self.path, json.loads(body)))
                collector.headers.append(self.headers.get("Authorization"))
                self.send_response(status)
                self.end_headers()
                self.wfile.write(b"{}")

            def log_message(self, *args):
                pass

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.url = f"http://127.0.0.1:{self.server.server_port}"

    def close(self):
        self.server.shutdown()
        self.thread.join()
        self.server.server_close()


class QueryTests(unittest.TestCase):
    def test_a_board_that_answers_reports_its_counters(self):
        counters = query_error_counters(CountersTransport({3: (12, 7)}), SPI_TARGET_STACKA, 3)
        self.assertTrue(counters.online)
        self.assertEqual((counters.tec, counters.rec), (12, 7))

    def test_a_board_that_does_not_answer_is_offline_and_no_counters_are_invented(self):
        counters = query_error_counters(CountersTransport({}), SPI_TARGET_STACKA, 3)
        self.assertFalse(counters.online)
        self.assertEqual((counters.tec, counters.rec), (0, 0))

    def test_a_transport_failure_is_offline(self):
        class Broken:
            def transceive(self, frame):
                raise OSError("no link")

        self.assertFalse(query_error_counters(Broken(), SPI_TARGET_STACKA, 0).online)

    def test_through_the_relay_tunnel_the_head_answers_with_its_counters(self):
        tier1 = FakeRelayingTier1Transport()
        counters = query_error_counters(RelayedTransport(tier1, SPI_TARGET_STACKA, 2), SPI_TARGET_STACKA, 2)
        self.assertTrue(counters.online)
        self.assertEqual((counters.tec, counters.rec), (3, 1))


class HttpRouteTests(unittest.TestCase):
    def test_get_counters_answers_the_counters_of_a_board(self):
        server = serve(CountersTransport({2: (5, 9)}), b"\x00" * 32, port=0)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            with urlopen(f"http://127.0.0.1:{server.server_port}/counters?tier=2&slot=2") as response:
                payload = json.loads(response.read())
            self.assertEqual(payload, {"online": True, "tec": 5, "rec": 9})
            with urlopen(f"http://127.0.0.1:{server.server_port}/counters?tier=2&slot=5") as response:
                self.assertEqual(json.loads(response.read())["online"], False)
        finally:
            server.shutdown()
            thread.join()
            server.server_close()


class TargetTests(unittest.TestCase):
    def test_targets_are_parsed_and_checked(self):
        self.assertEqual(parse_target("2:3"), Target(2, 3, False))
        self.assertEqual(parse_target("2:3:relay"), Target(2, 3, True))
        self.assertEqual(Target(2, 3, True).source_id, "canbus-tier2-slot3-relay")
        for bad in ("", "2", "2:3:x", "9:1", "2:8", "a:b"):
            with self.assertRaises(ValueError):
                parse_target(bad)


class PublisherTests(unittest.TestCase):
    def setUp(self):
        self.collector = FakeCollector()
        self.addCleanup(self.collector.close)

    def make(self, transport, targets, **kwargs):
        return CounterPublisher(transport, threading.Lock(), targets, self.collector.url, clock=lambda: 1_800_000_000.0, **kwargs)

    def test_each_board_that_answers_becomes_one_sample_with_a_growing_sequence(self):
        publisher = self.make(CountersTransport({1: (10, 20), 2: (0, 1)}), [Target(2, 1), Target(2, 2), Target(2, 6)], token="0123456789abcdef")
        self.assertEqual(publisher.poll_once(), 2)
        self.assertEqual(publisher.poll_once(), 2)
        self.assertEqual(len(self.collector.received), 4)
        path, sample = self.collector.received[0]
        self.assertEqual(path, "/ingest/ws")
        self.assertEqual(sample["sourceId"], "canbus-tier2-slot1")
        self.assertEqual(sample["kind"], "can_error_counters")
        self.assertEqual(sample["fields"], {"tec": 10.0, "rec": 20.0})
        self.assertEqual(sample["timestamp"], 1_800_000_000_000)
        self.assertEqual([s["sequence"] for _, s in self.collector.received], [1, 2, 3, 4])
        self.assertEqual(set(self.collector.headers), {"Bearer 0123456789abcdef"})
        self.assertEqual(publisher.stats.missed, 2, "the board that never answers is counted, not invented")
        self.assertEqual(publisher.stats.last_counters["canbus-tier2-slot2"], {"tec": 0, "rec": 1})

    def test_without_a_token_no_authorization_header_is_sent(self):
        publisher = self.make(CountersTransport({0: (1, 1)}), [Target(2, 0)])
        publisher.poll_once()
        self.assertEqual(self.collector.headers, [None])

    def test_a_collector_that_is_down_is_counted_and_does_not_stop_the_round(self):
        publisher = CounterPublisher(CountersTransport({0: (1, 1), 1: (2, 2)}), threading.Lock(), [Target(2, 0), Target(2, 1)], "http://127.0.0.1:1")
        self.assertEqual(publisher.poll_once(), 0)
        self.assertEqual(publisher.stats.delivery_failures, 2)
        self.assertTrue(publisher.stats.last_error)

    def test_a_busy_link_is_skipped_not_interrupted(self):
        lock = threading.Lock()
        lock.acquire()
        publisher = CounterPublisher(CountersTransport({0: (1, 1)}), lock, [Target(2, 0)], self.collector.url)
        self.assertEqual(publisher.poll_once(), 0)
        self.assertEqual(publisher.stats.skipped_busy, 1)
        self.assertEqual(self.collector.received, [])

    def test_the_settings_are_checked(self):
        with self.assertRaises(ValueError):
            CounterPublisher(CountersTransport({}), threading.Lock(), [], self.collector.url)
        with self.assertRaises(ValueError):
            CounterPublisher(CountersTransport({}), threading.Lock(), [Target(2, 0)], self.collector.url, interval_seconds=0.1)
        with self.assertRaises(ValueError):
            CounterPublisher(CountersTransport({}), threading.Lock(), [Target(2, 0)], "collector:8092")

    def test_the_thread_polls_until_it_is_stopped(self):
        publisher = self.make(CountersTransport({0: (1, 1)}), [Target(2, 0)], interval_seconds=1.0)
        thread, stop = publisher.start()
        for _ in range(100):
            if self.collector.received:
                break
            stop.wait(0.05)
        stop.set()
        thread.join(5)
        self.assertFalse(thread.is_alive())
        self.assertGreaterEqual(len(self.collector.received), 1)


class MainTests(unittest.TestCase):
    def test_the_key_file_holds_raw_or_hexadecimal_bytes_and_nothing_else(self):
        import tempfile
        from pathlib import Path

        from spi_bridge.__main__ import read_hmac_key

        with tempfile.TemporaryDirectory() as folder:
            raw = Path(folder, "raw")
            raw.write_bytes(bytes(range(32)))
            hexed = Path(folder, "hex")
            hexed.write_text(bytes(range(32)).hex() + "\n")
            short = Path(folder, "short")
            short.write_bytes(b"abc")
            self.assertEqual(read_hmac_key(str(raw)), bytes(range(32)))
            self.assertEqual(read_hmac_key(str(hexed)), bytes(range(32)))
            with self.assertRaises(ValueError):
                read_hmac_key(str(short))

    def test_the_publisher_is_built_only_when_asked_for_and_the_token_never_comes_from_the_arguments(self):
        import tempfile
        from pathlib import Path

        from spi_bridge.__main__ import COLLECTOR_TOKEN_ENV, build_publisher, make_parser

        base = ["serve", "--gpio-chip", "/dev/gpiochip4", "--gpio-line", "17", "--hmac-key-file", "k"]
        parse = make_parser().parse_args
        lock = threading.Lock()
        self.assertIsNone(build_publisher(parse(base), CountersTransport({}), lock, {}))
        with self.assertRaises(ValueError):
            build_publisher(parse(base + ["--counter-target", "2:1"]), CountersTransport({}), lock, {})
        with self.assertRaises(ValueError):
            build_publisher(parse(base + ["--collector-url", "http://127.0.0.1:8092"]), CountersTransport({}), lock, {})
        publisher = build_publisher(
            parse(base + ["--collector-url", "http://127.0.0.1:8092", "--counter-target", "2:1", "--counter-target", "2:3:relay"]),
            CountersTransport({}),
            lock,
            {COLLECTOR_TOKEN_ENV: "0123456789abcdef"},
        )
        self.assertEqual(publisher._token, "0123456789abcdef")
        self.assertEqual([t.source_id for t in publisher._targets], ["canbus-tier2-slot1", "canbus-tier2-slot3-relay"])
        with tempfile.TemporaryDirectory() as folder:
            token_file = Path(folder, "token")
            token_file.write_text("fedcba9876543210\n")
            publisher = build_publisher(
                parse(base + ["--collector-url", "http://127.0.0.1:8092", "--counter-target", "2:1", "--collector-token-file", str(token_file)]),
                CountersTransport({}),
                lock,
                {COLLECTOR_TOKEN_ENV: "ignored-because-the-file-wins"},
            )
            self.assertEqual(publisher._token, "fedcba9876543210")


if __name__ == "__main__":
    unittest.main()
