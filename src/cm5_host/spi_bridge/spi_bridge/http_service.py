# =============================================================================
# HYDRA-UMC - Local HTTP service exposing the real SPI-OTA bridge
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0 - see repo root LICENSE
# =============================================================================
"""A small, stdlib-only local HTTP service HYDRA-UMC-SERVER (Node.js)
relays to - the same "small local upstream service, reached over loopback
HTTP" pattern SERVER already uses for Voice UI/Datalake
(server.ts's own `POST /api/voice/turn`/`proxyToDatalake`).

Real routes:
  GET  /version?tier=&slot=&relay=   -> query_version()
  POST /flash?tier=&slot=&hardware_id=&relay= -> SpiOtaFlasher.flash(),
                                        streamed as newline-delimited JSON

`tier`/`slot` always identify the directly-reachable Tier 0/1 target (the
one `transport` was already opened against). `relay=1` additionally tunnels
through it to reach that Robot Controller Board's own URTC Tool Head
(Tier 2) via `RelayedTransport` - see relay_tunnel.py's own docstring for
the real RELAY_SEND/RELAY_RECV wire format this wraps. Tier 3 (Advanced
Expansion Board) needs one further real tunnel hop (URTC's own I2C bridge,
CAN IDs 0x210-0x221, docs/EXPANSION.TXT in the sibling URTC repo) - not
implemented here yet; `relay=1` only reaches Tier 2 today.

No TLS, no auth beyond loopback-only binding by default - same trust level
as the rest of this ecosystem's backend on its own LAN (see
HYDRA-UMC-SERVER's own docs/REMOTE_API.md for that established posture).
This service does not itself decide whether a flash is allowed - callers
(the SERVER relay) are expected to have already gated the request the same
way every other bridge in this ecosystem gates a real command, before it
ever reaches this process.
"""

from __future__ import annotations

import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

from .bootloader_client import SpiOtaFlasher, query_version
from .relay_tunnel import RelayedTransport
from .transport import SpiOtaTransport


def make_handler(transport: SpiOtaTransport, hmac_key: bytes) -> type[BaseHTTPRequestHandler]:
    """Builds a request handler bound to one already-open transport - kept
    as a factory (not a module-level handler) so tests can inject a fake
    transport without a real SPI/GPIO link."""

    class SpiBridgeHandler(BaseHTTPRequestHandler):
        def do_GET(self):  # noqa: N802 - BaseHTTPRequestHandler requires this name
            parsed = urlparse(self.path)
            if parsed.path != "/version":
                self._send_json(404, {"error": "not found"})
                return
            params = parse_qs(parsed.query)
            try:
                tier = int(params.get("tier", ["0"])[0])
                slot = int(params.get("slot", ["0"])[0])
            except ValueError:
                self._send_json(400, {"error": "tier/slot must be integers"})
                return
            effective_transport = RelayedTransport(transport, tier, slot) if params.get("relay", ["0"])[0] == "1" else transport
            info = query_version(effective_transport, tier, slot)
            self._send_json(
                200,
                {
                    "online": info.online,
                    "is_bootloader": info.is_bootloader,
                    "hardware_id": info.hardware_id,
                    "firmware_major": info.firmware_major,
                    "firmware_minor": info.firmware_minor,
                },
            )

        def do_POST(self):  # noqa: N802 - BaseHTTPRequestHandler requires this name
            parsed = urlparse(self.path)
            if parsed.path != "/flash":
                self._send_json(404, {"error": "not found"})
                return
            params = parse_qs(parsed.query)
            try:
                tier = int(params.get("tier", ["0"])[0])
                slot = int(params.get("slot", ["0"])[0])
                hardware_id = int(params.get("hardware_id", ["0"])[0], 0)
                version_major = int(params.get("version_major", ["0"])[0])
                version_minor = int(params.get("version_minor", ["0"])[0])
            except ValueError:
                self._send_json(400, {"error": "tier/slot/hardware_id/version_major/version_minor must be integers"})
                return

            try:
                content_length = int(self.headers.get("Content-Length", "0"))
            except ValueError:
                self._send_json(400, {"error": "Content-Length must be an integer"})
                return
            firmware = self.rfile.read(content_length)

            self.send_response(200)
            self.send_header("Content-Type", "application/x-ndjson")
            self.end_headers()
            effective_transport = RelayedTransport(transport, tier, slot) if params.get("relay", ["0"])[0] == "1" else transport
            flasher = SpiOtaFlasher(effective_transport, hmac_key)
            for progress in flasher.flash(tier, slot, hardware_id, firmware, version_major, version_minor):
                line = json.dumps(
                    {
                        "phase": progress.phase,
                        "pages_sent": progress.pages_sent,
                        "pages_total": progress.pages_total,
                        "percent": progress.percent,
                        "error": progress.error,
                    }
                )
                self.wfile.write((line + "\n").encode("utf-8"))
                self.wfile.flush()

        def _send_json(self, status: int, payload: dict) -> None:
            body = json.dumps(payload).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, format, *args):  # noqa: A002 - inherited API name
            """Quiet by default - a real deployment can override this."""

    return SpiBridgeHandler


def serve(transport: SpiOtaTransport, hmac_key: bytes, host: str = "127.0.0.1", port: int = 8765) -> ThreadingHTTPServer:
    """Starts the real local HTTP service. Caller owns the returned server's
    lifecycle (serve_forever()/shutdown())."""

    handler = make_handler(transport, hmac_key)
    return ThreadingHTTPServer((host, port), handler)
