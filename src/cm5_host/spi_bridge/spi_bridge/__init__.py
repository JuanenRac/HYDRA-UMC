# =============================================================================
# HYDRA-UMC - spi_bridge public package interface
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0 - see repo root LICENSE
# =============================================================================
"""Real CM5 <-> STM32H745 SPI-OTA bridge - protocol, transport, state
machine and a small local HTTP service SERVER (Node.js) relays to."""

from .bootloader_client import FlashProgress, SpiOtaFlasher, VersionInfo, query_version
from .protocol import SPI_FRAME_SIZE, SPI_TARGET_CM7, SPI_TARGET_SELF, SPI_TARGET_STACKA, SpiOtaFrame
from .transport import RealSpiOtaTransport, SpiOtaTransport, open_spi_transport

__all__ = [
    "SpiOtaFrame",
    "SPI_FRAME_SIZE",
    "SPI_TARGET_SELF",
    "SPI_TARGET_CM7",
    "SPI_TARGET_STACKA",
    "SpiOtaTransport",
    "RealSpiOtaTransport",
    "open_spi_transport",
    "VersionInfo",
    "query_version",
    "FlashProgress",
    "SpiOtaFlasher",
]
