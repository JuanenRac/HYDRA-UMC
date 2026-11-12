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
  GET  /counters?tier=&slot=&relay=  -> query_error_counters(): the transmit and receive error counters of the CAN controller of a board
  POST /fram/erase?slot=&confirm=ERASE    -> erase_fram(): zeros over the whole F-RAM of the controller board of that slot (docs/FRAM_ERASE.md)
  POST /fram/selftest?slot=               -> fram_selftest(): patterns over a scratch area of the F-RAM, the old bytes put back
                                        Both answer 403 unless the service was started with fram_erase=True (--enable-fram-erase); `GET /version`
                                        says so in `capabilities.fram_erase`, so an app offers the operation only when the bridge does. The board
                                        has its own switch (HYDRA_FRAM_ERASE) and no driver for the chip yet: it then answers `disabled` or `no-chip`.
  POST /flash?tier=&slot=&hardware_id=&relay=&version_major=&version_minor=[&version_patch=][&downgrade_from=x.y.z][&signature=HEX][&target=NAME][&min_bootloader=x.y.z] -> SpiOtaFlasher.flash(),
                                        streamed as newline-delimited JSON. The HYDRA-UMC bootloaders verify an Ed25519 signature of the image
                                        (128 hexadecimal digits in `signature`, made with tools/image_header.py sign on the machine that holds the
                                        private seed): this service holds no signing key. A downgrade needs a signer of the token of the session,
                                        which only a deployment that provides one (serve(downgrade_signer=...)) has. `target` and `min_bootloader`
                                        are the fields of the same names of the update manifest, passed on by the caller that holds it: a target
                                        that is not the board is refused (400) before the image is read, and a bootloader older than
                                        `min_bootloader` (or one whose version cannot be read) ends the stream with an error before any image
                                        byte is sent. Without them nothing is checked.

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

from .fram_client import erase_fram, fram_selftest
from .bootloader_client import SpiOtaFlasher, check_manifest_requirements, query_error_counters, query_version
from .image_header import DowngradeSigner, ImageSigner, SIGNATURE_SIZE, is_signed_header_target
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


