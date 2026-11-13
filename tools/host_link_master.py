#!/usr/bin/env python3
# =============================================================================
# HYDRA-UMC - tools/host_link_master.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""Reference of the transport of the host link with the board as SPI master (the C twin is src/mcu_stm32h757/Common/host_link_master.h; docs/HOST_LINK_MASTER.md).

On the H757 board the Jetson is the host and the board clocks the link. The frame is the sealed 128-byte frame of the slave link (tools/spi_frame.py), unchanged. The board runs
ONE transaction (one frame out, one frame in) when it has a frame staged, when the READY line of the host is high, or when the idle interval has passed without any; two
transactions are never closer than the minimum gap. The request of the host arrives in the same transaction in which the board sends its last answer or an idle frame, and the
answer to it goes out in the next one.

Rules the model keeps:
  - a frame in that is damaged (CRC) or, with the seal required, unsealed, is NOT dispatched; the frame that went out is sent again with the same sequence up to `max_retries`
    times and then dropped and counted, so a noisy link costs frames and never runs a wrong command;
  - a repeated sequence of the host is a retry of a frame already handled: it is counted and not dispatched twice;
  - a frame of 0xFF in every byte is the line floating with no host behind it: silence, never counted as an error; a staged frame stays and is offered again at the idle interval;
  - a failure of the peripheral keeps the staged frame and counts; `down_after_failures` in a row mark the link down, the first transaction that works marks it up again;
  - the link never carries the emergency stop (that is the CAN bus): it going down changes nothing about the stop.
