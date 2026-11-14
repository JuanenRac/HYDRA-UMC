# The host link with the board as SPI master

On the H757 board the Jetson Orin NX is the host and the board clocks the link (SPI3). The frame is the sealed 128-byte frame of the slave link (`SpiOtaFrame_t`: the 12 bytes of tier, slot, type, length and payload, then the marker `0xA1`, the sequence and the CRC-16; `docs/SPI_FRAME_INTEGRITY.md`), unchanged, so the relay to the other core and to the slots of the stack bus, and every handler, are used as they are. Only the transport is new: `src/mcu_stm32h757/Common/host_link_master.h`, written first in `tools/host_link_master.py`.

## When the board clocks

One call of `HostLink_Poll` runs at most one transaction, one frame out and one frame in, when:

* a frame is staged (an answer or something the board has to say), or
* the READY line of the host is high (only when the line is wired: `ready_known = 1`), or
* the idle interval has passed since the last transaction; the board then sends an idle frame (type `0xFF`) so the host can hand in a request even without READY.

Two transactions are never closer than `min_gap_ms`. The request of the host arrives in the same transaction in which the board sends its last answer or an idle frame; the answer to it goes out in the next one, so a request and its answer take two transactions.

## What the board does with the frame that came in

| It is | The board |
|---|---|
| a frame of `0xFF` in every byte | counts silence (the line floating, no host behind it); it is not an error and a staged frame stays for the next idle interval |
| damaged (CRC) | does not run it; the frame that went out is sent again with the same sequence up to `max_retries` times, then dropped and counted |
| unsealed (no marker) | with `require_seal = 1` the same as damaged; with `0` it is accepted |
| a repeat of the last sequence | counts a duplicate and does not run it twice |
| an idle frame | counts it |
| anything else | runs `dispatch`; the answer, if there is one, is staged for the next transaction |

A failure of the peripheral keeps the staged frame and counts; `down_after_failures` in a row mark the link down and the first transaction that works marks it up again. The link never carries the emergency stop: that travels on the CAN bus, so the link going down changes nothing about the stop.

## Configuration

`config/host_link.cfg` holds the idle interval, the minimum gap, the retries, the failures before the link is down, whether the seal is required (the conservative value, `1`) and whether the READY line is wired. The schematic of the board does not draw the READY line yet, so `ready_known` is `0` and the link runs on the idle interval alone until the pin is filled in `docs/platform_profiles.json` (`host_control.ready`).

## What the tests pin down

`tests/test_host_link_master.c` runs the C code against the scenarios of `tools/gen_host_link_vectors.py`, among them four long runs of a fixed seed, all written from the Python reference: the poll result, the verdict of the frame that came in, the type and sequence that went out, every counter and the state, after every call. Both sides refuse a configuration that cannot work. Nothing here has run on a board.

## What the CM7 does with a request

`src/mcu_stm32h757/Common/gateway_dispatch.h` is the `dispatch` of the link on the H757 board, where the CM7 owns SPI3, FDCAN1 and I2C4 and is therefore the gateway that the CM4 is on the H745 board. It reads the tier of the request (byte 0 of the frame) and sends it to the same three targets as the gateway of the H745 board: tier 0 to the handlers of the gateway core itself, tier 1 to the other core through the mailbox (the CM4 here; the tier is named CM7 on the H745 board and has the same number on the wire), tier 2 to the stack of controller boards on FDCAN1 (the slot is byte 1). When the other core or the stack gives no answer, or a damaged one, the host receives a status frame with `STATUS_ERROR` instead of silence; a tier that is none of the three is not run and not answered, and every case is counted. `tests/test_gateway_dispatch.c` runs it against a double of the mailbox with a backend that answers, stays silent or damages its answer, and feeds it through `HostLink_Take`. The handlers of the core and the transmission on FDCAN1 are given as callbacks: wiring them to the real peripherals of the CM7 is the next step, and nothing here has run on a board.

The stack tier (target 2) is `src/mcu_stm32h757/Common/gateway_stack_a.h`, the `stack_a` callback of the gateway over FDCAN1, and `src/mcu_stm32h757/CM7/GatewayFdcan1.c` binds it to the peripheral. A frame for the board in a slot goes out on `0x600 + slot * 0x40 + type`; the first frame of the same 64-id window that comes back is the answer and a frame of another window is dropped. The rules are those of the relay of the CM4 of the H745 board: slots 0 to 7 only, a type inside the window, the types that the boards transmit themselves are never put on the bus, and the poll type (0x17) sends nothing and only waits. Silence, a refused frame or a full transmit FIFO give the host STATUS_ERROR. The wait is `timeout_ms` of `config/gateway.cfg` (1 to 1000 ms). The binding ships off (`HYDRA_GATEWAY_FDCAN1 0`) and does not start FDCAN1: the bit timing, the filters and the clock stay with the set-up of the bus. `tests/test_gateway_stack_a.c` and `tests/test_gateway_fdcan1.c` run it against a double of the bus and of the HAL; a build step compiles the binding against the real header. Nothing has run on a board.

## Physical test (not done)

A Jetson Orin NX and an H757 board with a logic analyser on SPI3 (SCK PB3, MOSI PB5, MISO PB4, NSS PA15) and the READY line: the board clocks an idle frame every idle interval; the host raises READY with a request and the answer arrives in the next transaction; a frame with one byte changed on the host side is not run and is counted; unplugging the host makes the link go quiet (silence) without an error.
