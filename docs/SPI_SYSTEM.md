# SPI system commands: stop, release, telemetry, probe and board health

Commands of the SPI link between the CM5 and the H757 that the CM4 answers itself. They are frames of the tier `SELF` and never go on a CAN bus. The frame types start at `0x20`. The tier of the frame decides where it goes before its type is read (the tier `STACKA` is relayed to the CAN bus, the tier `SELF` is answered by the CM4), so a type of this table is never put on a bus even though the window of a slot, 64 identifiers wide, also has offsets from `0x20` (the F-RAM requests of `docs/FRAM_ERASE.md`).

| Type | Name | Length | Payload | Does |
|---|---|---|---|---|
| `0x20` | `STOP_REQUEST` | 5 | `S` `T` `O` `P` reason | asks the brain to put the whole stack in the emergency stop, with the reason byte of `docs/ESTOP_CAN.md` |
| `0x21` | `RELEASE_REQUEST` | 4 | `R` `E` `L` `S` | a person asked to end the stop; the brain still waits for every expected board to say READY, and the drivers stay off until the normal enable command |
| `0x22` | `TELEMETRY_QUERY` | 1 | selector (`0` = the CM4) | asks for the counters of the CM4 |
| `0x23` | `TELEMETRY_RESPONSE` | 8 | selector, flags, CRC errors, retries, sequence gaps, CAN tx errors, CAN rx errors, state | the answer; counters saturate at 255 |
| `0x24` | `PROBE_REQUEST` | 1 | scan id | runs the slot scan of `docs/SLOT_SCAN.md` before an update |
| `0x25` | `PROBE_RESPONSE` | 5 | scan id, then the verdict of the slots 0 to 7, a nibble each | `0` absent, `1` ok, `2` duplicate, `3` misplaced, `4` noisy |
| `0x26` | `ZONES_HEARTBEAT` | 4 | `Z` `O` `N` `E` | the CM5 says the safety-zones chain is healthy; not answered. Needs the stop switch, because what it protects is the stack |
| `0x27` | `HEALTH_QUERY` | 1 | slot 0 to 7 | asks for the last health frame the brain heard from the board in that slot. Read only; needs the switch `HYDRA_SPI_HEALTH` |
| `0x2A` | `HEALTH_RESPONSE` | 8, or 2 | slot, age, sequence, flags, tx errors, rx errors, stalls, uptime | the answer: the age is in units of 100 ms (254 at most, so 25.4 s or more); the six bytes after the age are the ones between the marker and the check byte of the health frame (`docs/FIRMWARE_SWITCHES.md`, `HYDRA_HEALTH_FRAME`), and the uptime is in units of 16 s. Two bytes, the slot and `0xFF`, while the brain has heard nothing from that slot |

The health answer leaves out the marker (`0x48`) and the check byte of the frame the board sent: the brain stores only a frame whose marker and check byte were right, so the CM5 rebuilds the same eight bytes with `tools/health_frame.py` (`build`) and the frame has its own seal again. The type `0x28` and `0x29` stay the F-RAM pair, which is why the health answer is `0x2A`. The brain keeps the frame of a slot only with the switch on and only a frame of the offset `0x11` of the slot that has the marker and the check byte of a health frame (the axis step telemetry uses the same offset).

Flags of the telemetry: bit 0 the link is usable, bit 1 the emergency stop is latched, bit 2 the stop commands are enabled in this build. State: `1` bootloader, `2` application.

Two more frame types of the tier `SELF` are not commands of this file: `0x28` (F-RAM request) and `0x29` (its answer), run by the STACK A task on the chip of the main board (`docs/FRAM_ERASE.md`).

A frame that is not exactly one of the above (wrong length, wrong magic, unknown selector) does nothing and is not answered.

## Switches

Both are off in a build as it ships, and the CM4 answers none of these frames:

* `HYDRA_SPI_SYSTEM=1` enables telemetry and probe (read only).
* `HYDRA_SPI_STOP_COMMAND=1` enables stop and release as well. It needs `HYDRA_SPI_SYSTEM=1`.
* `HYDRA_SPI_HEALTH=1` (off as it ships) keeps the last health frame of each board and answers the health query. It needs `HYDRA_SPI_SYSTEM=1`, acts on nothing and sends nothing on the bus; the boards must be built with `HYDRA_HEALTH_FRAME=1` to have anything to report. Without it the query is ignored and the CM4 stores no frame.

The bootloader of the CM4 answers the telemetry only: it has no emergency stop and no slot scan of its own. Stop, release and probe belong to the application of the CM4.

