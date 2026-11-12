# =============================================================================
# HYDRA-UMC - spi_bridge system_client: the commands of the SPI link that the CM4 answers itself
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see repo root LICENSE
# =============================================================================
"""Stop, release, telemetry and probe over the SPI link (docs/SPI_SYSTEM.md; the firmware side is src/mcu_stm32h757/CM4/spi_system.h and spi_app.h).

The frames are of the tier SELF and never reach a CAN bus. The firmware answers them only when it is built with the switches on, so every call here
reports "no answer" instead of inventing a value, and a call that acts on the stack (stop, release) says only that the frame was sent: whether the
brain acted is read back from the telemetry flags. SPI moves data only when the master clocks it, so the answer to a request waits for the next
transaction: ``_exchange`` sends the request and then an idle frame that the firmware ignores, and reads the answer from the second one."""

from __future__ import annotations

from dataclasses import dataclass

from .protocol import SPI_TARGET_SELF, SpiOtaFrame
from .transport import SpiOtaTransport

STOP_REQUEST = 0x20
RELEASE_REQUEST = 0x21
TELEMETRY_QUERY = 0x22
TELEMETRY_RESPONSE = 0x23
PROBE_REQUEST = 0x24
PROBE_RESPONSE = 0x25
ZONES_HEARTBEAT = 0x26
IDLE_FRAME = 0xFF  # the frame that asks for nothing: the firmware ignores it and the transaction brings the staged answer

STATE_BOOTLOADER = 0x01
STATE_APPLICATION = 0x02
FLAG_LINK_OK = 0x01
FLAG_ESTOP_LATCHED = 0x02
FLAG_STOP_ENABLED = 0x04

VERDICT_NAMES = {0: "absent", 1: "ok", 2: "duplicate", 3: "misplaced", 4: "noisy"}


@dataclass(frozen=True)
class Telemetry:
    link_ok: bool
    estop_latched: bool
    stop_enabled: bool
    crc_errors: int  # the counters saturate at 255
    retries: int
    seq_gaps: int
    can_tx_errors: int
    can_rx_errors: int
    state: int  # STATE_BOOTLOADER or STATE_APPLICATION

    @property
    def in_application(self) -> bool:
        return self.state == STATE_APPLICATION


@dataclass(frozen=True)
class ProbeResult:
    scan_id: int
    verdicts: tuple[str, ...]  # the slots 0 to 7: absent, ok, duplicate, misplaced, noisy (unknown for a nibble outside the table)


def _exchange(transport: SpiOtaTransport, frame_type: int, payload: bytes) -> SpiOtaFrame | None:
    """The answer frame of one request, or None when the link failed or the firmware had nothing to say (switches off, a frame it does not know)."""
    try:
        transport.transceive(SpiOtaFrame(SPI_TARGET_SELF, 0, frame_type, len(payload), payload))
        answer = transport.transceive(SpiOtaFrame(SPI_TARGET_SELF, 0, IDLE_FRAME, 0))
    except (OSError, ValueError):
        return None
    return answer


def decode_telemetry(frame: SpiOtaFrame) -> Telemetry | None:
    if frame.frame_type != TELEMETRY_RESPONSE or frame.dlc != 8 or len(frame.payload) < 8 or frame.payload[0] != 0:
        return None
    data = frame.payload
    return Telemetry(bool(data[1] & FLAG_LINK_OK), bool(data[1] & FLAG_ESTOP_LATCHED), bool(data[1] & FLAG_STOP_ENABLED), data[2], data[3], data[4], data[5], data[6], data[7])


def decode_probe(frame: SpiOtaFrame) -> ProbeResult | None:
    if frame.frame_type != PROBE_RESPONSE or frame.dlc != 5 or len(frame.payload) < 5:
        return None
    verdicts: list[str] = []
    for byte in frame.payload[1:5]:
        verdicts += [VERDICT_NAMES.get(byte & 0x0F, "unknown"), VERDICT_NAMES.get(byte >> 4, "unknown")]
    return ProbeResult(frame.payload[0], tuple(verdicts))


def read_telemetry(transport: SpiOtaTransport) -> Telemetry | None:
    """The counters and flags of the CM4 (bootloader or application), or None when it did not answer (switch off, link down, another kind of frame)."""
    answer = _exchange(transport, TELEMETRY_QUERY, b"\x00")
    return None if answer is None else decode_telemetry(answer)


def probe_slots(transport: SpiOtaTransport, scan_id: int, attempts: int = 8) -> ProbeResult | None:
    """Asks the application to scan the eight slots and polls for the answer. The scan takes the application a moment (it asks every slot), so the answer
    is read with idle frames, up to ``attempts`` of them; None when the request failed or the answer did not come (the probe is off in this build)."""
    try:
        transport.transceive(SpiOtaFrame(SPI_TARGET_SELF, 0, PROBE_REQUEST, 1, bytes((scan_id & 0xFF,))))
    except (OSError, ValueError):
        return None
    return poll_probe(transport, scan_id, attempts)


def poll_probe(transport: SpiOtaTransport, scan_id: int, attempts: int = 8) -> ProbeResult | None:
    """Reads the answer of a scan that was requested before, polling with idle frames. Returns None when the answer did not come in ``attempts`` polls."""
    for _ in range(max(1, attempts)):
        try:
            frame = transport.transceive(SpiOtaFrame(SPI_TARGET_SELF, 0, IDLE_FRAME, 0))
        except (OSError, ValueError):
            return None
        result = decode_probe(frame)
        if result is not None and result.scan_id == scan_id & 0xFF:
            return result
    return None


def request_stop(transport: SpiOtaTransport, reason: int) -> bool:
    """Asks the brain to put the whole stack in the emergency stop. True only says that the frame was sent: read the telemetry to see whether the brain
    acted (the firmware ignores it unless it was built with the stop commands on)."""
    try:
        transport.transceive(SpiOtaFrame(SPI_TARGET_SELF, 0, STOP_REQUEST, 5, b"STOP" + bytes((reason & 0xFF,))))
    except (OSError, ValueError):
        return False
    return True


def send_zones_heartbeat(transport: SpiOtaTransport) -> bool:
    """Says to the brain that the safety-zones chain is healthy. Send it every 100 ms, and only while the last verdict of the service is READY and fresh: a brain
    built with the heartbeat watchdog stops the stack when it stops coming. True only says that the frame was sent; the brain does not answer it."""
    try:
        transport.transceive(SpiOtaFrame(SPI_TARGET_SELF, 0, ZONES_HEARTBEAT, 4, b"ZONE"))
    except (OSError, ValueError):
        return False
    return True


def request_release(transport: SpiOtaTransport) -> bool:
    """Tells the brain that a person asked to end the stop. The brain still waits for every expected board to say READY. True only says that the frame was sent."""
    try:
        transport.transceive(SpiOtaFrame(SPI_TARGET_SELF, 0, RELEASE_REQUEST, 4, b"RELS"))
    except (OSError, ValueError):
        return False
    return True
