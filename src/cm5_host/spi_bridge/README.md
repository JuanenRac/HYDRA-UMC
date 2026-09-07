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
- `spi_bridge/relay_tunnel.py` — reaches Tier 2 (the URTC Tool Head,
  through its own Robot Controller Board's RELAY_SEND/RELAY_RECV tunnel,
  `architecture.md` section 5) with a real, explicit 5-byte fragmentation
  scheme for a CAN frame's up-to-8-byte data. `RelayedTransport`
  implements the exact same `SpiOtaTransport` protocol `transport.py`
  does, so `bootloader_client.py`'s state machine works against Tier 2
  completely unchanged - pass `relay=1` to `http_service.py`'s routes to
  use it. Tier 3 (Advanced Expansion Board) needs one further real tunnel
  hop (URTC's own I2C bridge, CAN IDs 0x210-0x221,
  `docs/EXPANSION.TXT` in the sibling URTC repo) - not implemented yet,
  same real pattern would apply.
- `tests/` — 31 deterministic tests against in-memory fake transports
  (including a faithful fake Tier 1-relaying-to-Tier-2 stand-in) and real
  local HTTP servers (only the SPI transport underneath is faked) — see
  `../../tools/build_test.py`'s own `firmware-c` branch for how this is
  wired into this repo's normal build-test.

## What's still needed

- A real STM32H745 board to test any of this against — none exists yet
  (no schematic, see `../../hardware/PCB/kinematic_brain_stm32h745/README.md`).
- Tier 3 (Advanced Expansion Board) - needs the real I2C-bridge tunnel hop
  documented in `relay_tunnel.py`'s own docstring; the same real pattern
  as the Tier 2 tunnel already implemented would apply.
- Real `spidev`/`gpiod` verification once a CM5 + real board exist -
  `spi_speed_hz` defaults conservatively to 10 MHz, not the documented
  50 MHz ceiling, until then.

## What's already wired up

- `HYDRA-UMC-SERVER`'s `GET/POST /api/hardware/canota/{version,flash}`
  relay to this service's own HTTP routes (`docs/REMOTE_API.md` section 2h
  in that repo).
- `HYDRA-UMC-STUDIO`'s `Flasher.tsx`/`Tester.tsx` reach this service for
  real, through that relay, when `settings.canOta.transport ===
  'hardware'` - Tier 0 (Kinematic Brain), Tier 1 (Robot Controller Board)
  and Tier 2 (URTC Tool Head, via `relay=1`) are all real; Tier 3 stays on
  the simulated transport until the I2C-bridge tunnel above exists.

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
