# Production protection: read-out protection, motion outputs and the device identity

## Read-out protection (RDP)

The bootloaders hold only public keys and a plain SHA-256 of the installed application, so reading a board gives nobody the power to sign an update. Protection of the flash stops someone with a debug probe from rewriting the table of keys or the metadata.

* **Level 0** (`0xAA`): no protection; the default of a board on the bench.
* **Level 1** (any other value, `0xBB` is the one programmer tools write): the flash cannot be read through the debug port while the chip runs. Reversible, but going back to level 0 **erases the whole flash**, the bootloader included, so a board that leaves level 1 is flashed again from the start.
* **Level 2** (`0xCC`): permanent. It is **not offered** by the tool of this repository and the bootloaders do not need it.

`python tools/rdp_plan.py decode 0xAA` reads an option byte; `python tools/rdp_plan.py command --level 1` prints the programmer command; the tool never talks to a board. Whether a production flash sets level 1 is meant to be a setting of the production configuration (`production.rdp_level`, 0 or 1, default 0, so nothing is protected unless it is asked for); no flashing tool reads it yet and the switch in STUDIO is still to be made. The procedure is tried on a sacrificial board first (row 16 of `docs/BENCH_TESTS.md`).

## Identity of one chip

`python tools/rdp_plan.py device-id --uid <24 hex digits>` prints `SHA-256("hydra-umc-device-id/1" || unique ID)`. It is public information that names one chip (a log of which board took which update); it is not a secret and no key is derived from it. A per-device secret key would need a place to keep it that the chip does not offer without level 1, and a decision on what it protects: that stays open.

## Motion outputs fail safe

* The six driver enable outputs of the controller board (`PA0`, `PA1`, `PA3`, `PA4`, `PA5`, `PA15`) are forced to the level that switches the drivers off (the TMC5160 input is active low, so high) by direct register writes when the application stops on a failed set-up of the bus, a stack overflow, a failed watchdog or a hard fault (`MotionOutputs_ForceDisabled`). The level is the build setting `MOTION_DISABLE_LEVEL_HIGH` (default 1).
* **Board requirement:** every enable line needs a pull-up resistor to the logic supply on the board, so the drivers are off while the chip is in reset or the bootloader runs, before any firmware can drive the pin. This is a change of the board, not of the firmware: row 26 of `docs/BENCH_TESTS.md`.
* **Watchdog by task:** the blink task no longer refreshes the watchdog on its own. The relay task of the controller board (the STACK A task of the kinematic brain) counts its passes and the watchdog is refreshed only while the count moves, so a stuck task ends in a reset.

## Hardware filters of the stack bus

The FDCAN of the controller board accepts only the 32 identifiers of its own slot (mask `0x7E0`) and the emergency stop `0x001`; the kinematic brain accepts the windows of the eight slots (`0x600` to `0x6FF`) and `0x001`. Everything else is rejected by the peripheral. A failed initialisation, filter or start of the peripheral is recorded: the bootloader still starts a valid application, takes no update, and with no application stops refreshing the watchdog so the reset tries again. Not executed on a board: rows 27 and 28 of `docs/BENCH_TESTS.md`.
