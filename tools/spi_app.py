#!/usr/bin/env python3
# =============================================================================
# HYDRA-UMC - tools/spi_app.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""Host twin of the SPI slave service of the application of the CM4 (src/mcu_stm32h745/CM4/spi_app.h, docs/SPI_SYSTEM.md).

The twin has the same state and takes the same decisions as the C header, frame by frame: the seal and the sequence of the link, which frames are
commands of the tier SELF, the reply that waits for the next transaction, and the requests the SPI task leaves for the STACK A task. The vectors of
tests/spi_app_vectors.json are produced by it and asserted by tests/test_spi_app.c as well, so the two cannot drift apart. `Cm5Link` is the other
end of the wire, as the bridge of the CM5 talks to it. Evidence of anything built on this file: simulator."""
from __future__ import annotations

from dataclasses import dataclass, field

import spi_frame
import spi_system

FRAME_SIZE = spi_frame.FRAME_SIZE
TARGET_SELF = 0
IDLE_TYPE = 0xFF


@dataclass
class SpiApp:
    config: spi_system.Config = field(default_factory=spi_system.Config)
    require_crc: bool = False
    tx_seq: int = 0
    rx_last: int | None = None
    crc_errors: int = 0
    retries: int = 0
    seq_gaps: int = 0
    unsealed: int = 0
    commands_run: int = 0
    frames_ignored: int = 0
    tx_wire: bytes = b""
    estop_latched: bool = True
    can_tx_errors: int = 0
    can_rx_errors: int = 0
    link_ok: bool = True
    trip_pending: bool = False
    trip_reason: int = 0
    release_pending: bool = False
    scan_pending: bool = False
    scan_id: int = 0
    scan_done: bool = False
    scan_done_id: int = 0
    scan_verdicts: list[int] = field(default_factory=lambda: [0] * 8)

    def __post_init__(self) -> None:
        self._seal(IDLE_TYPE, b"", 0)

    def _seal(self, frame_type: int, data: bytes, dlc: int) -> None:
        wire = bytearray(spi_frame.build(TARGET_SELF, 0, frame_type, data[:8]))
        wire[3] = dlc
        crc_input = None
        wire[spi_frame.OFS_MARKER] = spi_frame.MARKER
        wire[spi_frame.OFS_SEQ] = self.tx_seq & 0xFF
        crc_input = bytes(wire[: spi_frame.CRC_SPAN])
        crc = spi_frame.crc16(crc_input)
        wire[spi_frame.OFS_CRC], wire[spi_frame.OFS_CRC + 1] = crc >> 8, crc & 0xFF
        self.tx_seq = (self.tx_seq + 1) & 0xFF
        self.tx_wire = bytes(wire)

    def snapshot(self) -> spi_system.Snapshot:
        return spi_system.Snapshot(
            crc_errors=self.crc_errors,
            retries=self.retries,
            seq_gaps=self.seq_gaps,
            can_tx_errors=self.can_tx_errors,
            can_rx_errors=self.can_rx_errors,
            state=spi_system.STATE_APPLICATION,
            link_ok=self.link_ok,
            estop_latched=self.estop_latched,
            scan_verdicts=list(self.scan_verdicts),
        )

    def on_receive(self, wire: bytes) -> bool:
        """One frame from the CM5: True when it was a command this service ran."""
        verdict = spi_frame.judge(wire, self.rx_last)
        if verdict == "bad_crc":
            self.crc_errors += 1
            return False
        if verdict == "duplicate":
            self.retries += 1
            return False
        if verdict == "unsealed":
            self.unsealed += 1
            if self.require_crc:
                return False
        else:
            sequence = wire[spi_frame.OFS_SEQ]
            if self.rx_last is not None and ((sequence - self.rx_last) & 0xFF) != 1:
                self.seq_gaps += 1
            self.rx_last = sequence
        tier, frame_type, dlc = wire[0], wire[2], wire[3]
        is_command = spi_system.STOP_REQUEST <= frame_type <= spi_system.PROBE_REQUEST and frame_type != spi_system.TELEMETRY_RESPONSE
        if tier != TARGET_SELF or not is_command or dlc > 8:
            self.frames_ignored += 1
            return False
        action, data = spi_system.handle(self.config, self.snapshot(), frame_type, bytes(wire[4 : 4 + dlc]))
        if action == "reply":
            self._seal(spi_system.TELEMETRY_RESPONSE, data, len(data))
        elif action == "trip":
            self.trip_reason, self.trip_pending = data[0], True
        elif action == "release":
            self.release_pending = True
        elif action == "scan":
            self.scan_id, self.scan_done, self.scan_pending = data[0], False, True
        else:
            self.frames_ignored += 1
            return False
        self.commands_run += 1
        return True

    def service(self) -> None:
        """Between transactions: a finished scan becomes the answer of the probe."""
        if self.scan_done:
            self.scan_done = False
            answer = spi_system.probe_response(self.snapshot(), self.scan_done_id)
            self._seal(spi_system.PROBE_RESPONSE, answer, 5)

    # the STACK A task side
    def take_trip(self) -> int | None:
        if not self.trip_pending:
            return None
        self.trip_pending = False
        return self.trip_reason

    def take_release(self) -> bool:
        taken, self.release_pending = self.release_pending, False
        return taken

    def take_scan(self) -> int | None:
        if not self.scan_pending:
            return None
        self.scan_pending = False
        return self.scan_id

    def scan_finished(self, scan_id: int, verdicts: list[int]) -> None:
        self.scan_verdicts = list(verdicts)
        self.scan_done_id, self.scan_done = scan_id, True

    def publish_brain_state(self, estop_latched: bool, can_tx_errors: int, can_rx_errors: int, link_ok: bool = True) -> None:
        self.estop_latched, self.can_tx_errors, self.can_rx_errors, self.link_ok = estop_latched, can_tx_errors, can_rx_errors, link_ok


class Cm5Link:
    """The CM5 end: every transaction sends one sealed frame and receives what the firmware staged before it. A request therefore needs a second
    transaction (an idle frame the firmware ignores) to read its answer, exactly as the bridge does over the real spidev."""

    def __init__(self, app: SpiApp, pump=None):
        self.app = app
        self.sequence = 0
        self.pump = pump  # called between the two transactions: the STACK A task and the SPI task running

    def transaction(self, frame_type: int, payload: bytes = b"", tier: int = TARGET_SELF) -> bytes:
        self.sequence = (self.sequence + 1) & 0xFF
        sent = spi_frame.build(tier, 0, frame_type, payload, self.sequence)
        received = self.app.tx_wire
        self.app.on_receive(sent)
        self.app.service()
        return received

    def exchange(self, frame_type: int, payload: bytes) -> tuple[int, bytes]:
        self.transaction(frame_type, payload)
        if self.pump is not None:
            self.pump()
        answer = self.transaction(IDLE_TYPE)
        if spi_frame.check(answer) != "ok" or answer[2] == IDLE_TYPE:
            return 0, b""
        return answer[2], bytes(answer[4 : 4 + answer[3]])
