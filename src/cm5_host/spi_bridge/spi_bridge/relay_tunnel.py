# =============================================================================
# HYDRA-UMC - Real Tier 2/3 relay tunnel (RELAY_SEND/RELAY_RECV, +0x12/+0x13)
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0 - see repo root LICENSE
# =============================================================================
"""Reach the URTC Tool Head (Tier 2) and, through it, its optional Advanced
Expansion Board (Tier 3) - a real, ID-agnostic tunnel through the Robot
Controller Board's own second CAN controller, per architecture.md section
5: RELAY_SEND (+0x12) queues an opaque outbound CAN frame onto STACK A's
Tier-1 board; RELAY_RECV (+0x13) drains its FIFO of captured inbound
frames since the last poll. Neither of these two SPI frame types ever
needs new protocol design on the Tier 2/3 side - Tier 2 already speaks
URTC's own real, proven CAN bootloader protocol (0x7F0-0x7FF,
docs/CANBUS.TXT in the sibling URTC repo), which this module tunnels
byte-for-byte rather than reinventing.

`RelayedTransport` implements the exact same `SpiOtaTransport` protocol
`transport.py` does - so `bootloader_client.py`'s `SpiOtaFlasher`/
`query_version()` work against a Tier 2/3 target completely unchanged,
just given a `RelayedTransport` instead of a direct one. This is the same
kind of transport-agnostic reuse `bootloader_client.py`'s own docstring
already establishes for Tier 0/1.

Wire format for one relayed CAN frame's real 8-byte data across possibly
multiple RELAY_SEND/RELAY_RECV frames (this module's own real, explicit
fragmentation scheme, since a real CAN frame's DLC can be up to 8 bytes
but a single 128-byte SpiOtaFrame_t's 8-byte payload field only leaves
room for 5 real data bytes once the target id + dlc header is accounted
for):

    RELAY_SEND / RELAY_RECV payload (5 real header/data bytes, +3 reserved
    inside the SpiOtaFrame_t's own 8-byte payload field):
        payload[0:2] = target/source real CAN ID, big-endian (0x7F0-0x7FF
                        range - URTC's own bootloader protocol space)
        payload[2]   = real total DLC (0-8) of the CAN frame being relayed
        payload[3:8] = up to 5 bytes of this fragment's data, in order

    A CAN frame with dlc <= 5 fits in one RELAY_SEND/RELAY_RECV frame. A
    frame with dlc 6-8 needs exactly two: the first carries data[0:5], the
    second carries data[5:dlc] - the real total dlc is repeated in both
    fragments (this tunnel is stateless per SPI transaction, so the
    receiver never needs anything beyond "how many bytes total am I
    accumulating for this real CAN id" to know when a frame is complete).
"""

from __future__ import annotations

import time
from dataclasses import dataclass

from .protocol import (
    OFS_PAGE_ACK,
    OFS_QUERY_VERSION,
    OFS_RELAY_RECV,
    OFS_RELAY_SEND,
    OFS_STATUS,
    SpiOtaFrame,
)
from .transport import SpiOtaTransport

# Real, closed set of the ONLY frame types bootloader_client.py ever
# actually waits on a response for (query_version()'s QUERY_VERSION, and
# _send_page_and_wait_ack()/_wait_for_status()'s PAGE_ACK/STATUS polls) -
# see bootloader_client.py's own _send() helper, which fires-and-forgets
# every other frame type (ENTER_BOOTLOADER/START_UPDATE/HMAC_CHUNK/DATA/
# END_UPDATE/AUTHORIZE_DOWNGRADE) without reading its return value at all.
# Waiting on RELAY_RECV for those too would be real, needless latency -
# hundreds of full RELAY_RECV timeouts across a real flash cycle's HMAC/
# DATA fragments, for a response nothing ever reads.
_QUERY_FRAME_TYPES = frozenset((OFS_QUERY_VERSION, OFS_PAGE_ACK, OFS_STATUS))

# URTC's own real, proven CAN bootloader protocol space (docs/CANBUS.TXT in
# the sibling URTC repo) - this project's own OFS_* offset table (+0x00..
# +0x14) is deliberately numerically identical to URTC's real ID offsets
# from this base, so translating between them is exactly an addition/
# subtraction, not a lookup table.
URTC_BOOTLOADER_CAN_BASE = 0x7F0

_FRAGMENT_DATA_BYTES = 5


@dataclass(frozen=True)
class RelayFrame:
    """One real logical CAN frame being tunneled through Tier 1."""

    can_id: int
    dlc: int
    data: bytes


