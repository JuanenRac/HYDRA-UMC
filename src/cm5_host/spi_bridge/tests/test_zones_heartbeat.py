# =============================================================================
# HYDRA-UMC - spi_bridge zones_heartbeat tests: the heartbeat of the safety-zones chain goes to the brain only while the chain is healthy
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see repo root LICENSE
# =============================================================================
import json
import threading
import unittest
from datetime import UTC, datetime, timedelta
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from spi_bridge import zones_heartbeat as zh
from spi_bridge.__main__ import build_zones_heartbeat, make_parser
from spi_bridge.protocol import SpiOtaFrame

NOW = datetime(2026, 1, 2, 3, 4, 5, tzinfo=UTC)


def verdict(state="READY", age=0.2, stamp=None):
    moment = NOW - timedelta(seconds=age)
    return {"sdkSafetyState": {"schema_version": "1.0", "state": state, "source": "x", "timestamp_utc": stamp or moment.isoformat().replace("+00:00", "Z")}, "reason": None}


class RecordingTransport:
    def __init__(self):
        self.sent: list[SpiOtaFrame] = []
        self.fail: OSError | None = None

    def transceive(self, frame):
        if self.fail:
            raise self.fail
        self.sent.append(frame)
        return SpiOtaFrame(0, 0, 0xFF, 0)

    def wait_data_ready(self, timeout_seconds):
        return True

    def close(self):
        pass


class HealthyOrNot(unittest.TestCase):
    def test_a_fresh_ready_verdict_is_healthy(self):
        self.assertTrue(zh.state_is_healthy(verdict(), NOW, 1.0))

    def test_every_other_state_is_not(self):
        for state in ("INHIBITED", "SAFE_STOP", "FAULT", "ready", "", None, 1):
            self.assertFalse(zh.state_is_healthy(verdict(state), NOW, 1.0), repr(state))

    def test_an_old_verdict_is_not_and_the_limit_itself_still_counts(self):
        self.assertTrue(zh.state_is_healthy(verdict(age=1.0), NOW, 1.0))
        self.assertFalse(zh.state_is_healthy(verdict(age=1.001), NOW, 1.0))

    def test_a_verdict_from_the_future_is_a_wrong_clock_not_a_fresh_one(self):
        self.assertTrue(zh.state_is_healthy(verdict(age=-1.5), NOW, 1.0))
        self.assertFalse(zh.state_is_healthy(verdict(age=-2.5), NOW, 1.0))

    def test_what_is_not_a_verdict_is_not_healthy(self):
        for document in (None, [], "READY", 3, {}, {"sdkSafetyState": None}, {"sdkSafetyState": []}, {"sdkSafetyState": {"state": "READY"}}):
            self.assertFalse(zh.state_is_healthy(document, NOW, 1.0), repr(document))

    def test_a_time_that_is_missing_unparsable_or_without_a_zone_is_not_trusted(self):
        for stamp in ("", "yesterday", "2026-01-02T03:04:05", 12345, None):
            document = verdict()
            document["sdkSafetyState"]["timestamp_utc"] = stamp
            self.assertFalse(zh.state_is_healthy(document, NOW, 1.0), repr(stamp))
        self.assertTrue(zh.state_is_healthy(verdict(stamp="2026-01-02T03:04:05+00:00"), NOW, 1.0))
        self.assertTrue(zh.state_is_healthy(verdict(stamp="2026-01-02T04:04:05+01:00"), NOW, 1.0))


