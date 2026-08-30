# spi_bridge — real CM5 ↔ STM32H745 SPI-OTA bridge

**Project:** HYDRA-UMC
**Status:** ✅ real protocol/state-machine/HTTP service, unit-tested against fakes — 🚧 not yet exercised against real SPI/GPIO hardware or a real STM32H745 (none exists yet, see repo root `hardware/PCB/kinematic_brain_stm32h745/README.md`).

Replaces `../ipc_driver/`'s own unfinished C skeleton (kept, not deleted —
see that folder's own README for why) with a Python implementation that
reuses the sibling `URTC-FLASHER` tool's own real, already-proven
CRC32/HMAC-SHA256-verified bootloader state machine, adapted from CAN
framing to this project's real SPI framing.

## What's here today

- `spi_bridge/protocol.py` — the real, exact 128-byte `SpiOtaFrame` wire
  format (byte-for-byte match of `mcu_stm32h745/CM4/boot/bootloader_common.h`'s
  own C struct) and every `OFS_*`/`STATUS_*` constant.
- `spi_bridge/bootloader_client.py` — real `query_version()` and the full
  `SpiOtaFlasher.flash()` cycle (ENTER_BOOTLOADER → START_UPDATE →
  HMAC_CHUNK ×4 → DATA page-by-page → END_UPDATE → STATUS verify), a real
  CRC32/HMAC-SHA256 over the actual firmware bytes, not a placeholder.
- `spi_bridge/transport.py` — `RealSpiOtaTransport` (real `spidev` +
  `gpiod` v2, lazily imported so the rest of this package needs neither
  installed) plus the `SpiOtaTransport` protocol every other module here
  is written against, so the state machine is provably correct without
  real hardware.
- `spi_bridge/http_service.py` — a small, stdlib-only local HTTP service
  (`GET /version`, `POST /flash`) that `HYDRA-UMC-SERVER` (Node.js) relays
  to, the same "small local upstream service reached over loopback HTTP"
  pattern SERVER already uses for Voice UI/Datalake.
- `tests/` — 22 deterministic tests against an in-memory fake transport
  and a real local HTTP server (only the SPI transport underneath is
  faked) — see `../../tools/build_test.py`'s own `firmware-c` branch for
  how this is wired into this repo's normal build-test.

## What's still needed

- A real STM32H745 board to test any of this against — none exists yet
  (no schematic, see `../../hardware/PCB/kinematic_brain_stm32h745/README.md`).
- `HYDRA-UMC-SERVER`'s own relay routes (`POST /api/hardware/canota/*`)
  and `HYDRA-UMC-STUDIO`'s `Flasher.tsx`/`Tester.tsx` wiring the
  `transport === 'hardware'` case to this service instead of always
  calling `mock*` — see `HYDRA-UMC-STUDIO/src/lib/canOta.ts`'s own header
  comment for that switch.
- Real `spidev`/`gpiod` verification once a CM5 + real board exist -
  `spi_speed_hz` defaults conservatively to 10 MHz, not the documented
  50 MHz ceiling, until then.

## Running the tests

```bash
cd src/cm5_host/spi_bridge
PYTHONPATH=".:./tests" python3 -m unittest discover -s tests -v
```

No `spidev`/`gpiod` install required — only `open_spi_transport()` needs
them, and it isn't exercised by these tests (see `test_transport.py`).

## Running the local service (needs real hardware)

```python
from spi_bridge import open_spi_transport
from spi_bridge.http_service import serve

transport = open_spi_transport("/dev/spidev0.0", "/dev/gpiochip0", data_ready_line_offset=3)
server = serve(transport, hmac_key=b"...")  # real HMAC_KEY, matches the firmware's own
server.serve_forever()
```
