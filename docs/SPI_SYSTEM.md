# SPI system commands: stop, release, telemetry and probe

Commands of the SPI link between the CM5 and the H745 that the CM4 answers itself. They are frames of the tier `SELF` and never go on a CAN bus. The frame types start at `0x20`, above the 32 identifiers a slot window owns, so none of them can be mistaken for a CAN message of a slot.

| Type | Name | Length | Payload | Does |
|---|---|---|---|---|
| `0x20` | `STOP_REQUEST` | 5 | `S` `T` `O` `P` reason | asks the brain to put the whole stack in the emergency stop, with the reason byte of `docs/ESTOP_CAN.md` |
| `0x21` | `RELEASE_REQUEST` | 4 | `R` `E` `L` `S` | a person asked to end the stop; the brain still waits for every expected board to say READY, and the drivers stay off until the normal enable command |
| `0x22` | `TELEMETRY_QUERY` | 1 | selector (`0` = the CM4) | asks for the counters of the CM4 |
| `0x23` | `TELEMETRY_RESPONSE` | 8 | selector, flags, CRC errors, retries, sequence gaps, CAN tx errors, CAN rx errors, state | the answer; counters saturate at 255 |
| `0x24` | `PROBE_REQUEST` | 1 | scan id | runs the slot scan of `docs/SLOT_SCAN.md` before an update |
| `0x25` | `PROBE_RESPONSE` | 5 | scan id, then the verdict of the slots 0 to 7, a nibble each | `0` absent, `1` ok, `2` duplicate, `3` misplaced, `4` noisy |

Flags of the telemetry: bit 0 the link is usable, bit 1 the emergency stop is latched, bit 2 the stop commands are enabled in this build. State: `1` bootloader, `2` application.

A frame that is not exactly one of the above (wrong length, wrong magic, unknown selector) does nothing and is not answered.

## Switches

Both are off in a build as it ships, and the CM4 answers none of these frames:

* `HYDRA_SPI_SYSTEM=1` enables telemetry and probe (read only).
* `HYDRA_SPI_STOP_COMMAND=1` enables stop and release as well. It needs `HYDRA_SPI_SYSTEM=1`.

The bootloader of the CM4 answers the telemetry only: it has no emergency stop and no slot scan of its own. Stop, release and probe belong to the application of the CM4, which gets the same header (`src/mcu_stm32h745/CM4/spi_system.h`) when it handles the SPI link.

## References and tests

* `tools/spi_system.py` is the reference and the host client (`Client`), and `tests/sim_hil_bench.py` (`SpiBrainDouble`) is the double of the CM4 with the real slot scan on a virtual bus.
* `tests/test_spi_system.c` runs the header against the real state machine of the brain (`estop_brain.h`); the telemetry and probe bytes in it are compared with the reference by `tests/test_hy25_sources.py`.
* Not run on a board. Bench rows 13 (the link at full rate) and 19 (the telemetry over ten minutes) of `docs/BENCH_TESTS.md` are the checks that remain.