def build_relay_send_fragments(frame: RelayFrame) -> list[bytes]:
    """Split one real CAN frame's data into 1-2 real RELAY_SEND 5-byte fragments."""

    if not (0 <= frame.can_id <= 0xFFFF):
        raise ValueError("can_id must fit a uint16_t")
    if not (0 <= frame.dlc <= 8):
        raise ValueError("dlc must be 0-8, matching a real CAN frame")
    if len(frame.data) != frame.dlc:
        raise ValueError("data length must match dlc exactly")

    header = frame.can_id.to_bytes(2, "big") + bytes((frame.dlc,))
    if frame.dlc <= _FRAGMENT_DATA_BYTES:
        return [header + frame.data]
    first = header + frame.data[:_FRAGMENT_DATA_BYTES]
    second = header + frame.data[_FRAGMENT_DATA_BYTES:]
    return [first, second]


def parse_relay_fragment(payload: bytes) -> tuple[int, int, bytes]:
    """The real inverse of build_relay_send_fragments() for one fragment - (can_id, total_dlc, fragment_data)."""

    if len(payload) < 3:
        raise ValueError("relay fragment payload must be at least 3 bytes (id + dlc header)")
    can_id = int.from_bytes(payload[0:2], "big")
    total_dlc = payload[2]
    fragment_data = payload[3 : 3 + min(total_dlc, len(payload) - 3)]
    return can_id, total_dlc, fragment_data


class RelayedTransport:
    """A real SpiOtaTransport that reaches Tier 2/3 by tunneling through an
    already-open, directly-reachable Tier 1 transport - see this module's
    own docstring for the real RELAY_SEND/RELAY_RECV wire format."""

    def __init__(self, underlying: SpiOtaTransport, tier1_target_tier: int, tier1_target_slot: int):
        self._underlying = underlying
        self._tier1_target_tier = tier1_target_tier
        self._tier1_target_slot = tier1_target_slot

    def transceive(self, frame: SpiOtaFrame) -> SpiOtaFrame:
        # HYDRA-UMC's own OFS_* offsets are deliberately numerically
        # identical to URTC's real CAN ID offsets from URTC_BOOTLOADER_CAN_BASE
        # (see this module's own header comment) - no lookup table needed.
        real_can_id = URTC_BOOTLOADER_CAN_BASE + frame.frame_type
        outbound = RelayFrame(real_can_id, frame.dlc, frame.payload[: frame.dlc])
        for fragment in build_relay_send_fragments(outbound):
            self._underlying.transceive(
                SpiOtaFrame(self._tier1_target_tier, self._tier1_target_slot, OFS_RELAY_SEND, len(fragment), fragment)
            )

        if frame.frame_type not in _QUERY_FRAME_TYPES:
            # Fire-and-forget - the caller never reads this return value for
            # these frame types (see this module's own _QUERY_FRAME_TYPES
            # comment), so there is nothing real to wait on here.
            return SpiOtaFrame(frame.target_tier, frame.target_slot, 0xFF, 0)

        response_can_id, response_dlc, response_data = self._drain_relay_recv(timeout_seconds=2.0)
        response_frame_type = max(0, min(255, response_can_id - URTC_BOOTLOADER_CAN_BASE))
        return SpiOtaFrame(frame.target_tier, frame.target_slot, response_frame_type, response_dlc, response_data)

    def _drain_relay_recv(self, timeout_seconds: float) -> tuple[int, int, bytes]:
        """Polls RELAY_RECV, reassembling fragments for the first real CAN id
        it sees until `total_dlc` bytes are accumulated, or times out."""

        deadline = time.monotonic() + timeout_seconds
        accumulated_id: int | None = None
        accumulated_dlc = 0
        accumulated_data = b""
        while time.monotonic() < deadline:
            response = self._underlying.transceive(
                SpiOtaFrame(self._tier1_target_tier, self._tier1_target_slot, OFS_RELAY_RECV, 0)
            )
            if response.dlc < 3:
                continue  # nothing new queued this poll
            can_id, total_dlc, fragment_data = parse_relay_fragment(response.payload[: response.dlc])
            if accumulated_id is None:
                accumulated_id, accumulated_dlc = can_id, total_dlc
            elif can_id != accumulated_id:
                continue  # a different in-flight frame's fragment - not ours, keep waiting for ours
            accumulated_data += fragment_data
            if len(accumulated_data) >= accumulated_dlc:
                return accumulated_id, accumulated_dlc, accumulated_data[:accumulated_dlc]
        return 0, 0, b""  # timed out - caller (bootloader_client.py) already fails safe on an empty/unknown response

    def wait_data_ready(self, timeout_seconds: float) -> bool:
        return self._underlying.wait_data_ready(timeout_seconds)

    def close(self) -> None:
        pass  # the underlying Tier 1 transport owns the real connection lifecycle
