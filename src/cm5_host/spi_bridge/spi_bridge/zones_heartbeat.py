# =============================================================================
# HYDRA-UMC - spi_bridge zones_heartbeat: the "safety-zones chain is healthy" heartbeat to the brain
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see repo root LICENSE
# =============================================================================
"""Sends the heartbeat of the safety-zones chain (docs/SPI_SYSTEM.md, type 0x26) to the brain, and only while the chain is healthy.

The service of the safety zones answers ``GET /state`` with the verdict it last computed. This sender asks for it every ``period_seconds`` and sends the
heartbeat over the SPI link ONLY when the verdict is ``READY`` and not older than ``max_age_seconds``. In every other case it sends nothing: a service that
is down, frozen, inhibited, in a safe stop, that answers something that is not a verdict, or whose clock stopped, is a heartbeat that does not come, and a
brain built with ``HYDRA_SPI_ZONES_HEARTBEAT=1`` stops the stack when a heartbeat that had come stops coming. The sender never asks the brain to stop and
never sends a heartbeat to make up for a missing one. Off unless the service is started with ``--zones-state-url``."""

from __future__ import annotations

import json
import threading
import urllib.error
import urllib.request
from collections.abc import Callable
from dataclasses import dataclass
from datetime import UTC, datetime
from typing import Any

from . import system_client
from .transport import SpiOtaTransport

ZONES_STATE_ROUTE = "/state"
MAX_STATE_BYTES = 64 * 1024


def parse_utc(text: object) -> datetime | None:
    """An ISO 8601 time with a zone (``Z`` or an offset), or None for anything else (a time without a zone is not trusted)."""
    if not isinstance(text, str):
        return None
    try:
        moment = datetime.fromisoformat(text.replace("Z", "+00:00"))
    except ValueError:
        return None
    return moment if moment.tzinfo is not None else None


def state_is_healthy(document: Any, now: datetime, max_age_seconds: float, future_seconds: float = 2.0) -> bool:
    """True only for a verdict that says READY, carries a time, and is neither older than ``max_age_seconds`` nor more than ``future_seconds`` ahead of
    the clock of this machine (a verdict from the future is a clock that is wrong, not a fresh one)."""
    if not isinstance(document, dict):
        return False
    state = document.get("sdkSafetyState")
    if not isinstance(state, dict) or state.get("state") != "READY":
        return False
    moment = parse_utc(state.get("timestamp_utc"))
    if moment is None:
        return False
    age = (now - moment).total_seconds()
    return -future_seconds <= age <= max_age_seconds


def fetch_state(base_url: str, token: str | None, timeout_seconds: float) -> Any:
    """The verdict of the service as JSON, or None when it could not be read (any failure: nothing is invented)."""
    request = urllib.request.Request(base_url.rstrip("/") + ZONES_STATE_ROUTE)
    if token:
        request.add_header("Authorization", f"Bearer {token}")
    try:
        with urllib.request.urlopen(request, timeout=timeout_seconds) as response:
            raw = response.read(MAX_STATE_BYTES + 1)
    except urllib.error.HTTPError as error:
        error.close()
        return None
    except (urllib.error.URLError, OSError, ValueError):
        return None
    if len(raw) > MAX_STATE_BYTES:
        return None
    try:
        return json.loads(raw)
    except ValueError:
        return None


@dataclass
class HeartbeatStats:
    sent: int = 0
    withheld: int = 0  # passes in which the chain was not healthy, so nothing was sent
    link_failures: int = 0


class ZonesHeartbeat:
    def __init__(
        self,
        transport: SpiOtaTransport,
        bus_lock: threading.Lock,
        fetch: Callable[[], Any],
        *,
        period_seconds: float = 0.1,
        max_age_seconds: float = 1.0,
        clock: Callable[[], datetime] | None = None,
    ):
        if period_seconds <= 0 or max_age_seconds <= 0:
            raise ValueError("the period and the maximum age must be above zero")
        self._transport = transport
        self._lock = bus_lock
        self._fetch = fetch
        self.period_seconds = period_seconds
        self.max_age_seconds = max_age_seconds
        self._clock = clock or (lambda: datetime.now(UTC))
        self.stats = HeartbeatStats()

    def poll_once(self) -> bool:
        """One pass: True when a heartbeat was sent. A busy link (another operation holds the lock) skips the pass instead of waiting."""
        if not state_is_healthy(self._fetch(), self._clock(), self.max_age_seconds):
            self.stats.withheld += 1
            return False
        if not self._lock.acquire(blocking=False):
            self.stats.link_failures += 1
            return False
        try:
            sent = system_client.send_zones_heartbeat(self._transport)
        finally:
            self._lock.release()
        if sent:
            self.stats.sent += 1
        else:
            self.stats.link_failures += 1
        return sent

    def run(self, stop: threading.Event) -> None:
        while not stop.is_set():
            self.poll_once()
            stop.wait(self.period_seconds)

    def start(self) -> tuple[threading.Thread, threading.Event]:
        stop = threading.Event()
        thread = threading.Thread(target=self.run, args=(stop,), name="zones-heartbeat", daemon=True)
        thread.start()
        return thread, stop