def make_handler(
    transport: SpiOtaTransport,
    hmac_key: bytes,
    bus_lock: threading.Lock | None = None,
    *,
    image_signer: ImageSigner | None = None,
    downgrade_signer: DowngradeSigner | None = None,
    fram_erase: bool = False,
) -> type[BaseHTTPRequestHandler]:
    """Builds a request handler bound to one already-open transport - kept
    as a factory (not a module-level handler) so tests can inject a fake
    transport without a real SPI/GPIO link."""

    # One SPI link, one transaction at a time: a version query that slipped in
    # between two frames of a flash would be answered with (and would swallow)
    # the flash's own reply. Whoever finds the link busy is told so (409).
    # (Shared with whatever else reads the link, such as the publisher of the error counters.)
    if bus_lock is None:
        bus_lock = threading.Lock()
    lock = bus_lock

    class SpiBridgeHandler(BaseHTTPRequestHandler):
        def do_GET(self):  # noqa: N802 - BaseHTTPRequestHandler requires this name
            parsed = urlparse(self.path)
            if parsed.path not in ("/version", "/counters"):
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
            if not lock.acquire(blocking=False):
                self._send_json(409, {"error": "the SPI link is busy with another operation"})
                return
            try:
                if parsed.path == "/counters":
                    counters = query_error_counters(effective_transport, tier, slot)
                else:
                    info = query_version(effective_transport, tier, slot)
            except (OSError, ValueError) as error:
                self._send_json(502, {"error": f"SPI transfer failed: {error}"})
                return
            finally:
                lock.release()
            if parsed.path == "/counters":
                self._send_json(200, {"online": counters.online, "tec": counters.tec, "rec": counters.rec})
                return
            self._send_json(
                200,
                {
                    "online": info.online,
                    "is_bootloader": info.is_bootloader,
                    "hardware_id": info.hardware_id,
                    "firmware_major": info.firmware_major,
                    "firmware_minor": info.firmware_minor,
                    "capabilities": {"fram_erase": fram_erase},
                },
            )

        def do_POST(self):  # noqa: N802 - BaseHTTPRequestHandler requires this name
            parsed = urlparse(self.path)
            if parsed.path in ("/fram/erase", "/fram/selftest"):
                self._fram(parsed)
                return
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
                version_patch = int(params.get("version_patch", ["0"])[0])
                installed = params.get("downgrade_from", [""])[0]
                downgrade_from = tuple(int(part) for part in installed.split(".")) if installed else None
                if downgrade_from is not None and len(downgrade_from) != 3:
                    raise ValueError("downgrade_from must be major.minor.patch")
                target = params.get("target", [""])[0] or None
                min_bootloader = params.get("min_bootloader", [""])[0] or None
                signature_hex = params.get("signature", [""])[0]
                signature = bytes.fromhex(signature_hex) if signature_hex else None
                if signature is not None and len(signature) != SIGNATURE_SIZE:
                    raise ValueError("signature must be 128 hexadecimal digits")
            except KeyError:
                # Without the version of the image the board would compare it as 0.0 and
                # refuse it as a rollback only AFTER the whole transfer.
                self._send_json(400, {"error": "version_major and version_minor are required"})
                return
            except ValueError:
                self._send_json(400, {"error": "tier/slot/hardware_id/version_major/version_minor/version_patch must be integers, downgrade_from major.minor.patch and signature 128 hexadecimal digits"})
                return
            if tier not in VALID_TIERS or slot not in VALID_SLOTS:
                self._send_json(400, {"error": "tier must be 0, 1 or 2 and slot 0-7"})
                return
            refusal = check_manifest_requirements(hardware_id, min_bootloader, target)
            if refusal is not None:
                self._send_json(400, {"error": refusal})
                return
            if is_signed_header_target(hardware_id):
                # Said before the body is read and before the bootloader is touched: nothing the board would refuse after the whole transfer.
                if signature is None and image_signer is None:
                    self._send_json(400, {"error": "a HYDRA-UMC board needs the Ed25519 signature of the image (signature=...): this service holds no signing key"})
                    return
                if downgrade_from is not None and downgrade_signer is None:
                    self._send_json(400, {"error": "a downgrade of a HYDRA-UMC board needs a signer for the token of the session, which this service does not have"})
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

            if not lock.acquire(blocking=False):
                self._send_json(409, {"error": "the SPI link is busy with another operation"})
                return
            try:
                self.send_response(200)
                self.send_header("Content-Type", "application/x-ndjson")
                self.end_headers()
                effective_transport = RelayedTransport(transport, tier, slot) if params.get("relay", ["0"])[0] == "1" else transport
                flasher = SpiOtaFlasher(effective_transport, hmac_key, image_signer=image_signer, downgrade_signer=downgrade_signer)
                try:
                    for progress in flasher.flash(
                        tier, slot, hardware_id, firmware, version_major, version_minor,
                        version_patch=version_patch, allow_downgrade=downgrade_from is not None, downgrade_from=downgrade_from,
                        signature=signature, min_bootloader=min_bootloader, target=target,
                    ):
                        self._write_progress(progress.phase, progress.pages_sent, progress.pages_total, progress.percent, progress.error)
                except (OSError, ValueError) as error:
                    # The stream has started, so the failure has to be its last line.
                    self._write_progress("error", 0, 0, 0, f"SPI transfer failed: {error}")
            finally:
                lock.release()

        def _fram(self, parsed) -> None:
            """The F-RAM erase and self-test: off unless the service was started for it, and an erase needs the word `ERASE` as a confirmation."""
            if not fram_erase:
                self._send_json(403, {"error": "the F-RAM operations are switched off in this service (start it with --enable-fram-erase)"})
                return
            params = parse_qs(parsed.query)
            try:
                slot = int(params["slot"][0])
            except (KeyError, ValueError):
                self._send_json(400, {"error": "slot is required and must be an integer"})
                return
            if slot not in VALID_SLOTS:
                self._send_json(400, {"error": "slot must be 0-7"})
                return
            is_erase = parsed.path == "/fram/erase"
            if is_erase and params.get("confirm", [""])[0] != "ERASE":
                self._send_json(400, {"error": "an erase needs confirm=ERASE"})
                return
            if not lock.acquire(blocking=False):
                self._send_json(409, {"error": "the SPI link is busy with another operation"})
                return
            try:
                result = (erase_fram if is_erase else fram_selftest)(transport, slot)
            finally:
                lock.release()
            if result is None:
                self._send_json(504, {"error": "the board did not answer (absent, or not in its bootloader)"})
                return
            self._send_json(200, result.as_json())

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


def serve(
    transport: SpiOtaTransport,
    hmac_key: bytes,
    host: str = "127.0.0.1",
    port: int = 8765,
    bus_lock: threading.Lock | None = None,
    *,
    image_signer: ImageSigner | None = None,
    downgrade_signer: DowngradeSigner | None = None,
    fram_erase: bool = False,
) -> ThreadingHTTPServer:
    """Starts the real local HTTP service. Caller owns the returned server's
    lifecycle (serve_forever()/shutdown()). Pass the same `bus_lock` to
    anything else that talks to the link (the counter publisher). The signers are for a deployment that keeps the
    private seed behind some other process; without them every HYDRA-UMC update must arrive already signed."""

    handler = make_handler(transport, hmac_key, bus_lock, image_signer=image_signer, downgrade_signer=downgrade_signer, fram_erase=fram_erase)
    return ThreadingHTTPServer((host, port), handler)
