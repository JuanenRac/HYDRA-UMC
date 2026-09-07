# =============================================================================
# HYDRA-UMC - Real CM5 <-> STM32H745 SPI transport
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0 - see repo root LICENSE
# =============================================================================
"""Real SPI + GPIO transport for the STM32H745 SPI-OTA link - the piece
`src/cm5_host/ipc_driver/` (C) was left as a "STARTING POINT ONLY" skeleton
for (see its own README). Reimplemented here in Python rather than
finishing the C skeleton, so it can reuse the same proven bootloader
state-machine reasoning already implemented (and unit-tested) in the
sibling URTC-FLASHER tool's own flasher_protocol.py, adapted from CAN
framing to this project's real SPI framing - see bootloader_client.py.

Real link parameters (README.md section 10, docs/architecture.md section
3): SPI1, up to 50 MHz, Mode 0, MSB-first, a `HYDRA_DATA_READY` GPIO line
the STM32H745 asserts (output) when a frame is ready, 128-byte frames.

Both `spidev` and `gpiod` are imported lazily, only inside
`open_spi_transport()`, so `protocol.py`/`bootloader_client.py`'s
safety-relevant framing/state-machine logic (and every test of it) works
on a host without either installed - the exact same pattern already
established across this ecosystem's other lazily-imported hardware
transports (HYDRA-UMC-BRIDGE-CNC's pyserial, HYDRA-UMC-BRIDGE-LASER's
gpiod, ...).
"""

from __future__ import annotations

from typing import Protocol

from .protocol import SPI_FRAME_SIZE, SpiOtaFrame


class SpiOtaTransport(Protocol):
    """The minimal real interface bootloader_client.py depends on."""

    def transceive(self, frame: SpiOtaFrame) -> SpiOtaFrame: ...
    def wait_data_ready(self, timeout_seconds: float) -> bool: ...
    def close(self) -> None: ...


class RealSpiOtaTransport:
    """Talks to a real STM32H745 over a real spidev + gpiod link.

    SPI is master-clocked from the CM5 side (bootloader_common.h's own
    comment: "SPI is master-clocked, so *something* has to go out every
    transaction regardless") - `transceive()` always both sends and
    receives a full 128-byte frame in one transaction, matching that real
    hardware behavior; it never sends without also reading whatever the
    STM32H745 clocked back.
    """

    def __init__(self, spi_device: object, data_ready_line: object):
        self._spi = spi_device
        self._data_ready_line = data_ready_line

    def transceive(self, frame: SpiOtaFrame) -> SpiOtaFrame:
        raw_request = frame.to_bytes()
        raw_response = bytes(self._spi.xfer2(list(raw_request)))
        if len(raw_response) != SPI_FRAME_SIZE:
            raise OSError(f"SPI transfer returned {len(raw_response)} bytes, expected {SPI_FRAME_SIZE}")
        return SpiOtaFrame.from_bytes(raw_response)

    def wait_data_ready(self, timeout_seconds: float) -> bool:
        event = self._data_ready_line.event_wait(timeout=timeout_seconds)
        return bool(event)

    def close(self) -> None:
        self._spi.close()
        self._data_ready_line.release()


def open_spi_transport(
    spi_device_path: str,
    data_ready_chip_path: str,
    data_ready_line_offset: int,
    *,
    spi_speed_hz: int = 10_000_000,
) -> RealSpiOtaTransport:
    """Open a real SPI1 link + HYDRA_DATA_READY GPIO line. The only place
    this module imports spidev/gpiod.

    `spi_speed_hz` defaults to 10 MHz, not the documented 50 MHz ceiling
    (README.md section 10) - matching the same "conservative default until
    verified against real hardware" caution the C skeleton this replaces
    already used, not yet raised because no real STM32H745 board exists to
    verify a higher real clock against.

    Raises RuntimeError with a clear message if spidev/gpiod aren't
    installed, rather than letting an ImportError surface from deep inside
    this module.
    """

    try:
        import spidev  # type: ignore[import-untyped]
        import gpiod  # type: ignore[import-untyped]
        from gpiod.line import Direction, Edge  # type: ignore[import-untyped]
    except ImportError as error:
        raise RuntimeError(
            "spidev/gpiod are not installed - install them to talk to a real STM32H745 over SPI "
            "(this module's frame/state-machine logic works and is tested without them)"
        ) from error

    bus, device = _parse_spidev_path(spi_device_path)
    spi = spidev.SpiDev()
    spi.open(bus, device)
    spi.mode = 0
    spi.max_speed_hz = spi_speed_hz
    spi.bits_per_word = 8

    data_ready_request = gpiod.request_lines(
        data_ready_chip_path,
        consumer="hydra-umc-spi-bridge",
        config={data_ready_line_offset: gpiod.LineSettings(direction=Direction.INPUT, edge_detection=Edge.RISING)},
    )

    class _DataReadyLine:
        def event_wait(self, timeout: float) -> bool:
            return data_ready_request.wait_edge_events(timeout=timeout)

        def release(self) -> None:
            data_ready_request.release()

    return RealSpiOtaTransport(spi, _DataReadyLine())


def _parse_spidev_path(spi_device_path: str) -> tuple[int, int]:
    """"/dev/spidev0.0" -> (0, 0) - the real Linux spidev naming convention."""

    name = spi_device_path.rsplit("/", 1)[-1]
    if not name.startswith("spidev"):
        raise ValueError(f"expected a /dev/spidevBUS.DEVICE path, got {spi_device_path!r}")
    bus_str, _, device_str = name[len("spidev") :].partition(".")
    try:
        return int(bus_str), int(device_str)
    except ValueError as error:
        raise ValueError(f"expected a /dev/spidevBUS.DEVICE path, got {spi_device_path!r}") from error
