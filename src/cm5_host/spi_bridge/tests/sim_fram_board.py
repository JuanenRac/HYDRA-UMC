# =============================================================================
# HYDRA-UMC - Simulated controller board for the F-RAM requests (host only)
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see repo root LICENSE
# =============================================================================
"""A host model of a controller board behind the SPI-OTA bridge that answers the F-RAM erase and self-test requests like
src/mcu_stm32g474/boot/fram_node.h does (evidence: simulator; the real chip driver does not exist yet).

`answer_after_polls` makes the answer arrive only after that many polls, as it does when the erase of the chip takes longer than the wait of the gateway."""

from spi_bridge import fram_client as fc
from spi_bridge.protocol import (
    OFS_FRAM_ERASE_REQUEST,
    OFS_FRAM_ERASE_RESPONSE,
    OFS_FRAM_SELFTEST_REQUEST,
    OFS_FRAM_SELFTEST_RESPONSE,
    OFS_STATUS,
    SpiOtaFrame,
    polled_frame_type,
)

WORDS = {fc.OP_ERASE: b"ERAS", fc.OP_SELFTEST: b"TEST"}


class SimulatedFramBoard:
    def __init__(self, board_id=3, *, enabled=True, chip_size=256, update_running=False, answer_after_polls=0, stuck_at=None):
        self.board_id = board_id
        self.enabled = enabled
        self.update_running = update_running
        self.cells = bytearray((0x30 + i) & 0xFF for i in range(chip_size)) if chip_size else None
        self.stuck_at = stuck_at
        self.answer_after_polls = answer_after_polls
        self._pending = None
        self._polls = 0
        self.requests = []

    def _status(self, operation, slot):
        if not self.enabled:
            return fc.STATUS_DISABLED, 0
        if slot != self.board_id:
            return fc.STATUS_WRONG_SLOT, 0
        if self.update_running:
            return fc.STATUS_BUSY, 0
        if self.cells is None:
            return fc.STATUS_NO_CHIP, 0
        if operation == fc.OP_ERASE:
            written = len(self.cells)
            self.cells[:] = bytes(len(self.cells))
            if self.stuck_at is not None:
                return fc.STATUS_VERIFY_FAIL, self.stuck_at
            return fc.STATUS_OK, written
        if self.stuck_at is not None:
            return fc.STATUS_VERIFY_FAIL, self.stuck_at
        return fc.STATUS_OK, 0

    def transceive(self, frame: SpiOtaFrame) -> SpiOtaFrame:
        wanted = polled_frame_type(frame)
        if wanted is not None:
            self._polls += 1
            if self._pending and self._pending[0] == wanted and self._polls > self.answer_after_polls:
                kind, payload = self._pending
                self._pending = None
                return SpiOtaFrame(frame.target_tier, frame.target_slot, kind, 8, payload)
            return SpiOtaFrame(frame.target_tier, frame.target_slot, OFS_STATUS, 0)
        self.requests.append(frame)
        if frame.frame_type not in (OFS_FRAM_ERASE_REQUEST, OFS_FRAM_SELFTEST_REQUEST):
            return SpiOtaFrame(frame.target_tier, frame.target_slot, OFS_STATUS, 0)
        raw = bytes(frame.payload)
        operation = raw[1] if len(raw) == 8 else 0
        valid = frame.dlc == 8 and len(raw) == 8 and raw[0] == fc.REQUEST_MAGIC and raw[7] == fc.check_byte(raw[:7]) and raw[3:7] == WORDS.get(operation)
        if not valid:
            return SpiOtaFrame(frame.target_tier, frame.target_slot, OFS_STATUS, 0)  # noise gets no answer
        status, value = self._status(operation, raw[2])
        response = bytes((fc.RESPONSE_MAGIC, operation, status, self.board_id)) + value.to_bytes(4, "big")
        kind = OFS_FRAM_ERASE_RESPONSE if operation == fc.OP_ERASE else OFS_FRAM_SELFTEST_RESPONSE
        if self.answer_after_polls:
            self._pending, self._polls = (kind, response), 0
            return SpiOtaFrame(frame.target_tier, frame.target_slot, OFS_STATUS, 0)
        return SpiOtaFrame(frame.target_tier, frame.target_slot, kind, 8, response)
