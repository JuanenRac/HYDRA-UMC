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

The bootloader of the CM4 answers the telemetry only: it has no emergency stop and no slot scan of its own. Stop, release and probe belong to the application of the CM4.

## The application of the CM4

Once the application runs, the SPI link is served by `src/mcu_stm32h745/CM4/spi_app.h` (the logic, without any HAL) and `SpiApp.c` (the driver: SPI1 in slave mode on the pins of the bootloader and one task). A third switch starts it:

* `HYDRA_SPI_APP=1` starts the service and the task. It needs `HYDRA_SPI_SYSTEM=1`, and `HYDRA_SPI_STOP_COMMAND=1` for stop and release. With the switch at `0` (as it ships) `SpiApp.c` compiles to an empty unit.
* `HYDRA_SPI_REQUIRE_CRC=1` refuses the frames that come without the seal of `docs/SPI_FRAME_INTEGRITY.md`.
* `HYDRA_SPI_RELEASE_EXPECTED_MASK` (default `0xFF`) lists the slots that have to answer READY before a release ends the stop; with all eight expected, a stack with an empty slot stays stopped until a build names the slots it has.

What the service does with a frame: it checks the seal, drops a damaged frame and counts it, does not run a retry twice, and hands a command of the tier `SELF` to `SpiSys_Handle`. Relay frames (to the stack or the CM7) and update frames are not answered here: an update starts in the bootloader. The answer is sealed and goes out on the next transaction, because SPI moves data only when the master clocks it; a request therefore needs a second transaction (an idle frame of type `0xFF`, which the firmware ignores) to read its answer.

The SPI task never calls the emergency stop. It leaves the request (stop with its reason, release, slot scan) and the STACK A task, the only one that calls `KinematicBrainCan_Estop*`, takes it; that task in turn publishes whether the stop is latched, the error counters of the bus and the verdicts of a scan. The scan of this application asks every slot for its `AXIS_STATUS`: it tells an answering board (`ok`) from a silent one (`absent`); the `duplicate`, `misplaced` and `noisy` verdicts of `docs/SLOT_SCAN.md` need the full scan and are not produced by it.

On the CM5 the bridge (`src/cm5_host/spi_bridge`, module `system_client`) sends these frames: `read_telemetry`, `probe_slots`, `request_stop` and `request_release`. A stop or a release answers only that the frame was sent; the telemetry flags say whether the brain acted.

## References and tests

* `tools/spi_system.py` is the reference and the host client (`Client`), and `tests/sim_hil_bench.py` (`SpiBrainDouble`) is the double of the CM4 with the real slot scan on a virtual bus.
* `tools/spi_app.py` is the host twin of the service of the application (`SpiApp`) and of the CM5 end of the wire (`Cm5Link`); `tests/test_spi_app.c` runs the real header against the real state machine of the brain, built twice (switches on and off), and `tests/test_spi_app.py` compares the bytes of the two and guards the sources.
* `tests/test_spi_system.c` runs the header against the real state machine of the brain (`estop_brain.h`); the telemetry and probe bytes in it are compared with the reference by `tests/test_hy25_sources.py`.
* Not run on a board. Bench rows 13 (the link at full rate) and 19 (the telemetry over ten minutes) of `docs/BENCH_TESTS.md` are the checks that remain.
