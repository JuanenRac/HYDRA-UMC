# =============================================================================
# HYDRA-UMC - Simulated kinematic brain for the F-RAM requests (host only)
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see repo root LICENSE
# =============================================================================
"""A host model of the kinematic brain on the SPI link that answers the F-RAM erase and self-test requests like
src/mcu_stm32h745/CM4/fram_node.h does (evidence: simulator; the chip is the FM24CL64B on I2C4 and nothing has run on a board).

The brain stages its answer for the next transaction, so the client polls with idle frames. `answer_after_polls` makes the answer arrive only after that many
idle frames, as it does when the erase of the chip takes a few seconds."""

from spi_bridge import fram_client as fc
from spi_bridge.protocol import SPI_TARGET_SELF, SpiOtaFrame

WORDS = {fc.OP_ERASE: b"ERAS", fc.OP_SELFTEST: b"TEST"}


class SimulatedFramBoard:
    def __init__(self, board_id=fc.BRAIN_ID, *, enabled=True, chip_size=256, update_running=False, answer_after_polls=0, stuck_at=None):
        self.board_id = board_id
        self.enabled = enabled
        self.update_running = update_running
        self.cells = bytearray((0x30 + i) & 0xFF for i in range(chip_size)) if chip_size else None
        self.stuck_at = stuck_at
        self.answer_after_polls = answer_after_polls
        self._pending = None
        self._polls = 0
        self.requests = []

    def _status(self, operation, board):
        if not self.enabled:
            return fc.STATUS_DISABLED, 0
        if board != self.board_id:
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

    def _idle(self, frame: SpiOtaFrame) -> SpiOtaFrame:
        return SpiOtaFrame(frame.target_tier, 0, fc.IDLE_FRAME, 0)

    def transceive(self, frame: SpiOtaFrame) -> SpiOtaFrame:
        if frame.frame_type == fc.IDLE_FRAME:
            self._polls += 1
            if self._pending is not None and self._polls > self.answer_after_polls:
                payload, self._pending = self._pending, None
                return SpiOtaFrame(frame.target_tier, 0, fc.FRAM_RESPONSE, 8, payload)
            return self._idle(frame)
        self.requests.append(frame)
        if frame.target_tier != SPI_TARGET_SELF or frame.frame_type != fc.FRAM_REQUEST:
            return self._idle(frame)
        raw = bytes(frame.payload)
        operation = raw[1] if len(raw) == 8 else 0
        valid = frame.dlc == 8 and len(raw) == 8 and raw[0] == fc.REQUEST_MAGIC and raw[7] == fc.check_byte(raw[:7]) and raw[3:7] == WORDS.get(operation)
        if not valid:
            return self._idle(frame)  # noise gets no answer
        status, value = self._status(operation, raw[2])
        self._pending, self._polls = bytes((fc.RESPONSE_MAGIC, operation, status, self.board_id)) + value.to_bytes(4, "big"), 0
        if self.answer_after_polls:
            return self._idle(frame)
        payload, self._pending = self._pending, None
        return SpiOtaFrame(frame.target_tier, 0, fc.FRAM_RESPONSE, 8, payload)
