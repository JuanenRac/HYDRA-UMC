# F-RAM erase and self-test of a controller board

Status: **the requests, the answers, the logic of the board and the host side are written and tested on the host with a simulated chip. The driver of the real chip is not written: the chip (an FM25V05-G of 64 KB or an MB85RS64 of 8 KB) and the pins of SPI2 are not fixed. Nothing here has run on a board.** A board answers `disabled` (the switch of the firmware, `HYDRA_FRAM_ERASE`, ships off) or `no-chip` until that is done.

## Why the slot window is 64 identifiers wide

The first 32 identifiers of a slot window were all taken (`tools/can_ids.py`), so there was no room for a command. The window of a slot is now 64 identifiers: `slot_base + slot * 64 + offset` with `slot_base = 0x600`, so the eight slots fill `0x600` to `0x7FF` and the last identifier still fits the 11 bits of a standard frame. The hardware filter of a controller board changed from mask `0x7E0` to `0x7C0` (its own 64 identifiers and the emergency stop `0x001`) and the one of the kinematic brain from `0x700` to `0x600` (the eight windows and `0x001`). The three firmwares (the bootloader of the controller board, and the CM4 and CM7 of the brain) must be updated together: a board left on the old window is not seen by the new brain.

## Messages (offsets inside the window of a slot)

| Offset | Name | Emitter | Meaning |
|---|---|---|---|
| `0x20` | `FRAM_ERASE_REQUEST` | host (through the kinematic brain) | write zeros over the whole F-RAM and read them back |
| `0x21` | `FRAM_ERASE_RESPONSE` | bootloader | status and the number of bytes written |
| `0x22` | `FRAM_SELFTEST_REQUEST` | host | write two patterns over a scratch area, read them back, put the old bytes back |
| `0x23` | `FRAM_SELFTEST_RESPONSE` | bootloader | status and the address that failed |

Only the bootloader answers; the application ignores them. The brain never puts the answers on the bus, whatever the CM5 asks for.

Request (8 bytes): `0xFA`, operation (`1` erase, `2` self-test), slot, `E R A S` (erase) or `T E S T` (self-test), the complement of the XOR of the first seven bytes. A request that does not decode gets no answer.
Response (8 bytes): `0xFB`, operation, status, board identifier, four bytes big endian (bytes written, or the address that failed).

| Status | Name | Meaning |
|---|---|---|
| `0x00` | `ok` | done and read back |
| `0x01` | `disabled` | the switch of the firmware is off (the default) |
| `0x02` | `no-chip` | no driver: the chip is not fixed or not fitted |
| `0x03` | `bad-request` | reserved |
| `0x04` | `wrong-slot` | the slot byte is not this board |
| `0x05` | `busy` | an update is running: the board does not touch the F-RAM meanwhile |
| `0x06` | `chip-error` | the driver reported a failure |
| `0x07` | `verify-fail` | the readback is not what was written |

An F-RAM has nothing to erase: the erase writes zeros. The self-test works on the last 64 bytes of the chip only and puts them back, also when it fails.

## Host side

`src/cm5_host/spi_bridge/spi_bridge/fram_client.py` sends the request and waits for the answer by polling its frame type. The local service offers, only when started with `--enable-fram-erase`:

- `POST /fram/erase?slot=N&confirm=ERASE` (an erase without the word `ERASE` is refused with 400)
- `POST /fram/selftest?slot=N`
- `GET /version` carries `capabilities.fram_erase`, so an app offers the operation only when the bridge does.

With the service switch off every route answers 403 and the board is not touched. The board then has its own switch on top of that.

## Tests

`tests/test_fram_node.c` (the logic of the board with a simulated chip, built with the switch off and on), `src/cm5_host/spi_bridge/tests/test_fram_client.py` (the client, the HTTP routes and the comparison of every constant with `fram_node.h`, `tools/can_ids.py` and the three `bootloader_common.h`) and the existing identifier, filter and flood tests, which follow the new window. Bench rows 30 and 31 of `docs/BENCH_TESTS.md` stay open.
