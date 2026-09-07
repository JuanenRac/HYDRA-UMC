# =============================================================================
# HYDRA-UMC - Fake SpiOtaTransport for deterministic bootloader_client tests
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0 - see repo root LICENSE
# =============================================================================
"""A minimal, deterministic in-memory stand-in for a real STM32H745 talking
SPI-OTA - responds correctly to every real frame_type the flash cycle
sends, so bootloader_client.py's real state-machine logic can be proven
correct without any real SPI/GPIO/hardware."""

import struct

from spi_bridge.protocol import (
    OFS_DATA,
    OFS_PAGE_ACK,
    OFS_STATUS,
    OFS_VERSION_RESPONSE,
    STATUS_VERIFY_OK,
    SpiOtaFrame,
)


class FakeBootloaderTransport:
    def __init__(self, *, hardware_id: int = 0x48374334, online: bool = True):
        self.hardware_id = hardware_id
        self.online = online
        self.pages_received = 0
        self._page_bytes_buffer = bytearray()
        self.raise_on_transceive: OSError | None = None
        self.sent_frames: list[SpiOtaFrame] = []

    def transceive(self, frame: SpiOtaFrame) -> SpiOtaFrame:
        if self.raise_on_transceive:
            raise self.raise_on_transceive
        self.sent_frames.append(frame)

        if not self.online:
            raise OSError("no response (offline)")

        if frame.frame_type == 0x08:  # OFS_QUERY_VERSION
            payload = bytes([0x00]) + struct.pack(">I", self.hardware_id) + struct.pack(">H", 0) + bytes([1])
            return SpiOtaFrame(frame.target_tier, frame.target_slot, OFS_VERSION_RESPONSE, 8, payload)

        if frame.frame_type == OFS_DATA:
            self._page_bytes_buffer += frame.payload[: frame.dlc]
            if len(self._page_bytes_buffer) >= 2048 or frame.dlc < 8:
                self._page_bytes_buffer.clear()
                self.pages_received += 1
            return SpiOtaFrame(frame.target_tier, frame.target_slot, 0xFF, 0)  # no meaningful response expected

        if frame.frame_type == OFS_PAGE_ACK:
            acked_index = max(0, self.pages_received - 1)
            return SpiOtaFrame(frame.target_tier, frame.target_slot, OFS_PAGE_ACK, 4, struct.pack(">I", acked_index))

        if frame.frame_type == OFS_STATUS:
            return SpiOtaFrame(frame.target_tier, frame.target_slot, OFS_STATUS, 1, bytes([STATUS_VERIFY_OK]))

        # ENTER_BOOTLOADER/START_UPDATE/HMAC_CHUNK/END_UPDATE/AUTHORIZE_DOWNGRADE -
        # accepted, no real response payload needed by the state machine.
        return SpiOtaFrame(frame.target_tier, frame.target_slot, 0xFF, 0)

    def wait_data_ready(self, timeout_seconds: float) -> bool:
        return True

    def close(self) -> None:
        pass
