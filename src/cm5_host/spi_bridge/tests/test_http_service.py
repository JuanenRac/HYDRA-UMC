# =============================================================================
# HYDRA-UMC - spi_bridge http_service tests
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0 - see repo root LICENSE
# =============================================================================
"""Real end-to-end HTTP tests against the real local service (a real
socket, a real HTTP request/response) - only the SPI transport underneath
is faked, exactly mirroring HYDRA-UMC-BRIDGE-PRINTER3D's own real fixture-
HTTP-server test pattern used throughout this ecosystem this session."""

import json
import socket
import threading
import unittest
from urllib.request import Request, urlopen

from fake_transport import FakeBootloaderTransport
from test_relay_tunnel import FakeRelayingTier1Transport

from spi_bridge.http_service import serve


class HttpServiceTests(unittest.TestCase):
    def setUp(self):
        self.transport = FakeBootloaderTransport(hardware_id=0x48374334)
        self.server = serve(self.transport, hmac_key=b"\x00" * 32, port=0)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.base_url = f"http://127.0.0.1:{self.server.server_port}"

    def tearDown(self):
        self.server.shutdown()
        self.thread.join()
        self.server.server_close()

    def test_get_version_returns_the_real_queried_hardware_id(self):
        with urlopen(f"{self.base_url}/version?tier=2&slot=0") as response:
            payload = json.loads(response.read())
        self.assertTrue(payload["online"])
        self.assertEqual(payload["hardware_id"], 0x48374334)

    def test_get_version_with_a_non_integer_param_is_rejected(self):
        try:
            urlopen(f"{self.base_url}/version?tier=not-a-number")
            self.fail("expected an HTTPError")
        except Exception as error:  # urllib.error.HTTPError
            self.assertEqual(error.code, 400)
            error.close()

    def test_post_flash_streams_real_progress_lines_ending_in_done(self):
        firmware = b"\x00" * 2048
        request = Request(
            f"{self.base_url}/flash?tier=2&slot=0&hardware_id=0x48374334&version_major=0&version_minor=1",
            data=firmware,
            method="POST",
        )
        with urlopen(request) as response:
            lines = [json.loads(line) for line in response.read().decode("utf-8").splitlines()]
        self.assertEqual(lines[-1]["phase"], "done")
        self.assertEqual(lines[-1]["percent"], 100)

    def test_post_flash_with_a_malformed_content_length_is_rejected(self):
        # Real regression: do_POST() used to call int(Content-Length) with
        # no try/except at all - a non-numeric header crashed the handler
        # thread with an unhandled ValueError and sent back no HTTP
        # response whatsoever, instead of the clean 400 every other
        # malformed param on this route already gets. A raw socket is
        # needed here since urllib always computes a real Content-Length
        # itself.
        sock = socket.create_connection(("127.0.0.1", self.server.server_port), timeout=5)
        try:
            sock.sendall(
                b"POST /flash?tier=2&slot=0&hardware_id=0x48374334&version_major=0&version_minor=1 HTTP/1.1\r\n"
                b"Host: 127.0.0.1\r\n"
                b"Content-Length: not-a-number\r\n"
                b"Connection: close\r\n"
                b"\r\n"
            )
            sock.settimeout(5)
            response = sock.recv(65536)
        finally:
            sock.close()
        self.assertIn(b"400", response.split(b"\r\n", 1)[0])

    def test_post_flash_with_an_oversized_content_length_is_rejected(self):
        # Real regression: do_POST() used to call
        # self.rfile.read(content_length) straight off the unvalidated
        # Content-Length header - a client claiming a multi-gigabyte body
        # would make this handler thread block reading (and buffering)
        # that many bytes, a real DoS/OOM risk. A raw socket declares an
        # oversized header without actually sending gigabytes on the wire;
        # the handler must reject it from the header alone, before ever
        # calling rfile.read().
        from spi_bridge.http_service import MAX_FIRMWARE_BODY_BYTES

        sock = socket.create_connection(("127.0.0.1", self.server.server_port), timeout=5)
        try:
            sock.sendall(
                b"POST /flash?tier=2&slot=0&hardware_id=0x48374334&version_major=0&version_minor=1 HTTP/1.1\r\n"
                b"Host: 127.0.0.1\r\n"
                b"Content-Length: " + str(MAX_FIRMWARE_BODY_BYTES + 1).encode("ascii") + b"\r\n"
                b"Connection: close\r\n"
                b"\r\n"
            )
            sock.settimeout(5)
            response = sock.recv(65536)
        finally:
            sock.close()
        self.assertIn(b"413", response.split(b"\r\n", 1)[0])

    def test_unknown_route_is_a_real_404(self):
        try:
            urlopen(f"{self.base_url}/unknown")
            self.fail("expected an HTTPError")
        except Exception as error:
            self.assertEqual(error.code, 404)
            error.close()