Evidence: simulator. Nothing here has run on a board."""
from __future__ import annotations

import configparser
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable

import spi_frame as sf

ROOT = Path(__file__).resolve().parents[1]
CFG_PATH = ROOT / "config" / "host_link.cfg"
MASK = 0xFFFFFFFF
IDLE_TYPE = 0xFF

# what one call of poll() did
POLL_NOT_DUE = 0  # two transactions are never closer than the minimum gap
POLL_NOTHING = 1  # nothing staged, READY low, idle interval not over
POLL_DONE = 2  # a transaction ran
POLL_XFER_FAIL = 3  # the peripheral failed; the staged frame is kept

# what the board made of the frame that came in
RX_NONE = 0
RX_DISPATCH = 1
RX_DUPLICATE = 2
RX_BAD_CRC = 3
RX_UNSEALED = 4
RX_IDLE = 5
RX_SILENCE = 6

SILENCE = b"\xff" * sf.FRAME_SIZE


def load_config(path: Path = CFG_PATH) -> dict:
    parser = configparser.ConfigParser(inline_comment_prefixes=None)
    parser.read_string(path.read_text(encoding="utf-8"))
    section = parser["link"]
    cfg = {name: int(section[name]) for name in ("idle_interval_ms", "min_gap_ms", "max_retries", "down_after_failures", "require_seal", "ready_known")}
    if not 1 <= cfg["idle_interval_ms"] <= 10000 or not 0 <= cfg["min_gap_ms"] <= 1000 or cfg["min_gap_ms"] > cfg["idle_interval_ms"]:
        raise ValueError("config/host_link.cfg: idle_interval_ms 1..10000, min_gap_ms 0..1000 and not above the idle interval")
    if not 0 <= cfg["max_retries"] <= 10 or not 1 <= cfg["down_after_failures"] <= 100:
        raise ValueError("config/host_link.cfg: max_retries 0..10 and down_after_failures 1..100")
    if cfg["require_seal"] not in (0, 1) or cfg["ready_known"] not in (0, 1):
        raise ValueError("config/host_link.cfg: require_seal and ready_known are 0 or 1")
    return cfg


@dataclass
class Counters:
    transactions: int = 0
    dispatched: int = 0
    duplicates: int = 0
    bad_crc: int = 0
    unsealed: int = 0
    idle_rx: int = 0
    silence: int = 0
    transfer_failures: int = 0
    retries: int = 0
    dropped: int = 0


@dataclass
class HostLinkMaster:
    cfg: dict
    transfer: Callable[[bytes], bytes | None]  # sends 128 bytes, returns the 128 that came in, None when the peripheral failed
    ready: Callable[[], bool]
    dispatch: Callable[[bytes], bytes | None]  # runs a request, returns the answer frame (first 13 bytes, unsealed) or None
    counters: Counters = field(default_factory=Counters)
    staged: bytes | None = None
    staged_seq: int = 0
    attempts: int = 0
    tx_seq: int = 0
    have_last: bool = False
    last_rx_seq: int = 0
    last_time: int = 0
    ran_once: bool = False
    failures_in_a_row: int = 0
    down: bool = False
    last_rx_verdict: int = RX_NONE
    last_tx_type: int = 0
    last_tx_seq: int = 0

    def stage(self, frame: bytes) -> bool:
        """Puts one frame (the 12 bytes of the frame; the seal is written at the transaction) on the line. False when the slot is taken."""
        if self.staged is not None:
            return False
        self.staged = bytes(frame[:12]).ljust(12, b"\xff")
        self.tx_seq = (self.tx_seq + 1) & 0xFF
        self.staged_seq = self.tx_seq
        self.attempts = 0
        return True

    def _due(self, now: int) -> bool:
        return not self.ran_once or ((now - self.last_time) & MASK) >= self.cfg["min_gap_ms"]

    def poll(self, now: int) -> int:
        if not self._due(now):
            return POLL_NOT_DUE
        idle_over = not self.ran_once or ((now - self.last_time) & MASK) >= self.cfg["idle_interval_ms"]
        # a staged frame does not make the board clock a line that answered with silence at the full rate: it waits for the idle interval or for READY
        wants = (self.staged is not None and self.last_rx_verdict != RX_SILENCE) or (self.cfg["ready_known"] and self.ready()) or idle_over
        if not wants:
            return POLL_NOTHING
        if self.staged is not None:
            seq = self.staged_seq
            wire = sf.build(self.staged[0], self.staged[1], self.staged[2], self.staged[4 : 4 + min(self.staged[3], 8)], sequence=seq)
        else:
            self.tx_seq = (self.tx_seq + 1) & 0xFF
            seq = self.tx_seq
            wire = sf.build(0, 0, IDLE_TYPE, b"", sequence=seq)
        self.last_time, self.ran_once = now, True
        self.last_tx_type, self.last_tx_seq = wire[2], seq
        incoming = self.transfer(wire)
        if incoming is None or len(incoming) != sf.FRAME_SIZE:
            self.counters.transfer_failures += 1
            self.failures_in_a_row += 1
            if self.failures_in_a_row >= self.cfg["down_after_failures"]:
                self.down = True
            self.last_rx_verdict = RX_NONE
            return POLL_XFER_FAIL
        self.failures_in_a_row, self.down = 0, False
        self.counters.transactions += 1
        self.last_rx_verdict = self._take(incoming)
        return POLL_DONE

    def _take(self, wire: bytes) -> int:
        verdict, damaged = RX_NONE, False
        if wire == SILENCE:
            self.counters.silence += 1
            verdict = RX_SILENCE
        else:
            state = sf.check(wire)
            if state == "bad_crc":
                self.counters.bad_crc += 1
                verdict, damaged = RX_BAD_CRC, True
            elif state == "unsealed" and self.cfg["require_seal"]:
                self.counters.unsealed += 1
                verdict, damaged = RX_UNSEALED, True
            elif self.have_last and wire[sf.OFS_SEQ] == self.last_rx_seq and state == "ok":
                self.counters.duplicates += 1
                verdict = RX_DUPLICATE
            else:
                if state == "ok":
                    self.have_last, self.last_rx_seq = True, wire[sf.OFS_SEQ]
                if wire[2] == IDLE_TYPE:
                    self.counters.idle_rx += 1
                    verdict = RX_IDLE
                else:
                    self.counters.dispatched += 1
                    verdict = RX_DISPATCH
        if damaged and self.staged is not None:
            self.attempts += 1
            if self.attempts > self.cfg["max_retries"]:
                self.staged = None
                self.counters.dropped += 1
            else:
                self.counters.retries += 1
        elif self.staged is not None and verdict != RX_SILENCE:
            self.staged = None  # a good frame came back in the transaction that carried ours; with silence nobody took it, so it stays
        if verdict == RX_DISPATCH:
            answer = self.dispatch(wire)
            if answer is not None:
                self.stage(answer)
        return verdict
