# =============================================================================
# HYDRA-UMC - F-RAM erase and self-test client (CM5 side)
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see repo root LICENSE
# =============================================================================
"""The host side of the F-RAM requests of a controller board (src/mcu_stm32g474/boot/fram_node.h, docs/FRAM_ERASE.md).

Two operations, each one request and one answer on the window of the slot:

    erase     0x20 -> 0x21   writes zeros over the whole F-RAM and reads them back
    selftest  0x22 -> 0x23   writes two patterns over a scratch area, reads them back and puts the old bytes back

The frame layouts below are the ones of fram_node.h; tests/test_fram_client.py compares every constant with that file. The chip itself is not fixed
and the board has no driver for it: a board answers DISABLED (the switch of the firmware is off, the default) or NO_CHIP until that is done, and this
client reports exactly that. Evidence of anything built on this file: simulator."""

from __future__ import annotations

import time
from dataclasses import dataclass

from .protocol import (
    OFS_FRAM_ERASE_REQUEST,
    OFS_FRAM_ERASE_RESPONSE,
    OFS_FRAM_SELFTEST_REQUEST,
    OFS_FRAM_SELFTEST_RESPONSE,
    SPI_TARGET_STACKA,
    SpiOtaFrame,
    poll_frame,
)
from .transport import SpiOtaTransport

REQUEST_MAGIC = 0xFA
RESPONSE_MAGIC = 0xFB

OP_ERASE = 0x01
OP_SELFTEST = 0x02

STATUS_OK = 0x00
STATUS_DISABLED = 0x01
STATUS_NO_CHIP = 0x02
STATUS_BAD_REQUEST = 0x03
STATUS_WRONG_SLOT = 0x04
STATUS_BUSY = 0x05
STATUS_CHIP_ERROR = 0x06
STATUS_VERIFY_FAIL = 0x07
STATUS_NAMES = {
    STATUS_OK: "ok",
    STATUS_DISABLED: "disabled",
    STATUS_NO_CHIP: "no-chip",
    STATUS_BAD_REQUEST: "bad-request",
    STATUS_WRONG_SLOT: "wrong-slot",
    STATUS_BUSY: "busy",
    STATUS_CHIP_ERROR: "chip-error",
    STATUS_VERIFY_FAIL: "verify-fail",
}

_WORDS = {OP_ERASE: b"ERAS", OP_SELFTEST: b"TEST"}
_FRAMES = {
    OP_ERASE: (OFS_FRAM_ERASE_REQUEST, OFS_FRAM_ERASE_RESPONSE),
    OP_SELFTEST: (OFS_FRAM_SELFTEST_REQUEST, OFS_FRAM_SELFTEST_RESPONSE),
}
OPERATION_NAMES = {OP_ERASE: "erase", OP_SELFTEST: "selftest"}


def check_byte(first_seven: bytes) -> int:
    value = 0
    for byte in first_seven:
        value ^= byte
    return value ^ 0xFF


def build_request(operation: int, slot: int) -> bytes:
    if operation not in _WORDS:
        raise ValueError("operation must be OP_ERASE or OP_SELFTEST")
    if not 0 <= slot <= 7:
        raise ValueError("slot must be 0 to 7")
    body = bytes((REQUEST_MAGIC, operation, slot)) + _WORDS[operation]
    return body + bytes((check_byte(body),))


@dataclass(frozen=True)
class FramResult:
    operation: str
    status: int
    status_name: str
    board_id: int
    value: int  # bytes written (erase) or the address that failed (self-test)

    @property
    def ok(self) -> bool:
        return self.status == STATUS_OK

    def as_json(self) -> dict:
        return {"operation": self.operation, "status": self.status_name, "ok": self.ok, "board_id": self.board_id, "value": self.value}


def parse_response(operation: int, payload: bytes) -> FramResult | None:
    """The result an eight-byte answer carries, or None when it is not the answer to this operation (nothing is invented)."""
    if len(payload) < 8 or payload[0] != RESPONSE_MAGIC or payload[1] != operation:
        return None
    status = payload[2]
    return FramResult(
        OPERATION_NAMES[operation],
        status,
        STATUS_NAMES.get(status, f"unknown-{status:#04x}"),
        payload[3],
        int.from_bytes(payload[4:8], "big"),
    )


def run_operation(
    transport: SpiOtaTransport,
    slot: int,
    operation: int,
    *,
    timeout_seconds: float = 5.0,
    poll_interval_seconds: float = 0.05,
    target_tier: int = SPI_TARGET_STACKA,
) -> FramResult | None:
    """Sends the request to the board of `slot` and waits for the answer (the gateway hands it over when it polls for its frame type).
    None when no answer comes within the timeout: the board is absent, or not in its bootloader (the application does not answer these requests)."""
    request_type, response_type = _FRAMES[operation]
    request = SpiOtaFrame(target_tier, slot, request_type, 8, build_request(operation, slot))
    deadline = time.monotonic() + timeout_seconds
    try:
        answer = transport.transceive(request)
        while True:
            if answer.frame_type == response_type:
                result = parse_response(operation, answer.payload)
                if result is not None:
                    return result
            if time.monotonic() >= deadline:
                return None
            time.sleep(poll_interval_seconds)
            answer = transport.transceive(poll_frame(target_tier, slot, response_type))
    except (OSError, ValueError):
        return None


def erase_fram(transport: SpiOtaTransport, slot: int, **kwargs) -> FramResult | None:
    return run_operation(transport, slot, OP_ERASE, **kwargs)


def fram_selftest(transport: SpiOtaTransport, slot: int, **kwargs) -> FramResult | None:
    return run_operation(transport, slot, OP_SELFTEST, **kwargs)