class HttpServiceGuardTests(unittest.TestCase):
    """One SPI link, one operation at a time; bad targets and garbage frames are answered, not crashed on."""

    def _serve(self, transport):
        server = serve(transport, hmac_key=b"\x00" * 32, port=0)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()

        def stop():
            server.shutdown()
            thread.join()
            server.server_close()

        self.addCleanup(stop)
        return f"http://127.0.0.1:{server.server_port}"

    def _status(self, url, data=None, method="GET"):
        try:
            with urlopen(Request(url, data=data, method=method)) as response:
                return response.status, response.read()
        except Exception as error:  # urllib.error.HTTPError
            body = error.read()
            error.close()
            return error.code, body

    def test_flash_without_the_image_version_is_refused_up_front(self):
        base = self._serve(FakeBootloaderTransport())
        status, _ = self._status(f"{base}/flash?tier=2&slot=0&hardware_id=0x48374334", data=b"\x00" * 16, method="POST")
        self.assertEqual(status, 400)

    def test_a_tier_or_slot_outside_the_bus_is_refused(self):
        base = self._serve(FakeBootloaderTransport())
        self.assertEqual(self._status(f"{base}/version?tier=3&slot=0")[0], 400)
        self.assertEqual(self._status(f"{base}/version?tier=2&slot=8")[0], 400)
        status, _ = self._status(
            f"{base}/flash?tier=9&slot=0&hardware_id=0x48374334&version_major=0&version_minor=1", data=b"\x00" * 16, method="POST"
        )
        self.assertEqual(status, 400)

    def test_a_second_operation_while_a_flash_runs_gets_409_and_leaves_the_flash_alone(self):
        entered = threading.Event()
        release = threading.Event()

        class BlockingTransport(FakeBootloaderTransport):
            def transceive(self, frame):
                entered.set()
                release.wait(10)
                return super().transceive(frame)

        base = self._serve(BlockingTransport())
        flash = threading.Thread(
            target=self._status,
            args=(f"{base}/flash?tier=2&slot=0&hardware_id=0x48374334&version_major=0&version_minor=1",),
            kwargs={"data": b"\x00" * 2048, "method": "POST"},
            daemon=True,
        )
        flash.start()
        self.assertTrue(entered.wait(5))
        status, _ = self._status(f"{base}/version?tier=2&slot=0")
        self.assertEqual(status, 409)
        release.set()
        flash.join(10)

    def test_a_corrupt_reply_is_answered_as_an_offline_board_not_a_dropped_connection(self):
        transport = FakeBootloaderTransport()
        transport.raise_on_transceive = ValueError("dlc must be 0-8")
        base = self._serve(transport)
        status, body = self._status(f"{base}/version?tier=2&slot=0")
        self.assertEqual(status, 200)
        self.assertFalse(json.loads(body)["online"])

    def test_a_corrupt_reply_in_the_middle_of_a_flash_ends_the_stream_with_an_error_line(self):
        transport = FakeBootloaderTransport()
        transport.raise_on_transceive = ValueError("dlc must be 0-8")
        base = self._serve(transport)
        status, body = self._status(
            f"{base}/flash?tier=2&slot=0&hardware_id=0x48374334&version_major=0&version_minor=1", data=bytes(2048), method="POST"
        )
        self.assertEqual(status, 200)
        last = json.loads(body.decode("utf-8").splitlines()[-1])
        self.assertEqual(last["phase"], "error")


class HttpServiceRelayTests(unittest.TestCase):
    """relay=1 reaches Tier 2 (URTC Tool Head) through the real RELAY_SEND/
    RELAY_RECV tunnel - see relay_tunnel.py's own docstring."""

    def setUp(self):
        self.transport = FakeRelayingTier1Transport(hardware_id=0x0303CC01)
        self.server = serve(self.transport, hmac_key=b"\x00" * 32, port=0)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.base_url = f"http://127.0.0.1:{self.server.server_port}"

    def tearDown(self):
        self.server.shutdown()
        self.thread.join()
        self.server.server_close()

    def test_get_version_with_relay_reaches_the_real_tunneled_urtc_head(self):
        with urlopen(f"{self.base_url}/version?tier=2&slot=3&relay=1") as response:
            payload = json.loads(response.read())
        self.assertTrue(payload["online"])
        self.assertEqual(payload["hardware_id"], 0x0303CC01)

    def test_post_flash_with_relay_streams_real_progress_ending_in_done(self):
        firmware = b"\xAB" * 2048
        request = Request(
            f"{self.base_url}/flash?tier=2&slot=3&relay=1&hardware_id=0x0303CC01&version_major=0&version_minor=1",
            data=firmware,
            method="POST",
        )
        with urlopen(request) as response:
            lines = [json.loads(line) for line in response.read().decode("utf-8").splitlines()]
        self.assertEqual(lines[-1]["phase"], "done")


if __name__ == "__main__":
    unittest.main()