## The application of the CM4

Once the application runs, the SPI link is served by `src/mcu_stm32h757/CM4/spi_app.h` (the logic, without any HAL) and `SpiApp.c` (the driver: SPI1 in slave mode on the pins of the bootloader and one task). A third switch starts it:

* `HYDRA_SPI_APP=1` starts the service and the task. It needs `HYDRA_SPI_SYSTEM=1`, and `HYDRA_SPI_STOP_COMMAND=1` for stop and release. With the switch at `0` (as it ships) `SpiApp.c` compiles to an empty unit.
* `HYDRA_SPI_ZONES_HEARTBEAT=1` makes the application stop the stack, with the reason `safety-zone` (`0x04`), when the heartbeat of the safety-zones chain has come and then does not come for `HYDRA_SPI_ZONES_MAX_AGE_MS` (default 1000 ms; the service sends one every 100 ms). It never trips before the first heartbeat, so a machine without that service is not affected, and it is armed again by the next heartbeat after a loss. The CM5 sends the heartbeat only while the last verdict of the service is `READY` and fresh (`docs/CAN_ESTOP.md` of the safety-zones repository), so a crashed, frozen or inhibited service is a lost heartbeat.
* `HYDRA_SPI_REQUIRE_CRC=1` refuses the frames that come without the seal of `docs/SPI_FRAME_INTEGRITY.md`.
* `HYDRA_SPI_RELEASE_EXPECTED_MASK` (default `0xFF`) lists the slots that have to answer READY before a release ends the stop; with all eight expected, a stack with an empty slot stays stopped until a build names the slots it has.

What the service does with a frame: it checks the seal, drops a damaged frame and counts it, does not run a retry twice, and hands a command of the tier `SELF` to `SpiSys_Handle`. Relay frames (to the stack or the CM7) and update frames are not answered here: an update starts in the bootloader. The answer is sealed and goes out on the next transaction, because SPI moves data only when the master clocks it; a request therefore needs a second transaction (an idle frame of type `0xFF`, which the firmware ignores) to read its answer.

The SPI task never calls the emergency stop. It leaves the request (stop with its reason, release, slot scan) and the STACK A task, the only one that calls `KinematicBrainCan_Estop*`, takes it; that task in turn publishes whether the stop is latched, the error counters of the bus and the verdicts of a scan. The scan of this application asks every slot for its `AXIS_STATUS`: it tells an answering board (`ok`) from a silent one (`absent`); the `duplicate`, `misplaced` and `noisy` verdicts of `docs/SLOT_SCAN.md` need the full scan and are not produced by it.

On the CM5 the bridge (`src/cm5_host/spi_bridge`, module `system_client`) sends these frames: `read_telemetry`, `probe_slots`, `request_stop`, `request_release` and `read_health`. A stop or a release answers only that the frame was sent; the telemetry flags say whether the brain acted. With `--zones-state-url` the bridge also sends the heartbeat (`send_zones_heartbeat`) every 100 ms, and only while `GET /state` of the safety-zones service says READY and is not older than a second.

## References and tests

* `tools/spi_system.py` is the reference and the host client (`Client`), and `tests/sim_hil_bench.py` (`SpiBrainDouble`) is the double of the CM4 with the real slot scan on a virtual bus.
* `tools/spi_app.py` is the host twin of the service of the application (`SpiApp`) and of the CM5 end of the wire (`Cm5Link`); `tests/test_spi_app.c` runs the real header against the real state machine of the brain, built twice (switches on and off), and `tests/test_spi_app.py` compares the bytes of the two and guards the sources.
* `tests/test_spi_system.c` runs the header against the real state machine of the brain (`estop_brain.h`); the telemetry and probe bytes in it are compared with the reference by `tests/test_hy25_sources.py`.
* The health of the boards is kept by `src/mcu_stm32h757/CM4/node_health.h` (HAL-free) and read from the bus by `KinematicBrainCan.c` (`KinematicBrainCan_ReadHealth`); the STACK A task copies it to the SPI service every pass, one slot at a time with a counter that is odd while the slot is written, so the SPI task never answers with half a frame (a slot caught in the middle of a write is told as not heard). `tests/test_spi_app.c` and `tests/test_spi_system.c` run it against the twin with the bytes of `tools/spi_system.py` (`health_response`); the compile check of `tools/build_firmware.py` builds `KinematicBrainCan.c` and `SpiApp.c` with the switch on.
* Not run on a board. Bench rows 13 (the link at full rate) and 19 (the telemetry over ten minutes) of `docs/BENCH_TESTS.md` are the checks that remain.
