# F-RAM erase and self-test of the kinematic brain

Status: **the requests, the answers, the logic of the brain, the driver of the chip on a fake I2C bus and the host side are written and tested on the host with a simulated chip. Nothing here has run on a board.** The brain answers `disabled` (the switch of the firmware, `HYDRA_FRAM_ERASE`, ships off) or `no-chip` (the build did not fix the I2C timing, `HYDRA_FRAM_I2C_TIMING`, which has no default on purpose) until bench rows 30 and 31 pass.

## The chip and where it is

The F-RAM is the FM24CL64B (8 KiB, I2C, not SPI), U7 of the main board, on the local bus I2C4 of the H745 that nothing else shares:

| Signal | Pin of the H745 | Pin of U7 |
|---|---|---|
| `FRAM_I2C_SCL` | `PD12` (I2C4_SCL, alternate function 4) | 6 |
| `FRAM_I2C_SDA` | `PD13` (I2C4_SDA, alternate function 4) | 5 |

A2, A1 and A0 are tied to ground, so the 7-bit address is `0x50`; WP is tied to ground (writes enabled); the pull-ups are 4.7 kOhm to 3V3. There is one chip in the whole stack and it belongs to the brain, not to a controller board.

## The path of a request

CM5 -> SPI link (tier `SELF`) -> CM4 -> I2C4 -> U7. It never goes on a CAN bus and the controller boards do not take part (their bootloader no longer answers F-RAM requests). The CM4 owns the bus because it already runs the SPI service (`spi_app.h`) and the task that owns the emergency stop; the CM7 does not touch I2C4.

| Frame type | Name | Direction | Meaning |
|---|---|---|---|
| `0x28` | `SPI_APP_FRAM_REQUEST` | CM5 to CM4, DLC 8 | erase or self-test |
| `0x29` | `SPI_APP_FRAM_RESPONSE` | CM4 to CM5, DLC 8 | status and a value |

The SPI task only leaves the request for the STACK A task, which runs it between its stop frames and stages the answer for the next transaction; the CM5 polls with idle frames (`0xFF`) until `0x29` comes. A request with a length other than 8, or in a build with the SPI service off, is ignored.

Request (8 bytes): `0xFA`, operation (`1` erase, `2` self-test), the board byte (always `0`, the brain), `E R A S` (erase) or `T E S T` (self-test), the complement of the XOR of the first seven bytes. A request that does not decode gets no answer.
Response (8 bytes): `0xFB`, operation, status, the board byte, four bytes big endian (bytes written, or the address that failed).

| Status | Name | Meaning |
|---|---|---|
| `0x00` | `ok` | done and read back |
| `0x01` | `disabled` | the switch of the firmware is off (the default) |
| `0x02` | `no-chip` | no driver: the build fixed no I2C timing |
| `0x03` | `bad-request` | reserved |
| `0x04` | `wrong-slot` | the board byte is not `0` |
| `0x05` | `busy` | reserved: the brain has no update that touches the chip |
| `0x06` | `chip-error` | the bus did not answer after the tries, or a block did not read back as written |
| `0x07` | `verify-fail` | the final readback is not what was written |

An F-RAM has nothing to erase: the erase writes zeros over the 8192 bytes and reads them all back. The self-test works on the last 64 bytes of the chip only and puts them back, also when it fails.

## The driver (`fram_i2c.h`)

An access is the device address `0x50`, two address bytes (high first) and then the data; a read repeats the start and reads. An F-RAM has no page and no write delay, so a write may be of any length and the address wraps at the end of the chip; the driver cuts every access into chunks of 30 bytes only because of the buffer of the bus layer, refuses an access that would pass the end of the chip instead of wrapping, tries every chunk three times (a NACK, a timeout, or a block that does not read back as written) and runs a hook between chunks. The hook sends the stop frames that are due, so an erase (a few seconds at 100 kHz) never starves the ALIVE frames of the emergency stop. The HAL glue is in `SpiApp.c`: `HAL_I2C_Mem_Read` and `HAL_I2C_Mem_Write` with 16-bit memory addresses on `I2C4`, `PD12` and `PD13` in open drain, no internal pull.

`HYDRA_FRAM_I2C_TIMING` is the `TIMINGR` value for the kernel clock of I2C4 and the speed the build chooses. It has no default: a value that was never checked could run the bus out of its specification.

## Host side

`src/cm5_host/spi_bridge/spi_bridge/fram_client.py` sends the request to the brain and waits for the answer. The local service offers, only when started with `--enable-fram-erase`:

- `POST /fram/erase?confirm=ERASE` (an erase without the word `ERASE` is refused with 400)
- `POST /fram/selftest`
- `GET /version` carries `capabilities.fram_erase`, so an app offers the operation only when the bridge does.

With the service switch off every route answers 403 and the brain is not touched. The brain then has its own switch on top of that.

## The slot window

The window of a slot is 64 identifiers wide (`slot_base + slot * 64 + offset`, the eight slots fill `0x600` to `0x7FF`); the filter of a controller board is mask `0x7C0` and the one of the brain `0x600`. The offsets `0x20` to `0x23` were added for a first design that put the chip on each controller board; they stay reserved, nothing sends or answers them, and the wider window stays. The three firmwares must still be updated together.

## Tests

`tests/test_fram_node.c` (the logic of the brain with a simulated chip, built with the switch off and on), `tests/test_fram_i2c.c` (the driver on a fake I2C bus: the address, the two address bytes, the chunks, the end of the chip, the tries, a cell that keeps a wrong value, the whole path through the node), `tests/test_spi_app.c` (the request is left for the STACK A task and the answer is staged once), `src/cm5_host/spi_bridge/tests/test_fram_client.py` (the client, the HTTP routes and the comparison of every constant with `fram_node.h`, `fram_i2c.h` and `spi_app.h`). Bench rows 30 and 31 of `docs/BENCH_TESTS.md` stay open.
