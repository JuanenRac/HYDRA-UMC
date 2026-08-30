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
import threading
import unittest
from urllib.request import Request, urlopen

from fake_transport import FakeBootloaderTransport

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

    def test_unknown_route_is_a_real_404(self):
        try:
            urlopen(f"{self.base_url}/unknown")
            self.fail("expected an HTTPError")
        except Exception as error:
            self.assertEqual(error.code, 404)
            error.close()


if __name__ == "__main__":
    unittest.main()
