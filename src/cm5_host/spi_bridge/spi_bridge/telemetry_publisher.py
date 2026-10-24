# =============================================================================
# HYDRA-UMC - Publishes the CAN error counters of the boards to the telemetry collector
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see repo root LICENSE
# =============================================================================
"""Reads the transmit and receive error counters of the CAN controller of each configured board and sends them to
HYDRA-UMC-TELEMETRY-COLLECTOR (`POST /ingest/ws`) as one sample per board and round, so the history of the bus (a counter that
climbs before a board goes error passive or bus off) reaches the data lake.

Nothing is invented: a board that does not answer produces no sample, only a count of the rounds it missed. The SPI link carries
one operation at a time, so the reading waits for the lock the HTTP service uses and gives up on a round when a flash holds it. The
collector's token comes from a file or the environment, never from the command line.
"""

from __future__ import annotations

import json
import threading
import time
import urllib.error
import urllib.request
from dataclasses import dataclass, field

from .bootloader_client import query_error_counters
from .relay_tunnel import RelayedTransport
from .transport import SpiOtaTransport

KIND = "can_error_counters"
MIN_INTERVAL_SECONDS = 1.0
REQUEST_TIMEOUT_SECONDS = 5.0


@dataclass(frozen=True)
class Target:
    """A board whose counters are read: where it is reachable and whether through the relay tunnel of its controller board."""

    tier: int
    slot: int
    relay: bool = False

    @property
    def source_id(self) -> str:
        return f"canbus-tier{self.tier}-slot{self.slot}" + ("-relay" if self.relay else "")


def parse_target(text: str) -> Target:
    """`tier:slot` or `tier:slot:relay`, for example `2:3` or `2:3:relay`."""

    parts = text.split(":")
    if len(parts) not in (2, 3) or (len(parts) == 3 and parts[2] != "relay"):
        raise ValueError(f"{text!r} is not tier:slot or tier:slot:relay")
    tier, slot = int(parts[0]), int(parts[1])
    if tier not in (0, 1, 2) or not 0 <= slot <= 7:
        raise ValueError("tier must be 0, 1 or 2 and slot 0 to 7")
    return Target(tier, slot, len(parts) == 3)


@dataclass
class PublisherStats:
    rounds: int = 0
    sent: int = 0
    missed: int = 0  # a board that did not answer
    skipped_busy: int = 0  # the link was busy with another operation
    delivery_failures: int = 0
    last_error: str = ""
    last_counters: dict[str, dict[str, int]] = field(default_factory=dict)


class CounterPublisher:
    def __init__(
        self,
        transport: SpiOtaTransport,
        bus_lock: threading.Lock,
        targets: list[Target],
        collector_url: str,
        token: str | None = None,
        interval_seconds: float = 10.0,
        clock=time.time,
    ) -> None:
        if not targets:
            raise ValueError("at least one target is needed")
        if interval_seconds < MIN_INTERVAL_SECONDS:
            raise ValueError(f"the interval must be at least {MIN_INTERVAL_SECONDS} second")
        if not collector_url.startswith(("http://", "https://")):
            raise ValueError("the collector address must start with http:// or https://")
        self._transport = transport
        self._lock = bus_lock
        self._targets = list(targets)
        self._url = collector_url.rstrip("/") + "/ingest/ws"
        self._token = token
        self._interval = interval_seconds
        self._clock = clock
        self._sequence = 0
        self.stats = PublisherStats()

    def _sample(self, target: Target, tec: int, rec: int) -> dict:
        self._sequence += 1
        return {
            "sourceId": target.source_id,
            "kind": KIND,
            "timestamp": int(self._clock() * 1000),
            "fields": {"tec": float(tec), "rec": float(rec)},
            "sequence": self._sequence,
        }

    def _post(self, sample: dict) -> None:
        request = urllib.request.Request(
            self._url,
            data=json.dumps(sample).encode("utf-8"),
            method="POST",
            headers={"Content-Type": "application/json"},
        )
        if self._token:
            request.add_header("Authorization", f"Bearer {self._token}")
        with urllib.request.urlopen(request, timeout=REQUEST_TIMEOUT_SECONDS) as response:
            response.read()

    def poll_once(self) -> int:
        """One round over all the targets. Returns how many samples reached the collector."""

        self.stats.rounds += 1
        delivered = 0
        for target in self._targets:
            if not self._lock.acquire(blocking=False):
                self.stats.skipped_busy += 1
                continue
            try:
                transport = RelayedTransport(self._transport, target.tier, target.slot) if target.relay else self._transport
                counters = query_error_counters(transport, target.tier, target.slot)
            finally:
                self._lock.release()
            if not counters.online:
                self.stats.missed += 1
                continue
            self.stats.last_counters[target.source_id] = {"tec": counters.tec, "rec": counters.rec}
            try:
                self._post(self._sample(target, counters.tec, counters.rec))
            except (urllib.error.URLError, OSError, ValueError) as error:
                self.stats.delivery_failures += 1
                self.stats.last_error = str(error)
                continue
            delivered += 1
            self.stats.sent += 1
        return delivered

    def run(self, stop: threading.Event) -> None:
        while not stop.is_set():
            self.poll_once()
            stop.wait(self._interval)

    def start(self) -> tuple[threading.Thread, threading.Event]:
        stop = threading.Event()
        thread = threading.Thread(target=self.run, args=(stop,), name="can-counters", daemon=True)
        thread.start()
        return thread, stop
