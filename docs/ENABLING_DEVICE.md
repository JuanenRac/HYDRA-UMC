# The three-position enabling device

The logic of the pendant switch of an operator who stays beside the machine, as data and code that run on the host. **Nothing here has run on a pendant or a board**; the bench row of the safety file (`ISO-P09`) is the check that remains.

The device has two redundant channels. Each reads the position of the switch: `1` released, `2` held in the middle, `3` pressed hard (panic). `tools/enabling_device.py` is the reference and `src/mcu_stm32h757/CM4/enabling_device.h` its C twin; `tests/test_enabling_device.py` and `tests/test_enabling_device.c` hold both to the same vectors (`tests/enabling_device_vectors.h`, written by `tools/gen_enabling_vectors.py`), and a fuzz of fixed seed holds the rules below.

| Rule | What the logic does |
|---|---|
| Motion is allowed only while **both channels read 2** and the device got there **from 1** | held in the middle at power-up, motion stays off until the switch is released |
| Leaving the middle asks for the stop **at once** | the debounce never delays a stop |
| Position 3 (panic) | a stop; and the way back from 3 passes through 2, which **must not enable** motion again: the switch has to be released to 1 first |
| The channels disagree | tolerated while the switch moves between positions and the device was only armed; a device that was enabled stops; longer than the discrepancy time it is a **fault** |
| A code that is not 1, 2 or 3 | a fault, never trusted; only a clean release (both channels at 1, stable) leaves a fault |

The times (the debounce and the discrepancy time) are in `config/enabling.cfg`, not in the code; the C header holds the same defaults and a test fails when they differ. The device decides; the caller reads the two channels, calls `EnablingDevice_Update` and sends the stop (a TRIP of `docs/ESTOP_CAN.md`) each time the number of stops grows. How the two channels are wired to pins, and the input filtering of the board, are not here.