class SenderTests(unittest.TestCase):
    def make(self, documents, **options):
        transport = RecordingTransport()
        feed = iter(documents)
        sender = zh.ZonesHeartbeat(transport, threading.Lock(), lambda: next(feed), clock=lambda: NOW, **options)
        return sender, transport

    def test_it_sends_while_ready_and_stops_the_moment_it_is_not(self):
        sender, transport = self.make([verdict(), verdict(), verdict("SAFE_STOP"), verdict(), None])
        results = [sender.poll_once() for _ in range(5)]
        self.assertEqual(results, [True, True, False, True, False])
        self.assertEqual((sender.stats.sent, sender.stats.withheld), (3, 2))
        self.assertTrue(all((f.frame_type, f.dlc, f.payload) == (0x26, 4, b"ZONE") for f in transport.sent))

    def test_a_service_that_cannot_be_read_is_never_a_heartbeat(self):
        sender, transport = self.make([None, None])
        self.assertEqual([sender.poll_once(), sender.poll_once()], [False, False])
        self.assertEqual(transport.sent, [])

    def test_a_busy_link_skips_the_pass_instead_of_waiting(self):
        transport = RecordingTransport()
        lock = threading.Lock()
        sender = zh.ZonesHeartbeat(transport, lock, lambda: verdict(), clock=lambda: NOW)
        with lock:
            self.assertFalse(sender.poll_once())
        self.assertEqual((sender.stats.link_failures, transport.sent), (1, []))
        self.assertTrue(sender.poll_once(), "the lock was released: the next pass sends")

    def test_a_link_failure_is_counted_and_the_lock_is_released(self):
        transport = RecordingTransport()
        transport.fail = OSError("no device")
        lock = threading.Lock()
        sender = zh.ZonesHeartbeat(transport, lock, lambda: verdict(), clock=lambda: NOW)
        self.assertFalse(sender.poll_once())
        self.assertEqual(sender.stats.link_failures, 1)
        self.assertFalse(lock.locked())

    def test_the_period_and_the_maximum_age_must_be_above_zero(self):
        for options in ({"period_seconds": 0}, {"max_age_seconds": 0}, {"period_seconds": -1}):
            with self.assertRaises(ValueError):
                zh.ZonesHeartbeat(RecordingTransport(), threading.Lock(), lambda: None, **options)

    def test_the_thread_sends_until_it_is_told_to_stop(self):
        transport = RecordingTransport()
        sender = zh.ZonesHeartbeat(transport, threading.Lock(), lambda: verdict(), period_seconds=0.01, clock=lambda: NOW)
        thread, stop = sender.start()
        deadline = threading.Event()
        for _ in range(200):
            if sender.stats.sent >= 3:
                break
            deadline.wait(0.01)
        stop.set()
        thread.join(2)
        self.assertGreaterEqual(sender.stats.sent, 3)
        self.assertFalse(thread.is_alive())


class FetchTests(unittest.TestCase):
    def serve(self, body: bytes, status=200):
        seen = []

        class Handler(BaseHTTPRequestHandler):
            def do_GET(self):  # noqa: N802 - BaseHTTPRequestHandler requires this name
                seen.append((self.path, self.headers.get("Authorization")))
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, *args):
                pass

        server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()

        def stop():
            server.shutdown()
            server.server_close()
            thread.join(2)

        self.addCleanup(stop)
        return f"http://127.0.0.1:{server.server_address[1]}", seen

    def test_the_verdict_is_read_with_the_token(self):
        url, seen = self.serve(json.dumps(verdict()).encode())
        document = zh.fetch_state(url + "/", "secret-token", 2.0)
        self.assertEqual(document["sdkSafetyState"]["state"], "READY")
        self.assertEqual(seen, [("/state", "Bearer secret-token")])

    def test_without_a_token_no_header_is_sent(self):
        url, seen = self.serve(b"{}")
        zh.fetch_state(url, None, 2.0)
        self.assertEqual(seen[0][1], None)

    def test_a_service_that_is_down_or_answers_badly_is_none(self):
        self.assertIsNone(zh.fetch_state("http://127.0.0.1:9", None, 0.5))
        url, _ = self.serve(b"not json")
        self.assertIsNone(zh.fetch_state(url, None, 2.0))
        url, _ = self.serve(b"{}", status=503)
        self.assertIsNone(zh.fetch_state(url, None, 2.0))

    def test_an_answer_above_the_limit_is_refused(self):
        url, _ = self.serve(b'{"pad": "' + b"x" * (zh.MAX_STATE_BYTES + 10) + b'"}')
        self.assertIsNone(zh.fetch_state(url, None, 2.0))


class CommandLineTests(unittest.TestCase):
    def parse(self, *extra):
        return make_parser().parse_args(["serve", "--gpio-chip", "/dev/gpiochip4", "--gpio-line", "17", "--hmac-key-file", "k", *extra])

    def test_it_is_off_unless_the_address_is_given(self):
        self.assertIsNone(build_zones_heartbeat(self.parse(), RecordingTransport(), threading.Lock(), {}))

    def test_with_the_address_it_is_built_with_the_defaults_of_the_contract(self):
        sender = build_zones_heartbeat(self.parse("--zones-state-url", "http://127.0.0.1:8100"), RecordingTransport(), threading.Lock(), {})
        self.assertEqual((sender.period_seconds, sender.max_age_seconds), (0.1, 1.0))

    def test_a_wrong_period_is_refused_at_the_start(self):
        with self.assertRaises(ValueError):
            build_zones_heartbeat(self.parse("--zones-state-url", "http://x", "--zones-period", "0"), RecordingTransport(), threading.Lock(), {})


if __name__ == "__main__":
    unittest.main()
