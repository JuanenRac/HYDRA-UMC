# =============================================================================
# HYDRA-UMC - Local HTTP service exposing the real SPI-OTA bridge
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see repo root LICENSE
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
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

from .bootloader_client import SpiOtaFlasher, query_version
from .relay_tunnel import RelayedTransport
from .transport import SpiOtaTransport

# The largest real firmware image on this bus is the STM32H745's own 2 MB
# flash (the STM32G474 Robot Controller Board tops out at 512 KB) - 4 MiB
# is generous headroom above that while still refusing an oversized/bogus
# Content-Length before it is ever handed to rfile.read(), which otherwise
# blocks reading exactly that many bytes regardless of how large the value
# claimed is (real DoS/OOM risk from a malicious or malformed client).
MAX_FIRMWARE_BODY_BYTES = 4 * 1024 * 1024

# Valid targets of the SPI-OTA frame header: tier 0 = the Kinematic Brain's own
# CM4, 1 = its CM7, 2 = a Robot Controller Board on STACK A; 8 slots on STACK A.
VALID_TIERS = (0, 1, 2)
VALID_SLOTS = range(0, 8)


def make_handler(transport: SpiOtaTransport, hmac_key: bytes) -> type[BaseHTTPRequestHandler]:
    """Builds a request handler bound to one already-open transport - kept
    as a factory (not a module-level handler) so tests can inject a fake
    transport without a real SPI/GPIO link."""

    # One SPI link, one transaction at a time: a version query that slipped in
    # between two frames of a flash would be answered with (and would swallow)
    # the flash's own reply. Whoever finds the link busy is told so (409).
    bus_lock = threading.Lock()

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
            if tier not in VALID_TIERS or slot not in VALID_SLOTS:
                self._send_json(400, {"error": "tier must be 0, 1 or 2 and slot 0-7"})
                return
            effective_transport = RelayedTransport(transport, tier, slot) if params.get("relay", ["0"])[0] == "1" else transport
            if not bus_lock.acquire(blocking=False):
                self._send_json(409, {"error": "the SPI link is busy with another operation"})
                return
            try:
                info = query_version(effective_transport, tier, slot)
            except (OSError, ValueError) as error:
                self._send_json(502, {"error": f"SPI transfer failed: {error}"})
                return
            finally:
                bus_lock.release()
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
                version_major = int(params["version_major"][0])
                version_minor = int(params["version_minor"][0])
            except KeyError:
                # Without the version of the image the board would compare it as 0.0 and
                # refuse it as a rollback only AFTER the whole transfer.
                self._send_json(400, {"error": "version_major and version_minor are required"})
                return
            except ValueError:
                self._send_json(400, {"error": "tier/slot/hardware_id/version_major/version_minor must be integers"})
                return
            if tier not in VALID_TIERS or slot not in VALID_SLOTS:
                self._send_json(400, {"error": "tier must be 0, 1 or 2 and slot 0-7"})
                return

            try:
                content_length = int(self.headers.get("Content-Length", "0"))
            except ValueError:
                self._send_json(400, {"error": "Content-Length must be an integer"})
                return
            if content_length < 0 or content_length > MAX_FIRMWARE_BODY_BYTES:
                self._send_json(
                    413,
                    {"error": f"Content-Length exceeds the {MAX_FIRMWARE_BODY_BYTES} byte firmware body limit"},
                )
                return
            firmware = self.rfile.read(content_length)

            if not bus_lock.acquire(blocking=False):
                self._send_json(409, {"error": "the SPI link is busy with another operation"})
                return
            try:
                self.send_response(200)
                self.send_header("Content-Type", "application/x-ndjson")
                self.end_headers()
                effective_transport = RelayedTransport(transport, tier, slot) if params.get("relay", ["0"])[0] == "1" else transport
                flasher = SpiOtaFlasher(effective_transport, hmac_key)
                try:
                    for progress in flasher.flash(tier, slot, hardware_id, firmware, version_major, version_minor):
                        self._write_progress(progress.phase, progress.pages_sent, progress.pages_total, progress.percent, progress.error)
                except (OSError, ValueError) as error:
                    # The stream has started, so the failure has to be its last line.
                    self._write_progress("error", 0, 0, 0, f"SPI transfer failed: {error}")
            finally:
                bus_lock.release()

        def _write_progress(self, phase, pages_sent, pages_total, percent, error) -> None:
            line = json.dumps(
                {
                    "phase": phase,
                    "pages_sent": pages_sent,
                    "pages_total": pages_total,
                    "percent": percent,
                    "error": error,
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
