# =============================================================================
# HYDRA-UMC - F-RAM erase and self-test client (CM5 side)
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see repo root LICENSE
# =============================================================================
"""The host side of the F-RAM requests of the kinematic brain (src/mcu_stm32h745/CM4/fram_node.h, docs/FRAM_ERASE.md).

The chip is the FM24CL64B on the main board, on the local I2C bus of the brain, so the requests go to the brain itself over the SPI link (the tier SELF) and
never to a CAN bus. Two operations, each one request (frame type 0x28) and one answer (frame type 0x29), the operation being a byte of the payload:

    erase     writes zeros over the whole F-RAM and reads them back
    selftest  writes two patterns over a scratch area, reads them back and puts the old bytes back

The frame layouts below are the ones of fram_node.h; tests/test_fram_client.py compares every constant with that file and with spi_app.h. The brain
answers DISABLED (the switch of the firmware, off by default), NO_CHIP (no bus timing fixed in the build) or the result, and this client reports exactly
that. Evidence of anything built on this file: simulator."""

from __future__ import annotations

import time
from dataclasses import dataclass

from .protocol import SPI_TARGET_SELF, SpiOtaFrame
from .transport import SpiOtaTransport

FRAM_REQUEST = 0x28   # SPI_APP_FRAM_REQUEST of spi_app.h
FRAM_RESPONSE = 0x29  # SPI_APP_FRAM_RESPONSE
IDLE_FRAME = 0xFF     # the frame that asks for nothing: the firmware ignores it and the transaction brings the staged answer
BRAIN_ID = 0          # FRAM_BRAIN_ID: the one chip of the stack is the brain's

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
OPERATION_NAMES = {OP_ERASE: "erase", OP_SELFTEST: "selftest"}


def check_byte(first_seven: bytes) -> int:
    value = 0
    for byte in first_seven:
        value ^= byte
    return value ^ 0xFF


def build_request(operation: int, board: int = BRAIN_ID) -> bytes:
    if operation not in _WORDS:
        raise ValueError("operation must be OP_ERASE or OP_SELFTEST")
    if not 0 <= board <= 255:
        raise ValueError("board must be a byte")
    body = bytes((REQUEST_MAGIC, operation, board)) + _WORDS[operation]
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
    operation: int,
    *,
    timeout_seconds: float = 8.0,
    poll_interval_seconds: float = 0.05,
) -> FramResult | None:
    """Sends the request to the brain and polls with idle frames for the answer (an erase takes the brain a few seconds: it writes and reads back the whole chip).
    None when no answer comes within the timeout: the brain is absent, in its bootloader, or built without the SPI service (nothing answers these requests)."""
    request = SpiOtaFrame(SPI_TARGET_SELF, 0, FRAM_REQUEST, 8, build_request(operation))
    deadline = time.monotonic() + timeout_seconds
    try:
        answer = transport.transceive(request)
        while True:
            if answer.frame_type == FRAM_RESPONSE:
                result = parse_response(operation, answer.payload)
                if result is not None:
                    return result
            if time.monotonic() >= deadline:
                return None
            time.sleep(poll_interval_seconds)
            answer = transport.transceive(SpiOtaFrame(SPI_TARGET_SELF, 0, IDLE_FRAME, 0))
    except (OSError, ValueError):
        return None


def erase_fram(transport: SpiOtaTransport, **kwargs) -> FramResult | None:
    return run_operation(transport, OP_ERASE, **kwargs)


def fram_selftest(transport: SpiOtaTransport, **kwargs) -> FramResult | None:
    return run_operation(transport, OP_SELFTEST, **kwargs)
