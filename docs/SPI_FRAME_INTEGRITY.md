# Integrity of the SPI link and the dual-core mailbox

The 128-byte frame of the SPI link between the CM5 and the H757 carried 12 meaningful bytes and 116 bytes of `0xFF` padding. The first four bytes of that padding now seal the frame:

| Offset | Size | Field | Value |
|--------|------|-------|-------|
| 12 | 1 | marker | `0xA1` when the frame is sealed; the padding value `0xFF` means a frame of the older kind |
| 13 | 1 | sequence | one more than the sequence of the last new frame of the same sender; a retry repeats it |
| 14 | 2 | CRC16 | CRC-16/CCITT-FALSE (polynomial `0x1021`, start `0xFFFF`, no reflection, no final xor) of bytes 0 to 13, high byte first |

Check value: the CRC16 of the ASCII text `123456789` is `0x29B1`.

## What the receiver does

* A sealed frame whose CRC does not match is **dropped** and counted (`spi_crc_errors`).
* A sealed frame with the same sequence as the last one accepted is a **retry**: it is not run twice (`spi_retries`); the answer already staged for it goes out again.
* A jump of the sequence is accepted, because frames can be lost, and counted (`spi_seq_gaps`).
* A frame without the marker is accepted as before and counted (`spi_unsealed`), unless the build sets `HYDRA_SPI_REQUIRE_CRC=1`. That setting is the switch for the day every sender seals its frames; it is `0` by default so a CM5 side that does not seal yet keeps working.

Every frame the bootloader of the CM4 sends is sealed. The CM5 side seals its requests too: the Python bridge (`src/cm5_host/spi_bridge/spi_bridge/transport.py`) writes the marker, a sequence that moves on with every request and the CRC16 into the padding of each frame it sends (a retry by the caller is a new request, never a replay), and treats a sealed answer whose CRC does not match as a failed transfer; an answer without the marker is still accepted. `HYDRA_SPI_REQUIRE_CRC` stays `0` until bench row 13 has run with sealed requests.

## The mailbox between the two cores

The mailbox frame of 16 bytes (`src/mcu_stm32h757/Common/ipc_mailbox.h`) uses its reserved bytes the same way: byte 0 of the reserved area is the sequence, bytes 1 and 2 the CRC16 (same algorithm) over frame type, length, the 8 payload bytes and the sequence, byte 3 the marker `0xA1`. The writer seals the frame before it takes the hardware semaphore; the reader checks it before it releases the semaphore and drops a damaged frame. A frame without the marker is accepted unless the build sets `HYDRA_IPC_REQUIRE_CRC=1`.

## Where it lives

* `src/mcu_stm32h757/CM4/boot/spi_frame_crc.h` and `src/cm5_host/ipc_driver/src/spi_frame_crc.h`: the same header-only code for the two ends of the link (a test fails when the copies differ).
* `tools/spi_frame.py`: the reference in Python; `tests/spi_frame_vectors.json`: the vectors; `tests/test_spi_frame_crc.c`: the C test on the host.

Not executed on a board: see row 13 of `docs/BENCH_TESTS.md`.
